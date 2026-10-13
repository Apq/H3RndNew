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

## 2026-10-13 第三轮改版（整块面板底板）

> 注：本轮"attach 即显示整块面板"的行为已被第四轮双层交互改版取代（仓库默认隐藏，点按钮才展开）；本节仅保留逆向与素材结论。

- 用户指出 HD 完整背包扩展自身有整窗背景，要求提取原背景并基于原图扩高。完整逆向确认构造函数 RVA `0x039B25` 调 HD_SOD RVA `0x0FADB0` 生成 H3DlgPcx16 整窗画布；资源为 `DiBoxBck.PCX` + `dialgbox.def`，`sb...` 只是运行时缓存 key，并非磁盘图。旧版通用 `DIBOXBACK/diboxback.pcx + dialogbox.def` 手工绘制逻辑错误且读取256×256 surface后按405×523调用绘制存在越界风险，已删除。
- 实现改为复用 HD 背景生成器，参数 `(x,y,405,523,30004,1,hero->owner)`，检查返回 item 的 canvas 非空并且尺寸精确为405×523后才加入弹窗；背景自带 HD 的玩家色与底部 statusBar=1 凹槽样式。该函数的原生资源拼图/边框坐标、帧选择详见 H3Note 第7节。
- 离线从主 Data LOD 提取素材并复现原生405×459及扩高405×523对照图（非游戏截图）：`D:\\logs\\dsh\\scripts\\h3-warehouse-skin\\background_comparison.png`。原框上下边角帧保持原生比例；仅背景 tile 裁剪铺设，不缩放整图。游戏内像素效果仍由用户验收。

## 2026-10-13 第二轮改版（抄整个扩展界面 + 预留 Tab/分页行）

- 补齐"整个扩展界面"的其余原生层：右/底外框阴影（attach 时转发调用 `HD_SOD+0x6620`，与 0x039A00 构造同一函数、同一时机）；逆向确认该函数是 4 条 `0x44E2C0` 暗化条而非底图，完整背包还有底部 389×18 状态文字项（ID 777 @ (8,433)，存 dlg+0x74）。
- 面板高度 459 → **523**：标题条 (0,18) 不变，下方预留 **Tab 行 32px**（本版不画控件，网格原点相应下移至 (18,82)），BkPack.pcx 369×369 网格同构 8×8，网格底 (451) 之后是**分页行 32px**（y=459，`<`/`>` 与计数），底部留 32px 边距（状态条位预留 y=505）。
- 布局门槛：最小屏高 459→523（宽 834 不变）；高度不足523 fail closed。
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

## 2026-10-13 第四轮改版（双层交互：先背包后仓库 + Esc 分流）

用户裁定（取代旧"同时弹出仓库"叙事，上方阶段 B 执行记录中的同步展开描述已废弃）：

1. 背包扩展 modal 打开时只显示"仓库"按钮（`H3DlgCaptionButton`，ID 30200，位于背包标题条区 (8,18) 64x30，GBK 字节串"仓库"）；仓库面板所有控件默认 `HideDeactivate` 隐藏。
2. 点"仓库"按钮才展开仓库层（同一 dialog 的 owned-items，不创建嵌套 ShowAndRun）：展开前先 `H3LoadedPcx16::Create` + `CopyRegion` 从 `H3WindowManager::GetDrawBuffer()` 保存面板+8px 阴影边距的 save-under；展开后画原生 `+0x6620` 外框阴影，两层均可点击（游戏内顶层模态即"置顶"，非 OS TOPMOST）。
3. 仓库层右上"关闭"按钮（ID 30060，同款 CaptionButton）只收起仓库层：隐藏全部面板控件 → save-under `DrawToPcx16` 回写 → `H3Redraw` 局部重绘 → 销毁 save-under；背包 modal 不结束。
4. **Esc 分流（用户 2026-10-13 补充裁定）**：仓库展开时 Esc 只收起仓库层（消息中和为 `MOUSE_OVER` 后原版 callback 仍执行一次，不进关闭分支）；仓库已关闭时 Esc 完全走原生关闭背包路径，不做任何拦截。
5. 仓库展开时面板外左键（`0x200/C`、`0x200/D` 且非原生 1000..1063/2000..2063 的 ID）同样中和，不结束背包；右键 `0xE`、`0x200/A`、网络、超时、未知命令全部原样透传。纯函数 `SuppressPinnedClose` 已入离线回归（`warehouse_ui_flow_test.cpp`，77 checks）。
6. callback 包装保留原指针调用恰好一次（CrashGuard `__try` 外）；包装失败或 dialog 不匹配时直接透传原生回调。展开态点击宝物 DEF 图标（2000..2063）也进入 Flow 选择（右键仍归原生信息路径）。

