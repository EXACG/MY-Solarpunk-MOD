#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace
{
constexpr uintptr_t kGObjectsOffset = 0x076CD920;
constexpr uintptr_t kAppendStringOffset = 0x0122BD08;
constexpr size_t kProcessEventIndex = 0x4C;
constexpr uint64_t kClassCastFlag = 0x20;
constexpr uint64_t kFunctionCastFlag = 0x0000000000080000ULL;
constexpr int32_t kDesiredPublicConnections = 8;
constexpr char kEosImportName[] = "EOSSDK-Win64-Shipping.dll";
constexpr char kEosLobbySetMaxMembersName[] = "EOS_LobbyModification_SetMaxMembers";
constexpr char kEosSessionSetMaxPlayersName[] = "EOS_SessionModification_SetMaxPlayers";
constexpr wchar_t kSteamApiModuleName[] = L"steam_api64.dll";
constexpr char kSteamMatchmakingAccessorName[] = "SteamAPI_SteamMatchmaking_v009";
constexpr size_t kSteamCreateLobbyVtableOffset = 0x68;
constexpr size_t kSteamSetLobbyMemberLimitVtableOffset = 0xF8;

constexpr wchar_t kLogFileName[] = L"Room8Fix.log";

HMODULE g_selfModule = nullptr;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_logLock{};
bool g_logLockReady = false;

struct FName
{
    int32_t ComparisonIndex;
    int32_t Number;
};

template <typename T>
struct TArray
{
    T* Data;
    int32_t NumElements;
    int32_t MaxElements;

    explicit operator bool() const
    {
        return Data != nullptr && NumElements > 0;
    }

    int32_t Num() const
    {
        return NumElements;
    }
};

struct FString : TArray<wchar_t>
{
    FString()
    {
        Data = nullptr;
        NumElements = 0;
        MaxElements = 0;
    }

    FString(wchar_t* buffer, int32_t num, int32_t max)
    {
        Data = buffer;
        NumElements = num;
        MaxElements = max;
    }

    std::string ToUtf8() const
    {
        if (!Data || NumElements <= 1)
        {
            return {};
        }

        const int length = WideCharToMultiByte(
            CP_UTF8,
            0,
            Data,
            NumElements - 1,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (length <= 0)
        {
            return {};
        }

        std::string output(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            Data,
            NumElements - 1,
            output.data(),
            length,
            nullptr,
            nullptr);
        return output;
    }
};

struct UClass;

struct UObject
{
    void* VTable;
    uint32_t Flags;
    int32_t Index;
    UClass* Class;
    FName Name;
    UObject* Outer;
};

struct UField : UObject
{
    UField* Next;
};

struct UStruct : UField
{
    uint8_t Pad30[0x18];
    UField* Children;
    uint8_t Pad50[0x60];
};

struct UFunction : UStruct
{
    uint32_t FunctionFlags;
    uint8_t PadB4[0x24];
    void* ExecFunction;
};

struct UClass : UStruct
{
    uint8_t PadB0[0x28];
    uint64_t CastFlags;
    uint8_t PadE0[0x30];
    UObject* ClassDefaultObject;
};

struct FUObjectItem
{
    uint8_t Pad0[0x8];
    UObject* Object;
    uint8_t Pad10[0x8];
};

struct TUObjectArray
{
    FUObjectItem** Objects;
    uint8_t Pad8[0x8];
    int32_t MaxElements;
    int32_t NumElements;
    int32_t MaxChunks;
    int32_t NumChunks;

    UObject* GetByIndex(int32_t index) const
    {
        constexpr int32_t kElementsPerChunk = 0x10000;

        if (!Objects || index < 0)
        {
            return nullptr;
        }

        const int32_t chunkIndex = index / kElementsPerChunk;
        const int32_t inChunkIndex = index % kElementsPerChunk;

        if (chunkIndex >= NumChunks || index >= NumElements)
        {
            return nullptr;
        }

        FUObjectItem* chunk = Objects[chunkIndex];
        if (!chunk)
        {
            return nullptr;
        }

        return chunk[inChunkIndex].Object;
    }

    int32_t Num() const
    {
        return NumElements;
    }
};

#pragma pack(push, 1)
struct CreateAdvancedSessionParams
{
    void* WorldContextObject;
    uint8_t ExtraSettings[0x10];
    void* PlayerController;
    int32_t PublicConnections;
    int32_t PrivateConnections;
    uint8_t bUseLAN;
    uint8_t bAllowInvites;
    uint8_t bIsDedicatedServer;
    uint8_t bUseLobbiesIfAvailable;
    uint8_t bAllowJoinViaPresence;
    uint8_t bAllowJoinViaPresenceFriendsOnly;
    uint8_t bAntiCheatProtected;
    uint8_t bUsesStats;
    uint8_t bShouldAdvertise;
    uint8_t bUseLobbiesVoiceChatIfAvailable;
    uint8_t bStartAfterCreate;
    uint8_t Pad33[0x5];
    void* ReturnValue;
};

struct UpdateSessionParams
{
    void* WorldContextObject;
    uint8_t ExtraSettings[0x10];
    int32_t PublicConnections;
    int32_t PrivateConnections;
    uint8_t bUseLAN;
    uint8_t bAllowInvites;
    uint8_t bAllowJoinInProgress;
    uint8_t bRefreshOnlineData;
    uint8_t bIsDedicatedServer;
    uint8_t bShouldAdvertise;
    uint8_t bAllowJoinViaPresence;
    uint8_t bAllowJoinViaPresenceFriendsOnly;
    void* ReturnValue;
};
#pragma pack(pop)

static_assert(offsetof(CreateAdvancedSessionParams, PublicConnections) == 0x20);
static_assert(sizeof(CreateAdvancedSessionParams) == 0x40);
static_assert(offsetof(UpdateSessionParams, PublicConnections) == 0x18);
static_assert(sizeof(UpdateSessionParams) == 0x30);

using AppendStringFn = void(__fastcall*)(const FName*, FString&);
using ProcessEventFn = void(__fastcall*)(UObject*, UFunction*, void*);
using EosLobbySetMaxMembersFn = int32_t(*)(void* handle, const void* options);
using EosSessionSetMaxPlayersFn = int32_t(*)(void* handle, const void* options);
using SteamMatchmakingAccessorFn = void* (*)();
using SteamCreateLobbyFn = uint64_t(__fastcall*)(void* self, int32_t lobbyType, int32_t maxMembers);
using SteamSetLobbyMemberLimitFn = bool(__fastcall*)(void* self, uint64_t lobbyId, int32_t maxMembers);

ProcessEventFn g_originalProcessEvent = nullptr;
UFunction* g_createAdvancedSessionFunc = nullptr;
UFunction* g_updateSessionFunc = nullptr;
bool g_createHookReady = false;
bool g_updateHookReady = false;
EosLobbySetMaxMembersFn g_originalEosLobbySetMaxMembers = nullptr;
EosSessionSetMaxPlayersFn g_originalEosSessionSetMaxPlayers = nullptr;
bool g_eosLobbyHookReady = false;
bool g_eosSessionHookReady = false;
SteamCreateLobbyFn g_originalSteamCreateLobby = nullptr;
SteamSetLobbyMemberLimitFn g_originalSteamSetLobbyMemberLimit = nullptr;
bool g_steamCreateLobbyHookReady = false;
bool g_steamSetLobbyMemberLimitHookReady = false;

#pragma pack(push, 1)
struct EosLobbyModificationSetMaxMembersOptions
{
    int32_t ApiVersion;
    uint32_t MaxMembers;
};

struct EosSessionModificationSetMaxPlayersOptions
{
    int32_t ApiVersion;
    uint32_t MaxPlayers;
};
#pragma pack(pop)

uintptr_t GetImageBase()
{
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

std::wstring GetModuleDirectory(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));

    std::wstring result(path);
    const size_t pos = result.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
    {
        result.resize(pos + 1);
    }
    else
    {
        result.clear();
    }

    return result;
}

std::wstring BuildPathInModuleDir(const wchar_t* fileName)
{
    return GetModuleDirectory(g_selfModule) + fileName;
}

void OpenLogFile()
{
    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        return;
    }

    g_logFile = CreateFileW(
        BuildPathInModuleDir(kLogFileName).c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
}

void Log(const char* format, ...)
{
    char buffer[1024] = {};

    va_list args;
    va_start(args, format);
    const int written = _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
    va_end(args);

    if (written <= 0)
    {
        return;
    }

    if (g_logLockReady)
    {
        EnterCriticalSection(&g_logLock);
    }

    OpenLogFile();
    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        DWORD byteCount = 0;
        WriteFile(g_logFile, buffer, static_cast<DWORD>(written), &byteCount, nullptr);
        WriteFile(g_logFile, "\r\n", 2, &byteCount, nullptr);
    }

    if (g_logLockReady)
    {
        LeaveCriticalSection(&g_logLock);
    }
}

