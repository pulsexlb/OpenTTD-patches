# Merge Notes: The Primary Architecture

This fork decouples a train's **identity** from its **physical chain head**. Upstream OpenTTD assumes that the first vehicle of a chain (`First()`) is always the front engine and the sole carrier of consist information. That assumption no longer holds here. When merging upstream changes, this document describes what to look for and how to adapt new code.

## Core concepts

- `First()` — physical chain head. Purely spatial; may be any vehicle (including a wagon).
- `Primary()` — the *consist information carrier*: orders, name, unit number, age, group, profit, `decouple_part`, etc. May sit at **any position** in the chain.
  - Implemented as an explicit `primary` pointer on every `Vehicle` (see `Vehicle::Primary()` in `vehicle_base.h`), falling back to `First()` when unset.
- A train is "the" train only through its Primary. Two different vehicles of one chain must never resolve to different Primaries.

## Rules for merged code

### 1. Identity checks: use Primary resolution, never raw `IsPrimaryVehicle()`

Upstream code identifies "a train" with `v->IsPrimaryVehicle()` (i.e., `v == v->First()` plus engine flags). In this fork that only tells you whether a vehicle happens to be the chain head. Anywhere upstream code means *"is this vehicle the consist's identity carrier"* or *"is this the train shown in lists/news/orders"*, rewrite it as:

```cpp
v->First()->Primary()->IsPrimaryVehicle()   // for arbitrary chain members
// or simply
v->Primary()->IsPrimaryVehicle()
```

Typical upstream patterns to watch for when merging:

- Vehicle list building (`GenerateVehicleSortList` and callers): entries pushed into lists must be the **Primary**, not the chain head, otherwise unit numbers display as 0 and clicking opens a wrong window.
- GUIs that count/open windows per train (depot tooltips, group GUI, autoreplace, trace restrict): resolve through Primary before counting or opening windows keyed by `v->index`.
- News/advice items, script/AI API filters (`script_group.cpp`, `script_vehiclelist.cpp` style checks).
- `Vehicle::IterateTypeFrontOnly()` yields **chain heads**; anything displayed or identified from it needs Primary resolution.

### 2. Movement/tick architecture

- Ticks are triggered per **chain head**: `_tick_train_front_cache` (in `vehicle.cpp`) holds chain heads only; `Train::Tick` then drives via `this->Primary()`.
- Upstream changes to `Train::Tick`, `TrainLocoHandler`, `CallVehicleTicks`, or tick-cache maintenance must preserve this two-level scheme: cache holds heads, driving runs on the Primary.

### 3. ConsistChanged / CargoChanged anchoring

- `ConsistChanged()` must be called on the chain head (`First()`); `CargoChanged` asserts `First() == this`.
- `UpdateTrainGroupID` and similar per-consist updates operate on the Primary.
- When merging upstream edits inside these functions, keep the anchor distinction intact.

### 4. Chain surgery must re-materialise primaries

Any code that links/unlinks vehicles into chains must keep the invariant *"all members of a chain resolve to the same Primary"*. After any surgery call:

```cpp
MaterialiseTrainPrimary(head);        // sets primary pointers onto the first engine
NormaliseTrainHead(head, flags);      // enforces consistency + closes orphan windows
```

Conventions already established:

- Engine builds materialise primaries at build time; free wagon builds intentionally do not (wagons stay `nullptr` until connected).
- Vanilla-style surgery paths (`ArrangeTrains`, `NormaliseDualHeads`, depot moves) do **not** maintain primary pointers themselves — callers must materialise afterwards.
- If you merge upstream code that adds a new place where chains are split/merged/coupled, add primary materialisation + head normalisation there too.

### 5. Dual-headed engines

- Dual-head units may **wrap wagons between their halves** (`front half … wagons … rear half`). The rear half does not necessarily carry the engine subtype flag.
- `NormaliseDualHeads` stops its walk at `other_multiheaded_part` (not just at engines) so wrapped blocks are never broken up. Keep this when merging upstream changes to that function.
- `ReverseTrainNoSwapVehicles` falls back to full physical swap (`ReverseTrainSwapVehicles`) whenever a dual-head is present; pure NoSwap block-reversal is used otherwise.
- Never assume a multihead's rear half satisfies `IsEngine()`.

