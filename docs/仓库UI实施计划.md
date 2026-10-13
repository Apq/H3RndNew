# 仓库 UI 实施计划

> 项目：H3RndNew「仓库」
> 目标：在 HD_SOD 完整 8×8 背包弹窗内显示并操作当前英雄的外置仓库。
> 记录日期：2026-10-12
> 执行纪律：本文件是 UI 阶段的执行基线。每完成一个阶段，先完成该阶段的离线回归和文档状态更新，再进入下一阶段；任何未确认的原生 ABI、对象生命周期或版本事实都不得直接写成生产 Hook。

## 1. 当前基线

### 1.1 已完成并通过实机验证

- `WarehouseCore.hpp`：156 个固定英雄仓库、动态库存、原生 64 格导入/导出。
- `WarehouseCodec.hpp`：v1 专属附件格式、原生前缀绑定、第三方尾部保护、坏块拒读。
- `WarehouseStore.hpp`：保存/读档事务策略、失败保全、损坏附件锁定。
- `WarehouseNative.hpp`：zlib raw-deflate 边界读取、ISIZE 核对；游戏样本 trailer CRC 不作为信任条件。
- `WarehouseStore.inc.cpp`：SaveGame/LoadGame 后置接线、实际路径捕获、原子追加、新局清空。
- 游戏内存档回归：用户已验证新局、旧档、重复保存、带附件读档和自动存档均无问题。
- 日志编码：UTF-8 BOM；仓库路径已从游戏 ANSI/GBK 转为 UTF-8 后再写日志。

### 1.2 UI 目标与明确非目标

第一版 UI 只在 HD_SOD 的完整 8×8 背包弹窗中出现，服务当前打开的英雄；不扩大原生英雄结构，不修改原生背包 64 格数组，不创建第二个嵌套模态窗口。

第一版不承诺战役换图、联机同步、HotA/WoG、商店/祭坛/任务交付全链透明识别；未覆盖的原版取得路径不自动写入仓库。游戏内画面、点击、重开和连续操作由用户验收。

## 2. 固定逆向事实

以下事实来自 `D:\logs\dsh\scripts\h3-warehouse-ui\ui-audit-report.md` 和 `H3Note\英雄宝物背包容量与自动整理逆向笔记.md`，目标为本机 SoD/HD_SOD：

| 项目 | 事实 |
| --- | --- |
| HD_SOD 文件 | `D:\Heroes3\Heroes3_2026.10.09\HD_SOD.dll` |
| 当前文件版本 | FileVersion `0.2.0.92`；ProductVersion `5.0.0.4` |
| 当前 SHA256 | `37FA9723F39349558CA82F5379531311C266E8DFFFA382F80BB7550902039C75` |
| 完整背包构造 | `HD_SOD + 0x039A00`，六个栈参数，`ret 0x18`，callee-clean；不能按 cdecl 或普通四参 Hook |
| 背包 callback | `HD_SOD + 0x0395D0`；必须保留网络/超时/Esc/右键等原生通道 |
| 背包对象工厂 | `HD_SOD + 0x0FB180`，全屏事件容器；对象字段 `+0x70 callback`、`+0x78 hero`、`+0x7C result`、`+0x80 override` |
| 模态入口 | `HD_SOD + 0x039E7B` 调 `ShowAndRun(0)`；返回后 `+0x039EB8` 析构，不能保存并访问已析构 dialog |
| 原生背包数组 | `hero + 0x1D4`，64×8 字节；计数 `hero + 0x3D4` |
| 原生 item ID | 透明命中区 `1000+i`；宝物 DEF `2000+i` |
| 关键未决点 | `0x039E66` 是函数内部指令位置，不得未经指令级 ABI 验证就当作普通 HiHook 入口 |

所有 HD_SOD 地址在代码中必须按模块基址加 RVA 使用；不得把 preferred VA 直接当成任意版本绝对地址。

## 3. 阶段与执行顺序

### 阶段 A：版本门卫与 UI 生命周期骨架