构建/部署（2026-10-13 13:09）：全量回归 PASS（UI flow 77 checks 含置顶过滤契约）；Release 与 Packs 部署 SHA256 均为 `C29B7182D0A6A7D0365F07F5EA3D1D2C7B598658EE8F6C026E129533E0097717`。游戏内双层显示、按钮点击、Esc 分流、save-under 恢复效果全部留待用户实机验收。

## 2026-10-13 第五轮：中文标题、四态按钮、批量转移与分类页签

- 标题固定为 **仓库**，不拼英雄名。显示的是当前英雄 `hero.id` 对应的独立库存，不共享其他英雄道具。
- 仓库、关闭、一键取出、一键放入：各 66×32，采用 H3MegaDesc 的 DEF命中/按压 + PCX视觉模式，复制15个vtable槽，仅替换Draw槽4；普通/按下/悬停高亮/禁用各一张背景及自生成中文文字。按下取原生 `+0x16 & 1`，禁用取 `&0x20`，悬停由 command4绝对坐标维护自有bool，不占用原生SELECTED位。
- 一键取出在面板 `(8,50)`，一键放入在 `(331,50)`；二者均按**当前活动分类**操作。取出按库存顺序移动到背包空位，最多填到64格，剩余保留；放入只移动背包中匹配项（不含已穿戴装备），保持剩余背包和转移项顺序。无匹配、满包取出、locked/held时显示禁用态。
- 分类顺序（round-6）：常规、卷轴、装备、头部、肩部、项链、武器、盾牌、胸甲、戒指、靴子、杂物，共12项。机械与魔法书不进入背包，因此不列页签；一次显示4个44×22标题，左右18×22箭头滚动标题窗口；箭头边界钳制。切换分类回到第一页并清除待放入选择。
- 本轮实现约定：**常规=全部道具的总入口**，其他分类是子集；这不是对“常规”语义的额外用户裁定，若需要排除装备可再调整。卷轴优先按artifactId=1识别，装备包含非负穿戴位置且排除卷轴。左右戒指合并，5个杂物槽合并，机械合并；id=0魔法书也查询GetSlot。
- 可见列表保存真实库存索引，单件取回与分页使用筛选索引映射，避免筛选后取错物品。增加64个空格透明命中区域，便于空仓库放入。全部owned面板控件统一展开/隐藏，不新增modal或OS置顶。
- `DepositAll/RetrieveAll` 共用Flow的hero/owner/token/epoch/完整原生快照/locked/held校验；分配和候选整理在提交前，失败无变更，成功只推进一次epoch。完整8字节宝物pair（含卷轴subtype）不拆开。
- `DepositAll/RetrieveAll` 共用Flow的hero/owner/token/epoch/完整原生快照/locked/held校验；分配和候选整理在提交前，失败无变更，成功只推进一次epoch。完整8字节宝物pair（含卷轴subtype）不拆开。
- **Round-6 废止“成功后自动关闭重建”**（用户裁定：仓库打开时无论怎样移动道具都不应关闭仓库）。转移成功后改为就地刷新：`WarehouseUiRefreshAfterMove_` 重算筛选/翻页/图标/计数并按需局部重绘，`dlg+0x7C` 与 `CloseDialog` 都不再参与成功路径；只有关闭X（ID 30060）与 Esc 收起仓库层。旧的“先中和消息、原生返回后写 `-2` 并返回 2”顺序（`CallNativeThenFinalize`）已从生产代码删除。
- 44张素材由 `scripts/make_whui_assets.py` 从 `whui_src` 的 H3MegaDesc 原始四态参考（`bv_ok0..3.pcx`）生成；16按钮（4族×4态）、24页签（12×2态）、4箭头，共44个PCX，预览 `whui/preview.png`。按钮只保留参考图的立体斜面/边框纹理，检查勾被抹除后写中文文字。标题不再使用素材，改为原生 `H3DlgText`。
- 运行时用生产共用 `WarehouseUiPcx.hpp::DecodePcxRgb` 解码24bit三平面RLE，逐plane处理padding，拒绝坏头/越界/截断且失败不改输出；量化调色板取BkPack.pcx，透明洋红映射索引0。路径只用4MB堆缓冲。44张全部成功才启用新视觉，失败回退中文原生按钮/标题。
- **资源生命周期**：绘制前检查当前dialog owned-list；detach先恢复owned DEF原vtable及自有PCX创建时原资源指针，再清空对象引用，原生析构负责原资源引用；cached图片不被modal析构反复减引用。静态缓存只存颜色原始字节，避免静态构造游戏palette对象。visual guard与既有15项合计16项，不超CrashGuard注册容量。
- 第四轮原生右键、网络、超时、0x200/A、未知命令透传及展开/收起Esc分流保持。自有按钮/页签按下只中和原生callback未知ID关闭路径，动作在释放CLICK执行一次。

