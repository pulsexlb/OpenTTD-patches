# 道路运输分支（道路载具互载 / RoRo 运输）整体开发规划

> 目标仓库基线：`D:\CNS\ottd\OpenTTD-patches`（JGR's Patch Pack **0.73.1**，基于较新的 OpenTTD trunk——CMake 版本号 16.0、车辆代码已并入 vehicle.cpp/ground_vehicle.cpp 等，SA2 核实）。
> 参考实现：`OpenTTD-pulsexlb`（在 JGRPP 之上做列车解挂/模块化机场/库控三大特性）、本地 decouple fork `OpenTTD-patches-decouple`（R3R：解挂/编组/耦合，在 JGRPP 0.73.1 上净增约 71 文件 +8.6k 行，见 `Decouple-vs-JGRPP-0.73.1-对比分析.md`）——它是"在 JGRPP 上加一个大玩法特性"最直接的可复用样板。
> 配套阅读：`JGRPP-架构详解.md`、`YPS-vs-JGRPP-对比报告.md`、`道路运输分支规划.md`（需求原文，本文问题编号沿用其 Q1–Q4）。
> **实现配套**：`道路运输分支-实现规格书.md`（给实现 Agent 的"唯一输入"：改哪个文件/函数、加什么字段、存档怎么加、每步怎么验；本文管"为什么"，规格书管"改什么"）。

---

> **v2 更新（依评审意见修订）**：① 装卸画面**不做任何自定义装卸动画**，收/放道路载具统一复用载具自带的默认装卸动画（§1.3、D11）；② Q1"配对"改用**条件表达式筛选**——复用条件订单既有的变量/比较器/求值与编辑器，做"不匹配即跳过、留在队列"的语义（§4.1、D3）；③ **slot 不单独成机制**：`SlotOccupancy/VehicleInSlot` 本就是条件变量族的一员（配 `OT_SLOT` TryAcquire/Release 执行端），需要"预约/抢班次"时把相关变量编成子句即可（§4.1）。

## 0. 结论摘要（TL;DR）

1. **玩法本质**：本分支是 OpenTTD 中**首个"把玩家道路载具（RoadVehicle，RV，不含电车）当作货物由火车/轮船/飞机装运，期间保留其货物与身份"**的引擎级特性。全网（TT-Forums、GitHub）目前只有"画成货物的运车专用车"（NewGRF 数据方案，运的是抽象车辆货物）和"载具类结构玩法"（YPS 解挂等）两类先例，**没有现成的"玩家车辆互载"实现可抄**——必须像 decouple fork 那样按"引擎特性"整套自研，因此**工程上先按 R3R 分支的分层法组织：订单契约层 + 车辆状态层 + 站点/装卸层 + 存档扩展层 + GUI 层**。
2. **四个问题的推荐结论（详见第 4 章）**：
   - Q1 匹配（v2 已按评审改为条件表达式）：**载体主动挑选 + 条件表达式筛选**——逻辑式复用游戏现成的条件订单体系（变量/比较器/求值/编辑器），按需补少量"RV 侧"变量；**不匹配的 RV 留在队列（跳过），不会乱装**；slot 不是平行机制——`SlotOccupancy/VehicleInSlot` 本就是条件变量族成员（配 `OT_SLOT` TryAcquire/Release），作为子句编进表达式即可。
   - Q2 隐藏停放：**不要销毁/重建 RV，也不要自造"虚拟车库 tile"**；采用与 `GVSF_VIRTUAL`（模板替换的虚拟列车，全库已有 40+ 处排除点先例）同构的**新车辆子类型位/状态位"被运载(TRANSPORTED)"**，把"运载中的 RV"从道路 tick、碰撞、绘制、成本、列表等系统中整体豁免，本体保留在车辆池与存档里；"车库面板"只是**一个过滤后的普通车辆列表窗口**，不新建库格。
   - Q3 装卸表现：采用"**站台内瞬间出现/消失**"（v2：**不做自定义装卸动画，画面统一用载具自带默认装卸动画**），且**要求目的站必须存在与 RV 匹配的公路停靠站格（推荐直通式作为装卸坡道）且有无占用空间**；空间不足/长度超限/铰接车放不下时，RV 保持在"已装车待卸"状态继续留在载体上，随车去下一站或在该站等待重试（与货物"卸不下就 Keep/滞留"的既有语义同构）。
   - Q4 重量/容量：用"**运载单位**"记账（每节"运载容量(吨)"由 `cargo_cap×该货单位重量/16` 换算得出，RV 按**整备总重(吨)**折占单位，见 4.4#3），载体某节能否装载由**该节当前货物是否属于 CargoClass::Oversized** 门控（天然满足"只能被能装 oversized 的车装"的需求，且零 NewGRF 改动；默认内容无 oversized 货，需运车类 NewGRF 或开发测试 GRF 提供，见 D1 注）；**按节判定、逐节扣除**，解决铰接/多隔舱问题；**被运 RV 连同其自身载货的重量一起计入载体总重**（仅对火车/公路车有物理意义，船/机无重量模型）；初期**不允许一节同时混装虚拟载具与普通货物**（不同节可以混），二期再考虑节内共享剩余容量。
3. **规模预估**：参照 decouple fork（约 71 文件 / +8.6k 行、1.5–2 个月个人开发），本特性核心（不含视觉细化）预计 **40–60 文件、+6k–9k 行**；建议按第 5 章 **P0–P8 八个里程碑**推进，其中 P2（订单+状态机）与 P4（挂起/恢复生命周期）是风险最高的两块，必须各自带"最小可玩验证"。
4. 全程**遵守三条工程铁律**：确定性（无未固定浮点、无迭代序依赖，否则联机 desync）；存档单向兼容（fork 存档不回 JGRPP）；侵入面隔离（普通货物/普通列车行为零改动，所有新逻辑走新订单类型 + 新状态位 + 专门的判断入口）。

---

## 1. 玩法构想：功能效果与实现方式的总体思路

### 1.1 玩家可见的目标行为（以铁路为例，来自需求 MD）

道路载具排程（订单示例）：

```
[公路站 A: 装载中… →] 去火车站 A(或连体站) 订单#1：到达后"等待被运载"
                       去火车站 B(或连体站) 订单#2：到达即"被卸载"（此时车在火车上）
                       去公路站 C 订单#3：落地后按原计划继续装货/卸货
```

火车/轮船/飞机排程：

```
… → 去站 A 订单#1（附加"装载道路载具"，或对列车来说就是普通"装载"但只吸 RV）
      → 去站 B 订单#2（附加"卸载道路载具"）
      → 继续原有运输
```

画面呈现（v2：统一默认装卸动画，无自定义动画；详见 1.3 呈现方案）：

- 站 A：RV 驶入公路停靠站停定 → 进入"等待装运"状态（车停在站格排队/驻留，面板显示"等待被运载"）；火车进站停靠，按订单筛选条件（§4.1）把符合条件的 RV **逐台收起**——画面仅表现为"该 RV 从公路站格消失 + 载具进入其自带的默认'装载中'动画"（v2：不做吊装/滚装专属动画）；
- 途中：载具按既有方式显示装卸状态；"运载面板"（独立窗口，见 P5）列出实际装着的每台 RV（车型、重量、目的站、状态）；
- 站 B：火车停靠后按格位规则（§4.3）逐台放车，画面 = 默认"卸载中"动画 + RV 出现在公路站格，随后 RV 按原排程继续行驶。

### 1.2 必须被"保留/传递"的属性清单（影响对象模型）

| 属性 | 保留方式 | 风险点 |
|---|---|---|
| RV 引擎与改型(refit) | 本体保留 | refit 在收运期间禁止/锁定 |
| RV 自身货物（货物包 cargo packets、货量、来源/目的站） | 随本体 cargo list，全程不经任何站点货物槽 | 在途"时间/里程"计量需显式选口径（**D12**，默认冻结）；**货流图/链路图上中间被运载段不可见**（详见 §4.4 末节） |
| RV 的订单与排程位置 | 挂起冻结，落地后从**下一站**继续 | 需要定义"我应被送到哪"与"我的下一站在哪"的契约 |
| 单位号、公司、组、涂装、命名、价值/年龄/可靠性 | 本体保留 | 统计窗口需过滤（仿 GVSF_VIRTUAL 排除点） |
| 正在执行的时刻表时间 | 冻结/顺延 | 可先冻结，二期再做"在途时间计入时刻表" |

### 1.3 呈现方案（v2 定稿：不做自定义装卸动画）

按评审意见，**本特性不制作任何专属的吊装/滚装/装卸动画资源**，收/放 RV 的画面统一复用游戏既有表现：

1. **装卸过程画面 = 载具自带的默认装载/卸载动画**：RV 的收起/放下只是两个逻辑瞬间（从公路站格消失/在公路站格出现），其间的"装载中/卸载中"观感直接使用引擎对普通货物装卸的既有动画与状态显示，零新美术、零帧表、零"落点方向"动画校验；
2. **运载内容呈现 = 文本/图标数据视图（本期唯一视觉产出）**：运载面板（独立子窗口，见 P5）按"引擎名 × N、总重、目的站"罗列，风格沿用载货窗口逐节罗列的做法（train_gui.cpp DrawTrainDetails / roadveh_gui.cpp DrawRoadVehDetails 同款）；
3. **可选延伸（默认不做、不作为本期范围）**：把被运 RV 静态缩略贴到载体上——仅数据驱动取 RV 引擎一帧缩小绘制，无动画；工作量集中在绘图缓存一致性，若做需另立"显示开关"（默认隐藏）。

> 结论：**"最小可玩闭环"（P2/P3）即覆盖 100% 玩法逻辑**，视觉仅在"运载面板"做文本罗列；P6 只做船/机打通与默认动画适用性验证。

### 1.4 产品决策点（实现前必须先拍板；本规划给出推荐值）

| # | 决策 | 推荐 | 备选 |
|---|---|---|---|
| D1 | 谁能装 RV | 载体（火车/船/机）的**某节当前 cargo_type 属 CargoClass::Oversized**（运行时 `IsCargoInClass` 判定，cargotype.h:245） | 仅限固定虚拟货物/仅限专用车辆 |