目标：确认当前加载的 HD_SOD 是受支持版本，并建立“打开、持有、关闭、重开、析构前清理”的业务状态机；本阶段不移动宝物、不修改仓库库存。

任务：

1. 读取加载模块的 PE/版本信息，校验文件版本、ProductVersion、文件大小和 SHA256；校验失败只记录日志，完全不安装 UI Hook。
2. 对关键 HD_SOD RVA 做只读字节签名校验，至少覆盖背包构造尾部、callback 入口和模态调用附近；签名不匹配时停用 UI。
3. 设计独立 `WarehouseUiSession` 状态：`Inactive`、`Opening`、`Active`、`Closing`、`Invalidated`；只保存当前 dialog/hero 的观察身份，不在关闭后解引用游戏对象。
4. 先实现离线状态机测试：重复打开、同对象重入、不同对象替换、关闭清理、失效后拒绝更新、窗口重开不沿用旧指针。
5. 确认真正的指令级接入方式。若没有可证明的稳定入口，不安装生产 Hook，只保留版本门卫和离线状态机。

验收门槛：

- 离线测试覆盖全部状态迁移和旧对象拒绝。
- 版本/字节门卫不通过时无 UI Hook 注册。
- 主 DLL 与测试 EXE 0 warning/0 error。
- 用户实机只验证“背包打开/关闭不崩、无 UI 时原版行为不变”；未达到门槛不得进入阶段 B。

### 阶段 B：同一 dialog 的仓库可见外壳

目标：在原完整背包 dialog 的拥有对象列表内增加仓库背景、标题、当前英雄标识、分页区域和关闭清理；仍不接宝物转移。

任务：

1. 使用原 dialog 的 owned-items 生命周期，禁止创建嵌套 `ShowAndRun`。
2. 使用稳定的自有 ID 区间，避免与 `1000..2063` 原生 item ID 冲突。
3. 仓库控件只在完整背包模式显示；小分辨率按已核实尺寸重新布局，所有文字和控件必须在父容器内。
4. 保留原生 callback 的网络/超时/Esc/右键/左键关闭路径；任何未识别消息不得误关原生背包。
5. 关闭、Esc、外部 EndDialog、`result=-2` 重开和异常路径统一清理业务上下文。

验收门槛：

- 离线绘制/布局/ID/状态测试通过。
- 用户实机重复打开、关闭、Esc、右键、左右英雄切换、重开不崩且原 UI 不退化。
- 日志可区分版本门卫、打开、关闭、失效和防重复挂接。

### 阶段 C：背包宝物放入仓库事务

目标：完成用户要求的“先点背包宝物，再点仓库放入”。

事务规则：

1. 第一点只记录英雄身份、原生槽位、完整 8 字节宝物记录和本地修改 epoch，不写游戏全局 held。
2. 第二点先调用仓库状态的可失败准备逻辑；准备成功后再次验证源槽仍为同一完整记录。
3. 仓库提交成功后才删除原生背包槽并更新计数；任何失败都保留源物品。
4. Esc、关闭、超时、英雄切换和对象失效清空 pending selection。
5. 卷轴必须按 `(artifactId, subtype)` 保留；不能只按 artifactId。

验收门槛：空槽、满包、重复宝物、不同卷轴、失败回滚、重复点击和关闭取消全部通过；用户实机验证物品不丢、不重、不串英雄。

### 阶段 D：仓库取回与刷新

目标：从仓库取回至原生 64 格背包，并正确处理满包和刷新。

任务：

1. 原生背包满 64 格时拒绝取回且仓库不变。
2. 取回成功后再从仓库删除对应完整记录。
3. 刷新原生 8×8 icon、hit-test、tooltip、状态栏和仓库列表；必要时重建 owned item，但不访问旧对象。
4. 连续取回、重复宝物、卷轴 subtype、切换英雄和分页/滚动全部保持事务一致。

### 阶段 E：整体验收与文档收口

