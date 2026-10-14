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
2. **布局偏移入配置文件**：新增 `[WarehouseUI]` 节（default 出厂 + user 两层覆盖，`IniReadIntUtf8Layered`），七个键：`OpenButtonDX/DY`（仓库按钮距背包左上角，默认 8/18）、`CloseButtonDX/DY`（关闭X距仓库右上角，默认 0/0）、`BulkButtonLeftDX/RightDX`（一键放入距左/一键取出距右，默认 8/8）、`BulkButtonDY`（两键距仓库顶部，默认 50）。每次弹窗打开时重读，改 user.ini 下次开窗即生效；超范围回退默认值。（**round-8 更正**：越界不再回退默认值，改为夹到最近合法边界；`BulkButtonDY` 区间放宽为 0..440——旧实现把 `<30` 当成非法并静默换成默认 50，正是玩家配置“不生效”的根因。）
3. **分页栏纯数字+两侧箭头**（用户更正：不用中文）：两侧箭头按钮（复用 arrow 素材、独立实例 ID 30050/30051），中间 4 个页码按钮（ID 30400..30403，36x22 两态底图 pager_n/pager_s + 运行时 H3DlgText 叠数字、文本 id=0 不参与命中）。页码窗口用 `MakeTabWindow(page,pages,4)` 滑动，当前页高亮 s 态，不足 4 页的按钮整体隐藏；点页码直跳该页。素材 44→**46** 张，生成/清单/审计五处同步。
4. **一键功能固定全量语义**：一键放入/取出不再随页签过滤——放入掩码=“常规”页语义（可进背包的全部道具），取出=仓库内全部记录参与；后续页签仅分类浏览（单件移出仍支持）。取出禁用条件改为“仓库全空或背包满”。
5. **放入落点无关**：选中背包道具后，点仓库面板内任意位置（64 格、底板 30004、边框 30003、背景 30000、标题 30001）都提交放入，自动追加到仓库列表尾部（仓库本就是列表，天然无需精确空位）。
6. **事务性保持**：单件与批量仍走 Flow 快照 + hero/owner/epoch/token 五重校验 + 一次原子写回；成功后仅就地刷新，不关窗。
7. **未完成=取消**：选择背包道具只在本模块内存记录（不写原生 held/背包数组），切页签/收起/Esc/关闭均清 flowActive，道具从未离开原位，天然“回原位”。（**round-8 已取代**：背包侧改为单击即事务性入仓，不再存在“已选中”中间态；事务失败即不动数据，等价于取消。）

### 第七轮验证边界
2026-10-13 23:57（冻结源码后的最终全量）：`build.bat` + `H3RND_RUN_AUDIT_TESTS=1` + 游戏真实 `patcher_x86.dll`；两工程 0 警告 0 错误，15 项审计全 PASS（**UI flow 172**、**PCX 46 素材/274**、codec 3025、core 1017、store 28、native 30、状态机/指纹 77、LoHook ABI、真实 patcher RNG 等）。`deploy.bat` 退出码 0，Release 与 Packs DLL SHA256 均为 `7C96B52C62464A3606DAA9DF09A70D1B2A6CAC26EEFEBB4895B60E7A5DAE46AD`（162304 字节），whui 恰好 46 张（pager_n/s 在位、5 个旧素材不存在），部署 default.ini 含 [WarehouseUI] 七键，DLL 版本为用户本地 `0.6.2026.1012`。素材独立回归 7 项全过（46 清单）。首轮构建曾因新增静态变量“先用后声明”（C2065）失败一次，修正声明顺序后一次通过。

未运行游戏：图标与网格的像素对齐、配置键实际生效（改 user.ini 后重开弹窗）、页码按钮滑动窗口与当前页高亮观感、全量一键在非“常规”页签下的行为、面板空白处放入的命中范围，均由用户实机验收。`H3RndNew.rc` 保持不提交。

## 2026-10-14 第八轮：分类错位定性、配置项失效、单击即入仓、整理与同类相邻

用户六项裁定驱动本轮（前四项为实机缺陷报告）：

