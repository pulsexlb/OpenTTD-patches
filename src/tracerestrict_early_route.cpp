/* This file is part of OpenTTD, licensed under the GNU GPL, version 2. */
/** @file tracerestrict_early_route.cpp Persisted plans selected by the native rail pathfinder. */
#include "stdafx.h"
#include "tracerestrict_early_route.h"
#include "tracerestrict_overtaking.h"
#include "train.h"
#include "station_base.h"
#include "station_map.h"
#include "rail_map.h"
#include "depot_base.h"
#include "pbs.h"
#include "pathfinder/yapf/yapf.h"
#include "pathfinder/follow_track.hpp"
#include "core/backup_type.hpp"
#include "core/random_func.hpp"
#include "debug.h"
#include <map>
#include <set>

extern const Order *_choose_train_track_saved_current_order;

namespace {
constexpr uint MAX_NATIVE_PLAN_NODES = 256;
constexpr uint MAX_NATIVE_PLAN_STEPS = 2048;
constexpr uint32_t PLAN_VERSION = 1;
uint query_depth = 0;
uint16_t query_slot = UINT16_MAX;
AutoYieldNativeRouteScope::State *current = nullptr;

uint32_t OrderSignature(const Order &order)
{
	uint32_t hash = 2166136261u;
	const auto mix = [&](uint64_t value) {
		hash = (hash ^ uint32_t(value)) * 16777619u;
		hash = (hash ^ uint32_t(value >> 32)) * 16777619u;
	};
	/* Hash order semantics, never its address, padding, display colour or
	 * continually updated occupancy estimate. Stop position and decoupling
	 * have storage separate from raw flags; both can change an exit decision. */
	for (uint64_t value : {uint64_t(order.GetType()), uint64_t(order.GetDestination().base()),
			uint64_t(order.GetRawFlags()), uint64_t(order.GetNonStopType()), uint64_t(order.GetStopLocation()),
			uint64_t(order.GetConditionComparator()), uint64_t(order.GetDecouple()), uint64_t(order.GetNumDecouple()),
			uint64_t(order.GetRefitCargo()), uint64_t(order.GetMaxSpeed()), uint64_t(order.GetXData()), uint64_t(order.GetXData2()),
			uint64_t(order.GetWaitTime()), uint64_t(order.GetTravelTime()), uint64_t(order.IsWaitFixed()), uint64_t(order.IsTravelFixed()),
			uint64_t(order.IsWaitTimetabled()), uint64_t(order.IsTravelTimetabled()), uint64_t(order.GetLeaveType()),
			uint64_t(order.GetDispatchScheduleIndex())}) {
		mix(value);
	}
	for (uint row = 0; row < 2; row++) {
		mix(order.GetCoupleSpeedComparator(row)); mix(order.GetCoupleSpeedValue(row)); mix(order.GetCoupleSpeedUnit(row));
	}
	if (order.GetLoadType() == OrderLoadType::CargoTypeLoad || order.GetUnloadType() == OrderUnloadType::CargoTypeUnload) {
		for (CargoType cargo{0}; cargo < NUM_CARGO; cargo++) {
			mix(to_underlying(order.GetCargoLoadTypeRaw(cargo))); mix(to_underlying(order.GetCargoUnloadTypeRaw(cargo)));
		}
	}
	return hash;
}

uint32_t TrainRouteSignature(const Train *v)
{
	uint32_t hash = 2166136261u;
	const auto mix = [&](uint32_t value) { hash = (hash ^ value) * 16777619u; };
	mix(v->owner.base()); mix(v->group_id.base()); mix(v->IsDrivingBackwards());
	for (const Train *part = v->First(); part != nullptr; part = part->Next()) {
		mix(part->index.base()); mix(part->engine_type.base()); mix(part->gcache.cached_veh_length);
		mix(uint32_t(part->cargo_type)); mix(part->cargo_subtype); mix(part->cargo_cap); mix(part->cargo.StoredCount());
	}
	return hash;
}

/** No order executor is called: future order context is temporary and pure. */
class QueryContext {
	Train *train;
	Order order;
	TileIndex destination;
	StationID last_station;
	VehicleOrderID real_index, implicit_index, timetable_index;
	Randomizer random = _random;
	const Order *saved_order_pointer = _choose_train_track_saved_current_order;
	struct Cache { Train *part; NewGRFCache grf; uint8_t user_data, vehicle_flags; };
	std::vector<Cache> caches;
public:
	QueryContext(const Train *v, const Order &target, VehicleOrderID index) : train(const_cast<Train *>(v)),
			order(v->current_order), destination(v->dest_tile), last_station(v->last_station_visited),
			real_index(v->cur_real_order_index), implicit_index(v->cur_implicit_order_index), timetable_index(v->cur_timetable_order_index)
	{
		/* A PXP primary may sit inside its physical chain. NewGRF resolution
		 * can touch First() and neighbouring parts, not just the primary. */
		for (Train *part = train->First(); part != nullptr; part = part->Next()) {
			caches.push_back({part, part->grf_cache, part->tcache.user_def_data, part->vcache.cached_veh_flags});
		}
		train->current_order = target;
		train->cur_real_order_index = index;
		train->cur_implicit_order_index = index;
		train->cur_timetable_order_index = index;
		/* Native TR Loading conditions consult this pointer before current_order.
		 * Do not let a future GOTO query inherit the outer live loading order. */
		_choose_train_track_saved_current_order = &train->current_order;
		if (index != real_index && order.IsType(OT_GOTO_STATION)) train->last_station_visited = order.GetDestination().ToStationID();
		if (target.IsType(OT_GOTO_STATION) || target.IsType(OT_GOTO_WAYPOINT)) {
			if (const BaseStation *station = BaseStation::GetIfValid(target.GetDestination().ToStationID())) train->dest_tile = station->xy;
		} else if (target.IsType(OT_GOTO_DEPOT)) {
			if (const Depot *depot = Depot::GetIfValid(target.GetDestination().ToDepotID())) train->dest_tile = depot->xy;
		}
	}
	~QueryContext()
	{
		train->current_order = std::move(order); train->dest_tile = destination; train->last_station_visited = last_station;
		train->cur_real_order_index = real_index; train->cur_implicit_order_index = implicit_index; train->cur_timetable_order_index = timetable_index;
		_choose_train_track_saved_current_order = saved_order_pointer;
		for (auto &cache : caches) {
			cache.part->grf_cache = cache.grf; cache.part->tcache.user_def_data = cache.user_data; cache.part->vcache.cached_veh_flags = cache.vehicle_flags;
		}
		_random = random;
	}
};
}

