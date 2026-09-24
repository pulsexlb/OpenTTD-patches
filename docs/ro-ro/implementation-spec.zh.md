# 道路运输分支 · 实现规格书（给实现 Agent 的唯一输入）

> 本文件是"道路载具互载（RoRo）"特性的**实现用规格**，由 4 份代码调研底稿（`D:\CNS\ottd\.road-transport-analysis\sa1..sa4`）与《道路运输分支-整体规划.md》（下称《规划》）蒸馏而成。**用法**：实现 agent 以此为准逐模块施工；凡要"再探索"的东西，本文件已尽量压缩成"改哪个文件、哪个函数、加什么字段"。论证与背景见《规划》，本文件不再重复论述，只给指令。
> 基线：`D:\CNS\ottd\OpenTTD-patches`（JGRPP 0.73.1，SAVEGAME_VERSION 钉在 292）。行号仅作锚点，**以函数名为准定位**（版本漂移）。

---

## 0. 使用纪律（先读，违反即返工）

1. **确定性**：一切新逻辑只用整数/固定顺序迭代；不引入未固定浮点、不依赖 hash 迭代序；所有状态变更发生在 DoCommand 或 tick 内。每条新路径必须能被 sync test 覆盖。
2. **存档单向兼容**：fork 新档不得承诺回读 0.73.1；0.73.1 旧档必须能读入并取新字段默认值。**任何存档改动先做"自读自档"回归**（R3R 367 自读失败教训）。
3. **侵入面隔离**：普通货物/普通列车/非本特性订单行为零改动。新逻辑全部走：新状态位 + 新旗标 + 专用判断入口 + 站订单 RV 参数块（OrderExtraInfo）。
4. **禁改清单（踩过/验证过的坑，禁止触碰）**：
   - 不改 `Order` 既有字段**位宽**（pulsexlb 把 type 8→16 且无 compat 双档 → 旧档有判 corrupt 风险）；新参数一律新字节/新字段/`OrderExtraInfo`。
   - 不把新状态塞进 `Vehicle::vehicle_flags` 的高位（其存档文件宽度 16bit，R3R 已踩 bit23-25 截断）。
   - 不注册伪 CargoType、不把 RV 塞进 `st->goods`/`VehicleCargoList`（无身份、统计污染）。
   - 不做自定义装卸动画、不建"虚拟车库 tile"、不销毁重建 RV。
   - 不把条件求值写成有副作用（只读）；`OT_SLOT` 领/放仍由订单原语在各自时点执行。
5. **人类评审 gate**：每个里程碑（M1..M8）收口 = "可编译 + 最小场景可玩 + 回归不炸"，随后交人类评审再进下一步。

---

## 1. 范围与决策基线（快照，详见《规划》§1.4 / §4）

| 项 | 定稿 |
|---|---|
| 装载门 | **三档设置 `vehicle.rv_transport_carrier_parts`（M11d 修订 D1，默认档 2）**：0=任何有载货容量的节；1=该节 `cargo_type` 属 `CargoClass::Oversized`（`IsCargoInClass`，cargotype.h:245）；2（默认）=**正面白名单**：`IsCargoInClass(Bulk)` ∥ `IsCargoInClass(Oversized)` ∥ `CargoSpec::label == 'VEHC'`。判定集中在 `RVTransportPartCanCarry()`，**只在装载时生效** |
| 容量/重量 | 节容量换算成吨（`cargo_cap × CargoSpec::weight / 16`），RV 按整备重占用；按节判定；初期节内不混装普通货 |
| 重量物理 | 仅火车/公路车；入口 `GroundVehicle::CargoChanged()` |
| 配对 | 载体主导 + 条件表达式筛选 + 不匹配跳过（复用条件订单体系） |
| slot | 不单列机制：`SlotOccupancy/VehicleInSlot` 等作表达式子句；`OT_SLOT` TryAcquire/Release 作执行原语 |
| 装卸点 | 站内同 Station 的对应类型公路停靠站；直通式为推荐装卸坡道 |
| 收起/放下 | RV 从公路站格消失/出现 + 默认装卸动画（无自定义动画） |
| 在途货物口径 | D12 默认完全冻结（age/travelled 不计） |
| 运价 | 免费（只统计"被运载公里数"）；收费为二期（P7 仅评估） |
| 电车 | 不支持 |

---

## 2. 术语/锚点速查（改代码前先认这些）

- 装卸公共主链：`Vehicle::BeginLoading`（vehicle.cpp:3407）→ `PrepareUnload`（economy.cpp:1524，push 进 `Station::loading_vehicles`）→ 站每 tick `LoadUnloadStation`（economy.cpp:2438，FIFO 逐个）→ `LoadUnloadVehicle`（economy.cpp:1944）→ `HandleLoading`（vehicle.cpp:3797）/`LeaveStation`（vehicle.cpp:3574）。载体进站汇聚点：`TrainEnterStation`（train_cmd.cpp:5056）、`ShipController`（ship_cmd.cpp:851）、`AircraftEntersTerminal`（aircraft_cmd.cpp:1505）。
- RV 到站：`RoadVehController`（roadveh_cmd.cpp:2140）→`ProcessOrders`→`HandleLoading`；停靠帧逻辑与 `RoadVehArrivesAt`+`BeginLoading` 在 roadveh_cmd.cpp 到站分支；Bus/Truck 判定 `IsCargoInClass(cargo_type, Passengers)`（roadveh_cmd.cpp:91）。
- 公路停靠站记账：`RoadStop`（roadstop_base.h）：港湾=Bay0/Bay1+entrance busy（铰接拒入，roadstop.cpp:318）；直通=每方向 `Entry{length,occupied}` 按累计车长记账（roadstop.cpp:375-461，跨连续段共享）。
- 条件订单：`OrderConditionVariable`（order_type.h:217-243）、求值 switch 在 order_cmd.cpp（pulsexlb 追加变量求值先例 order_cmd.cpp:4768-4771）、编辑器在 order_gui.cpp、`OT_SLOT/OT_COUNTER/OT_LABEL/OT_SLOT_GROUP`（order_type.h:96-110）。
- 离图车辆先例：`GVSF_VIRTUAL`（vehicle_base.h:83）+ 全套豁免点（economy.cpp:159/248/541、vehiclelist.cpp:149/178、vehicle.cpp:423/851、disaster_vehicle.cpp:592、infrastructure.cpp、group_cmd.cpp:142、vehicle_cmd.cpp:270/606、sl/vehicle_sl.cpp:299）；tile 哈希 `UpdateVehicleTileHash`（vehicle.cpp:846）。
- 存档：`SAVEGAME_VERSION`=292（sl/saveload_common.h:465）；XSLF 特征（sl/extended_ver_sl.h/.cpp，`XSLFI_*` + `SlXvFeatureTest`/`SlXvIsFeatureMissing`）；VEHS 表 `_common_veh_desc`（sl/vehicle_sl.cpp:1021）与类型表 :1365、`AfterLoadVehiclesPhase1/2`（sl/vehicle_sl.cpp:285/476）；独立"每车侧表"chunk 先例 'VESR'（sl/vehicle_sl.cpp:1893）。
- 命令：`Commands` 枚举 command_type.h:492（只能插 `End` 哨兵前）；`DEF_CMD_TUPLE` 于对应 `*_cmd.h`；表自动生成（command_table.cpp:185）。
- 设置：`src/table/settings/*.ini`（settingsgen→table/settings.h，勿手改生成物）+ `settings_type.h` 结构 + `settings_compat.h` 老档映射。
- 窗口：详情窗 `VehicleDetailsWindow`（vehicle_gui.cpp:3058）、火车详情内部 tab `TrainDetailsWindowTabs`（vehicle_gui.h:26）、各型绘制 `DrawTrainDetails/RoadVehDetails/ShipDetails/AircraftDetails`、depot 窗 `DepotWindow`（depot_gui.cpp:273）、列表 `BuildDepotVehicleList`（vehiclelist.cpp:78）、`VL_DEPOT_LIST` 按订单枚举（vehiclelist.cpp:192）。

---

## 3. M1 数据模型与存档骨架（对应 P1；先做，其余模块都依赖）

### 3.1 新增字段（全部经 XSLF 特征 `XSLFI_ROAD_VEH_TRANSPORT`（v1）门控，**不 bump SAVEGAME_VERSION**）

> **2026-09-10 实现简化（M1 开工定稿）**：不再使用"载体 Front `transported_rvs` vector + 节缓存"双写模型。评审 #1 的实质诉求是"节级可追溯"，用 **RV 侧标量字段**即可完全满足：被运 RV 自己记住"宿主 Front + 装载到哪一节 + 占多少吨 + 入队时刻"，载体端的"载了什么/每节用了多少"**按需反查**（过滤车辆池 / 过滤宿主节上的 RV）。好处：**不动 Station chunk、不需要新侧表 chunk 'VRVS'、不涉及 vector 序列化**，M1 风险与工作量大幅下降；载体侧汇总只做 NOSAVE 缓存（后加）。

| 挂载点 | 字段 | 类型/语义 | 入档方式 |
|---|---|---|---|
| `Vehicle`（RV 侧） | `uint8_t rv_transport_flags` | 位0=`WaitingToBeTransported`（在站待运，仍在图上停车）、位1=`Transported`（挂起离图） | VEHS `_common_veh_desc` 加 `SLE_CONDVAR_X(Vehicle, rv_transport_flags, SLE_UINT8, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_ROAD_VEH_TRANSPORT))`；**不用 vehicle_flags 高位** |
| `Vehicle`（RV 侧） | `VehicleID transported_by` | 宿主载体 **Front**（整列/整船/整机维度） | `SLE_CONDREF_X(Vehicle, transported_by, REF_VEHICLE, …, 同一测试)` + Ptrs 回填；`VehicleID::Invalid()`=未收运 |
| `Vehicle`（RV 侧） | `VehicleID transported_host_part` | 宿主**节**（车厢/船节/主机）→ 满足"节级可追溯"：落地/容量归还按此节；一节装哪些 RV = 过滤 `transported_host_part==该节` | 同上（REF_VEHICLE） |
| `Vehicle`（RV 侧） | `uint16_t transported_weight` | 该 RV 占用宿主节的吨数（1 吨单位；装载瞬间快照 = 整备重 [+可选含货]） | `SLE_CONDVAR_X(…, SLE_UINT16, …)` |
| `Vehicle`（RV 侧） | `uint32_t transport_wait_tick` | 进入"等待被运载"的时刻（FIFO 排序、"未卸/未装 N 天"告警） | `SLE_CONDVAR_X(…, SLE_UINT32, …)` |
| `Vehicle`（载体 Front/节，NOSAVE） | `transported_weight_cache`（Front 汇总）、每节用量缓存 | 反查得到的缓存，随 `GroundVehicle::CargoChanged`/MarkDirty 重算 | NOSAVE，载入后按需重建 |

**待运队列**：不加 Station 字段——"站内等待被运载的 RV" = 过滤车辆池中 `rv_transport_flags.WaitingToBeTransported == true && last_station_visited == st->index` 的 RV，按 `transport_wait_tick` 排序即 FIFO；站销毁/车辆离开队列时按状态位注销。

**RV 侧状态位实现选择**：给 `Vehicle` 增加**XSLF 门控的独立字节** `rv_transport_flags`，而不是占 `subtype` 的 bit7（0.73.1 基线 bit7 空闲，但 decouple/pulsexlb 各自占用了 GVSF bit 7 用于 front-wagon 类角色，为将来合并留余量）。

### 3.2 AfterLoad 迁移与验证

- `AfterLoadVehiclesPhase1`（sl/vehicle_sl.cpp:285 区）追加：若 `SlXvIsFeatureMissing(XSLFI_ROAD_VEH_TRANSPORT)` → 全池清 `rv_transport_flags/transported_by/transported_host_part/transported_weight/transport_wait_tick`（旧档无此数据）；若 Present → 校验：`Transported` 的 RV 必须（a）`transported_by` 与 `transported_host_part` 有效且同属一条链（宿主节能解析到宿主 Front）；（b）不在任何 tile 哈希；（c）不在任何 Station 的 `loading_vehicles`。不一致=按"落地失败→滞留宿主"修复并记 debug 日志（DEBUG(misc, 1)）。
- `WaitingToBeTransported` 但所在站已无效/无匹配路站 → 清位并恢复正常行驶（避免死锁）。
- 回归三连（每档变更都跑）：① 0.73.1 旧档读入（新字段取默认、无崩溃）；② fork 自存自读一致；③ "收运中存档→读→再落地"往返（覆盖 vehicle_sl.cpp:299 式链一致性检查路径）。

