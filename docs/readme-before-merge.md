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
