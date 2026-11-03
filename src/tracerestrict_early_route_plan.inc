/* This file is part of OpenTTD, licensed under the GNU GPL, version 2. */
/* Included at file scope by tracerestrict_early_route.cpp. No order executor,
 * vehicle mutation, reservation transaction, or automatic-yield policy lives
 * here. The caller supplies a scoped-pure native query and owns persistence. */
#include "depot_map.h"

enum class EarlyRailPlanReason : uint8_t {
	Complete,
	InvalidInput,
	NoCurrentRoute,
	DeferredNoNextOrder,
	DeferredConditional,
	DeferredSideEffect,
	DeferredScheduleReturn,
	DeferredNativeQuery,
	DeferredNodeBudget,
	DeferredUnsupportedReverse,
	DeferredNoClearance,
	DeferredShortPlatform,
	DeferredIntermediateStop,
	DeferredBlocked,
	CandidateLimit,
};

struct EarlyRailRejectedPlatform {
	TileIndex first, last;
	Trackdir direction;
	bool unreachable_next_target = false;
};

struct EarlyRailStopPlan {
	YapfRailRoute current, onward;
	std::optional<std::pair<TileIndex, Trackdir>> selected_clear;
	VehicleOrderID next_order_index = INVALID_VEH_ORDER_ID;
	EarlyRailPlanReason reason = EarlyRailPlanReason::InvalidInput;
	std::vector<EarlyRailRejectedPlatform> rejected_platforms;
	bool physical_exit_proven = false;
};

using EarlyRailQuery = std::function<YapfRailRoute(const Order &, VehicleOrderID, TileIndex, Trackdir, const YapfRailRouteFilter &)>;