---

## 4. M2 订单与状态机（对应 P2；核心，最小可玩闭环在此收口）

### 4.1 载体侧："站订单开关 + 参数块（OrderExtraInfo）"（2026-09-10 评审处置 #4 定稿，取代原"参数行/元订单"方案）

- 给 `OT_GOTO_STATION` 订单加"装载/卸载道路载具"开关（新 `ModifyOrderFlags`，参照 MOF_RV_TRAVEL_DIR 的既有加旗标套路）。
- 打开开关后，**参数直接挂在该站订单本体上**：`OrderExtraInfo`（懒分配）里新增一个"RV 运载参数块"结构，字段：`筛选条件集`（见 4.3）、`数量上限`、`是否空等`（与 full-load 旗标组合）、`目的地不匹配是否放行`、`下车方向`（复用 `SetRoadVehTravelDirection`，order_cmd.cpp:2711）。**不新增 OrderType、不插入任何"参数行/元订单"**——因此没有 ProcessOrders/AdvanceOrderIndex/删除级联那套特判；所有副作用只在装货循环（LoadUnloadVehicle 扩展分支）与 GUI 编辑里发生。载体进站时读到的正是该站订单副本（含 OrderExtraInfo），天然可取参数块。
- GUI：order_gui.cpp 的站订单详情行加开关 + 参数块展开编辑（条件编辑器复用条件订单控件）；英文/简中字符串按 STR_ORDER_* 命名新增。
- 归档：该参数块随订单序列化（OrderExtraInfo 的既有存档通道，XSLF 门控）。

### 4.2 RV 侧：等待被运载状态机

```
普通行驶 →(到"等待被运载"订单的站，停定在匹配公路停靠站格)
  → WaitingToBeTransported：注册进 Station 待运队列（记 dest=下一被卸载站）；
     → 该状态下不进行普通装卸（ProcessOrders/BeginLoading 特判，参照 WAIT_COUPLE 防误装卸先例）
  →(载体装载扫描命中，原子收运：清出队列/释放 bay·Entry 记账)
  → Transported：置 transported_flags.Transported + transported_by=载体Front；
     退出 tile 哈希、Hidden+Stopped、不 tick/不绘制/不计成本/不进列表（M4 清单）
  →(载体到卸货站、格位就绪，原子放车)
  → 落地：清位、重挂 tile 哈希、恢复运行；从其排程"被卸载"订单之后继续
```
- RV 订单表示：新增订单行/旗标"等待被运载"，可携带"目的地声明"（默认=下一被卸载站，显式可选）。RV 侧不承载筛选表达式（表达式归载体，见 4.3）。
- 状态串新增（照 pulsexlb "Waiting for locomotive" 风格）：`Waiting to be transported` / `Being transported` / `Waiting to unload`。

### 4.3 条件表达式筛选（评审定稿：复用条件订单体系，"不匹配即跳过"）

- **改造点 A（唯一必要的求值侧重构）**：把条件订单求值入口从"作用域=拥有订单的车辆"抽成显式入参形式，例如 `bool EvaluateVehicleCondition(const Order& cond, const Vehicle* subject, …)`；对既有条件订单调用传原车，行为不变（零影响）。位置：order_cmd.cpp 的条件求值 switch 一带。
- **改造点 B**：注册 RV 侧新变量到 `OrderConditionVariable`（追加枚举值，带 versioning 注释）：候选 RV 的卸车目标站、Bus/Truck、极速、长度、载货率、是否铰接、引擎 ID、组 ID。取值函数以 subject 为候选 RV 计算。
- **改造点 C**：载体站订单参数块（§4.1）存"条件集"（≥0 子句；子句=变量+比较器+值；多子句=AND；0 子句=不过滤）。默认新建时**自动插入一条预设子句**：`候选RV.卸车目标站 == 本载体订单本次到站之后的下一个停靠站`（作为普通可删子句，勿写死为引擎规则）。
- **装载扫描语义（挂在 LoadUnloadVehicle 扩展分支）**：载体 OT_LOADING 期间，若当前站订单参数块要求装 RV 且该节可装（4.4）：对站待运队列按 FIFO 逐台求值 → 命中则走收运事务（容量/重量校验见 4.4），不命中则跳过留队；0 命中且不允许空等 → 按普通满载语义离站。扫描与普通货装卸同 tick 节拍，FIFO 纪律不破坏。
- slot 相关：`SlotOccupancy/VehicleInSlot/VehicleInSlotGroup` 直接作为变量使用（读共享槽状态，不随作用域变）；`OT_SLOT` TryAcquire/Release 的领/放仍由**拥有订单的车**执行——"预约班次"组合用法（载体先 TryAcquire 槽、表达式用槽状态筛选）留到 P3 验证清单再实测，本期 GUI 只做基础筛选。

### 4.4 容量/重量校验（收运事务的前置判定，逐节）

- 装载门：见 §1 表"装载门"——`RVTransportPartCanCarry(part)` 按三档设置判定（默认档 2：散货 / oversized / 标签 `VEHC` 三类之一），档 1 才是只认 `IsCargoInClass(cargo_type, CargoClass::Oversized)`；`force` 参数可绕过（调试用）。
- 节运载吨容量 = `cargo_cap × CargoSpec::Get(cargo_type)->weight / 16`；RV 占用吨 = 整备重（`GetWeight()` 不含货）+（可选开关：+自身载货重）。
- 校验：`sum(transported_units_used) + 本RV占用 ≤ 节吨容量`；不满足→拒装（新闻串提示），RV 留队。
- 铰接 RV 整组按一个候选判定与收运（原子）；一节装不下整组就不装。
- 落地归还容量/重量；同节剩余吨容量**不得**再装普通货（节内不混装），其余节正常装普通货。

### 4.5 最小可玩闭环（本里程碑验收）

测试地图：一站同时含铁路站台+公路停靠站（直通式）A/B。场景：RV(带货) 从 A 站路站排队 → 火车到 A 有"装载 RV"参数块（默认目的站匹配）→ RV 消失并计入火车（运载面板可见）→ 火车到 B → RV 落地恢复行驶 → 后续订单继续。**联机 2 端 sync test 1 小时**。收/放画面仅默认装卸动画。

---

## 5. M3 站队列、匹配表达式接入与重量（对应 P3）

- 队列注册/注销点：RV 进入 WaitingToBeTransported（注册）与收运/玩家手动取消（注销）；落地不经过队列。
- 装载扫描与条件求值挂接：`LoadUnloadVehicle` 扩展分支内（见 4.3/4.4），调用 `EvaluateVehicleCondition(subject=候选RV)`。
- 重量：`GroundVehicle<T,Type>::CargoChanged()`（ground_vehicle.cpp:103-145）逐节循环内、`current_weight = u->GetCargoWeight()` 之后加 `current_weight += u->GetTransportedWeight()`（读节缓存 transported_weight，收/放时置脏 → `MarkDirty`（train_cmd.cpp:4993 / roadveh_cmd.cpp:438）触发重算）。坡度阻力/质心/PowerChanged 自动联动。船/机无重量模型，只做容量与显示。
- GUI 数据：运载面板/站窗显示用的聚合（数量/总重/目的站）由 Front `transported_rvs` + 节缓存现算。
- 验收：5 RV × 3 载体（含"带筛选/不带筛选"两类订单）反复装卸 30 分钟无错乱；筛选不匹配的确认留队等后续班次；重量反映到列车动力（上坡极速）；货+RV 混列正常。**补测**：slot 变量作为子句时求值上下文正常、OT_SLOT 领/放时点不破坏扫描（本期只验证读取语义）。

---

## 6. M4 TRANSPORTED 生命周期与豁免点（对应 P4；风险最高，先行自测）

### 6.1 离图/回图事务

- 收运 = 对 RV Front 整链（铰接）：记现场快照（NOSAVE：原 tile/方向/last_station_visited/orders 游标）→ `UpdateVehicleTileHash(remove)`（vehicle.cpp:846，参照 :851 VIRTUAL 分支写法）→ 置 `Hidden|Stopped` + transport_state.Transported + transported_by → 从 `loading_vehicles`/站队列/`VehiclesOnTile` 语义中消失 → 失败则回滚（备份恢复）。
- 落地 = 逆向：格位分配器（见 6.3）→ 重挂哈希 → 清位/清 Hidden/恢复 Stopped=false → 校验成功后返回运行；失败保持 Transported，绝不半落地。
- 宿主被售/被毁/公司破产清盘时的清场规则（新增处理点，见 6.4）：先把被运 RV 作"强制落地到最近匹配站"，无匹配站/无空间则"按残值赔偿移除"（先在设置开关下实现"移除+赔款+新闻"，默认开）。

### 6.2 豁免点清单（对照 GVSF_VIRTUAL 排雷；逐点改并断言）

| 系统 | 文件（锚点函数） | 处理 |
|---|---|---|
| 经济/折旧/运行成本/利润 | economy.cpp（车辆遍历处 159/248/541 同款位置） | Transported 的 RV 跳过 |
| 公司统计/基建/网络报表 | infrastructure.cpp、network_server.cpp:1890 同款 | 跳过 |
| 车辆列表/车库/组/共享订单 | vehiclelist.cpp:149/178、vehiclelist.cpp:78（BuildDepotVehicleList 天然不中——无 tile）、group_cmd.cpp:142 | 跳过/天然排除 |
| 绘制/视口 | vehicle.cpp:423 IsDrawn（参照 VIRTUAL） | 不绘制 |
| 碰撞/灾难/新闻目标 | disaster_vehicle.cpp:592 同款 | 排除 |
| 自动替换/克隆/模板 | autoreplace_*、vehicle_cmd.cpp 克隆路径 | 排除 + "运载中不可操作"断言 |
| 卖出/拆除 | vehicle_cmd.cpp（SellVehicleFlags 同款加 VirtualOnly 式旗标） | 直接禁止：CmdSellVehicle 对 Transported 返回错误 |
| AI/脚本 | script/api（script_vehicle* 等） | 只读隐藏（查询返回"被运载中"，禁命令） |
| desync 校验 | cachecheck.cpp | 允许瞬时态（链头≠front 之类）或跳过 |
| 存档链完整性 | sl/vehicle_sl.cpp:299 同款 | Transported RV 不被当作普通链/图上车校验 |
| 基建共享计费/破产 | 同上经济遍历处 | 跳过 |

> 原则：与 GVSF_VIRTUAL 一致——"在池里但被所有常规系统豁免"，逐点改判定，不改既有行为。

### 6.3 落地格位分配器（卸载侧规则，优先级从高到低）

1. 目的站（RV dest）同 Station 的**直通式公路停靠站**：该方向 `Entry.length − Entry.occupied ≥ RV 整车长（铰接按 gcache.cached_total_length）` → 落格；
2. 港湾格（Bay0/Bay1 空闲 且 !entrance busy 且 RV 非铰接——铰接本身被 RoadStop::Enter 拒）→ 落格；
3. 都无 → RV 保持 Transported，随载体去下一兼容站重试；达到"未卸天数阈值"（设置）→ 新闻告警；
4. 目的站被拆/路线被改永不触达 → 见 4.2 兜底/6.1 清场。
落地瞬间需恢复的下车方向：取 RV 订单"下车方向"参数（默认随路站方向/其排程下一站方位）。

### 6.4 新增处理点（宿主生命周期联动）

- `~Vehicle`/`PreDestructor`（vehicle.cpp:1152/1241 区）：宿主（载体）销毁前，对被运 RV 执行 6.1 清场；
- 公司破产/清盘：遍历公司 Transported RV 走清场；
- 卖出命令、autoreplace 换代、克隆：均对"宿主含被运 RV"或"RV 处于 Transported"报错或先清场（按设置：auto 清场 on/off）。

### 6.5 验收

运载中 RV：崩溃/卖出/车库/替换/克隆/灾难均不可触碰；联机 2 端 1h sync test 无 desync；落地后统计、重量、顺序、货物完整恢复。

---

---

## 7. M5 GUI（对应 P5）