1. **分类页签整体错位一位（头部空、肩部显示头部…）**：根因是**原生宝物表 `position` 在运行期是 1-based（0=无装备位）**，而 `SlotTab` 分类表按 H3API/NH3API 的 0-based `eArtifactSlots` 语义编写，于是每个分类都向后错一位：头部（raw 1）落到空位、肩部（raw 2）显示头部、胸甲（raw 8 → 0-based 7 戒指位）落进戒指类。修复：新增纯函数 `NormalizeArtifactTableSlot(raw)`（`raw<=0 → kSlotNone`，否则 `raw-1`），在 `WarehouseUiArtifactSlot_(id)` 内统一归一化后再分类。
2. **`BulkButtonDY` 等布局配置项不生效**：根因是取值函数把“越界值静默替换为默认值”——旧实现 `ClampOffset(value, 30, 440, fallback)` 认为任何 `<30` 的值非法，于是玩家在 user.ini 写的 `BulkButtonDY=18` 被悄悄换成默认 50（玩家日志 `布局配置 open=18,18 close=6,6 bulk=18,18,50` 正是该症状）。修复：改为真正的钳制（越界夹到最近边界，不再回退默认），`BulkButtonDY` 合法区间放宽为 `0..440`；同时新增原始读数日志（`布局配置原始读数 …`）与最终生效值日志，便于下次一眼分辨“玩家值非法”与“读取失败”。
3. **背包侧单击即入仓（两步交互取消）**：仓库展开时，左键点背包里有物品的格子（或覆盖图标）**立即**执行单件事务放入，不再要求点仓库面板落点；空槽只消费左键、不产生事务。实现上不再依赖消息里的 `itemId` 可靠：先按 attach 时记录的原生命中项几何命中，再用 `GridSlotFromPoint`（原点 `背包+(18,50)`、步距 46、8×8）做坐标兜底，因此“点装备就进仓库”与 id 无关。
4. **点空格不再关窗**：根因是展开态下原生命中区 `1000..1063` 的**左键按下（0x200/0xC）被透传**，原版按下状态机在空槽上走到了上层关闭分支；round-7 只吞 `0xD`，所以 `0xC` 仍能关窗。修复：展开期间左键手势两态（`0xC/0xD`）一律中和，且不再按 id 放行；`END_DIALOG(0xA)`、右键 `0xE`、Esc、滚轮 `0x20A`、网络/超时命令继续透传（保持“绝不屏蔽 EndDialog”的既有硬约束）。
5. **分类页展示时需要排序（相同道具连续）**：新增纯函数 `SortRecordIndicesByIdentity`，以 `(宝物ID, 子类型)` 为同一性键对**展示视图**做稳定排序——同组内保持原仓库顺序，只改展示/取回顺序，不动存储顺序。
6. **分页条左侧新增“整理”按钮（仅常规页，做存储层排序）**：分页条最左预留一个按钮位（36×22，ID 30052，`tidy_n/tidy_s` 两态素材，中文“整理”按既有约定烘焙进 PCX——游戏字体不保证中文字形），仅在“常规”页签且展开态显示，切走页签立即 `HideDeactivate`。点击执行 `SortRecordsByIdentity`：直接重排仓库记录本体（空记录排最后、同组稳定、幂等），epoch 递增使在途 Flow 失效，随后就地刷新；这是**存储层**真实重排，下次存档即持久。素材 46→**48** 张，生成器/独立回归/部署/打包/PCX 审计同步。

### 第八轮验证边界
2026-10-14 02:08（源码冻结后的最终全量）：`build.bat` + `H3RND_RUN_AUDIT_TESTS=1` + 游戏真实 `patcher_x86.dll`；两工程 **0 警告 0 错误**（13 项审计全 PASS：**UI flow 180**、**PCX 48 素材/282**、codec 3025、core 1017、store 28、native 30、状态机/指纹 77、hooks、LoHook ABI、真实 patcher RNG 分类审计与并发对账等）。`deploy.bat` 退出码 0，Release 与 Packs DLL SHA256 均为 `68742C34C656B1A8ADB54884891DE559CAF96CBEECFEDE33A88583019B14B860`（182784 字节），whui 恰好 48 张（`tidy_n/tidy_s` 在位），部署 default.ini 保留用户本地实测值（`OpenButtonDX=18 / CloseButtonDX=6 / BulkButtonDY=18`），`H3RndNew.rc` 与 `H3RndNew.default.ini` 哈希构建前后一致（脚本未覆盖用户手改文件）。素材独立回归 8 项全过（48 清单 + 两态同字形/两字笔画/墨色差分断言）。