AppendStringFn GetAppendString()
{
    return reinterpret_cast<AppendStringFn>(GetImageBase() + kAppendStringOffset);
}

TUObjectArray* GetGObjects()
{
    return reinterpret_cast<TUObjectArray*>(GetImageBase() + kGObjectsOffset);
}

std::string FNameToString(const FName& name)
{
    wchar_t buffer[1024] = {};
    FString out(buffer, 0, static_cast<int32_t>(std::size(buffer)));

    GetAppendString()(&name, out);
    std::string value = out.ToUtf8();

    const size_t slashPos = value.rfind('/');
    if (slashPos != std::string::npos)
    {
        value = value.substr(slashPos + 1);
    }

    return value;
}

bool HasCastFlag(const UObject* object, uint64_t requiredFlag)
{
    return object && object->Class && ((object->Class->CastFlags & requiredFlag) == requiredFlag);
}

bool PatchIatEntry(
    HMODULE module,
    const char* importModuleName,
    const char* importFunctionName,
    void* replacement,
    void** original)
{
    if (!module || !importModuleName || !importFunctionName || !replacement || !original)
    {
        return false;
    }

    auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (!dosHeader || dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return false;
    }

    auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(module) + dosHeader->e_lfanew);
    if (!ntHeaders || ntHeaders->Signature != IMAGE_NT_SIGNATURE)
    {
        return false;
    }

    const IMAGE_DATA_DIRECTORY& importDirectory =
        ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDirectory.VirtualAddress || !importDirectory.Size)
    {
        return false;
    }

    auto* importDescriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        reinterpret_cast<uint8_t*>(module) + importDirectory.VirtualAddress);

    for (; importDescriptor->Name != 0; ++importDescriptor)
    {
        const char* currentImportModule = reinterpret_cast<const char*>(reinterpret_cast<uint8_t*>(module) + importDescriptor->Name);
        if (_stricmp(currentImportModule, importModuleName) != 0)
        {
            continue;
        }

        auto* thunkData = reinterpret_cast<IMAGE_THUNK_DATA64*>(reinterpret_cast<uint8_t*>(module) + importDescriptor->FirstThunk);
        auto* nameThunkData = importDescriptor->OriginalFirstThunk != 0
            ? reinterpret_cast<IMAGE_THUNK_DATA64*>(reinterpret_cast<uint8_t*>(module) + importDescriptor->OriginalFirstThunk)
            : thunkData;

        for (; nameThunkData->u1.AddressOfData != 0; ++nameThunkData, ++thunkData)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(nameThunkData->u1.Ordinal))
            {
                continue;
            }

            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                reinterpret_cast<uint8_t*>(module) + nameThunkData->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(importByName->Name), importFunctionName) != 0)
            {
                continue;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(&thunkData->u1.Function, sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProtect))
            {
                Log("IAT patch failed for %s!%s (%lu)", importModuleName, importFunctionName, GetLastError());
                return false;
            }

            *original = reinterpret_cast<void*>(thunkData->u1.Function);
            thunkData->u1.Function = reinterpret_cast<ULONGLONG>(replacement);

            DWORD ignored = 0;
            VirtualProtect(&thunkData->u1.Function, sizeof(uintptr_t), oldProtect, &ignored);

            Log("IAT patched: %s!%s -> %p", importModuleName, importFunctionName, replacement);
            return true;
        }
    }

    return false;
}