#include "tracerestrict_early_route_plan.inc"

struct AutoYieldNativeRouteScope::State {
	const Train *train;
	TileIndex origin;
	Trackdir origin_td;
	AutoYieldRouteArea area;
	EarlyRailStopPlan plan;
	Order source;
	VehicleOrderID source_index;
	uint32_t source_signature = 0, next_signature = 0;
	bool staged = false, saved = false, onward = false;
	bool inspected = false, relaxed = false;
	bool split_requested = false, split_attempted = false;
	uint32_t split_order_signature = 0;
	bool effective_order = true;
	std::pair<TileIndex, Trackdir> detector{INVALID_TILE, INVALID_TRACKDIR};
	struct Candidate { TileIndex tile; Trackdir td; uint16_t slot; };
	std::vector<Candidate> candidates;
	std::vector<uint32_t> pending;
	std::map<TileIndex, TrackdirBits> corridor;
	std::set<std::pair<TileIndex, Trackdir>> selected_platform;
	std::set<TileIndex> rejected_platforms;
	std::map<uint32_t, std::set<std::pair<TileIndex, Trackdir>>> native_denied;

	void BuildCorridor(const YapfRailRoute &route)
	{
		corridor.clear();
		for (const auto &step : route.path) corridor[step.tile] |= TrackdirToTrackdirBits(step.trackdir);
	}
	bool OrderStillValid() const
	{
		const Order *order = train->GetOrder(source_index);
		if (order == nullptr || OrderSignature(*order) != source_signature) return false;
		if (next_signature != 0) {
			/* The intervening labels/jumps are part of the departure decision.
			 * An unchanged old target is insufficient if an edited jump no
			 * longer reaches it, or now needs live conditional evaluation. */
			const auto next = EarlyRailPlanningDetail::NextExplicitOrder(train, *order, source_index);
			if (next.order == nullptr || next.index != plan.next_order_index) return false;
			order = train->GetOrder(plan.next_order_index);
			if (order == nullptr || OrderSignature(*order) != next_signature) return false;
		}
		return true;
	}
	void BuildPlatformFilter()
	{
		selected_platform.clear(); rejected_platforms.clear();
		if (source.IsType(OT_GOTO_STATION)) for (const auto &step : plan.current.path) {
			if (IsRailStationTile(step.tile) && GetStationIndex(step.tile) == source.GetDestination()) selected_platform.emplace(step.tile, step.trackdir);
		}
		for (const auto &platform : plan.rejected_platforms) {
			const auto delta = TileOffsByDiagDir(TrackdirToExitdir(platform.direction));
			for (TileIndex tile = platform.first; IsValidTile(tile); tile += delta) {
				rejected_platforms.insert(tile); if (tile == platform.last || rejected_platforms.size() >= MAX_NATIVE_PLAN_STEPS) break;
			}
		}
	}
	void SelectEffectiveLeg()
	{
		if (!staged) return;
		const auto signature = OrderSignature(train->current_order);
		if (signature == source_signature) { effective_order = true; onward = false; }
		else if (next_signature != 0 && signature == next_signature) { effective_order = true; onward = true; }
		else { effective_order = false; return; }
		if (saved) BuildCorridor(onward ? plan.onward : plan.current);
	}
};

