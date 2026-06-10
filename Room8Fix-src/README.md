# Room8Fix
这个MOD可修改游戏的默认4人为8人房间(没测试)！

你可以用任何注入工具进行MOD注入这个MOD

以本地文件的winmm.dll钩子为例：
`Room8Fix.dll` 会通过 `winmm.txt` 作为附加模块加载，并在运行时把 `AdvancedSessions` 的 `PublicConnections` 从 `4` 提升到 `8`。

构建：

`Room8Fix-src\build.bat`

恢复原状：

1. 把 `Solarpunk\Binaries\Win64\winmm.txt` 改回只保留 `SteamFix64.dll`
2. 删除 `Solarpunk\Binaries\Win64\Room8Fix.dll`
