# 真随机开局

英雄无敌3 HD Mod 插件。两档随机：

1. 主菜单「新游戏 → 单人场景」的选图界面增加「开局真随机」复选框。勾上后，这场新游戏开局每次取随机数都向系统要一个新数；不勾则仍用原版伪随机。
2. 游戏内随时按热键（默认 F11）打开设置窗，可开「全局真随机」——游戏全程（含战斗等）随机数都换成系统级随机；还可调日志等级、修改热键本身。

开局档只影响单人场景点「开始」之后的开局：固定场景换这场档案，随机地图连人数和地图布局一起换。多人、战役、教程、读取旧档、已经在打的游戏和战斗都不改。全局档才覆盖战斗等全程取数；联机锁定回合时不要开全局档。

目标版本是 Shadow of Death（`SOD = 0xFFFFE403`），仅 x86。

## 行为

### 开局真随机（选图界面复选框）

- 复选框在「显示可选场景」右侧，第一次打开默认勾上。
- 点一下即切换，当时写入 `H3RndNew.user.ini`，下次启动恢复。
- 勾上：开局期间游戏 `Rand` 和随机图直接调用的 `_rand` 都改为系统随机数。
- 不勾：这两处走原版伪随机公式。
- 系统随机数取不到的那一次退回原版，不用时间补。

### 设置窗（游戏内热键，默认 F11）

- 冒险地图、战斗、主菜单按热键都能打开；选图等 H3 自己的模态界面里暂不保证。
- 窗里三样：日志等级下拉框、全局真随机复选框（默认不勾）和热键修改。
- 日志等级下拉五级：trace/debug/info/warn/error，点选立即生效并写入 user.ini；ESC、回车、点空白处只收起不改动。默认 info。
- 点「全局真随机」即切换并立即写入 user.ini；开启后全程 `Rand` / `_rand` 都走系统级随机（开局复选框此时被全局档覆盖）。
- 点键名（如 `F11`）进入侦听，按一个新键即生效（ESC 取消）；修饰键单独按不生效。改完即写入 user.ini。
- F12 已被 SoD_SP 插件的设置占用，别选。
- 热键存的是键盘扫描码（=游戏内部键码），默认 87=F11。

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

它用 Visual Studio 2026 的 MSBuild 做 Release|Win32 全量 Rebuild。产物是 `Release\H3RndNew.dll`。编译依赖仓库里的 `H3API\single_header`，不另带一份头文件。先 `set H3RND_RUN_AUDIT_TESTS=1 && build.bat` 会额外编译并运行不启动游戏的随机审计独立测试（`tests\random_audit_test.cpp`）。

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
TrueRandomFull=0
SettingsHotkeyScan=87

[Logging]
DisableLog=0
MinLevel=info
```

`TrueRandom` 是界面没有读到玩家配置时的出厂勾选，`0` 不勾，`1` 勾上。`TrueRandomFull` 是全局真随机（设置窗里切换，`1` 全程替换）。`SettingsHotkeyScan` 是设置窗热键的扫描码（默认 87=F11；F1..F10=59..68，A..Z=30..55，F12=88 勿用）。后两项在游戏内设置窗里改过之后以 user 层为准，不要手改 default。

日志写在插件自己的目录，文件名是 `H3RndNew_日期_时间.log`。`DisableLog=1` 完全不写。`MinLevel` 可选 `trace`、`debug`、`info`、`warn`、`error`。运行期间的 `RNG审计[…]` 行按入口分类汇总全部随机调用（真随机/原版路径/失败回退等）。Info 保留关键边界快照及有调用时每 5 分钟的心跳，Debug 每 30 秒最多一组心跳、只列关键控件，完整控件清单留给 Trace；未启用且无历史数据的全局空统计省略。Hook 链首次/变化详情和故障诊断仍保留。读法与复测步骤见 `使用说明.txt`。
