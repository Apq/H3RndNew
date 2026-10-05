# 真随机开局

英雄无敌3 HD Mod 插件。主菜单「新游戏 → 单人场景」的选图界面增加「开局真随机」复选框。勾上后，这场新游戏开局每次取随机数都向系统要一个新数；不勾则仍用原版伪随机。

只影响单人场景点「开始」之后的开局：固定场景换这场档案，随机地图连人数和地图布局一起换。多人、战役、教程、读取旧档、已经在打的游戏和战斗都不改。

目标版本是 Shadow of Death（`SOD = 0xFFFFE403`），仅 x86。

## 行为

- 复选框在「显示可选场景」右侧，第一次打开默认勾上。
- 点一下即切换，当时写入 `H3RndNew.user.ini`，下次启动恢复。
- 勾上：开局期间游戏 `Rand` 和随机图直接调用的 `_rand` 都改为系统随机数。
- 不勾：这两处走原版伪随机公式。
- 系统随机数取不到的那一次退回原版，不用时间补。

玩家能看到的完整行为见 [需求文档.md](需求文档.md)，接入点和取数替换见 [设计文档.md](设计文档.md)。

## 部署

```text
<游戏目录>\_HD3_Data\Packs\真随机开局\
```

目录里放：

```text
H3RndNew.dll
H3RndNew.default.ini
使用说明.txt
```

`deploy.ps1` 把这三个文件复制到 `D:\Heroes3\Heroes3_2026.05.01\_HD3_Data\Packs\真随机开局`。然后在 HD Mod 启动器的插件页，把「真随机开局」加到已加载。

## 构建

在本目录执行：

```text
build.bat
```

它用 Visual Studio 2026 的 MSBuild 做 Release|Win32 全量 Rebuild。产物是 `Release\H3RndNew.dll`。编译依赖仓库里的 `H3API\single_header`，不另带一份头文件。

## 打包

在本目录执行：

```text
pack.bat
```

它把部署目录（默认 `D:\Heroes3\Heroes3_2026.05.01\_HD3_Data\Packs\真随机开局`）里的插件文件打成 `Release\真随机开局_vX.Y.zip`，版本号取自 `H3RndNew.rc` 的 `FileVersion` 前两段。包内带 `真随机开局\` 前缀目录，解压到游戏 `Packs` 即装。运行日志（`*.log`）和玩家配置 `H3RndNew.user.ini` 自动排除。成功后窗口默认直接关闭，要看结果先 `set PAUSE_ON_SUCCESS=1 && pack.bat`。

## 配置

`H3RndNew.default.ini` 是出厂默认，升级会被覆盖。玩家改动写在同目录的 `H3RndNew.user.ini`，没有这个文件也能跑。

```ini
[General]
TrueRandom=1

[Logging]
DisableLog=0
MinLevel=info
```

`TrueRandom` 是界面没有读到玩家配置时的出厂勾选，`0` 不勾，`1` 勾上。界面上改过之后以 user 层为准，不要手改 default。

日志写在插件自己的目录，文件名是 `H3RndNew_日期_时间.log`。`DisableLog=1` 完全不写。`MinLevel` 可选 `trace`、`debug`、`info`、`warn`、`error`。