- 【第五轮结论，round-6 已废止】第五轮曾在成功转移后于原生返回之后写 `-2` 并 `CloseDialog`+返回 `2`（证据：`CloseDialog` 跳 `0x00491640` 写 `0x200/0xA`，modal loop 只在返回 2 且消息为 `0x200/0xA` 时退出）。round-6 用户裁定“仓库打开后移动道具不得关闭仓库”，该路径连同 `CallNativeThenFinalize` 一并删除；现在只中和被消费消息，不让原生 callback 进入未知ID关闭分支。
- 布局只规划不平移：`layout.backpack` 必须与原生锚点实际原点一致，否则拒绝 attach 保留原生背包，避免两套命中控件重叠。
- 两只 modal LoHook 安装失败必须回滚并禁止 attach；仅装一半会留下永不 detach 的 vtable/PCX 裸指针残留。

### 第五轮验证记录（历史，round-6 已取代）

2026-10-13 18:08：`build.bat` + `H3RND_RUN_AUDIT_TESTS=1` + 游戏真实 `patcher_x86.dll`；主 DLL 与测试工程均 0 warning/0 error，全回归 PASS：codec 3025、core 1017、store 28、native 30、**UI flow 170**（新增回调关闭顺序 6 项与布局 fail-closed 3 项）、PCX 286、UI 状态机/11 指纹 77、LoHook ABI、真实 patcher callback/RNG 并发。回调顺序由生产共用 `modules/WarehouseUiCallback.hpp::CallNativeThenFinalize` 保证并离线回归。部署（游戏未运行，仅 HD_Launcher）Release 与 Packs DLL SHA256 均为 `C8BFD5AF792DBB2735AAECA1570B816FC8D3561C0697491EF41EDAAD4B3F5DF9`，49 张 whui PCX 随包部署，DLL 版本为用户本地 `0.6.2026.1012`。第四轮的中和语义、Esc/右键/网络/超时透传不变。

未运行游戏：四态按钮实际观感、分类切换与批量后的 `-2` 重开、标题透明与 save-under、存档读档回环、以及本次关闭顺序修复的实机效果均由用户验收。`H3RndNew.rc` 为用户本地版本升级修改（0.5→0.6.2026.1012），保持不提交。

## 2026-10-13 第六轮：原生质感素材、12页签、就地刷新与“移动不关闭”

用户四项裁定驱动本轮：