namespace {
/** Deliberately simple, versioned on-disk representation; no client-local cache is authoritative. */
void WriteRoute(std::vector<uint32_t> &data, const YapfRailRoute &route)
{
	data.push_back(uint32_t(route.path.size()));
	for (const auto &step : route.path) {
		data.insert(data.end(), {step.tile.base(), uint32_t(step.trackdir), step.wormhole_from.base(), uint32_t(step.wormhole_from_trackdir), step.wormhole_tiles});
	}
}
bool ReadRoute(const std::vector<uint32_t> &data, size_t &position, YapfRailRoute &route)
{
	if (position >= data.size()) return false;
	const uint count = data[position++];
	if (count > MAX_NATIVE_PLAN_STEPS || count > (data.size() - position) / 5) return false;
	for (uint i = 0; i < count; i++) {
		YapfRailRouteStep step{TileIndex{data[position]}, Trackdir(data[position + 1]), TileIndex{data[position + 2]}, Trackdir(data[position + 3]), data[position + 4]};
		position += 5;
		if (!IsValidTile(step.tile) || !IsValidTrackdir(step.trackdir) || !HasTrackdir(GetTileTrackdirBits(step.tile, TransportType::Rail, 0), step.trackdir)) return false;
		route.path.push_back(step);
	}
	route.complete = count != 0;
	route.reason = route.complete ? YapfRailRouteReason::Complete : YapfRailRouteReason::InvalidInput;
	return true;
}
void ClipAtClear(YapfRailRoute &route, const AutoYieldRouteArea &area)
{
	for (size_t i = 0; i < route.path.size(); i++) {
		if (std::find(area.clears.begin(), area.clears.end(), std::pair{route.path[i].tile, route.path[i].trackdir}) != area.clears.end()) {
			route.path.resize(i + 1); return;
		}
	}
}

std::vector<uint32_t> EncodePlan(AutoYieldNativeRouteScope::State &s, uint32_t kind)
{
	ClipAtClear(s.plan.current, s.area); ClipAtClear(s.plan.onward, s.area);
	std::vector<uint32_t> data{PLAN_VERSION, s.area.slot, s.train->orders->index.base(), uint32_t(s.source_index), s.source_signature,
			uint32_t(s.plan.next_order_index), s.next_signature, s.plan.selected_clear->first.base(), uint32_t(s.plan.selected_clear->second), s.area.signature, kind, TrainRouteSignature(s.train)};
	WriteRoute(data, s.plan.current); WriteRoute(data, s.plan.onward);
	return data;
}
}

AutoYieldNativeRouteScope::AutoYieldNativeRouteScope(const Train *v, TileIndex origin, Trackdir td) : previous(current)
{
	if (query_depth != 0 || !_settings_game.vehicle.automatic_yield || v == nullptr) return;
	if (previous != nullptr && previous->train == v) { active = previous; active->SelectEffectiveLeg(); return; }
	if (v->early_route.empty() && !AutoYieldRouteAreasAvailable()) return;
	state = std::make_unique<State>();
	active = state.get();
	auto &s = *state; s.train = v; s.origin = origin; s.origin_td = td; s.source = v->current_order; s.source_index = v->cur_real_order_index;
	current = &s;
	const auto reject = [&](std::string_view reason) { Debug(yapf, 7, "EarlyRoute evidence train={} origin={} reason={}", v->index, origin, reason); };
	const auto &data = v->early_route;
	if (data.size() < 14 || data[0] != PLAN_VERSION || (data[10] != 1 && data[10] != 2)) { reject("no-valid-plan"); return; }
	if (data[11] != TrainRouteSignature(v)) { reject("consist-changed"); return; } // Real loading/refitting/reversal requires native re-evaluation.
	if (!GetAutoYieldRouteArea(v, uint16_t(data[1]), s.area)) { reject("area-unavailable"); return; }
	if (s.area.signature != data[9]) { reject("area-changed"); return; }
	if (v->orders == nullptr || v->orders->index.base() != data[2]) { reject("order-list-changed"); return; }
	s.source_index = VehicleOrderID(data[3]); s.source_signature = data[4];
	s.plan.next_order_index = VehicleOrderID(data[5]); s.next_signature = data[6];
	s.plan.selected_clear = std::pair{TileIndex{data[7]}, Trackdir(data[8])};
	if (std::find(s.area.clears.begin(), s.area.clears.end(), *s.plan.selected_clear) == s.area.clears.end()) return;
	if (!s.OrderStillValid()) { reject("source-or-next-order-changed"); return; }
	s.source = *v->GetOrder(s.source_index);
	size_t pos = 12;
	if (!ReadRoute(data, pos, s.plan.current) || !ReadRoute(data, pos, s.plan.onward) || pos != data.size()) { reject("invalid-path"); return; }
	const auto signature = OrderSignature(v->current_order);
	if (signature == s.source_signature) s.onward = false;
	else if (s.next_signature != 0 && signature == s.next_signature) s.onward = true;
	else if ((v->current_order.IsAnyLoadingType() || v->current_order.IsType(OT_LEAVESTATION)) &&
			v->GetOrder(s.source_index)->IsType(OT_GOTO_STATION) && v->GetOrder(s.source_index)->GetDestination() == v->last_station_visited) s.onward = false;
	else { reject("active-order-not-in-plan"); return; }
	s.BuildCorridor(s.onward ? s.plan.onward : s.plan.current);
	s.BuildPlatformFilter();
	/* A reversal or a legally changed PBS corridor is a new decision point. */
	if (auto it = s.corridor.find(origin); it != s.corridor.end() && !HasTrackdir(it->second, td)) { s.corridor.clear(); return; }
	s.saved = data[10] == 2; s.staged = true; s.inspected = true;
}
AutoYieldNativeRouteScope::~AutoYieldNativeRouteScope() { if (state) current = previous; }