> **D1 修订（M11d，2026-09-11，已实现）**：原推荐"只允许 oversized 类货物的节"在**原版内容下完全不可用**（0.73.1 货物表没有任何 oversized 货物，见下注），会让整个特性开箱即死，也无法回归测试。**改为三档设置 `vehicle.rv_transport_carrier_parts`**（类别：专家设置；独立设置页"道路车辆运输"；`src/roadveh_transport.cpp: RVTransportPartCanCarry()`，`src/table/settings/game_settings.ini`）：
>
> | 值 | 规则 | 原版内容下的效果 |
> |---|---|---|
> | 0 | 任何有载货容量的节（`cargo_cap > 0`） | 全部车厢/货舱都收车 |
> | 1 | 该节当前货物属 `CargoClass::Oversized`（**原 D1 的门**） | **谁都装不上**（无 oversized 货）→ 需运车类 NewGRF |
> | **2（默认）** | 该节货物满足 **散货 `Bulk`**、**`Oversized`**、或**货物标签为 `VEHC`（"运载工具"）**任意一条 | 散货（敞车/漏斗车：煤、矿石、谷物、水果、糖、太妃糖）收车；木材/钢材/货物/邮件/油/乘客都不收车 |
>
> 档 2 是"**针对 GRF 特化**"的门（M11d 定稿，替换了中间尝试过的两种写法）：
> 1. 原稿"只允许 Oversized"在原版内容下谁都装不上（无 oversized 货）；
> 2. 中间尝试过的"子集判定（类别集合 ⊄ {乘客, 液体, 特殊}）"**实现上失效**——液体货物都额外带 `Potable`/`NonPotable` 口味位（原版 `Oil` = `{Liquid, NonPotable}`），子集判定会放行油罐车；改成"含乘客/液体即拒"虽能挡住罐车，但那是靠类别**排除法**猜意图，对 GRF 自定义货物仍不准；
> 3. **现定稿：正面白名单** —— `IsCargoInClass(Bulk)` ∥ `IsCargoInClass(Oversized)` ∥ `CargoSpec::label == 'VEHC'`。运车类 NewGRF（汽车渡轮/汽车运输船/驮背车厢）按其本意提供三类之一的货物，引擎不需要猜；原版内容下只有散货车厢满足，其余（木材/钢材/货物/邮件/液体/乘客）一律拒绝。
>
> 判定只在**装载**时进行，**卸载不看此设置**（`RVTransportDetachAtStation` 不判容量门），所以中途切换设置不会把已在车上的 RV 丢在路上。三档均已用 `testrun\verify_carrier_parts.ps1` 无头验证（默认值为 2；同一节木材车厢在档 0 下 `rv_capacity=30t` 且 `attached=true`，档 1/2 下 `rv_capacity=0t` 且 `attached=false`）。**回归套件**在 `_common.ps1` 里统一先执行 `setting vehicle.rv_transport_carrier_parts 0`（测试存档的载体是木材车厢，属于被档 2 拒绝的货物）——即机制类脚本在"门全开"下跑，门本身由上面那支脚本覆盖。
>
> D1 注：**0.73.1 自带货物表没有任何 Oversized 类货物**（默认货车只含 Passengers/Mail/…/PieceGoods 等，SA2 核对 table/cargo_const.h），所以档 1 在原版内容下不会开启；要玩严格模式需挂载定义了 oversized 货（运车货列/滚装船常见的 "Cars/Vehicles" 货）的 NewGRF，开发期另备一只测试 GRF。这与 MD"只能被能装 cc_oversized 的车装"的原意一致——**"可装性"由 NewGRF 内容自然供给，引擎零硬编码**。
| D2 | RV 能"上车"的门槛 | 载具一节一判，容量按运载单位扣除；超重拒绝（新闻提示） | 只在整列整船层校验 |
| D3 | 匹配规则（v2） | 载体主动 + 等待队列 FIFO 扫描 + **条件表达式筛选**：逻辑式复用条件订单的变量/比较器/求值与编辑器（§4.1），**不匹配即跳过（RV 留在队列）**；默认提供"目的站匹配"预设子句防装错 | 无平行机制：slot 是表达式既有变量维度（SlotOccupancy/VehicleInSlot 等），精确班次预约再按需组合 `OT_SLOT`（§4.1） |
| D4 | 装载/卸载地点 | 要求同一站实体(Station)内存在对应类型的公路停靠站（Bus/Truck 按 RV 当前货物类别），直通式优先 | 无公路设施也允许（纯逻辑消失/出现） |
| D5 | 收运期间 RV 状态 | 冻结一切（不老化/不折旧/无故障/无运行成本/不计时刻表） | 正常折旧（更真实但复杂） |
| D6 | 运价 | 免费内部转运（仅计"服务次数"统计）。**评审定稿：不做收费模式**（备选方案放弃，见 M11j） | ~~按虚拟货物计费（二期，可做 AI 收入）~~ |
| D7 | 允许的最大被运 RV 数/节 | 无全局上限，由容量决定；**每张"装载道路载具"订单可再设"最多同时装载 N 辆"（0=不限，默认不限）**，判定为"载体当前已载数量 ≥ N 即停止装载"（`RVTransportAttachAuto()`，见 M11e） | 有全局上限（防滥用/存档膨胀） |
| D8 | 运载中的 RV 是否允许乘客/货物同时上下 | 不允许（收运=整车上锁） | 允许卸货装货（玩法怪） |
| D9 | 卸载失败兜底 | RV 留在载体上随车去下一站/在本站反复重试，绝不丢弃 | 强制丢弃（不推荐） |
| D10 | 电车(tram) | 明确**不支持**（需求 MD 指定） | —— |
| D11 | 装卸动画（v2） | **统一复用载具自带默认装卸动画，不制作任何自定义装卸动画**（§1.3） | 自绘吊装/滚装动画（明确不做） |
| D12 | 被运 RV 在途货物的"时间/里程"口径 | **默认完全冻结**（age/travelled 都不计，见 §4.4 末节口径 A）：免费内部转运下最简、零失真风险 | B 只计时间 / C 时间+里程联动累进（做收费运价时于 P7 再启用，需在载体移动热路径联动 RV cargo） |

---

## 2. 现状调研：JGRPP 0.73.1 中与本特性相关的系统（代码地图）

> 说明：0.73.1 已采用 trunk 15.x 的代码重组：`roadveh.cpp` 并入 `roadveh_cmd.cpp`/`vehicle.cpp`/`ground_vehicle.cpp`；通用装卸在 `economy.cpp`；道路停靠站建筑逻辑在 `roadstop.cpp`；文档所述文件/函数均经本会话代码核对（详见附 A 行号表）。

### 2.1 车辆对象与"不在图上的车辆"已有一个官方先例：GVSF_VIRTUAL

- `Vehicle` 基类字段：`tile/x_pos/y_pos`、`vehstatus`（含 `Stopped`、`Hidden`）、`vehicle_flags`、`subtype`（地面车辆角色位 `GVSF_*`，`vehicle_base.h:77-83`）。
- `GVSF_VIRTUAL(=6)`：模板替换（tbtr）用来表示"只在建造窗口里存在、不占图、不进列表"的虚拟列车。**全库已为此打了 40+ 处排除点**：economy 不计运行成本/利润（economy.cpp:159/248/541）、vehiclelist 不列入普通列表（vehiclelist.cpp:149/178）、不绘制（vehicle.cpp:423）、不参与灾难/基建统计/公司统计/网络（disaster_vehicle.cpp:592、infrastructure.cpp:89+、network_server.cpp:1890）、销售命令带 `SellVehicleFlags::VirtualOnly` 通道（vehicle_cmd.cpp:270/606）、存档里按链连续处理（sl/vehicle_sl.cpp:299）、车场命令跳过（group_cmd.cpp:142+）。
- **启示**：引擎完全接受"一个活的、在池里、但不在地图上、被大部分系统豁免"的车辆。做"被运载中的 RV"就是**再定义一个这样的状态位/子类型位，并仿照上表补齐豁免点**（约 20–30 处遍历），而不是发明新存储。

### 2.2 道路车辆：到站、装载、队列（roadveh_cmd.cpp）

- RV 每 tick：`RoadVehController`（roadveh_cmd.cpp:2140）→ `ProcessOrders(v)` → `HandleLoading()` → 前进/停站；进站判定发生在逐帧推进逻辑（roadveh_cmd.cpp 约 2002–2114）：
  - **港湾式**（bay）：每个港湾格一个 `RoadStop`，Bay0/Bay1 两个车位居位 + 共享的 `entrance busy` 标志（roadstop_base.h）；车头须停在停站帧才停，**铰接车（articulated）被拒绝进入港湾停靠站**；后来的车在站外排队；
  - **直通式**（drive-through，DT）：按**每方向 Entry 记账占用累计车长（以 TILE 为单位）**、跨多格连续段共享容量（roadstop.cpp:296-337/400-461），无"全局 bay 槽位"概念；一辆车在 DT 站台停车**不阻塞**后车经过/使用相邻"内联"停靠位（`RoadStop::IsDriveThroughRoadStopContinuation`）；
  - 站点匹配严格分 **Bus/Truck 类型**：`GetRoadStopType(tile) == (v->IsBus() ? Bus : Truck)`；`IsBus()` 由**当前货物是否 Passenger 类**决定（roadveh_cmd.cpp:91）。
  - 到站动作 `RoadVehArrivesAt(v, st)` → `v->BeginLoading()`（roadveh_cmd.cpp:2086-2091 / 2024 / 1626）。
- **装卸主链是车型无关的公共机制（本项目最重要的挂接点）**：`Vehicle::BeginLoading`（vehicle.cpp:3407）→ `PrepareUnload`（economy.cpp:1524，把车头 push 进 `Station::loading_vehicles`）→ 站每 tick 的 `LoadUnloadStation`（economy.cpp:2438，**按进站先后 FIFO 逐个装卸**）驱动 `LoadUnloadVehicle`（economy.cpp:1944）→ `HandleLoading`（vehicle.cpp:3797）判满后 `LeaveStation`（vehicle.cpp:3574，`MakeLeaveStation` + 出列）。火车 `TrainEnterStation`（train_cmd.cpp:5056）、船 `ShipController`（ship_cmd.cpp:851）、飞机 `AircraftEntersTerminal`（aircraft_cmd.cpp:1505）**全部汇聚到 `BeginLoading`**——"载体到站装载 RV"应挂在这里，且"本站是否还有车辆在装"天然有 `st->loading_vehicles` 可查。
- **JGRPP 订单层现成原语**（0.73.1 的 OrderType 已扩到 0–14：OT_GOTO_STATION/GOTO_DEPOT/LOADING/LEAVESTATION/…/OT_WAITING=9/OT_LOADING_ADVANCE=10/OT_SLOT=11/OT_COUNTER=12/OT_LABEL=13/OT_SLOT_GROUP=14，order_type.h:74-110；新类型从 15 起追加、不重排，与 R3R 在 decouple fork 的追加法一致）：条件订单变量族（含 CargoWaiting/FreePlatforms/SlotOccupancy/VehicleInSlot…）、`OT_SLOT` 的 TryAcquire/Release（"槽位预约"现成语义）、订单级限速、`MOF_RV_TRAVEL_DIR`（`Order::SetRoadVehTravelDirection`，order_cmd.cpp:2711——**可为"下车后朝哪个方向走"复用**）、以及同站续装 `CheckRestartLoadingAtRoadStop`（roadveh_cmd.cpp:1603）。这些是 Q1"匹配/预约"与"下车方向"设计的免费地基。
- **条件订单体系（Q1 条件表达式方案直接复用的"逻辑式"）**：`OrderConditionVariable` 变量族 + 比较器 + 取值求值 + order_gui 条件编辑器均现成，且条件订单对道路车等**所有车型**开放；既有求值以"拥有该订单的车辆"为作用域（pulsexlb 追加的 DecouplePart 变量即在该求值 switch 中求值，order_cmd.cpp:4768-4771 一带）。本特性只需要：(a) 把求值入口参数化为"任意候选 Vehicle"；(b) 增补少量"RV 侧"变量；(c) 在订单 GUI 里复用编辑器组件。→ 详见 §4.1。
- **启示**：① 若把"等待被运载的 RV"停在公路停靠站，它会像普通车一样占用港湾 bay / DT 累计车长——**站格即物理容量**，这是 Q3 各种溢出问题的根源（见 4.3）；② 现有"不在地图上却保留身份的车辆"先例**只有** depot 内车辆（仍占图）与仅限火车的 TBTR 虚拟列车（`GVSF_VIRTUAL`），道路车此前**没有** off-map 存车机制，必须新增（Q2 方案）。

### 2.3 站点：设施、货流、登记

- `Station` 的设施位 `StationFacility`（Train/TruckStop/BusStop/Airport/Dock，station_type.h:53-62）；一个 Station 实体可同时拥有多类设施（"连体站"），**站内货物(`st->goods[]`)按货物类型全站共享**——公路站卸的货火车能装走，天然支持"道路↔轨道中转"。RoadStop 与"站"是多对一。
- 车辆到站装卸全部由 `economy.cpp` 的 `LoadUnloadVehicle(front)`（1944 行起，按 `current_order == OT_LOADING` 进入）驱动：**逐节遍历**（`GetMovingNext`），每节用 `v->cargo_cap`、`v->cargo`(VehicleCargoList)、`st->goods[v->cargo_type]` 做装卸；每次最多装卸 `GetLoadAmount(v)` 单位（`gradual_loading` 时逐次渐进）；完成为止（装/卸时间由 `load_unload_ticks` 累积决定）。
- `CargoClass::Oversized(=10)` 与 `IsCargoInClass()` 都在 `cargotype.h`（51/245）；车辆"能否载某货"来自 NewGRF 属性（`cargo_allowed` 等）与当前 refit。**不需要任何 cargo 系统改动就能判断"该节现在是不是在装 oversized 类货物"**。