**本轮两次真实失败与其根因（记录以免重犯）**：
1. 首轮构建 2 个 `error C3861`：`WarehouseUiButtons.inc.cpp(249)` 调用本轮新增的 `WarehouseUiUpdateTidyHover_`、`WarehouseUiStageB.inc.cpp(229)` 由新增的 `WarehouseUiOrganizeStorage_` 调用 `WarehouseUiRefresh_`——两处都是**同翻译单元内 static 函数“先用后定义”**（第三轮同类问题的复发）。修法：各补一条前置声明（StageB 并入既有声明块）。
2. 第二轮 UI flow `1/180`：断言的期望顺序写错（同一宝物 ID 下子类型应升序，(1,3) 在 (1,9) 前），实现本身正确——**测试期望错误也会让审计红**，改断言而非改实现。

未运行游戏：四类页签归类（头部/肩部/胸甲/戒指/杂物）是否与实机一致、`BulkButtonDY` 等键改 user.ini 后重开弹窗是否立即生效、单击背包装备是否即时入仓且不关窗、点空格是否不再关窗、“整理”按钮在各页签的显隐与排序观感、整理后存档再读是否保持新顺序，均由用户实机验收；`H3RndNew.rc` 保持不提交。

## 2026-10-14 第八轮补丁（8.1）：点“仓库”按钮无反应

用户实机报告：收起状态下点背包里的“仓库”按钮没反应。根因是第八轮新增的**几何兜底分支排在已知控件分派之前**：打开按钮（ID 30200）位于背包面板矩形内部，其点击先落进“背包区域内”判定，被当成区域空点击消费掉（`WarehouseUiDepositSlot_(-1)` 后 `return true`），永远到不了后面的打开分支。修法：新增纯函数 `UsesGeometryFallback(id) = !IsWarehouseControlId(id)`，几何兜底只兜“背包区域内但无已知控件”的点击（空格、底板），已知仓库控件（打开/整理/关闭/页签/分页/槽位）一律优先走各自 ID 分派；离线回归补一条不变量断言（含 30200/30060/30052/30400/30100 让位于几何、1005/2005/0/-1 走几何）。

### 8.1 验证边界
2026-10-14 08:2x（源码冻结）：`build.bat` + `H3RND_RUN_AUDIT_TESTS=1` + 游戏真实 `patcher_x86.dll`，两工程 0 警告 0 错误，13 项审计全 PASS（**UI flow 181**、PCX 48/282、codec 3025、core 1017、store 28、native 30、状态机/指纹 77、hooks、LoHook ABI、真实 patcher RNG 等）；`deploy.bat` 退出码 0，Release 与 Packs SHA256 均为 `B000A86221A946AAAB0CEE9ECBE837CA890B2CA2A3EE031E26BEDAA6B5E6AFEA`（182784 字节），whui 恰好 48 张，`H3RndNew.rc` 与 `H3RndNew.default.ini` 哈希前后一致。打开按钮的实际响应仍由用户实机验收。

## 2026-10-14 第九轮：页签行改版（两端箭头 + 居中窄页签 + 滚轮）

用户裁定三条，全部落地：
1. **滚动箭头移到页签行左右两端**：左箭头 `面板左+8`、右箭头 `面板右-26`（与分页条同边距），不再用旧的 `+80/+311` 内缩位置；四个可见页签在两箭头之间居中（`CenteredTabsX(405,36,4,4)=124`）。
2. **页签收窄**：标题均为两字，44→**36**（与页码/整理按钮同宽、间距 4、步距 40），24 张页签素材由生成器重出（尺寸/独立解码/两态差异/预览全部随动）。
3. **页签行支持滚轮**：仓库展开且光标位于页签行（`面板左+8` 到 `面板右-8`、Y=`TabY..TabY+22`）时，滚轮滚动页签窗口。方向按 H3 消息层约定：`subtype` 承载 WM_MOUSEWHEEL 增量，**>0 上滚=窗口左移、<0 下滚=窗口右移**（`TabWheelShift`；H3Auto PanelInput 同源实证，原生 `OnMouseWheel(msg.subtype)` 同通道）。**只在实际滚动时消费**；行外、未展开、已到边界一律透传，不影响原生滚轮路径与其他区域。