1. **按钮素材沿用 H3MegaDesc 原始四态质感**：`scripts/make_whui_assets.py` 直接以 `whui_src/bv_ok0..3.pcx` 为参考，抹掉勾选图形、保留其立体斜面/边框纹理，再写中文文字。素材总数由 49 收敛为 **44**（16按钮 + 24页签 + 4箭头），标题不再用素材。独立回归 `scripts/test_whui_assets.py` 七项全过：精确44文件清单、RGB/尺寸、独立PCX RLE解码对拍、四态字节互异、preview逐像素对应、生成器分类、以及 PowerShell BOM/CRLF 与 C++ 消费数量。
2. **页签由14项减为12项**：机械与魔法书不会进入背包，也就不会进入仓库，故不再列出（`kTabCount=12`）。槽位映射严格按 H3 原生 `NSlots::eArtifactSlots`：MISC1..MISC4 = 9..12、MISC5 = 18 归杂物；BALLISTA/AMMO_CART/FIRST_AID_TENT/CATAPULT = 13..16 与 SPELLBOOK = 17 被 `IsBackpackSlot` 拒绝，任何页签都不匹配。
3. **两个一键按钮左右交换**：左上 `(8,50)` 为“一键放入”（ID 30202），右上为“一键取出”（ID 30201），语义与素材族一一对应。
4. **移动道具绝不关闭仓库**（用户原话：仓库打开时不管如何移动道具，都不应该关闭仓库，只有手动点关闭按钮或ESC键才关闭仓库页面）。成功转移只调 `WarehouseUiRefreshAfterMove_` 就地刷新；`dlg+0x7C=-2`、`CloseDialog`、返回 2 的旧链路和 `CallNativeThenFinalize` 一并删除，原生 callback 仍在 `__try` 外恰好执行一次。

本轮运行时接线要点：

- **图标层重做**：每格创建两个同 ID（`2000+i`）的覆盖 DEF——`artifact.def` 与 HD `def.SpScrArt`，按当前记录只激活其一（`MakeIconView` 纯函数选择资源族，卷轴走 `def.SpScrArt` 帧=subtype，受 HD 全局 `HD_SOD+0x157FB8` 判别，已在 `.data` 段映射内核实）。原生原件自接管起永久 `HideDeactivate`：`SetFrame` 不能换 DEF 资源，且收起后重新启用原件会显示构造时的旧帧。
- **关闭控件**：右上角零内缩贴角的 24×24 小四态 X（ID 30060）。“仓库”按钮（ID 30200）改为幂等展开，不再切换收起；收起只有 X 与 Esc 两条路径。
- **点击边沿策略**：仓库控件只中和 `LBUTTON_DOWN(0xC)`，动作只在 `LBUTTON_CLICK(0xD)` 执行一次；`IsWarehouseControlId` 覆盖翻页箭头 `30050/30051`、`30060..30202`、页签 `30300..30311`，不含背景 `30000..30004`。展开态下宝物图标 `2000..2063` 的 DOWN 与 CLICK 都由本模块接管，否则会落进原生 `0x039987` 未知回退关闭分支。
- **标题**：原生 `H3DlgText` 参数复用（`405×30`、bigfont、`eTextColor::REGULAR`、居中），与原生背包标题同字号同亮度。

### 第六轮验证边界

2026-10-13 23:20（冻结源码后的最终全量）：`build.bat` + `H3RND_RUN_AUDIT_TESTS=1` + 游戏真实 `patcher_x86.dll`；主 DLL 与测试工程均 0 警告 0 错误，15 项审计全部 PASS（codec 3025、core 1017、store 28、native 30、**UI flow 171**、PCX 44 素材/266、UI 状态机/11 指纹 77、LoHook ABI、真实 patcher callback/RNG 并发）。`deploy.bat` 退出码 0，Release 与 Packs DLL SHA256 均为 `4E53C0F99464B29DC418123192EC776E330F2D534C1FCDB62F5AF78A310C96BA`（159744 字节），部署目录 whui PCX 恰好 44 张且 `title.pcx/tab12n/tab12s/tab13n/tab13s` 已清除，DLL 版本为用户本地 `0.6.2026.1012`。素材独立回归 `scripts/test_whui_assets.py` 七项全过。前两次构建失败均为构建窗口内源码被并发修改产生的混合快照（4/170 与 1/171 假失败），冻结源码后一次通过——教训已沉淀进技能。

未运行游戏：四态按钮与 H3MegaDesc 原图的实际观感、中文标题亮度、极端右上小 X 的命中范围、12 页签滚动与两个一键按钮的左右语义、就地刷新后 tooltip/状态栏一致性、以及“移动道具后仓库保持打开”均由用户实机验收。`H3RndNew.rc` 为用户本地版本修改（0.6.2026.1012），保持不提交。

