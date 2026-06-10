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
constexpr DWORD kInitTimeoutMs = 120000;
constexpr DWORD kRepairIntervalMs = 250;
constexpr int32_t kRestoredDurability = 999999;

constexpr wchar_t kLogFileName[] = L"InfiniteDurability.log";

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

struct ABP_MainPlayerCharacterLike : UObject
{
    uint8_t Pad28[0x6D0];
    void* InventorySystem;
};

static_assert(offsetof(UWorld, PersistentLevel) == 0x30);
static_assert(offsetof(UWorld, OwningGameInstance) == 0x228);
static_assert(offsetof(UGameInstance, LocalPlayers) == 0x38);
static_assert(offsetof(UPlayer, PlayerController) == 0x30);
static_assert(offsetof(APlayerControllerLike, AcknowledgedPawn) == 0x358);
static_assert(offsetof(ABP_MainPlayerCharacterLike, InventorySystem) == 0x6F8);

#pragma pack(push, 1)
struct InventorySlotSlim
{
    UObject* Item;
    int32_t Quantity;
    uint8_t PadC[0x4];
    FString AdditionalSavedata;
};

struct PlayerDecreaseDurabilityParams
{
    int32_t DecreaseAmt;
    bool ItemDestroyed;
    uint8_t Pad5[0x3];
    uint8_t Scratch[0x50];
};

struct ControllerDecreaseToolDurabilityParams
{
    int32_t DecreaseAmt;
};

struct ServerDecreaseToolDurabilityParams
{
    InventorySlotSlim NewItem;
    bool DurabilityZero;
    uint8_t Pad21[0x3];
    int32_t Index;
};

struct AttributeDecreaseDurabilityParams
{
    InventorySlotSlim Item;
    int32_t DecreaseAmt;
    uint8_t Pad24[0x4];
    UObject* WorldContext;
    InventorySlotSlim NewItem;
    bool DurabilityZero;
    uint8_t Scratch[0xB7];
};

struct AttributeSetItemDurabilityParams
{
    InventorySlotSlim Item;
    int32_t NewDurability;
    uint8_t Pad24[0x4];
    UObject* WorldContext;
    InventorySlotSlim ItemWithDurability;
    uint8_t Scratch[0xA0];
};

struct CharacterGetCurrentHoldItemParams
{
    InventorySlotSlim CurItem;
    bool EmptyHand;
    uint8_t Scratch[0x47];
};

struct CharacterGetInventoryIndexParams
{
    int32_t ReturnValue;
};

struct InventoryOverwriteAndSaveItemAtIndexParams
{
    InventorySlotSlim NewItem;
    int32_t Index;
    uint8_t Pad24[0x4];
};
#pragma pack(pop)

static_assert(sizeof(InventorySlotSlim) == 0x20);
static_assert(offsetof(PlayerDecreaseDurabilityParams, DecreaseAmt) == 0x0);
static_assert(offsetof(PlayerDecreaseDurabilityParams, ItemDestroyed) == 0x4);
static_assert(sizeof(PlayerDecreaseDurabilityParams) == 0x58);
static_assert(offsetof(ControllerDecreaseToolDurabilityParams, DecreaseAmt) == 0x0);
static_assert(offsetof(ServerDecreaseToolDurabilityParams, DurabilityZero) == 0x20);
static_assert(offsetof(ServerDecreaseToolDurabilityParams, Index) == 0x24);
static_assert(sizeof(ServerDecreaseToolDurabilityParams) == 0x28);
static_assert(offsetof(AttributeDecreaseDurabilityParams, DecreaseAmt) == 0x20);
static_assert(offsetof(AttributeDecreaseDurabilityParams, NewItem) == 0x30);
static_assert(offsetof(AttributeDecreaseDurabilityParams, DurabilityZero) == 0x50);
static_assert(sizeof(AttributeDecreaseDurabilityParams) == 0x108);
static_assert(offsetof(AttributeSetItemDurabilityParams, NewDurability) == 0x20);
static_assert(offsetof(AttributeSetItemDurabilityParams, WorldContext) == 0x28);
static_assert(offsetof(AttributeSetItemDurabilityParams, ItemWithDurability) == 0x30);
static_assert(sizeof(AttributeSetItemDurabilityParams) == 0xF0);
static_assert(offsetof(CharacterGetCurrentHoldItemParams, EmptyHand) == 0x20);
static_assert(sizeof(CharacterGetCurrentHoldItemParams) == 0x68);
static_assert(offsetof(InventoryOverwriteAndSaveItemAtIndexParams, Index) == 0x20);
static_assert(sizeof(InventoryOverwriteAndSaveItemAtIndexParams) == 0x28);