- **运载面板（跨车型统一）**：新增独立子窗口 `WindowClass::VehicleTransportedList`（window_type.h 追加值；window number=公司/VehicleID 语义自定），由 `VehicleViewWindow` 加按钮打开（vehicle_gui.cpp:3955 区；widget 定义 widgets/vehicle_widget.h）。内容：遍历 `Vehicle::IterateTypeFrontOnly` 过滤 `transport_state.Transported`，列"车型(引擎名×N)/总重/目的站/宿主载体/滞留天数/车内货物摘要"；点击单项开该车 `VehicleView`（只读）。不提供买/卖/改装/维护入口。
- **火车详情可选 tab**：`TrainDetailsWindowTabs`（vehicle_gui.h:26）追加 `TDW_TAB_TRANSPORTED`；改 `_nested_train_vehicle_details_widgets`（vehicle_gui.cpp:2985）、tab 切换（OnClick :3597 区）、`DrawTrainDetails`（train_gui.cpp:429）加分支。注意 static_assert（vehicle_gui.cpp:2954）把 tab widget 与枚举钉死，加值要同步。非火车不做 tab（其详情窗无 tab 条）。
- **站窗口**：待运队列显示（数量×目的站）放 station_gui.cpp（可加 tooltip/次要文本，避免大改布局）。
- **订单编辑器**：载体站订单行加"装载/卸载道路载具"开关（order_gui.cpp；MOF_* 追加，order_cmd.cpp）；条件编辑器复用条件订单控件组（变量下拉/比较器/值 + 预设按钮：目的站匹配/仅卡车/仅巴士/长度≤N…）；RV"等待被运载"行编辑（目的地声明可选）。
- **字符串**：`src/lang/english.txt` 新增（命名 STR_ORDER_/STR_VEHICLE_/STR_NEWS_ 前缀按窗口上下文；需要 4 车种×N 的用 `###length VEHICLE_TYPES` 组）；中文同步译（missing 回退英文）。新增状态串/新闻失败串：装载失败(超重/无空间/目的地不可达/表达式不匹配时无车可装)、未卸告警。
- 验收：运载面板数据与实体一致；订单开关+条件编辑闭环可用；人类评审 UI 走查。

## 8. M6 船/机打通（对应 P6；无动画工作）

- 船：多节船每节独立 Ship Vehicle、独立 `cargo_cap`（XSLFI_MULTI_CARGO_SHIPS + GRF multi_part_ships 门控；articulated_vehicles.cpp AddArticulatedParts）→ 逐节判定与收运；无重量物理（跳过 CargoChanged 分量，仅容量/显示）。
- 机：只有整机容量概念（mail 部件=第 2 个 Aircraft 部件、AIR_SHADOW，aircraft_cmd.cpp:293-378）；`cargo_type` 属 Passengers 时天然不可装（门不过）；货机 refit oversized 后可用；无重量物理。
- 收/放画面：全部走默认装卸动画（无自定义）；运载面板文本罗列跨 4 类一致。
- 验收：船/机各跑一遍 4.5 闭环；refit 到 oversized 前后装载门行为正确。

## 9. M7 设置/经济/NewGRF/脚本（对应 P7）

- 设置项（table/settings/game_settings.ini + settings_type.h 结构 + settings_compat.h 老档名映射）：
  - `roadveh_transport.enabled`（总开关，GameSetting）；
  - `roadveh_transport.count_transported_weight_with_cargo`（RV 自重是否含其载货，默认 true）；
  - `roadveh_transport.freeze_in_transit_cargo`（D12 口径，默认 A=完全冻结；预留 B/C 枚举位）；
  - `roadveh_transport.unload_warning_days`（未卸告警阈值，默认 30 天）；
  - `roadveh_transport.liquidation`（宿主清场策略：force-land / compensate-remove，默认 compensate-remove）。
- 经济：本期免费；只加统计字段（被运载公里数：按宿主累计移动 × 车上 RV 数计，随宿主里程钩子，见下）。收费运价=二期（需自建价目表，勿混入 CargoPayment）。
- D12 口径 C（时间+里程联动）若启用：在宿主移动/到站钩子处，把行进增量按比例写入被运 RV 各 cargo packet 的 travelled 并调用 AgeCargo——**本期不做**，仅留钩子注释。
- NewGRF：验证点 = 带 refit 容量回调（`CBID_VEHICLE_REFIT_CAPACITY`/`Engine::DetermineCapacity` engine.cpp:236）的车在收运瞬间容量换算走 `refit_cap` 正确；铰接 RV 收/放瞬间的 NewGRF 变量（var0x42 等）一致性——如需可在收运/落地前后调用 `InvalidateNewGRFCacheOfChain`+`RefreshTrainUserDefData`（pulsexlb 假想列套路），失败即回滚。
- AI/脚本：script API 对"被运载 RV"只读（返回状态），禁任何命令；站订单 RV 参数块对脚本可见性按只读最小化。

## 10. M8 回归与收尾（对应 P8）

回归矩阵（每条 = 可运行场景 + 预期）：

| 场景 | 预期 |
|---|---|
| 0.73.1 旧档读入 | 无崩溃，新字段默认 |
| fork 自读自档（含收运中） | 一致（M1 三回归） |
| gradual_loading / improved_load / 真实制动 与收运组合 | 节拍不冲突、重量正确 |
| 模板替换/自动替换/克隆碰到被运 RV 或宿主 | 被拒或先清场（M4） |
| Cargodist：RV 货物在途 | 不产生中间 flow（D12/§4.4 末节） |
| 铰接 RV / 多节船 / 货机 | 逐节判定正确 |
| 港湾 vs 直通混用、铰接卸车只走直通 | 格位分配器行为符合 M4.6.3 |
| 站台被拆/RV 滞留 | 告警 + 兜底（M4） |
| 条件表达式各比较器/预设/RV 新变量 | 求值正确、跳过语义稳定 |
| slot 子句读取 + OT_SLOT 领放时点 | 无死锁/无 desync（P3 补测） |

- 联机：2–4 端 + AI 对手 ≥2h；sync test；desync 修复后补跑全矩阵。
- 卫生：删调试桩（R3R 教训：fopen 逐 tick 写盘/硬编码地图坐标/TEMPORARY DIAGNOSTIC）；`english.txt`+`simplified_chinese.txt` 增量最小化；确认无死代码/孤立字符串。
- 收口：CHANGELOG 条目 + 发布说明；更新《规划》附 B 开放项与本节估算。

---

## 11. M9–M11b：施工中追加的里程碑（正文 §3–§10 只规划到 M8）

> 正文 §3–§10 是"原计划 M1–M8"的设计细节，**仍然有效**；M8 之后的部分是在与评审来回中追加的，设计依据以本节 + 附录 D + 附录 E 为准。

| 里程碑 | 内容 | 验证 |
|---|---|---|
| **M9** | **铰接（多节）道路载具运载**：装载/卸载/释放/销毁全部按整挂（front + 各铰接节）处理；每节都要隐藏并逐节退出路网哈希（弯道上各节本就不同格）；卸载/释放整挂落同一格并按"出库"方式散开；销毁从尾到头；计数/列表/读档校验以车头为准；重量沿用引擎整挂重量缓存 | 单节路径回归全绿；**铰接车实机走查通过**（评审实测） |
| **M10** | **条件选择运载（参数化筛选核心）**：候选载货状态（任意/空/满）、货物（任意/能载/正载）、最短等待天数；`OrderExtraInfo` 新字段 + 4 个 MOF + `RVTransportOrderAllowsCandidate()`；装货循环与"等待"判定统一改用它；"不匹配即跳过" | `verify_filter.ps1` PASS（7 用例） |
| **M10b/c** | **GUI 完整化**：先是下拉里的循环切换项（已废弃），再改为**一个独立窗口**装下全部道路载具运输设置（4 个开关 + 判据下拉），订单行同步列出生效项；`WindowClass::VehicleRVTransportCriteria` 独立窗口类 | 编译 + 全量回归；评审实测 |
| **M10d** | **加入"路签"判据**（对齐 px-patch 的 `MOF_COUPLE_SLOT`：候选必须是该路签的占用者）；**移除"声明目的地"判据**（多个卸货计划时只读第一条，指代不清） | `verify_slot.ps1` PASS |
| **M11** | **评审实测 5 个问题的修复**：订单行字符串参数错配（`(invalid parameter)`，亦为那次崩溃最可信根因）、直通站单侧被占不能卸（改用站点自身的泊位/入口记账 + `Enter()` 记账配对）、卸载不区分车的调度（只卸"自己声明在本站下车"的车）、设置窗口按钮无交互反馈（改用 `OnRealtimeTick()` 轮询快照）、"卸不下就直接开走"补成可选的对称等待 | 全量回归 12 脚本全绿、0 断言 |
| **M11b** | **等待状态在改命令后不消失**：跳到下一条调度/命令回库时清掉 `RVTF_WAITING` 与随之的 `Stopped`（放在 `RoadVehController()` 的停止判定之前，与进入等待的 `Vehicle::BeginLoading()` 条件对称） | 全量回归全绿；待评审复测 |
| **M11c** | **重量记账 + 载重外观 + 载运清单**（评审实测反馈的三点）：①被运载车辆计入**载体自重**（`RVTransportGetCarriedWeightTonnes()` → `GroundVehicle::CargoChanged()`，装卸后 `Train::MarkDirty()` 重算）；②载体"看起来装满了"（原版火车按"货物过半即满"、NewGRF 车辆集按 `stored*totalsets/capacity`，另外从**货物量变量 0x3C/0x3D**取图的车辆集也会读成满载；只要该节载有道路载具就取满载图）；③**载运清单**：载体详情窗口列出所载车辆（火车在"信息"页末尾、可滚动，船/机在详情面板底部、窗口高度自动增减）。**附带修掉一次必崩**（详情窗"信息"页：循环把形参 `v` 走到 nullptr 后仍被使用，见 D.5 第 5 条） | `verify_attach.ps1` 重量断言 PASS（装卸前后 `carried=`/`total_incl_carried=`/`own=`）+ `verify_details.ps1` 行数记账 PASS + 全量回归 13 脚本 |
| **M11d** | **"哪节可以装车"改为三档设置**（修订 D1，见《规划》§1.4 的 D1 修订框）：`vehicle.rv_transport_carrier_parts`（`enum class RVTransportCarrierParts`，`settings_type.h` + `game_settings.ini` 的 `[SDT_ENUM]` + `_rv_transport_carrier_parts[]` 枚举表，`SettingFlag::Patch`，**默认档 2 = 散货/`Oversized`/标签 `VEHC` 三选一**）；判定集中在 `RVTransportPartCanCarry()`；类别 `SC_EXPERT`，并在设置窗口里**单独开一页"道路车辆运输"**（`src/settingentry_gui.cpp`，`SettingsPage *rv_transport`）；英文/简中各 5 条新字符串，删掉旧的 `vehicle.rv_transport_require_oversized` 与其字符串。**只在装载时判定，卸载不看此设置**（切档不会把已在车上的车丢在路上）。回归套件在 `_common.ps1` 里统一先 `setting ... 0` 开门（测试存档载体是木材车厢，属档 2 拒绝的货物），`-CarrierParts -1` 可关掉这个前置 | `verify_carrier_parts.ps1` PASS：默认值 = 2；同一节木材车厢在档 0 下 `rv_capacity=30t`、`loadfrom ... attached=true`，档 1/2 下 `rv_capacity=0t`、`attached=false`（用 `setting vehicle.rv_transport_carrier_parts <0\|1\|2>` 在运行中切换） |

追加项共同的收尾约束（与 M8 一致）：每个里程碑都要"可编译 + 最小场景可玩 + 回归不炸"，并同步规划/规格/手测文档。

### 11.1 M11e：每单装载上限与两处订单命令修复（2026-09-12）

`OrderExtraInfo::rv_transport_max`（自 M1 起就在存档里、但没人读的**死字段**）接完整：

| 项 | 内容 |
|---|---|
| 语义 | 「最多同时装载 N 辆」= 载体**当前已载**的道路载具数量达到 N 时停止继续装；**0 = 不限**（默认）。多段行程里上一站装的车也计入 |
| 数据/命令 | `MOF_RV_MAX`（`order_type.h`）+ 站订单白名单 + 校验（`data <= 0xFF`）+ 写入 `GetRVTransportMaxRef()`；`Order::AssignOrder()` 早前已带上该字段 |
| 判定点 | `RVTransportAttachAuto()`（`force=true` 时跳过，供调试命令使用）。所有自动装载与调试装载都经此处，因此只有一个判定点 |
| GUI | 设置窗口新增一行「最多同时装载」下拉（不限/1/2/3/5/10，非预置值显示为"最多自定义数量"）；勾选装载时才可用；订单行同步显示「最多 N 辆」 |
| 控制台 | `rvtransport criteria <vehicle> <order> max <n>` |
| 回归 | `verify_max_load.ps1`：船 #28 上限 1 → 装载#1 `attached=true`、装载#2 `attached=false`；上限 2 → `true`；上限 0 → `true` |