### 6. Coupling / decoupling validation

- Couple target selection pre-validates the merged consist via `IsCoupleArrangementValid()` (declared in `train.h`): backup → arrange → `CheckTrainAttachment` + `CanConsistChange` → restore. Invalid targets are skipped by the couple pathfinder (`CYapfDestinationTrainRailT::PfDetectDestination`).
- Decoupling validates the split result the same way inside `TryTrainDecouple` and silently skips on failure.
- If upstream adds alternative couple/split paths, wire them through the same validation helpers rather than duplicating checks.
- The waiting-train identity for orders is resolved via `t->First()->Primary()` (e.g., `FindSafeCouplePositionProc`, `PfDetectDestination`) because the wait-for-couple order carrier may sit mid-chain.

### 7. Savegame compatibility

- Feature flag `XSLFI_TRAIN_PRIMARY` (in `sl/extended_ver_sl.h`) marks saves that persist which vehicle is the primary.
- Per-vehicle transient field `consist_primary` (`vehicle_base.h`, saved in `sl/vehicle_sl.cpp`) restores primary pointers after load. Merging upstream saveload changes must keep this field saved *per vehicle* and the post-load pointer rebuild intact.

## Quick merge checklist

When reviewing an upstream diff touching rail vehicles, grep the changed hunks for:

- [ ] `IsPrimaryVehicle()` used as "is this the train" → replace with Primary resolution.
- [ ] `->index` used as window/list/news key on a non-head vehicle → resolve to Primary first.
- [ ] Iteration starting at `First()` that reads orders/name/unitnumber/group/profit → read those from Primary().
- [ ] New chain surgery without `MaterialiseTrainPrimary` / `NormaliseTrainHead` afterwards.
- [ ] `ConsistChanged` called on something that may not be the chain head.
- [ ] Multihead assumptions (`IsEngine()` on rear halves, halves adjacency).
- [ ] Saveload chunks adding per-vehicle fields near the vehicle pool — confirm `consist_primary` handling still round-trips.

# Merge Notes: OrderList / Schedule (调度计划) Changes since 3a4525ea8c

Since 3a4525ea8c this fork heavily reworked the order system: player-created order lists (调度计划, standalone `OrderList`s), an *execute-schedule detour* mechanism, and schedule options on couple/decouple orders. Upstream assumes `v->orders` is a vehicle-owned list that is freed with its last vehicle, and that a consist's order state is just `orders` + the three order indices. None of that holds anymore. The sections below list what breaks when merging naively.

## 1. `Vehicle::primary_order` / `primary_order_index` (execute-schedule detour)

- `vehicle_base.h` adds, next to `orders`:
  - `OrderListID primary_order` — the consist's *home* order list. **Invariant: `primary_order == orders->index` whenever the vehicle is not executing another list**; `primary_order_index` is `INVALID_VEH_ORDER_ID` then.
  - `VehicleOrderID primary_order_index` — while away on an execute-schedule detour: the position in the home list to resume at.
- `Vehicle::IsExecutingSchedule()` is the test for "is currently away executing another list".
- The invariant is maintained **manually at every `v->orders` assignment site** (share/copy/clone, `InsertOrder` create branch, `CmdBulkOrder`, `CmdInsertOrdersFromVehicle`, `order_backup.cpp`, `schdispatch_cmd.cpp`, train couple/sell transfers). If a merge adds a new place that assigns `v->orders`, `primary_order` must be synced there too — a stale value makes vehicles jump back to a list they no longer belong to.
- The detour *ends* on order-index wrap-around: `SkipToNextRealOrderIndex()` (now returns `bool`, detecting the wrap) and `IncrementImplicitOrderIndex()` both call `Vehicle::ReturnFromExecuteSchedule()` when `IsExecutingSchedule()`. Upstream changes to these index functions must preserve the wrap detection.
- The resume position is kept valid when the *home* list is edited by others: `AdjustExecutingResumeIndices()` (order_cmd.cpp) is called from `InsertOrder`, `DeleteOrder`, `CmdMoveOrder`, `CmdReverseOrderList` **and** from the standalone-list editor paths (`StandaloneListInsertUpdateVehicles` / `StandaloneListDeleteUpdateVehicles`). New order-list mutation paths need the same call; note the standalone editor paths additionally patch shared-chain vehicles' indices themselves (`InsertOrderOnStandaloneList` etc. bypass the vehicle commands).
- Saveload: feature flag `XSLFI_VEHICLE_PRIMARY_ORDER` ("vehicle_primary_order" in `sl/extended_ver_sl.*`) gates the two named fields `primary_order` / `primary_order_index` in `sl/vehicle_sl.cpp`. For older saves `AfterLoadVehiclesPhase1` backfills `primary_order = orders->index`. Keep both intact when merging saveload changes.