namespace EarlyRailPlanningDetail {
constexpr uint MAX_PLATFORM_CANDIDATES = 16;
enum class Reach : uint8_t { Yes, No, Unknown };

struct NextOrder {
	const Order *order = nullptr;
	VehicleOrderID index = INVALID_VEH_ORDER_ID;
	EarlyRailPlanReason reason = EarlyRailPlanReason::DeferredNoNextOrder;
};

/** Only labels and unconditional control flow are safe to skip. In particular,
 * a slot/counter action can change native signal restrictions before departure;
 * knowing the next physical target does not make that side effect predictable. */
static NextOrder NextExplicitOrder(const Train *v, const Order &current_order, VehicleOrderID source_index)
{
	NextOrder result;
	const auto count = v->GetNumOrders();
	if (v->orders == nullptr || count == 0 || source_index >= count) return result;
	const Order *source = v->GetOrder(source_index);
	if (source == nullptr || source->GetType() != current_order.GetType() || source->GetDestination() != current_order.GetDestination()) return result;
	if (source->GetDecouple() != ODF_NOTHING) { result.reason = EarlyRailPlanReason::DeferredSideEffect; return result; }
	std::set<VehicleOrderID> seen{source_index};
	VehicleOrderID index = source_index;
	for (uint examined = 0; examined < std::min<uint>(count, 64); examined++) {
		if (index + 1 >= count && v->IsExecutingSchedule()) { result.reason = EarlyRailPlanReason::DeferredScheduleReturn; return result; }
		index = index + 1 < count ? index + 1 : 0;
		for (;;) {
			if (index >= count || !seen.insert(index).second) return result;
			const Order *order = v->GetOrder(index);
			if (order == nullptr) return result;
			if (!order->IsType(OT_CONDITIONAL)) break;
			if (order->GetConditionVariable() != OrderConditionVariable::Unconditionally) {
				result.reason = EarlyRailPlanReason::DeferredConditional; return result;
			}
			index = order->GetConditionSkipToOrder();
		}
		const Order *order = v->GetOrder(index);
		if (order->IsType(OT_LABEL)) continue;
		if (order->IsType(OT_GOTO_STATION) || order->IsType(OT_GOTO_WAYPOINT) ||
				(order->IsType(OT_GOTO_DEPOT) && !(order->GetDepotActionType() & ODATFB_NEAREST_DEPOT) &&
				 !order->GetDepotOrderType().Test(OrderDepotTypeFlag::Service))) {
			result.order = order; result.index = index; result.reason = EarlyRailPlanReason::Complete;
			return result;
		}
		result.reason = order->IsType(OT_EXECUTE_SCHEDULE) ? EarlyRailPlanReason::DeferredScheduleReturn : EarlyRailPlanReason::DeferredSideEffect;
		return result;
	}
	return result;
}

static NextOrder NextExplicitOrder(const Train *v)
{
	return NextExplicitOrder(v, v->current_order, v->cur_real_order_index);
}

static std::optional<std::pair<TileIndex, Trackdir>> FirstClear(const YapfRailRoute &route, const std::vector<std::pair<TileIndex, Trackdir>> &clears)
{
	for (const auto &step : route.path) {
		const std::pair at{step.tile, step.trackdir};
		if (std::find(clears.begin(), clears.end(), at) != clears.end()) return at;
	}
	return std::nullopt;
}

static bool AtTarget(const Order &order, TileIndex tile)
{
	if (order.IsType(OT_GOTO_STATION)) return IsRailStationTile(tile) && GetStationIndex(tile) == order.GetDestination().ToStationID();
	if (order.IsType(OT_GOTO_WAYPOINT)) return IsRailWaypoint(tile) && GetStationIndex(tile) == order.GetDestination().ToStationID();
	return order.IsType(OT_GOTO_DEPOT) && IsRailDepotTile(tile) && GetDepotIndex(tile) == order.GetDestination().ToDepotID();
}

/** A conservative physical connectivity proof, never a route selection.
 * Ignore reservations and ordinary conditional Deny (which may be temporary),
 * but respect ownership, compatible rail, turn rules and physical one-way
 * signalling. A "No" can reject a truly disconnected candidate; a "Yes"
 * cannot replace the native query or choose an unknown future destination. */
template <class Follower>
static Reach PhysicalReach(const Train *v, TileIndex start, Trackdir td,
		const std::vector<std::pair<TileIndex, Trackdir>> &clears, const Order *target, uint &remaining)
{
	std::vector<std::pair<TileIndex, Trackdir>> pending{{start, td}};
	std::set<std::pair<TileIndex, Trackdir>> seen;
	bool unknown_reverse = false;
	for (size_t head = 0; head < pending.size(); head++) {
		if (remaining == 0) return Reach::Unknown;
		const auto [tile, direction] = pending[head];
		if (!seen.insert({tile, direction}).second) continue;
		--remaining;
		if (HasOnewaySignalBlockingTrackdir(tile, direction)) continue;
		if (target != nullptr ? AtTarget(*target, tile) : std::find(clears.begin(), clears.end(), std::pair{tile, direction}) != clears.end()) return Reach::Yes;
		if (IsRailDepotTile(tile)) unknown_reverse = true; // Native YAPF may leave a non-target depot reversed.
		if (IsPlainRailTile(tile) || IsRailTunnelBridgeTile(tile)) {
			if (const auto *program = GetExistingTraceRestrictProgram(tile, TrackdirToTrack(direction)); program != nullptr &&
					(program->actions_used_flags & (TRPAUF_REVERSE_BEHIND | TRPAUF_REVERSE_AT))) unknown_reverse = true;
		}
		/* No live order overlay is needed for a connectivity over-approximation.
		 * In particular, a currently inactive depot order must not make an
		 * otherwise connected future depot look physically unreachable. */
		Follower follower{v->owner, v->compatible_railtypes};
		if (!follower.Follow(tile, direction)) continue;
		for (TrackdirBits choices = follower.new_td_bits; choices != TRACKDIR_BIT_NONE; choices = KillFirstBit(choices)) {
			pending.emplace_back(follower.new_tile, FindFirstTrackdir(choices));
		}
	}
	return unknown_reverse ? Reach::Unknown : Reach::No;
}

static Reach PhysicalReach(const Train *v, TileIndex start, Trackdir td,
		const std::vector<std::pair<TileIndex, Trackdir>> &clears, const Order *target, uint &remaining)
{
	return _settings_game.pf.forbid_90_deg
		? PhysicalReach<CFollowTrackRailNo90>(v, start, td, clears, target, remaining)
		: PhysicalReach<CFollowTrackRail>(v, start, td, clears, target, remaining);
}

static EarlyRailPlanReason DeferredQueryReason(const YapfRailRoute &route)
{
	if (route.reason == YapfRailRouteReason::NodeBudget || route.reason == YapfRailRouteReason::RouteLimit) return EarlyRailPlanReason::DeferredNodeBudget;
	if (route.reason == YapfRailRouteReason::UnsupportedReverse) return EarlyRailPlanReason::DeferredUnsupportedReverse;
	return EarlyRailPlanReason::DeferredNativeQuery;
}
}