顺带修掉的两处（都由这轮测试暴露）：

1. **`rvtransport orderflag`/`modify` 与 GUI 的 `UNLOAD_ALL`**：`MOF_RV_TRANSPORT` 的数据白名单漏了 `ORVTF_UNLOAD_ALL`，导致设置窗口里「在此卸下全部道路载具」勾选框**点了没反应**（命令返回 `CMD_ERROR`）。修后 `rvtransport modify … unloadall` 由 `FAILED → OK`，并加进 `verify_toggle.ps1` 的断言（旧脚本用的 `orderflag` 直接写引用、绕过校验，所以一直没覆盖到）。
2. **控制台的"同步当前订单"辅助函数漏字段**：`RVTransportDebugSyncCurrentOrder()` 没把 `max` 抄进 `current_order`，于是上限只写进订单列表、装载读不到——`verify_max_load.ps1` 第一次跑就是 `attached=true`（本应为 `false`），补上该行后通过。GUI 路径不受影响（引擎在 `ProcessOrders()` 里整体 `current_order = *order`，`extra` 是深拷贝）。

### 11.2 M11f：联机（网络游戏）验证（2026-09-12）

分支此前**从未做过联机测试**。新增 `testrun/verify_mp_sync.ps1`，两端都无头运行：

- **服务器**：`-D :3982 -g <存档>`；
- **客户端**：`-D -n 127.0.0.1:3982`（走 `NetworkClientConnectGame()` 的"无头客户端"，不需要窗口）。

脚本先自己造出"已有道路载具在车上"的存档（`sim` + `save`），再让客户端加入，然后比对：

| 检查 | 结果 |
|---|---|
| 客户端确实以客户端身份加入（服务器日志 `[server] Client #N … joined as`；客户端 `Connected to 127.0.0.1`） | ✅ |
| 同一辆车 `rvtransport state` 在两端**逐字段一致**（`flags=2 tile=… hidden=true by=6 part=7 weight=10 waiting_tick=…`） | ✅ |
| `rvtransport carried firsttrain` 两端一致（`carrier #6 holds 1 road vehicle(s)`） | ✅ |
| 无 desync（两端日志均无 desync/checksum 相关输出） | ✅ |

> 说明（**为何不用 `sim` 驱动装载**）：`rvtransport` 的修改类子命令（`sim`/`attach`/`loadfrom`/`setwaiting`）是**本地调试命令**，直接在服务端改状态、**不走命令框架**，客户端按设计看不到（实测正是如此：服务端 `flags=2`、客户端仍 `flags=1`）。因此联机检查用"加入时整盘传输"来验证本分支新增的**车辆存档字段**（`rv_transport_flags`/`transported_by`/`transported_host_part`/`transported_weight`）在网络传输后一致——这也是该特性最可能引入不同步的地方。

### 11.3 M11g：被运载过久的告警（规格书 §7 设置项，2026-09-12）

规格书 §7 列的设置项里，"被运载超过 N 天未卸"的告警阈值此前未实现，而它对应的正是 §6 风险表里那条"目的站被拆/改建导致滞留堆车"。补上：

| 项 | 内容 |
|---|---|
| 设置 | `vehicle.rv_transport_unload_warn_days`（`SLE_UINT16`，`SettingFlag::Patch`，默认 **30**，范围 0–1000，0 = 不告警；类别 `SC_EXPERT`，与"哪节可以装"同页） |
| 计时 | 装载成功时把 `transport_wait_tick` 记为当前 tick（该字段只在**等待**状态下被"最短等待"判据读，而等待开始时会被重新赋值，故复用安全）；`RVTF_UNLOAD_WARNED`（bit 2）记录"本趟已告警" |
| 判定 | 在 `RunVehicleDayProc()` 里对**被运载**的车调用 `RVTransportCheckCarriedTooLong()`（被运载车本来在日循环里被整体跳过，正好在这里检查）；超过 `阈值 × DAY_TICKS`（与"最短等待"判据同一换算口径）就发一次 `NewsType::Advice` 新闻 `STR_NEWS_RV_TRANSPORT_UNLOAD_OVERDUE` 并置位 |
| 重置 | 重新进入等待（`RVTransportSetWaiting(true)`）或再次被装上时清掉告警位 → 每趟只提示一次 |
| 回归 | `verify_unload_warn.ps1`：先把车装上并**清空该车所有订单的 RoRo 旗标**（保证不会再被卸下），阈值设 1 天后跑 60 s → `flags=6`（carried｜warned）；阈值设 0 后同样跑 60 s → `flags=2`（无告警） |

### 11.4 M11h：特性总开关（规格书 §7 设置项，2026-09-12）

规格书 §7 的"特性总开关"此前没有实现（只有"哪节可以装车"的三档门）。补上 `vehicle.rv_transport_enabled`（bool，默认 **开**，`SettingFlag::Patch`，`SC_EXPERT`，与其它两项同页）：

| 项 | 内容 |
|---|---|
| 关闭时 | **不装载**（`RVTransportPartCanCarry()` 直接 false，装载循环的 LOAD 分支也整段跳过 → 关闭时该特性的每 tick 成本为零）；正在等待的车在 `RVTransportTickWaiting()` 里**结束等待**并继续自己的调度（否则会永远等一班不会来的车）；载体订单里的 `ORVTF_WAIT`（"等到装上车"）不再压住发车 |
| 保持可用 | **卸载/放下照常**（`RVTransportDetachAtStation` 与卸载分支不受门控影响）→ 切开关不会把已经在车上的车卡死 |
| 不改数据 | 只是运行时门控，**订单里的 RoRo 设置原样保留**，重新打开即恢复 |
| 绘制路径 | 关闭时连**精灵/货物量查询**也跳过（`train_cmd.cpp` 的"满载外观"、`newgrf_engine.cpp` 的变量 0x3C/0x3D 与 `RVTransportExtraCargoAmount()`）——这两处会扫全车辆池，而它们在精灵路径上；状态类查询（载运清单/`RVTransportCountOnCarrier`）**不挂门控**，保证 UI 始终显示真实情况 |
| 回归 | `verify_carrier_parts.ps1` 追加两组读数：开关关闭时 `rv_capacity=0t` 且 `loadfrom … attached=false`；重新打开后恢复 `rv_capacity=30t`（PASS 文案同步更新）。全量 22 支全绿 |

### 11.6 M11j：评审定稿（2026-09-12）——运价模式不做，分支可合并

- **D6 运价**：评审明确**不做收费模式**，保持"免费内部转运"（被运载段不产生收入，只计入服务次数统计）。规格书 §7 的设置项清单里"运价模式(免费/收费)"一项据此关闭。
- **合并前收尾**：评审同意合并；分支推送后，剩余唯一收尾项是"删除 `rvtransport` 调试命令"（该命令目前同时是全部 22 支回归脚本的驱动手段，删除方式需保证回归仍可运行，见下一轮处置）。

### 11.5 M11i：性能实测方法与被测结果（2026-09-12）

`testrun/measure_perf_run.ps1`：单次运行版测量脚本，用"**每墙钟秒推进的游戏日数**"作指标（专用服务器按实时跑，若额外开销把 tick 顶出预算，达成速率就会掉）。两次运行分别用 `-Switch on` / `-Switch off`。

测量中踩到并解决的两个坑：

1. **存档自带的暂停模式**：评审那份 21 MB 存档存了一种 `unpause` 清不掉的暂停状态（实测三次读日期完全相同、进程 CPU 近似空转）。解法：起一个**无头专用客户端**（`-D -n 127.0.0.1:端口`，与联机测试同一手法）加入服务器，客户端加入即清除该状态，时钟随即开始推进。
2. **日长因子**：首次测得 0.0222 日/秒（≈74×22.5 tick/天）并非"卡在加载"，而是该存档的 `economy.day_length_factor` 被拉长。脚本现在先 `setting economy.day_length_factor 1` 归一化，使达成速率与满速基准（0.5 日/秒）可比。

被测结果（评审存档的**副本**，日长因子=1，两次都读到 76 辆车，客户端均已加入）：

| 配置 | 区间1 | 区间2 |
|---|---|---|
| 特性 ON（`rv_transport_enabled = true`）| 0.2933 日/秒 | **0.3200 日/秒** |
| 特性 OFF（门控关闭，一行特性代码都不执行）| 0.3133 日/秒 | **0.3000 日/秒** |

结论：差异（ON 反而快 ~6%）完全落在逐次运行噪声内 → **该特性在此规模下无可测量开销**，与 §11.1 的审计一致（每 tick 只有 O(1) 旗标判断；全车辆池扫描只在精灵路径，且总开关关闭时整段跳过）。

**满负荷场景（评审要求的 500 台等待车）已补测**：子代理按"临时调试子命令造车→存场景档→跑两次→删命令"的流程执行，产出场景存档 `build\save\roro_fullload.sav`（**511 辆车，其中 500 台是在同一年站"等待被运载"的公路车**，载体订单带装载旗标）：

| 配置 | 区间1（10-10→12-25）| 区间2（12-25→03-10）| 区间3（03-10→05-25）|
|---|---|---|---|
| ON | 0.5067 日/秒 | 0.5000 日/秒 | 0.5067 日/秒 |
| OFF | 0.5067 日/秒 | 0.5000 日/秒 | 0.5067 日/秒 |

两次运行的日期推进**逐区间完全相同**（1950-10-10 → 12-25 → 1951-03-10 → 1951-05-25）⇒ **服务器 500 台等待车满载时仍完全跟得上实时**。

**更精细的复测（同一场景，细分指标）**：请求指标在该场景下**没有分辨率**（两种配置都恰好跑在标称速率上），于是补做了两组更灵敏的测量：

| 指标 | ON | OFF | 差值 |
|---|---|---|---|
| 秒表计时的达成速率（区间实长约 148–152 s，ON/OFF 日期仍逐区间相同）| 0.5133 / 0.5200 / 0.5133 日/秒（ON 实例）| 0.5133 / 0.5200 / 0.5133 日/秒 | **0** |
| 强制全速吞吐（临时 `fastforward`，各 2 次 × 4 区间）| 9.169 / 9.132 日/秒 | 9.182 / 9.159 日/秒 | **−0.2%**（运行间离散 ±0.2%）|
| **每 tick CPU（最灵敏）** | 0.351 ms/tick | 0.316 ms/tick | **+35 µs/tick** |
| 同上（全速档） | 0.158 ms/tick | 0.119 ms/tick | +39 µs/tick |

两档独立给出**同一量级：开启该特性多花 ≈35–40 µs/tick，约为每 tick CPU 的 +10%，但只占 30 ms tick 预算的 0.13%（≈0.13% 单核）**——这就是"日/秒"指标测不出来的原因。**边界说明**：这 40 µs 不能全部归因于"500 个等待候选"：ON 世界的行为本身也不同（载体真的在装车，全速档一趟装了 37 台），且其中一部分是"只要特性开启就会跑"的车辆池扫描（与候选数无关）；本想再做"开关 ON + `nowait` 存档（开启但零候选）"作为分界实验，两次都因并发会话替换 `build\openttd.exe` 而中断 ✗。此外**测量期间评审自己的 px-patch OpenTTD 一直在运行**（CPU 时间统计有 ~10–15% 区间抖动），所有结论都取自存活的那些运行。对照用的控制存档 `build\save\roro_fullload_nowait.sav`（同一世界、同一订单，499 台车存在但**不带等待旗标**）也已保留——因为把总开关关掉时引擎会顺带解除等待状态，同一存档的 OFF 半场会测得"另一个世界"。临时造车子命令已删除并重编（`git diff --stat -- src/console_cmds.cpp` 为空，工作区只剩刻意保留的 `src/window.cpp` 崩溃诊断）。

---

## 附录 A：实现顺序与依赖

M1 → M2 → M3 → M4 必须依序（各自收口即"最小可玩"增量）；M5 可与 M2-M3 并行起步（先订单开关 UI 后可单独交付）；M6 依赖 M2-M4 全绿；M7 依赖 M4/M5；M8 收尾。若做"试点校准"：取 M1(最小)+M2 的站订单参数块+4.5 闭环 = 一个可独立量测 token 的小试点。

## 附录 B：本文件来源与版本

- 依据：《规划》§1.3/§1.4/§4（决策与论证）、调研底稿 sa1–sa4（函数/行号锚点）、评审 v2 意见（默认动画/条件表达式/slot 维度/在途口径）。
- 定位铁律：所有"文件（函数）"引用在 0.73.1 实测；行号随版本漂移，实现时以函数名 grep 定位。
- 本规格不回答"为什么"，只回答"改什么/怎么验"；如实现 agent 发现与《规划》冲突，以《规划》决策表为准并回写本文件。