## 2. Player-created OrderLists (调度计划)

- `OrderList` carries fork fields `name`, `company`, `is_public`, `dispatch_enabled`, `separation_enabled` (saved in `sl/order_sl.cpp` `GetOrderListDescription`). `IsPlayerCreated()` means `company != INVALID_OWNER`; visibility for selection is `IsVisibleToCompany()` (own company or public).
- `OrderList::FreeChain()` **never frees a player-created list** — it only clears contents. Such lists live on with zero vehicles, are part of the savegame (`OrderList::Iterate()` saves the whole pool, including detached/empty lists), and are managed by dedicated commands (`CreateOrderList`, `RenameOrderList`, `DeleteOrderList`, `SetOrderListPublic`).
- `num_manual_orders`, `timetable_duration`, `total_duration` are NOSAVE. They are recomputed by `Initialize(v)` — which only runs for lists that have vehicles. For vehicle-less player-created lists `InitializePlayerCreated()` (called from `AfterLoadVehiclesPhase1`) recomputes them. If a merge drops that pass, every standalone list loads with manual count 0 and durations 0 (this caused real crashes: `SkipToNextRealOrderIndex` takes the "no manual orders" branch and editor deletes underflow the counter).
- `DeleteVehicleOrders` detaches from player-created lists without freeing them. `CmdDeleteOrderList` refuses while `GetNumVehicles() != 0` (`STR_ERROR_ORDER_LIST_IN_USE`, GUI shows the error) and, when vehicles are away on execute detours with this list as home, makes them adopt the list they are currently executing.
- `InvalidateVehicleOrder` additionally invalidates the standalone OrderList windows when the vehicle's list is player-created.

## 3. `OT_EXECUTE_SCHEDULE` orders and the detour lifecycle

- New order type `OT_EXECUTE_SCHEDULE`: `dest` holds the target `OrderListID` (`DestinationID::ToOrderListID()`); modified via `MOF_EXECUTE_SCHEDULE`; target must be a player-created list visible to the owner.
- Jump-in happens in `UpdateOrderDest`'s `OT_EXECUTE_SCHEDULE` case: step past the order, remember home + resume index, detach from the home chain (**the home list is kept alive even when vehicle-owned** — upstream's `FreeChain(false)` here was removed deliberately), join the target's chain via `OrderList::AssignVehicle`, mirror the target's dispatch/separation flags. Nested execute orders are skipped while already executing.
- Wrap-around in the target list triggers `ReturnFromExecuteSchedule()`: detach from target, re-join home, restore dispatch/separation flags (vehicle-owned homes get their state mirrored into `dispatch_enabled`/`separation_enabled` at jump-in), resume at the saved index.
- `Commands::ExitExecuteSchedule` (`CmdExitExecuteSchedule`) exits a detour early; the vehicle order GUI shows an "End of executed schedule" row (`STR_ORDERS_END_OF_EXECUTE_SCHEDULE`) with an exit button while executing.
- `OrderList::AssignVehicle()` / `RemoveVehicle()` keep `num_vehicles` / `first_shared` consistent — route any new chain (de)registration through them.
- `DeleteVehicleOrders` first drops execute-detour state and afterwards frees an orphaned vehicle-owned home list (`FreeOrphanedExecuteScheduleHome`).