using AppendStringFn = void(__fastcall*)(const FName*, FString&);
using ProcessEventFn = void(__fastcall*)(UObject*, UFunction*, void*);

void __fastcall HookedProcessEvent(UObject* object, UFunction* function, void* params);

struct HookEntry
{
    void** Slot;
    ProcessEventFn Original;
    const char* Tag;
};

constexpr size_t kMaxHookEntries = 64;
HookEntry g_hookEntries[kMaxHookEntries] = {};
int g_hookEntryCount = 0;

constexpr const char* kToolClassNames[] = {
    "_BP_HandItem_MASTER_C",
    "BP_HandItem_WoodAxe_C",
    "BP_HandItem_Axe_Metal_C",
    "BP_HandItem_Axe_Diamond_C",
    "BP_HandItem_Axe_Kickstarter_C",
    "BP_HandItem_Pickaxe_Metal_C",
    "BP_HandItem_Pickaxe_Diamond_C",
    "BP_HandItem_Pickaxe_Kickstarter_C",
    "BP_HandItem_Hoe_Metal_C",
    "BP_HandItem_BuildHammer_C",
    "BP_HandItem_Watercan_C",
};

bool g_toolHookReady[_countof(kToolClassNames)] = {};

UClass* g_characterClass = nullptr;
UClass* g_controllerClass = nullptr;
UClass* g_attributeClass = nullptr;
UClass* g_inventoryClass = nullptr;
UObject* g_attributeDefaultObject = nullptr;

UFunction* g_decreaseCurItemDurabilityFunc = nullptr;
UFunction* g_testDurabilityDecreaseFunc = nullptr;
UFunction* g_decreaseItemToolDurabilityFunc = nullptr;
UFunction* g_serverDecreaseItemToolDurabilityFunc = nullptr;
UFunction* g_decreaseDurabilityFunc = nullptr;
UFunction* g_setItemDurabilityFunc = nullptr;
UFunction* g_getCurrentHoldItemFunc = nullptr;
UFunction* g_getInventoryIndexForCurHoldItemFunc = nullptr;
UFunction* g_overwriteAndSaveItemAtIndexFunc = nullptr;

bool g_characterHookReady = false;
bool g_controllerHookReady = false;
bool g_attributeHookReady = false;

volatile LONG g_characterPrevented = 0;
volatile LONG g_controllerPrevented = 0;
volatile LONG g_serverPrevented = 0;
volatile LONG g_attributePrevented = 0;
volatile LONG g_testPrevented = 0;
volatile LONG g_repairCount = 0;

UFunction* g_loggedFunctions[256] = {};
int g_loggedFunctionCount = 0;
ULONGLONG g_nextRepairAt = 0;
thread_local bool g_insideMaintenance = false;

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

bool RecordHookEntry(void** slot, ProcessEventFn original, const char* tag)
{
    if (!slot || !original)
    {
        return false;
    }

    for (int index = 0; index < g_hookEntryCount; ++index)
    {
        if (g_hookEntries[index].Slot == slot)
        {
            return true;
        }
    }

    if (g_hookEntryCount >= static_cast<int>(kMaxHookEntries))
    {
        Log("%s: hook entry table full", tag);
        return false;
    }

    g_hookEntries[g_hookEntryCount++] = HookEntry{ slot, original, tag };
    return true;
}

