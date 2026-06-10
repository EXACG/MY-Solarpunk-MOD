这是一个Solarpunk 的 MOD
发布在：https://www.nexusmods.com/solarpunk/mods/5

# AirshipShare

`AirshipShare.dll` 会在本地玩家的 `ProcessEvent` 上挂钩，并每秒对所有 `BP_Airship_C` 实例执行两层放行：

- 调 `BP_Airship_C::UnblockAirshipForCharacter(LocalCharacter, true)`
- 调 `NonOwnerBlocker->SetCollisionEnabled(NoCollision)` 作为兜底

这样非拥有者也能进入同一条飞艇。

## 构建

`AirshipShare-src\build.bat`

生成物：`Solarpunk\Binaries\Win64\AirshipShare.dll`

## 启用
你可以用任何注入工具进行MOD注入

以本地文件的winmm.dll钩子为例：
把 `AirshipShare.dll` 加到 `Solarpunk\Binaries\Win64\winmm.txt`，每行一个模块，例如：

```text
AirshipShare.dll
```

## 日志

运行后会在 `Solarpunk\Binaries\Win64\AirshipShare.log` 输出：

- Hook 是否装上
- 目标类 / 函数是否解析成功
- 当前扫到的飞艇数量

## 回退

以本地文件的winmm.dll钩子为例：
1. 从 `Solarpunk\Binaries\Win64\winmm.txt` 删除 `AirshipShare.dll`
2. 删除 `Solarpunk\Binaries\Win64\AirshipShare.dll`