1. 完成 Release|Win32 Rebuild、独立测试和真实 patcher ABI 回归。
2. 游戏关闭后部署，核对 Release 与 Packs DLL SHA256。
3. 用户执行 UI 实机验收：放入、取回、满包、Esc、右键、重开、存档/读档、自动存档、换英雄。
4. 更新 H3Note UI 逆向笔记和本文件的实际结果；推测与实测分栏。
5. 不在 UI 未验收前声称“完整仓库功能通过”。

## 4. 日志与失败策略

- 所有 UI 日志继续使用 UTF-8 BOM 日志器。
- 路径和 HD_SOD 版本字段必须先转 UTF-8；不直接把 ANSI 字节作为 `%s` 写入。
- 门卫失败、对象失效、重复 Hook、事务失败均记录原因，但不得影响原生背包。
- UI 自有逻辑使用 CrashGuard；原版调用在 `__try` 外恰好一次；异常时只清理插件状态并放行原版默认行为。
- 任何无法证明对象仍属于当前活动 dialog 的指针，只能忘记，不能读字段、改 vtable、Hide、析构或重复释放。

## 5. 当前执行项

本次只执行阶段 A：版本门卫、关键字节校验、UI session 状态机及离线测试。阶段 B 及以后必须等阶段 A 验收完成后再进入。

## 6. 阶段 A 执行记录（2026-10-13）

已落地：

- 新增 `modules/WarehouseUi.hpp`：`WarehouseUi` 生命周期纯核心；`UiSessionStore_` 使用 token 拒绝旧窗口；保留 pending selection、Esc/外部关闭/失效语义；`HsodFingerprint_` 校验入口 ctor/callback/modal call/modal return 的关键字节。
- 新增 `modules/WarehouseUiStageA.inc.cpp`：只在主 DLL 启动时校验已加载 `HD_SOD.dll` 的版本、大小、SHA256 和内存字节；失败只记录日志，不安装任何 UI Hook；成功也只记录“生命周期核心就绪”。
- 新增 `tests/warehouse_ui_test.cpp`：状态机、token 复用、关闭原因、合成指纹匹配/单字节失配/越界拒绝的离线回归；不启动游戏、不依赖真实背包进程。
- 接入 `H3RndNew.vcxproj`、`tests/random_audit_test.vcxproj`、`random_audit_test.cpp`、`H3RndNew.cpp`、`modules/Entry.inc.cpp`。

验收边界：

- 阶段 A 未注册任何 UI Hook；`0x039E66` 指令级接入仍需阶段 B 给出可证明 ABI。
- 真实界面显示、点击、放入、取回、Esc/右键/重开仍由用户实机验收。
- 用户已有 `H3RndNew.rc` 本地修改继续保留，不得由本阶段提交。

指令级接入确认结论（阶段 A 任务 5，只做静态确认，不安装 Hook）：

- **后置 Hook 不可用**：`0x039A00` 构造函数的模态循环、`+0x7C` 结果读取和析构都在函数体内，在 `0x039EC0` 返回后的 HiHook 里 dialog 已在 `0x039EB8` 附近释放，此时才加控件或读结果都无效。
- **已验证接入**：`0x039E66` 的 LoHook 在 `ShowAndRun(0)` 前执行（`push 0; mov edx,4` 为 displaced 前缀，共 7 字节），读取当前函数栈 `[EBP-8]` dialog（`HookContext::Local(2)`）；`0x039E80` 的 LoHook 在 `ShowAndRun` 返回后、析构（`0x039EB8`）前执行，撤销 callback 包装并清理业务 session。`0x039E7B` 是 `FF D0` 间接调用，patcher `CALL_` 契约只支持 `E8/FF 15`，未安装 HiHook。
- **事件分发（已实现）**：回调函数指针在构造期（`0x039A44`）写入对象 `+0x70`；阶段 B 在 attach 时保存原指针并替换为 `__stdcall(this,msg)` 包装，自有 ID（30000+）先处理，未识别消息按原 ABI 转发原生回调，保留网络/超时/Esc/右键通道；detach 在 `0x039E80` 恢复原指针。
- 启动期依赖说明：`WarehouseUiStageA.inc.cpp` 通过 `GetModuleHandleW(L"HD_SOD.dll")` 取宿主模块；插件由 HD_SOD 加载，该模块在加载器表中可查。若实机日志出现模块未加载，说明加载期取句柄时机不成立，再改为首次背包打开时惰性复验，不得据此直接放宽指纹。