ProcessEventFn FindOriginalProcessEvent(UObject* object)
{
    if (object)
    {
        auto*** objectAsVtable = reinterpret_cast<void***>(object);
        if (objectAsVtable && *objectAsVtable)
        {
            void** slot = &(*objectAsVtable)[kProcessEventIndex];
            for (int index = 0; index < g_hookEntryCount; ++index)
            {
                if (g_hookEntries[index].Slot == slot)
                {
                    return g_hookEntries[index].Original;
                }
            }

            if (*slot && *slot != reinterpret_cast<void*>(&HookedProcessEvent))
            {
                void* current = *slot;
                return reinterpret_cast<ProcessEventFn>(current);
            }
        }
    }

    return g_hookEntryCount > 0 ? g_hookEntries[0].Original : nullptr;
}

bool CallProcessEvent(UObject* object, UFunction* function, void* params)
{
    if (!object || !function)
    {
        return false;
    }

    ProcessEventFn processEvent = FindOriginalProcessEvent(object);
    if (!processEvent || processEvent == reinterpret_cast<ProcessEventFn>(&HookedProcessEvent))
    {
        return false;
    }

    processEvent(object, function, params);
    return true;
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

bool ContainsText(std::string_view text, std::string_view needle)
{
    return text.find(needle) != std::string_view::npos;
}

bool IsToolItemClass(UObject* itemClass)
{
    if (!itemClass)
    {
        return false;
    }

    const std::string itemName = FNameToString(itemClass->Name);
    return ContainsText(itemName, "Axe") ||
        ContainsText(itemName, "Pickaxe") ||
        ContainsText(itemName, "Hoe") ||
        ContainsText(itemName, "Hammer") ||
        ContainsText(itemName, "Watercan") ||
        ContainsText(itemName, "FishingRod") ||
        ContainsText(itemName, "Rod") ||
        ContainsText(itemName, "Tool");
}

void LogInterestingFunction(UObject* object, UFunction* function)
{
    if (!function || g_loggedFunctionCount >= static_cast<int>(_countof(g_loggedFunctions)))
    {
        return;
    }

    for (int index = 0; index < g_loggedFunctionCount; ++index)
    {
        if (g_loggedFunctions[index] == function)
        {
            return;
        }
    }

    const std::string functionName = FNameToString(function->Name);
    if (!ContainsText(functionName, "Durability") &&
        !ContainsText(functionName, "Tool") &&
        !ContainsText(functionName, "Axe") &&
        !ContainsText(functionName, "Pickaxe") &&
        !ContainsText(functionName, "HandItem") &&
        !ContainsText(functionName, "Overwrite"))
    {
        return;
    }

    g_loggedFunctions[g_loggedFunctionCount++] = function;

    std::string className;
    if (object && object->Class)
    {
        className = FNameToString(object->Class->Name);
    }
    Log("observed ProcessEvent: %s::%s (%p)",
        className.empty() ? "<unknown>" : className.c_str(),
        functionName.c_str(),
        function);
}

bool ResolveFunction(UClass* classObject, std::string_view functionName, UFunction*& targetFunction, const char* tag)
{
    if (targetFunction)
    {
        return true;
    }

    targetFunction = FindFunctionByName(classObject, functionName);
    if (targetFunction)
    {
        Log("resolved %s at %p", tag, targetFunction);
        return true;
    }

    return false;
}

bool ResolveRuntimeFunctions()
{
    if (!g_characterClass)
    {
        g_characterClass = reinterpret_cast<UClass*>(FindObjectByShortName("BP_MainPlayerCharacter_C", kClassCastFlag));
    }
    if (!g_controllerClass)
    {
        g_controllerClass = reinterpret_cast<UClass*>(FindObjectByShortName("BP_MainPlayerController_C", kClassCastFlag));
    }
    if (!g_attributeClass)
    {
        g_attributeClass = reinterpret_cast<UClass*>(FindObjectByShortName("BPL_AttributeFunctions_C", kClassCastFlag));
        if (g_attributeClass)
        {
            g_attributeDefaultObject = g_attributeClass->ClassDefaultObject;
        }
    }
    if (!g_inventoryClass)
    {
        g_inventoryClass = reinterpret_cast<UClass*>(FindObjectByShortName("BC_InventorySystem_C", kClassCastFlag));
    }

    if (g_characterClass)
    {
        ResolveFunction(g_characterClass,
            "DecreaseCurItemDurability",
            g_decreaseCurItemDurabilityFunc,
            "BP_MainPlayerCharacter_C::DecreaseCurItemDurability");
        ResolveFunction(g_characterClass,
            "TestDurabilityDecrease",
            g_testDurabilityDecreaseFunc,
            "BP_MainPlayerCharacter_C::TestDurabilityDecrease");
        ResolveFunction(g_characterClass,
            "GetCurrentHoldItem",
            g_getCurrentHoldItemFunc,
            "BP_MainPlayerCharacter_C::GetCurrentHoldItem");
        ResolveFunction(g_characterClass,
            "GetInventoryIndexForCurHoldItem",
            g_getInventoryIndexForCurHoldItemFunc,
            "BP_MainPlayerCharacter_C::GetInventoryIndexForCurHoldItem");
    }

    if (g_controllerClass)
    {
        ResolveFunction(g_controllerClass,
            "DecreaseItemToolDurability",
            g_decreaseItemToolDurabilityFunc,
            "BP_MainPlayerController_C::DecreaseItemToolDurability");
        ResolveFunction(g_controllerClass,
            "SERVER_DecreaseItemToolDurability",
            g_serverDecreaseItemToolDurabilityFunc,
            "BP_MainPlayerController_C::SERVER_DecreaseItemToolDurability");
    }

    if (g_attributeClass)
    {
        ResolveFunction(g_attributeClass,
            "DecreaseDurability",
            g_decreaseDurabilityFunc,
            "BPL_AttributeFunctions_C::DecreaseDurability");
        ResolveFunction(g_attributeClass,
            "SetItemDurability",
            g_setItemDurabilityFunc,
            "BPL_AttributeFunctions_C::SetItemDurability");
    }

    if (g_inventoryClass)
    {
        ResolveFunction(g_inventoryClass,
            "OverwriteAndSaveItemAtIndex",
            g_overwriteAndSaveItemAtIndexFunc,
            "BC_InventorySystem_C::OverwriteAndSaveItemAtIndex");
    }

    return g_getCurrentHoldItemFunc &&
        g_getInventoryIndexForCurHoldItemFunc &&
        g_setItemDurabilityFunc &&
        g_overwriteAndSaveItemAtIndexFunc &&
        g_attributeDefaultObject;
}

bool RestoreDurabilityOnItem(InventorySlotSlim& item, UObject* context, const char* sourceTag)
{
    if (!item.Item || !IsToolItemClass(item.Item) || !ResolveRuntimeFunctions())
    {
        return false;
    }

    AttributeSetItemDurabilityParams setParams{};
    setParams.Item = item;
    setParams.NewDurability = kRestoredDurability;
    setParams.WorldContext = context;

    if (!CallProcessEvent(g_attributeDefaultObject, g_setItemDurabilityFunc, &setParams))
    {
        return false;
    }

    if (!setParams.ItemWithDurability.Item)
    {
        return false;
    }

    item = setParams.ItemWithDurability;
    const LONG hitCount = InterlockedIncrement(&g_repairCount);
    if (hitCount <= 10 || (hitCount % 50) == 0)
    {
        const std::string itemName = FNameToString(item.Item->Name);
        Log("%s: restored %s durability to %d (repair %ld)",
            sourceTag,
            itemName.c_str(),
            kRestoredDurability,
            hitCount);
    }
    return true;
}

void RepairCurrentHeldTool()
{
    if (g_insideMaintenance || !ResolveRuntimeFunctions())
    {
        return;
    }

    g_insideMaintenance = true;

    UObject* localCharacter = reinterpret_cast<UObject*>(GetLocalCharacter());
    if (!localCharacter)
    {
        g_insideMaintenance = false;
        return;
    }

    auto* character = reinterpret_cast<ABP_MainPlayerCharacterLike*>(localCharacter);
    UObject* inventorySystem = reinterpret_cast<UObject*>(character->InventorySystem);
    if (!inventorySystem)
    {
        g_insideMaintenance = false;
        return;
    }

    CharacterGetCurrentHoldItemParams holdParams{};
    if (!CallProcessEvent(localCharacter, g_getCurrentHoldItemFunc, &holdParams) || holdParams.EmptyHand)
    {
        g_insideMaintenance = false;
        return;
    }

    if (!holdParams.CurItem.Item || !IsToolItemClass(holdParams.CurItem.Item))
    {
        g_insideMaintenance = false;
        return;
    }

    CharacterGetInventoryIndexParams indexParams{};
    if (!CallProcessEvent(localCharacter, g_getInventoryIndexForCurHoldItemFunc, &indexParams) || indexParams.ReturnValue < 0)
    {
        g_insideMaintenance = false;
        return;
    }

    InventorySlotSlim restoredItem = holdParams.CurItem;
    if (RestoreDurabilityOnItem(restoredItem, localCharacter, "held-tool repair"))
    {
        InventoryOverwriteAndSaveItemAtIndexParams overwriteParams{};
        overwriteParams.NewItem = restoredItem;
        overwriteParams.Index = indexParams.ReturnValue;
        CallProcessEvent(inventorySystem, g_overwriteAndSaveItemAtIndexFunc, &overwriteParams);
    }

    g_insideMaintenance = false;
}

void MaybeRepairCurrentHeldTool()
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextRepairAt)
    {
        return;
    }

    g_nextRepairAt = now + kRepairIntervalMs;
    RepairCurrentHeldTool();
}