### 第九轮验证边界
2026-10-14 08:4x（源码冻结 114 文件）：`build.bat` + `H3RND_RUN_AUDIT_TESTS=1` + 游戏真实 `patcher_x86.dll`，两工程 0 警告 0 错误，13 项审计全 PASS（**UI flow 183**（新增居中 X 与滚轮方向断言）、PCX 48/282）；素材独立回归 8/8（48 清单 + 页签 36x22）。`deploy.bat` 退出码 0，Release 与 Packs SHA256 均为 `7892AD7C93FAAA1BA042A1B5A04D52AA970ED6D049370F7D0DBB4C0764A1F5F0`（183296 字节），whui 48 张**逐张哈希对拍一致**（含 24 张新页签）。页签行两端箭头位置、36 宽观感、滚轮手感与“到边界透传”行为由用户实机验收；`H3RndNew.rc` 与 `H3RndNew.default.ini` 未提交、构建前后哈希一致。

### 9.1 调整（用户裁定）：箭头距边 18px、中间能放多少页签放多少
初版两端箭头距边 8px、仅 4 个可见页签居中，行内留白过大。调整：箭头距边 **18px**（右箭头 X=`宽-18-18`）；两箭头之间内区 = `405-18*2-18*2 = 333px`，按“能显示多少就显示多少”用新纯函数 `FittingTabCount(333,36,4)=8` 放 **8 个**页签（316px，余 17px 居中：左 8 右 9）。窗口机制随之从固定 4 槽改为 `kWh_uiTabSlotCount_=8`（`MakeTabWindow/TabOffsetShift` 全部改传该常量，命中 ID 扩为 30300..30307，仍在 `IsWarehouseControlId` 的 30300..30311 区间内），偏移范围 0..4；滚轮判定行的边距同步 8→18。素材不变（仍 48 张、36 宽页签）。

## 2026-10-14 第十轮：分类槽位分段编码定案（推翻第八轮“均匀 1-based”高段）

用户实机（均匀 `raw-1` 部署后）：戒指页=戒指+靴子、靴子页全是杂物、杂物页空。与第八轮修前症状（只有“从胸甲往头部方向”错位、靴子/杂物原本正确）联立，唯一自洽解：运行时 `position` 为**分段**编码——raw 0=无位、raw 1..6=头..胸甲(+1)、**raw 7=戒指族单值**、**raw 8 起与 0-based 规范一致**（8=靴子、9..12=杂物1..4、17=魔法书、18=杂物5）。第八轮把 `-1` 均匀套到全部值，把本就正确的高段也错位一格（靴→戒指页、杂物1→靴子页、杂物5→魔法书位被拒→杂物页空）。修法：`NormalizeArtifactTableSlot` 改为分段（`<=6 → -1`、`==7 → 6`、`>=8 → 原值`），断言更新为逐值走查（1/6/7/8/9/10/12/18/19/23 与 Head/Shoulders/Neck/Torso/Ring/Feet/Misc 落位）。详见 H3Note §17（含静态取证失败原因：表在 .data 零初始化区，运行时构建）。

### 第十轮验证边界
2026-10-14 09:0x（源码冻结 35 个 C++ 文件）：`build.bat` + 审计全绿（0 警告 0 错误、13 项 PASS、UI flow 185、PCX 48/282）；`deploy.bat` 退出码 0，Release 与 Packs SHA256 均为 `0F262A787F750F9F52AA724991AAB97F0C5CCA2FFFC596E8CB04FBFB9017F156`。raw 8=靴子、raw 9=杂物族由实机锁定；raw 10..18 按连续性推定，各分类页的最终归属由用户实机验收；`H3RndNew.rc`/`H3RndNew.default.ini` 未提交、哈希前后一致。

## 2026-10-14 第十一轮：页签顺序进配置（TabOrder）

用户裁定：页签顺序不再固定在代码里，用 `[WarehouseUI]` **`TabOrder`** 配置（直接写中文页签名、逗号分隔），默认=第十轮后的顺序：`常规,卷轴,杂物,靴子,戒指,胸甲,盾牌,武器,项链,肩部,头部,装备`（常规、卷轴，随后“杂物→头部”倒序，装备最后）。实现：