void AutoYieldNativeRouteScope::PrepareFromContext(TileIndex origin, Trackdir td)
{
	if (active == nullptr || active->staged || active->inspected) return;
	uint16_t slot = active->train->early_route_area;
	if (slot == UINT16_MAX) {
		/* Old saves can already own a detector slot but predate per-train
		 * route context. Recover only an unambiguous existing multi-outlet
		 * membership. This query neither registers a train nor publishes state;
		 * Finish still requires a successful native reservation to do that. */
		if (!active->train->vehicle_flags.Test(VehicleFlag::HaveSlot)) return;
		std::vector<TraceRestrictSlotID> memberships;
		TraceRestrictGetVehicleSlots(active->train->index, memberships);
		if (memberships.size() > 32) return;
		for (auto id : memberships) {
			AutoYieldRouteArea candidate;
			if (!GetAutoYieldRouteArea(active->train, id.base(), candidate) || !candidate.registered) continue;
			if (slot != UINT16_MAX) {
				Debug(yapf, 7, "EarlyRoute train={} reason=ambiguous-legacy-area", active->train->index);
				return;
			}
			slot = id.base();
		}
		if (slot == UINT16_MAX) return;
	}
	AutoYieldRouteArea area;
	if (!GetAutoYieldRouteArea(active->train, slot, area) || !area.registered) return;
	/* A track/order edit after the detector must not make replanning wait for
	 * another lap. The active saved slot supplies context, not a reservation;
	 * native selection still starts at the end of the currently owned prefix. */
	AutoYieldStageNativeRoute(active->train, origin, td, area.slot, true);
}

bool AutoYieldRouteQuerySuppresses(uint16_t slot) { return query_depth != 0 && slot == query_slot; }
bool AutoYieldHasNativeRoute(const Train *v) { return query_depth == 0 && current != nullptr && current->train == v && current->staged && current->effective_order; }

void AutoYieldStageNativeRoute(const Train *v, TileIndex detector, Trackdir td, uint16_t slot, bool authoritative)
{
	if (query_depth != 0 || current == nullptr || current->train != v || current->staged) return;
	auto &s = *current;
	if (!GetAutoYieldRouteArea(v, slot, s.area)) return;
	if (!authoritative) {
		if (s.candidates.size() < 16 && std::none_of(s.candidates.begin(), s.candidates.end(), [=](const auto &item) { return item.tile == detector && item.td == td && item.slot == slot; })) s.candidates.push_back({detector, td, slot});
		return;
	}
	/* A single bounded decision per search, even if native search revisits the detector. */
	s.staged = true; s.inspected = true; s.detector = {detector, td}; s.source = v->current_order; s.source_index = v->cur_real_order_index; s.source_signature = OrderSignature(s.source);
	uint remaining = MAX_NATIVE_PLAN_NODES;
	AutoRestoreBackup depth(query_depth, query_depth + 1);
	AutoRestoreBackup area(query_slot, slot);
	const EarlyRailQuery query = [&](const Order &order, VehicleOrderID index, TileIndex start, Trackdir direction, const YapfRailRouteFilter &filter) {
		QueryContext context(v, order, index);
		const auto denied = s.native_denied.find(OrderSignature(order));
		const YapfRailRouteFilter admit = [&](TileIndex tile, Trackdir td) {
			return (!filter || filter(tile, td)) && (denied == s.native_denied.end() || !denied->second.contains({tile, td}));
		};
		const YapfRailRouteFilter export_end = [&](TileIndex tile, Trackdir td) {
			return std::find(s.area.clears.begin(), s.area.clears.end(), std::pair{tile, td}) != s.area.clears.end();
		};
		return YapfTrainPlanRoute(v, start, direction, remaining, admit, INVALID_TILE, INVALID_TRACKDIR, MAX_NATIVE_PLAN_STEPS, std::nullopt, export_end);
	};
	s.plan = BuildEarlyRailStopPlan(v, detector, td, s.area.clears, remaining, query);
	if (const Order *next = v->GetOrder(s.plan.next_order_index); s.plan.onward.complete && next != nullptr) s.next_signature = OrderSignature(*next);
	s.BuildPlatformFilter();
	Debug(yapf, 3, "EarlyRoute stage train={} area={} detector={} reason={} nodes={} current={} onward={} exit={}",
			v->index, slot, detector, uint(s.plan.reason), MAX_NATIVE_PLAN_NODES - remaining, s.plan.current.path.size(), s.plan.onward.path.size(), s.plan.selected_clear ? s.plan.selected_clear->first : INVALID_TILE);
}