### 2.4 载体属性与重量

- 重量物理仅存在于**火车与公路车**（船/飞机引擎无 weight 字段，飞机只有航程限制）。编组重量缓存 `GroundVehicleCache::cached_weight`（ground_vehicle.hpp:33，"仅车头有效"）在**每次载货变化**时由 `GroundVehicle::CargoChanged()`（ground_vehicle.cpp:103-145）逐节重算：`u->GetCargoWeight()` + 自重 → 坡度阻力/质心/`PowerChanged()`/真实制动全部联动。触发链 = 装卸 dirty → `MarkDirty()`（火车 train_cmd.cpp:4993 / 公路 roadveh_cmd.cpp:438）或 refit/ConsistChanged。
- **把被运 RV 重量加入载体** = 在 `CargoChanged` 逐节累加处加一个 `GetTransportedWeight()` 分量（仅对火车/公路车有意义）；船/机只做容量与显示。引擎**没有**"按重量拒绝装载"的现成逻辑。

### 2.5 订单与 GUI

- `OrderType`（order_type.h）与 `OrderExtraInfo`（order_base.h，JGRPP 懒分配扩展字段，R3R 用它在**不重排存档布局**的前提下加参数）；订单窗口 `order_gui.cpp` 用下拉列表增订单类型；语言串在 `src/lang/*.txt`。
- 车辆 UI 是**多个独立窗口类**而非"单窗口多页签"：`VehicleView`（主视图，vehicle_gui.cpp:3955）、`VehicleDetails`（详情，:3058；**只有火车详情窗有内部 5 个 tab**：载货/信息/容量/合计/性能，`TrainDetailsWindowTabs` vehicle_gui.h:26、widget 位 vehicle_widget.h:51-66）、`VehicleOrders`/`VehicleTimetable` 为独立子窗口；各车型详情绘制分函数 `DrawTrainDetails/RoadVehDetails/ShipDetails/AircraftDetails`（train_gui.cpp:429、roadveh_gui.cpp:30、ship_gui.cpp:65、aircraft_gui.cpp:30）。→ 本特性"运载清单"若做成页签只适合火车详情窗；跨车型统一建议**做一个新的独立子窗口（由 VehicleView 的按钮打开）**，避免动 4 套非火车详情布局。
- 车库窗与"车库车辆列表"是两回事：车库窗（`DepotWindow`，depot_gui.cpp:273，窗口号=库 tile）用 `VehiclesOnTile`+`IsInDepot` 枚举**物理停在库格上的车**（vehiclelist.cpp:78 BuildDepotVehicleList）；而 `VL_DEPOT_LIST` 列表窗按**订单目标**枚举"明确要去该库的车"（vehiclelist.cpp:192 FindVehiclesWithOrder）。"被运载 RV"两者都不该出现（收运中 RV 无 tile、无"去车库"订单）。
- 车辆与 tile 的关系：**地图没有存车指针**，各类型用 tile→车链哈希（vehicle.cpp:846 UpdateVehicleTileHash；`VehiclesOnTile` 查哈希，vehicle_base.h:1772+）；`Vehicle::tile` 默认 `INVALID_TILE`（vehicle_base.h:253）；**"实例存在但不在图上"的现成先例只有 GVSF_VIRTUAL 虚拟车全家桶**（不加入 tile 哈希、Hidden+Stopped、不绘制、不进列表、销售走 VirtualOnly 通道、存档链一致性检查 vehicle_sl.cpp:299）。→ B2 方案的"让被运载 RV 离图"可直接照搬这套：清 tile 哈希条目 + Hidden/Stopped + 新角色位豁免（见 Q2 表 B2 行）。
- 车辆列表/详情/库窗均假设"车有 tile 且有订单"：`VL_*` 列表按订单枚举（vehiclelist.cpp:139 GenerateVehicleSortList 与 :192），详情窗按 `v->Next()` 链画；新增"被运载面板"最省事 = 直接 `Vehicle::IterateTypeFrontOnly` 过滤新状态位。
- 存档体系（0.73.1 实况）：`SAVEGAME_VERSION` 钉在基线 292（sl/saveload_common.h:465），特性全走 **XSLFI 特征（含独立 16 位特征版本）+ SLXI 子块**；"加字段"标准三件套 = VEHS 表的 `SLE_CONDVAR_X(…, SlXvFeatureTest(XSLFTO_AND, XSLFI_xxx))`（例 sl/vehicle_sl.cpp:1124 economy_age）+ 独立"每车侧表"chunk 模式（仿 'VESR' sl/vehicle_sl.cpp:1893）+ `AfterLoadVehiclesPhase1/Phase2` 迁移（sl/vehicle_sl.cpp:285/476）；老档兼容用 `IsSavegameVersionBefore&&SlXvIsFeatureMissing` 句式（afterload.cpp:666）。**不要动 Order 既有字段的位宽**（pulsexlb 把 type 8→16 位且无 compat 双档，旧档强类型校验可能直接判 corrupt，SA4 存疑项）——新参数一律新字节/新字段。
- 命令：`Commands` 枚举在 command_type.h:492（**只能插在 End 哨兵前**，命令号入网络包）；`DEF_CMD_TUPLE` 注册宏在 `*_cmd.h`（如 vehicle_cmd.h:27），执行函数写 `CmdXxx(DoCommandFlags, …)`，`_command_proc_table` 自动生成（command_table.cpp:185-196）；UI 用 `Command<Commands::X>::Post`；联机走 DoCommandP→network_command。
- 设置：定义迁移到 `src/table/settings/*.ini`（含 game_settings.ini），由 settingsgen 生成 `table/settings.h`（settings_table.cpp:79 include）；结构字段在 `src/settings_type.h`；老档兼容名映射在 settings_compat.h。
- 语言串：`src/lang/english.txt` 必填主语言，STR 名 strgen 自动编号；车种×4 字符串用 `###length VEHICLE_TYPES` 组（english.txt:4679 起）。

### 2.6 存档与联机（决定一切新状态的落地方式）

- `SAVEGAME_VERSION` + `SLV_*` 枚举在 `sl/saveload_common.h`（decouple fork 用 292→367、在 `SL_MAX_VERSION` 之前插新枚举不挤压编号）；JGRPP 的 `XSLFI_*` 特性标志（`sl/extended_ver_sl.*`）允许**不 bump 基础版本**地给字段加"特性门"，并保证 vanilla/JGRPP 打开 fork 档时忽略未知 chunk；老档迁移在 `afterload.cpp AfterLoadGame`。
- 联机为 lockstep 帧同步（命令经 `DoCommandP`→`network_command`），任何新状态变更必须发生在**命令或 tick 内确定性代码**中；新命令需注册进 `command_table.cpp`。
- 铁律：被运载 RV 的状态（挂起中/在哪台载体上/装载了哪些 RV）**全部要入档**；`orders_backup/unitnumber_backup` 这类 NOSAVE 运行时备份模式（R3R 用过）只适用于临时事务。

### 2.7 decouple fork（R3R）可直接复用/参照的资产清单

| R3R 资产 | 本特性的用法 |
|---|---|
| 追加式新订单类型（尾部追加 15/16/17，不重排 0–14）+ `OrderTypeMask` 加宽 | RV"等待被运载"、载体"装载/卸载道路载具"都做成新类型或新旗标 |
| `OrderExtraInfo` 懒分配 + XSLF 特征门控的入档套路 | 新订单参数零成本入档 |
| 伪引擎/`GVSF_*` 新角色位 + `IsPrimaryVehicle()` 式"统一问法" | "被运载中"角色位 + 全库豁免点问法（收运中 / 可被收运） |
| `WAIT_COUPLE` 的"就地静候、占位不装卸"先例（TrainEnterStation 特判防误装卸推单） | 等待被运载 RV 的"不装卸只等"状态机 |
| `orders_backup/unitnumber_backup` NOSAVE 事务模式 | 收运/落地瞬间的排程交接 |
| 存档 367 修复经验：fork 档 base 版本与 EXT 规则冲突的自读失败坑 | 本分支若再 bump 版本，先做"自读自档"回归 |
| depot 段工具 GUI（depot_gui.cpp +186） | "被运载车库面板"列表窗口 |
| artic"身份/状态分层 + 快照守恒"方法论 | 铰接 RV 的收/放完整性校验 |

**pulsexlb（YPS 解挂的现代移植，SA4 底稿 sa4_pulsexlb_decouple_patterns.md）可复用的补充模式**：

| pulsexlb 资产 | 本特性的用法 |
|---|---|
| 身份/物理分离：`Primary()`(consist 信息载体) vs `First()`(物理链头) + `consist_primary` 存档位（vehicle_base.h:263-264/778）+ 链手术统一 `MaterialiseTrainPrimary+NormaliseTrainHead` | 若"运载中 RV"需要被当"货物"看待的同时又保持自身统计身份，这套"信息载体可异于物理头"的机制是最接近的抽象（本项目初期不必须，但收运/落地瞬间做统计交接可借鉴） |
| 存档不 bump：`SAVEGAME_VERSION` 保持 292 + 3 个恒写 XSLF 特征 + 命名表按字段名匹配（sl/vehicle_sl.cpp:1059/344-369 重建指针） | 本特性 P1 存档走**纯 XSLF 路线**（与 pulsexlb 同款），不 bump 主版本，避免 R3R"367 自读失败"坑 |
| "链手术 = 备份→试算(含 NewGRF 假想校验 var0x42/start-stop 回调)→校验→回滚"（TryTrainCouple/Decouple 模式） | 收运/落地 = 把 RV 从路网摘除/放回的一次"链手术"，套同一模板，备份必须从物理链头开始 |
| 认领协议 claim（NOSAVE 瞬态 couple_target/couple_claimant；GetValidCoupleClaimant/ClaimCoupleTarget 夺标重寻路；"接触路径忽略 claim 兜底"） | 多载体抢同一台等待 RV 的防重设计（Q1 二期）；全部瞬态不落档，读档靠兜底 |
| 订单"站订单开关 + 自动插入参数行（OT_DECOUPLE 行禁移动/级联删除/跳过执行）"的双层订单结构 | 载体"到站装载/卸载 RV"与"等待被运载"都可做成"旗标/参数行"而非新增一堆独立订单类型，减少类型膨胀 |
| 订单条件变量（DecouplePart=23 等）、车辆状态串（"Heading for couple"/"Waiting for locomotive"） | 状态显示与条件分流样板 |
| 作者式 merge 备忘 `docs/readme-before-merge.md`（不变量清单 + 未勾选项） | 每里程碑维护一张"不变量+易碎点"checklist，防止新特性踩同批坑 |

> 注：R3R fork 与 pulsexlb 对"无引擎链头"的解法不同（假引擎编组 ConsistGroup vs FrontWagon+Primary 分离）；本特性属于"把车当货"，两个参考实现都只提供**工程模式**而非成品代码。另注意 pulsexlb 的已知弱点（decouple_part 不入档、设置可能不持久化、Order type 位宽 8→16 无 compat 双档）——移植时一律绕开。

---

## 3. 总体技术架构（推荐方案）

### 3.1 三层模型

