# Road vehicle transport (RoRo) — a JGR's Patch Pack feature branch

This branch adds **road vehicles carried by other vehicles** to [JGR's Patch Pack](https://github.com/JGRennison/OpenTTD-patches):
a road vehicle (truck/bus) can drive to a station, **wait to be transported**, be loaded onto a
train/ship/aircraft that stops there, travel with it (off the road network, keeping its cargo,
refit, orders and identity), and be unloaded at another station, where it simply continues its
own schedule.

* **Base**: JGRPP **0.73.1**, commit `611aabd7ba` (this is *not* upstream OpenTTD trunk)
* **Status**: playable and verified in game — waiting, loading, transport, unloading, release,
  articulated road vehicles, and train/ship/aircraft carriers have all been walked through; the
  remaining work is listed under *Known limitations*
* **Diff vs. base**: `src/` — 43 files, +2648/−47 · `docs/ro-ro/` — 4 files (+1366) ·
  `testrun/` — 16 scripts (+1084). See `git diff --stat 611aabd7ba..HEAD`
* **Commits**: 34 commits on `feature/road-veh-transport` since `611aabd7ba`
* **Milestones**: the implementation specification plans M1–M8; the work that was added while
  reviewing (M9 articulated vehicles, M10 selection criteria, M10b–d the settings window and the
  slot criterion, M11/M11b fixes, M11c weight/appearance/carried-vehicle list) is described in its
  chapter 11 and appendix D

## How it works from the player's point of view

On a **station order** of a vehicle, two dropdowns gain road vehicle transport entries. They are
**check boxes** (more than one can be active at the same time) and the list is rebuilt after every
click, so the ticks always show the current state:

*load behaviour dropdown* (the one with "No loading"/"Full load"):

| Entry | Meaning on a train/ship/aircraft | Meaning on a road vehicle |
|---|---|---|
| `Load road vehicles` | load waiting road vehicles here | **wait here to be transported** |
| `Only road vehicles for the next stop` | only take vehicles whose declared destination is the carrier's next stop (on by default when loading is enabled) | – (not offered: a road vehicle's own order has no use for it) |
| `Wait for road vehicles` | keep waiting at this station until road vehicles have been loaded | – |

*unload behaviour dropdown* (the one with "Unload"/"Transfer"):

| Entry | Meaning on a train/ship/aircraft | Meaning on a road vehicle |
|---|---|---|
| `Unload road vehicles` | unload carried road vehicles here | **be unloaded here** (this is the "declared destination") |

The combinations are kept meaningful: switching loading off clears the match and wait entries,
while switching the match or wait on implies loading. A road vehicle's own order clears (and the
display ignores) the match bit, so an order saved by an older build of this branch cannot show up as
"only road vehicles for the next stop" any more.

A typical setup:

* truck: `Go to A` + *wait to be transported* → `Go to B` + *be unloaded here* → `Go to C`
* train: `Go to A` + *load road vehicles* → `Go to B` + *unload road vehicles*

### Selection criteria

A "load road vehicles" order can also **select which** road vehicles it takes: a candidate which
does not satisfy every criterion that is in use is simply skipped and keeps waiting for another
carrier (the same "no match, skip it" semantics as the destination match). This follows the
parameterised style of the px-patch train coupling feature (whose `CoupleOrderLoadOk()` /
`CoupleCargoOk()` / `CoupleNumOk()` pick the partner train by order parameters):

| Criterion | Values | Meaning |
|---|---|---|
| load state | any / empty / full | only take an empty (or a fully loaded) road vehicle |
| cargo | any / can carry X / is carrying X | only take a vehicle which can carry, or currently carries, cargo X |
| minimum waiting time | 0 = no limit / N days | only take a vehicle which has been waiting for at least N days |
| trace restrict slot | any / a road vehicle slot | only take a vehicle which is an occupant of that slot (the "路签" parameter of the px-patch coupling feature) |
| most road vehicles at once | no limit / 1 / 2 / 3 / 5 / 10 | stop loading when the carrier already holds that many road vehicles (0 = no limit) |

