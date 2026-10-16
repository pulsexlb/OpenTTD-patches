# pxp-2610.2 desync 复现与分析记录 (2026-10-03 晚)

## 复现结论
- 5 个服务器 desync 存档（090940Z/093753Z/093819Z/103102Z/111950Z）本地全部可复现。
- 复现方法：desync/ 下的服务器存档用本地 pxp-2610.2 构建（build/openttd，含 RANDOM_DEBUG）作 dedicated server，
  null 客户端 `-v null:until_exit=true -s null -m null -n 127.0.0.1#255` 加入，通常 10~90 秒内 desync。
- 签名一律为 A 类：`Desync subframe mismatch: 0x…, VEH_TRAIN, seed[, state checksum]`。
  （与线上日志的 5 次失步中 3 次相同；另有 2 次 VEH_EFFECT 是其下游表现。）
- 注意：desync 报告写在 desync-server-*.log 文件里，控制台不一定有 "subframe" 字样，之前脚本按控制台 grep 全部漏检。

## 根因定位（statecsum + random 调用流对齐）
- 双端 random 消耗流 + state checksum 流在 desync 帧之前完全一致（逐条对齐，含 checksum 值）。
- 首个分歧帧 0x20E5（desync 帧 0x20E6 上一帧）：vehicle 0（公司1列车）
  - 客户端：ShowVisualEffect gate-skip：cur_speed=117 > max_speed=116（超速跳过烟雾）
  - 服务器：没有调用该 gate（speed ≤ 116）
  - 而双方 train_cmd.cpp:9799 的 x/y/track 校验和输入完全一致 → 位置相同、速度不同。
- 结论：列车 cur_speed（或相关缓存，如 cached_max_speed/加速度输入）在位置类校验和覆盖之外分叉，
  与 2609.9 时代的 VENC/缓存家族问题同源但走的是速度通道；解挂/对接订单的列车同样可触发。
- 已在 train_cmd.cpp Train::Tick 的校验和日志中加入 cur_speed/vmax/vehstatus/reverse_distance，
  下一步：重跑复现 → 对齐日志找出 v0 速度从哪个 tick、因哪个缓存分叉。

## 环境
- build/openttd：当前 HEAD(px-2610.2+2) + 未提交插桩（repro-20261003/instrumented-worktree.patch）。
- 插桩内容：UpdateVehicleTileHash 站点审计（cachecheck 输出 last remove/insert site）；
  ShowVisualEffect gate-skip 探针；Train::Tick 速度探针；RANDOM_DEBUG 开启 statecsum/random 调用流。
- statecsum/random 流写入 save/autosave/random-out-<pid>-0.log（每端一份，数百 MB）。
- 已保存的 desync-server-*.log 为本地复现报告；*.gz 为 151333Z desync 的双端 random-out 流。

## 2026-10-04 更新：新复现一局 + 精确钉到帧和机制

### 环境/工具（本次新增）
- repro-env/pxp-test.sh：单存档一键测试（setup/run/stop/status）。
- repro-env/pxp-par.sh：5 存档并行（端口 3979/3981/…/3987），客户端按轮加入，触发即收集。
- repro-env/diff_streams.py：双端 random-out 流按帧对齐 + 逐行 diff。
- 两个关键坑已修：client_name 必须写数据目录 private.cfg（否则静默不加网络）；desync=2 的
  "Order destination refcount map not valid" 是设计内空操作但每秒写 1.2MB inconsistency 存档，
  会把 /tmp 写满拖死游戏（本次已改用 desync=1）。

### 今日复现
- desync-server-20261004T031256Z.log（存档 111950Z，第 2 轮客户端加入后 ~40s）：
  `Desync subframe mismatch: 000011F5, VEH_TRAIN, seed, state checksum`——与昨日签名一致。
- 证据齐套（双端报告 + 双端 random-out 流）：repro-env/results/par-20261004T111300-i4/
  （stream-server.log / stream-client.log，已去 NUL 为 s.log/c.log）。

### 逐帧定位（stream 对齐结果）
- 0x0D40（客户端加入）～0x11E9：双端完全一致。v90（车厢编组头）停在 (12328,10383)，
  cur_speed=0、vmax=30（无动力部分限速）、vstat=24，等待对接。
- 0x11EA：**服务器侧 v90 突然启动**（cur=2，且 vmax 30→86，即编组缓存最高速恢复为带动力值），
  客户端侧仍 cur=0/vmax=30。附近无任何移动列车 → 非物理接触，是**对接等待的内部决策**
  （claim/等待释放）只在服务器上成立。
- 0x11EA～0x11F4：每帧仅 v90 一行不同（cur/vmax），其余 404 行全同——速度不在校验和输入里
  （train_cmd.cpp:9802 只喂 x/y/track），所以校验和看不出分叉。
- 0x11F5：v90 位置走出一步（y 10383→10384），位置入校验和 → state checksum 分叉，
  同时服务器侧运动产生随机消耗 → seed 分叉。与 desync 报告帧完全吻合。

### 结论（修正昨日推断）
- 不是"速度缓存自己分叉"，而是**解挂后等待对接的部分，其"是否满足对接条件/释放出发"的判定
  依赖了未纳入 UpdateStateChecksum 的状态**（couple_target/claim、预定路径、信号/预定等 PB 状态），
  双端在此类状态上早已不同，等到它决定 v90 是否出发时才表现为可见行为差。
- 修复方向：把对接判定所依赖的状态（claim/couple_target、预定、接触点判定的输入）纳入
  UpdateStateChecksum，再跑一轮即可把分叉的状态项精确报出来。

### 待办
1. 在 ClaimCoupleTarget/GetClaimedCoupleTarget/IsCoupleApproachPathClear 的输入上加探针重跑，
   报出第一个不同的判定输入。
2. 检查 OT_WAIT_COUPLE → OT_GOTO_COUPLE 的推进条件中所有非校验和状态。

## 2026-10-04 修复尝试：同版本载入不再擦除 couple claim 状态

### 根因确认（帧级证据）
- 插桩把 couple_target/couple_claim_cost 纳入 UpdateStateChecksum 并打印后：
  desync 帧 0x0DA7 首个差异行：服务器 v0 `ctgt: 5` vs 客户端 `ctgt: 10`——
  移动端火车的对接目标认领在"存档→下载→载入"往返中不一致。
- 载入 netsave0.sav（客户端下载的快照）验证：CPLM 块里有 carrier 侧的 claimant
  （v3.claimant=0、v108.claimant=90 等），但 mover 侧的 couple_target 缺失/被擦除；
  客户端第一个 tick 重新推导出不同的目标，与服务器保留的值分叉。
- 擦除者是 AfterLoadValidateCoupleClaims：只在新客户端载入时运行（服务器从不重载），
  对同版本数据做了"单边状态"清洗 → 客户端与服务器状态不对称 → desync。

### 修复（src/saveload/afterload.cpp）
- AfterLoadValidateCoupleClaims：当前版本（本构建写的数据，含网络加入快照）只
  SlApplyCoupleClaims，不再擦除；擦除逻辑仅保留给旧版本迁移。

### 效果（5 存档并行、10 轮）
- 修复前：第 2 轮 5/5 实例全部触发。
- 修复后：前 6 轮 0 触发；第 7-8 轮 3 例"触发"但双端报告均为 0 字节空文件，
  客户端已回片头游戏——疑似断连而非真 desync，待查。实例 3、4 十轮无触发。

### 待办
1. 确认残余"触发"是否真 desync（抓断连原因）。
2. 更长周期验证 + 真实服务器回归。
3. 插桩与修复分开提交；探针验证通过后可移除。