/** The query must cap itself to remaining_nodes, but not charge the budget.
 * All confirmed routes come from that native query. Topology is used solely to
 * disprove permanent connectivity, never to invent a route or an exit. */
static EarlyRailStopPlan BuildEarlyRailStopPlan(const Train *v, TileIndex origin, Trackdir td,
		const std::vector<std::pair<TileIndex, Trackdir>> &clears, uint &remaining_nodes,
		const EarlyRailQuery &query, const YapfRailRouteFilter &filter = {})
{
	using namespace EarlyRailPlanningDetail;
	EarlyRailStopPlan result;
	if (v == nullptr || !query || !IsValidTile(origin) || !IsValidTrackdir(td) || clears.size() < 2) return result;
	std::set<TileIndex> excluded;
	std::vector<EarlyRailStopPlan> deferred;
	std::vector<EarlyRailRejectedPlatform> proven_rejections;
	/* A native onward failure can be temporary or require the native reverse
	 * policy. Try other incoming platforms first, retaining the native-cheapest
	 * uncertain candidate. Only an exhausted physical graph may reject it. */
	const auto fallback = [&]() {
		for (auto candidate : deferred) {
			candidate.rejected_platforms = proven_rejections;
			if (candidate.onward.reason != YapfRailRouteReason::NoPath || candidate.current.path.empty()) return candidate;
			const auto endpoint = candidate.current.path.back();
			const auto exit_direction = TrackdirToExitdir(endpoint.trackdir);
			const uint length = Station::GetByTile(endpoint.tile)->GetPlatformLength(endpoint.tile, ReverseDiagDir(exit_direction));
			const TileIndex near_end = endpoint.tile - TileOffsByDiagDir(exit_direction) * (length - 1);
			const Order *next = v->GetOrder(candidate.next_order_index);
			if (next == nullptr) return candidate;
			const auto forward = PhysicalReach(v, endpoint.tile, endpoint.trackdir, clears, next, remaining_nodes);
			if (forward != Reach::No) {
				candidate.reason = forward == Reach::Yes ? EarlyRailPlanReason::DeferredBlocked : EarlyRailPlanReason::DeferredNodeBudget;
				return candidate;
			}
			const auto reverse = PhysicalReach(v, near_end, ReverseTrackdir(endpoint.trackdir), clears, next, remaining_nodes);
			if (reverse != Reach::No) {
				candidate.reason = remaining_nodes == 0 ? EarlyRailPlanReason::DeferredNodeBudget : EarlyRailPlanReason::DeferredUnsupportedReverse;
				return candidate;
			}
			proven_rejections.push_back({near_end, endpoint.tile, endpoint.trackdir, true});
		}
		EarlyRailStopPlan failed;
		failed.reason = EarlyRailPlanReason::NoCurrentRoute;
		failed.rejected_platforms = proven_rejections;
		return failed;
	};
	const auto query_route = [&](const Order &order, VehicleOrderID index, TileIndex start, Trackdir direction, const YapfRailRouteFilter &admit) {
		YapfRailRoute route;
		if (remaining_nodes == 0) { route.reason = YapfRailRouteReason::NodeBudget; return route; }
		route = query(order, index, start, direction, admit);
		remaining_nodes -= std::min(remaining_nodes, route.nodes);
		return route;
	};
	for (uint candidate = 0; candidate < MAX_PLATFORM_CANDIDATES; candidate++) {
		const YapfRailRouteFilter admit = [&](TileIndex tile, Trackdir direction) {
			return !excluded.contains(tile) && (!filter || filter(tile, direction));
		};
		result.current = query_route(v->current_order, v->cur_real_order_index, origin, td, admit);
		result.onward = {};
		result.selected_clear = FirstClear(result.current, clears);
		result.physical_exit_proven = false;
		if (!result.current.complete || result.current.path.empty()) {
			if (!deferred.empty()) return fallback();
			result.reason = result.current.reason == YapfRailRouteReason::NoPath ? EarlyRailPlanReason::NoCurrentRoute : DeferredQueryReason(result.current);
			return result;
		}
		for (const auto &step : result.current.path) {
			if (result.selected_clear && std::pair{step.tile, step.trackdir} == *result.selected_clear) break;
			if (IsRailStationTile(step.tile) && !(v->current_order.IsType(OT_GOTO_STATION) &&
					GetStationIndex(step.tile) == v->current_order.GetDestination().ToStationID()) &&
					v->current_order.ShouldStopAtStation(v, GetStationIndex(step.tile), false)) {
				result.selected_clear.reset(); result.reason = EarlyRailPlanReason::DeferredIntermediateStop; return result;
			}
		}
		/* A Clear reached before the active destination already fixes this
		 * area's exit; work after that boundary belongs to a later decision. */
		if (result.selected_clear) { result.reason = EarlyRailPlanReason::Complete; result.physical_exit_proven = true; return result; }
		const auto endpoint = result.current.path.back();
		if (!v->current_order.IsType(OT_GOTO_STATION) || !IsRailStationTile(endpoint.tile) ||
				GetStationIndex(endpoint.tile) != v->current_order.GetDestination().ToStationID()) {
			result.reason = EarlyRailPlanReason::DeferredNoClearance; return result;
		}
		if (!v->current_order.ShouldStopAtStation(v, GetStationIndex(endpoint.tile), false)) {
			result.reason = EarlyRailPlanReason::DeferredNoNextOrder; return result;
		}
		const auto exit_direction = TrackdirToExitdir(endpoint.trackdir);
		const uint length = Station::GetByTile(endpoint.tile)->GetPlatformLength(endpoint.tile, ReverseDiagDir(exit_direction));
		if (length == 0 || Station::GetByTile(endpoint.tile)->GetPlatformLength(endpoint.tile, exit_direction) != 1) {
			result.reason = EarlyRailPlanReason::DeferredNativeQuery; return result;
		}
		const TileIndexDiff delta = TileOffsByDiagDir(exit_direction);
		const TileIndex near_end = endpoint.tile - delta * (length - 1);
		if (v->gcache.cached_total_length > length * TILE_SIZE) {
			/* Native stopping may permit overhang/through loading. Do not alter
			 * that policy or claim a fully sheltered departure here. */
			result.reason = EarlyRailPlanReason::DeferredShortPlatform; return result;
		}
		const auto next = NextExplicitOrder(v);
		result.next_order_index = next.index;
		bool reject = false;
		if (next.order != nullptr) {
			result.onward = query_route(*next.order, next.index, endpoint.tile, endpoint.trackdir, {});
			if (result.onward.complete) {
				result.selected_clear = FirstClear(result.onward, clears);
				result.physical_exit_proven = true;
				result.reason = result.selected_clear ? EarlyRailPlanReason::Complete : EarlyRailPlanReason::DeferredNoClearance;
				if (result.selected_clear) { result.rejected_platforms = proven_rejections; return result; }
			} else {
				result.reason = result.onward.reason == YapfRailRouteReason::NoPath ? EarlyRailPlanReason::DeferredUnsupportedReverse : DeferredQueryReason(result.onward);
			}
			deferred.push_back(result);
			for (uint i = 0; i < length; i++) excluded.insert(near_end + delta * i);
			if (remaining_nodes == 0) return fallback();
			continue;
		} else {
			/* Unknown future work never implies an unusable current stop. Only
			 * a complete physical dead-end proof in both directions can reject. */
			const auto forward = PhysicalReach(v, endpoint.tile, endpoint.trackdir, clears, nullptr, remaining_nodes);
			result.physical_exit_proven = forward == Reach::Yes;
			if (forward == Reach::Yes) { result.reason = next.reason; return result; }
			if (forward == Reach::Unknown) { result.reason = EarlyRailPlanReason::DeferredNodeBudget; return result; }
			const auto reverse = PhysicalReach(v, near_end, ReverseTrackdir(endpoint.trackdir), clears, nullptr, remaining_nodes);
			reject = reverse == Reach::No;
			if (!reject) { result.reason = remaining_nodes == 0 ? EarlyRailPlanReason::DeferredNodeBudget : EarlyRailPlanReason::DeferredUnsupportedReverse; return result; }
		}
		if (reject) {
			proven_rejections.push_back({near_end, endpoint.tile, endpoint.trackdir, next.order != nullptr});
			result.rejected_platforms = proven_rejections;
			for (uint i = 0; i < length; i++) excluded.insert(near_end + delta * i);
		}
	}
	if (!deferred.empty()) return fallback();
	result.reason = EarlyRailPlanReason::CandidateLimit;
	return result;
}