bool AutoYieldNativeRouteAllows(const Train *v, TileIndex tile, Trackdir td)
{
	if (!AutoYieldHasNativeRoute(v)) return true;
	const auto &s = *current;
	/* Frozen before the fresh retry starts. Evidence collected during the
	 * previous A* must not alter that A*'s filter midway through its search. */
	if (s.split_attempted && s.split_order_signature == (s.onward ? s.next_signature : s.source_signature)) {
		const auto denied = s.native_denied.find(s.split_order_signature);
		if (denied != s.native_denied.end() && denied->second.contains({tile, td})) return false;
	}
	if (s.rejected_platforms.contains(tile)) return false;
	if (s.plan.selected_clear && std::find(s.area.clears.begin(), s.area.clears.end(), std::pair{tile, td}) != s.area.clears.end()) return *s.plan.selected_clear == std::pair{tile, td};
	if (s.saved) {
		const auto it = s.corridor.find(tile);
		return it == s.corridor.end() || HasTrackdir(it->second, td);
	}
	if (!s.plan.current.complete) return true;
	if (!s.selected_platform.empty() && IsRailStationTile(tile) && GetStationIndex(tile) == s.source.GetDestination()) return s.selected_platform.contains({tile, td});
	return true;
}

void AutoYieldNativeRouteDenied(const Train *v, TileIndex tile, Trackdir td)
{
	if (!AutoYieldHasNativeRoute(v) || !IsValidTile(tile) || !IsValidTrackdir(td)) return;
	/* The one split retry consumes this existing map as an immutable filter.
	 * It cannot replan again, so later evidence is unused and must not mutate
	 * the active graph. No extra per-decision set is needed for ordinary trains. */
	if (current->split_attempted) return;
	/* Pure planning suppresses this area's recursive yield evaluation. If the
	 * real search subsequently forbids the chosen platform's approach, retain
	 * that directed evidence for its one fresh replan. Never change the filter
	 * of an A* already in progress, and never persist a temporary signal result. */
	auto &denied = current->native_denied[OrderSignature(v->current_order)];
	if (denied.size() < 64) denied.emplace(tile, td);
}

std::optional<std::pair<TileIndex, Trackdir>> AutoYieldNativeRouteRequiredPoint(const Train *v, TileIndex origin, Trackdir td)
{
	if (!AutoYieldHasNativeRoute(v) || !current->plan.selected_clear) return std::nullopt;
	const auto &s = *current;
	const auto &route = s.onward ? s.plan.onward : s.plan.current;
	const auto point = *s.plan.selected_clear;
	const auto clear = std::find_if(route.path.begin(), route.path.end(), [&](const auto &step) { return std::pair{step.tile, step.trackdir} == point; });
	if (clear == route.path.end()) return std::nullopt; // This leg must stop at its platform before Clear.
	if (std::pair{origin, td} == point || TrainReservationPassesThroughTrackdir(v, point.first, point.second)) return std::nullopt;
	if (s.saved && std::none_of(route.path.begin(), clear, [&](const auto &step) { return step.tile == origin && step.trackdir == td; })) {
		/* A saved corridor ends at Clear. A later origin outside that corridor
		 * is already beyond it, except a real station departure which starts
		 * earlier than the platform-end native query used in the onward leg. */
		if (!s.onward || !IsRailStationTile(origin) || !s.source.IsType(OT_GOTO_STATION) || GetStationIndex(origin) != s.source.GetDestination()) return std::nullopt;
	}
	return point;
}

