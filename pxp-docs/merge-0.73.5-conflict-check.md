# jgrpp-0.73.5 合并冲突预检报告

预检时间：2026-11-05。全部结论来自 `git merge-tree --write-tree` 只读预演，**未执行任何实际合并，未改动任何分支或工作区文件**。

## 结论

有冲突，而且语义冲突比文本冲突严重得多。文本冲突 19 个文件 / 26 个冲突块，属于可解范围；真正卡住合并的是两件事：**PXP 自定义机场系统与上游 0.73.5 机场/NewGRF 重构的架构级不兼容**，以及 **XSLFI 存档 feature 编号冲突（会静默损坏旧存档）**。不要直接 `git merge` 然后按标记挑一边。

另外，「先合并到 pxp-jgrpp，再合并到 px-patch」这个流程按当前仓库状态是绕路且会重复解冲突，见第 2 节。

---

## 1. 基线与拓扑

| 项 | 值 |
| --- | --- |
| `jgrpp-0.73.5` | `5389ea187fe6f81a0ca3d61569330401ec92a431` (2026-11-01) |
| `jgrpp-0.73.4` | `cd2235f78365bad1dc6769f3b8f8b1e66b24fdf8` |
| merge-base(0.73.5, px-patch) | 正好是 0.73.4 |
| 0.73.4→0.73.5 | 120 commits（27 first-parent，其余为 master 合入），149 文件，+1514 / -938 |
| 本地 `pxp-jgrpp` | `8b6297897d3aacc0d1a0bf7870b8324286f0ccb4` |
| 本地 `px-patch` | `8b6297897d3aacc0d1a0bf7870b8324286f0ccb4`（与 pxp-jgrpp 同一个 commit） |
| `origin-gh/pxp-jgrpp`、`origin-gitea/pxp-jgrpp` | `e3e836fce143fae2c882d3fb8d72014e2bd811ec`，是本地 px-patch 的祖先，落后 67 commits |
| 本地 `jgrpp` | `d1b840b86b`，已包含 0.73.5 且 ahead 1 个提交（revert #16114） |

好消息：merge-base 精确落在 0.73.4，`jgrpp-0734-bridge` 已经把 0.73.3 时代的历史重写接好，本次没有「同内容不同 commit」造成的伪冲突。坏消息是下面两节。

---

## 2. 两段式合并流程本身有问题

- **按本地分支**：`pxp-jgrpp` 与 `px-patch` 指向同一个 commit。第一步合并完成后，第二步是 no-op / fast-forward，没有意义。
- **按远程 `pxp-jgrpp`（e3e836fce1）**：它是 px-patch 的祖先且落后 67 个提交。先把 0.73.5 合到这个旧基线，再合进 px-patch，等于把 0.73.5 的 120 个提交与 PXP 后续 67 个提交重新做一次三方合并。预演（`commit-tree` 造临时对象模拟第一步结果，再 `merge-tree` 到 px-patch）显示第二步会**再产生冲突**（`.ottdrev-vc`、`src/order_gui.cpp`），并有十几个文件被重复 auto-merge。重复劳动且容易解出不一致的结果。
- **建议**：一次性在 `px-patch`（或已对齐的本地 `pxp-jgrpp`）上合并 tag，得到合并提交 M，然后让两条分支都指向 M。若必须保留「集成分支先行」的语义，先把 `pxp-jgrpp` 对齐到 `8b6297897d`。

两个目标（本地 px-patch / 远程 pxp-jgrpp）的文本冲突清单**完全一致**，都是下面 19 个文件。

---

## 3. 文本冲突清单（19 文件 / 26 个冲突块）

**modify/delete ×7**（PXP 删除、上游修改）：`.github/workflows/` 下的 `ci-build.yml`、`ci-linux.yml`、`ci-macos.yml`、`ci-mingw.yml`、`ci-nightly.yml`、`ci-windows.yml`、`codeql.yml`。

**content 冲突 ×12**：