bool PatchVtableEntry(
    void* object,
    size_t methodOffset,
    void* replacement,
    void** original,
    const char* tag)
{
    if (!object || !replacement || !original)
    {
        return false;
    }

    auto*** objectAsVtable = reinterpret_cast<void***>(object);
    if (!objectAsVtable || !*objectAsVtable)
    {
        Log("%s: object has no vtable", tag);
        return false;
    }

    void** vtable = *objectAsVtable;
    void** slot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(vtable) + methodOffset);

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Log("%s: VirtualProtect failed (%lu)", tag, GetLastError());
        return false;
    }

    if (*slot != replacement)
    {
        *original = *slot;
        *slot = replacement;
        Log("%s: vtable hook installed at offset 0x%zX", tag, methodOffset);
    }

    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    return true;
}

UObject* FindObjectByShortName(std::string_view name, uint64_t requiredFlag)
{
    TUObjectArray* objects = GetGObjects();
    if (!objects || !objects->Objects || objects->Num() <= 0)
    {
        return nullptr;
    }

    for (int32_t index = 0; index < objects->Num(); ++index)
    {
        UObject* object = objects->GetByIndex(index);
        if (!object)
        {
            continue;
        }

        if (requiredFlag && !HasCastFlag(object, requiredFlag))
        {
            continue;
        }

        if (FNameToString(object->Name) == name)
        {
            return object;
        }
    }

    return nullptr;
}