void AutoYieldNativeRouteScope::Commit(const YapfRailRoute &actual)
{
	if (!active || !active->staged || !active->effective_order || active->saved || !actual.complete || !active->plan.current.complete || !active->plan.selected_clear || !active->OrderStillValid()) return;
	auto &s = *active;
	if (std::any_of(actual.path.begin(), actual.path.end(), [&](const auto &step) { return !AutoYieldNativeRouteAllows(s.train, step.tile, step.trackdir); })) return;
	if (s.detector.first != INVALID_TILE && std::none_of(actual.path.begin(), actual.path.end(), [&](const auto &step) { return std::pair{step.tile, step.trackdir} == s.detector; })) {
		/* Extension may already own the detector prefix. This decision can be
		 * applied only to that same continuous native reservation. */
		if (!TrainReservationPassesThroughTrackdir(s.train, s.detector.first, s.detector.second)) return;
	}
	if (s.onward) s.plan.onward = actual; else s.plan.current = actual;
	if (s.plan.current.path.size() > MAX_NATIVE_PLAN_STEPS || s.plan.onward.path.size() > MAX_NATIVE_PLAN_STEPS) return;
	s.pending = EncodePlan(s, 2);
}

bool AutoYieldNativeRouteScope::NeedsInspection() const { return active != nullptr && ((active->staged && !active->saved) || (!active->inspected && !active->candidates.empty())); }
bool AutoYieldNativeRouteScope::NeedsSelection() const { return active != nullptr && !active->staged && !active->inspected && !active->candidates.empty(); }

bool AutoYieldNativeRouteScope::ExportEnd(TileIndex tile, Trackdir td) const
{
	if (active == nullptr) return false;
	if (active->staged) return std::find(active->area.clears.begin(), active->area.clears.end(), std::pair{tile, td}) != active->area.clears.end();
	return std::any_of(active->candidates.begin(), active->candidates.end(), [=](const auto &candidate) { return candidate.tile == tile && candidate.td == td; });
}

bool AutoYieldNativeRouteScope::Select(const YapfRailRoute &actual)
{
	if (active == nullptr || active->staged || active->inspected || !actual.complete) return false;
	active->inspected = true;
	for (const auto &step : actual.path) for (const auto candidate : active->candidates) {
		if (step.tile != candidate.tile || step.trackdir != candidate.td) continue;
		AutoYieldStageNativeRoute(active->train, candidate.tile, candidate.td, candidate.slot, true);
		return active->staged && active->plan.current.complete;
	}
	return false;
}

bool AutoYieldNativeRouteScope::RelaxForReplan()
{
	if (active == nullptr || !active->staged || active->relaxed) return false;
	auto &s = *active;
	const State backup = s;
	const auto origin = FollowTrainReservation(s.train, nullptr, FollowTrainReservationFlag::OkayUnused);
	/* A conditional signal or a changed platform may make the old exit
	 * unusable even though its track still exists. Ask the native chooser
	 * again from the owned prefix; do not force it back to the old exit. */
	s.staged = false; s.saved = false; s.inspected = false; s.relaxed = true;
	s.plan = {}; s.next_signature = 0; s.pending.clear(); s.corridor.clear(); s.selected_platform.clear(); s.rejected_platforms.clear();
	AutoYieldStageNativeRoute(s.train, origin.tile, origin.trackdir, backup.area.slot, true);
	/* An unresolved conditional departure has no selected exit to preserve.
	 * Its newly proved current stop and physical departure may replace a
	 * platform that the real search denied. Never replace an established exit
	 * with an inconclusive future query. */
	if (s.plan.current.complete && (s.plan.selected_clear || (!backup.plan.selected_clear && s.plan.physical_exit_proven))) return true;
	/* An inconclusive query cannot clear an established obligation. Retry
	 * ordinary native selection through its still-valid exit/platform while
	 * allowing a different lane; publication still requires a real success. */
	s = backup;
	/* Re-run native selection through the same proven exit/platform while
	 * retaining owned PBS. A temporary block must not permanently pin a lane. */
	s.relaxed = true; s.saved = false; s.corridor.clear();
	s.BuildPlatformFilter();
	return true;
}

bool AutoYieldNativeRouteScope::ReplanForSplit()
{
	if (active == nullptr || !active->split_requested || active->split_attempted || active->relaxed) return false;
	active->split_requested = false;
	active->split_attempted = true;
	return RelaxForReplan();
}

bool AutoYieldMayRequestSplitAlternative(const Train *v, uint16_t slot)
{
	return query_depth == 0 && current != nullptr && current->train == v && current->staged && current->effective_order &&
			current->area.slot == slot && current->area.clears.size() > 1 && !current->relaxed && !current->split_attempted && !current->split_requested;
}