## 2026-10-13 第二轮改版（抄整个扩展界面 + 预留 Tab/分页行）

- 补齐"整个扩展界面"的其余原生层：右/底外框阴影（attach 时转发调用 `HD_SOD+0x6620`，与 0x039A00 构造同一函数、同一时机）；逆向确认该函数是 4 条 `0x44E2C0` 暗化条而非底图，完整背包还有底部 389×18 状态条（ID 84 @ (8,441)，存 dlg+0x74）。
- 面板高度 459 → **523**：标题条 (0,18) 不变，下方预留 **Tab 行 32px**（本版不画控件，网格原点相应下移至 (18,82)），BkPack.pcx 369×369 网格同构 8×8，网格底 (451) 之后是**分页行 32px**（y=459，`<`/`>` 与计数），底部留 32px 边距（状态条位预留 y=505）。
- 布局门槛：最小屏高 459→523（宽 834 不变）；800×600 仍可显示。
- 全量回归 73 flow checks PASS；部署 SHA256 `9CDA4581158C4BFA122562F1FD3A39276E75274EFF1906912935743033F495AF` 双侧一致。

## 2026-10-13 同构改版（用户实机反馈：面板过宽、无背景）

- 逆向确认完整背包视觉构成（见 H3Note 对应笔记第 5 节）：`BkPack.pcx` 369×369 @ (18,50)、标题条 405×30 @ (0,18) `bigfont.fnt`、8×8 网格步距 46（hit 47×47 / DEF 44×44 偏 +2,+1）。
- 改版内容：仓库面板固定 **405×459** 与完整背包同尺寸（不再撑满剩余屏宽）；背景 `BkPack.pcx`（加载失败退回纯色框）；标题条同构 `bigfont.fnt`；**64 槽/页**（8×8 同构，原 3×8=24）；底部条放计数与 `<`/`>` 翻页；锚点换算 = ID 1000 控件坐标 − (18,50)。
- 布局门槛变为最小屏宽 834（两个 405 面板 + 3 个 8px 边距）；800×600 等 fail closed（无仓库面板，仅原生 UI）。
- 全量回归 73 flow checks PASS（0 警告 0 错误）；部署 SHA256 `53B2F474E6AAB53807DFB34A2A595ECD27AA27D6E9DD52DA6F28579B4C59CA7C` 双侧一致。
- 卷轴槽当前显示 `artifact.def` 帧 1 通用卷轴图（不区分法术）；如需法术专属图（def.SpScrArt 帧=subtype）再迭代。

## 阶段 B/C/D 执行记录（2026-10-13）

实现（`modules/WarehouseUiStageB.inc.cpp`，约 400 行）：