- **素材与显示顺序解耦**：24 张页签素材固定按 `TabKind` 枚举规范序出图（`tab{枚举值}.pcx`：常规,卷轴,装备,头部,肩部,项链,武器,盾牌,胸甲,戒指,靴子,杂物），显示槽 i 展示 `顺序数组[i]` 对应页签的素材；换序只改配置，不必重出素材。
- **纯解析器 `ParseTabOrder`**：逗号分隔、容忍分隔符两侧空白；要求恰好 12 个互不相同且合法的页签名，任何缺失/重复/未知名/多余项整体判无效，回退默认并留日志（原始读数与生效顺序各一条）。两层 ini 叠加（user 层可覆盖 default 层），attach 时与布局配置同轮重读，改完 ini 重开弹窗即生效。
- `kTabOrder` 常量删除，改为 `kDefaultTabOrder`/`kDefaultTabOrderText` + 运行时数组 `g_wh_ui_tab_order_`（ResetTabs_ 复位默认，attach 重读）。
- **默认键落在随包分发的 `H3RndNew.default.ini`**（`[WarehouseUI] TabOrder=常规,卷轴,杂物,靴子,戒指,胸甲,盾牌,武器,项链,肩部,头部,装备`），玩家改序请写 `H3RndNew.user.ini` 同名键。为避免“ini 里改了没反应”的隐性故障，离线回归新增一致性检查（`test_whui_assets.py::test_tab_order_config_matches_code_default`）：default.ini 的 TabOrder 必须与 `kDefaultTabOrderText` **逐字一致**，且恰好是 12 个互不重复、与素材生成器 TABS 同集合的页签名。

### 第十一轮验证边界
2026-10-14 09:2x（源码冻结 35 个 C++ 文件）：`build.bat` + 审计全绿（0 警告 0 错误、13 项 PASS、**UI flow 191**（新增 6 条 TabOrder 解析断言：默认序/自定义排列/重复拒绝/缺失拒绝/多余拒绝/未知拒绝/空拒绝/空白容忍）、PCX 48/282）；素材独立回归 **9/9**（第 9 条为上述 ini↔代码↔素材三方一致性）；`deploy.bat` 退出码 0，Release 与 Packs SHA256 均为 `319398B0D2F9C516CAAD73CD13719B67E65642071339A25F77AB1B533056467F`，whui 48 张逐张对拍一致。默认顺序的实际显示与换序生效节奏由用户实机验收。
补充（09:3x，仅配置与离线回归变动、未重建）：`H3RndNew.default.ini` 加入 `TabOrder` 键后重新 `deploy.bat`，DLL 哈希仍是 `319398B0…6467F`（源码未变），仓库与游戏目录两份 ini 哈希一致。

## 2026-10-14 第十二轮：整理按钮不显示定性 + 分页栏改纯数字页码

用户实机裁定两条：1）常规页签下应能看到「整理」按钮（当时看不到）；2）分页栏简化——只显示数字页码、点哪个直接跳那页、去掉两端 `< >` 箭头，仓库容量不设上限（“能摆几个页码按钮就显示几页”）；随后两次追加：页码一般最多两位数、按钮宽度按此定；分页行左右各留 18px（与页签行同规范）。

**「整理」不显示的确定根因（静态走查 + 子代理独立复核）**：视觉控件登记表 `g_wh_ui_visual_resources_[20]` 是“替换 PCX 原指针的恢复账本”（detach 时逐张换回 BkPack 指针），同时限定了创建数——旧版实际创建恰好 20 个（4 主按钮 + 8 页签 + 2 页签箭头 + 4 页码 + 2 分页箭头），**整理是第 21 次 `WarehouseUiCreateVisual_`，被 `>= 20` 拒绝后 label=null**（透明命中区仍创建并加入面板项，即“不可见但可点击”）。素材日志 `ready=1` 证明不是加载问题。同时存在两个叠加缺陷：整理与左分页箭头同在 `x+8`、同 `footerY` 完全重叠；整理显隐/点击资格按“页签顺序下标==0”判定，`TabOrder` 把常规移出首位时会跑到别的页签上。

实现：