```
┌───────────────────────────────────────────────────────────────┐
│ L1 契约层(订单)  RV:"等待被运载"(可带:允许的载体条件/目标站)   │
│                  载体:"到站装载/卸载道路载具"(可带:数量/条件)   │
├───────────────────────────────────────────────────────────────┤
│ L2 对象层(状态)  待运队列(站点侧) → 装车 → TRANSPORTED 挂起集   │
│                  (挂在载体上,随载体移动) → 到站 → 落地恢复       │
├───────────────────────────────────────────────────────────────┤
│ L3 呈现层        默认装卸动画复用 / 运载面板(文本) / 被运载车库面板 / 站台格位 │
└───────────────────────────────────────────────────────────────┘
```

### 3.2 数据模型（推荐结构草案）

```cpp
// (1) 站点侧：等待被运载队列 —— 用"虚拟货物"的现有站台 goods 槽来承载？
//     否：见 3.3 决策。改为站点附加结构：
struct StationRoadVehicleQueueItem {          // 挂在 Station 上（新字段，XSLF 门控）
    VehicleID rv_id;            // 等待被运载的 RV（Front）
    StationID dest_station;     // RV 希望被送到的下一站(=RV 排程中"被卸载"订单指向的站)
    // ……可再存 RV 类型/数量快照便于过滤与显示
};
// 每个 Station 一份"待运队列"，按公司内先到先得排序。

// (2) 载体侧：一节车可以装运的 RV 名单
struct TransportedRoadVehicle {               // 挂在 Vehicle(节) 的 cargo 旁（新字段）
    VehicleID rv_id;            // TRANSPORTED 状态下的 RV 本体引用
    StationID load_station;     // 装载站（落地校验/统计用）
    // 重量、占用单位数在装载瞬间快照进载体缓存；运行中不变
};
// 载体节上：Vehicle 新增 transported_rvs（vector）或复用 OrderExtraInfo 式懒分配。
// 载体 Front 缓存: cached_transported_weight / 单位占用总数。

// (3) RV 侧：状态与现场信息
//   - subtype 新位 / vehicle_flags 新位: "被运载中(TRANSPORTED)"
//   - 收运时现场快照: NOSAVE 运行时字段 backup_orders / 落地目标(站,格,方向)，
//     但 orders 本体保留并冻结在"被卸载"订单前；落地后从该订单继续。
```

### 3.3 三个关键架构决策（详析在第 4 章）

1. **不把 RV 编码成 cargo packet / 不塞进 `st->goods`**：货物单位是"无身份的数量"，而 RV 需要本体、订单、货物三样同时存活；硬塞会破坏货物系统的统计/联机/存档语义。→ 独立队列 + 独立挂起集（Q2/Q4 展开）。
2. **容量与重量用"运载单位"记账、物理上挂在载体节上**：每台 RV 换算为 `ceil(整备重×系数 / 单位基准)` 个运载单位扣在该节的"运载容量"上；被运 RV 的自身货物**不算**载体"货物利润"，但**重量计入**载体总重（Q4 展开）。
3. **装/卸的物理锚点 = 站内公路停靠站格**（Q3 展开），不是铁路站台/船坞/机场本身：火车在 A 站站台装货的同时，逻辑上从 A 站公路停靠站的"待运队列"取车，卸货时在 B 站公路停靠站"放车"。没有公路停靠站的站无法作为被运 RV 的装卸点（UI 校验 + 新闻提示），这样**完全不碰"车辆跨地形瞬移"的碰撞/物理问题**。

### 3.4 最小可玩闭环（建议第一里程碑就打通）

> 一辆卡车在"火车 A 站(带公路停靠站)"等待 → 火车在站 A 有"装载 RV"行为 → 卡车消失并计入火车 → 火车到站 B(带公路停靠站) → 卡车出现、续跑自己排程。**先不做飞机/轮船、不做条件过滤、不做运价、不做动画**，只验证：订单、状态、挂起/恢复、存档、不炸联机。

### 3.5 分载具类型差异（重要）

| 类型 | 装载节 | 卸载格 | 特殊问题 |
|---|---|---|---|
| 火车 | 每节车厢(需 oversized cargo) | 站 B 公路停靠站格 | 车厢数多，可"整车从队尾逐节扣容量"；铰接车厢按整段判定 |
| 轮船 | 船的一节（多节船各自有 cargo_cap） | 码头/船坞所在站连体公路停靠站 | JGRPP/trunk15 多节船每节独立 cargo → 逐节判定；舱面画 RV 可选 |
| 飞机 | 全机容量（含 mail compartment 只装 mail） | 机场站连体公路停靠站 | 只有整机容量概念；refit 到 oversized 类的货机才可装；乘客机型自然拒绝 |

---

## 4. 需求 MD 四个问题的分析与方案可行性

### Q1 等待被装载时的"配对"：谁选谁？用什么判断？

**问题本质**：站 A 可能同时有多辆 RV 等待、多列火车/多艘船经过。需要定义配对语义，否则会出现"被不相干的车运走/运到错站"。

**候选方案**：

| 方案 | 描述 | 可行性 | 优点 | 缺点/风险 |
|---|---|---|---|---|
| A1 slot 维度（并入 A2，非独立路线） | "槽位/路签"不另造机制：`SlotOccupancy / VehicleInSlot / VehicleInSlotGroup` 等**本就是条件变量族成员**（order_type.h:217-243），`OT_SLOT`(TryAcquire/Release/ReleaseSlotGroup) 是"把槽位当资源占用/释放"的执行端原语（order_type.h:96-110）——把它们作为普通子句编进载体（或 RV）的筛选表达式即可表达"该槽空着才装/该槽被我这趟车领取后放行"等语义 | 直接可用（复用条件体系，无需单独生命周期管理） | 与 A2 同构、零新增机制；为"预约/抢班次"保留表达空间 | "预约到具体班次"的高级 GUI（在表达式里建槽、领槽）留待按需评估；槽状态变量的求值上下文需纳入 P3 验证 |
| A2 条件表达式筛选（**v2 采纳**） | 载体"装载道路载具"订单带一组**条件子句**（变量+比较器+值，多子句=AND）；到站对等待队列 FIFO 扫描，**以每台候选 RV 为求值作用域**计算表达式：命中才装，**不命中即跳过**（留在队列等后续班次），语义等同条件订单"不满足就跳过" | 可行性高：**求值函数、比较器、变量枚举、GUI 编辑器全复用条件订单现成体系**（`OrderConditionVariable` order_type.h:217-243 + 求值 switch + order_gui 条件编辑），只需补 4 类小改动（见下） | 玩家可组合出 MD 想要的"若列车下一站/极速/长度/…是 X 则被此列车装载"式规则；默认预设"目的站匹配"子句防装错；无表达式的订单=不过滤装全部（宽松语义由玩家自行收紧） | 表达式存站订单参数块需入档（XSLF 门控）；求值作用域要参数化（见下） |
| A3 道路车选载体（RV 主动"登车"） | RV 声明"我要去 B 站"，载体到站广播，RV 与载体互相确认 | 可行但反模式 | 语义精确（我只坐去 B 的车） | RV 需要停在路边等"车来了再判断"，现场管理复杂；多车同时确认易竞态；与"火车是主体"的既有游戏心智冲突 |
| A4 显式配对（订单 ID 关联） | RV 排程里直接写"坐 3 号列车" | 技术上最容易，体验最差 | 完全确定 | 玩家要手工维护配对，班次一改就全断；不推荐 |

**推荐（v2 定稿）：A2 条件表达式筛选（slot 不设独立路线，见 A1 行）。回答评审问题——"游戏里是否已有支持"：是的，直接有。**

- **现成体系就是条件订单（conditional orders）**：`OrderConditionVariable` 变量族（order_type.h:217-243）、比较器、取值求值 switch、以及 order_gui 里的条件编辑器都是现成的，且条件订单对道路车等所有车型都开放。玩家在订单窗里"加条件→选变量→选比较符→填值"就是我们要复用的"逻辑式"交互。
- **语义＝跳过**：与条件订单"满足则跳转/不满足则继续"的惯用法对齐——本特性的求值发生在**每台候选 RV** 上：满足 → 装运；不满足 → 跳过该车（留在站内队列，等后续满足条件的班次）。MD 原文"若列车下一个调度计划/长度/最高速度/xxx 是 xxx 则被此列车装载"可直接表达为对候选求值的子句；若想表达"只坐去 B 站的船"，则用预设子句"候选 RV 的卸车目标站 == 本载体本站之后的下一停靠站"。
- **表达式放哪、默认值**：放在载体"装载道路载具"站订单的参数块（P2 路线乙，OrderExtraInfo），一个"条件集"＝若干子句（AND），0 子句 = 不过滤（装任意）。**默认自动插入一条"目的站匹配"预设子句**（作为普通可删子句，而非引擎写死的隐式规则），从机制上消灭"被运到错站"，又保留玩家自定义空间。
- **求值作用域（本特性唯一需要的求值侧改动）**：既有条件求值默认以"拥有该订单的车辆"为作用域；本特性需要以**另一辆车（候选 RV）**为作用域去读载体订单的条件——把求值入口抽成"以 `const Vehicle *subject` 为入参"（小重构，对既有条件订单调用零行为影响），再为 RV 场景注册少量新变量：候选 RV 的"被卸载"目标站、Bus/Truck 类别、极速、长度、载货率、是否铰接、引擎/组等。注：**slot 类变量读的是共享槽状态、不随作用域车变化**，参数化求值读它们不受影响；但 OT_SLOT 的"领取/释放"仍是订单原语、有各自执行时点，其与筛选的配合需进 P3 验证清单。
- **GUI**：直接复用条件订单编辑器做"装载筛选条件"面板（变量下拉/比较器/值控件），另给预设按钮（目的站匹配/仅卡车/仅巴士/长度上限…）；**下车方向**继续复用 JGRPP 既有 `MOF_RV_TRAVEL_DIR`/`Order::SetRoadVehTravelDirection` 通道（order_cmd.cpp:2711，该字段本就随订单入档），不新增字段。
- **职责分层**：载体侧一次配置"我要什么样的车"（表达式）；RV 侧不写表达式，只隐含携带"我要被送到站 X"（=其排程中下一"被卸载"订单指向的站，天然存在）。两层都显式，互不歧义。
- **空等与节奏**：扫描完 0 命中时，载体按订单装载旗标（是否等待满载/是否必装 RV）决定空等或离站；扫描放进 `LoadUnloadVehicle` 扩展分支、与普通货物装载同一站 tick 节拍内做（FIFO 纪律不破坏），保证联机确定性。
- **与 A1/A3/A4 关系**：A1 的 slot 诉求**已被 A2 吸收为条件变量维度**（`SlotOccupancy/VehicleInSlot/VehicleInSlotGroup` 子句 + `OT_SLOT` TryAcquire/Release 执行端），不再单列机制——多载体抢车或"预约特定班次"等高级用法，后续在筛选子句里编排 OT_SLOT 实现、届时补 GUI 即可（详见上表 A1 行）；A3/A4 维持不推荐（反模式）。
- 此决策把 Q1 从"运行时规则"降维成"订单编辑时的表达式数据 + 装货循环里一段对候选的筛选"，**与 R3R"订单即契约"及 pulsexlb"订单即状态机"的支柱一致**。

### Q2 "被运载的 RV 放哪"：虚拟车库 vs 销毁重建 vs 保留挂起

**问题本质**：车辆必须离开路网随载体走；OpenTTD 里常规车辆总在某个 tile 上，需要决定"不在路网期间"对象怎么活、怎么进存档、怎么从 UI 看到。

**候选方案**：