| 文件 | 冲突块 | 性质 |
| --- | --- | --- |
| `.ottdrev-vc` | 1 | 版本元数据：`pxp-2611.1-p3` vs `jgrpp-0.73.5` |
| `README.md` | 1 | 标题一行 |
| `src/aircraft_cmd.cpp` | 7 | 同一批函数两侧重写，git 合并错配 |
| `src/lang/simplified_chinese.txt` | 3 | eints 译文 vs PXP 自定义措辞 |
| `src/newgrf/newgrf_act0_airports.cpp` | 4 | 机场布局解析两套实现 |
| `src/order_gui.cpp` | 3 | 上游拖拽修复 vs PXP 订单窗口重构 |
| `src/pathfinder/follow_track.hpp` | 1 | 纯格式（`inline static` / `static inline`） |
| `src/sl/afterload.cpp` | 1 | 机场 afterload API |
| `src/sl/extended_ver_sl.h` | 1 | **存档 feature 编号** |
| `src/station.cpp` | 1 | catchment 计算两套实现 |
| `src/station_cmd.cpp` | 2 | PXP 替换了 `CmdBuildAirport`，上游重写了 227 行 |
| `src/train_cmd.cpp` | 1 | consist cache 刷新两套实现 |

另有 `release-linux.yml`、`release-windows.yml` 等约 56 个文件自动合并成功。

---

## 4. 语义冲突

### P0-1 机场子系统架构不兼容 —— 最大工作量

上游 0.73.5 重构了 NewGRF 机场布局：`ValidateAirportLayout()`（重复 tile 校验）、`definition_size` 边界检查、无效 tile ID 拒绝、`tile.gfx` 单值 + 0xFF 空洞处理；同时把大量 `airport.tile != INVALID_TILE` 换成 `airport.IsEmpty()`，catchment 改从 `GetSpec()->catchment` 取，afterload 改用 `GetSpec()->size_x/size_y`。

PXP 的自定义机场系统（`src/newgrf_airport.h`、`airport.h` 相对 0.73.4 改了 197 行、`airport.cpp` +954 行、`station_base.h` -140 行、`airport_cmd.cpp`、`newgrf_act0_airports.cpp`）用的是另一套：`as->layouts[n]`、`tile.gfx[rotation]` 数组、`AirportCatchmentRadius()`（基于 `AirTypeInfo::catchment_radius`，PXP 的 air type 动态覆盖半径）。

具体后果：

- `newgrf_act0_airports.cpp`：theirs 侧 `tile.gfx = tempid` 与 PXP 结构里的 `tile.gfx[]` 数组**类型不匹配，必然编译失败**。PXP 的 grf_version<=8 兼容分支与上游新的多 layout 校验无法共存。
- `station.cpp`：取 theirs 会丢掉 PXP 的 air type catchment（多格机场功能），取 ours 会丢掉上游 #16097 站牌移动后的 catchment 更新。
- `station_cmd.cpp`：冲突里 ours 侧整段为空（PXP 替换了 `CmdBuildAirport` / `UpdateAirportsNoise`），theirs 是上游 227 行重写版。任取一边都丢功能。
- `aircraft_cmd.cpp`：7 个块里 ours 多处为空、theirs 是整块上游代码（`AircraftController`、`GetTargetAirportIfValid`、直升机旋翼等），是典型的同函数两侧重写导致的对齐错乱，必须逐块人工合并。
- 上游只改了 `newgrf_act0_airports.cpp` 等文件，**没有改 PXP 自定义的 `airport.h` / `airport.cpp` / `station_base.h`**，合并后同一代码库里两套 API 并存。

这不是「解冲突」，是把 PXP 多格机场 patch 移植到 0.73.5 的机场 API 上。至少涉及 `aircraft_cmd.cpp`、`station.cpp`、`station_cmd.cpp`、`sl/afterload.cpp`、`newgrf_act0_airports.cpp`、`script/api/script_order.cpp`、`newgrf_station.cpp`、`newgrf_engine.cpp`、`disaster_vehicle.cpp` 九个引用了新 API 的文件。