## 2026-10-13 第七轮：图标错位修复、布局入配置、数字分页栏与全量批量

用户七项裁定驱动本轮：

1. **背包网格与物品错位（回归修复）**：根因是第六轮背包覆盖图标误用仓库网格原点 `kGridOriginDY=82`（含 Tab 行 +32 偏移），而原生背包图标在 +50——物品整体比网格低 32px，观感即“背包初始位置上移了、物品没跟上”。修复：attach 时先收齐原生命中项 1000..1063（每格真实坐标），覆盖图标一律取 `hit->GetX()+2 / hit->GetY()+1`，命中项缺失才回退常量推导且改用新常量 `kBackpackGridOriginDY=50`（不再共用仓库原点）。
2. **布局偏移入配置文件**：新增 `[WarehouseUI]` 节（default 出厂 + user 两层覆盖，`IniReadIntUtf8Layered`），七个键：`OpenButtonDX/DY`（仓库按钮距背包左上角，默认 8/18）、`CloseButtonDX/DY`（关闭X距仓库右上角，默认 0/0）、`BulkButtonLeftDX/RightDX`（一键放入距左/一键取出距右，默认 8/8）、`BulkButtonDY`（两键距仓库顶部，默认 50）。每次弹窗打开时重读，改 user.ini 下次开窗即生效；超范围回退默认值。
3. **分页栏纯数字+两侧箭头**（用户更正：不用中文）：两侧箭头按钮（复用 arrow 素材、独立实例 ID 30050/30051），中间 4 个页码按钮（ID 30400..30403，36x22 两态底图 pager_n/pager_s + 运行时 H3DlgText 叠数字、文本 id=0 不参与命中）。页码窗口用 `MakeTabWindow(page,pages,4)` 滑动，当前页高亮 s 态，不足 4 页的按钮整体隐藏；点页码直跳该页。素材 44→**46** 张，生成/清单/审计五处同步。
4. **一键功能固定全量语义**：一键放入/取出不再随页签过滤——放入掩码=“常规”页语义（可进背包的全部道具），取出=仓库内全部记录参与；后续页签仅分类浏览（单件移出仍支持）。取出禁用条件改为“仓库全空或背包满”。
5. **放入落点无关**：选中背包道具后，点仓库面板内任意位置（64 格、底板 30004、边框 30003、背景 30000、标题 30001）都提交放入，自动追加到仓库列表尾部（仓库本就是列表，天然无需精确空位）。
6. **事务性保持**：单件与批量仍走 Flow 快照 + hero/owner/epoch/token 五重校验 + 一次原子写回；成功后仅就地刷新，不关窗。
7. **未完成=取消**：选择背包道具只在本模块内存记录（不写原生 held/背包数组），切页签/收起/Esc/关闭均清 flowActive，道具从未离开原位，天然“回原位”。

### 第七轮验证边界
2026-10-13 23:57（冻结源码后的最终全量）：`build.bat` + `H3RND_RUN_AUDIT_TESTS=1` + 游戏真实 `patcher_x86.dll`；两工程 0 警告 0 错误，15 项审计全 PASS（**UI flow 172**、**PCX 46 素材/274**、codec 3025、core 1017、store 28、native 30、状态机/指纹 77、LoHook ABI、真实 patcher RNG 等）。`deploy.bat` 退出码 0，Release 与 Packs DLL SHA256 均为 `7C96B52C62464A3606DAA9DF09A70D1B2A6CAC26EEFEBB4895B60E7A5DAE46AD`（162304 字节），whui 恰好 46 张（pager_n/s 在位、5 个旧素材不存在），部署 default.ini 含 [WarehouseUI] 七键，DLL 版本为用户本地 `0.6.2026.1012`。素材独立回归 7 项全过（46 清单）。首轮构建曾因新增静态变量“先用后声明”（C2065）失败一次，修正声明顺序后一次通过。

未运行游戏：图标与网格的像素对齐、配置键实际生效（改 user.ini 后重开弹窗）、页码按钮滑动窗口与当前页高亮观感、全量一键在非“常规”页签下的行为、面板空白处放入的命中范围，均由用户实机验收。`H3RndNew.rc` 保持不提交。