| 方案 | 描述 | 可行性 | 优点 | 缺点/风险 |
|---|---|---|---|---|
| B1 销毁+快照重生 | 上车时把 RV 整对象删掉，仅存"引擎ID+订单+货物+统计"快照数据，下车时重建 | 技术上可行（R3R artic 打散"整列快照→均分覆写"用过类似） | 最干净：车辆池没有"幽灵对象" | ① cargo packets 属于 pool 对象，快照重建要整段复制/迁移，极易泄漏或双计；② 单位号、组、livery 引用要重建；③ RV 年龄/可靠性/利润归零或需手抄，NewGRF 随机变量(车号等)失真；④ 重建时机与站点格位绑定，出错=丢车。**不推荐作主方案** |
| B2 官方先例"GVSF_VIRTUAL 式豁免 + 新角色位 TRANSPORTED"（本体保留、退出一切系统） | 定义新 ground-vehicle 子类型位（如 subtype bit7 或 vehicle_flags 位）"被运载中"，让 RV 保留在车辆池、不 tick、不绘制、不计成本、不进列表，从地图语义中退出 | **可行且推荐**：引擎已为 GVSF_VIRTUAL 打了 40+ 处排除点，等于**现成排雷清单**；车辆一切属性天然保留 | 存档只需新增 1–2 个字段（角色位+宿主引用）；恢复=清位+落地 | 需要把全库遍历点补齐（仿 GVSF_VIRTUAL 逐个核：经济、列表、崩溃、车站计数、新闻、自动替换、公司统计、AI 视图……），有遗漏才会出 bug；需保证"被运载中"的 RV 不会被 AutoReplace/克隆/灾难/卖出等抓到 |
| B3 每台 RV 停在某个"虚拟车库 tile" | 造不可见车库建筑/停在不存在的 tile | **不可行/不推荐** | —— | 地图上必须存在该 tile（OpenTTD 无"无格车辆空间"概念）；要么占地图边缘格（被截图/统计污染、被拆除），要么造"假建筑"需要新 tiletype，改动爆炸；且虚拟车库的买卖/改装入口还得逐一禁用，正是 MD 担心的"与车库根本功能矛盾" |
| B4 现库房实现（depot 车辆） | 把 RV 放进某公司某个**真实存在的公路车库**里 | 不可行 | 零新存储 | 玩家可见/可操作、占真实库位、可能被玩家拖走卖掉/改装 → 语义错乱 |

**针对 MD 顾虑的回答**：

1. **改动稳定性**：B2 的改动面是"**新增一个状态 + 补齐 ~20–40 处豁免点 + 存档 1–2 字段**"，全部是"加判断"而非"改既有行为"；参照 GVSF_VIRTUAL 的既有模式逐项对照即可控。真正要小心的是**遍历点清单要全**（本规划第 5 章 P4 会列检查表），而不是机制本身。
2. **"车库面板"**：不要做成真车库。做一个 **`被运载道路载具` 列表窗口**（独立子窗口，由 VehicleView 按钮打开，见 P5）：从"全公司 vehicle 池"按 `TRANSPORTED` 位过滤出当前被运载 RV，可浏览每台的状态/宿主/目的站；火车可在其详情窗追加"运载"tab，其余车型在独立窗口展示。**窗口只读（不能买/卖/改装/维护）**——这不和任何既有功能冲突，因为它根本不是车库：它的数据源是车辆列表过滤，无需在库格上登记。
3. **不会和游戏底层"车库"功能矛盾**：B2 下"被运载 RV"**不进任何车库的车辆列表**（仿 vehiclelist.cpp:149 的排除），车库照常运作；收运/落地都在**站点装卸代码**里完成，不经过 CmdStartStopVehicle/CmdMoveRoadVehicle 等车库命令，所以不存在"车库里的车被错误操作"的入口。
4. **兜底**：若落地失败（无格位），RV 保持 TRANSPORTED 直到成功，绝不落地成"半状态"。

### Q3 装/卸的物理呈现与各种"放不下"怎么办

**子问题拆解**：a) 装/卸表现方式；b) 站没有公路设施；c) 站格全被占用；d) 站台/格位长度不够、铰接 RV 太长；e) 可否借鉴"直通式"（车过车库）与车库绑定。

**结论先行**：

- **装卸锚点 = 站内同 Station 的公路停靠站**。表现方式用"**站台格内动画消失/出现**"，不把 RV 真的开上铁轨/上船坞。装卸本身是一个"节拍"：站 B 放车 = 在空闲公路停靠站格生成 RV（该格必须是 RV 对应类型 Bus/Truck、方向朝外、无人占用）。
- **为什么要公路停靠站而不是"任意出现"**：① 出现/消失都需要一个**合法、可碰撞、有方向的车辆格**，公路停靠站格就是现成的合法格；② 复用 Bus/Truck 分流语义（RV 当前载 Passenger → Bus 站）；③ 天然解决"从哪条路进/出"的画面一致性问题（车从站格驶出即可继续排程）。**纯"任意路边消失/出现"会造成车辆出现位置与排程下一站矛盾、且要额外处理路边临停合法性，不推荐作主方案**（可作为"无连体公路站时禁用"的例外被否掉）。
- 装卸瞬间的处理：收运 = RV 在公路站完成停定后进入"等待被运载"，载体停靠时按序抽取并**在同一 tick 内**原子完成"RV 离开路网（挂起）+ 载体登记 + 容量扣除 + 站格释放（释放所占用 bay/Entry 记账）"；卸货相反。整个过程在 `LoadUnloadVehicle` 的一个扩展分支里做，保证帧同步确定。
- 关于"等待位"：等待被运载的 RV 必须先停进某个公路停靠站格（否则没有合法停驻位）。港湾格入口被 entrance busy 串行化且**铰接车被 `RoadStop::Enter` 直接拒入**（roadstop.cpp:318），吞吐低；**直通式多格连续停靠站是"等待 + 装卸"共同推荐设施**（每方向 Entry 按累计车长记账，可同时驻留多台）。若玩家把等待 RV 停在普通站格上，可仅作"等待动画"，上车时仍以站台队列为单位整批抽取。

**具体子问题答案**：

| MD 子问题 | 处理 | 依据/风险 |
|---|---|---|
| b) 目的站无公路停靠站/连体站 | 该站不能作为装卸站：订单/新闻提示"目的地无可用公路停靠站"；载体到站后**不卸**（RV 继续随车）或按 NoUnload 语义跳过 | 与"货物卸不下时 Keep/滞留"语义一致；绝不产生"无格车辆" |
| c) 公路站所有格被占用（含排队） | 与 b 相同走"**滞留重试**"：每 tick 或每若干 tick 检查是否有释放；港湾格判定 = "Bay0/Bay1 有空位 **且** entrance 不被占"（进场串行化）；直通格判定 = "该方向 `Entry.occupied + 待放车长 ≤ Entry.length`"（roadstop.cpp:375-461，跨连续段共享账本） | 港湾吞吐低时建议 UI 提示玩家用直通式/多格站 |
| d) 长度/铰接放不下 | 精确判定式：整台 RV（铰接整组按 `gcache.cached_total_length` 计）长度 ≤ 该方向 Entry 可用长度 `Entry.length − Entry.occupied`（可用部分需连续）；不满足则滞留；**铰接 RV 被 `RoadStop::Enter` 拒入港湾站（roadstop.cpp:318），其等待与上下车都只能走直通式停靠站**（恰好与"直通式=装卸坡道"的推荐一致） | 判定数据在 roadstop_base.h 的 `Entry{length,occupied}`，零新增存储 |
| e) 借鉴"车过车库/直通车辆段" | 可以：**把"直通式公路停靠站"作为推荐装卸坡道**（OpenTTD 14+ 道路车库/路站直通概念已成熟）；"进入车库再绑定附近车站"的虚拟连接**不引入**（避免造 tile/绑定复杂度），用"同 Station 实体"这一既有概念代替"绑定" | 同 Station 共享货物已内建，等于免费获得"绑定" |
| a) 装卸动画/表现（v2） | **统一复用载具自带默认装卸动画，不做自定义动画**：收/放 RV = "RV 从公路站格消失/出现"两个逻辑瞬间 + 既有默认装卸观感（见 §1.3/D11）；运载清单用运载面板的文本罗列（见 P5） | 无新美术资源；稳定性与普通装卸一致 |

**MD 提到"会不会不稳定"的答复**：车辆"在站台格消失/出现"若实现为"合法公路停靠站格内的整格占位/释放"，不会不稳定；v2 又已取消任何自定义动画（画面 = 引擎默认装卸观感，由 `load_unload_ticks` 与既有装卸状态串驱动），表现层需要保证的只剩两件事：收/放瞬间的格位占用与释放正确、运载面板数据与实体一致。真正的稳定性风险仍在**对象生命周期**（Q2 已解）与**容量记账**（Q4 已解），不在表现层。

**"目的地站被拆/永远无法卸下"的兜底设计（补充规则）**：若载体携带的 RV 其卸载站被拆毁或路线被改导致永不触达，RV 将一直保持"被运载"状态（绝不静默丢失）。给玩家的出口：① 载体侧订单提供"**灵活卸载**"选项（在该载体任何一次停靠的、具备匹配路站的站均可卸下——由玩家明确勾选，避免默认乱卸）；② "被运载面板"提供只读状态 + 新闻/告警提示"某 RV 已随车超过 N 天未卸载"；③ 二期再加"赔偿放弃"（按车辆残值扣款并移除）。初期至少保证 ① + 提示，防止死档。

### Q4 载体货物状态/重量/容量怎么算：虚拟货物方案评估

**问题本质**：被运 RV 不是"普通货"，还自带货物；载体容量、重量、利润、按节/按隔舱如何计算；"装不下/剩余空间"如何处理。

**现状锚点**：装货是"逐节按节 `cargo_cap`（单位数）扣容量"（economy.cpp `LoadUnloadVehicle` :1944 起逐节循环）；**单 Vehicle 只装一种货**，多货=链上多个 Vehicle 各一种货（火车/道路车铰接节、JGRPP 多节船每节独立 Ship（XSLFI_MULTI_CARGO_SHIPS + GRF multi_part_ships 门控）、飞机 mail 是链上第二个 Aircraft 部件且仅当主舱为乘客货时独立成舱）；"一单位货"的物理含义由 CargoSpec 决定（重量=1/16t×weight 整数，cargotype.h:81），节容量=单位数而非吨；重量=车重+货重、装卸后经 MarkDirty→CargoChanged 被动重算，且只对火车/公路车存在；现有货物卸不下时走 Keep/Transfer 兜底（economy.cpp:2110-2134 等）。

**候选方案**：

| 方案 | 描述 | 可行性 | 评价 |
|---|---|---|---|
| C1 真"虚拟货物"（注册一个伪 CargoType，RV 折算成若干单位，进 `st->goods` 与 `v->cargo`） | 把 RV 当作货装进普通 cargo 通道 | 表面可行 | **不推荐**：① 单位无身份，无法挂载 RV 本体/订单；② 货量是整数、单位重量固定，RV 重 2.3 吨与 20 吨都得换算，精度差；③ 货物统计、评级、linkgraph 会被污染；④ 站台"货物已满不显示车辆"，体验差。MD 担心"破坏稳定性"成立 |
| C2 专用运载记录 + 虚拟"运载单位"只在**容量扣减/重量**层面换算（不进 cargo 系统） | RV 上车 = 在宿主节的 `transported_rvs` 记录一条，同时把 `cargo_cap` 语义替换为"运载容量"：`节运载容量单位数 -= 该 RV 折算单位`；总重缓存 += RV 全重 | **推荐** | 货物系统零污染；重量、超重判定自然成立；卸载时归还单位。缺点是要在容量/重量热路径加一个分量（改动点集中：编组缓存求值 + 容量显示） |
| C3 直接把"整台 RV 重量"当"一单位货"，占用容量按 1:1 | 太粗暴 | 可行但体验差 | 一台 40 吨卡车与一台 2 吨面包车占一样多"货位"，不合理 |

**具体问题答案（在 C2 前提下）**：