### P0-2 XSLFI 存档 feature 编号 —— 静默数据损坏

- 上游在 `XSLFI_DOUBLE_ENDED_SHIPS` 之后插入 `XSLFI_SCRIPT_RANDOMIZER`；PXP 在**同一位置**插入 `XSLFI_TRAIN_SERVICE_AT_STATION`、`XSLFI_TRAIN_NO_DEPOT_TEMPORARY_STOP`，并在枚举末尾又追加了 20+ 个自定义 feature。
- 枚举值就是存档 SLXI feature 位图的索引。若解决冲突时把上游新 feature 插在中间，`XSLFI_PR_13745_APPLIED` 及其后所有 feature 编号整体 +1，PXP 已发布存档（`pxp-2611.1-p3`）的 feature 版本数组全部错位 → 旧存档加载错误或被拒。
- `src/sl/extended_ver_sl.cpp` 的 `_sl_xv_sub_chunk_infos` **自动合并成功**：上游的 `XSLFI_SCRIPT_RANDOMIZER` 条目落在 230 行附近，PXP 的两个条目在 154/155 行。文件不报错，但枚举顺序与条目顺序的一致性没人保证 —— 教科书式的静默语义冲突。
- 建议：把上游新 feature 放到 PXP 自定义区段之后（或给枚举显式编号），保证现有编号不变；同步核对 `_sl_xv_sub_chunk_infos`、`_sl_xv_feature_versions` 与 `XSLFI_SIZE`；用旧存档做加载回归。

### P0-3 上游 saveload / 类型 API 变化（自动合并成功，需编译验证）

- `sl/company_sl.cpp` 新增 `max_loan` 存档字段；新增 `src/sl/randomizer_sl.cpp`（已确认合并树里 `src/sl/CMakeLists.txt` 正确带入 ✓）；脚本 randomiser chunk 改为按上游格式保存并加 extended version tag。
- `saveload/saveload.h`：删除 `VarMemType::StrQ` / `VarTypes::STRQ`，新增 `VarFileType::Bool`/`Label` 别名、`SaveLoad::DetermineMemType()`、模板 `SlCopy()`；`settings_sl.cpp` 把 `SLE_VAR_STRQ` 映射到 `VarMemType::Str`。
- `newgrf.h`：`FlattenNewGRFLabel` / `UnflattenNewGRFLabel` 改为委托 `BaseLabel::FlattenToUint32()` / `UnflattenFromUint32()`。
- 已验证合并树里 `VarMemType::StrQ`、`VarTypes::STRQ`、`CORNER_*`（旧 Corner 枚举）、`IndustryTileSpec::enabled` **无残留** ✓；`SLE_STRQ` 仅存在于 ini 与兼容层，上游仍保留，不是问题 ✓。
- 剩余风险集中在 PXP 自定义 saveload / Label 代码是否适配新 API，只能靠编译确认。

### P1 逻辑回归：两边各有一半正确