The last row is a limit rather than a filter: it is checked in `RVTransportAttachAuto()` (through which
every load goes, so a debug command cannot bypass it either) against the number of road vehicles the
carrier currently holds, which also covers road vehicles it picked up at an earlier stop of a
multi-leg route.

A specific declared destination was tried and dropped again: the destination a road vehicle
"declares" is the *first* station of its schedule that carries *be unloaded here*, so with several
unloading orders only the first one would ever be read. The *Only road vehicles for the next stop*
toggle (which compares it with the carrier's next stop) is kept.

The criteria are stored per station order (extra fields in `OrderExtraInfo`, behind extended
savegame feature version 3, so older saves simply have no criteria). They are edited from the load
dropdown of the order window, which has a single *Road vehicle transport...* entry opening a window
with the four toggles (load, only for the next stop, wait for road vehicles, unload) and one
dropdown per criterion. All active criteria are listed in the order row.

The order row lists **every** part of the setting that is active (e.g. "Go to A, load road vehicles,
only road vehicles for the next stop"), and a waiting/carried road vehicle shows the status
*"Waiting to be transported" / "Being transported"*; a carrier shows how many road vehicles it
currently holds.

Carried road vehicles are visible from the carrier's side too:

* the carrying vehicle part is drawn with its **"loaded" appearance** (the same graphics a full
  cargo would use: the default "half the capacity is loaded" rule for trains, and the
  `stored * totalsets / capacity` rule for NewGRF sets). Default ships and aircraft have no
  loaded/unloaded variant, so nothing changes visually for them;
* the carrier's **detail window lists** what it carries — the top of the window shows a *Carrying N
  road vehicles* line as soon as it holds any, trains have a **"Carried" tab** which lists one vehicle
  per line, and **clicking a line opens the window of that vehicle** (ships and aircraft have no tab bar,
  so their list sits at the bottom of their details panel and is clickable as well);
* a train's consist **weighs more** while it carries road vehicles (they are added to the consist
  weight, so acceleration, running cost, bridge limits, and the "performance" tab all see them).
  `rvtransport state <train>` prints `carried=`/`total_incl_carried=`/`own=`. Ships and aircraft
  have no cached consist weight in this engine, so for them the weight only matters for the
  capacity check when loading;
* which part a vehicle ends up on is decided by **weight**: a part can take a road vehicle when
  `cargo_cap × (cargo unit weight) / 16` tonnes is at least the vehicle's empty weight
  (`rvtransport parts <carrier>` prints this as `rv_capacity`/`rv_used`). Passenger carriages are
  usually far too light for a truck, so a truck normally rides on a freight wagon. The "loaded"
  appearance only changes on the part that actually holds a vehicle, and only if that vehicle set
  has separate empty/loaded graphics.

Behavioural notes:

* **Matching is a condition**: *"Only road vehicles for the next stop"* is evaluated like a
  conditional order — a road vehicle whose declared destination is not the carrier's next stop is
  simply **skipped** (it is not an error and the carrier does not wait for it).
* **`Wait for road vehicles` waits indefinitely** (the `finished_loading` flag is held down, exactly
  like "Full load"); there is no engine-side timeout, so pair it with a timetable or only enable it
  where vehicles are actually available.
* **A carried vehicle is moved on to its next order when it is loaded**, and a carrier only drops it at
  the station that order names. A vehicle can therefore be carried several times on its way (train from
  A to B, drive to C, ship from C to D): mark "wait to be transported" at each boarding station and
  "be unloaded here" at each drop-off station. If a vehicle's own schedule does not name the station the
  carrier unloads at, tick **Unload all road vehicles** on the carrier's unload order to drop everything
  anyway (see the order window, unload dropdown, "Road vehicle transport...").
* **Carriers are trains, ships and aircraft**; a road vehicle cannot carry another road vehicle.
* **A carried road vehicle stays visible and reachable**: it remains in the vehicle/group lists (its
  status reads "Being transported"), and "centre on vehicle" / the follow camera look at the
  **carrier** instead of at the station it was loaded at.
* **When a carrier is destroyed, the road vehicles it carries are destroyed with it**, like the
  wagons of a crashed train — they are not left behind on the map. A carrier holding road vehicles —
  and a road vehicle being carried — **cannot be sold** until the vehicles are unloaded (use
  `Unload road vehicles` at a station first).

## Building

Same toolchain as OpenTTD/JGRPP, e.g. with MSYS2 (MINGW64) on Windows:

```bash
pacman -S --needed base-devel mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
    mingw-w64-x86_64-zlib mingw-w64-x86_64-libpng mingw-w64-x86_64-lzo2
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOPTION_USE_ASSERTS=ON
cmake --build build --target openttd
```

Run it from the build directory (so that `lang/` and `baseset/` are found), for example
`build/openttd.exe -c <your-config>.cfg`. The binary must not be running while relinking on Windows.

> On Windows/MSYS2 make sure `D:\msys64\mingw64\bin` is on `PATH` **when you invoke the build**:
> `cc1plus.exe` finds its runtime DLLs there, and without it the compiler dies silently with no
> error message (exit code `0xC0000135`).

## Savegame compatibility

The feature uses JGRPP's extended savegame feature mechanism (`XSLFI_ROAD_VEH_TRANSPORT`,
version 3) and does **not** bump `SAVEGAME_VERSION`:

* **0.73.1 savegames load fine** (new fields default to "unused"), verified against a savegame
  written by a pristine 0.73.1 build;
* savegames produced by this branch are **not** loadable by vanilla 0.73.1 (the feature is
  written as a non-ignorable extended feature, so the older build refuses them cleanly).
* Road vehicles carried *while saving* survive a save/load round-trip (carrier, host part and
  recorded weight are restored), verified by `testrun/verify_intransit.ps1`.

## Reviewing this branch — where to look

| Area | Files | What to check |
|---|---|---|
| Core transactions | `src/roadveh_transport.{h,cpp}` | attach/detach: `Stopped`+`Hidden`, road-network hash removal/restore, capacity-vs-weight check, finding a free road-stop tile, emergency release |
| Vehicle state | `src/vehicle_base.h`, `src/sl/vehicle_sl.cpp`, `src/sl/extended_ver_sl.{h,cpp}` | 5 new fields, gated behind `XSLFI_ROAD_VEH_TRANSPORT`; IDs stored with `SLE_UINT32` |
| Order data | `src/order_base.h`, `src/order_cmd.cpp`, `src/order_type.h`, `src/sl/order_sl.cpp` | `MOF_RV_TRANSPORT` field, command whitelist, `Order::AssignOrder()` must copy the flags |
| Loading loop | `src/economy.cpp` (`LoadUnloadVehicle`) | waiting road vehicles skip normal loading; 8 vehicles/tick matched/unloaded per order flags; `ORVTF_WAIT` holds `finished_loading` |
| Exemptions | `src/vehicle.cpp`, `vehiclelist.cpp`, `group_cmd.cpp`, `economy.cpp`, `infrastructure.cpp`, `network/network_server.cpp`, `disaster_vehicle.cpp`, `engine.cpp`, `industry_cmd.cpp`, `settings_table.cpp`, `order_cmd.cpp` | carried road vehicles behave like virtual vehicles everywhere these loops run |
| GUI | `src/order_gui.cpp`, `src/vehicle_gui.cpp`, `src/lang/extra/*.txt` | dropdown entries, road-vehicle vs carrier wording, order-row markers, status strings |
| Destruction | `src/vehicle.cpp` (`PreDestructor` → `RVTransportDestroyCarriedVehicles`) | a destroyed carrier takes the road vehicles it holds with it |
| Position/lists | `src/roadveh_transport.cpp` (`RVTransportGetFollowVehicle`), `viewport.cpp`, `window.cpp`, `vehicle_gui.cpp`, `vehiclelist.cpp` | carried road vehicles stay listed and are followed/located at their carrier |
| Setting | `src/table/settings/game_settings.ini`, `src/settings_type.h`, `src/settingentry_gui.cpp`, `src/roadveh_transport.h` | `vehicle.rv_transport_enabled` (bool, default on, expert category, own settings page) = master switch: with it off nothing is loaded any more, vehicles which are waiting stop waiting and carry on with their own schedule, vehicles which are already on board can still be put down (so nothing is stranded), and the sprite/cargo-amount lookups are skipped as well, so the feature costs nothing at all while it is off; `vehicle.rv_transport_carrier_parts` (enum, same page): `0` = any part with cargo capacity may carry, `1` = only a part whose cargo is in the *oversized* class may carry, `2` (**default**) = only a part whose cargo is bulk, *oversized*, or the NewGRF "Vehicles" cargo (label `VEHC`) may carry; `vehicle.rv_transport_unload_warn_days` (uint16, default 30, same page) = warn when a carried road vehicle was not unloaded for that many days (0 = no warning) |