void ForceZeroDecrease(int32_t& decreaseAmt, const char* sourceTag, volatile LONG& counter)
{
    if (decreaseAmt <= 0)
    {
        return;
    }

    const int32_t original = decreaseAmt;
    decreaseAmt = 0;

    const LONG hitCount = InterlockedIncrement(&counter);
    if (hitCount <= 10 || (hitCount % 50) == 0)
    {
        Log("%s: decrease %d -> 0 (hit %ld)", sourceTag, original, hitCount);
    }
}

void HandleKnownDurabilityFunction(UObject* object, UFunction* function, void* params)
{
    if (!function || !params)
    {
        return;
    }

    if (function == g_decreaseCurItemDurabilityFunc)
    {
        auto* durabilityParams = static_cast<PlayerDecreaseDurabilityParams*>(params);
        ForceZeroDecrease(durabilityParams->DecreaseAmt,
            "BP_MainPlayerCharacter::DecreaseCurItemDurability",
            g_characterPrevented);
    }
    else if (function == g_decreaseItemToolDurabilityFunc)
    {
        auto* durabilityParams = static_cast<ControllerDecreaseToolDurabilityParams*>(params);
        ForceZeroDecrease(durabilityParams->DecreaseAmt,
            "BP_MainPlayerController::DecreaseItemToolDurability",
            g_controllerPrevented);
    }
    else if (function == g_serverDecreaseItemToolDurabilityFunc)
    {
        auto* durabilityParams = static_cast<ServerDecreaseToolDurabilityParams*>(params);
        durabilityParams->DurabilityZero = false;
        if (RestoreDurabilityOnItem(durabilityParams->NewItem, object, "SERVER_DecreaseItemToolDurability"))
        {
            InterlockedIncrement(&g_serverPrevented);
        }
    }
    else if (function == g_decreaseDurabilityFunc)
    {
        auto* durabilityParams = static_cast<AttributeDecreaseDurabilityParams*>(params);
        ForceZeroDecrease(durabilityParams->DecreaseAmt,
            "BPL_AttributeFunctions::DecreaseDurability",
            g_attributePrevented);
    }
    else if (function == g_testDurabilityDecreaseFunc)
    {
        auto* durabilityParams = static_cast<PlayerDecreaseDurabilityParams*>(params);
        ForceZeroDecrease(durabilityParams->DecreaseAmt,
            "BP_MainPlayerCharacter::TestDurabilityDecrease",
            g_testPrevented);
    }
}