| 位置 | 上游 0.73.5 | PXP | 风险 |
| --- | --- | --- | --- |
| `train_cmd.cpp` | `ConsistChanged({}) + UpdateAcceleration()` | `First()->CargoChanged() + UpdateAcceleration() + BroadcastConsistCaches()` | 取上游丢 PXP 的 desync 修复；取 PXP 丢上游的统一 cache 刷新。必须把广播并入新流程 |
| `order_gui.cpp` | `drag_selected` 修复：`vehicle->owner != _local_company`、`GetNumOrders()`、`GetOrder()` | 重构为 `IsLocalTarget()` / `NumOrders()` / `OrderAt()` + 独立订单列表 `list_id` | theirs 的 `this->vehicle->GetNumOrders()` 在独立订单列表路径下不适用，需把修复移植到 PXP 抽象层 |
| `station.cpp` | #16097 + `GetSpec()->catchment` | `AirportCatchmentRadius()` | 见 P0-1 |
| `script/api/script_order.cpp` | 全量换成 `airport.IsEmpty()` | PXP `Airport : TileArea`，上游 0.73.5 给 `TileArea` 加了 `IsEmpty()` | 兼容 ✓，但 PXP 自定义文件里残留的 `tile != INVALID_TILE` 写法需统一 |
| `.ottdrev-vc` | `jgrpp-0.73.5` | `pxp-2611.1-p3` | 保 ours 或重新生成，别用上游版本串（影响版本识别 / 网络匹配） |
| `README.md` | 标题改 0.73.5 | `# OpenTTD Patch Pack` | 保 ours |
| `simplified_chinese.txt` | eints 改成「强制卸载 / 转运」 | PXP 统一改「普通货物」以区分特殊货物 | 保 ours。上游新增行只引入 `{BLACK}` / `{STRING}`，符合项目占位符约定 ✓；其它语言上游更新也只引入 `{SPACE}` / `{STRING}` / `{BLACK}`，无 `{STRING1}` / `{RAW_STRING}` ✓ |
| `follow_track.hpp` | `static inline` | `inline static` | 纯格式，取 theirs |
| CI ×7 | 上游修改 | PXP 有意删除 | 保持删除；`release-*.yml` 自动合并会带入上游 CI 变更，需人工核对 |

### 其它已确认项

- `src/afterload.cpp` 是 PXP 侧遗留的**孤儿文件**（`src/sl/CMakeLists.txt` 只引用 `src/sl/afterload.cpp`），不参与构建。里面还有旧机场修复，合并后不会随上游更新，容易误判；建议合并后删除或明确标注。
- `src/saveload/` 与 `src/sl/` 双目录都被 `add_subdirectory`，上游 0.73.5 也是如此，属正常结构，不是冲突。
- 本地 `jgrpp` 分支已含 0.73.5 并多 1 个 revert 提交；若目标其实是「最新 jgrpp」而非 tag，需把 `d1b840b86b` 一并纳入。

---

## 5. 建议执行顺序

1. **先确认 `pxp-jgrpp` 指哪个 commit**：本地 `8b6297897d` 还是远程 `e3e836fce1`。这决定第二段合并是 no-op 还是重复解冲突。
2. 合并前给两条分支打备份 tag。
3. 一次性合并，按 P0 顺序处理：XSLFI 编号 → 机场子系统（主体工作量）→ 逻辑回归项 → 琐碎项。
4. 验证：`cmake --build build -j4`；strgen 语言校验；用现有 `.sav` 做旧存档加载回归；对接/解挂 + 网络 desync 回归（重点看 `ConsistChanged` 与 `BroadcastConsistCaches`）。

## 附：预演方法

```
git merge-tree --write-tree --name-only px-patch jgrpp-0.73.5
git merge-tree --write-tree --name-only origin-gh/pxp-jgrpp jgrpp-0.73.5
git merge-tree --write-tree px-patch jgrpp-0.73.5   # 取 stage 1/2/3 blob 做三方合并看冲突块
```

第二步流程用 `git commit-tree` 造临时 commit 对象模拟（只写对象，不改任何 ref）。

---

## 6. 执行结果（2026-11-05）

已按用户要求**一次性合并进 `px-patch`**，没有走两段式流程。

| 项 | 值 |
| --- | --- |
| 合并提交 | `333fdb35ef` — `Merge tag 'jgrpp-0.73.5' into px-patch` |
| 父提交 | `8b6297897d` (px-patch) + `5389ea187f` (jgrpp-0.73.5) |
| 变更规模 | 139 文件，+1436 / -904 |
| 合并前备份 | 分支 `backup/px-patch-pre-0735` → `8b6297897d` |
| `pxp-jgrpp` | **未改动**，仍指向 `8b6297897d`（需要的话可 fast-forward 到 `333fdb35ef`） |

### 逐文件冲突决策