## Developer/debug console commands

`rvtransport` (in the game console) is a development aid and can be dropped before merging:

```
rvtransport list                                    # ids/state of road vehicles and carriers
rvtransport orders <vehicle>                        # every order with its RoRo flags
rvtransport state <vehicle>                         # RoRo state of one vehicle
rvtransport wait <vehicle> on|off                   # force the "waiting" state
rvtransport orderflag <vehicle> load|unload|dest|wait    # set flags on the current order
rvtransport modify <vehicle> <order> load|unload|dest|wait   # set the flags through the real order command
rvtransport toggle <vehicle> <order> load|unload|dest|wait   # apply the order window's check box rules
rvtransport setflags <vehicle> <order> <flags>      # force an order into a known flag state
rvtransport criteria <vehicle> <order> loadstate any|empty|full
rvtransport criteria <vehicle> <order> cargo any|<cargo_id> [carrying]
rvtransport criteria <vehicle> <order> minwait <days>
rvtransport criteria <vehicle> <order> slot any|<slot_id>
rvtransport criteria <vehicle> <order> max <count>            # 0 = no limit
rvtransport carried <vehicle>                       # the road vehicles a carrier holds
rvtransport parts <vehicle>                         # per carrier part: cargo/cap/stored/rv_capacity/rv_used/holds_rv
rvtransport vscroll <vehicle>                       # line count of every tab of the train details window
rvtransport setcurrent <vehicle> <order> [loading]  # make an order the current one (test aid)
rvtransport mkslot <name> [max_occupancy]   # create a road vehicle slot (debug)
rvtransport slot <vehicle> <slot_id> on|off # add/remove a vehicle from a slot (debug)
rvtransport attach|detach <carrier> <rv|station> [force]  # run the load/unload transactions directly
rvtransport release <rv>                            # emergency release (same function as carrier destruction)
rvtransport sim <carrier> <rv>                      # simulate waiting -> scan -> load on a real map
rvtransport selftest                                # automatic attach/detach check
```

`firstrv` / `firsttrain` may be used instead of a vehicle id.

## Verification scripts

`testrun/*.ps1` (PowerShell) were used while developing; they start a headless server
(`-D -g <savegame>`) and exercise the console commands above. They expect a build in `build/`, a
config in `build/roro-test.cfg` and a savegame in `build/save/`; adapt the paths as needed.

