#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace
{
constexpr uintptr_t kGObjectsOffset = 0x076CD920;
constexpr uintptr_t kGWorldOffset = 0x078CFBB8;
constexpr uintptr_t kAppendStringOffset = 0x0122BD08;
constexpr size_t kProcessEventIndex = 0x4C;
constexpr uint64_t kClassCastFlag = 0x20;
constexpr uint64_t kFunctionCastFlag = 0x0000000000080000ULL;
constexpr DWORD kSweepIntervalMs = 1000;

constexpr wchar_t kLogFileName[] = L"AirshipShare.log";

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

    int32_t Num() const
    {
        return NumElements;
    }

    explicit operator bool() const
    {
        return Data != nullptr && NumElements > 0;
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

struct UWorld : UObject
{
    uint8_t Pad28[0x8];
    void* PersistentLevel;
    uint8_t Pad38[0x1F0];
    void* OwningGameInstance;
};

struct ULevel : UObject
{
    uint8_t Pad28[0x78];
    TArray<void*> Actors;
};

struct UGameInstance : UObject
{
    uint8_t Pad28[0x10];
    TArray<void*> LocalPlayers;
};

struct UPlayer : UObject
{
    uint8_t Pad28[0x8];
    void* PlayerController;
};

struct APlayerControllerLike : UObject
{
    uint8_t Pad28[0x328];
    void* Player;
    void* AcknowledgedPawn;
};

struct ABP_AirshipLike : UObject
{
    uint8_t Pad28[0x338];
    void* AirshipCharacterAnimSync;
    void* NonOwnerBlocker;
    uint8_t Pad370[0x6F8];
    FString OwningPlayer;
};

static_assert(offsetof(UWorld, OwningGameInstance) == 0x228);
static_assert(offsetof(UWorld, PersistentLevel) == 0x30);
static_assert(offsetof(UGameInstance, LocalPlayers) == 0x38);
static_assert(offsetof(ULevel, Actors) == 0xA0);
static_assert(offsetof(UPlayer, PlayerController) == 0x30);
static_assert(offsetof(APlayerControllerLike, AcknowledgedPawn) == 0x358);
static_assert(offsetof(ABP_AirshipLike, NonOwnerBlocker) == 0x368);
static_assert(offsetof(ABP_AirshipLike, OwningPlayer) == 0x0A68);

#pragma pack(push, 1)
struct PrimitiveComponentSetCollisionEnabledParams
{
    uint8_t NewType;
};

struct AirshipUnblockParams
{
    void* Character;
    bool Unblock;
};
#pragma pack(pop)

static_assert(sizeof(PrimitiveComponentSetCollisionEnabledParams) == 1);
static_assert(offsetof(AirshipUnblockParams, Character) == 0x0);
static_assert(offsetof(AirshipUnblockParams, Unblock) == 0x8);

enum class ECollisionEnabled : uint8_t
{
    NoCollision = 0,
};

using AppendStringFn = void(__fastcall*)(const FName*, FString&);
using ProcessEventFn = void(__fastcall*)(UObject*, UFunction*, void*);

ProcessEventFn g_originalProcessEvent = nullptr;
UClass* g_airshipClass = nullptr;
UFunction* g_unblockAirshipForCharacter = nullptr;
UClass* g_primitiveComponentClass = nullptr;
UFunction* g_setCollisionEnabled = nullptr;

bool g_controllerHookReady = false;
bool g_characterHookReady = false;

ULONGLONG g_nextSweepAt = 0;
int g_lastAirshipCount = -1;
bool g_loggedResolveState = false;

uintptr_t GetImageBase()
{
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

std::wstring GetModuleDirectory(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, static_cast<DWORD>(_countof(path)));

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

UWorld* GetWorld()
{
    auto** worldSlot = reinterpret_cast<UWorld**>(GetImageBase() + kGWorldOffset);
    return worldSlot ? *worldSlot : nullptr;
}

std::string FNameToString(const FName& name)
{
    wchar_t buffer[1024] = {};
    FString out(buffer, 0, static_cast<int32_t>(_countof(buffer)));

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

ProcessEventFn ResolveProcessEvent(UObject* object)
{
    if (g_originalProcessEvent)
    {
        return g_originalProcessEvent;
    }

    if (!object)
    {
        return nullptr;
    }

    auto*** objectAsVtable = reinterpret_cast<void***>(object);
    if (!objectAsVtable || !*objectAsVtable)
    {
        return nullptr;
    }

    void** vtable = *objectAsVtable;
    g_originalProcessEvent = reinterpret_cast<ProcessEventFn>(vtable[kProcessEventIndex]);
    Log("captured original ProcessEvent at %p", reinterpret_cast<void*>(g_originalProcessEvent));
    return g_originalProcessEvent;
}

void CallProcessEvent(UObject* object, UFunction* function, void* params)
{
    ProcessEventFn processEvent = ResolveProcessEvent(object);
    if (!processEvent || !object || !function)
    {
        return;
    }

    processEvent(object, function, params);
}

void* GetLocalCharacter()
{
    UWorld* world = GetWorld();
    if (!world || !world->OwningGameInstance)
    {
        return nullptr;
    }

    auto* gameInstance = static_cast<UGameInstance*>(world->OwningGameInstance);
    if (!gameInstance->LocalPlayers || gameInstance->LocalPlayers.Num() <= 0)
    {
        return nullptr;
    }

    auto* localPlayer = static_cast<UPlayer*>(gameInstance->LocalPlayers.Data[0]);
    if (!localPlayer || !localPlayer->PlayerController)
    {
        return nullptr;
    }

    auto* controller = static_cast<APlayerControllerLike*>(localPlayer->PlayerController);
    return controller->AcknowledgedPawn;
}

void ResolveAirshipFunctions()
{
    if (!g_airshipClass)
    {
        g_airshipClass = reinterpret_cast<UClass*>(FindObjectByShortName("BP_Airship_C", kClassCastFlag));
        if (g_airshipClass)
        {
            Log("resolved BP_Airship_C class at %p", g_airshipClass);
        }
    }

    if (g_airshipClass && !g_unblockAirshipForCharacter)
    {
        g_unblockAirshipForCharacter = FindFunctionByName(g_airshipClass, "UnblockAirshipForCharacter");
        if (g_unblockAirshipForCharacter)
        {
            Log("resolved BP_Airship_C::UnblockAirshipForCharacter at %p", g_unblockAirshipForCharacter);
        }
    }

    if (!g_primitiveComponentClass)
    {
        g_primitiveComponentClass = reinterpret_cast<UClass*>(FindObjectByShortName("PrimitiveComponent", kClassCastFlag));
        if (g_primitiveComponentClass)
        {
            Log("resolved PrimitiveComponent class at %p", g_primitiveComponentClass);
        }
    }

    if (g_primitiveComponentClass && !g_setCollisionEnabled)
    {
        g_setCollisionEnabled = FindFunctionByName(g_primitiveComponentClass, "SetCollisionEnabled");
        if (g_setCollisionEnabled)
        {
            Log("resolved PrimitiveComponent::SetCollisionEnabled at %p", g_setCollisionEnabled);
        }
    }

    if (!g_loggedResolveState && g_airshipClass && g_unblockAirshipForCharacter && g_setCollisionEnabled)
    {
        Log("airship share logic fully resolved");
        g_loggedResolveState = true;
    }
}

void SweepAirships()
{
    ResolveAirshipFunctions();

    if (!g_airshipClass || !g_unblockAirshipForCharacter || !g_setCollisionEnabled)
    {
        return;
    }

    void* localCharacter = GetLocalCharacter();
    if (!localCharacter)
    {
        return;
    }

    UWorld* world = GetWorld();
    if (!world || !world->PersistentLevel)
    {
        return;
    }

    auto* level = static_cast<ULevel*>(world->PersistentLevel);
    if (!level->Actors || level->Actors.Num() <= 0)
    {
        return;
    }

    int airshipCount = 0;

    for (int32_t index = 0; index < level->Actors.Num(); ++index)
    {
        UObject* object = reinterpret_cast<UObject*>(level->Actors.Data[index]);
        if (!object || object == g_airshipClass->ClassDefaultObject)
        {
            continue;
        }

        if (object->Class != g_airshipClass)
        {
            continue;
        }

        ++airshipCount;

        auto* airship = reinterpret_cast<ABP_AirshipLike*>(object);
        if (airship->NonOwnerBlocker)
        {
            PrimitiveComponentSetCollisionEnabledParams collisionParams{};
            collisionParams.NewType = static_cast<uint8_t>(ECollisionEnabled::NoCollision);
            CallProcessEvent(static_cast<UObject*>(airship->NonOwnerBlocker), g_setCollisionEnabled, &collisionParams);
        }

        AirshipUnblockParams unblockParams{};
        unblockParams.Character = localCharacter;
        unblockParams.Unblock = true;
        CallProcessEvent(object, g_unblockAirshipForCharacter, &unblockParams);
    }

    if (airshipCount != g_lastAirshipCount)
    {
        g_lastAirshipCount = airshipCount;
        Log("airship sweep active: %d airships, local character=%p", airshipCount, localCharacter);
    }
}

void __fastcall HookedProcessEvent(UObject* object, UFunction* function, void* params)
{
    const ULONGLONG now = GetTickCount64();
    if (now >= g_nextSweepAt)
    {
        g_nextSweepAt = now + kSweepIntervalMs;
        SweepAirships();
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

    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    return true;
}

bool TryHookClassByName(const char* className, bool& hookReady)
{
    if (hookReady)
    {
        return true;
    }

    auto* classObject = reinterpret_cast<UClass*>(FindObjectByShortName(className, kClassCastFlag));
    if (!classObject)
    {
        return false;
    }

    if (!classObject->ClassDefaultObject)
    {
        Log("%s found without class default object", className);
        return false;
    }

    hookReady = PatchProcessEventSlot(classObject->ClassDefaultObject, className);
    return hookReady;
}

DWORD WINAPI InitWorker(LPVOID)
{
    Log("AirshipShare init thread started");

    while (true)
    {
        TryHookClassByName("BP_MainPlayerController_C", g_controllerHookReady);
        TryHookClassByName("BP_MainPlayerCharacter_C", g_characterHookReady);

        if (g_controllerHookReady || g_characterHookReady)
        {
            Log("airship share hook ready%s%s",
                g_controllerHookReady ? " [controller]" : "",
                g_characterHookReady ? " [character]" : "");
            return 0;
        }

        Sleep(1000);
    }
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