UFunction* FindFunctionByName(UClass* type, std::string_view functionName)
{
    if (!type)
    {
        return nullptr;
    }

    for (UField* field = type->Children; field; field = field->Next)
    {
        if (!HasCastFlag(field, kFunctionCastFlag))
        {
            continue;
        }

        if (FNameToString(field->Name) == functionName)
        {
            return reinterpret_cast<UFunction*>(field);
        }
    }

    return nullptr;
}

void ForcePublicConnections(int32_t& value, const char* sourceTag)
{
    if (value >= kDesiredPublicConnections)
    {
        return;
    }

    Log("%s: public connections %d -> %d", sourceTag, value, kDesiredPublicConnections);
    value = kDesiredPublicConnections;
}

int32_t __fastcall HookedEosLobbySetMaxMembers(void* handle, const void* options)
{
    if (options)
    {
        auto* mutableOptions = const_cast<EosLobbyModificationSetMaxMembersOptions*>(
            static_cast<const EosLobbyModificationSetMaxMembersOptions*>(options));
        int32_t current = static_cast<int32_t>(mutableOptions->MaxMembers);
        if (current < kDesiredPublicConnections)
        {
            Log("EOS_LobbyModification_SetMaxMembers: %d -> %d", current, kDesiredPublicConnections);
            mutableOptions->MaxMembers = static_cast<uint32_t>(kDesiredPublicConnections);
        }
    }

    return g_originalEosLobbySetMaxMembers ? g_originalEosLobbySetMaxMembers(handle, options) : 0;
}

int32_t __fastcall HookedEosSessionSetMaxPlayers(void* handle, const void* options)
{
    if (options)
    {
        auto* mutableOptions = const_cast<EosSessionModificationSetMaxPlayersOptions*>(
            static_cast<const EosSessionModificationSetMaxPlayersOptions*>(options));
        int32_t current = static_cast<int32_t>(mutableOptions->MaxPlayers);
        if (current < kDesiredPublicConnections)
        {
            Log("EOS_SessionModification_SetMaxPlayers: %d -> %d", current, kDesiredPublicConnections);
            mutableOptions->MaxPlayers = static_cast<uint32_t>(kDesiredPublicConnections);
        }
    }

    return g_originalEosSessionSetMaxPlayers ? g_originalEosSessionSetMaxPlayers(handle, options) : 0;
}

uint64_t __fastcall HookedSteamCreateLobby(void* self, int32_t lobbyType, int32_t maxMembers)
{
    int32_t adjustedMaxMembers = maxMembers;
    if (adjustedMaxMembers < kDesiredPublicConnections)
    {
        Log("Steam CreateLobby: type=%d members %d -> %d", lobbyType, adjustedMaxMembers, kDesiredPublicConnections);
        adjustedMaxMembers = kDesiredPublicConnections;
    }

    return g_originalSteamCreateLobby ? g_originalSteamCreateLobby(self, lobbyType, adjustedMaxMembers) : 0;
}

bool __fastcall HookedSteamSetLobbyMemberLimit(void* self, uint64_t lobbyId, int32_t maxMembers)
{
    int32_t adjustedMaxMembers = maxMembers;
    if (adjustedMaxMembers < kDesiredPublicConnections)
    {
        Log("Steam SetLobbyMemberLimit: lobby=0x%llX members %d -> %d",
            static_cast<unsigned long long>(lobbyId),
            adjustedMaxMembers,
            kDesiredPublicConnections);
        adjustedMaxMembers = kDesiredPublicConnections;
    }

    return g_originalSteamSetLobbyMemberLimit
        ? g_originalSteamSetLobbyMemberLimit(self, lobbyId, adjustedMaxMembers)
        : false;
}