bool AutoYieldRequestSplitAlternative(const Train *v, uint16_t slot, const std::vector<std::pair<TileIndex, Trackdir>> &blocked)
{
	if (!AutoYieldMayRequestSplitAlternative(v, slot) || blocked.empty() || blocked.size() > 64) return false;
	const auto &path = current->onward ? current->plan.onward.path : current->plan.current.path;
	for (const auto &[tile, td] : blocked) {
		if (!IsValidTile(tile) || !IsValidTrackdir(td) || TrainReservationPassesThroughTrackdir(v, tile, td) ||
				std::none_of(path.begin(), path.end(), [=](const auto &step) { return step.tile == tile && step.trackdir == td; })) return false;
	}
	auto &denied = current->native_denied[OrderSignature(v->current_order)];
	if (denied.size() + blocked.size() > 64) return false;
	denied.insert(blocked.begin(), blocked.end());
	current->split_order_signature = OrderSignature(v->current_order);
	current->split_requested = true;
	Debug(yapf, 6, "EarlyRoute train={} area={} reason=split-independent-alternative-request resources={}", v->index, slot, blocked.size());
	return true;
}

void AutoYieldNativeRouteScope::Finish(bool reservation_made)
{
	if (!state || !reservation_made) return;
	const bool owns_detector = state->staged && state->OrderStillValid() && state->detector.first != INVALID_TILE &&
			TrainReservationPassesThroughTrackdir(state->train, state->detector.first, state->detector.second);
	if (owns_detector) {
		/* A conditional or side-effect order can defer the exit decision while
		 * native PBS legitimately reserves the current stop. Keep only its area
		 * context, so a later legal decision can plan after the order executes. */
		const_cast<Train *>(state->train)->early_route_area = state->area.slot;
	}
	if (state->pending.empty() && state->staged && state->effective_order && !state->saved && state->plan.current.complete && state->plan.selected_clear && state->OrderStillValid() &&
			owns_detector) {
		/* A real extension can end at a safe signal before the first fork.
		 * Keep the selected native plan without claiming that its whole route
		 * was reserved. The first later PF adopts an actual corridor (kind 2). */
		state->pending = EncodePlan(*state, 1);
	}
	if (state->pending.empty()) return;
	const_cast<Train *>(state->train)->early_route = std::move(state->pending);
	const_cast<Train *>(state->train)->early_route_area = state->area.slot;
	Debug(yapf, 3, "EarlyRoute commit train={} area={} exit={} current={} onward={}", state->train->index, state->area.slot, state->plan.selected_clear->first, state->plan.current.path.size(), state->plan.onward.path.size());
}

bool AutoYieldGetNativeRoute(const Train *v, uint16_t slot, TileIndex start, Trackdir td, YapfRailRoute &route)
{
	if (query_depth != 0) return false;
	const auto collect = [&](const AutoYieldNativeRouteScope::State &s) {
		if (!s.staged || !s.effective_order || s.area.slot != slot || !s.plan.selected_clear) return false;
		std::vector<YapfRailRouteStep> steps = s.onward ? s.plan.onward.path : s.plan.current.path;
		if (!s.onward && s.plan.onward.complete) {
			auto begin = s.plan.onward.path.begin();
			if (!steps.empty() && begin != s.plan.onward.path.end() && steps.back().tile == begin->tile && steps.back().trackdir == begin->trackdir) ++begin;
			steps.insert(steps.end(), begin, s.plan.onward.path.end());
		}
		auto begin = std::find_if(steps.begin(), steps.end(), [=](const auto &step) { return step.tile == start && step.trackdir == td; });
		if (begin == steps.end() && !steps.empty()) {
			/* Native replanning begins at the end of the existing reservation.
			 * That selected leg alone omits the train's still-owned approach.
			 * Join only the actual continuous PBS prefix, never a possible route. */
			std::vector<std::pair<TileIndex, Trackdir>> prefix;
			if (GetTrainReservationPathTo(v, steps.front().tile, steps.front().trackdir, MAX_NATIVE_PLAN_STEPS, prefix)) {
				const auto from = std::find(prefix.begin(), prefix.end(), std::pair{start, td});
				bool adjacent = true;
				if (from != prefix.end()) for (auto it = from + 1; it != prefix.end(); ++it) {
					if (DistanceManhattan((it - 1)->first, it->first) != 1) { adjacent = false; break; }
				}
				/* A skipped bridge/station arc needs its native span metadata;
				 * keep that uncommon incomplete-prefix case conservative. */
				if (from != prefix.end() && adjacent && size_t(prefix.end() - from) + steps.size() - 1 <= MAX_NATIVE_PLAN_STEPS) {
					std::vector<YapfRailRouteStep> joined;
					for (auto it = from; it + 1 != prefix.end(); ++it) joined.push_back({it->first, it->second});
					joined.insert(joined.end(), steps.begin(), steps.end()); steps = std::move(joined); begin = steps.begin();
				}
			}
		}
		if (begin == steps.end()) {
			Debug(yapf, 7, "EarlyRoute evidence train={} start={} td={} reason=origin-outside-selected-leg first={} last={}",
					v->index, start, uint(td), steps.empty()?INVALID_TILE:steps.front().tile, steps.empty()?INVALID_TILE:steps.back().tile);
			return false;
		}
		route.path.assign(begin, steps.end()); ClipAtClear(route, s.area);
		route.complete = !route.path.empty() && std::pair{route.path.back().tile, route.path.back().trackdir} == *s.plan.selected_clear;
		return route.complete;
	};
	if (current != nullptr && current->train == v && collect(*current)) return true;
	if (v->early_route.empty()) return false;
	AutoYieldNativeRouteScope saved(v, start, td);
	return current != nullptr && current->train == v && collect(*current);
}