- **登记表容量 20→32**（当前实际 25：4+8+2+10+1），恢复语义不变（仍逐张入账、detach 先恢复 DEF vtable 再换回原 PCX 指针）。
- **整理显隐改按页签种类**：新纯函数 `TidyVisible(expanded, TabKind)`（Catalog.hpp），显隐与点击共用；Refresh_ 里在所有其他显隐之后应用，并在状态翻转时打一条 `整理按钮 visible/owns/expanded/tab/pager` 日志，便于实机定位判定/所有权/层级。
- **分页栏纯数字重排**：删除两端箭头（ID 30050/30051 及创建/刷新/分派全部移除）；页码底图改 28x22（`pager_n/s.pcx` 重出，其余 46 张哈希不变）；整理按钮 36x22 贴左 18px 起（`kPagerLeftInset=18`，同页签行规范），间隔 4px 后页码区 `[58, 387]` 共 329px，`FittingTabCount(329,28,4)=10` 个页码（占 316px，居中偏 6px、起点 64）；ID 30400..30409（区间 30400..30411 留余量）。
- **滑动页码窗口**（去掉箭头后的可达性关键）：`PagerWindowBegin` 让当前页尽量居中，两侧相邻页永远同屏可点——**不能用整块对齐**（首屏点最右数字会永远锁死在 1..N）；离线回归对 1..120 页做全量走查（当前页在窗内、窗口不越界、前后邻页均可见）。点击数字=跳转该绝对页号；分页行滚轮=上滚上一页/下滚下一页（`PagerWheelTarget`），边界透传，与页签行滚轮按行矩形互斥。
- **可见数随页数收缩**：`PagerVisibleCount(pages,10)`，不足的槽位底图/命中/文本同步 Hide，绝不点击空白页码；页码文本仍是绝对页号 1 起。

### 第十二轮验证边界
2026-10-14 11:0x（源码冻结 35 个 C++ 文件）：`build.bat` + 审计全绿（0 警告 0 错误、13 项 PASS、**UI flow 200**、PCX 48/282）；素材独立回归 **10/10**（tidy 字形差分改用测试内自建 36x22 无字底板，且新增“自建底板与 28x22 pager 逐像素相等”交叉证明）；46 张素材哈希不变，仅 `pager_n/s.pcx` 与 `preview.png` 变化。`deploy.bat` 退出码 0，Release 与 Packs SHA256 一致，whui 48 张逐张对拍一致。整理按钮实际显示、页码点击跳页/滚轮翻页与 18px 边距观感由用户实机验收；若整理仍未显示，翻转日志可直接区分判定/所有权/层级。

## 2026-10-14 第十三轮：常规页不排序 + 整理并入主按钮四态 + 分页行距底边入配置

用户实机三条裁定：1）放入仓库“看起来自动排序了”不对——正确语义是移出时剩余前移、移入时逐个加到空位（事务层 round-8 起本就如此：Deposit 追加到记录末尾并把背包压缩到前部、Retrieve 取首个空背包位并压缩仓库记录）；真凶是**展示层**每次重建都对 filtered 按 (宝物ID,子类型) 重排。2）**常规页以外的页排序是对的（只在筛选结果内）**——即分类页保留“同类相邻”，常规页展示纯存储顺序；用新纯函数 `TabSortsDisplay(tab) = tab != Regular` 门控（Catalog.hpp），存储顺序此后只有“整理”按钮能改。3）**整理按钮的视觉效果要与两个一键按钮同款，不另发明**——废除 tidy 专用 PCX+透明命中+自管 hover 的做法，把它并入主按钮数组第 5 个：原生 `H3DlgDefButton`（吃输入、维护按压/禁用状态位）+ 同坐标 PCX 视觉件 + 换 vtable Draw 按四态换底图，素材从 `tidy_{n,s}` 2 态扩为 `tidy0..3` 4 态（与主按钮同一生成管线：bv_ok 清勾九宫格+参考金框+状态墨色+按压下移 1px），悬停/按压/禁用全部走既有 `WarehouseUiUpdateHover_/RefreshButton_` 路径。追加：**分页行距面板底边的空隙入配置** `[WarehouseUI] PagerRowBottomGap`（整理+页码整行移动），默认 37（旧固定位置隐含 32，上移 5px），钳制 8..40，就近钳制并打 raw/生效双日志；纯函数 `PagerRowY(gap)=523-gap-32`。

实现要点：素材清单 **48→50**（deploy/pack 精确清单、加载器、PCX 测试、python 断言全链同步，deploy 顺带清理游戏目录旧 `tidy_n/s.pcx`）；python 回归 10→**11**（新增 `test_pager_row_gap_config_matches_code_default`：ini 默认 == Flow.hpp 字面量 == 钳制区间内；tidy 字形差分改走“导入生成器重建无字底板”验证 make_tidy 只叠文字、四态同字形、按压整体下移 1px）；preview 拼版改“非按钮素材从按钮整行数之后起排”（888x1560，旧公式在按钮块占满第 4 行时会覆盖 tidy 行）。整理按钮显隐沿用 `WarehouseUiSetButtonVisible_(4, TidyVisible(...))`（视觉件+原生命中按钮同步 Show/Hide），翻转日志保留。