void TryInstallEosHooks()
{
    HMODULE mainModule = GetModuleHandleW(nullptr);
    if (!mainModule)
    {
        return;
    }

    if (!g_eosLobbyHookReady)
    {
        void* original = nullptr;
        if (PatchIatEntry(
                mainModule,
                kEosImportName,
                kEosLobbySetMaxMembersName,
                reinterpret_cast<void*>(&HookedEosLobbySetMaxMembers),
                &original))
        {
            g_originalEosLobbySetMaxMembers = reinterpret_cast<EosLobbySetMaxMembersFn>(original);
            g_eosLobbyHookReady = true;
        }
    }

    if (!g_eosSessionHookReady)
    {
        void* original = nullptr;
        if (PatchIatEntry(
                mainModule,
                kEosImportName,
                kEosSessionSetMaxPlayersName,
                reinterpret_cast<void*>(&HookedEosSessionSetMaxPlayers),
                &original))
        {
            g_originalEosSessionSetMaxPlayers = reinterpret_cast<EosSessionSetMaxPlayersFn>(original);
            g_eosSessionHookReady = true;
        }
    }
}

void TryInstallSteamHooks()
{
    HMODULE steamApiModule = GetModuleHandleW(kSteamApiModuleName);
    if (!steamApiModule)
    {
        return;
    }

    auto accessor = reinterpret_cast<SteamMatchmakingAccessorFn>(
        GetProcAddress(steamApiModule, kSteamMatchmakingAccessorName));
    if (!accessor)
    {
        Log("Steam matchmaking accessor export missing");
        return;
    }

    void* matchmakingInterface = accessor();
    if (!matchmakingInterface)
    {
        return;
    }

    if (!g_steamCreateLobbyHookReady)
    {
        void* original = nullptr;
        if (PatchVtableEntry(
                matchmakingInterface,
                kSteamCreateLobbyVtableOffset,
                reinterpret_cast<void*>(&HookedSteamCreateLobby),
                &original,
                "ISteamMatchmaking::CreateLobby"))
        {
            g_originalSteamCreateLobby = reinterpret_cast<SteamCreateLobbyFn>(original);
            g_steamCreateLobbyHookReady = true;
        }
    }

    if (!g_steamSetLobbyMemberLimitHookReady)
    {
        void* original = nullptr;
        if (PatchVtableEntry(
                matchmakingInterface,
                kSteamSetLobbyMemberLimitVtableOffset,
                reinterpret_cast<void*>(&HookedSteamSetLobbyMemberLimit),
                &original,
                "ISteamMatchmaking::SetLobbyMemberLimit"))
        {
            g_originalSteamSetLobbyMemberLimit = reinterpret_cast<SteamSetLobbyMemberLimitFn>(original);
            g_steamSetLobbyMemberLimitHookReady = true;
        }
    }
}

void __fastcall HookedProcessEvent(UObject* object, UFunction* function, void* params)
{
    if (function && params)
    {
        if (function == g_createAdvancedSessionFunc)
        {
            auto* createParams = static_cast<CreateAdvancedSessionParams*>(params);
            ForcePublicConnections(createParams->PublicConnections, "CreateAdvancedSession");
        }
        else if (function == g_updateSessionFunc)
        {
            auto* updateParams = static_cast<UpdateSessionParams*>(params);
            ForcePublicConnections(updateParams->PublicConnections, "UpdateSession");
        }
    }

    g_originalProcessEvent(object, function, params);
}

bool PatchProcessEventSlot(UObject* defaultObject, const char* tag)
{
    if (!defaultObject)
    {
        return false;
    }

    auto*** objectAsVtable = reinterpret_cast<void***>(defaultObject);
    if (!objectAsVtable || !*objectAsVtable)
    {
        Log("%s: missing vtable", tag);
        return false;
    }

    void** vtable = *objectAsVtable;
    void** slot = &vtable[kProcessEventIndex];
    if (!slot)
    {
        Log("%s: invalid ProcessEvent slot", tag);
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Log("%s: VirtualProtect failed (%lu)", tag, GetLastError());
        return false;
    }

    if (*slot != reinterpret_cast<void*>(&HookedProcessEvent))
    {
        if (!g_originalProcessEvent)
        {
            g_originalProcessEvent = reinterpret_cast<ProcessEventFn>(*slot);
            Log("captured original ProcessEvent at %p", reinterpret_cast<void*>(g_originalProcessEvent));
        }

        *slot = reinterpret_cast<void*>(&HookedProcessEvent);
        Log("%s: ProcessEvent hook installed on %p", tag, defaultObject);
    }
    else
    {
        Log("%s: ProcessEvent already hooked", tag);
    }

    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    return true;
}