## 附录 C：外部评审处置记录（2026-09-10，优先级高于正文冲突处）

> 评审来源：`建议.txt`（16 问题 + 风险 A/B/C）。状态：✅定案（改设计）/ 🔧规格补丁 / ⭕约束消解 / ⛔不成立。

| # | 状态 | 处置 |
|---|---|---|
| 1 | ✅ | 落地记账改**节级**：每节持 `transported_slots{RV_front_id, 占用吨}`；Front `transported_rvs` 降为汇总缓存（顺序=装载序）；落地按节归还，删除"Front 表+节缓存"双写旧设计。 |
| 2 | ✅ | `StationRVItem` **只存 `rv_id`，不存 dest 快照**；dest/条件在扫描与显示时实时解析 RV 当前订单。 |
| 3 | 🔧 | SLXI 子块登记：`sl/extended_ver_sl.cpp` 表项（仿 `"VESR"` 行，extended_ver_sl.cpp:144 一带）+ `extended_ver_sl.h` 加 `XSLFI_ROAD_VEH_TRANSPORT` 枚举。 |
| 4 | ✅ | **弃用"元订单/参数行"（OT_LOAD_ROAD_VEHICLES=15）**：改为站订单 `OrderExtraInfo` 挂"装载 RV 参数块"（筛选条件集/数量上限/空等/下车方向）；无新 OrderType、无 ProcessOrders 特判；正文 §4.1 按此改。 |
| 5 | 🔧 | 不重构既有条件求值调用链；新增独立入口 `EvaluateConditionForRV(const Order&, const Vehicle* rv)`；"变量取值"抽成可按 subject 调用的最小函数；既有 switch 耦合低才顺手参数化。 |
| 6 | ✅ | **收运不消耗 slot**：slot 仅只读筛选（SlotOccupancy/VehicleInSlot 子句）；OT_SLOT 领/放属载体订单原语、与收运不相交；预约玩法二期。 |
| 7 | 🔧 | RV 占用吨 = `GetWeightWithoutCargo()`（roadveh.h:254-270，1/4t 换算）+ 可选开关加 `GetCargoWeight()`；修正正文"GetWeight() 不含货"笔误。 |
| 8 | ⭕ | D12 冻结口径 + "Transported 期间 cargo 加锁"→ 货物重量恒定，transported_weight 无需同步；口径 B/C 二期再议。 |
| 9 / 风险A | ✅ | **tick 内提交纪律（非命令事务）**：装卸本不经 DoCommand；收运同 tick"先全判定→按分配先行、纯赋值收尾提交→异常同 tick 逐个还原"，无跨 tick 可见中间态；运行时快照含站队列项/RV 哈希与状态/节占用表。 |
| 10 | 🔧 | 豁免点逐条给"一行伪代码 + 天然排除/显式跳过"：`BuildDepotVehicleList`（VehiclesOnTile）天然排除；公司全列表（vehiclelist.cpp:149 Iterate）须显式跳过（仿 GVSF_VIRTUAL）。 |
| 11 | 🔧 | 定案**无状态重试**：每到兼容站从头扫、判定天然防重复、失败零数据变更（不重算重量）；无"已尝试"标记。 |
| 12 | 🔧 | 运载面板 window number=CompanyID（每公司一扇）。 |
| 13 | 🔧 | 货机 refit oversized 后 mail 容量并入主舱 → 飞机按**整机单节判定**；与多节船逐节的差异=载体形态差异。 |
| 14 | ✅ | 清场默认 **force-land → 无处可落才 compensate-remove**（设置默认值修改）。 |
| 15 | 🔧 | 性能验收：500 台待运+20 节车单次装货扫描 ≤ 设定阈值；扫描仅在到站装卸节拍发生。 |
| 16 | 🔧 | 日志白名单：保留未卸告警/装载失败新闻/回滚警告（DEBUG misc 1 级）；删 fopen 写盘/逐 tick/硬编码坐标/TEMPORARY DIAGNOSTIC。 |
| 风险B | ✅ | 筛选求值**只读**，新变量清单禁副作用；OT_SLOT 领/放只由订单原语执行，扫描内绝不 TryAcquire。 |
| 风险C | ⛔ | XSLF 特征 XSCF_NULL 恒写 → 0.73.1 打开新档为优雅拒绝并提示（非崩溃）；只需发布说明 + 验证拒绝文案。 |

## 附录 D：实现进度与验证手册（截至 M11b 主体完成，2026-09-11）

### D.1 工作副本与提交

| 项 | 值 |
|---|---|
| 开发副本 | `D:\CNS\ottd\OpenTTD-patches-rvtransport`（由 0.73.1 基线复制） |
| 分支 | `feature/road-veh-transport` |
| 提交 | 分支 `feature/road-veh-transport` 共 **34 个提交**（`611aabd7ba`..`847f02b354`，2026-09-11）：`bf70a8b4da` M1 存档骨架 → `cdf4cef8e0` M2a 核心事务 → `780396da90` M2b 订单驱动装卸 → `79937c9ecd` 载体等车 → `60d51de6b0` 载体状态串 → `7ba6ce0d55` 载体销毁释放 → `1ff9a7ff61` 载体类型收紧 + release 调试命令 → … → `2c312ae3fe` M9–M11b（铰接/条件筛选/设置窗口/路签/五项修复/等待状态）→ `847f02b354` M11c（重量记账 + 满载外观 + 载运清单，见 §11 与 D.5） |
| 纯基线副本（旧档生成器） | `D:\CNS\ottd\OpenTTD-patches-baseline`（未改动，已构建） |
| 构建方式 | MSYS2/MinGW64：`D:\msys64\mingw64\bin` 的 cmake+ninja+g++；`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOPTION_USE_ASSERTS=ON`，然后 `cmake --build build --target openttd` |

### D.2 ⚠ 命令行参数陷阱（曾导致验证假阳性）

**`-g <savegame>` = 加载存档；`-G <seed>` = 生成新地图。**（我们最初把两者搞反，使"旧档/自读自档"验证在对空地图做假阳性检查；现已修正并重验。）

### D.3 验证手册（全部自包含，不使用玩家存档）

> **怎么跑**：`powershell -File testrun\run_all.ps1 [-Jobs N] [-Only a.ps1,b.ps1]` 会并行跑下面这些脚本并汇总 PASS/FAIL（默认 4 个并行）；脚本也可以单独跑。2026-09-11 起脚本内部改成"探针轮询等存档加载完 + 1 秒命令间隔"（`testrun\_common.ps1` 的 `Invoke-RoRoTest`），单个脚本从约 70–190 秒降到十几秒（测试存档实测 1.2 秒就加载完，原来写死等 45 秒）。

| 脚本（`testrun\`） | 作用 | 期望输出 |
|---|---|---|
| `verify_oldsave.ps1` | 基线 exe 生成旧档 → fork 加载 | `step1: OK` / `step2: PASS (fork loads pristine 0.73.1 savegame)` |
| `m1_verify.ps1` | 自产地图生成→save→重新加载（自读自档）；Part A 若存在 `testrun\baseline.sav` 则顺带验证旧档 | `Part B: PASS (self save/load round-trip OK)` |
| `smoke_m2a.ps1` | 空地图加载 + 跑 `rvtransport` 命令 | `SMOKE: no crash detected` |
| `verify_intransit.ps1` | **收运中存档往返**：装载 → save → 重新加载 → 检查被运载状态是否保留 → 再落地 | `RESULT: PASS (carried state survives save/load, and unload still works)` |
| `verify_sim.ps1` | 在真实存档上跑"等待 → 站内扫描 → 装载"完整链路 | `sim: scan found=true attached=true carrying=1` |
| `verify_attach.ps1` | 强制装载/卸载事务、载运清单与**重量记账**（`weights: carried=… total_incl_carried=… own=…`，装卸前后各读一次） | `attach`/`detach: ok`、`carrier #6 holds 1 road vehicle`、`RESULT: PASS (attach/detach, carrying list, and the carrier weight includes the carried vehicle)` |
| `verify_details.ps1` | **载运清单的行数记账**（`rvtransport vscroll`）：装车前 `info=4 carried=0` → 装车后 `info=6 carried=1`，即"信息"页会多出表头 + 每台一行（这是清单能被滚到的前提；绘制本身需要 GUI，见 D.5 第 5 条） | `RESULT: PASS (the vehicles tab grows by the carried list: header plus one line per vehicle)` |
| `verify_part_carrier.ps1` | **按"节"调用装载代码**（多舱段船的模型）：用调试命令 `rvtransport loadfrom <载体某节> <车>` 模拟"某一节进站跑装载代码"，检查车被挂到**载体车头**上（多舱段船以前就是因为把"节"当载体而卸不下车） | `loadfrom: part #7 -> carrier #6 … attached=true`、`carried rv #8 by=6 host_part=7` → `RESULT: PASS` |
| `verify_wait_tick.ps1` | **等待状态与游戏时钟**（评审实测"卡车不再等待/装不上"的回归）：让游戏真的走 tick，检查当前订单是"站订单派生的装载中订单 + 等待旗标"时等待状态**保持**（修复前必被清掉），以及把该旗标取消后等待状态**被放弃** | `waiting flag readings: set \| set \| clear` → `RESULT: PASS` |
| `verify_unload_match.ps1` | **装载推进订单 + 卸载比对**（多段接驳的关键）：装载后卡车的当前订单必须前进到"在此被卸下"那条（测试存档里 `3 -> 4`）；随后 `detach` 到**不是**它声明的那一站必须失败（留在车上），`detach` 到它声明的那一站必须成功 | `order index readings: 3 -> 4`、`detach: failed (on board=1)`、`detach: ok (on board=0)` → `RESULT: PASS` |
| `verify_release.ps1` | **释放路径**（与载体销毁同一函数）：装载 → `rvtransport release` → 检查状态 | `RESULT: PASS (carried vehicle released and back on the road)` |
| `verify_user_save.ps1` | 订单命令链验证（`rvtransport modify` load/unload/dest） | `modify: OK` |
| `verify_toggle.ps1` | 勾选规则（与订单窗口共用 `RVTransportToggleOrderFlag()`） | `RESULT: PASS` |
| `verify_filter.ps1` | 筛选条件（无条件/货物命中与不命中/空车/最短等待超时与关闭） | `RESULT: PASS` |
| `verify_slot.ps1` | **路签判据**：非法路签被拒 → 建路签并保存 → 未持有=跳过 → 加入=装 → 移出=跳过 | `RESULT: PASS` |
| `verify_destroy.ps1` | 载体被销毁时整挂一起消失 | `RESULT: PASS` |
| `run_selftest.ps1` | （**已停用**）批量用户存档跑 selftest——用户存档过大/带 NewGRF，不适用 | — |

> 脚本都会在启动后立刻 `pause`：早先的 `detach: failed` / `RESULT: FAIL` 全是**测试时序**假故障——等待存档加载的 45 秒里游戏仍在跑，载体自己把车卸了（不是代码问题）。

### D.3b 人工实机走查记录（评审玩家实测）

| 项目 | 结论 | 备注 |
|---|---|---|
| 铰接（多节）道路载具装载/运输/卸载 | ✅ **通过**（玩家实测"暂时没问题"） | 需要含铰接车的 NewGRF（默认内容没有铰接道路车） |
| 船 / 飞机作为载体 | ✅ **通过**（玩家实测"暂时没问题"） | 走的是与火车完全相同的代码路径 |
| 载体事故销毁 → 被运载车辆的去向 | ✅ **效果达成** | 被运载的车**不会先显示"被撞毁"外观**（它在载体出事时由 `RVTransportDestroyCarriedVehicles()` 直接删除），但**残骸随列车一起清除**，没有"卡车被传送到上一个车站继续跑"的现象——与设计（"像车厢一样一起没"）一致；视觉上少了一段"卡车残骸"的中间画面，属于可接受的实现取舍 |
| 手动改命令后的等待状态（M11b） | ⏳ 待复测 | 跳到下一条调度 / 命令回库后应当不再显示"等待被运载" |

### D.4 调试/验收命令 `rvtransport`