void __fastcall HookedProcessEvent(UObject* object, UFunction* function, void* params)
{
    LogInterestingFunction(object, function);
    HandleKnownDurabilityFunction(object, function, params);
    MaybeRepairCurrentHeldTool();

    ProcessEventFn original = FindOriginalProcessEvent(object);
    if (original && original != &HookedProcessEvent)
    {
        original(object, function, params);
    }
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

    if (*slot == reinterpret_cast<void*>(&HookedProcessEvent))
    {
        return true;
    }

    ProcessEventFn original = reinterpret_cast<ProcessEventFn>(*slot);
    if (!RecordHookEntry(slot, original, tag))
    {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Log("%s: VirtualProtect failed (%lu)", tag, GetLastError());
        return false;
    }

    *slot = reinterpret_cast<void*>(&HookedProcessEvent);
    Log("%s: ProcessEvent hook installed on %p, original=%p",
        tag,
        defaultObject,
        reinterpret_cast<void*>(original));

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

void TryInstallHooks()
{
    ResolveRuntimeFunctions();

    if (g_characterClass && g_decreaseCurItemDurabilityFunc)
    {
        TryHookClassByName("BP_MainPlayerCharacter_C", g_characterHookReady);
    }

    if (g_controllerClass && g_decreaseItemToolDurabilityFunc)
    {
        TryHookClassByName("BP_MainPlayerController_C", g_controllerHookReady);
    }

    if (g_attributeClass && g_decreaseDurabilityFunc)
    {
        TryHookClassByName("BPL_AttributeFunctions_C", g_attributeHookReady);
    }

    for (size_t index = 0; index < _countof(kToolClassNames); ++index)
    {
        TryHookClassByName(kToolClassNames[index], g_toolHookReady[index]);
    }
}

bool AnyToolHookReady()
{
    for (bool ready : g_toolHookReady)
    {
        if (ready)
        {
            return true;
        }
    }

    return false;
}

DWORD WINAPI InitWorker(LPVOID)
{
    Log("InfiniteDurability init thread started");

    const ULONGLONG deadline = GetTickCount64() + kInitTimeoutMs;
    while (GetTickCount64() < deadline)
    {
        TryInstallHooks();

        if (g_characterHookReady && g_controllerHookReady && g_attributeHookReady && AnyToolHookReady())
        {
            Log("infinite durability hooks installed [character=%d controller=%d attribute=%d tool=%d]",
                g_characterHookReady ? 1 : 0,
                g_controllerHookReady ? 1 : 0,
                g_attributeHookReady ? 1 : 0,
                AnyToolHookReady() ? 1 : 0);
            return 0;
        }

        Sleep(1000);
    }

    Log("hook init timeout [character=%d controller=%d attribute=%d tool=%d setItem=%d overwrite=%d]",
        g_characterHookReady ? 1 : 0,
        g_controllerHookReady ? 1 : 0,
        g_attributeHookReady ? 1 : 0,
        AnyToolHookReady() ? 1 : 0,
        g_setItemDurabilityFunc ? 1 : 0,
        g_overwriteAndSaveItemAtIndexFunc ? 1 : 0);

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