| 文件 | 决策 |
| --- | --- |
| 7 个 CI workflow | 保持 PXP 删除 |
| `.ottdrev-vc`、`README.md` | 保 PXP 版本串 |
| `simplified_chinese.txt` | 保 PXP「普通货物」措辞；删掉上游 eints 重复插入的 `STR_ORDER_CONDITIONAL_DRIVING_BACKWARDS` |
| `newgrf_act0_airports.cpp` | 整文件保 PXP 版本（上游 `ValidateAirportLayout` / `tile.gfx` 单值重构与 PXP 结构不兼容） |
| `aircraft_cmd.cpp` | 5 块全保 PXP（函数被重排）；手工补上上游 `Aircraft::MarkDirty()` 的 `UpdateAircraftCache(this)` |
| `station_cmd.cpp` | 2 块保 PXP 删除（`CmdBuildAirport` / `UpdateAirportsNoise` 在 PXP 的 `airport_cmd.cpp`） |
| `sl/afterload.cpp` | 保 PXP 的 `layouts[0].size_x/size_y` |
| `station.cpp` | 保 PXP 的 `AirportCatchmentRadius()`（基于 `AirTypeInfo`） |
| `sl/extended_ver_sl.h` | 保 PXP 两个 feature 原位；**上游 `XSLFI_SCRIPT_RANDOMIZER` 追加到 PXP 自定义区段末尾**，`XSLFI_SIZE` 之前，避免已有存档 feature 位图错位 |
| `train_cmd.cpp` | `First()->ConsistChanged({})`（上游修复），PXP 的 `BroadcastConsistCaches()` / `UpdateAcceleration()` 已由 `ConsistChanged()` 内部调用 |
| `order_gui.cpp` | 保 PXP 的 `NumOrders()` / `OrderAt()` / `IsLocalTarget()`，加入上游 `drag_selected` 成员、`was_selected` 局部与拖拽判定条件 |
| `pathfinder/follow_track.hpp` | 上游 `static inline` 格式 + 保留 PXP 的 `IsAirTT()` |

### 验证结果

- `git diff --cached --check` 干净；无冲突标记残留；无未合并路径。
- strgen 全语言通过（修掉一处重复字符串后）。上游新增译文的占位符只有 `{STRING}` / `{BLACK}` / `{SPACE}`，符合项目占位符约定。
- **8 个冲突源文件全部编译通过**（`aircraft_cmd.cpp`、`train_cmd.cpp`、`order_gui.cpp`、`station.cpp`、`station_cmd.cpp`、`sl/afterload.cpp`、`sl/extended_ver_sl.cpp`、`newgrf_act0_airports.cpp`），仅既有警告。
- **16 个上游自动合并的高风险文件编译通过**（`script/api/script_order.cpp`、`saveload/*`、`sl/*`、`newgrf_*`、`viewport.cpp`、`window.cpp` 等）。
- 完整构建跑到 199/550 目标时**零真实 C++ 错误**；所有失败都是工具沙箱随机拒绝编译器读取头文件（`Operation not permitted`），与代码无关。

### 遗留风险 / 后续必做

1. **完整链接未验证**。请在本地终端直接跑 `cmake --build build -j4`（本环境的构建沙箱会随机拦截编译器读系统头文件）。链接阶段重点看 `_randomizer_chunk_handlers` 等新增符号。
2. **旧存档加载回归**：`pxp-2611.1-p3` 存档的 SLXI feature 位图。`XSLFI_SCRIPT_RANDOMIZER` 用 `XSCF_IGNORABLE_ALL`，风险较低，但必须实测。
3. **机场功能**：上游 0.73.5 的机场布局校验、无效 tile ID 拒绝、0xFF 空洞移除等修复**未移植到 PXP 自定义机场**。PXP 功能不丢，但这些安全加固缺失；若要补齐需要把多格机场 patch 移植到新 API，属于独立工作量。
4. **联机 desync 回归**：`Train::MarkDirty()` 改用 `ConsistChanged({})` 后，重点验证对接/解挂场景。
5. **上游其它新特性**（LibSoxr 依赖、screensaver、脚本 randomiser chunk 等）只做了编译级验证，未做运行验证。
6. `.ottdrev-vc` 仍是合并前的 `pxp-2611.1-p3` 版本串，发版前需手工 bump。