| Script | Checks | State |
|---|---|---|
| `verify_oldsave.ps1` | a pristine 0.73.1 build writes a savegame, this branch loads it | PASS |
| `m1_verify.ps1` | self save/load round-trip | PASS |
| `smoke_m2a.ps1` | empty map + `rvtransport` commands, crash watch | PASS |
| `verify_attach.ps1` | forced attach/detach transactions, the carrying list, and weight accounting (the carrier's consist weight must include the carried vehicle) | PASS |
| `verify_details.ps1` | the line accounting behind the details window's carried-vehicle list (`rvtransport vscroll`: the "vehicles" tab grows by a header plus one line per vehicle) | PASS |
| `verify_wait_tick.ps1` | the waiting state against the game clock (it must survive ticking with a station order, and be given up when the order no longer asks for it) | PASS |
| `verify_part_carrier.ps1` | loading through a *part* of a carrier (a multi-hold ship enters the station part by part) ends up on the carrier's front vehicle | PASS |
| `verify_unload_match.ps1` | loading moves the vehicle on to its drop-off order, and a carrier only drops it at that station | PASS |

Run them all at once with `powershell -File testrun\run_all.ps1 [-Jobs N] [-Only a.ps1,b.ps1]`,
which runs them in parallel and prints a PASS/FAIL summary.
| `verify_intransit.ps1` | carried state survives save/load, then unloads | PASS |
| `verify_sim.ps1` | waiting → station scan → load chain on a real map | PASS |
| `verify_user_save.ps1` | order command chain (`load`/`unload`/`dest`) | PASS |
| `verify_toggle.ps1` | the order window's flag toggling rules (via `rvtransport toggle`, which calls the same `RVTransportToggleOrderFlag()`) | PASS |
| `verify_filter.ps1` | the selection criteria (no criteria, cargo match/mismatch, empty, minimum waiting time too long/off) | PASS |
| `verify_slot.ps1` | the trace restrict slot criterion (invalid slot rejected, non-occupant skipped, occupant taken, removed again) | PASS |
| `verify_destroy.ps1` | destroying a carrier also destroys the road vehicles it carried | PASS |
| `verify_release.ps1` | the manual release path (`RVTransportForceRelease`, now a debug/safety net) | PASS |
| `run_selftest.ps1`, `m1_roundtrip.ps1`, `probe_ai.ps1` | early scaffolding / diagnostics, superseded | disabled |

Note: `delete_vehicle_id` is **not** registered on a dedicated server, so the carrier-destruction
path is exercised through `rvtransport release` (the same `RVTransportForceRelease()` that
`Vehicle::PreDestructor()` calls).

## Design documentation

* `design-overview.zh.md` — full design/planning document (Chinese)
* `implementation-spec.zh.md` — implementation specification, including the review disposition
  table (appendix C) and the progress/verification appendix (appendix D)
* `manual-test-guide.zh.md` — step-by-step manual test guide (Chinese)

## Known limitations / next steps

Work that is deliberately **not** in this branch yet:

* **A road vehicle which is carried when its carrier is destroyed is deleted outright**, so it never
  shows a wreck of its own (the wreck disappears together with the carrier's). Walking through a real
  crash confirms that the effect matches the design, just without that intermediate picture.
* Measured on a real 21 MB savegame (a copy of the tester's, 76 vehicles, day length factor forced to
  1): the achieved game rate is 0.32 game days/s with the feature on and 0.30 with the master switch
  off, i.e. the difference is inside the run-to-run variation - the feature has no measurable overhead
  on that savegame. The measurement is `testrun/measure_perf_run.ps1` (run it with `-Switch on` and
  `-Switch off`): it starts a dedicated server, joins it with a headless dedicated client (a savegame
  can carry a pause mode which `unpause` does not clear) and reports the rate. The scale the tester
  asked for (500 waiting road vehicles, 20-car train) is still unmeasured: creating that many vehicles
  needs tooling the game does not have.
* Full-load measurement (the scale the reviewer asked for): a scenario savegame with **511 vehicles -
  500 of them road vehicles waiting to be transported at one station**, whose carrier's order loads
  road vehicles, was measured with the same script. Both runs advanced the game clock *identically*
  (1950-10-10 → 1950-12-25 → 1951-03-10 → 1951-05-25), i.e. **0.5067 / 0.5000 / 0.5067 game days per
  second with the feature on and exactly the same with it off** - the nominal rate is 0.5, so the
  server keeps up completely and the overhead is below the measurement's resolution (±1 day per
  interval = ±0.7%). The 500 waiting vehicles were created with a temporary debug subcommand which was
  removed again (the measurement is reproducible from this description; `src/console_cmds.cpp` has no
  trace of it).
* The `rvtransport` debug command is still present (to be stripped before merging). A network game is
  covered: `testrun/verify_mp_sync.ps1` starts a dedicated server and
  a headless dedicated client (`-D -n host:port`), and the client's received state of a road vehicle
  which is on board a train has to match the server's field by field, with no desync.

Walked through in game (tester): waiting → loading → transport → unloading, articulated (multi-part)
road vehicles, ship and aircraft carriers, and carrier destruction. See appendix D.3b of the
implementation specification for the notes.

Design decisions worth knowing when reviewing:

* Carried road vehicles are frozen: they do not tick, age, depreciate, pay running costs,
  appear in vehicle lists/groups/statistics, or contribute to the road network.
* While carried, a road vehicle's own cargo is frozen (in-transit time/distance is not advanced);
  the cargo flow graphs therefore do not show the carried leg (it never enters a station cargo slot).
* A road vehicle is only put down at a station **its own schedule** asks for (a "be unloaded here"
  order), so a carrier which never stops at such a station keeps it on board. The advice news
  *"{Vehicle} has been carried for N days without being unloaded"* points that out; it is shown once per
  trip, and the threshold is the setting `vehicle.rv_transport_unload_warn_days` (expert category, on
  the same "Road vehicle transport" page, default 30 days, 0 disables it).
* Which carrier parts may carry a road vehicle is decided by one three-value setting
  (*Settings → Expert → Road vehicle transport → "Which carrier parts may carry road vehicles"*):

  | Value | Rule | Effect on the default game content |
  |---|---|---|
  | 0 | any part with cargo capacity | every wagon/hold can carry a road vehicle |
  | 1 | only a part whose cargo is in the `oversized` class | nothing can carry (no default cargo is oversized) — needs a NewGRF which provides such cargo |
  | **2 (default)** | only a part whose cargo is **bulk**, `oversized`, or the NewGRF "Vehicles" cargo (label `VEHC`) | bulk (hopper/open) wagons can carry — coal, ore, grain, fruit, sugar, toffee; wood/steel/goods vans, passenger carriages, mail vans, tank cars and aircraft do *not* |

  Value 2 is the "GRF-aware" gate: it accepts the cargoes which can physically hold a road vehicle,
  namely bulk cargo (`CargoClass::Bulk`), cargo a NewGRF marks as oversized (`CargoClass::Oversized`,
  the stake/flatbed wagon / car ferry case), and the dedicated `VEHC` ("Vehicles") cargo which car
  ferry and car carrier NewGRFs define for exactly this purpose. A stock game has neither an
  `oversized` nor a `VEHC` cargo, so with it only bulk wagons (coal/ore/grain/…) qualify; a piece
  goods or liquid cargo such as Wood, Steel, Goods or Oil does *not*. Road vehicles are always
  *unloaded* regardless of the setting, so changing it never strands a vehicle which is already on
  board. The regression suite opens the gate explicitly (`setting
  vehicle.rv_transport_carrier_parts 0`) because its test savegame's carrier is a wood wagon; the gate
  itself is covered by `testrun/verify_carrier_parts.ps1`.
* Only station orders can be configured; carried road vehicles and carriers holding them cannot be
  sold until unloaded.