void TryHookCreateSession()
{
    if (g_createHookReady)
    {
        return;
    }

    auto* createClassObject = reinterpret_cast<UClass*>(
        FindObjectByShortName("CreateSessionCallbackProxyAdvanced", kClassCastFlag));
    if (!createClassObject)
    {
        return;
    }

    if (!g_createAdvancedSessionFunc)
    {
        g_createAdvancedSessionFunc = FindFunctionByName(createClassObject, "CreateAdvancedSession");
    }

    if (!g_createAdvancedSessionFunc)
    {
        Log("CreateSessionCallbackProxyAdvanced found, but CreateAdvancedSession is still missing");
        return;
    }

    g_createHookReady = PatchProcessEventSlot(createClassObject->ClassDefaultObject, "CreateSessionCallbackProxyAdvanced");
}

void TryHookUpdateSession()
{
    if (g_updateHookReady)
    {
        return;
    }

    auto* updateClassObject = reinterpret_cast<UClass*>(
        FindObjectByShortName("UpdateSessionCallbackProxyAdvanced", kClassCastFlag));
    if (!updateClassObject)
    {
        return;
    }

    if (!g_updateSessionFunc)
    {
        g_updateSessionFunc = FindFunctionByName(updateClassObject, "UpdateSession");
    }

    if (!g_updateSessionFunc)
    {
        Log("UpdateSessionCallbackProxyAdvanced found, but UpdateSession is still missing");
        return;
    }

    g_updateHookReady = PatchProcessEventSlot(updateClassObject->ClassDefaultObject, "UpdateSessionCallbackProxyAdvanced");
}

DWORD WINAPI InitWorker(LPVOID)
{
    Log("Room8Fix init thread started");
    TryInstallEosHooks();
    TryInstallSteamHooks();

    const ULONGLONG deadline = GetTickCount64() + 120000;
    while (GetTickCount64() < deadline)
    {
        TryHookCreateSession();
        TryHookUpdateSession();
        TryInstallEosHooks();
        TryInstallSteamHooks();

        if (g_eosLobbyHookReady || g_eosSessionHookReady)
        {
            Log("EOS import hooks ready%s%s",
                g_eosLobbyHookReady ? " [LobbyMaxMembers]" : "",
                g_eosSessionHookReady ? " [SessionMaxPlayers]" : "");
        }

        if (g_steamCreateLobbyHookReady || g_steamSetLobbyMemberLimitHookReady)
        {
            Log("Steam matchmaking hooks ready%s%s",
                g_steamCreateLobbyHookReady ? " [CreateLobby]" : "",
                g_steamSetLobbyMemberLimitHookReady ? " [SetLobbyMemberLimit]" : "");
        }

        if ((g_steamCreateLobbyHookReady || g_eosLobbyHookReady || g_createHookReady) &&
            (g_steamSetLobbyMemberLimitHookReady || g_eosSessionHookReady || g_updateHookReady))
        {
            Log("all available session hooks installed");
            return 0;
        }

        Sleep(1000);
    }

    if (!g_createHookReady)
    {
        Log("timed out waiting for CreateSessionCallbackProxyAdvanced");
    }
    if (!g_updateHookReady)
    {
        Log("UpdateSessionCallbackProxyAdvanced was not found before timeout");
    }
    if (!g_eosLobbyHookReady)
    {
        Log("EOS_LobbyModification_SetMaxMembers import hook was not installed");
    }
    if (!g_eosSessionHookReady)
    {
        Log("EOS_SessionModification_SetMaxPlayers import hook was not installed");
    }
    if (!g_steamCreateLobbyHookReady)
    {
        Log("Steam CreateLobby hook was not installed");
    }
    if (!g_steamSetLobbyMemberLimitHookReady)
    {
        Log("Steam SetLobbyMemberLimit hook was not installed");
    }

    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_selfModule = module;
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_logLock);
        g_logLockReady = true;

        HANDLE thread = CreateThread(nullptr, 0, InitWorker, nullptr, 0, nullptr);
        if (thread)
        {
            CloseHandle(thread);
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_logFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(g_logFile);
            g_logFile = INVALID_HANDLE_VALUE;
        }

        if (g_logLockReady)
        {
            DeleteCriticalSection(&g_logLock);
            g_logLockReady = false;
        }
    }

    return TRUE;
}
