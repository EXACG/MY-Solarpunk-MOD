# BUG
根据反馈，可能会导致飞艇异常?

# InfiniteDurability

`InfiniteDurability.dll` 会通过 `ProcessEvent` Hook 把工具耐久扣除量改成 `0`，用于斧头、镐子等手持工具无限耐久。

## 实现方式

- Hook `BP_MainPlayerCharacter_C::DecreaseCurItemDurability(int32 DecreaseAmt, bool* ItemDestroyed)`
- Hook `BP_MainPlayerController_C::DecreaseItemToolDurability(int32 DecreaseAmt)`
- 兜底 Hook `BPL_AttributeFunctions_C::DecreaseDurability(..., int32 DecreaseAmt, ...)`

只改扣除量，不直接改背包物品结构，也不直接改建筑、放置物等世界对象的 `Durability` 字段。任何走这些手持物品扣耐久入口的物品都会被保护。

## 构建

运行：

```bat
InfiniteDurability-src\build.bat
```

生成物：`Solarpunk\Binaries\Win64\InfiniteDurability.dll`

## 启用
你可以用任何注入工具进行MOD注入

以本地文件的winmm.dll钩子为例：
把 `InfiniteDurability.dll` 加到 `Solarpunk\Binaries\Win64\winmm.txt`，每行一个模块，例如：

```text
InfiniteDurability.dll
```

## 日志

运行后会在 `Solarpunk\Binaries\Win64\InfiniteDurability.log` 输出：

- Hook 是否装上
- 目标函数是否解析成功
- 每个扣耐久入口的命中次数

关键命中日志示例：

```text
BP_MainPlayerCharacter::DecreaseCurItemDurability: decrease 1 -> 0 (hit 1)
```

## 回退

1. 从 `Solarpunk\Binaries\Win64\winmm.txt` 删除 `InfiniteDurability.dll`
2. 删除 `Solarpunk\Binaries\Win64\InfiniteDurability.dll`
3. 如需清理日志，删除 `Solarpunk\Binaries\Win64\InfiniteDurability.log`