```
rvtransport                      # 帮助
rvtransport list                 # 列出车辆 ID / 类型 / 状态 / 载运数
rvtransport state <vehicle_id>   # 打印 RoRo 状态（flags/tile/hidden/宿主/节号/重量）
rvtransport wait <vehicle_id> on|off
rvtransport orderflag <vehicle_id> load|unload|dest|wait   # 给当前订单与订单列表项打旗标
rvtransport modify <vehicle_id> <order_nr> load|unload|dest|wait  # 走与 GUI 相同的命令路径
rvtransport sim <carrier_id> <rv_id>   # 一键链路仿真（等待 → 站内扫描 → 装载）
rvtransport attach <carrier_id> <rv_id> [force]
rvtransport detach <carrier_id> <station_id>
rvtransport carried <carrier_id>       # 列出所载道路载具（编号/货物/重量/声明卸货站）
rvtransport parts <carrier_id>         # 逐节：cargo/cap/stored/rv_capacity=…t/rv_used=…t/holds_rv=…
rvtransport vscroll <train_id>         # 详情窗每页行数（含"信息"页的载运清单记账）
rvtransport release <rv_id>      # 紧急释放（与载体销毁走同一函数）
rvtransport selftest             # 自动收运→落地并判定 PASS/FAIL（需要地图上有车）
```

> ⚠ `delete_vehicle_id` 只在**非专用服务器**（有 GUI 的客户端）里注册，专用服务器（`-D`）下不可用，因此"载体被销毁"的自动化验证改用 `rvtransport release`（同一释放函数）。

### D.5 进度与待办

- ✅ **M1**：XSLFI_ROAD_VEH_TRANSPORT + 5 个 Vehicle 字段 + XSLF 门控序列化；旧档兼容与自读自档已用真实档验证通过。
- ✅ **M2a**：`roadveh_transport.{h,cpp}` 收运/落地事务（Stopped+Hidden+哈希移除 / 路站格恢复）、容量-重量判定、调试命令。
- ✅ **M2b**：`LoadUnloadVehicle` 读订单参数块执行装/卸；RV 站订单带 `ORVTF_LOAD` 时进入等待态并跳过普通装卸。
- ✅ **M3a/M3b**：目的地匹配筛选（`ORVTF_MATCH_DEST`，"不匹配即跳过"）；订单类型白名单修复（"不能执行这个命令"根因）。
- ✅ **M4a**：被运载车辆从 tick 缓存/每日处理/经济/列表/组/基建统计/联机统计/灾难/绘制中豁免（仿 GVSF_VIRTUAL）。
- ✅ **M5a–d**：订单窗口入口（装货方式下拉三项）、按载具类型文案、订单行与按钮状态显示、状态串、启用装载时默认打开目的地匹配。
- ✅ **M6**：载体"等待道路载具"（`ORVTF_WAIT`，实现方式是在 `LoadUnloadVehicle` 里压住 `finished_loading`——语义与 "Full load" 完全一致，**无限等待**，引擎没有超时兜底）；载体状态串追加"正在运载 N 辆道路载具"。
- ✅ **M7**：卖出保护（被运载车辆 / 载有车辆的载体都不能卖）；载体销毁时**连同所载车辆一起销毁**（`Vehicle::PreDestructor` → `RVTransportDestroyCarriedVehicles`，语义同火车车厢出事）；装载时释放原停车位（`RoadStop::Leave`）并把车辆状态改成行驶态，避免 bay 泄漏与二次释放；载体类型收紧为火车/船/机。
- ✅ **M7b（本轮修正）**：被运载车辆**仍留在车辆/分组列表里**（不再隐藏），**定位与跟随镜头指向其载体**（`RVTransportGetFollowVehicle`，接入 viewport.cpp 的每帧跟随、车辆窗口"定位车辆"、window.cpp 与 vehicle_gui.cpp 的落点计算）；`rvtransport release` 保留为调试命令与读档兜底通道。
- ✅ **M9：铰接（多节）道路载具**：装载/卸载/释放/销毁全部改为**按整挂（front + 各铰接节）**处理——每一节都要设 `RVTF_TRANSPORTED`、Hidden、逐节从路网哈希摘除（弯道上各节本来就在不同格）；卸载与释放时整挂落到同一格并按"出库"方式散开（与 `RoadVehLeaveDepot()` 同构）；重量沿用引擎的 `gcache.cached_weight`（本身就是整挂重量，`CargoChanged()` 汇总所有节）；计数/列表/读档校验一律以车头为准（`IsFrontEngine()` 过滤，避免整挂被重复计数或只放走一节）；`rvtransport state` 逐节打印 `part N: ... hidden=... state=... artic=...` 便于核对。
  - ⚠ **实机走查待做**：原版内容**没有铰接道路车辆**（铰接客车/卡车都来自 NewGRF），所以这一项的端到端验证需要一局加载了含铰接车的车辆集（如本地 `chinasetbuses.grf`）的存档；手测步骤见 `manual-test-guide.zh.md` 第 8 节。单节路径的回归（attach/detach/intransit/sim/toggle/release/destroy）已全绿。
  - 📌 引擎自身约束（非本分支引入）：**铰接车无法进入港湾式停车位**（`RoadStop::Enter()` 直接拒绝 `HasArticulatedPart()`），因此铰接车只能在**直通式**停靠站等待/被放下。
- ✅ **M5c（修正）**：订单窗口的两处道路载具设置分别归入**装货/卸货两个下拉**，并以**勾选框**呈现（可多选、点击后原地刷新）；修掉"启用了装载却显示成'只装载去下一站的道路载具'"的显示优先级问题（根因：文案与行文本用 `if/else if` 让 `ORVTF_MATCH_DEST` 抢在 `ORVTF_LOAD` 前面，且勾选装载时无条件默认打开匹配位，连卡车自己的订单也被置位）。
- ✅ **人工验收**：玩家实测确认"卡车自动等待 → 被装载 → 被卸下 → 继续执行调度"全流程正常。
- ✅ **M8（部分）**：**收运中存档往返 PASS**（状态/宿主/节号/重量完整保留，读档后仍可落地）；**旧档兼容重跑 PASS**；自读自档 PASS；链路仿真、强制装卸、订单命令链、勾选规则、释放路径全部 PASS（脚本清单见 D.3）。
- 🔧 关键修复（都是实测暴露后定位）：`Order::AssignOrder()` 拷贝时丢弃 RoRo 旗标（车辆读到的是丢失后的当前订单）；装载时对非车头调用 `MarkDirty()` 触发 `CargoChanged()` 断言；订单类型白名单未允许新字段；订单行/按钮文案被匹配位抢占；卡车订单被误置匹配位。
- ✅ **M10：条件选择运载（参数化筛选）**：一条"装载道路载具"的订单现在可以带**筛选条件**，只有**全部满足**的等待车辆才会被装走，不满足的继续留在站里等下一班（语义与"只装载去下一站的道路载具"一致，只是判据变多）。参照 px-patch"连接车辆"的参数化风格（它是"新订单类型 + 每参数一个 MOF"，见 `train_cmd.cpp` 的 `CoupleOrderLoadOk/CoupleCargoOk/CoupleNumOk`）：
  - **判据**：候选载货状态（任意/空车/满载）、货物（任意/能载 X/正载 X）、最短等待天数、声明目的地（任意/下一站/指定车站，其中"下一站"就是原来那个勾选项）。
  - **数据**：`OrderExtraInfo` 增 5 个字段（`rv_transport_load_state`、`rv_transport_cargo_mode`、`rv_transport_cargo`、`rv_transport_min_wait`、`rv_transport_dest_station`），XSLF 特性 `XSLFI_ROAD_VEH_TRANSPORT` 版本 **1→2**，新字段按版本 ≥2 门控（旧档自动取默认=无条件）。
  - **命令**：`MOF_RV_LOAD_STATE` / `MOF_RV_CARGO_MODE`（货物经 `cargo_id` 传入）/ `MOF_RV_MIN_WAIT` / `MOF_RV_DEST_STATION`，均限车站订单；`Order::AssignOrder()` 的 extra 拷贝条件同步扩展。
  - **求值**：`RVTransportOrderAllowsCandidate(carrier, rv)`（roadveh_transport.cpp）逐条判定；装货循环与 `ORVTF_WAIT` 等待判定统一改用它（原来只判目的地）。
  - **调试**：`rvtransport criteria <车辆> <订单> loadstate|cargo|minwait|dest …`；`rvtransport state` 现在打印每节的 `cargo: type/cap/stored/group`；`sim` 输出追加 `criteria=`。
  - **验证**：新增 `testrun/verify_filter.ps1` → **PASS**（7 个用例：无条件=装、货物=自有货物=装、货物=其它=**跳过**、空车条件符合=装、最短等待 100 天=**跳过**、等待 0=装、指定声明目的地=装）。
  - 🔧 顺带修掉一个**真崩溃**（本轮实测暴露）：装载时对**直通式**停靠站直接调用 `RoadStop::Leave()` 做占用减法会触发 `roadstop.cpp:377` 断言（该站点的占用缓存可能并未把这辆车算进该入口，例如车辆停放朝向与入口不匹配时重建的结果）。现改为：先把车辆（含各铰接节）移出路网哈希，再按引擎读档时的做法**重建**该站点链基站的入口占用（`GetEntry(NE/NW).Rebuild(base)`）；停车位（bay）仍按标志释放（在状态里还带着 bay 号时调用 `Leave`）。
- ✅ **M10b：筛选条件的订单窗口入口（GUI）**：装货方式下拉在三个道路载具条目下面追加两条**循环切换项**，文本直接显示当前取值、点击切到下一个值（与 px-patch 的对接参数交互一致）：
  - `载具条件：任意 → 仅空车 → 仅满载 → 任意`（`MOF_RV_LOAD_STATE`）；
  - `最短等待：不限 → 1 → 2 → 5 → 10 → 30 → 60 天 → 不限`（`MOF_RV_MIN_WAIT`，预设表 `_rv_transport_min_wait_presets`）。
  订单行（`DrawOrderString`）现在把生效的条件逐条列出：载货状态、最短等待、货物条件（可载/正载 + 货物名）、指定目的地车站——**控制台设置的 cargo/dest 条件同样会显示**，所以不做 GUI 也不会"看不见"。新增 10 条语言串（英文 + 简体中文各一份）。
  - ⏳ 仍未做 GUI 的部分：**货物条件**与**"指定车站"目的地**（需要货物/车站选择器，目前用控制台 `cargo` / `dest` 设置，订单行会显示结果）。
- ✅ **M10c：筛选条件的完整编辑窗口**：装货下拉新增 **`筛选条件…`** 项，打开一个独立小窗口（`RVTransportCriteriaWindow`，独立窗口类 `WindowClass::VehicleRVTransportCriteria`，窗口号 = 车辆 ID，父窗口 = 订单窗口，因此**不改动订单窗口的布局**），逐行下拉设置全部判据：载货状态、货物（列出本局出现过的每种货物）、货物判据（能载/正载）、最短等待（不限/1/2/5/10/30/60 天；控制台设的自定义值显示为"自定义时间"）、声明目的地（任意/下一站/本公司或无所属的每个车站，列表显示站名）。
  - 订单一旦变化（插入/删除订单、订单列表重分配）窗口自行关闭；车辆销毁时由 `Vehicle::PreDestructor` 关闭；改动走与订单窗口相同的 `CmdModifyOrder` 路径并立即刷新订单行。
  - 技术注记：`NWidgetCore::SetString()` 只接受 StringID（**不能带参数**），所以按钮文字用"每个取值一条完整字符串"的小表；车站判据的按钮只显示"指定车站"，具体站名在**下拉列表**与**订单行**里显示。
  - 又一次踩到并修好的坑：新窗口类的 `WindowClass` 需要同时登记到 `window_type.h`、`viewport.cpp` 的"窗口类 → 所属车辆"映射与 `Vehicle::PreDestructor` 的关窗列表，否则点视口/销毁车辆时会留孤儿窗口。