1. **载体货物状态如何呈现**：火车/船/机的"载货"视图（火车详情窗 Cargo tab / 其它车型详情文本区）保留给普通货物；运载 RV 单独列在"运载面板"（独立窗口/火车附加 tab，见 P5）；**不伪装成货**，所以不存在"无法常规装卸"的假货。若玩家把 cargo_type 显示换成运载 RV 的模式，只是 UI 视图，不是数据。
2. **重量**：**重量物理只存在于火车与公路车**（船/飞机引擎无 weight 字段、只有航程限制，SA2：engine_type.h 无 Ship/Aircraft weight），因此：
   - 对火车/公路车：载体编组缓存 `gcache.cached_weight` 在装卸后经 `MarkDirty → GroundVehicle::CargoChanged()`（ground_vehicle.cpp:103-145）被动重算；在该函数逐节累加处插入"该节被运 RV 总重（整备重 + 其自身载货重，后者可设开关）"即可，坡度阻力/质心/加速/制动/分摊费自动生效。
   - 对船/飞机：无重量模型，只做容量与显示层处理（不产生"超重"物理效应）。
   - 超重判定：**引擎没有现成"按重量拒绝装载"逻辑**（全仓无 overload/payload 限容，SA2），本特性的"这台 RV 装不下就拒装、留在站台并提示"由运载容量判定承担（见第 3 点），是新增的玩家体验逻辑，不是物理硬约束。
   - **不需要虚构"载重上限字段"**：用容量判定即可，重量只影响性能与显示（真实感）。
3. **铰接车厢/多节判定**：**按节判定**（推荐）：先把该节"运载容量（吨）"定下来——`节 cargo_cap ×（其当前 oversized 货物每单位重量）`换算成吨（`CargoSpec::weight` 是"每 1/16 吨/单位"的整数，cargotype.h:81，吨数 = `cargo_cap × weight / 16`；节容量本就是"单位数"而非吨，必须换算），RV 按**整备总重（吨）**占用之；一节装不下（不管铰接与否）就不装；若一列车的多节都是 oversized，火车会自然"逐节装满"。这同时回答"每部分单独判定好还是取总载重好"：**用节的容量判定装载、用整列重量判定可行性与性能**，两者不矛盾。多隔舱船/飞机同理：只有"当前装 oversized 货物的货舱"能接 RV，与普通货物互不抢占其他舱。
4. **剩余空间装其他货**：一节之内**禁止混装**（初期）：运载单位扣在节的"运载容量"上后，该节剩余运载容量不能再装普通货（语义清晰、避免节内两种计量混算）；**同列其他节照常装普通货**（天然支持"3 节平板装卡车 + 2 节棚车装货物"）。二期可做"剩余空间折算回普通 cargo 单位"（节内余量换算），但**不建议**：换算会让玩家困惑且改动 LoadUnloadVehicle 主循环。
5. **"虚拟货物会不会破坏稳定性"**：C2 不注册伪 cargo、不进 cargo 系统，所以货物统计/评级/linkgraph/cargodist **零影响**，稳定性风险集中在 3 个新字段的存档与遍历豁免上（Q2 方案），可测可控。
6. **其他取巧**：不加伪货物、不加伪订单 flag 而用"整列 refit 到'汽车运输'专用 cargo"的方案**不可行**：专用 cargo 会出现在 NewGRF refit 列表、占用 cargo 编号、且仍无身份，等于 C1 换皮。

**收运期间 RV 自身货物的处理（含货流图/统计呈现，评审澄清）**：

1. **货物的归属与可见性**：RV 被运载期间其 cargo list 随本体冻结随行，**从不进入任何站点 goods 槽**，也不被载体当作可装卸物（载体到站 B 卸的是 RV 而不是 RV 里的货；RV 落地后到 C 站自己卸货）——用"RV 处于 TRANSPORTED 时其 cargo 不可被外部访问"锁住即可。
2. **货流图/链路图呈现**：因为货物不经站台槽，**中间"被运载段"不会在客货流图上产生任何 flow 边**（Link graph、车站来源-去向、CargoDist 均不记账）；图上这批货表现为"源头站 → RV 最终卸货站"（加上 RV 自身公路段的正常 edges）。送达与公司月度统计照常只记一次——这是**本设计的特性而非缺陷**：要让中间段入图，就必须让货在起运站以 transfer 落槽再由载体装走，那与"车随货整体走"是互斥玩法。载体侧的"被运载公里数"是独立账本（D6），不写进货物 flow。
3. **在途"时间/里程"口径（D12，默认冻结）**：TRANSPORTED 的 RV 不 tick、不移动，其货物的老化（AgeCargo）与行进距离（travelled）不会自动累计。三种口径：
   - **A 完全冻结（默认）**：age/travelled 都不计。免费内部转运下无收入失真问题，实现最简、最安全；
   - **B 只计时间**：由宿主周期驱动被运 RV 货物 AgeCargo——若 travelled 不涨，付款时间衰减偏大、收入反而偏低；
   - **C 时间+里程联动累进（"真联运"）**：宿主每移动一段，把行进距离/时间同步累加到被运 RV 的 cargo packet——付款口径与"货真留在车上跑完被运段"一致，最真实，但需在载体移动热路径为被运 RV 的 cargo 做联动累进（含 travelled 与存档），工程代价中等。
   - 默认做 **A**；P7 若引入收费运价再评估 B/C（口径 C 需联动实现，涉及跨宿主记账，放入 P7 验证清单）。

---

## 5. 开发流程拆解：P0–P8 里程碑

> 通用约定：每个里程碑以"可运行 + 回归不炸"收口；分支从 `OpenTTD-patches`(jgrpp-0.73.1) 拉出，建议名 `feature/ro-ro-road-vehicles`；每步先补回归测试点（`regression/` 目录、同步测试 sync test），全部改动带确定性的纯整数运算。
>
> **追加里程碑（施工中与评审来回产生，超出本表 P0–P8）**：M9 铰接（多节）道路载具运载、M10 条件选择运载（参数化筛选）、M10b/c 设置窗口 GUI、M10d"路签"判据（并移除指代不清的"声明目的地"）、M11/M11b 实测问题与等待状态修复、M11c 重量记账（被运载车辆计入载体自重）与"满载"外观、载体详情窗口的载运清单、M11d"哪节可以装车"三档设置（修订 D1）、M11e 每单装载上限与两处订单命令修复。设计依据与验证记录见《实现规格书》§11 与附录 D。

### P0 玩法规格冻结（0.5–1 周，纯文档+原型演示）
- 产出：本文档第 1.4 决策表全部打勾；用现有 0.73.1 或 decouple fork 跑一张测试地图，人工确认"一个站内公路停靠站+铁路站台、Bus/Truck 分类、直通式路站行为"符合假设（§2.2）。
- 验收：D1–D10 定稿；测试地图与测试车队（含 1 台铰接 RV）就位。

### P1 存档与数据骨架（0.5–1 周）
- 新增字段与结构（§3.2）并完成最小入档：
  - `Vehicle`（RV 侧）新状态：**不要只往 `vehicle_flags` 塞位**（其存档宽度仅 16bit，R3R 已踩过 bit23-25 截断坑，sl/vehicle_sl.cpp:151）；推荐 **subtype 未用位** 或 **新增 XSLF 门控字节** `transported_flags`；宿主引用 `transported_by`（SLE_REF REF_VEHICLE + Ptrs 回填，仿 VEHS 既有引用字段）；
  - `Vehicle`（载体节）`transported_rvs`（按 SLE_REFLIST/REF_VEHICLE 门控入档）与节缓存分量（重量、占位）；Front 缓存 Σ；
  - `Station` 新"待运队列"字段（仿既有 struct-list 的 station chunk 表格式）；
  - 订单参数（见 P2）走**新字节/`OrderExtraInfo` 懒分配**（R3R 与 pulsexlb 同套路），**绝不改既有订单字段位宽**（pulsexlb 把 Order type 8→16 位且无 compat 双档，旧档强类型校验有直接判 corrupt 风险，SA4 存疑项）；
  - 版本策略：**纯 XSLF 路线（推荐，pulsexlb 先例）**——`SAVEGAME_VERSION` 不动（0.73.1 钉在 292，sl/saveload_common.h:465），新增一个恒写 XSLF 特征（如 `XSLFI_ROAD_VEHICLE_TRANSPORT`）＋独立"每车侧表"chunk（仿 'VESR' 模式，sl/vehicle_sl.cpp:1893）则需在 SLXI 的 chunk_list 登记；老档迁移用 `IsSavegameVersionBefore && SlXvIsFeatureMissing` 句式（afterload.cpp:666）。若坚持走 R3R 的 bump 路线则参照其 §5.6 教训：**先做"自读自档"回归**，避免 367 那种自己存的档自己读不回。
- 涉及文件：`sl/vehicle_sl.cpp`（VEHS 表 `_common_veh_desc` 1021 / 类型表 1365 / AfterLoadVehiclesPhase1 285）、`sl/station_sl.cpp`、`sl/order_sl.cpp`、`sl/saveload_common.h`、`sl/extended_ver_sl.*`（XSLFI 索引 + SLXI 子块表）、`saveload/afterload.cpp`、`vehicle_base.h/.cpp`、`station_base.h`、`order_base.h`。
- 验收：空档建→存→读一致；JGRPP 0.73.1 旧档可读且新字段取默认；**自读自档回归**；被运载 RV 的"载体↔RV"跨对象引用在 Ptrs/AfterLoad 阶段重建成功。

### P2 订单与状态机（核心，2–3 周）
- 订单（两种既有实现路线供选，定稿在 P0/P2 评审）：
  - **路线甲＝独立新订单类型**（R3R 式：尾部追加，基线 0.73.1 的 OrderType 止于 OT_SLOT_GROUP=14，新类型从 15 起；注意 `OrderTypeMask` 基线上是 uint16、位宽需评估，且新增 OrderType 会同步影响订单下拉/存档/读档的既有假设）；
  - **路线乙＝"站订单开关 + 参数块"（推荐；2026-09-10 外部评审处置 #4 定稿）**：载体到站订单本体加 RV 装卸开关，参数（筛选条件表达式（§4.1）/数量上限/是否等待/是否允许目的地不匹配/下车方向）以 `OrderExtraInfo` **挂在该站订单本身上（参数块）**，不新增 OrderType、不插入任何"参数行/元订单"——避开 ProcessOrders 跳转/删除级联/跳过执行那套特判，副作用集中在装货循环扩展分支与 GUI 编辑里。细节以《实现规格书》§4.1 与附录 C#4 为准。
  - RV 侧"等待被运载"需要显式可匹配状态：建议**新旗标/新状态位 + 订单参数（OrderExtraInfo）**表达（等待目标站隐含=下一"被卸载"订单站），并与 `OT_WAITING`/unbunch 等待语义区分开（状态串照 pulsexlb "Waiting for locomotive" 样式新增，如 "Waiting to be transported"）。
  - GUI：订单窗口按路线乙加开关按钮 + **筛选条件编辑器（直接复用条件订单的变量/比较器/值控件，§4.1）** + 预设子句按钮（目的站匹配/仅卡车/仅巴士…）；语言串 `lang/english.txt`+中文。
- 状态机：
  - RV：`等待被运载`（在公路站停定→注册进站待运队列→不再进行普通装卸）；`被运载中(TRANSPORTED)`（不 tick/不绘制/不进列表/不计成本/退出 tile 哈希）；`落地`（恢复→清待运登记→按排程续跑）。
  - 载体：进站后按 §4.1 表达式对队列逐台求值（命中装运、不命中跳过）→ 原子收运；到卸货站按 §4.3 格位规则原子放车。
  - 与既有流程的接线点：RV `RoadVehController`→到站（roadveh_cmd.cpp 到站分支）、载体 `economy.cpp LoadUnloadVehicle` 的装载主循环扩展分支（若 cargo_type 属 Oversized 且订单带 RV 旗标 → 走运载分支）、`ProcessOrders`/`BeginLoading` 语义特判（防收运中 RV 触发普通装卸，参照 WAIT_COUPLE 特判先例）；载体收/放 RV 的"链手术"统一走"备份→试算→校验→回滚"（pulsexlb 模式 3），备份从物理链头开始。