- 纯模型 `modules/WarehouseUiFlow.hpp`：`Rect/MakeLayout`（405x459 背包、面板 ≥160x459、右优先左次之、8px 间隔、宽 <581 或高 <459 fail closed、3x8 固定分页）与 `Flow`（完整 `NativeBackpack` 快照 + hero/owner/epoch/token 五重身份校验；`Deposit/Retrieve` 先在 try 外准备目标态，校验全过后原子提交，失败零改动；`held` 非空或仓库锁定一律拒绝）。
- 生产 LoHook：`0x039E66`（模态前 attach）+ `0x039E80`（模态返回 detach），仅在阶段 A 六项指纹全过时安装；两钩各注册独立 CrashGuard id（Modal/ModalEnd/Callback），插件逻辑各自 `__try/__except`，原版路径始终 `EXEC_DEFAULT`。
- attach：从 `dlg->GetList()` 找原生第一格 hit item（ID 1000）实际坐标作布局锚点（Arg(1)/Arg(2) 语义未确证，仅作缺失回退）；`MakeLayout` 不满足时只记日志、保留原生 UI。自有控件 ID 30000..30123（frame/title/count/24 槽/翻页 hit），全部 `AddItem` 幂等；attach 阶段只写内容不 `Redraw`，模态运行中翻页才重绘。
- callback 包装：只拦 `ITEM_COMMAND + LBUTTON_DOWN` 且 ID 属于自有区间或原生背包 1000..1063；第一击背包槽仅记录 Flow 选择（不写 held 698AD8/ADC、不 EndDialog），第二击仓库槽提交 `Deposit/Retrieve`；成功后写回 `hero+0x1D4/0x3D4`（与 Flow 紧缩语义一致）、置 `dialog+0x7C = -2` 走原生"重建背包"重开路径，由游戏自己重建 icon/hit/tooltip/状态栏。
- 离线回归新增：`warehouse_ui_flow_test.cpp`（布局/分页/事务/身份/锁定/held/满包，69 checks）与 `warehouse_ui_abi.inc.cpp`（受控 0x1A 字节窗口复刻 + 真实 patcher LoHook：Local(2)/Arg(1..6)/寄存器与 EFLAGS 恢复/ESP 平衡/FF D0 show 恰一次/SEH 传播/callback ret 8）。

边界与遗留：

- 离线测试不加载 HD_SOD/游戏；`GetID()==1000` 锚点、控件可见性、`-2` 重开链路、HeldArtifact 交互、Esc/右键/外部关闭的实机行为全部留给用户游戏内验收。
- Arg(1)/Arg(2) 若实机证明是弹窗坐标，可改回直用；当前锚点方案优先原生事实。
- 用户本地 `H3RndNew.rc` 修改继续保留，不随本阶段提交。

当前状态：阶段 A–D 源码已完成并通过全量离线回归；阶段 E（部署与文档收口）执行中。游戏内验收全部留给用户。

执行结果：

- 构建入口：`build.bat`，日志 `D:\logs\dsh\scripts\h3rnd-audit-build\warehouse-ui-stage-a-final.log`；退出码 0，DLL/测试项目合计 0 warning/0 error。
- 全量回归（阶段 B/C/D 后，2026-10-13 02:31）：仓库 codec 3025、core 1017、store 28、native 30、UI flow 69、UI 状态机+6 指纹 57、UI LoHook ABI、生产 Hook/patcher ABI、outcomes 等全部 PASS，退出码 0。
- 阶段 E 部署（2026-10-13 02:4x，游戏未运行，仅 HD_Launcher 在）：`deploy.bat` 退出码 0；Release 与 `D:\Heroes3\Heroes3_2026.10.09\_HD3_Data\Packs\热血插件\H3RndNew.dll` SHA256 均为 `ED265209ACA7A197C9149FA96C1312197D5597410CA449910F251DB04C82D81D`。
- 逆向结论已记入 `H3Note\HD_SOD完整背包弹窗模态与LoHook接入逆向笔记.md` 并提交推送。
- 游戏内验收清单（用户执行）：打开完整背包确认仓库面板出现与位置不遮挡；第一击背包物品不再原地拿起而进入待放入状态；第二击仓库槽完成放入并自动重开；反向取回（含满 64 拒绝）；翻页 `<`/`>`；Esc/右键关闭后重开正常；存放/取回后存读档回环。
- `H3RndNew.rc` 保持用户本地未提交修改。
- 部署入口：`deploy.bat`，退出码 0；Release 与 `D:\Heroes3\Heroes3_2026.10.09\_HD3_Data\Packs\热血插件\H3RndNew.dll` SHA256 均为 `C6A8E047E4720CA64DD32DFF6C5E8BA3E5A1D9E33A684EF7ADFF3933A87C2FA8`。
- 部署内容只有阶段 A 门卫和生命周期核心；没有注册真实 UI Hook、没有新增控件，不能把本次部署称为仓库 UI 可用。下一阶段必须先完成 `0x039E66` 指令级接入 ABI 证明，再按阶段 B 实现同 dialog 可见外壳。
- `H3RndNew.rc` 保持用户本地未提交修改。