- ✅ **M10d：加入"路签"判据、移除"声明目的地"**（评审反馈）：
  - **加入路签判据**（对齐 px-patch"连接车辆"的 `MOF_COUPLE_SLOT` / `CoupleSlotOk()`：判据是"候选必须是该路签的占用者"）：`OrderExtraInfo` 新增 `rv_transport_slot`（路签 ID + 1，0 = 任意），XSLF 特性版本 **2→3**（旧档自动取默认=任意）；新 MOF `MOF_RV_SLOT`（校验：路签必须存在且为**道路载具类型**）；求值用 `TraceRestrictSlot::GetIfValid(...)->IsOccupant(rv->index)`；窗口新增"路签"下拉（列出本公司/公开的**道路载具**类型路签，文本用 `STR_TRACE_RESTRICT_SLOT_NAME`）；订单行显示"路签：<名字>"。
  - **移除"声明目的地"判据**：`RVTransportGetDeclaredDestination()` 读的是卡车调度里**第一条**带"在此被卸下"的车站调度，多个卸货计划时只有第一个生效（评审判定指代不清）——GUI 行、MOF、控制台键、订单行显示全部移除；**存档字段 `rv_transport_dest_station` 保留为保留位**（feature v2 存档仍能加载）。"只装载去下一站的道路载具"开关（`ORVTF_MATCH_DEST`）保留，覆盖常见用法。
  - **调试命令**（供自动化验证，合并前剥离）：`rvtransport mkslot <名字> [上限]`（建道路载具路签）、`rvtransport slot <车辆> <路签> on|off`（加入/移出路签）；`rvtransport state` 现在还会列出车辆持有的路签。
  - **验证**：新增 `testrun/verify_slot.ps1` → **PASS**（非法路签被拒 → 建立路签并保存 → 未持有时 `scan found=false` → 加入后 `found=true` → 移出后 `found=false`）；全量回归 12 个脚本全绿、0 断言。
- ✅ **M11：评审实测 5 个问题的修复**（含一次**崩溃**）：
  1. **订单行字符串显示 `(invalid parameter)`（并极可能是那次崩溃的根因）**：我给 `{STRING}` 参数传了**已格式化的字符串**（先 `GetString(STR_TRACE_RESTRICT_SLOT_NAME, id)` 再当作参数），而引擎的参数系统只接受 StringID / 数值——参数类型不匹配时，格式化器会把栈上的垃圾当成 StringID 去查表（`crash-20260911T050825Z.log` 正是 `C0000005 read` 于 UI 线程，且命令日志显示崩溃前在暂停状态下反复切换订单字段）。修法：**只用完整字符串 + 数值/自定义参数**，并用 `format_target::append(std::string)` 直接追加成品文本（订单行的货物/路签/最短等待/载货状态全部改成这种写法）；`STR_ORDER_RV_SLOT` 改为 `{TRSLOT}` 参数。
  2. **直通站单侧被占就不能卸**：原来要求"整格没有车辆"。现改为按**路站自身的记账**判断可否放下：泊位站用 `HasFreeBay()` + 空闲入口 + 整格无车（且铰接车禁用泊位站）；直通站按**将使用的那个入口**（`GetEntry(dd)`）的 `occupied + 车长 <= length` 判断 —— 对向有空位即可卸。放置完成后调用 `RoadStop::Enter()` 让站点记账与引擎后续的 `Leave()` 成对（此前只在装载侧做了重建，卸载侧没记账）。
  3. **卸载不区分车的调度**：现在只卸"**自己的调度说要在本站被卸下**"的车（`RVTransportGetDeclaredDestination() == 本站`；**没有任何声明**的车仍会在载体的卸货站被卸下，否则它永远下不来）；调试命令可加 `force` 强制卸全部（`rvtransport detach <载体> <车站> [force]`）。
  4. **窗口前四行按钮没有交互反馈**：`CmdModifyOrder` 是**异步**执行的，且不会使本窗口失效。现在窗口用 `OnRealtimeTick()`（暂停时也会跑）轮询快照，值一变就 `UpdateWidgetTexts()` + `SetDirty()`。
  5. **卸不下时列车直接开走**（评审提问）：这是原实现的"刻意回退"——找不到空路站格就留在车上、下次停靠再试，但载体不会因此等待、也不提示。现在补成**对称的可选行为**：载体订单勾了**"等待道路载具"**时，若还有"想在本站下车"的车没下来，就继续等（`finished_loading=false`，语义同 Full load，**无限等**）；不勾仍然是"下次再试"。
  - **验证**：全量回归 12 个脚本全绿、0 断言（`verify_attach` / `verify_intransit` 的卸载改用调试 `force`，因为它们的测试车站不是车辆自己声明的卸货站——顺带印证了新规则生效）。
  - ⚠ 崩溃日志（`C:\Users\11936\Documents\OpenTTD\crash-20260911T050825Z.log`）没有符号无法逐帧定位；上述参数错配是当前最可信的根因并已消除，**若再复现请把新日志给我**（新的崩溃日志会带同样的栈）。
- ✅ **M11b：等待状态在"改命令"后不消失的修复**：道路载具进入"等待被运载"后，玩家**跳到下一条调度**或**命令回库**时，等待标记（与随之而来的 `Stopped`）没有被清掉——车辆窗口状态栏会一直显示"等待被运载"，而且因为车被我们置于停止状态，**回库命令根本不会被执行**。修法：新增 `RVTransportTickWaiting()`，在 `RoadVehController()` 的"已停止"提前返回**之前**调用：只要当前订单不再是"车站订单 + 装载道路载具"（与进入等待时的条件 `Vehicle::BeginLoading()` 完全对称），就调用 `RVTransportSetWaiting(rv, false)` 清掉标记并解除停止 ✓。
  - 顺带确认：车辆窗口状态栏里"等待被运载"后面的 `1:` 不是 bug，而是"车辆窗口显示订单编号"设置附带的订单号（`STR_VEHICLE_VIEW_ORDER_NUMBER`），与等待状态无关。