## 4. Couple / decouple schedule options

- `OrderDecoupleOrdersFlags` gained `ODOF_EXECUTE_SCHEDULE = 5` (`ODOF_END = 6`). The schedule ids for both train parts are stored on the `OT_DECOUPLE` order in `xdata` / `xdata2` low 16 bits (`Get/SetDecoupleFirstScheduleID`, `Get/SetDecoupleSecondScheduleID`).
- The decouple dropdown stores **mapped** values: `_order_decouple_orders_drowdown_flags` maps dropdown index → ODOF (value 2 is retired and skipped). Never store a raw dropdown index — that was the off-by-one bug fixed here.
- `AdoptDecoupleSchedule` (train_cmd.cpp) does *not* attach the part to the chosen player-created list directly: it creates a **vehicle-owned wrapper list containing a single `OT_EXECUTE_SCHEDULE` order** and sets `orders` + `primary_order` to it, so the part runs the target through the normal detour mechanism and auto-cleans on sell/order replacement.
- `OT_GOTO_COUPLE` gained `GetCoupleUseWaitingSchedule()` (flags bit 3, `MOF_COUPLE_USE_WAITING_SCHEDULE`). When set, `Couple()` calls `AdoptCoupleWaitingSchedule(v, u)` **before** `DeleteVehicleOrders(u)`: the surviving consist takes over the waiting consist's order list (incl. chain membership), `primary_order`/`primary_order_index`, all three order indices, `current_order_time`/`lateness_counter`/`timetable_start`, `dispatch_records`, and the dispatch/separation/automation flags. Any new per-consist schedule state must be added to that transfer (and to `ReturnFromExecuteSchedule` / the jump-in code) to stay consistent.

## 5. `CmdModifyOrder` has TWO validation switches

`CmdModifyOrder` validates `ModifyOrderFlags` twice: once per order type (whitelist switch), then once per MOF for argument checks — and that second switch ends in `default: NOT_REACHED()`. **Every new `MOF_*` must be added to both switches** (this already caused a crash-on-click once). Execution happens in the third switch.

## 6. `OrderTargetType` refactor

Order commands take `OrderTargetType` (Vehicle or OrderList) + id instead of a bare `VehicleID`, so the same command works on a vehicle's orders and on a standalone list. Upstream order commands merged in need the dual-target treatment, and their standalone branch must maintain vehicle order indices as described in section 1.

## 7. GUI / strings

- The vehicle order window's "end of orders" row shows the executed schedule's name while executing, and the bottom-middle button stack has a third plane (`DP_BOTTOM_MIDDLE_EXIT_EXECUTE`).
- `orderlist_gui.cpp`: delete posts with an error string; "make private" is disabled (placeholder) while the list is public.
- Most feature strings live in `lang/extra/english.txt` / `lang/extra/simplified_chinese.txt`; a few (decouple/couple options, delete-error) were added to `lang/english.txt` / `lang/simplified_chinese.txt`. `STR_ORDER_DECOUPLE_DETAILS` / `_AUTO` take `{RAW_STRING}` params (not `{STRING}`) because part texts can embed schedule names.

## Quick merge checklist for order code

- [ ] New `v->orders` assignment → `primary_order` synced (and execute state cleared or preserved deliberately).
- [ ] Order list insert/delete/move/reverse (vehicle *and* standalone editor paths) → shared-chain indices **and** `AdjustExecutingResumeIndices`.
- [ ] `FreeChain(false)` reachable on a possibly player-created list.
- [ ] New `MOF_*` added to **both** `CmdModifyOrder` validation switches.
- [ ] Anything recomputing `num_manual_orders` / durations — keep `InitializePlayerCreated()` for vehicle-less lists.
- [ ] Saveload hunks near vehicle/order lists — keep `XSLFI_VEHICLE_PRIMARY_ORDER` fields + backfill and the `InitializePlayerCreated()` pass intact.
- [ ] New per-consist schedule state → add it to `ReturnFromExecuteSchedule`, the `UpdateOrderDest` jump-in, and `AdoptCoupleWaitingSchedule`.