### 第十三轮验证边界
2026-10-14 12:0x（源码冻结 35 个 C++ 文件）：`build.bat` + 审计全绿（0 警告 0 错误、13 项 PASS、**UI flow 203**、**PCX 50/290**）；素材独立回归 **11/11**；`deploy.bat` 退出码 0，Release 与 Packs SHA256 一致（`618A64393D1C930DBB647B7C1BC37128C96C8CBE862B7C6456408391CCDFF6EB`），whui 50 张逐张对拍一致，游戏目录旧 `tidy_n/s.pcx` 已清理，`H3RndNew.default.ini` 双侧一致（新增 `PagerRowBottomGap=37`）。常规页放入/取出顺序、分类页同类相邻、整理四态视觉（悬停/按压/禁用）、分页行上移 5px 及配置改动的实际观感由用户实机验收。

## 2026-10-14 第十四轮：仓库状态条（悬停提示与背包同款，含文字颜色）

用户实机裁定：背包界面自带底部状态条（悬停道具时显示提示），仓库也要有一条，且**移到道具上时提示文字与背包一致（包括文字颜色）**。逆向取证（走查 0x010395D0 callback 全码 + 构造码 0x01039DFF，只读 HD_SOD.dll，RVA 见 H3Note 完整背包笔记）：

- **原生状态条本体**：`new(0x54)` + **H3DlgTextPcx 构造 0x5BCB70(thiscall)**，参数字面序 `(x+8, y+433, 389, 18, "", "smalfont.fnt", "HD_TStat.pcx", 1, 777, 5, 0)`；默认文案 RVA 0x13EA2D 是**空串**（初始无字）；指针存 `dlg+0x74`。文字颜色/字体随 H3DlgTextPcx 构造参数走——**仓库条照抄同构造即天然同色同字体，不自己另调色**。
- **原生悬停分支**（command==4）：`0x5FF970` 按坐标取命中项 id ∈ [2000,2064] → `artifactId = hero+0x1D4+(id-2000)*8`；**普通宝物**名=表 `[0x660B68]→+id*0x20+0`；**法术卷轴**(id==1)=`wsprintf("%s. {%s.}", 宝物名, 法术名)`（RVA 0x142658），法术名=`[0x687FA8]+subtype*0x88+0x10`（H3Spell.name）；然后 `状态条->SetText(vtable[13]) → Draw(vtable[4]) → 0x10FAA10(item)` 只刷该 item 屏幕矩形（→00603190）。**离开物品不清空**（枝条保留最后文案）。
- **实现**（WarehouseUiStageB）：新增三个原生工厂指针（Install 时随 StageA 字节门卫同批解析：构造 0x5BCB70 / 分配 0x617492 / 刷新 0xFAA10）+ 空白默认文案 0x13EA2D；attach 时在整理按钮之后按同构造在 `(仓库面板原点+8, +505, 389, 18)` 创建仓库状态条（ID 30500 仅日志用——H3DlgText 不参与命中测试），`AddPanelItem_` 走既有生命周期（展开显/收起隐，析构由 dialog owned-list 统一完成）。
- **悬停接线**：`MOUSE_OVER` 分支新增 `WarehouseUiUpdateSlotStatus_`——**用坐标定位仓库槽**（与原生同样不用 msg->itemId）：`GridSlotFromPoint` 命中且有记录 → `H3Artifact(id,subtype).GetName()`（同一原生宝物表）+ 卷轴 `H3Spell` 同款拼接（纯函数 `FormatStatusHint`）→ `SetText/Draw/Refresh` 同序列更新；**背包侧悬停仍走原生 2000..2063 分支更新原生状态条，两条状态条各管各自面板**；空槽/离开不做任何更新（与背包语义完全一致）。
- **与分页行的边界**：状态条固定在底边 18px 高，分页行 `PagerRowBottomGap` 钳制由 [8,40] 收紧为 **[18,40]**（row bottom = 523-gap 必须 ≤ 505；最小值时分页行底正好贴住状态条顶，static_assert 锁死）；ini 注释同步。
- 离线回归：`CheckRound14StatusBar` 覆盖文案拼接（普通/卷轴/空名/截断/容量拒绝/空法术名退化）与几何共存边界；UI flow 203→**212**；python 回归无需改素材（无新素材）保持 **11/11**（`PagerRowBottomGap` 一致性断言仍满足 18≤37≤40）。