- ✅ **M11c：重量记账、载重外观与载运清单**（评审实测三点反馈）：
  1. **被运载车辆计入载体自重**：此前载体的重量完全不含车上的卡车。新增 `RVTransportGetCarriedWeightTonnes()`（按车头累加 `transported_weight`，整挂只算一次），接到 `GroundVehicle::CargoChanged()` 里——这是**引擎唯一的"重量热点"**，所有载具类型的重量缓存都在这一行产生；装卸后的重算搭载体自己的 `MarkDirty()`（它本来就会调 `CargoChanged()` / `UpdateAcceleration()` / 整列图像缓存）。**范围说明**：只有火车维护整列重量缓存（`gcache.cached_weight`，影响加速/油耗/桥重/性能页），船与飞机在本引擎里**没有**这种缓存（它们的重量不参与任何判定），因此对它们"重量"只体现在容量判定上，无需也不该另造缓存。调试命令 `rvtransport state <火车>` 现在打印 `weights: carried=Nt total_incl_carried=Nt own=Nt`，可用来直接观察。
  2. **载体外观"装满了"**：默认火车按原版规则（`cargo.StoredCount() >= cargo_cap / 2` → `_wagon_full_adder`，`train_cmd.cpp`）；NewGRF 车辆集的**真实精灵组**按 `stored * totalsets / capacity` 取图（`newgrf_engine.cpp` 的 `ResolveReal`）；**从货物量变量取图**的车辆集则读变量 `0x3C/0x3D`（"车上货量"）—— 三处都加了同一条短路（"该节载有道路载具 ⇒ 视为满载"，`RVTransportPartHoldsRoadVehicles()` / `RVTransportExtraCargoAmount()`），所以只要车上装着卡车，那一节就画成满载外观；装卸后清掉该节的图像缓存（`InvalidateImageCache()`）并标记视口重画（火车整列由 `Train::MarkDirty()` 覆盖，船/机里非车头的节由我们额外补一刀）。**默认船只/飞机没有"满载"变体图**（它们的图与载货无关），所以对它们无可见变化——这是引擎内容所限，不是没接。
  3. **载运清单**：载体详情窗口现在列出所载车辆（`载运的道路载具：` + 每台一行，`{VEHICLE}` = 车辆名 + 编号，可与车辆列表对上）。火车放在**"信息"（车辆）页的末尾**（该页本来就有滚动条，**需要滚到底部**才看得到）；船/机放在详情面板**底部**，且**窗口高度随装载数自动增减**（`DrawCarriedRoadVehicles()` + `VehicleDetailsWindow::GetVehDetailsHeight()`，装卸后由 `InvalidateWindowData(WindowClass::VehicleDetails, carrier)` 触发重排）。行数记账（滚不滚得到、会不会画过头）由 `GetTrainDetailsWndVScroll()` 负责，`verify_details.ps1` 用 `rvtransport vscroll` 守住它。控制台 `rvtransport carried <载体ID>` 可脚本化地列出同样内容（编号/货物/重量/声明卸货站）。
  4. **不做的事**（评审确认）：**等待中的卡车仍然占着停靠点的一个停车位**——保持引擎原有语义（它在站里等，本来就该占位），不改。
  5. 🔧 **本轮自己踩到并修掉的两次崩溃**：
     - **（a）`verify_destroy` 稳定复现的那次**：刷新被影响的那一节时，最初写了 `Vehicle::UpdateViewportDeferred()`。它跟"立即版" `Vehicle::UpdateViewport()` 不同，**在专用服务器（headless）上不会提前返回**：它把"新的视口哈希桶 + 车辆裸指针"塞进延迟队列，同时立刻改写车辆的 `coord`，而真正的哈希链更新要等队列结算（队列只在视口绘制里结算，专用服务器永远不结算）。于是哈希链与坐标不一致，这种状态下删除车辆（例如销毁载体）就会顺着失效指针写内存（`C0000005`，写入地址 0）。修法：这里只做 `InvalidateImageCache()` + 立即版 `Vehicle::UpdateViewport(true)`（headless 下直接跳过），重量刷新交给 `MarkDirty()`。**经验：除非能确定队列会在车辆被删除前结算，否则不要用 `UpdateViewportDeferred()`**（顺带确认 `Train::MarkDirty()` 已经会遍历整列并清图像缓存，所以最初多写的 `ConsistChanged()` 也是多余的，已去掉）。
     - **（b）评审实机崩溃 `crash-20260911T125152Z`（详情窗"信息"页必崩）**：`DrawTrainDetails()` 的**形参 `v` 被它自己的循环当成循环变量**（`for (; v != nullptr …; v = v->GetNextVehicle())`，循环结束时 `v` 必然是 `nullptr`），而我把"载运的道路载具"清单块加在了循环**之后**、却继续用 `v`：`RVTransportGetCarriedVehicles(Vehicle::Get(v->index), …)` → 对空指针读 `offsetof(Vehicle, index)`（= `0x34`）→ `C0000005` 读地址 `0x34`。**因此只要"信息"页画到编组末尾就必崩**，与车上有没有卡车无关 —— 这正是评审"找不到载运清单"的原因（清单标题在崩溃点之后才画）。定位方式是拿 exe 里的 DWARF 调试信息把崩溃日志的裸地址换算回源码（由栈底 `__tmainCRTStartup` 反推模块基址 → RVA → `addr2line -f -C -i`），直接落到 `train_gui.cpp:528`。修法：进入循环前先保存车头（`const Train *front = v;`），清单块只用 `front`；顺带这写法**天生免疫空指针**（`RVTransportGetCarriedVehicles()` 对 `nullptr` 直接返回空表）。
     - **为什么回归没抓到（重要经验）**：12/13 个脚本跑的是 `-D` 专用服务器，**根本不画窗口**（不执行 `UpdateWindows()`，精灵数据也不加载），GUI 代码等于零覆盖。我试过用"专用服务器 + 真 blitter（`-D -b 32bpp-simple`）+ 调试命令直调绘制函数"来做 headless 绘制测试：先发现 `-D` 会把 blitter 强制设成 `null`（`-b` 必须写在 `-D` **之后**），换成真 blitter 后又发现**即使车里空着**，在专用服务器上画文字/精灵本身就会在引擎内部崩（`DrawString` → `format_buffer` 析构），所以这条路走不通，相关测试钩子已全部删除。**替代做法**：把"行数记账"这类**纯逻辑**抽出来单独守（`rvtransport vscroll` + `verify_details.ps1`），绘制部分只能靠人工看 → 见手测指引 §7。
     - **新增调试命令（合并前剥离）**：`rvtransport vscroll <车辆>`（详情窗每页行数）、`rvtransport parts <载体>`（逐节 `cargo/cap/stored/rv_capacity=…t/rv_used=…t/holds_rv=…`，用来判断"车装在哪一节、那节能装多少吨"，也就解释了为什么某节/某车型看不到满载外观）、`rvtransport setcurrent <车辆> <订单号> [loading]`（把某条订单设成当前订单，可选按"刚进站装载中"处理；专门用来在专用服务器上测"订单驱动"的行为）；`testrun/probe_user_crash.ps1` 可把评审存档读进来直接打印这些信息。
  6. 🔧 **M11d：评审实测"卡车不再等待、也装不上车"的修复（等待状态被 tick 清掉）**：评审反馈——**打开稍早的存档后，卡车不再进入"等待被运载"，火车也就装不上它**。根因：`RVTransportTickWaiting()` 只承认 `current_order` 是 `OT_GOTO_STATION`，但卡车到站时 `Vehicle::BeginLoading()` 会把当前订单**转成 `OT_LOADING`（装载中订单，运输旗标仍在）**，于是**下一个 tick 就把等待状态清掉**，卡车随即按自己的调度开走 —— 火车当然找不到可装的车。修法：把 `OT_LOADING` 也算作"仍要等待"，并且当当前订单是装载中订单时**回订单列表重读那条站订单**（这样在订单窗口里取消勾选"等待被运载"也会立刻生效）。
     - **为什么以前没抓到**：老脚本全都 `pause` 着测，且 `rvtransport sim` 是在同一条命令里"设等待 + 立刻装载"，**从没让游戏走过 tick** → 新增脚本 `verify_wait_tick.ps1`（让游戏真跑 1.2 秒再检查），它在**未修复的构建上正好失败**（`set | clear | clear`），修复后 `set | set | clear` → PASS ✓。
     - **顺带的可发现性修复**：评审一直找不到"载运清单"。清单本身在**详情窗"信息"页的最底部**（要滚到底），现在额外在**详情窗顶部**（不用切页、不用滚动）常显一行 `载有 N 台道路载具`（复用状态栏字符串，只在载体真的装着车时出现），并把"信息页要滚到底"写进手测指引。
  7. 🔧 **测试提速**（评审提问"回归能不能快点、能不能并行"）：实测测试存档 **1.2 秒**就加载完，而脚本原来写死等 **45 秒**、命令之间还各等 4–5 秒 —— 时间几乎全花在 `Start-Sleep` 上。现在：①`testrun/_common.ps1` 的 `Invoke-RoRoTest` 用"`save <探针>` 命令 + 轮询存档文件"判断"加载完/控制台可用"，命令间隔 1 秒（单个脚本 70–190 秒 → 十几秒）；②新增 `testrun/run_all.ps1` 并行跑全部脚本并汇总（默认 4 并行，实测 6 并行跑 6 个脚本墙钟 172 秒，瓶颈是还没改造的老脚本）；③脚本一律带 `-x` 运行（不回写配置文件，避免测试运行改掉 `build/roro-test.cfg`，这坑我踩过一次：端口实验把 `server_port` 改成了 4301）；④`-D :<端口>` 可给每个并行实例分配独立端口（实测同端口并行也能跑，只是会打一条绑定失败警告）。
  8. 🔧 **M11e：多舱段（多节）船装不上/卸不下车的修复**：评审反馈"**不同舱段的船只似乎无法正常卸载车辆**"。根因：**多舱段船的每一节都是独立载具**，各自进站并各自跑 `LoadUnloadVehicle()`，而**被运载的道路载具是挂在"车头"上的**（`RVTransportAttach()` 要求 `part->First() == carrier`，记录的是车头 index）——
     - 卸载侧：`RVTransportDetachAtStation(front /*那一节*/, st)` 在找 `transported_by == 该节 index` 的车，**永远找不到** → 一辆也卸不下来（评审看到的现象）；
     - 装载侧：`RVTransportAttachAuto(那一节, …)` 会被 `part->First() != carrier` 直接拒掉 → 只有**恰好是车头的那一节**能装车（多舱段船等于只有一个舱能收车）；
     - 等待判定：`RVTransportCountOnCarrier(那一节) == 0` → 载体的"等待道路载具"也会算错。
     修法：`LoadUnloadVehicle()` 里的三处 RoRo 代码（装卸、等待装载、等待卸载）统一改用 **`front->First()`** 作为载体（火车本来就是车头，行为不变）。**验证**：新增 `verify_part_carrier.ps1`，用调试命令 `rvtransport loadfrom <某一节> <车>` 直接按"某节进站"调用装载代码，检查车被挂到车头上（`attached=true`、`by=6 host_part=7`）→ PASS；全量回归 15 脚本 ALL PASS。⚠ 真正的多舱段船需要 NewGRF 支持（`multi_part_ships`），自动化测试用"火车车厢当那一节"覆盖同一条代码路径，实船由评审复测。
  9. 🔧 **M11f：多段接驳的"下车点"设计（评审定案，替代原来的"扫订单表第一条卸货订单"）**：评审复测发现船仍然卸不下车，用 `rvtransport dump` 读存档定位到**真正原因**——评审那辆卡车是**多段接驳**的：`2: Windywick 废车场 等待被运载 → 3: Jellywig 西站 在此被卸下 → 4: Tanglewood 西站 等待被运载 → 5: Tanglewood 码头 在此被卸下`（先用火车运一段，自己开一段，再用船运一段）。我原来的规则读"**订单表里第一条**带'在此被卸下'的订单"，于是船到 `Tanglewood 码头` 时读到的是第一段的 `Jellywig 西站` → 判定"你要在别站下"，把车留在船上。**评审提出的方案**（采用）：
     1. **装载时把被运载车辆的订单推进到下一条**（`IncrementImplicitOrderIndex()` + `current_order.Free()` + `ProcessOrders()`，与引擎自己"走完一条订单"的写法一致）→ 它在被运载期间的**当前订单就是"在此被卸下"那一条**；
     2. **卸载时比对**：`载体卸货订单的站 == 卡车当前订单的站`（且该订单带"在此被卸下"）才放下；没声明则兜底卸在载体卸货站。
     附带好处：**修掉一个隐藏 bug** —— 原来放下车时不动订单索引，车的当前订单还停在"上车站等待被运载"，放下来后会**往回开**；现在放下时它的当前订单就是那一站，会像正常到站一样继续跑自己的下一条。
     另加 **`ORVTF_UNLOAD_ALL`（订单窗第 5 个勾选框 `卸载所有道路载具`）**：载体说了算，忽略各车自己的声明全部放下（玩家的兜底开关）。
     老存档迁移：`RVTransportValidateAfterLoad()` 里对"已在车上但当前订单仍在等车（带 RV_LOAD 旗标）"的车补一次推进。
     **⚠ 这一版自己踩到并修掉的崩溃**（评审实机"一卸载就闪退"，两份日志 `crash-20260911T160802Z` / `crash-20260911T160833Z`）：`Assertion failed at line 1947 of economy.cpp: front->current_order.IsType(OT_LOADING)`，发生在 `CallVehicleTicks → LoadUnloadStation`（站 9 / 站 7，正是卡车**上车的那些站**）。根因：卡车到站等待时 `Vehicle::BeginLoading()` 已把它登记进**上车站的 `loading_vehicles` 列表**，而我们把它带走时**没有把它摘掉**（引擎只在正常 `LeaveStation()` 或销毁时摘）。以前它留在车上、`current_order` 还是 `OT_LOADING`，列表扫到它也无害；**这次把它的订单推进到下一条之后**，一旦卸载（车被解除停止状态）该站就把它当"正在装货的车"处理 → 断言。修法：`RVTransportAttach()` 里把它从上车站的 `loading_vehicles` 摘掉，并按 `Vehicle::PreDestructor()` 的做法收尾（`HideFillingPercent` + `CancelReservation`）。**测试补强**：`verify_unload_match.ps1` 在卸载后增加 `unpause` + `pause`（让游戏真的走 tick）——这类"tick 之后才炸"的问题只有跑起来才能抓到（我之前所有脚本都 `pause` 着测，这是第二次栽在同一类盲区上）。
     **验证**：新增 `verify_unload_match.ps1`（`3 -> 4` 的索引推进 + 错站 `detach: failed` / 正确站 `detach: ok`）→ PASS；用评审存档实测 `declared_dest=10 'Tanglewood 码头' -> unloads here: true` ✓；全量回归 **16 脚本 ALL PASS**。
  10. 🔧 **调试工具扩充**（都为定位上面这些问题而加，合并前剥离）：`rvtransport dump`（一次性打印所有卡车/载体/逐节/所载车/声明卸货站/所属订单站，并对每个载体判断"车能否卸在它当前订单的站"及原因）、`rvtransport station <站> [车]`（该站公路停靠站格与占用、能否放下）、`rvtransport setwaiting <载体> <车>`（把车置为在该载体订单站等待，不装载）、`rvtransport loadfrom <载体某节> <车>`（按"某节进站"调用装载代码）、`rvtransport setcurrent <车> <订单号> [loading]`（把某条订单设为当前订单）；`rvtransport orders` 现在打印**每条订单的目标站编号 + 站名**（游戏界面只显示名字，而两个站可以重名，这一条是这次排查的关键）；`rvtransport modify/orderflag` 支持 `unloadall` 旗标。另有通用探针 `testrun/probe_save.ps1 -SavePath <存档> -Commands …`，可直接把评审存档读进来跑任意调试命令。

- ✅ **M11j：列车详情窗的 `载运`页 + 点击跳转（评审要求）**：`TrainDetailsWindowTabs` 增 `TDW_TAB_CARRIED`、`VehicleDetailsWidgets` 增 `WID_VD_DETAILS_CARRIED`（两者按"减法"对应，新项都放末尾并加 `static_assert`），火车详情窗多出第 6 个标签按钮（`载运` / `Carried`）。该页一车一行（编号+车名、记录重量、货物、声明卸货站），空着时写"本列车没有载运道路载具"；**点某一行 → 打开那辆车的窗口并把镜头带过去**（行→车辆映射 `GetTrainDetailsCarriedVehicleRow()`，调试命令 `rvtransport row <火车id> <行>` 打印同一映射，无需 GUI 即可验证）。**船/机没有标签栏**，其清单仍在面板底部，但**每行也可点**（`DrawCarriedRoadVehicles()` 记录行区域 + `CarriedVehicleAtPoint()` 命中判定）。行数记账已并入 `GetTrainDetailsWndVScroll()`（一车一行，空时一行"没有"），`verify_details.ps1` 用 `rvtransport vscroll` + `rvtransport row` 守住；全量回归 17 脚本 ALL PASS。视觉与点击本身需要 GUI，由评审复测。
- ⏳ **待办**：联机 sync test；性能验收（500 台待运 + 20 节车单次装货扫描）；合并前剥离 `rvtransport` 调试命令；**M11j 的人工复测**（列车 `载运`页的外观与点击跳转、船/机面板清单的行点击）。

---

## 附录 E：为什么"目的地匹配"没有做成条件判别式（评审问答）

**问题**：能不能完全依靠（JGRPP 现成的）条件判别式来表达"只装目的地一致的车"，做出来之后是不是可以把"只装载去下一站的道路载具"这一项删掉？

**结论：不能（用现有条件变量），这一项必须保留。** 理由有两层：

1. **条件变量集里根本没有这个维度。** `OrderConditionVariable`（`src/order_type.h:217`）的全部取值是：载货百分比、可靠性、最高速度、年龄、需要维修、无条件、剩余寿命、最高可靠性、**车站有某货物在等**、车站接收某货物、空站台数、百分比跳转、**PBS 槽占用**、**车辆是否在某槽内/槽组内**、某货物载货百分比、**车站某货物等待量（可带"经由某站"）**、计数器值、时间日期、时刻表、调度槽、倒车。它们全部描述"载体自己/它所在车站"的状态；**没有任何一项能看到"某辆等待中的道路载具的目的地"**。`CargoWaitingAmount` 虽然能带"经由站"，但那是**货物**在车站货位的等待量——被运载的道路载具不是货物，它不进车站货位、不进货流图（见 D.5 的设计取舍），条件系统看不到它。
2. **即使新增一个变量，语义层级也不对。** 条件判别式的粒度是"**整条订单跳不跳**"（对载体整体做一个布尔判断），而目的地匹配的粒度是"**在装载循环里逐辆候选车做筛选**"（`RVTransportFindWaitingAtStation(st, carrier, match_destination)` 决定"这一辆装不装"）。前者最多能表达"若本站没有开往下一站的车，就跳过整条装载订单"，但**表达不了"同一站里装 A 车、不装 B 车"**——而后者才是这个功能的核心（"不匹配即跳过"）。

**将来若要更进一步**，可选（都不替代现有勾选框，只是叠加）：

- 新增只读条件变量"本站在等、且目的地为 X 的道路载具数量"，让玩家用条件订单做"没车就直接开走"这类**粗粒度门控**；实现代价 = 新变量 + 条件窗口取值 UI + 存档字段；
- 把匹配位从"独立勾选框"改成"下拉里两个互斥的装载项"（`装载道路载具` / `只装载去下一站的道路载具`）。这能让引擎原生的单选勾选语义完全贴合状态，但会与"等待"位形成 4 种组合（装载×匹配×等待），下拉需要 4+ 项，反而更啰嗦；因此当前选择"勾选框 + 组合规则"（见附录 D 的 M5c 条目）。