- 验收：P0 测试地图上打通**最小可玩闭环**（3.4 节场景）；收/放画面复用默认装卸动画无异常、运载详情可见。

### P3 站点队列、匹配表达式与容量记账（1–2 周）
- 站点"待运队列"注册/注销/遍历；**按载体订单携带的条件表达式对候选逐台求值（§4.1：命中→装，不命中→跳过留在队列）**；队列显示在站窗口（可选）；容量/重量分量接入编组缓存与显示；超重拒绝与新闻。
- 求值侧小重构：把条件求值入口抽成"以 `const Vehicle *subject` 为入参"（供以候选 RV 为作用域），注册 RV 侧新变量（目的站/Bus-Truck/极速/长度/载货率/铰接/引擎组）。
- 重量接入的**唯一热路径点**：`GroundVehicle::CargoChanged()`（ground_vehicle.cpp:103-145）逐节 `current_weight = u->GetCargoWeight()` 后累加 `gcache.cached_weight`，坡度阻力/质心/功率随其后自动更新——在循环里加 `current_weight += u->GetTransportedWeight()`（单节被运 RV 总重）即可；装卸完成路径对 front 调 `CargoChanged()`/`ConsistChanged()` 刷新（对火车同时走 ConsistChanged 里的相关汇总）。
- 涉及：`station.cpp/.h`、`roadstop.cpp`、`economy.cpp`、`order_cmd.cpp`（条件求值参数化 + 新变量）、`vehicle.cpp`(缓存/重量求值)、`ground_vehicle.cpp`（CargoChanged）、`vehicle_cmd.cpp`(refit 容量路径) 及 `ship_cmd.cpp`/`aircraft_cmd.cpp` 的容量显示点。
- 验收：同一站 5 台 RV + 3 趟车（含"带筛选表达式/不带表达式"两类订单）反复装卸 30 分钟无错乱；表达式不匹配的车确认留在队列等后续班次；重量变化反映到列车动力表现（含上坡极速）；货与 RV 混列正常。

### P4 TRANSPORTED 生命周期与全库豁免点补全（1–2 周，风险最高）
- **离图/回图的一致性（本里程碑核心）**：收运 = 从 `Vehicle::tile/dest_tile/x/y/z` 语义与 **tile→车哈希（UpdateVehicleTileHash vehicle.cpp:846，VIRTUAL 分支 :851 就是现成"不进哈希"写法）**中退出、置 Hidden+Stopped、不参与 `VehiclesOnTile`（因此自然从车库窗/闭塞/寻路/站台占用消失）、不绘制（IsDrawn vehicle.cpp:423）；落地 = 逆向重挂哈希并校验格位。**除 GVSF_VIRTUAL 全家桶外没有其它现成无 tile 先例**——凡遍历 `v->tile`、`VehiclesOnTile`、`IsInDepot`、视口/公司统计处都要照 SA3 Q4/Q8 清单核一遍。
- 仿 GVSF_VIRTUAL 排雷清单，逐点给"被运载 RV"加豁免（检查表至少含）：经济/折旧/维护（economy.cpp 系列）、车辆列表与车库窗口（vehiclelist.cpp、vehicle_gui、roadveh_gui、depot_gui）、视图/绘制/视口脏区（vehicle.cpp:423/846 同款）、碰撞/灾难/新闻（disaster_vehicle.cpp）、组统计（group_cmd.cpp）、自动替换/自动更新（autoreplace_*）、克隆/复制、基建统计/公司/网络报表、AI 与脚本可见性（script_vehicle 等，参照 R3R 改 script_vehicle.cpp 的做法）、存档链完整性（vehicle_sl.cpp:299 同款一致性检查）、卖出/拆除防误操作（vehicle_cmd.cpp SellVehicleFlags 类似物）、基建共享计费、desync 校验（cachecheck）。
- 收运/落地事务化：先快照(orders_backup 风格 NOSAVE) → 变更 → 校验失败回滚；卸货格位分配器（优先直通连续格→港湾空位→滞留重试）。
- 涉及文件：以 vehicle.cpp/vehicle_cmd.cpp/roadveh_cmd.cpp 为核，扩散约 20–30 个文件。
- 验收：运载中的 RV 在联机 2 端各跑 1 小时 sync test 无 desync；崩溃/卖出/车库/替换等常规操作无法触碰它；落地后一切统计恢复如常。

### P5 GUI 完整化（1 周）
- UI 方案（基于 0.73.1 窗口实况，SA3）：**运载清单做成由 VehicleView 打开的独立子窗口**（类比 Orders/Timetable 子窗，`WindowClass::VehicleDetails` 之外新值或复用机制），数据源 = `Vehicle::IterateTypeFrontOnly` 过滤 TRANSPORTED 位，只读列"车型/数量/重量/目的站/宿主载体/滞留天数"，点击单项可开该车 VehicleView/Details；**不要给非火车详情窗加页签**（只有火车详情窗有 tab 条且带 static_assert 约束，vehicle_gui.h:26、vehicle_gui.cpp:2954）；可选：火车详情窗加第 6 个 tab（TrainDetailsWindowTabs+widget+DrawTrainDetails 分支）。
  - **M11c 实作（比上表更省）**：没有新开独立窗口，也没有加第 6 个 tab——火车沿用已有的**"车辆"页**（该页本来就有滚动条），在列表末尾追加"载运的道路载具"一段；船/机在**现有详情面板底部**追加同样的列表（`DrawCarriedRoadVehicles()`），面板高度随装载数自动增减（`VehicleDetailsWindow::GetVehDetailsHeight()` + 装卸后 `InvalidateWindowData(WindowClass::VehicleDetails, carrier)`）。这样完全没有碰"非火车详情窗不能加页签"的约束，也没有引入新的窗口类；需要精确清单的脚本化场景仍可用控制台 `rvtransport carried <载体ID>`。
- 站窗口显示"待运队列"（station_gui.cpp）；订单条件编辑面板（order_gui 开关/下拉/地图拾取，参照 pulsexlb 模式）；新增"状态串/新闻串/提示串"（en+简中，车种×4 文本可用 `###length VEHICLE_TYPES` 组）。
- 涉及：`vehicle_gui.cpp`(+`widgets/vehicle_widget.h`、`vehicle_gui.h`)、`roadveh_gui.cpp`、`station_gui.cpp`、`order_gui.cpp`、`depot_gui.cpp`（列表入口按钮）、`train_gui.cpp`（可选 tab）、`widgets/*`、`lang/english.txt`+中文。

### P6 视觉收尾与载具类型补全（0.5–1 周，v2：无动画工作量）
- 按 v2 定稿（§1.3/D11）：**不制作任何自定义装卸动画**；P6 只做：① 船（多节逐节判定+舱面逻辑）、飞机（货机 refit oversized 才可装；乘客机自动拒绝）全打通；② 验证收/放 RV 时复用默认装卸动画不出现观感/状态问题（停站帧、loading 状态串）；③ "运载面板"文本罗列跨 4 类载具一致（含数量/总重/目的站）。
- 可选延伸（默认不做）：被运 RV 的静态缩略贴图（数据驱动、无动画），独立设置开关控制；如需可单独评估。
- 涉及：`aircraft_cmd/ship_cmd`、`ship_gui/aircraft_gui/roadveh_gui/train_gui`（运载面板文本行）、`vehicle_gui.cpp`（运载面板窗口）。

### P7 经济、设置与 NewGRF/脚本兼容（1 周）
- 设置项：定义写入 `src/table/settings/game_settings.ini`（settingsgen 生成 table/settings.h，勿手改生成物），结构字段在 `src/settings_type.h`，老档名称映射在 settings_compat.h；涉及项：特性总开关、收运中 RV 货物是否随行计入重量、是否冻结折旧/运行成本、运价模式(免费/收费)、"被运载超过 N 天未卸"告警阈值。
- 经济：若做运价（二期，D6 备选），注意现有收入公式 = **单位数×距离×时间衰减×current_payment**（`GetTransportedGoodsIncome`，economy.cpp:1078），没有"按车辆价值计价"的现成通道——需自建价目表（如按整备重/车龄的每公里单价），且**不要把运价混入 CargoPayment 链路**（会污染公司利润表与 linkgraph）；统计新增"被运载公里数"即可。
- NewGRF：无必改项（零 cargo 注册）；仅需验证带"refit 回调容量"的车（如按 cargo 变化容量）收 RV 时容量换算走 `refit_cap` 正确；铰接 RV 在收运/落地瞬间的 sprite/变量一致性检查。
- AI/脚本：默认不主动使用；把新命令/状态对 `script/` 层隐藏或加只读查询（参照 R3R 只读思路）。
- 涉及：`settings_table.cpp`/`settings_type.h`、`economy.cpp`、`table/settings/*.ini`、`script/api/*`（按需）。

### P8 回归、联机、收尾（1 周+）
- 回归矩阵：旧档读取（JGRPP 0.73.1 档 / decouple fork 档若可共存）、自读自档、含 `gradual_loading`/`improved_load`/真实制动/模板替换/Cargodist 的组合、铰接 RV、多隔舱船、DT/港湾混用、站台被拆（RV 在途时目的站被拆：落地失败→滞留→提示拆站或随车到下一站）。
- 联机：2–4 端 + AI 对手长时间跑；sync test；desync 修复。
- 代码卫生：清理调试桩（R3R 教训）、补注释、去死代码、`english.txt`+`simplified_chinese.txt` 增量最小化；可考虑把 R3R_* 式备忘移到 docs/。
- 验收：发布候选 RC，写 CHANGELOG 条目与发布说明。

---

## 6. 风险与兼容性清单

| 风险 | 等级 | 缓解 |
|---|---|---|
| TRANSPORTED 豁免点漏网（被卖出/替换/计入成本/出现在列表） | 高 | P4 检查表对照 GVSF_VIRTUAL 40+ 点逐项测试；写"运载中不可操作"断言 |
| 收运/落地瞬间的事务回滚不足致丢车/双车 | 高 | orders_backup 式 NOSAVE 快照+校验回滚；崩溃点集中在 P4 |
| 存档版本/EXT 规则冲突（自存自读失败，R3R 367 教训） | 中 | P1 就做"自读自档 + 旧档升级"双回归 |
| 与 JGRPP 特性组合回归（真实制动重量、模板替换、Cargodist 把 RV 货物当普通货分流） | 中 | P3/P7 明确"RV 货不参与 linkgraph 期间性活动"并测 |
| 目的站被拆/改建导致滞留堆车 | 低 | 卸载失败→随车下一站/长期滞留+新闻；上限保护(设置) |
| 存档"两轴"兼容与命名表细节（主版本轴 vs upstream/XSLF 轴换算未逐行核实，SA3 标注） | 中 | 纯 XSLF 路线 + P1 自读自档/旧档升级/拒读单向兼容三回归；不重排既有字段 |
| 订单既有字段位宽被改动（pulsexlb Order type 8→16 无 compat 双档的旧档判 corrupt 风险）；若要与 decouple fork 合并则订单类型号冲突 | 中 | 参数块（OrderExtraInfo）路线避开新可执行类型；与 R3R/pulsexlb 的合并决策在 P0 冻结（基线用 0.73.1 原版即新类型从 15 起；若基于 decouple fork 则从 18 起） |
| "无 tile 车辆"存档往返未证实（GVSF_VIRTUAL 有存档一致性检查 vehicle_sl.cpp:299，但虚拟车实际是否会持久化未实测） | 中 | P1 专门做"收运中 RV 存→读→再落地"往返测试 + 触发一致性检查 |
| 收运中 RV 被间接清除路径（公司破产清盘、基建共享、脚本批量操作、克隆） | 中 | P4 豁免清单含破产/清盘/脚本视图；定义"宿主载体被售/被毁"时的清场规则（先强制落地或连 RV 一起处置） |
| 玩家滥用（免费跨洲运载刷距离/卡容量） | 低 | 容量/重量真实化 + 收费模式选项 |
| 规模失控（越做越大） | 中 | 严格 P0 决策冻结；P2/P4 用最小闭环收口后再扩类型 |