### 第十四轮验证边界
2026-10-14 12:1x（源码冻结 35 个 C++ 文件）：`build.bat` + 审计全绿（0 警告 0 错误、13 项 PASS、**UI flow 212**、PCX 50/290）；素材独立回归 11/11（同轮含第 13 轮的 `PagerRowBottomGap` 一致性断言）；`deploy.bat` 退出码 0，Release 与 Packs SHA256 一致，whui 50 张逐张对拍一致，ini 双侧一致。仓库状态条实际显示、悬停提示与背包逐字同款（含颜色）、离开保留文案、分页行配置边界的实机观感由用户验收。

#### 第十三/十四轮追加修复：状态条地址空间混淆导致仓库界面打不开（2026-10-14 12:19 实机报告）

用户报告“仓库界面打不开了”。日志（`H3RndNew_20261014_121901.log`）定案：三次开窗都**没有** `仓库UI: attached ...` 行，且首帧即 `[Guard] WarehouseUiModal 异常 0xC0000005(访问冲突: 写入 0xC76ABA00) addr=0x0470B609 HD_SOD.dll+0x10FB609`；后两次被 Guard 的异常聚合抑制未重复打。**崩溃 PC 在 HD_SOD 的 `.data` 段**（RVA 0x10FB609 ∈ .data 0x155000..0x1114ECC）= 跳进数据执行 → 间接调用了错误指针。

根因：round-14 初版把 `H3DlgTextPcx` 构造 `0x5BCB70` 与游戏 `operator new` `0x617492` **当成 HD_SOD 的 RVA 且加了 HD_SOD 基址**——这两个是 **H3.exe 的绝对地址**（H3API 本身就 `THISCALL_12(H3DlgTextPcx*, 0x5BCB70, …)` 直接调用，可作铁证），真正属于 HD_SOD 的是 `0x10FADB0`/`0x10FAA10`/`0x1006620`/`0x113EA2D` 那一批。归属表与判别法已写进 H3Note（commit `e1e0fe8`）。

修复：**不再自行解析 H3.exe 侧的任何函数指针**，全改 H3API 官方路径——创建用 `H3ObjectAllocator<H3DlgTextPcx>().allocate(1)`（H3API 已证 `sizeof(H3DlgTextPcx)==0x54`，与原生 `new(0x54)` 完全一致，析构仍归 dialog owned-list）+ `THISCALL_12(0x5BCB70, …)` 按原生 11 个栈参数**逐参同构**（末位背包传 0；H3API 的 `Create` 内部固定传 8，故不直接用它）；悬停更新改用 `SetText`（→`vSetText`=原生 vtable[13]）→ `Draw`（→`vDrawToWindow`=vtable[4]）→ `Refresh`（经父对话框刷屏，对应原生 `0x10FAA10` 效果）。同时：①创建逻辑独立成 `WarehouseUiCreateStatusBar_`——MSVC 禁止在“需要对象展开”（含 std::vector 局部对象）的函数里用 `__try`（C2712），必须拆出去，失败只等于“没有状态条”；②install 期对每个解析出的指针做 `VirtualQuery` 归属自证并打日志（模块/偏移/页属性，期望代码页但不可执行或模块不符即告警），把这次的“肉眼读不出”的地址空间混淆变成日志里一眼可查。

验证：`build.bat` + 审计全绿（0 警告 0 错误、13 项 PASS、UI flow 212、PCX 50/290）；`deploy.bat` 退出码 0，Release 与 Packs 哈希一致（`7980FF66A86279A120E923B4722FB13CBB12F92A735C64CD1F64412383B80B84`）；面板项实际约 130 个 < 容量 320（状态条随 `ShowActivate/HideDeactivate` 展开收起同步，不会重蹈 round-12“登记表上限”那种静默失败）。**实机待用户验收：打开仓库是否恢复、状态条是否显示、悬停提示是否与背包一致**。