bool AutoYieldNativeStopNeedsApproach(const Train *v, uint16_t slot, TileIndex tile, Trackdir td)
{
	if (query_depth != 0) return false;
	const auto needed = [&](const AutoYieldNativeRouteScope::State &s) {
		if (!s.staged || !s.effective_order || s.onward || s.area.slot != slot ||
				!s.plan.selected_clear || !s.plan.onward.complete || s.selected_platform.empty()) return false;
		const auto &path = s.plan.current.path;
		const auto first_platform = std::find_if(path.begin(), path.end(), [&](const auto &step) { return s.selected_platform.contains({step.tile, step.trackdir}); });
		const auto clearance = std::find_if(path.begin(), path.end(), [&](const auto &step) { return std::pair{step.tile, step.trackdir} == *s.plan.selected_clear; });
		if (first_platform == path.end() || (clearance != path.end() && clearance <= first_platform)) return false; // The stop must be inside this area.
		auto point = std::find_if(path.begin(), path.end(), [=](const auto &step) { return step.tile == tile && step.trackdir == td; });
		if (point == path.end()) {
			/* A new leg starts at the owned prefix's end, possibly after Main.
			 * Prove only its short unique directed predecessor, never search an
			 * alternative branch or infer it from coordinates. Ambiguity, skipped
			 * stations/bridges and this fixed local cap leave ordinary rules intact. */
			if (path.empty()) return false;
			TileIndex before = path.front().tile;
			Trackdir reverse = ReverseTrackdir(path.front().trackdir);
			bool reached = false;
			for (uint n = 0; n < 64; n++) {
				if (before == tile && reverse == ReverseTrackdir(td)) { reached = true; break; }
				CFollowTrackRailNo90 follow(v, v->GetIndirectCompatibleRailTypes());
				if (!follow.Follow(before, reverse) || follow.tiles_skipped != 0 || follow.new_td_bits == TRACKDIR_BIT_NONE ||
						KillFirstBit(follow.new_td_bits) != TRACKDIR_BIT_NONE) return false;
				before = follow.new_tile; reverse = FindFirstTrackdir(follow.new_td_bits);
			}
			if (!reached) return false;
			point = path.begin();
		}
		const auto platform = std::find_if(point, path.end(), [&](const auto &step) { return s.selected_platform.contains({step.tile, step.trackdir}); });
		/* A platform on Main itself keeps ordinary platform/siding diversion.
		 * This exception is only its actual downstream branching approach. */
		return platform != path.end() && std::any_of(point, platform, [=](const auto &step) { return step.trackdir != td; });
	};
	if (current != nullptr && current->train == v) return needed(*current);
	if (v->early_route.empty()) return false;
	const Train *front = v->GetMovingFront();
	AutoYieldNativeRouteScope saved(v, front->tile, front->GetVehicleTrackdir());
	return current != nullptr && current->train == v && needed(*current);
}

void AutoYieldInvalidateNativeRoutes(TileIndex tile)
{
	if (!IsValidTile(tile)) return;
	/* Only trains with plans incur work on an actual layout command. A nearby
	 * new connector can improve a valid old route too, so invalidate the local
	 * corridor, not only an exactly deleted tile. Native reservations remain. */
	for (Train *v : Train::Iterate()) {
		if (v->early_route.empty()) continue;
		const auto &data = v->early_route;
		size_t position = 12;
		bool invalidate = false;
		for (uint leg = 0; leg < 2 && position < data.size(); leg++) {
			const size_t count = data[position++];
			if (count > (data.size() - position) / 5) { invalidate = true; break; }
			for (size_t i = 0; i < count; i++, position += 5) {
				const TileIndex planned{data[position]};
				if (IsValidTile(planned) && DistanceMax(planned, tile) <= 8) invalidate = true;
			}
		}
		if (invalidate) v->early_route.clear();
	}
}