## 7. 参考资料

- 本地：`道路运输分支规划.md`（需求原文）、`Decouple-vs-JGRPP-0.73.1-对比分析.md`、`YPS-vs-JGRPP-对比报告.md`、`JGRPP-架构详解.md`、`NewGRF-原理详解.md`；代码：`OpenTTD-patches`(基线 0.73.1)、`OpenTTD-patches-decouple`(R3R 样板)、`OpenTTD-pulsexlb`。
- 本规划配套调研底稿（`.road-transport-analysis\`）：`sa1_roadveh_orders_stations.md`、`sa2_cargo_load_weight_income.md`、`sa3_gui_saveload_commands_settings.md`、`sa4_pulsexlb_decouple_patterns.md`（正文所有"文件:行号"均可在其中回溯）。
- 社区先例讨论（目前均为"货物制运车/请求"，无玩家车辆互载引擎实现）：
  - TT-Forums "Trains carrying cars"（运车货列玩法需求）https://www.tt-forums.net/viewtopic.php?p=1267481
  - TT-Forums "Trucks on Trains" https://gandalf.zernebok.com/viewtopic.php?p=214961
  - TT-Forums "Super Ferry OR Lorry Ferry"（渡轮运车）https://www.tt-forums.net/viewtopic.php?style=4&t=9204
  - TT-Forums "Road vehicle carriages" https://www.tt-forums.net/viewtopic.php?style=1&t=44799
  - 相关引擎机制出处：OpenTTD wiki Manual/Vehicles、repo.or.cz/openttd-jgr

---

## 附 A：文中关键代码位置（0.73.1 实测，行号可能随版本漂移，以函数名为准）

| 事实 | 位置 |
|---|---|
| CargoClass 枚举(Oversized=10) | `src/cargotype.h:51` |
| IsCargoInClass | `src/cargotype.h:245` |
| GVSF_* 子类型位（含 GVSF_VIRTUAL=6） | `src/vehicle_base.h:77-83` |
| GVSF_VIRTUAL 豁免点样例 | economy.cpp:159/248/541、vehiclelist.cpp:149/178、vehicle.cpp:423/851/1039、disaster_vehicle.cpp:592、infrastructure.cpp、group_cmd.cpp:142、vehicle_cmd.cpp:270/606、sl/vehicle_sl.cpp:299 |
| 装卸公共主链（本特性挂接点） | `BeginLoading` vehicle.cpp:3407 → `PrepareUnload` economy.cpp:1524（登记进 Station::loading_vehicles）→ `LoadUnloadStation` economy.cpp:2438（每站每 tick）→ `LoadUnloadVehicle` economy.cpp:1944 → `HandleLoading` vehicle.cpp:3797 / `LeaveStation` vehicle.cpp:3574 |
| 各类型进站汇聚点 | 火车 `TrainEnterStation` train_cmd.cpp:5056、船 `ShipController` ship_cmd.cpp:851、飞机 `AircraftEntersTerminal` aircraft_cmd.cpp:1505 |
| Station::goods / loading_vehicles 全站共享 | `src/station_base.h:923-924` |
| 港湾站 RoadStop：Bay0/Bay1 + entrance busy | `src/roadstop_base.h`、`src/roadstop.cpp` |
| 直通站按方向累计占用车长记账（跨多格连续段） | `src/roadstop.cpp:296-337/400-461` |
| RV Bus/Truck 判定 | `src/roadveh_cmd.cpp:91`（IsCargoInClass Passengers） |
| 车库进出 | `VehicleEnterTile_Road` road_cmd.cpp:2994、`VehicleEnterDepot` vehicle.cpp:2686、`RoadVehLeaveDepot` roadveh_cmd.cpp:1311、车库窗枚举 `BuildDepotVehicleList` vehiclelist.cpp:78 |
| 站点/路站/车库 TileType 不同，不能同格 | 见 `depot_map.h`/`station_map.h`/`road_map.h` 类型分布 |
| 铰接车 = GVSF_ARTICULATED_PART 独立 Vehicle 链；cargo/cargo_cap/refit_cap 每节独立；refit 更新 | `src/vehicle_base.h:357-359/1106-1127`、`src/vehicle_cmd.cpp:495-509/583-584` |
| 编组重量缓存（含货重） | `GroundVehicleCache::cached_weight` ground_vehicle.hpp:33；整列重量汇总 ground_vehicle.cpp（约 92–160）；单节 `GetWeight()=GetWeightWithoutCargo()+GetCargoWeight()` train.h:319/354/433、roadveh.h:276-287 |
| LoadUnloadVehicle 逐节装卸主循环 | `src/economy.cpp:1944` 起（逐节 cap/GetLoadAmount/装卸位/Keep 语义） |
| 多节船（artic parts）、飞机 mail compartment | ship_cmd.cpp:1132（AddArticulatedParts）、engine.cpp:256（mail capacity）、aircraft_cmd.cpp:293-378（mail 部件=第 2 个 Aircraft）；多货船存档特征 XSLFI_MULTI_CARGO_SHIPS `sl/extended_ver_sl.h:142`；jgrpp-changelog.md:657 |
| 订单结构/OrderExtraInfo/OrderType | `src/order_base.h`、`src/order_type.h`（0.73.1 止于 OT_SLOT_GROUP=14） |
| 条件订单变量/求值/编辑器（Q1 表达式方案复用） | `OrderConditionVariable` 枚举 order_type.h:217-243；求值 switch 在 order_cmd.cpp（新变量先例：pulsexlb DecouplePart 求值 order_cmd.cpp:4768-4771）；条件编辑器在 order_gui.cpp，所有车型可用 |
| 扩展存档 XSLFI / SLV 版本 | `src/sl/extended_ver_sl.*`、`src/sl/saveload_common.h`（SAVEGAME_VERSION=292 钉住，:465；加字段三件套例 sl/vehicle_sl.cpp:1124/461；侧表 chunk 例 'VESR' :1893） |
| 车辆窗口/tab 结构 | VehicleDetailsWindow vehicle_gui.cpp:3058、TrainDetailsWindowTabs vehicle_gui.h:26（仅火车 5 tab）、各型 DrawXxxDetails train_gui.cpp:429/roadveh_gui.cpp:30/ship_gui.cpp:65/aircraft_gui.cpp:30、DepotWindow depot_gui.cpp:273、列表窗 vehicle_gui.cpp:2383 |
| 车库/站列表枚举语义 | BuildDepotVehicleList vehiclelist.cpp:78（按 tile+IsInDepot）；VL_DEPOT_LIST vehiclelist.cpp:192（按订单目标）；GenerateVehicleSortList vehiclelist.cpp:139 |
| 车辆 tile/哈希/"无 tile 先例" | Vehicle::tile 默认 INVALID_TILE vehicle_base.h:253；UpdateVehicleTileHash vehicle.cpp:846（VIRTUAL 不进哈希 :851）；VehiclesOnTile 查哈希 vehicle_base.h:1772+；IsDrawn vehicle.cpp:423；命令链 command_type.h:492(Commands)/vehicle_cmd.h:27(DEF_CMD_TUPLE)/command_table.cpp:185 |
| refit/容量/计量细节（SA2） | 运行期只认 refit_mask：RefitVehicle vehicle_cmd.cpp:397(:427 refittable)；DetermineCapacity engine.cpp:236；每单位重量=CargoSpec::weight/16 吨 cargotype.h:81；收入=单位数×距离×时间×current_payment economy.cpp:1078；deliver/transfer/payment economy.cpp:1324/1454/1477；无任何按重量拒载逻辑 |
| 设置与字符串（SA3） | 设置定义 src/table/settings/*.ini（settingsgen→table/settings.h，settings_table.cpp:79）；结构 settings_type.h；字符串 src/lang/english.txt（strgen 自动编号；`###length VEHICLE_TYPES` 车种组） |

## 附 B：调研结论核销登记（随四路代码调研逐条核销）

> 调研底稿归档目录：`D:\CNS\ottd\.road-transport-analysis\`（每路子代理交付 saN_*.md 全文，正文引用与校订以此为准）。

- [x] RV 港湾/直通停靠站占位与排队（SA1，底稿 sa1_roadveh_orders_stations.md）：港湾=Bay0/Bay1+entrance busy、铰接车禁入港湾；直通=每方向按累计车长记账、跨多格连续段共享；无全局 bay 槽位
- [x] 装卸主链与各类型进站汇聚点（SA1）：BeginLoading/PrepareUnload/LoadUnloadStation/LoadUnloadVehicle/HandleLoading/LeaveStation；TrainEnterStation/ShipController/AircraftEntersTerminal（见附 A）
- [x] 全站共享 goods 与 loading_vehicles（SA1）：station_base.h:923-924
- [x] off-map 车辆先例范围（SA1）：仅 depot 内车辆（仍占图）与 TBTR 虚拟列车 GVSF_VIRTUAL（仅火车）；道路车无 off-map 存车先例
- [x] 编组"当前载货总重"入口（自查+SA2）：cached_weight 汇总在 ground_vehicle.cpp CargoChanged（104-145）；单节 GetWeight 语义 train.h:433 / roadveh.h:287；**重量仅火车/公路车存在**；装卸后经 MarkDirty（train_cmd.cpp:4993 / roadveh_cmd.cpp:438）触发
- [x] 多节船/飞机 mail compartment（自查+SA2）：多节船=独立 Ship 部件（XSLFI_MULTI_CARGO_SHIPS + GRF multi_part_ships）；飞机 mail=第 2 个 Aircraft 部件（aircraft_cmd.cpp:293-378）
- [x] 车辆窗口/tab/车库/列表/命令/设置/字符串/存档三件套（SA3，底稿 sa3_gui_saveload_commands_settings.md）：见附 A 新增行
- [x] cargo 单位/容量乘数/refit 回调容量/计费（SA2，底稿 sa2_cargo_load_weight_income.md）：容量=单位数；单位重量=weight/16 吨；refit 只认 refit_mask；收入按单位数计价；无按重量拒载逻辑
- [x] JGRPP 多货船/机 mail 等"多货种"机制（SA2）：单 Vehicle 单货；多货=链上多 Vehicle；见附 A
- [x] pulsexlb decouple 函数级实现模式与存档/GUI 细节（SA4，底稿 sa4_pulsexlb_decouple_patterns.md）：Primary/First 分离、XSLF-only 存档、认领协议、链手术模板、参数行订单、merge 备忘（见 §2.7 新增表）
- [x] SAVEGAME_VERSION/XSLF 精确机制与 R3R/pulsexlb 两种存档路线（SA3/SA4）：推荐纯 XSLF；不 bump；自读自档回归

**仍未实测、留待 P1/P8 验证的开放项（非调研缺口，是设计依赖的实测项）**：① 旧 JGRPP(0.73.1) 档载入新 fork 的行为（XSLF unknown-feature 拒读/忽略）；② 无 tile 车辆（GVSF_VIRTUAL 式）存档往返与 vehicle_sl.cpp:299 一致性检查对"被运载 RV"的适配；③ Order 表由位置式→命名表后字段增删的读写（SA3 称当前格式为命名表/RIFF）；④ 港湾双 bay 同停/直通同格单停的运行时行为（SA1 结构推断未实测）。
