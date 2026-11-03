/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */
/** @file tracerestrict_overtaking.cpp Automatic registration and siding claims reuse rollback-capable slot transactions. */
#include "stdafx.h"
#include "tracerestrict_overtaking.h"
#include "tracerestrict_early_route.h"
#include "tracerestrict_backup.h"
#include "train.h"
#include "cargotype.h"
#include "rail_map.h"
#include "station_map.h"
#include "station_base.h"
#include "pbs.h"
#include "signal_func.h"
#include "pathfinder/follow_track.hpp"
#include "pathfinder/yapf/yapf.h"
#include "core/backup_type.hpp"
#include "timer/timer_game_tick.h"
#include "vehicle_func.h"
#include "date_func.h"
#include "debug.h"
#include <map>
#include <set>
#include <queue>
#include "safeguards.h"

namespace {
/** Match the native rail pathfinder's turn rule. The ordinary follower alone
 * permits 90 degree choices that YAPF may forbid on this saved game. */
struct AutoYieldTrack : CFollowTrackRail {
	using CFollowTrackRail::CFollowTrackRail;
	bool Follow(TileIndex tile, Trackdir td)
	{
		if (!CFollowTrackRail::Follow(tile, td)) return false;
		if (_settings_game.pf.forbid_90_deg && this->tiles_skipped == 0 &&
				Rail90DegTurnDisallowedTilesFromDiagDir(this->old_tile, this->new_tile, this->exitdir, true)) {
			this->new_td_bits &= ~TrackdirCrossesTrackdirs(this->old_td);
			if (this->new_td_bits == TRACKDIR_BIT_NONE) { this->err = ErrorCode::SharpTurn; return false; }
		}
		return true;
	}
};
const Train *GetTrain(VehicleID id)
{
	const Vehicle *vehicle = Vehicle::GetIfValid(id);
	return vehicle != nullptr && vehicle->type == VehicleType::Train ? Train::From(vehicle) : nullptr;
}
struct Binding {
	TraceRestrictRefId ref;
	AutoYieldRole role;
	Trackdir td;
	bool platform = false;
	uint16_t speed_cap = 0; ///< Approach only; zero follows the current rail type limit.
	AutoYieldRegistration registration = AutoYieldRegistration::Reservation;
};
struct Area {
	CompanyID owner;
	uint16_t id;
	uint16_t percent = 0; ///< Strictest configured approach threshold; areas without an approach remain disabled.
	std::vector<Binding> bindings;
	TraceRestrictSlot *detection = nullptr;
	TraceRestrictSlot *pending = nullptr; ///< Registered by reservation, not physically past a detector yet.
	std::map<TraceRestrictRefId, TraceRestrictSlot *> lanes;
	std::map<TraceRestrictRefId, TraceRestrictSlot *> targets; ///< Saved obligations belonging to each committed lane occupant.
	std::vector<SignalReference> signal_dependants;
	bool exit_only = false; ///< An exit marks and claims its whole siding when no entrance markers are configured.
	bool enabled = false;
	bool has_platform = false;
	uint clear_count = 0; ///< Separate exits remain associated through the same saved slot.
	uint main_count = 0; ///< Rebuilt from saved markers; never a local UI preference.
};
using AreaKey = std::pair<CompanyID, uint16_t>;
std::map<AreaKey, Area> areas;
/* A coarse lookup only narrows candidates. Physical route proofs below, never
 * coordinates, decide whether two trains conflict. Rebuilt with marker edits;
 * track edits need no invalidation because the lookup contains no track data. */
std::map<std::pair<uint, uint>, std::set<AreaKey>> coordination_index;
bool dirty = true;
bool multiple_outlets = false; ///< Rebuilt with the saved marker registry, never client-local layout history.
uint probe_depth = 0;
/* Keep each path restriction evaluation bounded. Larger areas degrade to
 * ordinary signal behaviour instead of making route search expensive. */
constexpr size_t MAX_PARTICIPANTS = 16;
constexpr size_t MAX_SIDINGS = 16;
constexpr size_t MAX_APPROACH_MARKERS = 16;
constexpr size_t MAX_MAIN_MARKERS = 16;
constexpr size_t MAX_ROUTE_NODES = 256;
/* A condition or marker may ask several related questions (free siding,
 * evacuation, and route length). Share a budget across those searches. */
constexpr size_t MAX_PROBE_NODES_PER_EVALUATION = 1024;
constexpr size_t COMPLEX_APPROACH_EXTRA_NODES = 512;
constexpr uint SAME_SPEED_PASSENGER_GAP = 16; ///< Maximum clear gap behind a freight train, in route tiles.
constexpr uint SHORT_STATION_STOP_TICKS = 256; ///< Never hold traffic indefinitely for station work.
size_t probe_budget_remaining = 0;
struct OutletProof {
	std::optional<TraceRestrictRefId> ref;
	uint distance = 0;
	bool complete = false;
};
struct OutletEntry { const Area *area; const Train *train; OutletProof proof; };
std::array<OutletEntry, MAX_PARTICIPANTS + 1> outlet_cache;
size_t outlet_count = 0;
struct PassengerPriorityEntry { const Train *train; bool loaded; };
std::array<PassengerPriorityEntry, MAX_PARTICIPANTS> passenger_priority_cache;
size_t passenger_priority_count = 0; // Only valid within one read-only evaluation.
struct MergeProofEntry { const Area *area; const Train *train; TileIndex start; Trackdir td; std::optional<uint> distance; std::optional<TraceRestrictRefId> clearance; };
std::array<MergeProofEntry, MAX_PARTICIPANTS + 1> merge_proof_cache;
size_t merge_proof_count = 0;
size_t merge_probe_remaining = 0; ///< Shared optional allowance, inside the existing total probe cap.
struct SplitRouteTile {
	TileIndex tile;
	TrackBits tracks;
	Trackdir td = INVALID_TRACKDIR;
	uint distance = 0; ///< Forward distance from the moving front, in route tiles.
	bool body = false; ///< Already occupied: a head beyond a junction is not tail clearance.
};
struct SplitRouteProof {
	const Area *area = nullptr;
	const Train *train = nullptr;
	std::optional<TraceRestrictRefId> clearance;
	std::vector<SplitRouteTile> tiles;
	bool complete = false; ///< Reached this area's boundary or the active mandatory target.
	bool native_selected = false; ///< A valid actual route, not the union of possible outlets.
	Trackdir clearance_td = INVALID_TRACKDIR; ///< Direction of the actual selected outlet, not a guessed map heading.
};
std::array<SplitRouteProof, MAX_PARTICIPANTS + 1> split_proof_cache;
size_t split_proof_count = 0;
size_t split_probe_remaining = 0; // Optional proof, within the original total budget.
struct MainTargetEntry { const Train *train; TileIndex tile; Trackdir td; bool unreachable; };
std::array<MainTargetEntry, (MAX_PARTICIPANTS + 1) * MAX_MAIN_MARKERS> main_target_cache;
size_t main_target_count = 0;

/** Unique-outlet forecasts add priority, never prove independent exits or grant
 * track access. Candidate admission only needs a positive legal continuation;
 * its non-unique result must never be reused as a unique-outlet forecast. */
struct GoalClearanceHint {
	const Area *area = nullptr;
	const Train *train = nullptr;
	TileIndex start = INVALID_TILE;
	Trackdir start_td = INVALID_TRACKDIR;
	bool station_reached = false;
	std::optional<TraceRestrictRefId> clearance;
	uint distance = 0;
	std::optional<bool> free_siding;
	bool unique_outlet = true;
	std::optional<TraceRestrictRefId> required_clearance;
};
std::array<GoalClearanceHint, MAX_PARTICIPANTS + MAX_SIDINGS + 1> goal_hint_cache;
size_t goal_hint_count = 0;
size_t goal_hint_remaining = 0;
constexpr uint MAX_DESTINATION_PROBE_NODES = 32;
uint destination_probe_remaining = 0; ///< Separate native segment-node cap, shared by the whole evaluation.
struct DestinationProbeEntry { const Train *train; TraceRestrictRefId outlet; StationID station; bool reachable; };
std::array<DestinationProbeEntry, MAX_SIDINGS> destination_probe_cache;
size_t destination_probe_count = 0;
void ResetPlatformInfoCache();


class AutoYieldProbeBudget {
public:
	AutoYieldProbeBudget() : previous(probe_budget_remaining)
	{
		if (probe_depth == 0) {
			probe_budget_remaining = MAX_PROBE_NODES_PER_EVALUATION;
			passenger_priority_count = 0;
			outlet_count = 0;
			merge_proof_count = 0;
			merge_probe_remaining = MAX_ROUTE_NODES;
			split_proof_count = 0;
			main_target_count = 0;
			/* A pair of real corridors must fit: one long approach must not
			 * consume the entire shared directional allowance before the other
			 * train's physical route can be compared. The total cap stays 1024. */
			split_probe_remaining = MAX_PROBE_NODES_PER_EVALUATION / 2;
			goal_hint_count = 0;
			goal_hint_remaining = MAX_ROUTE_NODES + MAX_ROUTE_NODES / 2;
			destination_probe_remaining = MAX_DESTINATION_PROBE_NODES * 4;
			destination_probe_count = 0;
			ResetPlatformInfoCache();
		}
	}
	/** Multiple Mains and outlets require extra candidate-route proofs. Keep
	 * the ordinary allowances and single-Main behaviour, but leave bounded
	 * room for parking after those optional classification queries. */
	void UseArea(const Area &area)
	{
		if (probe_depth == 0 && area.main_count > 1 && area.clear_count > 1 && !extended) {
			probe_budget_remaining += COMPLEX_APPROACH_EXTRA_NODES;
			extended = true;
		}
	}
	~AutoYieldProbeBudget() { probe_budget_remaining = previous; }
private:
	size_t previous;
	bool extended = false;
};

/** A soft preference must leave room for the authoritative exit/claim checks. */
class AutoYieldPreferenceBudget {
public:
	explicit AutoYieldPreferenceBudget(size_t limit = MAX_ROUTE_NODES, size_t reserve = 0) : previous(probe_budget_remaining),
			allowance(std::min(limit, reserve != 0 && probe_budget_remaining > reserve ? probe_budget_remaining - reserve : probe_budget_remaining / 2))
	{
		probe_budget_remaining = allowance;
	}
	~AutoYieldPreferenceBudget() { probe_budget_remaining = previous - (allowance - probe_budget_remaining); }
private:
	size_t previous;
	size_t allowance;
};

bool Active(const Train *v, bool departing_depot = false)
{
	return v != nullptr && v->IsPrimaryVehicle() && !v->IsVirtualOrCarried() && (departing_depot || !v->IsInDepot()) && IsValidTile(v->tile) &&
			!v->vehstatus.Any({VehState::Stopped, VehState::Crashed}) && v->GetDisplayMaxSpeed() > 0;
}
const Area *FindArea(const Train *v, uint16_t id)
{
	if (v == nullptr || probe_depth != 0 || dirty) return nullptr;
	const TraceRestrictSlot *slot = TraceRestrictSlot::GetIfValid(id);
	if (slot == nullptr || slot->automatic_yield_area != 0 || slot->vehicle_type != VehicleType::Train) return nullptr;
	auto found = areas.find({CompanyID{slot->owner}, id});
	if (found == areas.end() || !found->second.enabled || found->second.detection == nullptr || found->second.detection->occupants.size() > MAX_PARTICIPANTS) return nullptr;
	return &found->second;
}
/** Same effective cap for every member prevents contradictory priorities after
 * entering through different detectors. Rail limits are read live (rail conversion
 * must not leave a stale cached cap); rail type zero means unrestricted. */
uint DetectionSpeed(const Area &area, const Train *v)
{
	uint speed = v->GetDisplayMaxSpeed();
	for (const Binding &binding : area.bindings) {
		if (binding.role != AutoYieldRole::Approach) continue;
		const TileIndex tile = GetTraceRestrictRefIdTileIndex(binding.ref);
		const uint cap = binding.speed_cap != 0 ? binding.speed_cap : GetRailTypeInfo(GetRailTypeByTrack(tile, GetTraceRestrictRefIdTrack(binding.ref)))->max_speed;
		if (cap != 0) speed = std::min(speed, cap);
	}
	return speed;
}
/** Actual passengers, including NewGRF passenger classes and articulated parts.
 * Cache only for this evaluation: loading, unloading and refits are visible next
 * time without persisted fields or client-dependent invalidation. */
bool HasPassengerPriority(const Train *v)
{
	for (size_t i = 0; i < passenger_priority_count; i++) if (passenger_priority_cache[i].train == v) return passenger_priority_cache[i].loaded;
	bool loaded = false;
	for (const Train *part = v; part != nullptr; part = part->Next()) {
		if (part->cargo_type < NUM_CARGO && IsCargoInClass(part->cargo_type, CargoClass::Passengers) &&
				(part->cargo.StoredCount() != 0 || (v->current_order.IsAnyLoadingType() && part->cargo_cap != 0))) { loaded = true; break; }
	}
	if (passenger_priority_count < passenger_priority_cache.size()) passenger_priority_cache[passenger_priority_count++] = {v, loaded};
	return loaded;
}
bool HigherPriority(const Area &area, const Train *a, const Train *b, uint percent)
{
	const uint64_t as = DetectionSpeed(area, a), bs = DetectionSpeed(area, b);
	return as == bs ? HasPassengerPriority(a) && !HasPassengerPriority(b) : as > bs && as * 100 >= bs * (100 + percent);
}
const Binding *FindBinding(const Area &area, TraceRestrictRefId ref)
{
	for (const Binding &binding : area.bindings) if (binding.ref == ref) return &binding;
	return nullptr;
}
bool AtRole(const Area &area, TileIndex tile, Trackdir td, AutoYieldRole role)
{
	for (const Binding &binding : area.bindings) if (binding.role == role && binding.ref == MakeTraceRestrictRefId(tile, TrackdirToTrack(td)) && binding.td == td) return true;
	return false;
}

/** Read-only signal restrictions also apply when proving a parked train can
 * eventually leave, independently of temporary occupancy/reservations. */
bool SignalAllowsRoute(const Train *v, TileIndex tile, Trackdir td, bool topology_only = false)
{
	const bool signal_tile = IsPlainRailTile(tile) ||
			(IsTileType(tile, TileType::TunnelBridge) && IsTunnelBridgeWithSignalSimulation(tile));
	if (const TraceRestrictProgram *program = signal_tile ? GetExistingTraceRestrictProgram(tile, TrackdirToTrack(td)) : nullptr; program != nullptr) {
		TraceRestrictProgramInput input(tile, td, nullptr, nullptr);
		TraceRestrictProgramResult out;
		program->Execute(v, input, out);
		if ((out.flags & TRPRF_DENY) || (!topology_only && (out.flags & TRPRF_WAIT_AT_PBS))) return false;
	}
	return true;
}

/** Check every physical tile, including station tiles skipped by the follower. */
bool FreeTrackTile(const Train *v, TileIndex tile, Trackdir td, const Train *departing = nullptr, const Train *yielding_reservation = nullptr)
{
	const Track track = TrackdirToTrack(td);
	bool blocked = false;
	for (const Train *part : VehiclesOnTile<VehicleType::Train>(tile)) {
		if (part->Primary() != v->Primary() && (departing == nullptr || part->Primary() != departing->Primary()) && ((part->track & TrackToTrackBits(track)) != TRACK_BIT_NONE || TracksOverlap(part->track | TrackToTrackBits(track)))) {
			blocked = true;
			break;
		}
	}
	if (blocked) return false;
	TrackBits reserved = GetReservedTrackbits(tile);
	if ((reserved & TrackToTrackBits(track)) == TRACK_BIT_NONE && TracksOverlap(reserved | TrackToTrackBits(track))) {
		/* A tentative main-line reservation can cross this alternative at a
		 * junction. As with reservations on this exact track, distinguish our
		 * own route (or the hypothetical departure) from another train's route.
		 * This filters only the read-only proof; PBS still owns every real bit. */
		for (TrackBits bits = reserved; bits != TRACK_BIT_NONE; bits = KillFirstBit(bits)) {
			const Track reserved_track = FindFirstTrack(bits);
			const Train *owner = GetTrainForReservation(tile, reserved_track);
			if (owner != nullptr && (owner->Primary() == v->Primary() || (departing != nullptr && owner->Primary() == departing->Primary()) ||
					(yielding_reservation != nullptr && owner->Primary() == yielding_reservation->Primary()))) reserved &= ~TrackToTrackBits(reserved_track);
		}
		if (TracksOverlap(reserved | TrackToTrackBits(track))) return false;
	}
	if ((reserved & TrackToTrackBits(track)) != TRACK_BIT_NONE) {
		const Train *owner = GetTrainForReservation(tile, track);
		if (owner == nullptr || (owner->Primary() != v->Primary() && (departing == nullptr || owner->Primary() != departing->Primary()) &&
				(yielding_reservation == nullptr || owner->Primary() != yielding_reservation->Primary()))) return false;
	}
	return SignalAllowsRoute(v, tile, td);
}

bool IsSplitPredictionTarget(const Train *v, TileIndex tile);

/** Bounded, read-only path proof. Ordinary restrictions and PBS ownership still apply.
 * Unknown/budget exhausted means no proof, never permission to force a train.
 * departing is used only for a hypothetical evacuation, never for admission. */
std::optional<uint> RouteToRole(const Area &area, const Train *v, TileIndex start, Trackdir start_td, AutoYieldRole target, bool free_path, std::optional<TraceRestrictRefId> exact_ref = std::nullopt, bool *search_incomplete = nullptr, const Train *departing = nullptr, std::optional<TraceRestrictRefId> *matched_ref = nullptr, std::optional<TraceRestrictRefId> excluded_ref = std::nullopt, bool require_clearance = false, bool signals_only = false, const Train *yielding_reservation = nullptr, bool before_main = false, std::optional<std::pair<uint, TraceRestrictRefId>> *nearest_main = nullptr)
{
	if (search_incomplete != nullptr) *search_incomplete = false;
	if (matched_ref != nullptr) matched_ref->reset();
	if (!IsValidTile(start) || !IsValidTrackdir(start_td) || !v->railtypes.Any()) return std::nullopt;
	AutoRestoreBackup guard(probe_depth, probe_depth + 1);
	struct Node { TileIndex tile; Trackdir td; uint distance; };
	std::queue<Node> queue;
	std::set<std::pair<TileIndex, Trackdir>> seen;
	queue.push({start, start_td, 0});
	/* A long approach may need more than 256 visited states even though the
	 * shared evaluation still has room. Keep the same total work bound. */
	while (!queue.empty() && seen.size() < MAX_PROBE_NODES_PER_EVALUATION && probe_budget_remaining > 0) {
		const Node current = queue.front(); queue.pop();
		if (!seen.insert({current.tile, current.td}).second) continue;
		probe_budget_remaining--;
		if (before_main && (AtRole(area,current.tile,current.td,AutoYieldRole::Main) || AtRole(area,current.tile,current.td,AutoYieldRole::Clear))) continue;
		/* A parallel escape must not lead straight back through the Main
		 * being avoided. Serial controls are checkpoints, not extra lanes. */
		if (target == AutoYieldRole::Clear && excluded_ref &&
				MakeTraceRestrictRefId(current.tile, TrackdirToTrack(current.td)) == *excluded_ref) continue;
		/* The geometric follower does not enforce signal direction. Match the
		 * existing train rules without changing the pathfinder itself. */
		if (HasOnewaySignalBlockingTrackdir(current.tile, current.td)) continue;
		if (free_path) {
			if (!FreeTrackTile(v, current.tile, current.td, departing, yielding_reservation)) continue;
		} else if (signals_only && !SignalAllowsRoute(v, current.tile, current.td, true)) {
			continue;
		}
		if (AtRole(area, current.tile, current.td, target)) {
			const auto ref = MakeTraceRestrictRefId(current.tile, TrackdirToTrack(current.td));
			if (nearest_main != nullptr && target == AutoYieldRole::Main && !*nearest_main) *nearest_main = std::pair{current.distance, ref};
			if ((!exact_ref || *exact_ref == ref) && (!excluded_ref || *excluded_ref != ref)) {
				/* Try the other branches if this parallel Main has no proven
				 * free departure; a third usable main must not be overlooked. */
				if (require_clearance && !RouteToRole(area, v, current.tile, current.td, AutoYieldRole::Clear, true,
						std::nullopt, nullptr, departing, nullptr, excluded_ref, false, false, yielding_reservation)) continue;
				if (matched_ref != nullptr) *matched_ref = ref;
				return current.distance;
			}
			/* A second Main on the same track is not a parallel escape.
			 * Likewise the first Clear ends this area: an exact-outlet proof
			 * cannot pass another clearance and re-enter downstream. */
			if ((target == AutoYieldRole::Main && !exact_ref) || target == AutoYieldRole::Clear) continue;
		}
		/* An area's Clear is also the end of its admission search. Walking
		 * beyond it cannot find parking for this encounter: tail passage there
		 * releases this area's membership. In particular an unreachable first
		 * siding must not consume the whole budget on the downstream network
		 * before another, locally reachable siding gets its own check. */
		if (target != AutoYieldRole::Clear && AtRole(area, current.tile, current.td, AutoYieldRole::Clear)) continue;
		/* Entering a depot ends this forward route. Its automatic reversal is
		 * a later movement, not an available siding ahead of the current train. */
		if (IsRailDepotTile(current.tile)) continue;
		/* A following relationship cannot be forecast through an unfinished
		 * mandatory target: the next order may reverse or take another exit. */
		if (target == AutoYieldRole::Main && IsSplitPredictionTarget(v, current.tile)) continue;
		{
			AutoYieldTrack follower(v);
			if (!follower.Follow(current.tile, current.td)) {
				Debug(yapf, 7, "AutoYield area={} train={} route-end={} direction={} reason={}", area.id, v->index, current.tile, uint(current.td), uint(follower.err));
				continue;
			}
			if (target == AutoYieldRole::Main && follower.is_station && follower.tiles_skipped > 0) {
				bool target_before_main = false;
				const TileIndexDiff step = TileOffsByDiagDir(follower.exitdir);
				for (int i = follower.tiles_skipped; i > 0; i--) {
					if (probe_budget_remaining == 0) { target_before_main = true; break; }
					probe_budget_remaining--;
					if (IsSplitPredictionTarget(v, follower.new_tile - step * i)) { target_before_main = true; break; }
				}
				if (target_before_main) continue;
			}
			/* A platform needs per-tile checks. An ordinary tunnel/bridge can
			 * be proved through its two PBS ends and the existing vehicle query;
			 * do not inspect surface tiles underneath it as railway tiles. Signal
			 * simulation is accepted only when the entire span is empty and both
			 * ends unreserved; partial block availability is never guessed. */
			if (free_path && (follower.tiles_skipped != 0 || follower.is_tunnel || follower.is_bridge)) {
				if (size_t(follower.tiles_skipped) > probe_budget_remaining) continue;
				if (follower.is_station) {
					bool clear = true;
					const TileIndexDiff step = TileOffsByDiagDir(follower.exitdir);
					for (int i = follower.tiles_skipped; i > 0; --i) {
						probe_budget_remaining--;
						if (!FreeTrackTile(v, follower.new_tile - step * i, FindFirstTrackdir(follower.new_td_bits), departing, yielding_reservation)) { clear = false; break; }
					}
					if (!clear) continue;
				} else {
					if (!follower.is_tunnel && !follower.is_bridge) continue;
					const bool simulated = IsTunnelBridgeWithSignalSimulation(current.tile) || IsTunnelBridgeWithSignalSimulation(follower.new_tile);
					if (simulated && (HasAcrossTunnelBridgeReservation(current.tile) || HasAcrossTunnelBridgeReservation(follower.new_tile))) continue;
					if (TunnelBridgeIsFree(current.tile, follower.new_tile, simulated ? nullptr : v, TBIFM_ACROSS_ONLY).Failed()) continue;
					probe_budget_remaining -= follower.tiles_skipped;
				}
			}
			for (TrackdirBits bits = follower.new_td_bits; bits != TRACKDIR_BIT_NONE; bits = KillFirstBit(bits)) {
				queue.push({follower.new_tile, FindFirstTrackdir(bits), current.distance + uint(1 + follower.tiles_skipped)});
			}
		}
	}
	if (search_incomplete != nullptr) *search_incomplete = !queue.empty();
	return std::nullopt;
}

/** Measure only the exclusive parking segment immediately before an exit.
 * Any overlapping track is its upstream boundary, regardless of the shape of
 * the junction/crossing. Exclude that tile rather than guessing its inlet
 * topology. Parallel, disjoint curves remain separate tracks. Length uses the
 * same full/half-tile units as trains; route admission remains a separate proof. */
std::optional<uint> SidingParkingLength(const Area &area, const Train *v, const Binding &exit, std::vector<StationID> *stations = nullptr,
		std::optional<TraceRestrictRefId> *entrance = nullptr)
{
	if (entrance != nullptr) entrance->reset();
	TileIndex tile = GetTraceRestrictRefIdTileIndex(exit.ref);
	Trackdir reverse_td = ReverseTrackdir(exit.td);
	uint length = 0;
	std::set<std::pair<TileIndex, Trackdir>> seen;
	while (seen.size() < MAX_ROUTE_NODES && probe_budget_remaining > 0) {
		if (!seen.emplace(tile, reverse_td).second) return std::nullopt;
		probe_budget_remaining--;
		const Trackdir forward_td = ReverseTrackdir(reverse_td);
		if (HasOnewaySignalBlockingTrackdir(tile, forward_td) || AtRole(area, tile, forward_td, AutoYieldRole::Main)) return std::nullopt;
		if (IsPlainRailTile(tile)) {
			if (TracksOverlap(GetTrackBits(tile))) return length;
		} else if (!HasStationTileRail(tile) && !IsRailTunnelBridgeTile(tile)) {
			return std::nullopt;
		}
		if (stations != nullptr && IsRailStationTile(tile) && !IsRailWaypoint(tile)) {
			const StationID station = GetStationIndex(tile);
			if (std::find(stations->begin(), stations->end(), station) == stations->end()) stations->push_back(station);
		}
		/* Do not count the exit signal tile itself. Keep an additional full
		 * tile of tail clearance in the common length comparison. */
		if (tile != GetTraceRestrictRefIdTileIndex(exit.ref)) length += IsDiagonalTrackdir(reverse_td) ? TILE_SIZE : TILE_SIZE / 2;
		if (AtRole(area, tile, forward_td, AutoYieldRole::Siding)) {
			if (entrance != nullptr) *entrance = MakeTraceRestrictRefId(tile, TrackdirToTrack(forward_td));
			return length;
		}
		AutoYieldTrack backward(v);
		if (!backward.Follow(tile, reverse_td) || (backward.tiles_skipped != 0 && !backward.is_station && !backward.is_tunnel && !backward.is_bridge) ||
				(!IsPlainRailTile(backward.new_tile) && !HasStationTileRail(backward.new_tile) && !IsRailTunnelBridgeTile(backward.new_tile)) || backward.new_td_bits == TRACKDIR_BIT_NONE) return std::nullopt;
		if (size_t(backward.tiles_skipped) > probe_budget_remaining) return std::nullopt;
		probe_budget_remaining -= backward.tiles_skipped;
		length += backward.tiles_skipped * TILE_SIZE;
		if (KillFirstBit(backward.new_td_bits) != TRACKDIR_BIT_NONE) {
			/* The complete boundary is excluded. A Main on one of the possible
			 * inlets must still not be reclassified as siding parking. */
			for (TrackdirBits bits = backward.new_td_bits; bits != TRACKDIR_BIT_NONE; bits = KillFirstBit(bits)) {
				if (AtRole(area, backward.new_tile, ReverseTrackdir(FindFirstTrackdir(bits)), AutoYieldRole::Main)) return std::nullopt;
			}
			return length;
		}
		tile = backward.new_tile;
		reverse_td = FindFirstTrackdir(backward.new_td_bits);
	}
	return std::nullopt;
}

/** Both legacy entrance markers and exit-only lanes use the same parking proof.
 * Legacy entry-to-exit access must additionally be free; hypothetical turnover
 * may ignore only the particular departing train, as before. */
std::optional<uint> LaneParkingLength(const Area &area, const Train *v, const Binding &binding, const Train *departing = nullptr)
{
	if (binding.role == AutoYieldRole::Exit) return SidingParkingLength(area, v, binding);
	std::optional<TraceRestrictRefId> exit_ref;
	if (!RouteToRole(area, v, GetTraceRestrictRefIdTileIndex(binding.ref), binding.td, AutoYieldRole::Exit, true,
			std::nullopt, nullptr, departing, &exit_ref) || !exit_ref) return std::nullopt;
	const Binding *exit = FindBinding(area, *exit_ref);
	return exit != nullptr ? SidingParkingLength(area, v, *exit) : std::nullopt;
}

bool HasLaneClaim(const Area &area, const Train *v)
{
	for (const auto &[ref, lane] : area.lanes) if (lane->IsOccupant(v->index)) return true;
	return false;
}

struct PlatformInfo {
	bool marked = false;
	bool known = false;
	std::optional<TraceRestrictRefId> exit;
	std::vector<StationID> stations;
};
struct PlatformCacheEntry { const Area *area; const Train *train; TraceRestrictRefId ref; PlatformInfo info; };
std::array<PlatformCacheEntry, (MAX_PARTICIPANTS + 1) * MAX_SIDINGS> platform_cache;
size_t platform_cache_count = 0;
void ResetPlatformInfoCache() { platform_cache_count = 0; }

/** Resolve either the exit-only layout or an optional legacy entrance marker. */
PlatformInfo ComputePlatformInfo(const Area &area, const Train *v, TraceRestrictRefId ref)
{
	PlatformInfo result;
	if (!area.has_platform) return result;
	const Binding *binding = FindBinding(area, ref);
	if (binding == nullptr) return result;
	const Binding *exit = nullptr;
	if (binding->role == AutoYieldRole::Exit) {
		result.marked = binding->platform;
		if (result.marked) exit = binding;
	} else if (binding->role == AutoYieldRole::Siding) {
		result.marked = binding->platform;
		for (const Binding &candidate : area.bindings) {
			if (candidate.role != AutoYieldRole::Exit || (!binding->platform && !candidate.platform)) continue;
			if (!RouteToRole(area, v, GetTraceRestrictRefIdTileIndex(ref), binding->td, AutoYieldRole::Exit, false, candidate.ref)) continue;
			result.marked = true;
			exit = &candidate;
			break;
		}
	}
	if (exit != nullptr) {
		result.exit = exit->ref;
		result.known = SidingParkingLength(area, v, *exit, &result.stations).has_value();
	}
	return result;
}

PlatformInfo GetPlatformInfo(const Area &area, const Train *v, TraceRestrictRefId ref)
{
	for (size_t i = 0; i < platform_cache_count; i++) {
		const auto &entry = platform_cache[i];
		if (entry.area != &area || entry.ref != ref) continue;
		if (entry.train == v) return entry.info;
		/* Reuse only complete geometry in mixed stations. Native ownership
		 * and rail compatibility are its train-dependent inputs; orders,
		 * occupancy and PBS admission are evaluated separately. */
		if (area.lanes.size() > 1 && entry.info.known && entry.train->owner == v->owner &&
				entry.train->railtypes == v->railtypes && entry.train->compatible_railtypes == v->compatible_railtypes &&
				std::any_of(area.bindings.begin(), area.bindings.end(), [](const Binding &b) {
					return b.role == AutoYieldRole::Exit && !b.platform;
				})) return entry.info;
	}
	auto info = ComputePlatformInfo(area, v, ref);
	if (platform_cache_count < platform_cache.size()) platform_cache[platform_cache_count++] = {&area, v, ref, info};
	return info;
}

/** Only a positively identified, scheduled stop can request a platform turn. */
bool ScheduledPlatformStop(const Train *v, const PlatformInfo &platform)
{
	if (!platform.marked || !platform.known || !v->current_order.IsType(OT_GOTO_STATION)) return false;
	return std::any_of(platform.stations.begin(), platform.stations.end(), [v](StationID station) {
		return v->current_order.GetDestination().ToStationID() == station && v->current_order.ShouldStopAtStation(v, station, false);
	});
}

/** Preserve mandatory station, waypoint and PXP coupling destinations. */
bool RequiresPlatform(const Train *v, const PlatformInfo &platform)
{
	if (!platform.known) return true; // An unknown layout cannot justify rejecting a route.
	if (v->current_order.IsAnyLoadingType() || v->current_order.IsType(OT_GOTO_WAYPOINT) ||
			v->current_order.IsType(OT_GOTO_COUPLE) || v->current_order.IsType(OT_WAIT_COUPLE) ||
			v->current_order.IsType(OT_DECOUPLE)) return true;
	for (StationID station : platform.stations) if (v->current_order.ShouldStopAtStation(v, station, false)) return true;
	return false;
}

bool DoingStationWork(const Train *v)
{
	return v->current_order.IsAnyLoadingType() || v->current_order.IsType(OT_WAIT_COUPLE) ||
			v->current_order.IsType(OT_DECOUPLE) || (v->current_order.IsType(OT_WAITING) && v->cur_speed == 0);
}

/** Station work must not become an in-transit priority or a forced evacuation. */
bool ServingPlatform(const Area &area, const Train *v)
{
	if (!area.has_platform || v == nullptr || !DoingStationWork(v)) return false;
	StationID station = v->last_station_visited;
	if (v->current_order.IsType(OT_LOADING_ADVANCE)) station = v->current_order.GetDestination().ToStationID();
	if (v->current_order.IsType(OT_WAIT_COUPLE) && v->current_order.HasWaitCoupleStation()) station = v->current_order.GetWaitCoupleStation();
	if (station == StationID::Invalid()) return false;
	for (const auto &[ref, lane] : area.lanes) {
		const PlatformInfo platform = GetPlatformInfo(area, v, ref);
		if (platform.marked && platform.known && std::find(platform.stations.begin(), platform.stations.end(), station) != platform.stations.end()) {
			if (lane->IsOccupant(v->index)) return true;
			/* A station can span marked and unmarked tracks. Without a lane
			 * claim, prove that an actual station part can reach this exit. This
			 * also covers long coupled trains with the front outside the platform. */
			for (const Train *part = v; part != nullptr; part = part->Next()) {
				if (probe_budget_remaining == 0) break;
				probe_budget_remaining--;
				if (!IsValidTile(part->tile) || !IsRailStationTile(part->tile) || GetStationIndex(part->tile) != station) continue;
				if (RouteToRole(area, v, part->tile, part->GetVehicleTrackdir(), AutoYieldRole::Exit, false, platform.exit)) return true;
				if (probe_budget_remaining == 0) break;
			}
		}
		if (probe_budget_remaining == 0) break;
	}
	return false;
}

bool IsPredictionOrderTarget(const Order &order, TileIndex tile)
{
	if (order.IsType(OT_GOTO_STATION)) return IsRailStationTile(tile) && GetStationIndex(tile) == order.GetDestination();
	if (order.IsType(OT_GOTO_WAYPOINT)) return IsRailWaypointTile(tile) && GetStationIndex(tile) == order.GetDestination();
	return order.IsType(OT_GOTO_DEPOT) && !(order.GetDepotActionType() & ODATFB_NEAREST_DEPOT) &&
			IsRailDepotTile(tile) && GetDepotIndex(tile) == order.GetDestination();
}

/** Admission still cannot skip the active mandatory stop. */
bool IsSplitPredictionTarget(const Train *v, TileIndex tile)
{
	return IsPredictionOrderTarget(v->current_order, tile);
}

struct PredictionOrders {
	const Order *first;
	const Order *next = nullptr;
	bool first_reached = false;
	bool continuation_unknown = false;
};

/** Read at most one physical continuation after a scheduled station. Never
 * execute conditionals, acquire slots, switch schedules or change a vehicle.
 * Ambiguous branches/coupling remain unknown rather than independent exits. */
PredictionOrders GetPredictionOrders(const Train *v)
{
	PredictionOrders result{&v->current_order};
	const Order *planned = v->GetOrder(v->cur_real_order_index);
	const bool at_stop = (v->current_order.IsAnyLoadingType() || v->current_order.IsType(OT_LEAVESTATION)) &&
			planned != nullptr && planned->IsType(OT_GOTO_STATION) && planned->GetDestination() == v->last_station_visited;
	if (at_stop) { result.first = planned; result.first_reached = true; }
	if (!result.first->IsType(OT_GOTO_STATION) || v->orders == nullptr) return result;
	result.continuation_unknown = true;
	if (planned == nullptr || !planned->IsType(OT_GOTO_STATION) || planned->GetDestination() != result.first->GetDestination()) return result;
	VehicleOrderID index = v->cur_real_order_index;
	std::set<VehicleOrderID> seen{index};
	for (uint count = 0; count < std::min<uint>(64, v->GetNumOrders()); count++) {
		/* Executed schedules return to a saved parent at their end; they do
		 * not loop to their first order. Never invent that departure here. */
		if (index + 1 >= v->GetNumOrders() && v->IsExecutingSchedule()) return result;
		index = index + 1 < v->GetNumOrders() ? index + 1 : 0;
		const Order *order = v->GetOrder(index);
		if (order == nullptr || !seen.insert(index).second) return result;
		while (order->IsType(OT_CONDITIONAL) && order->GetConditionVariable() == OrderConditionVariable::Unconditionally) {
			index = order->GetConditionSkipToOrder();
			if (!seen.insert(index).second || seen.size() > 64) return result;
			order = v->GetOrder(index);
			if (order == nullptr) return result;
		}
		if (order->IsType(OT_LABEL) || order->IsSlotCounterOrder()) continue;
		if (order->IsType(OT_GOTO_STATION) || order->IsType(OT_GOTO_WAYPOINT) ||
				(order->IsType(OT_GOTO_DEPOT) && !(order->GetDepotActionType() & ODATFB_NEAREST_DEPOT) &&
				 !order->GetDepotOrderType().Test(OrderDepotTypeFlag::Service))) {
			if (order->GetType() == result.first->GetType() && order->GetDestination() == result.first->GetDestination()) return result;
			result.next = order;
			result.continuation_unknown = false;
		}
		return result;
	}
	return result;
}

GoalClearanceHint *GetGoalClearanceHint(const Area &area, const Train *v, TileIndex start = INVALID_TILE, Trackdir start_td = INVALID_TRACKDIR,
		bool station_reached = false, bool unique_outlet = true, std::optional<TraceRestrictRefId> required_clearance = std::nullopt);

/** A bounded topology proof, not an alternative train pathfinder. A branch may
 * be excluded only after exhausting every forward continuation without reaching
 * the active target. Cycles, skipped structures and budget exhaustion are unknown.
 * Even a reachable branch is unusable if another branch remains unknown. */
Trackdir PredictSplitTargetBranch(const Train *v, TileIndex tile, TrackdirBits choices, const Order &target, bool *proven_unreachable = nullptr)
{
	if (proven_unreachable != nullptr) *proven_unreachable = false;
	if (!(target.IsType(OT_GOTO_STATION) || target.IsType(OT_GOTO_WAYPOINT) ||
			(target.IsType(OT_GOTO_DEPOT) && !(target.GetDepotActionType() & ODATFB_NEAREST_DEPOT)))) return INVALID_TRACKDIR;
	enum class Reach : uint8_t { Visiting, No, Yes, Unknown };
	std::map<std::pair<TileIndex, Trackdir>, Reach> memo;
	const size_t available = std::min(split_probe_remaining, probe_budget_remaining);
	/* Keep room to append the selected corridor, without halving a small
	 * caller allowance before excluding a depot turnaround branch. */
	size_t remaining = available > 16 ? std::min(size_t{96}, available - 16) : 0;
	const auto visit = [&](auto &&self, TileIndex here, Trackdir td) -> Reach {
		if (!IsValidTile(here) || !IsValidTrackdir(td)) return Reach::Unknown;
		const auto key = std::make_pair(here, td);
		if (auto it = memo.find(key); it != memo.end()) return it->second == Reach::Visiting ? Reach::Unknown : it->second;
		if (remaining == 0) return Reach::Unknown;
		remaining--; probe_budget_remaining--; split_probe_remaining--;
		memo.emplace(key, Reach::Visiting);
		const auto finish = [&](Reach result) { memo[key] = result; return result; };
		if (HasOnewaySignalBlockingTrackdir(here, td)) return finish(Reach::No);
		if (IsPredictionOrderTarget(target, here)) return finish(Reach::Yes);
		/* A non-target depot is not automatically a dead end: YAPF can reverse
		 * there. Exclude only a unique return blocked by a one-way signal.
		 * Open returns, forks, other targets/depots and exhaustion remain
		 * unknown; a turnaround never becomes a predicted forward corridor. */
		if (IsRailDepotTile(here)) {
			TileIndex back_tile = here;
			Trackdir back_td = ReverseTrackdir(td);
			std::set<std::pair<TileIndex, Trackdir>> back_seen;
			while (remaining > 0 && back_seen.emplace(back_tile, back_td).second) {
				remaining--; probe_budget_remaining--; split_probe_remaining--;
				if (HasOnewaySignalBlockingTrackdir(back_tile, back_td)) return finish(Reach::No);
				if (back_tile != here && (IsPredictionOrderTarget(target, back_tile) || IsRailDepotTile(back_tile))) break;
				AutoYieldTrack reverse(v);
				if (!reverse.Follow(back_tile, back_td) || reverse.tiles_skipped != 0 || reverse.is_tunnel || reverse.is_bridge ||
						reverse.new_td_bits == TRACKDIR_BIT_NONE || KillFirstBit(reverse.new_td_bits) != TRACKDIR_BIT_NONE || reverse.new_tile == back_tile) break;
				back_tile = reverse.new_tile;
				back_td = FindFirstTrackdir(reverse.new_td_bits);
			}
			return finish(Reach::Unknown);
		}
		AutoYieldTrack follow(v);
		if (!follow.Follow(here, td)) return finish(Reach::No);
		if (follow.is_station && follow.tiles_skipped > 0) {
			const TileIndexDiff step = TileOffsByDiagDir(follow.exitdir);
			for (int skipped = follow.tiles_skipped; skipped > 0; skipped--) {
				if (remaining == 0) return finish(Reach::Unknown);
				remaining--; probe_budget_remaining--; split_probe_remaining--;
				if (IsPredictionOrderTarget(target, follow.new_tile - step * skipped)) return finish(Reach::Yes);
			}
		}
		if (follow.is_tunnel || follow.is_bridge || (follow.tiles_skipped != 0 && !follow.is_station)) {
			return finish(IsPredictionOrderTarget(target, follow.new_tile) ? Reach::Yes : Reach::Unknown);
		}
		bool unknown = false;
		for (TrackdirBits bits = follow.new_td_bits; bits != TRACKDIR_BIT_NONE; bits = KillFirstBit(bits)) {
			const Reach result = self(self, follow.new_tile, FindFirstTrackdir(bits));
			if (result == Reach::Yes) return finish(Reach::Yes);
			unknown |= result != Reach::No;
		}
		return finish(unknown ? Reach::Unknown : Reach::No);
	};
	Trackdir selected = INVALID_TRACKDIR;
	for (TrackdirBits bits = choices; bits != TRACKDIR_BIT_NONE; bits = KillFirstBit(bits)) {
		const Trackdir td = FindFirstTrackdir(bits);
		const Reach result = visit(visit, tile, td);
		if (result == Reach::Unknown || result == Reach::Visiting) return INVALID_TRACKDIR;
		if (result == Reach::Yes) {
			if (IsValidTrackdir(selected)) return INVALID_TRACKDIR;
			selected = td;
		}
	}
	if (proven_unreachable != nullptr) *proven_unreachable = !IsValidTrackdir(selected);
	return selected;
}

/** Cache only within this read-only evaluation: orders, rail edits and PBS
 * changes are read afresh next time. Exhaustion cannot establish exclusion. */
bool MainTargetProvenUnreachable(const Train *v, TileIndex tile, Trackdir td)
{
	if (!v->current_order.IsType(OT_GOTO_STATION) || !Station::IsValidID(StationID{v->current_order.GetDestination().base()})) return false;
	for (size_t i = 0; i < main_target_count; i++) {
		const auto &entry = main_target_cache[i];
		if (entry.train == v && entry.tile == tile && entry.td == td) return entry.unreachable;
	}
	if (main_target_count == main_target_cache.size()) return false;
	/* Positive native reachability avoids spending a tile-by-tile exclusion
	 * search on a normal distant destination. A failed bounded query is not
	 * an exclusion proof; it falls through to the original exhaustive check. */
	if (destination_probe_remaining != 0) {
		uint nodes_used = 0;
		const uint allowance = std::min(MAX_DESTINATION_PROBE_NODES,destination_probe_remaining);
		bool reachable;
		{
			AutoRestoreBackup guard(probe_depth,probe_depth+1);
			reachable = YapfTrainCanReachStationBounded(v,tile,td,StationID{v->current_order.GetDestination().base()},allowance,nodes_used);
		}
		destination_probe_remaining -= std::min(allowance,nodes_used);
		if (reachable) {
			main_target_cache[main_target_count++] = {v,tile,td,false};
			return false;
		}
	}
	bool unreachable = false;
	PredictSplitTargetBranch(v, tile, TrackdirToTrackdirBits(td), v->current_order, &unreachable);
	main_target_cache[main_target_count++] = {v, tile, td, unreachable};
	return unreachable;
}

/** Prove a directed route to a specific exit, using only a unique topology
 * or this train's already owned reservation at each split. This is not a
 * destination guess or a pathfinder: only an exhaustively unique target branch
 * may resolve an unreserved fork; ambiguity remains unknown. */
const SplitRouteProof *GetSplitRouteProof(const Area &area, const Train *v, TileIndex start = INVALID_TILE, Trackdir start_td = INVALID_TRACKDIR)
{
	for (size_t i = 0; i < split_proof_count; i++) {
		if (split_proof_cache[i].area == &area && split_proof_cache[i].train == v) {
			if (start == INVALID_TILE) return &split_proof_cache[i];
			split_proof_cache[i] = split_proof_cache[--split_proof_count];
			break;
		}
	}
	if (split_proof_count == split_proof_cache.size()) return nullptr;
	auto &proof = split_proof_cache[split_proof_count++];
	proof.area = &area; proof.train = v; proof.clearance.reset(); proof.tiles.clear(); proof.complete = false; proof.native_selected = false; proof.clearance_td = INVALID_TRACKDIR;
	AutoYieldPreferenceBudget preference_budget;
	AutoRestoreBackup guard(probe_depth, probe_depth + 1);
	const Train *front = v->GetMovingFront();
	TileIndex tile = start == INVALID_TILE ? front->tile : start;
	Trackdir td = start == INVALID_TILE ? front->GetVehicleTrackdir() : start_td;
	YapfRailRoute selected;
	const bool actual_route = area.clear_count > 1 && AutoYieldGetNativeRoute(v, area.id, tile, td, selected);
	uint distance = 0;
	const auto append = [&](TileIndex tile, TrackBits tracks, Trackdir td, bool body = false) {
		if ((!actual_route && probe_budget_remaining == 0) || split_probe_remaining == 0 || proof.tiles.size() == MAX_ROUTE_NODES ||
				!IsValidTile(tile) || tracks == TRACK_BIT_NONE || (tracks & ~TRACK_BIT_ALL) != TRACK_BIT_NONE) return false;
		/* Reading an already selected corridor is not a topology search. Use
		 * the existing shared footprint cap even if optional search is spent;
		 * neither cap is raised, and unknown paths still require search budget. */
		if (!actual_route) probe_budget_remaining--;
		split_probe_remaining--;
		proof.tiles.push_back({tile, tracks, td, distance, body});
		return true;
	};
	/* Include every occupied body tile: an engine beyond the junction does
	 * not make its still-conflicting tail independent of the other exit. */
	for (const Train *part = v->First(); part != nullptr; part = part->Next()) {
		if (!append(part->tile, part->track, part->GetVehicleTrackdir(), true)) return &proof;
	}
	if (actual_route) {
		bool first_step = true;
		for (const auto &step : selected.path) {
			if (!first_step) distance += 1 + step.wormhole_tiles;
			first_step = false;
			if (!append(step.tile, TrackToTrackBits(TrackdirToTrack(step.trackdir)), step.trackdir)) return &proof;
		}
		proof.clearance = MakeTraceRestrictRefId(selected.path.back().tile, TrackdirToTrack(selected.path.back().trackdir));
		proof.complete = true;
		proof.native_selected = true;
		proof.clearance_td = selected.path.back().trackdir;
		std::sort(proof.tiles.begin(), proof.tiles.end(), [](const SplitRouteTile &a, const SplitRouteTile &b) { return a.tile < b.tile; });
		return &proof;
	}
	const auto prediction = GetPredictionOrders(v);
	/* Loading proves the station was reached, not which way a conditional
	 * departure will go. Retain body conflicts and existing obligations. */
	if (prediction.first_reached && prediction.continuation_unknown) return &proof;
	bool after_station = prediction.first_reached && prediction.next != nullptr;
	bool onward_proved = false;
	const Order *target = after_station ? prediction.next : prediction.first;
	const auto reached_target = [&](TileIndex here) {
		if (!IsPredictionOrderTarget(*target, here)) return false;
		if (target == prediction.first && prediction.next != nullptr) {
			target = prediction.next; after_station = true; return false;
		}
		return true;
	};
	bool reservation_connected = IsValidTrackdir(td) && (GetReservedTrackbits(tile) & TrackToTrackBits(TrackdirToTrack(td))) != TRACK_BIT_NONE;
	std::set<std::pair<TileIndex, Trackdir>> seen;
	while (IsValidTrackdir(td) && seen.emplace(tile, td).second) {
		if (HasOnewaySignalBlockingTrackdir(tile, td) || !SignalAllowsRoute(v, tile, td, true)) return &proof;
		if (!append(tile, TrackToTrackBits(TrackdirToTrack(td)), td)) return &proof;
		if (AtRole(area, tile, td, AutoYieldRole::Clear)) {
			/* Do not forecast a departure in the present direction if the next
			 * destination actually requires reversing or another outlet. */
			if (after_station && !onward_proved && PredictSplitTargetBranch(v, tile, TrackdirToTrackdirBits(td), *target) != td) return &proof;
			std::sort(proof.tiles.begin(), proof.tiles.end(), [](const SplitRouteTile &a, const SplitRouteTile &b) { return a.tile < b.tile; });
			proof.clearance = MakeTraceRestrictRefId(tile, TrackdirToTrack(td));
			proof.complete = true;
			return &proof;
		}
		if (reached_target(tile)) { proof.complete = !prediction.continuation_unknown; return &proof; }
		if (IsRailDepotTile(tile)) return &proof;
		AutoYieldTrack follow(v);
		/* Unexamined skipped station/tunnel tiles could hide a conflict. */
		if (!follow.Follow(tile, td)) {
			/* A confirmed end of the current legal forward corridor is distinct
			 * from an unresolved signal, fork, structure or exhausted search. */
			proof.complete = follow.err != AutoYieldTrack::ErrorCode::None;
			return &proof;
		}
		if (follow.is_tunnel || follow.is_bridge || follow.new_td_bits == TRACKDIR_BIT_NONE) return &proof;
		if (follow.is_station && follow.tiles_skipped > 0) {
			const uint origin_distance = distance;
			const TileIndexDiff step = TileOffsByDiagDir(follow.exitdir);
			for (int skipped = follow.tiles_skipped; skipped > 0; skipped--) {
				distance = origin_distance + uint(follow.tiles_skipped - skipped + 1);
				const TileIndex station_tile = follow.new_tile - step * skipped;
				if (!append(station_tile, TrackToTrackBits(TrackdirToTrack(td)), td)) return &proof;
				if (reached_target(station_tile)) { proof.complete = !prediction.continuation_unknown; return &proof; }
			}
			distance = origin_distance;
		} else if (follow.tiles_skipped != 0) return &proof;
		TrackdirBits choices = follow.new_td_bits;
		if (KillFirstBit(choices) != TRACKDIR_BIT_NONE) {
			/* Do not jump across an unreserved gap onto a reservation belonging
			 * to this train's rear on a loop. Only its continuous forward path
			 * can resolve the next fork. */
			TrackdirBits owned = TRACKDIR_BIT_NONE;
			for (TrackdirBits bits = reservation_connected ? choices : TRACKDIR_BIT_NONE; bits != TRACKDIR_BIT_NONE; bits = KillFirstBit(bits)) {
				if (probe_budget_remaining == 0 || split_probe_remaining == 0) return &proof;
				probe_budget_remaining--; split_probe_remaining--;
				const Trackdir next = FindFirstTrackdir(bits);
				const Track track = TrackdirToTrack(next);
				if (!(GetReservedTrackbits(follow.new_tile) & TrackToTrackBits(track))) continue;
				const Train *owner = GetTrainForReservation(follow.new_tile, track);
				if (owner != nullptr && owner->Primary() == v->Primary()) owned |= TrackdirToTrackdirBits(next);
			}
			if (owned != TRACKDIR_BIT_NONE) {
				if (KillFirstBit(owned) != TRACKDIR_BIT_NONE) return &proof;
				choices = owned;
			} else {
				const Trackdir predicted = PredictSplitTargetBranch(v, follow.new_tile, choices, *target);
				if (!IsValidTrackdir(predicted)) return &proof;
				onward_proved |= after_station;
				choices = TrackdirToTrackdirBits(predicted);
				reservation_connected = false;
			}
		}
		tile = follow.new_tile;
		td = FindFirstTrackdir(choices);
		distance += 1 + follow.tiles_skipped;
		reservation_connected &= (GetReservedTrackbits(tile) & TrackToTrackBits(TrackdirToTrack(td))) != TRACK_BIT_NONE;
	}
	return &proof;
}

/** A pair-specific physical resource, never a lock on the entire station.
 * Positive overlaps remain useful even if the remaining route is unknown.
 * Only complete, disjoint corridors can establish independence. */
struct CommonConflict {
	TileIndex tile = INVALID_TILE;
	TrackBits tracks = TRACK_BIT_NONE;
	uint left_distance = 0;
	uint right_distance = 0;
	uint left_release = 0;
	bool directed = false;
	bool right_occupied = false;
	bool opposing = false;
	bool same_directed_track = false; ///< Actual following rail, distinct from intersecting switch movements.
};

std::optional<CommonConflict> FindCommonConflict(const Area &area, const Train *left, const Train *right)
{
	const auto *a = GetSplitRouteProof(area, left), *b = GetSplitRouteProof(area, right);
	if (a == nullptr || b == nullptr) return std::nullopt;
	std::map<TileIndex, std::vector<const SplitRouteTile *>> index;
	for (const auto &node : b->tiles) index[node.tile].push_back(&node);
	std::optional<CommonConflict> result;
	bool same_directed_track = false;
	for (const auto &node : a->tiles) {
		auto found = index.find(node.tile);
		if (found == index.end()) continue;
		for (const auto *other : found->second) {
			if ((node.tracks & other->tracks) == TRACK_BIT_NONE && !TracksOverlap(node.tracks | other->tracks)) continue;
			same_directed_track |= IsValidTrackdir(node.td) && node.td == other->td;
			const bool directed = IsValidTrackdir(node.td) && IsValidTrackdir(other->td) &&
					(node.td == other->td || TrackdirToExitdir(node.td) == TrackdirToExitdir(other->td));
			if (!result || std::tie(node.distance, node.tile) < std::tie(result->left_distance, result->tile)) {
				const bool opposing = IsValidTrackdir(node.td) && IsValidTrackdir(other->td) && ReverseTrackdir(node.td) == other->td;
				const uint release = result ? std::max(result->left_release, node.distance) : node.distance;
				/* Choosing an earlier conflict must retain a tail still occupying
				 * any other shared resource in this pair of corridors. */
				const bool occupied = other->body || (result && result->right_occupied);
				result = CommonConflict{node.tile, node.tracks | other->tracks, node.distance, other->distance, release, directed, occupied, opposing};
			} else {
				result->left_release = std::max(result->left_release, node.distance);
				result->right_occupied |= other->body;
			}
		}
	}
	if (result) result->same_directed_track = same_directed_track;
	return result;
}

bool RegisteredForYield(const Train *v)
{
	std::vector<TraceRestrictSlotID> slots;
	TraceRestrictGetVehicleSlots(v->index, slots);
	for (auto id : slots) {
		const auto *slot = TraceRestrictSlot::GetIfValid(id);
		if (slot == nullptr || slot->automatic_yield_area != 0) continue;
		auto a = areas.find({CompanyID{slot->owner}, id.base()});
		if (a != areas.end() && a->second.enabled) return true;
	}
	return false;
}

/** Local slot members plus bounded neighbours. No global vehicle iteration,
 * no new membership in another company's/user's slot, and no area-wide lock. */
std::vector<VehicleID> Participants(const Area &area, const Train *focus = nullptr)
{
	std::vector<VehicleID> result(area.detection->occupants.begin(), area.detection->occupants.end());
	std::set<AreaKey> neighbours;
	const auto collect = [&](TileIndex tile) {
		auto found = coordination_index.find({TileX(tile) / 128, TileY(tile) / 128});
		if (found != coordination_index.end()) neighbours.insert(found->second.begin(), found->second.end());
	};
	if (focus != nullptr) collect(focus->GetMovingFront()->tile);
	else for (const auto &binding : area.bindings) if (binding.role == AutoYieldRole::Main || binding.role == AutoYieldRole::Exit) collect(GetTraceRestrictRefIdTileIndex(binding.ref));
	if (neighbours.size() > 8) {
		Debug(yapf, 7, "AutoYield area={} cross-area fallback=neighbour-budget", area.id);
		return result;
	}
	std::set<VehicleID> extra;
	for (const auto &key : neighbours) {
		const auto &other = areas.at(key);
		if (&other == &area || !other.enabled || other.detection->occupants.size() > MAX_PARTICIPANTS) continue;
		for (VehicleID id : other.detection->occupants) {
			if (std::find(result.begin(), result.end(), id) == result.end() && Active(GetTrain(id))) extra.insert(id);
		}
	}
	if (result.size() + extra.size() > MAX_PARTICIPANTS) {
		Debug(yapf, 7, "AutoYield area={} cross-area fallback=participant-budget", area.id);
		return result;
	}
	result.insert(result.end(), extra.begin(), extra.end());
	return result;
}

/** Only proven disjoint corridors ending at different directed Clear markers
 * suppress an interaction. Shared rails, crossing tracks, tails and unknown
 * choices retain the existing rules. All proof data lives in this evaluation. */
bool ProvenIndependentExits(const Area &area, const Train *a, const Train *b)
{
	if (area.clear_count < 2 || a == b) return false;
	const auto *first = GetSplitRouteProof(area, a);
	if (first == nullptr || !first->clearance) return false;
	const auto *second = GetSplitRouteProof(area, b);
	if (second == nullptr || !second->clearance || first->clearance == second->clearance) return false;
	/* Sorted, bounded corridors make each pair check linear, including all
	 * body parts sharing a tile; there is no quadratic scan on long trains. */
	size_t i = 0, j = 0;
	while (i < first->tiles.size() && j < second->tiles.size()) {
		const auto left_tile = first->tiles[i].tile, right_tile = second->tiles[j].tile;
		if (left_tile < right_tile) { i++; continue; }
		if (right_tile < left_tile) { j++; continue; }
		TrackBits left = TRACK_BIT_NONE, right = TRACK_BIT_NONE;
		do { left |= first->tiles[i++].tracks; } while (i < first->tiles.size() && first->tiles[i].tile == left_tile);
		do { right |= second->tiles[j++].tracks; } while (j < second->tiles.size() && second->tiles[j].tile == right_tile);
		if ((left & right) != TRACK_BIT_NONE || TracksOverlap(left | right)) return false;
	}
	return true;
}

struct MainApproach {
	const Train *train;
	uint distance;
	std::optional<TraceRestrictRefId> main;
	bool clearance_only = false; ///< Requires a positive merge proof; not an unknown follower.
	uint priority_gap = UINT_MAX; ///< Precomputed before sorting; probes never run in a comparator.
	bool candidate_detour = false; ///< This route borrows a different Main than the nearest reachable one.
	uint64_t catch_time = UINT64_MAX; ///< Integer relative-speed estimate, computed before sorting.
	bool target_before_main = false; ///< Main is a bypass unable to serve the active station; retain station-capacity demand.
};
/** Same-speed priority is local. Compare forward routes to the common clearance
 * marker, including the freight consist length, rather than map distance (which
 * could confuse opposite directions or unrelated tracks). An incomplete proof
 * cannot introduce a new same-speed wait. Actual PBS remains authoritative. */
bool ClosePassenger(const Area &area, const Train *passenger, const Train *freight)
{
	if (ProvenIndependentExits(area, passenger, freight)) return false;
	AutoYieldPreferenceBudget preference_budget;
	const Train *p = passenger->GetMovingFront(), *f = freight->GetMovingFront();
	std::optional<TraceRestrictRefId> clearance;
	const auto pd = RouteToRole(area, passenger, p->tile, p->GetVehicleTrackdir(), AutoYieldRole::Clear, false, std::nullopt, nullptr, nullptr, &clearance);
	const auto fd = clearance ? RouteToRole(area, freight, f->tile, f->GetVehicleTrackdir(), AutoYieldRole::Clear, false, clearance) : std::nullopt;
	if (!pd || !fd) return false;
	if (*pd >= *fd) return *pd - *fd <= SAME_SPEED_PASSENGER_GAP + CeilDiv(freight->gcache.cached_total_length, TILE_SIZE);
	/* Once it is alongside/ahead, protect its tail through the shared merge. */
	return *fd - *pd <= SAME_SPEED_PASSENGER_GAP + CeilDiv(passenger->gcache.cached_total_length, TILE_SIZE);
}

/** Loading is not loss of priority. Preserve a normal short stop, but release
 * indefinite full-load/cargo-specific waits and coupling work. All inputs are
 * saved simulation state; no wall clock, local player or persistent cache. */
bool StationPriorityPaused(const Area &area, const Train *v)
{
	if (!ServingPlatform(area, v)) return false;
	return !v->current_order.IsAnyLoadingType() || v->current_order.IsFullLoadOrder() ||
			v->current_order.GetLoadType() == OrderLoadType::CargoTypeLoad ||
			v->current_order.GetWaitTime() > SHORT_STATION_STOP_TICKS || v->current_order_time >= SHORT_STATION_STOP_TICKS;
}
std::vector<MainApproach> GetMainApproaches(const Area &area, const Train *focus = nullptr, std::optional<TraceRestrictRefId> focus_main = std::nullopt)
{
	if (area.main_count == 0) return {};
	std::vector<MainApproach> result;
	result.reserve(area.detection->occupants.size() + 1);
	/* Classifying distant members must leave room to prove admission to a
	 * free siding. Resolve the train choosing its branch first; otherwise
	 * earlier slot occupants can consume even its own route proof. Unknown
	 * followers cannot create a confirmed overtaking conflict. */
	std::optional<AutoYieldPreferenceBudget> classification_budget;
	if (focus != nullptr) {
		/* A single bottleneck with several ordinary parking alternatives can spend its
		 * approach allowance on the shared feeder before finding the express.
		 * Reuse part of the parking reserve for that proof, without raising the
		 * evaluation or classification caps. Keep ordinary one-siding and
		 * multi-outlet and scheduled-platform recognition budgets unchanged. */
		const size_t parking_reserve = !area.has_platform && area.main_count == 1 && area.clear_count == 1 && area.lanes.size() > 1 &&
				probe_budget_remaining > MAX_ROUTE_NODES ?
				MAX_ROUTE_NODES / 2 : MAX_ROUTE_NODES;
		classification_budget.emplace(MAX_PROBE_NODES_PER_EVALUATION / 2, parking_reserve);
	}
	const auto append = [&](const Train *train) {
		if (!Active(train)) return;
		const bool serving = ServingPlatform(area, train);
		bool local_stop = serving;
		if (!local_stop && HasLaneClaim(area, train) && train->current_order.IsType(OT_GOTO_STATION)) {
			for (const auto &[ref, lane] : area.lanes) if (lane->IsOccupant(train->index) && ScheduledPlatformStop(train, GetPlatformInfo(area, train, ref))) {
				local_stop = true;
				break;
			}
		}
		/* A scheduled platform stop is not a final exit. A proved onward
		 * destination can retain priority through a short stop, without making
		 * the loading train itself an evacuation/admission candidate. Apply
		 * this only to a stop in the claimed platform, not a distant timetable
		 * destination: that would hide a just-committed diversion's own Main
		 * before its confirmed follower can be saved. */
		if (HasLaneClaim(area, train) && local_stop && GetPredictionOrders(train).next != nullptr) {
			if (!StationPriorityPaused(area, train)) {
				auto *goal = GetGoalClearanceHint(area, train);
				if (goal != nullptr && goal->clearance) result.push_back({train, UINT_MAX, std::nullopt, true});
			}
			return;
		}
		if (serving) return;
		/* A neighbouring detector does not own our Main markers or siding
		 * capacity. Its physical corridor is compared only when it has higher
		 * priority, instead of repeatedly searching for an unrelated Main. */
		if (!area.detection->IsOccupant(train->index)) {
			result.push_back({train, UINT_MAX, std::nullopt, true});
			return;
		}
		bool incomplete = false;
		std::optional<TraceRestrictRefId> main;
		std::optional<std::pair<uint, TraceRestrictRefId>> nearest;
		const auto requested = area.main_count > 1 && train == focus ? focus_main : std::nullopt;
		/* Find the nearest Main and this candidate in one BFS. Restarting at
		 * the head for the candidate repeats the entire approach and can leave
		 * too little budget to admit an otherwise safe, empty siding. */
		auto distance = RouteToRole(area, train, train->GetMovingFront()->tile, train->GetMovingFront()->GetVehicleTrackdir(),
				AutoYieldRole::Main, false, requested, &incomplete, nullptr, &main,
				std::nullopt, false, false, nullptr, false, requested ? &nearest : nullptr);
		bool detour = distance && nearest && main != nearest->second;
		/* An unresolved requested branch keeps the original nearest-Main
		 * classification. Budget exhaustion does not prove it unreachable. */
		if (!distance && nearest) { distance = nearest->first; main = nearest->second; incomplete = false; }
		Debug(yapf, 7, "AutoYield area={} approach={} distance={} incomplete={} budget={}", area.id, train->index, distance.value_or(UINT_MAX), incomplete, probe_budget_remaining);
		/* Keep unknown membership for later reevaluation, but do not promote
		 * a failed route proof into a confirmed follower. */
		if (distance || incomplete) {
			bool target_before_main = false;
			/* A mandatory station can sit on an unmarked branch too. Its
			 * reachability must not depend on the platform hint on a siding. */
			if (main && !HasLaneClaim(area,train)) {
				const Binding *binding = FindBinding(area,*main);
				if (binding != nullptr) target_before_main = MainTargetProvenUnreachable(train,GetTraceRestrictRefIdTileIndex(*main),binding->td);
			}
			result.push_back({train, distance.value_or(UINT_MAX), main, false, UINT_MAX, detour, UINT64_MAX, target_before_main});
		} else if ((area.main_count > 1 || !area.detection->IsOccupant(train->index)) && !HasLaneClaim(area, train)) result.push_back({train, UINT_MAX, std::nullopt, true});
	};
	if (focus != nullptr) append(focus);
	std::vector<const Train *> members;
	for (VehicleID id : Participants(area, focus)) {
		const Train *train = GetTrain(id);
		if (train != focus && Active(train)) members.push_back(train);
	}
	/* Resolve priority traffic before low-speed remote members can spend the
	 * bounded allowance. Equal speeds use saved IDs, never slot insertion order. */
	std::sort(members.begin(), members.end(), [&area](const Train *a, const Train *b) {
		return std::pair{UINT_MAX - DetectionSpeed(area, a), a->index} < std::pair{UINT_MAX - DetectionSpeed(area, b), b->index};
	});
	for (const Train *train : members) append(train);
	return result;
}
bool FreeLane(const Area &area, const Train *v, bool approaching = false, bool ordinary_only = false, std::optional<TraceRestrictRefId> exact_ref = std::nullopt, std::optional<TraceRestrictRefId> required_clearance = std::nullopt, bool require_free_departure = true);

/** Prove a unique forward continuation to the area's directed clearance point.
 * A branch, loop, opposing signal or exhausted budget cannot establish a shared
 * bottleneck. Results live only in this evaluation; rail edits and joining
 * clients read the same saved layout, without a persistent topology cache. */
std::optional<uint> MergeClearanceDistance(const Area &area, const MainApproach &approach, std::optional<TraceRestrictRefId> *matched_clearance = nullptr)
{
	if (!approach.main && !approach.clearance_only) return std::nullopt;
	const Binding *main = approach.main ? FindBinding(area, *approach.main) : nullptr;
	if (approach.main && main == nullptr) return std::nullopt;
	const Train *front = approach.train->GetMovingFront();
	const TileIndex start = main != nullptr ? GetTraceRestrictRefIdTileIndex(main->ref) : front->tile;
	const Trackdir start_td = main != nullptr ? main->td : front->GetVehicleTrackdir();
	const auto total = [&](std::optional<uint> distance) -> std::optional<uint> {
		if (!distance || (main != nullptr && approach.distance == UINT_MAX)) return std::nullopt;
		return *distance + (main != nullptr ? approach.distance : 0);
	};
	for (size_t i = 0; i < merge_proof_count; i++) {
		const auto &entry = merge_proof_cache[i];
		if (entry.area == &area && entry.train == approach.train && entry.start == start && entry.td == start_td) {
			if (matched_clearance != nullptr) *matched_clearance = entry.clearance;
			return total(entry.distance);
		}
	}
	std::optional<uint> distance;
	std::optional<TraceRestrictRefId> clearance;
	{
		AutoYieldPreferenceBudget preference_budget;
		AutoRestoreBackup guard(probe_depth, probe_depth + 1);
		TileIndex tile = start;
		Trackdir td = start_td;
		uint travelled = 0;
		std::set<std::pair<TileIndex, Trackdir>> seen;
		while (probe_budget_remaining > 0 && merge_probe_remaining > 0 && seen.size() < MAX_ROUTE_NODES) {
			if (!IsValidTile(tile) || !IsValidTrackdir(td) || !seen.emplace(tile, td).second) break;
			probe_budget_remaining--;
			merge_probe_remaining--;
			if (HasOnewaySignalBlockingTrackdir(tile, td)) break;
			const bool signal_tile = IsPlainRailTile(tile) ||
					(IsTileType(tile, TileType::TunnelBridge) && IsTunnelBridgeWithSignalSimulation(tile));
			if (const TraceRestrictProgram *program = signal_tile ? GetExistingTraceRestrictProgram(tile, TrackdirToTrack(td)) : nullptr; program != nullptr) {
				TraceRestrictProgramInput input(tile, td, nullptr, nullptr);
				TraceRestrictProgramResult out;
				program->Execute(approach.train, input, out);
				if (out.flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) break;
			}
			if (AtRole(area, tile, td, AutoYieldRole::Clear)) { distance = travelled; clearance = MakeTraceRestrictRefId(tile, TrackdirToTrack(td)); break; }
			/* A mandatory stop may reverse or choose another outlet. Its
			 * continuation needs the separate explicit-destination proof. */
			if (IsSplitPredictionTarget(approach.train, tile)) break;
			/* Continue through serial controls to the actual common resource. */
			if (IsRailDepotTile(tile)) break;
			AutoYieldTrack follow(approach.train);
			if (!follow.Follow(tile, td) || follow.new_td_bits == TRACKDIR_BIT_NONE || KillFirstBit(follow.new_td_bits) != TRACKDIR_BIT_NONE) break;
			bool target_skipped = false;
			if (follow.is_station) {
				const TileIndexDiff step = TileOffsByDiagDir(follow.exitdir);
				for (int skipped = follow.tiles_skipped; skipped > 0; skipped--) {
					if (probe_budget_remaining == 0 || merge_probe_remaining == 0) { target_skipped = true; break; }
					probe_budget_remaining--; merge_probe_remaining--;
					if (IsSplitPredictionTarget(approach.train, follow.new_tile - step * skipped)) { target_skipped = true; break; }
				}
			}
			if (target_skipped) break;
			tile = follow.new_tile;
			td = FindFirstTrackdir(follow.new_td_bits);
			travelled += 1 + follow.tiles_skipped;
		}
	}
	if (merge_proof_count < merge_proof_cache.size()) merge_proof_cache[merge_proof_count++] = {&area, approach.train, start, start_td, distance, clearance};
	if (matched_clearance != nullptr) *matched_clearance = clearance;
	return total(distance);
}

/** A bounded forward proof to an explicit destination. Priority requires all
 * destination routes to agree on the first outlet. Admission of a candidate
 * siding requires one legal destination route via an allowed first outlet;
 * unrelated branches cannot invalidate that positive proof. Neither mode
 * changes native routing, reservations or independent-route classification. */
GoalClearanceHint *GetGoalClearanceHint(const Area &area, const Train *v, TileIndex start, Trackdir start_td, bool station_reached,
		bool unique_outlet, std::optional<TraceRestrictRefId> required_clearance)
{
	const Train *front = v->GetMovingFront();
	if (start == INVALID_TILE) { start = front->tile; start_td = front->GetVehicleTrackdir(); }
	for (size_t i = 0; i < goal_hint_count; i++)
		if (goal_hint_cache[i].area == &area && goal_hint_cache[i].train == v &&
			goal_hint_cache[i].start == start && goal_hint_cache[i].start_td == start_td && goal_hint_cache[i].station_reached == station_reached &&
			goal_hint_cache[i].unique_outlet == unique_outlet && goal_hint_cache[i].required_clearance == required_clearance) return &goal_hint_cache[i];
	if (goal_hint_count == goal_hint_cache.size()) return nullptr;
	auto &hint = goal_hint_cache[goal_hint_count++];
	hint = {&area, v, start, start_td, station_reached, std::nullopt, 0, std::nullopt, unique_outlet, required_clearance};
	const auto prediction = GetPredictionOrders(v);
	if ((station_reached || prediction.first_reached) && prediction.next == nullptr) return &hint;
	if (!(prediction.first->IsType(OT_GOTO_STATION) || prediction.first->IsType(OT_GOTO_WAYPOINT) ||
		  (prediction.first->IsType(OT_GOTO_DEPOT) && !(prediction.first->GetDepotActionType() & ODATFB_NEAREST_DEPOT))))
		return &hint;
	AutoYieldPreferenceBudget allowance;
	AutoRestoreBackup guard(probe_depth, probe_depth + 1);
	struct Node {
		TileIndex tile;
		Trackdir td;
		uint distance;
		std::optional<TraceRestrictRefId> clearance;
		uint clearance_distance;
		bool after_station;
	};
	std::vector<Node> nodes{{start, start_td, 0, std::nullopt, 0, (prediction.first_reached || station_reached) && prediction.next != nullptr}};
	std::set<std::tuple<TileIndex, Trackdir, std::optional<TraceRestrictRefId>, bool>> seen;
	std::optional<TraceRestrictRefId> destination_clearance;
	uint clearance_distance = UINT_MAX;
	uint i = 0;
	for (; i < nodes.size() && seen.size() < MAX_ROUTE_NODES && probe_budget_remaining > 0 && goal_hint_remaining > 0; i++) {
		auto current = nodes[i];
		if (!IsValidTile(current.tile) || !IsValidTrackdir(current.td)) continue;
		if (!current.after_station && prediction.next != nullptr && IsPredictionOrderTarget(*prediction.first, current.tile)) current.after_station = true;
		const Order &target = current.after_station ? *prediction.next : *prediction.first;
		if (!current.clearance && AtRole(area, current.tile, current.td, AutoYieldRole::Clear)) {
			current.clearance = MakeTraceRestrictRefId(current.tile, TrackdirToTrack(current.td));
			current.clearance_distance = current.distance;
		}
		if (!unique_outlet && required_clearance && current.clearance && current.clearance != required_clearance) continue;
		if (!seen.emplace(current.tile, current.td, current.clearance, current.after_station).second) continue;
		probe_budget_remaining--; goal_hint_remaining--;
		if (HasOnewaySignalBlockingTrackdir(current.tile, current.td)) continue;
		const bool signal_tile = IsPlainRailTile(current.tile) ||
				(IsTileType(current.tile, TileType::TunnelBridge) && IsTunnelBridgeWithSignalSimulation(current.tile));
		if (const auto *program = signal_tile ? GetExistingTraceRestrictProgram(current.tile, TrackdirToTrack(current.td)) : nullptr; program != nullptr) {
			TraceRestrictProgramInput input(current.tile, current.td, nullptr, nullptr);
			TraceRestrictProgramResult out; program->Execute(v, input, out);
			if (out.flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) continue;
		}
		if (IsPredictionOrderTarget(target, current.tile)) {
			if (!unique_outlet) {
				if (!current.clearance) continue;
				hint.clearance = current.clearance;
				hint.distance = current.clearance_distance;
				return &hint;
			}
			/* Every forward route reaching the target must agree on its first
			 * outlet. A shortest route alone must not impose a wait on a train
			 * which could legitimately use another outlet to that same target. */
			if (!current.clearance || (destination_clearance && destination_clearance != current.clearance)) {
				Debug(yapf, 7, "AutoYield area={} train={} forecast=ambiguous-destination-outlet", area.id, v->index);
				return &hint;
			}
			destination_clearance = current.clearance;
			clearance_distance = std::min(clearance_distance, current.clearance_distance);
			continue;
		}
		if (IsRailDepotTile(current.tile)) continue;
		AutoYieldTrack follow(v);
		if (!follow.Follow(current.tile, current.td)) continue;
		/* A linear platform is not an unknown branch. The follower may jump
		 * from just before the target station to just beyond it, so test the
		 * skipped tiles before deciding the destination cannot be proved. */
		bool reached_station = false;
		if (follow.is_station && follow.tiles_skipped > 0) {
			const TileIndexDiff step = TileOffsByDiagDir(follow.exitdir);
			for (int skipped = follow.tiles_skipped; skipped > 0; skipped--) {
				if (probe_budget_remaining == 0 || goal_hint_remaining == 0) return &hint;
				probe_budget_remaining--; goal_hint_remaining--;
				const TileIndex station_tile = follow.new_tile - step * skipped;
				if (!IsPredictionOrderTarget(target, station_tile)) continue;
				nodes.push_back({station_tile, FindFirstTrackdir(follow.new_td_bits), current.distance + uint(follow.tiles_skipped - skipped + 1), current.clearance, current.clearance_distance, current.after_station});
				reached_station = true;
				break;
			}
		}
		if (reached_station) continue;
		/* Unexamined structures cannot prove a unique destination branch. */
		if ((follow.is_tunnel || follow.is_bridge || (follow.tiles_skipped && !follow.is_station)) && !IsPredictionOrderTarget(target, follow.new_tile)) {
			if (unique_outlet) return &hint;
			continue;
		}
		for (auto bits = follow.new_td_bits; bits != TRACKDIR_BIT_NONE; bits = KillFirstBit(bits)) {
			nodes.push_back({follow.new_tile, FindFirstTrackdir(bits), current.distance + uint(1 + follow.tiles_skipped), current.clearance, current.clearance_distance, current.after_station});
		}
	}
	if (i == nodes.size() && destination_clearance) {
		hint.clearance = destination_clearance;
		hint.distance = clearance_distance;
	}
	Debug(yapf, 7, "AutoYield area={} train={} forecast={} nodes={}/{} budget={}/{}", area.id, v->index,
			hint.clearance ? "proved" : "incomplete", i, nodes.size(), probe_budget_remaining, goal_hint_remaining);
	return &hint;
}

/** Keep the old unique-continuation result. Only when it is unavailable may
 * two explicit forward destination forecasts add a common-outlet preference.
 * They never remove a priority or classify unknown routes as independent. */
std::optional<std::pair<uint, uint>> FollowingClearanceDistances(const Area &area, const MainApproach &fast, const MainApproach &slow)
{
	std::optional<TraceRestrictRefId> fast_clearance, slow_clearance;
	const auto fd = MergeClearanceDistance(area, fast, &fast_clearance), sd = MergeClearanceDistance(area, slow, &slow_clearance);
	if (fd && sd) return fast_clearance == slow_clearance ? std::optional{std::pair{*fd, *sd}} : std::nullopt;
	if (DetectionSpeed(area, fast.train) == DetectionSpeed(area, slow.train)) return std::nullopt;
	/* Reuse a positive unique fast outlet instead of scanning its destination
	 * again. Spend the optional forecast budget on the ambiguous slow route. */
	uint fast_distance = fd.value_or(0);
	if (!fd) {
		auto *f = GetGoalClearanceHint(area, fast.train);
		if (f == nullptr || !f->clearance) return std::nullopt;
		fast_clearance = f->clearance;
		fast_distance = f->distance;
	}
	auto *s = GetGoalClearanceHint(area, slow.train);
	if (s == nullptr || !s->clearance || fast_clearance != s->clearance) return std::nullopt;
	/* A destination hint may only add a diversion to a proved free siding
	 * reaching that same first outlet. It cannot introduce a new full-lane
	 * eviction, force a blocked platform, or send the slow train out elsewhere.
	 */
	if (!s->free_siding) {
		AutoYieldPreferenceBudget allowance;
		s->free_siding = FreeLane(area, slow.train, false, false, std::nullopt, s->clearance, false);
	}
	if (!*s->free_siding) return std::nullopt;
	return std::pair{fast_distance, s->distance};
}
/** Different Main distances are comparable only after a positive common-exit proof. */
bool FollowingThreat(const Area &area, const MainApproach &fast, const MainApproach &slow, bool *merging = nullptr)
{
	if (fast.train == slow.train || !HigherPriority(area, fast.train, slow.train, area.percent) || ProvenIndependentExits(area, fast.train, slow.train)) return false;
	/* Geometric access to a bypass Main cannot forecast a following train
	 * which must first turn into another station branch. Preserve real shared
	 * inbound tracks through a positive physical-resource proof instead. The
	 * original Main remains available to station-capacity/deadlock planning. */
	if (fast.target_before_main || slow.target_before_main) {
		const auto resource = FindCommonConflict(area,slow.train,fast.train);
		if (!resource || !resource->directed || resource->right_occupied || resource->right_distance <= resource->left_distance) return false;
		if (DetectionSpeed(area,fast.train)==DetectionSpeed(area,slow.train) &&
				resource->right_distance-resource->left_distance>SAME_SPEED_PASSENGER_GAP+CeilDiv(slow.train->gcache.cached_total_length,TILE_SIZE)) return false;
		if (merging != nullptr) *merging = true;
		return true;
	}
	if (!area.detection->IsOccupant(fast.train->index)) {
		const auto resource = FindCommonConflict(area, slow.train, fast.train);
		if (!resource || !resource->directed || resource->right_occupied || resource->right_distance < resource->left_distance) return false;
		if (DetectionSpeed(area, fast.train) == DetectionSpeed(area, slow.train) &&
				resource->right_distance - resource->left_distance > SAME_SPEED_PASSENGER_GAP + CeilDiv(slow.train->gcache.cached_total_length, TILE_SIZE)) return false;
		if (merging != nullptr) *merging = true;
		return true;
	}
	const bool distinct = fast.clearance_only || slow.clearance_only || (fast.main && slow.main && fast.main != slow.main);
	if (distinct) {
		const auto distances = FollowingClearanceDistances(area, fast, slow);
		/* A common directed track can precede a later split into different
		 * outlets. Do not require the entire station to have one Clear. */
		const auto resource = !distances ? FindCommonConflict(area, slow.train, fast.train) : std::nullopt;
		if (!distances && (!resource || !resource->directed || resource->right_occupied)) return false;
		const auto [fd, sd] = distances.value_or(std::pair{resource ? resource->right_distance : 0, resource ? resource->left_distance : 0});
		if (fd < sd) return false;
		if (DetectionSpeed(area, fast.train) == DetectionSpeed(area, slow.train) &&
				fd - sd > SAME_SPEED_PASSENGER_GAP + CeilDiv(slow.train->gcache.cached_total_length, TILE_SIZE)) return false;
		if (merging != nullptr) *merging = true;
		return true;
	}
	return fast.main && fast.main == slow.main && fast.distance != UINT_MAX && slow.distance != UINT_MAX && fast.distance > slow.distance &&
			(DetectionSpeed(area, fast.train) != DetectionSpeed(area, slow.train) ||
					(fast.main && slow.main && fast.distance != UINT_MAX && slow.distance != UINT_MAX &&
					 fast.distance - slow.distance <= SAME_SPEED_PASSENGER_GAP + CeilDiv(slow.train->gcache.cached_total_length, TILE_SIZE)));
}

bool CrossingThreat(const Area &area, const Train *priority, const Train *yielding)
{
	if (!HigherPriority(area, priority, yielding, area.percent) || StationPriorityPaused(area, priority)) return false;
	const auto resource = FindCommonConflict(area, yielding, priority);
	if (!resource || resource->directed || resource->opposing) return false; // Opposing is not a following express.
	return resource->right_occupied || uint64_t(resource->right_distance) * DetectionSpeed(area, yielding) <=
			uint64_t(resource->left_release + CeilDiv(yielding->gcache.cached_total_length, TILE_SIZE) + 8) * DetectionSpeed(area, priority);
}

/** Precompute relative-speed catching risk; the sort itself never performs probes. */
void SetApproachGaps(const Area &area, std::vector<MainApproach> &candidates, const std::vector<MainApproach> &approaches)
{
	for (MainApproach &slow : candidates) {
		slow.priority_gap=UINT_MAX;
		slow.catch_time=UINT64_MAX;
		uint priority_design_speed = 0;
		for (const MainApproach &fast : approaches) {
			if (!FollowingThreat(area,fast,slow)) continue;
			uint gap = UINT_MAX;
			if (!fast.clearance_only && !slow.clearance_only && fast.main && fast.main==slow.main && fast.distance!=UINT_MAX && slow.distance!=UINT_MAX) {
				gap=fast.distance-slow.distance;
			} else {
				const auto distances=FollowingClearanceDistances(area,fast,slow);
				if (distances && distances->first>=distances->second) gap=distances->first-distances->second;
			}
			if (gap != UINT_MAX) {
				/* Compare arrivals against their highest-priority confirmed
				 * follower. An intermediate slow train is also faster than the
				 * front train; its tiny gap must not steal the slot from the train
				 * immediately ahead of the common fastest follower. */
				const uint design = fast.train->GetDisplayMaxSpeed();
				if (design < priority_design_speed) continue;
				if (design > priority_design_speed) {
					priority_design_speed = design;
					slow.priority_gap = UINT_MAX;
					slow.catch_time = UINT64_MAX;
				}
				slow.priority_gap = std::min(slow.priority_gap, gap);
				const uint relative = std::max(1u, DetectionSpeed(area, fast.train) - DetectionSpeed(area, slow.train));
				slow.catch_time = std::min(slow.catch_time, uint64_t(gap) * 65536 / relative);
			}
		}
	}
}

/** A proven free parallel Main avoids an unnecessary diversion. This is only
 * a read-only preference: normal pathfinding and PBS still choose/reserve it. */
bool FreeParallelMain(const Area &area, const MainApproach &approach)
{
	if (area.main_count <= 1 || !approach.main) return false;
	AutoYieldPreferenceBudget preference_budget;
	const Train *front = approach.train->GetMovingFront();
	return RouteToRole(area, approach.train, front->tile, front->GetVehicleTrackdir(), AutoYieldRole::Main, true,
			std::nullopt, nullptr, nullptr, nullptr, approach.main, true).has_value();
}

bool NeedsSiding(const Area &area, const Train *v, const std::vector<MainApproach> *known_approaches = nullptr, bool *parking_free = nullptr)
{
	if (parking_free != nullptr) *parking_free = false;
	if (!Active(v)) return false;
	MainApproach own{v, UINT_MAX, std::nullopt};
	std::vector<MainApproach> approaches = known_approaches != nullptr ? *known_approaches : GetMainApproaches(area, v);
	for (const MainApproach &approach : approaches) if (approach.train == v) own = approach;
	if (own.distance == UINT_MAX) {
		auto distance = RouteToRole(area, v, v->GetMovingFront()->tile, v->GetMovingFront()->GetVehicleTrackdir(), AutoYieldRole::Main, false, std::nullopt, nullptr, nullptr, &own.main);
		if (!distance) return false;
		own.distance = *distance;
	}
	const auto threatened = [&](const MainApproach &slow, bool *merging = nullptr) {
		bool found = false;
		for (const MainApproach &approach : approaches) {
			bool joined = false;
			if (!FollowingThreat(area, approach, slow, &joined) && !CrossingThreat(area, approach.train, slow.train)) continue;
			found = true;
			if (merging != nullptr && joined) *merging = true;
			/* One confirmed follower is sufficient unless another merging
			 * threat could invalidate a parallel-Main escape. Do not spend the
			 * parking budget on unrelated, unresolved neighbouring routes. */
			if (merging == nullptr || *merging || area.main_count <= 1) return true;
		}
		return found;
	};
	/* A candidate borrowing another approach must not justify itself with
	 * the free Main on its original line. Keep that alternative available to
	 * PBS, while rejecting this conflicting detour before the merge. */
	bool merging = own.candidate_detour;
	const bool parallel = !merging && FreeParallelMain(area, own);
	/* Only a positively available parallel Main needs all merging threats
	 * checked. With no such escape, one confirmed follower already decides
	 * admission; scanning unrelated later members can exhaust parking's
	 * budget after that positive decision. */
	if (!threatened(own, parallel ? &merging : nullptr) || (parallel && !merging)) return false;
	/* A proved free normal siding takes precedence over full-lane policy.
	 * Ranking an uncommitted follower cannot cancel this train's diversion:
	 * doing so lets it continue Main despite an empty, safe parking place.
	 * Native PBS and the lane transaction still arbitrate actual acquisition;
	 * nearest-arrival ranking belongs to full-lane turnover only. */
	if (HasLaneClaim(area, v)) {
		if (parking_free != nullptr) *parking_free = FreeLane(area, v, false, false, std::nullopt, std::nullopt, false);
		return true;
	}
	size_t available = 0;
	for (const auto &[ref, lane] : area.lanes) {
		/* An empty automatic claim is not an empty physical lane. Include
		 * non-members, cancelled membership, foreign trains and reservations
		 * in the same free-parking proof used for actual admission. */
		if (lane->occupants.empty() && FreeLane(area, v, parking_free == nullptr, false, ref, std::nullopt, false)) available++;
	}
	if (parking_free != nullptr) *parking_free = available != 0;
	return true;
}
enum class YieldConflict : uint8_t { Unknown, Independent, SharedDirectedResource, CrossingResource };



/** Exhaust all legal forward branches inside the local budget. Rejoining
 * alternatives are allowed; a second outlet, a cycle, a target reached before
 * clearance, or an unresolved structure cannot establish a shared resource.
 * This never chooses or reserves a route for the native pathfinder. */
OutletProof ComputeOutlet(const Area &area, const Train *v, TileIndex start = INVALID_TILE, Trackdir start_td = INVALID_TRACKDIR)
{
	AutoRestoreBackup guard(probe_depth, probe_depth + 1);
	std::map<std::pair<TileIndex, Trackdir>, uint8_t> visited;
	std::map<std::pair<TileIndex, Trackdir>, uint> shortest;
	OutletProof proof;
	/* The engine may already be beyond Clear while its tail still owns that
	 * physical conflict resource. Do not require the engine to reach it again. */
	for (const Train *part = start == INVALID_TILE ? v : nullptr; part != nullptr; part = part->Next()) {
		if (probe_budget_remaining == 0) return {};
		probe_budget_remaining--;
		const auto td = part->GetVehicleTrackdir();
		if (!IsValidTrackdir(td) || !AtRole(area, part->tile, td, AutoYieldRole::Clear)) continue;
		const auto ref = MakeTraceRestrictRefId(part->tile, TrackdirToTrack(td));
		if (proof.ref && proof.ref != ref) return {};
		proof.ref = ref;
	}
	if (proof.ref) { proof.complete = true; return proof; }
	bool unknown = false;
	const auto visit = [&](auto &&self, TileIndex tile, Trackdir td, uint distance) -> void {
		if (unknown || !IsValidTile(tile) || !IsValidTrackdir(td)) return;
		const auto key = std::pair{tile, td};
		if (auto it = visited.find(key); it != visited.end()) {
			if (it->second == 1) { unknown = true; return; }
			if (distance >= shortest[key]) return;
		}
		/* Include the long approach plus its alternative local sidings. A
		 * 256-state total could truncate a 200-tile approach merely because
		 * there are two legal parking branches. Still share the original
		 * 1024-node evaluation cap and reserve half for other decisions. */
		if (probe_budget_remaining == 0 || visited.size() >= MAX_PROBE_NODES_PER_EVALUATION / 2) { unknown = true; return; }
		probe_budget_remaining--;
		visited[key] = 1;
		shortest[key] = distance;
		if (HasOnewaySignalBlockingTrackdir(tile, td)) { visited[key] = 2; return; }
		if (!SignalAllowsRoute(v, tile, td, true)) { unknown = true; visited[key] = 2; return; }
		if (AtRole(area, tile, td, AutoYieldRole::Clear)) {
			const auto ref = MakeTraceRestrictRefId(tile, TrackdirToTrack(td));
			if (proof.ref && proof.ref != ref) { unknown = true; return; }
			proof.distance = proof.ref ? std::min(proof.distance, distance) : distance;
			proof.ref = ref;
		} else if (IsSplitPredictionTarget(v, tile) || IsRailDepotTile(tile)) {
			unknown = true;
		} else {
			AutoYieldTrack follow(v);
			if (follow.Follow(tile, td)) {
				if (size_t(follow.tiles_skipped) > probe_budget_remaining) { unknown = true; return; }
				probe_budget_remaining -= follow.tiles_skipped;
				if (follow.is_station) {
					const TileIndexDiff step = TileOffsByDiagDir(follow.exitdir);
					for (int i = follow.tiles_skipped; i > 0; i--) {
						if (IsSplitPredictionTarget(v, follow.new_tile - step * i)) { unknown = true; return; }
					}
				}
				for (auto bits = follow.new_td_bits; bits != TRACKDIR_BIT_NONE; bits = KillFirstBit(bits)) {
					self(self, follow.new_tile, FindFirstTrackdir(bits), distance + 1 + follow.tiles_skipped);
				}
			}
		}
		visited[key] = 2;
	};
	const Train *front = v->GetMovingFront();
	visit(visit, start == INVALID_TILE ? front->tile : start, start == INVALID_TILE ? front->GetVehicleTrackdir() : start_td, 0);
	proof.complete = !unknown;
	return proof;
}

OutletProof ProveOutlet(const Area &area, const Train *v)
{
	for (size_t i = 0; i < outlet_count; i++) if (outlet_cache[i].area == &area && outlet_cache[i].train == v) return outlet_cache[i].proof;
	const auto proof = ComputeOutlet(area, v);
	if (outlet_count < outlet_cache.size()) outlet_cache[outlet_count++] = {&area, v, proof};
	return proof;
}

/** Unknown is deliberately distinct from a confirmed conflict. Ordinary PBS
 * continues to protect crossings, opposing moves and unresolved route choices. */
YieldConflict ClassifyYieldConflict(const Area &area, const Train *a, const Train *b)
{
	/* Resolve committed multi-outlet routes before spending the optional
	 * allowance on all possible topology branches. Otherwise that older
	 * forecast can exhaust the budget and preserve a saved wait as Unknown
	 * even after the express's whole body has cleared the last shared rail.
	 * Different exits alone prove nothing: compare tracks and bodies too.
	 * Single-outlet and unresolved routes keep the existing classification. */
	if (area.clear_count > 1) {
		const auto *ap = GetSplitRouteProof(area, a), *bp = GetSplitRouteProof(area, b);
		if (ap != nullptr && bp != nullptr && ap->native_selected && bp->native_selected) {
			const auto resource = FindCommonConflict(area, a, b);
			Debug(yapf, 7, "AutoYield area={} train={} other={} selected-routes={}/{} resource={} reason={}",
					area.id, a->index, b->index, *ap->clearance, *bp->clearance,
					resource ? resource->tile : INVALID_TILE, resource ? "shared-track-or-body" : "actual-corridors-disjoint");
			if (resource) Debug(yapf, 7, "AutoYield area={} train={} other={} tracks={} distance={}/{} body={} targets={}/{}",
					area.id, a->index, b->index, to_underlying(resource->tracks), resource->left_distance,
					resource->right_distance, resource->right_occupied, a->dest_tile, b->dest_tile);
			if (!resource) return YieldConflict::Independent;
			/* Two actual routes to distinct outlets may only cross through
			 * different tracks of a switch. A common exit direction on that tile
			 * is not following the same rail. Leave such transient interlocking
			 * exclusively to PBS, rather than retaining an overtaking dependency.
			 * A later shared directed rail (including a remerge) keeps the original
			 * decision, as do equal/unknown outlets and incomplete route evidence. */
			if (ap->clearance != bp->clearance && !resource->same_directed_track &&
					IsValidTrackdir(ap->clearance_td) && IsValidTrackdir(bp->clearance_td) &&
					TrackdirToExitdir(ap->clearance_td) == TrackdirToExitdir(bp->clearance_td)) {
				Debug(yapf, 7, "AutoYield area={} train={} other={} independent-departures=native-switch-crossing-only PBS-authority=1", area.id, a->index, b->index);
				return YieldConflict::Independent;
			}
			return resource->directed ? YieldConflict::SharedDirectedResource : YieldConflict::CrossingResource;
		}
	}
	const auto left = ProveOutlet(area, a), right = ProveOutlet(area, b);
	if (left.complete && right.complete && left.ref && left.ref == right.ref) return YieldConflict::SharedDirectedResource;
	/* An exhaustively ineligible forward corridor (e.g. incompatible rails
	 * before any clearance) cannot demand overtaking priority. Occupied rails
	 * remain protected by native PBS. Signals and budgets produce Unknown,
	 * never this result; another area's reachable mandatory target does too. */
	if ((left.complete && !left.ref) || (right.complete && !right.ref)) return YieldConflict::Independent;
	if (const auto resource = FindCommonConflict(area, a, b)) {
		Debug(yapf, 7, "AutoYield area={} train={} other={} resource={}:{} directed={} distance={}/{} body={}",
				area.id, a->index, b->index, resource->tile, to_underlying(resource->tracks), resource->directed,
				resource->left_distance, resource->right_distance, resource->right_occupied);
		return resource->directed ? YieldConflict::SharedDirectedResource : YieldConflict::CrossingResource;
	}
	if (ProvenIndependentExits(area, a, b)) return YieldConflict::Independent;
	/* The proved prefix is useful even when a later fork is unknown. Only
	 * shared directed tracks count here, not opposite moves or a crossing. */
	const auto *ap = GetSplitRouteProof(area, a), *bp = GetSplitRouteProof(area, b);
	if (ap != nullptr && bp != nullptr) {
		/* Complete current corridors may end at different regions' boundaries
		 * or mandatory targets. Actual body tracks were included above; once
		 * disjoint, an unrelated downstream Clear must not keep this pair held. */
		if (ap->complete && bp->complete) return YieldConflict::Independent;
		std::set<std::pair<TileIndex, Trackdir>> prefix;
		for (const auto &node : ap->tiles) if (IsValidTrackdir(node.td)) prefix.emplace(node.tile, node.td);
		for (const auto &node : bp->tiles) if (prefix.contains({node.tile, node.td})) return YieldConflict::SharedDirectedResource;
	}
	Debug(yapf, 7, "AutoYield area={} train={} other={} reason=unresolved-route targets={}/{} budget={}",
			area.id, a->index, b->index, a->dest_tile, b->dest_tile, probe_budget_remaining);
	return YieldConflict::Unknown;
}

bool HasYieldObligation(const Area &area, const Train *v, VehicleID target)
{
	for (const auto &[ref, lane] : area.lanes) {
		if (!lane->IsOccupant(v->index)) continue;
		auto found = area.targets.find(ref);
		if (found != area.targets.end() && found->second->IsOccupant(target)) return true;
	}
	return false;
}

/** Reuse saved obligations and the native stopped-at-PBS state. There is no
 * unsaved session flag that could disappear on load or differ on a client. */
bool YieldWaitInProgress(const Area &area, const Train *v)
{
	if (v->cur_speed != 0 || !HasLaneClaim(area, v)) return false;
	if (v->flags.Test(VehicleRailFlag::Stuck)) return true;
	for (const auto &[ref, lane] : area.lanes) if (lane->IsOccupant(v->index)) {
		auto targets = area.targets.find(ref);
		if (targets != area.targets.end() && !targets->second->occupants.empty()) return true;
	}
	return false;
}

/** Follow saved wait edges through native vehicle/slot indexes, not all trains.
 * A proved logical cycle may yield to ordinary PBS only at its largest saved
 * vehicle ID and only with a positively free forward departure. */
bool SafeYieldCycleFallback(const Area &area, const Train *v, const Train *target)
{
	if (target->index > v->index || !HasLaneClaim(area, v) || v->cur_speed != 0 || !HasYieldObligation(area, v, target->index)) return false;
	std::set<VehicleID> path;
	size_t remaining = MAX_PARTICIPANTS;
	const auto reaches = [&](auto &&self, VehicleID id) -> bool {
		if (id == v->index) return true;
		if (remaining == 0 || id > v->index || !path.insert(id).second) return false;
		remaining--;
		std::vector<TraceRestrictSlotID> memberships;
		TraceRestrictGetVehicleSlots(id, memberships);
		std::set<VehicleID> next;
		for (auto slot_id : memberships) {
			const auto *slot = TraceRestrictSlot::GetIfValid(slot_id);
			if (slot == nullptr || slot->automatic_yield_area == 0 || slot->automatic_yield_signal == AUTO_YIELD_PENDING_REF || slot->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets)) continue;
			auto group = areas.find({CompanyID{slot->owner}, uint16_t(slot->automatic_yield_area - 1)});
			if (group == areas.end() || !group->second.enabled || !group->second.detection->IsOccupant(id)) continue;
			auto edges = group->second.targets.find(slot->automatic_yield_signal);
			const Train *current = GetTrain(id);
			if (edges != group->second.targets.end() && Active(current)) for (VehicleID next_id : edges->second->occupants) {
				const Train *other = GetTrain(next_id);
				if (Active(other) && RegisteredForYield(other) && HigherPriority(group->second, other, current, group->second.percent) && !StationPriorityPaused(group->second, other)) next.insert(next_id);
			}
		}
		for (VehicleID next_id : next) if (self(self, next_id)) return true;
		path.erase(id);
		return false;
	};
	if (!reaches(reaches, target->index)) return false;
	AutoYieldPreferenceBudget allowance;
	const Train *front = v->GetMovingFront();
	if (!RouteToRole(area, v, front->tile, front->GetVehicleTrackdir(), AutoYieldRole::Clear, true)) return false;
	Debug(yapf, 6, "AutoYield area={} train={} target={} cycle=confirmed fallback=free-native-PBS", area.id, v->index, target->index);
	return true;
}

bool CanClearBefore(const Area &area, const Train *v, const Train *other)
{
	const auto slow = ProveOutlet(area, v), fast = ProveOutlet(area, other);
	return DetectionSpeed(area, v) != DetectionSpeed(area, other) && slow.complete && fast.complete && slow.ref && slow.ref == fast.ref && fast.distance > slow.distance &&
			uint64_t(fast.distance) * DetectionSpeed(area, v) >
			uint64_t(slow.distance + CeilDiv(v->gcache.cached_total_length, TILE_SIZE) + 8) * DetectionSpeed(area, other);
}
bool StoppedQueueBehind(const Train *arrival, const Train *follower);

struct TurnoverReleasePath {
	std::map<TileIndex,TrackBits> tracks;
	uint distance = 0;
};

/** The area marker is not the end of a native PBS block. Prove a free,
 * directed departure through Clear and the next native signal, so the tail
 * can actually release the block the priority train needs to enter. */
std::optional<TurnoverReleasePath> ProveTurnoverReleasePath(const Area &area,const Train *v,const Train *yielding_reservation)
{
	AutoRestoreBackup guard(probe_depth,probe_depth+1);
	TurnoverReleasePath path;
	bool cleared=false;
	uint clearance_distance=0;
	for(const Train *part=v;part;part=part->Next()){
		if(probe_budget_remaining==0)return std::nullopt;
		probe_budget_remaining--;
		if(!IsValidTile(part->tile)||part->track==TRACK_BIT_NONE||(part->track&~TRACK_BIT_ALL)!=TRACK_BIT_NONE)return std::nullopt;
		cleared |= AtRole(area,part->tile,part->GetVehicleTrackdir(),AutoYieldRole::Clear);
		path.tracks[part->tile] |= part->track;
	}
	const Train *front=v->GetMovingFront();TileIndex tile=front->tile;Trackdir td=front->GetVehicleTrackdir();
	std::set<std::pair<TileIndex,Trackdir>> seen;
	while(probe_budget_remaining>0&&seen.size()<MAX_ROUTE_NODES&&IsValidTile(tile)&&IsValidTrackdir(td)&&seen.emplace(tile,td).second){
		probe_budget_remaining--;
		if(!FreeTrackTile(v,tile,td,nullptr,yielding_reservation)||IsRailTunnelBridgeTile(tile)){Debug(yapf,7,"AutoYield area={} turnover={} release-path=blocked tile={} distance={}",area.id,v->index,tile,path.distance);return std::nullopt;}
		path.tracks[tile] |= TrackToTrackBits(TrackdirToTrack(td));
		const bool at_clear=AtRole(area,tile,td,AutoYieldRole::Clear);
		/* A signal immediately after Clear is too short a stopping point for
		 * a long consist. Its whole tail must fit beyond the shared boundary;
		 * otherwise prove the continuation through another native block. */
		if(cleared&&!at_clear&&path.distance>=clearance_distance+CeilDiv(v->gcache.cached_total_length,TILE_SIZE)+1&&IsPlainRailTile(tile)&&HasSignalOnTrackdir(tile,td)){
			if(const auto*program=GetExistingTraceRestrictProgram(tile,TrackdirToTrack(td)))for(auto it:program->IterateInstructions()){
				if(IsAutoYieldInstruction(it.Instruction().GetType())&&it.Instruction().GetType()!=TRIT_AUTO_YIELD_CLEAR)return std::nullopt;
			}
			return path;
		}
		if(at_clear&&!cleared)clearance_distance=path.distance;
		cleared |= at_clear;
		if(IsSplitPredictionTarget(v,tile)||IsRailDepotTile(tile)){Debug(yapf,7,"AutoYield area={} turnover={} release-path=target tile={} distance={}",area.id,v->index,tile,path.distance);return std::nullopt;}
		AutoYieldTrack follow(v);
		if(!follow.Follow(tile,td)||follow.is_tunnel||follow.is_bridge||follow.new_td_bits==TRACKDIR_BIT_NONE||KillFirstBit(follow.new_td_bits)!=TRACKDIR_BIT_NONE){Debug(yapf,7,"AutoYield area={} turnover={} release-path=branch-or-end tile={} distance={} next={}",area.id,v->index,tile,path.distance,uint(follow.new_td_bits));return std::nullopt;}
		if(follow.tiles_skipped!=0){
			if(!follow.is_station||size_t(follow.tiles_skipped)>probe_budget_remaining)return std::nullopt;
			const TileIndexDiff step=TileOffsByDiagDir(follow.exitdir);
			for(int i=follow.tiles_skipped;i>0;i--){
				probe_budget_remaining--;const TileIndex station_tile=follow.new_tile-step*i;
				if(IsSplitPredictionTarget(v,station_tile)||!FreeTrackTile(v,station_tile,FindFirstTrackdir(follow.new_td_bits),nullptr,yielding_reservation))return std::nullopt;
				path.tracks[station_tile] |= TrackToTrackBits(TrackdirToTrack(FindFirstTrackdir(follow.new_td_bits)));
			}
		}
		tile=follow.new_tile;td=FindFirstTrackdir(follow.new_td_bits);path.distance+=1+follow.tiles_skipped;
	}
	return std::nullopt;
}

/** Lower bound over all legal forward branches, not a guessed route. Reaching
 * a physical crossing or shared track gives an earliest possible conflict;
 * unresolved structures, mandatory targets and budgets cannot grant turnover. */
std::optional<uint> EarliestTurnoverConflict(const Train *v,const TurnoverReleasePath &path,bool &complete)
{
	complete=false;AutoRestoreBackup guard(probe_depth,probe_depth+1);
	const auto overlaps=[&](TileIndex tile,TrackBits bits){auto it=path.tracks.find(tile);return it!=path.tracks.end()&&((it->second&bits)!=TRACK_BIT_NONE||TracksOverlap(it->second|bits));};
	for(const Train *part=v;part;part=part->Next()){
		if(probe_budget_remaining==0)return std::nullopt;
		probe_budget_remaining--;
		if(!IsValidTile(part->tile)||part->track==TRACK_BIT_NONE||(part->track&~TRACK_BIT_ALL)!=TRACK_BIT_NONE)return std::nullopt;
		if(overlaps(part->tile,part->track)){complete=true;return 0;}
	}
	using Node=std::tuple<uint,TileIndex,Trackdir>;
	std::priority_queue<Node,std::vector<Node>,std::greater<Node>> pending;
	const Train*front=v->GetMovingFront();pending.emplace(0,front->tile,front->GetVehicleTrackdir());
	std::set<std::pair<TileIndex,Trackdir>> seen;uint unknown_distance=UINT_MAX;
	while(!pending.empty()&&probe_budget_remaining>0&&seen.size()<MAX_PROBE_NODES_PER_EVALUATION/2){
		const auto [distance,tile,td]=pending.top();pending.pop();if(!seen.emplace(tile,td).second)continue;
		probe_budget_remaining--;
		if(!IsValidTile(tile)||!IsValidTrackdir(td)||HasOnewaySignalBlockingTrackdir(tile,td))continue;
		if(overlaps(tile,TrackToTrackBits(TrackdirToTrack(td)))){if(unknown_distance<distance)return std::nullopt;complete=true;return distance;}
		if(IsSplitPredictionTarget(v,tile)){unknown_distance=std::min(unknown_distance,distance);continue;}
		if(IsRailDepotTile(tile))continue;
		AutoYieldTrack follow(v);
		if(!follow.Follow(tile,td))continue;
		if(follow.is_tunnel||follow.is_bridge||(follow.tiles_skipped!=0&&!follow.is_station)){unknown_distance=std::min(unknown_distance,distance);continue;}
		bool stop=false;
		const TileIndexDiff step=TileOffsByDiagDir(follow.exitdir);
		for(int i=follow.tiles_skipped;i>0;i--){
			if(probe_budget_remaining==0){unknown_distance=std::min(unknown_distance,distance);stop=true;break;}
			probe_budget_remaining--;const TileIndex station_tile=follow.new_tile-step*i;
			if(overlaps(station_tile,TrackToTrackBits(TrackdirToTrack(FindFirstTrackdir(follow.new_td_bits))))){pending.emplace(distance+1+follow.tiles_skipped-i,station_tile,FindFirstTrackdir(follow.new_td_bits));stop=true;break;}
			if(IsSplitPredictionTarget(v,station_tile)){unknown_distance=std::min(unknown_distance,distance+1+follow.tiles_skipped-i);stop=true;break;}
		}
		if(stop)continue;
		for(auto bits=follow.new_td_bits;bits!=TRACKDIR_BIT_NONE;bits=KillFirstBit(bits))pending.emplace(distance+1+follow.tiles_skipped,follow.new_tile,FindFirstTrackdir(bits));
	}
	complete=pending.empty()&&unknown_distance==UINT_MAX;return std::nullopt;
}

/** Integer risk estimate, not permission to occupy track. Use the last common
 * resource for the departing tail and the first common resource for the
 * express head. Unknown geometry cannot justify a full-lane turnover. */
std::optional<uint64_t> TurnoverCost(const Area &area, const Train *departing, const Train *requester, bool station_access, TraceRestrictRefId freed_ref,const Train *yielding_reservation=nullptr)
{
	uint64_t cost = 0;
	std::optional<TurnoverReleasePath> release_path;
	auto participants = Participants(area, departing);
	for (const auto &[ref, lane] : area.lanes) if (lane->IsOccupant(departing->index)) {
		for (VehicleID id : area.targets.at(ref)->occupants) {
			if (std::find(participants.begin(), participants.end(), id) == participants.end()) participants.push_back(id);
		}
	}
	if (participants.size() > MAX_PARTICIPANTS) return std::nullopt;
	for (VehicleID id : participants) {
		const Train *express = GetTrain(id);
		if (express == departing || !Active(express) || !HigherPriority(area, express, departing, area.percent) || StationPriorityPaused(area, express)) continue;
		/* Other stationary siding candidates have no through movement yet.
		 * Treating each as an arriving express makes all proposals veto one
		 * another. Live reservations and station priority remain protected. */
		if (express->cur_speed == 0 && HasLaneClaim(area, express) && !ServingPlatform(area, express)) continue;
		/* This exact arrival cannot use its occupied platform before it is
		 * freed. This exemption never applies to another passing express. */
		if (station_access && express == requester) continue;
		if (ScheduledPlatformStop(express, GetPlatformInfo(area, express, freed_ref))) {
			/* A scheduled express cannot pass the exit before completing its
			 * stop. If every suitable platform is still occupied by a stationary
			 * train, this selected departure must free one before it can arrive. */
			bool blocked = true;
			for (const auto &[ref, lane] : area.lanes) {
				if (!ScheduledPlatformStop(express, GetPlatformInfo(area, express, ref))) continue;
				if (lane->occupants.empty()) { blocked = false; break; }
				for (VehicleID occupant : lane->occupants) if (const Train *train = GetTrain(occupant); train == nullptr || train->cur_speed != 0) blocked = false;
			}
			if (blocked) continue;
		}
		/* A physically proved stopped queue already depends on freeing this
		 * lane. Its follower cannot race through the resource before the
		 * requester; the ordinary free-departure/replacement proofs still apply. */
		if (StoppedQueueBehind(requester, express)) continue;
		if (!area.detection->IsOccupant(express->index) || area.clear_count>1) {
			const auto conflict = ClassifyYieldConflict(area, departing, express);
			/* A neighbouring detector is only a candidate source. An incomplete
			 * route without a previously confirmed obligation must not veto a
			 * necessary platform departure merely through geographic proximity.
			 * Local priority traffic and saved obligations remain conservative. */
			const bool unconfirmed_neighbour = conflict == YieldConflict::Unknown &&
					!area.detection->IsOccupant(express->index) && !HasYieldObligation(area, departing, express->index);
			if(conflict==YieldConflict::Independent||unconfirmed_neighbour)continue;
		}
		if(!release_path)release_path=ProveTurnoverReleasePath(area,departing,yielding_reservation);
		if(!release_path){Debug(yapf,7,"AutoYield area={} turnover={} rejected=native-block-release-unproved budget={}",area.id,departing->index,probe_budget_remaining);return std::nullopt;}
		bool complete=false;const auto earliest=EarliestTurnoverConflict(express,*release_path,complete);
		if(!earliest){if(complete)continue;Debug(yapf,7,"AutoYield area={} turnover={} express={} rejected=earliest-conflict-unproved budget={}",area.id,departing->index,express->index,probe_budget_remaining);return std::nullopt;}
		const uint speed = DetectionSpeed(area, departing);
		/* A stopped departure gets an acceleration allowance. This forecast is
		 * intentionally conservative; native PBS still checks every reservation. */
		const uint departure_speed = std::max(1u, std::min(speed, std::max(uint(departing->cur_speed), speed / 2)));
		const uint64_t clear = uint64_t(release_path->distance + CeilDiv(departing->gcache.cached_total_length, TILE_SIZE) + 8) * 65536 / departure_speed;
		const uint64_t arrival = uint64_t(*earliest) * 65536 / std::max(1u, DetectionSpeed(area, express));
		Debug(yapf, 7, "AutoYield area={} turnover={} requester={} express={} native-release={} earliest={} clear_estimate={} arrival_estimate={} accepted={}",
				area.id, departing->index, requester->index, express->index, release_path->distance, *earliest, clear, arrival, clear < arrival);
		if (clear >= arrival) return std::nullopt;
		cost = std::max(cost, clear);
	}
	return cost;
}

bool MustWait(const Area &area, const Train *v, bool *fully_known = nullptr)
{
	if (fully_known != nullptr) *fully_known = true;
	if (!area.detection->IsOccupant(v->index)) return false;
	std::array<const Train *, MAX_PARTICIPANTS> faster{};
	size_t count = 0;
	auto participants = Participants(area, v);
	/* A saved dependency is not discarded because a new local lookup ran out
	 * of capacity or the express moved into a neighbouring detector region. */
	for (const auto &[ref, lane] : area.lanes) if (lane->IsOccupant(v->index)) {
		auto target_slot = area.targets.find(ref);
		if (target_slot == area.targets.end()) continue;
		for (VehicleID id : target_slot->second->occupants) {
			if (const Train *target = GetTrain(id); target != nullptr && RegisteredForYield(target) &&
					std::find(participants.begin(), participants.end(), id) == participants.end()) participants.insert(participants.begin(), id);
		}
	}
	for (VehicleID id : participants) {
		const Train *other = GetTrain(id);
		if (other != v && Active(other) && HigherPriority(area, other, v, area.percent) && !StationPriorityPaused(area, other) &&
				(DetectionSpeed(area, other) != DetectionSpeed(area, v) || HasYieldObligation(area, v, id) || ClosePassenger(area, other, v))) {
			if (count == faster.size()) break;
			faster[count++] = other;
		}
	}
	/* Optional direction classification shares a small budget across all
	 * candidates, leaving the original evacuation and claim checks room. */
	AutoYieldPreferenceBudget direction_budget;
	for (size_t i = 0; i < count; i++) {
		const auto conflict = ClassifyYieldConflict(area, v, faster[i]);
		Debug(yapf, 7, "AutoYield area={} train={} other={} wait-conflict={} saved-target={}", area.id, v->index, faster[i]->index, to_underlying(conflict), HasYieldObligation(area, v, faster[i]->index));
		if (SafeYieldCycleFallback(area, v, faster[i])) continue;
		/* A failed/limited forecast is not evidence that an accepted conflict
		 * disappeared. These obligations are saved slots, recorded only after
		 * PBS commits, rather than unsaved process-local decision caches. */
		if (conflict == YieldConflict::Unknown && HasYieldObligation(area, v, faster[i]->index)) return true;
		if (conflict == YieldConflict::Unknown && fully_known != nullptr) *fully_known = false;
		Debug(yapf, 7, "AutoYield area={} train={} other={} conflict={} budget={}", area.id, v->index, faster[i]->index, to_underlying(conflict), probe_budget_remaining);
		if (conflict != YieldConflict::SharedDirectedResource && conflict != YieldConflict::CrossingResource) continue;
		/* A shared outlet alone is not a reason to wait indefinitely for a
		 * remote express. Integer travel estimates include body clearance and
		 * a departure margin; PBS remains the authority for actual admission. */
		if (!HasYieldObligation(area, v, faster[i]->index) && !YieldWaitInProgress(area, v) && CanClearBefore(area, v, faster[i])) continue;
		/* Two confirmed split routes can share the chosen throat even though
		 * another legal route to this same destination avoids it. Keep the wait
		 * for this search; only a later fresh native search may try exclusions.
		 * Equal/unknown outlets and single-outlet following keep their policy. */
		if (area.clear_count > 1 && HasLaneClaim(area, v) && AutoYieldMayRequestSplitAlternative(v, area.id)) {
			const auto *own = GetSplitRouteProof(area, v), *other = GetSplitRouteProof(area, faster[i]);
			if (own != nullptr && other != nullptr && own->native_selected && other->native_selected && own->clearance != other->clearance) {
				std::vector<std::pair<TileIndex, Trackdir>> blocked;
				for (const auto &step : own->tiles) {
					if (step.body || !IsValidTrackdir(step.td)) continue;
					for (const auto &obstacle : other->tiles) if (step.tile == obstacle.tile &&
							((step.tracks & obstacle.tracks) != TRACK_BIT_NONE || TracksOverlap(step.tracks | obstacle.tracks))) {
						blocked.emplace_back(step.tile, step.td); break;
					}
					if (blocked.size() > 64) break;
				}
				AutoYieldRequestSplitAlternative(v, area.id, blocked);
			}
		}
		return true;
	}
	return false;
}

/** A nearby same-speed passenger already in a siding is not a following
 * approach. Protect a ready departure at Main only with a positive free route
 * proof. A passenger still waiting for a faster follower must not prevent the
 * intervening freight from reaching a free siding and unblocking that follower. */
bool PrioritisePassengerExit(const Area &area, const Train *incoming)
{
	if (HasPassengerPriority(incoming) || HasLaneClaim(area, incoming)) return false;
	AutoYieldPreferenceBudget preference_budget;
	for (const auto &[ref, lane] : area.lanes) for (VehicleID id : lane->occupants) {
		const Train *passenger = GetTrain(id);
		bool known = false;
		if (passenger == incoming || !Active(passenger) || !area.detection->IsOccupant(id) ||
				DetectionSpeed(area, passenger) != DetectionSpeed(area, incoming) || !HasPassengerPriority(passenger) ||
				StationPriorityPaused(area, passenger) || !ClosePassenger(area, passenger, incoming) || MustWait(area, passenger, &known) || !known) continue;
		const Train *front = passenger->GetMovingFront();
		if (RouteToRole(area, passenger, front->tile, front->GetVehicleTrackdir(), AutoYieldRole::Clear, true)) return true;
	}
	return false;
}
bool FreeLane(const Area &area, const Train *v, bool approaching, bool ordinary_only, std::optional<TraceRestrictRefId> exact_ref, std::optional<TraceRestrictRefId> required_clearance, bool require_free_departure)
{
	bool check_candidate_destination = false;
	/* With multiple outlets, prove that the candidate can serve the explicit
	 * destination through an allowed first clearance. This is reachability,
	 * not a claim that every possible route uses the same outlet. */
	if (area.clear_count > 1 && (v->current_order.IsType(OT_GOTO_STATION) || v->current_order.IsType(OT_GOTO_WAYPOINT) || v->current_order.IsType(OT_GOTO_DEPOT))) {
		/* All legal local continuations may already prove one outlet even
		 * though the area contains other, independent exits. Rewalking a far
		 * destination can exhaust admission's budget after conflict discovery. */
		const auto local = ProveOutlet(area, v);
		if (local.complete && local.ref && (!required_clearance || required_clearance == local.ref)) {
			required_clearance = local.ref;
		} else {
			/* A choice before the siding need not have one global outlet. Prove
			 * this actual candidate reaches the active destination instead of
			 * exhausting the budget on irrelevant head-to-destination branches. */
			check_candidate_destination = true;
		}
	}
	const auto candidate_clearance = [&](TraceRestrictRefId ref, const Binding &binding, bool &departure_proven) -> std::optional<TraceRestrictRefId> {
		departure_proven = false;
		if (!check_candidate_destination) return required_clearance;
		const auto prediction = GetPredictionOrders(v);
		/* Mixed-station admission can satisfy the current stop before its
		 * departure is predictable. Keep a positive physical first-exit proof;
		 * this does not forecast a conflict or permit crossing a held signal.
		 * Single-lane and all-platform layouts retain their existing rules. */
		if (area.has_platform && area.lanes.size() > 1 && prediction.next == nullptr) {
			const auto platform = GetPlatformInfo(area, v, ref);
			if (ScheduledPlatformStop(v, platform) && platform.exit &&
					std::any_of(area.lanes.begin(), area.lanes.end(), [&](const auto &other) {
						return other.first != ref && !GetPlatformInfo(area, v, other.first).marked;
					})) {
				const Binding *exit = FindBinding(area, *platform.exit);
				std::optional<TraceRestrictRefId> entrance, clearance;
				if (exit != nullptr && SidingParkingLength(area, v, *exit, nullptr, &entrance) &&
						(binding.role == AutoYieldRole::Exit ? exit->ref == ref : entrance == ref) &&
						RouteToRole(area, v, GetTraceRestrictRefIdTileIndex(exit->ref), exit->td, AutoYieldRole::Clear,
								false, required_clearance, nullptr, nullptr, &clearance, std::nullopt, false, true)) {
					Debug(yapf, 7, "AutoYield area={} train={} lane={} admission=current-stop-with-forward-exit outlet={}", area.id, v->index, ref, *clearance);
					return clearance;
				}
			}
		}
		/* An exit-only platform's exclusive parking segment contains the
		 * mandatory stop before this forecast's start. Do not demand reaching
		 * that station again behind the exit. Legacy entrances still forecast
		 * through the station in the ordinary way. Real PBS retains the stop. */
		const bool stopped_before_exit = binding.role == AutoYieldRole::Exit && ScheduledPlatformStop(v, GetPlatformInfo(area, v, ref));
		auto *goal = GetGoalClearanceHint(area, v, GetTraceRestrictRefIdTileIndex(ref), binding.td, stopped_before_exit, false, required_clearance);
		if (goal != nullptr && goal->clearance && (!required_clearance || required_clearance == goal->clearance)) return goal->clearance;
		/* A distant station can be beyond the tile forecast's horizon even
		 * though the candidate rejoins a perfectly valid destination route.
		 * Ask the native, non-reserving station probe only after proving the
		 * local departure reaches an allowed first clearance. Its small shared
		 * segment-node allowance is independent of local YAPF cache history.
		 * This positive admission result never establishes a unique outlet. */
		const Order *target = stopped_before_exit || prediction.first_reached ? prediction.next : prediction.first;
		if (goal != nullptr && target != nullptr && target->IsType(OT_GOTO_STATION)) {
			/* A nearer first outlet may lead to a different destination. Test
			 * every locally reachable first boundary rather than rejecting the
			 * entire lane after that outlet's negative destination proof. Exact
			 * RouteToRole still stops at every other Clear, so this cannot leave
			 * the area and re-enter through a later clearance. The existing
			 * shared local and native budgets bound the whole enumeration. */
			for (const Binding &clear : area.bindings) {
				if (clear.role != AutoYieldRole::Clear || (required_clearance && clear.ref != *required_clearance)) continue;
				const auto outlet = clear.ref;
				if (!RouteToRole(area, v, GetTraceRestrictRefIdTileIndex(ref), binding.td, AutoYieldRole::Clear, false,
						outlet, nullptr, nullptr, nullptr, std::nullopt, false, true)) continue;
				const StationID station = target->GetDestination().ToStationID();
				std::optional<bool> reachable;
				for (size_t i = 0; i < destination_probe_count; i++) {
					const auto &entry = destination_probe_cache[i];
					if (entry.train == v && entry.outlet == outlet && entry.station == station) { reachable = entry.reachable; break; }
				}
				if (!reachable && destination_probe_remaining != 0) {
					uint used = 0;
					AutoRestoreBackup guard(probe_depth, probe_depth + 1);
					reachable = YapfTrainCanReachStationBounded(v, GetTraceRestrictRefIdTileIndex(outlet), clear.td,
							station, std::min(destination_probe_remaining, MAX_DESTINATION_PROBE_NODES), used);
					destination_probe_remaining -= used;
					if (destination_probe_count < destination_probe_cache.size()) destination_probe_cache[destination_probe_count++] = {v, outlet, station, *reachable};
					Debug(yapf, 7, "AutoYield area={} train={} lane={} outlet={} destination-probe={} nodes={} remaining={}", area.id, v->index, ref, outlet, *reachable, used, destination_probe_remaining);
				}
				if (reachable.value_or(false)) {
					/* This exact local departure was just proved above. Repeating
					 * it can exhaust admission's budget and hide a usable siding.
					 * It does not prove a free PBS departure; those callers must
					 * still perform their occupancy/reservation check below. */
					departure_proven = true;
					goal->clearance = outlet;
					return outlet;
				}
			}
		}
		Debug(yapf, 7, "AutoYield area={} train={} lane={} rejected=destination-outlet-unknown-or-incompatible budget={}", area.id, v->index, ref, probe_budget_remaining);
		return std::nullopt;
	};
	/* Priority considers a contender's reachable branch even while a train
	 * ahead temporarily occupies the shared approach. Actual diversion and
	 * admission always require the original full free-path proof. */
	for (const auto &[ref, lane] : area.lanes) {
		if (exact_ref && ref != *exact_ref) continue;
		if (!lane->occupants.empty() && !lane->IsOccupant(v->index)) {
			Debug(yapf, 7, "AutoYield area={} train={} lane={} rejected=claimed", area.id, v->index, ref);
			continue;
		}
		const Binding *binding = FindBinding(area, ref);
		if (binding == nullptr) { Debug(yapf, 7, "AutoYield area={} train={} lane={} rejected=missing-marker", area.id, v->index, ref); continue; }
		if (ordinary_only && GetPlatformInfo(area, v, ref).marked) { Debug(yapf, 7, "AutoYield area={} train={} lane={} rejected=platform-preference", area.id, v->index, ref); continue; }
		if (area.exit_only) {
			const auto length = SidingParkingLength(area, v, *binding);
			if (!length || *length < v->gcache.cached_total_length + TILE_SIZE) {
				Debug(yapf, 7, "AutoYield area={} train={} lane={} rejected=parking length={} required={}", area.id, v->index, ref, length.value_or(0), v->gcache.cached_total_length + TILE_SIZE);
				continue;
			}
			if (!RouteToRole(area, v, v->GetMovingFront()->tile, v->GetMovingFront()->GetVehicleTrackdir(), AutoYieldRole::Exit, !approaching, ref)) {
				Debug(yapf, 7, "AutoYield area={} train={} lane={} rejected=ingress-or-reservation budget={}", area.id, v->index, ref, probe_budget_remaining);
				continue;
			}
			bool departure_proven = false;
			const auto clearance = candidate_clearance(ref, *binding, departure_proven);
			if (check_candidate_destination && !clearance) continue;
			if (!require_free_departure && departure_proven) return true;
			if (RouteToRole(area, v, GetTraceRestrictRefIdTileIndex(ref), binding->td, AutoYieldRole::Clear, require_free_departure, clearance,
					nullptr, nullptr, nullptr, std::nullopt, false, !require_free_departure)) return true;
			Debug(yapf, 7, "AutoYield area={} train={} lane={} rejected=departure-or-reservation budget={}", area.id, v->index, ref, probe_budget_remaining);
			continue;
		}
		if (!RouteToRole(area, v, v->GetMovingFront()->tile, v->GetMovingFront()->GetVehicleTrackdir(), AutoYieldRole::Siding, !approaching, ref)) {
			Debug(yapf, 7, "AutoYield area={} train={} lane={} rejected=ingress-or-reservation budget={}", area.id, v->index, ref, probe_budget_remaining);
			continue;
		}
		const auto length = LaneParkingLength(area, v, *binding);
		if (!length || *length < v->gcache.cached_total_length + TILE_SIZE) {
			Debug(yapf, 7, "AutoYield area={} train={} lane={} rejected=parking length={} required={} budget={}", area.id, v->index, ref, length.value_or(0), v->gcache.cached_total_length + TILE_SIZE, probe_budget_remaining);
			continue;
		}
		bool departure_proven = false;
		const auto clearance = candidate_clearance(ref, *binding, departure_proven);
		if (check_candidate_destination && !clearance) continue;
		if (!require_free_departure && departure_proven) return true;
		if (RouteToRole(area, v, GetTraceRestrictRefIdTileIndex(ref), binding->td, AutoYieldRole::Clear, require_free_departure, clearance,
				nullptr, nullptr, nullptr, std::nullopt, false, !require_free_departure)) return true;
		Debug(yapf, 7, "AutoYield area={} train={} lane={} rejected=departure-or-reservation budget={}", area.id, v->index, ref, probe_budget_remaining);
	}
	return false;
}
constexpr uint EQUAL_SPEED_TURNOVER_GAP = 32;

/** Confirm an actual stopped queue, not just distances on adjacent feeders. */
bool StoppedQueueBehind(const Train *arrival, const Train *follower)
{
	if (arrival->cur_speed != 0 || follower->cur_speed != 0) return false;
	AutoYieldPreferenceBudget budget;
	AutoRestoreBackup guard(probe_depth, probe_depth + 1);
	const Train *front = follower->GetMovingFront();
	TileIndex tile = front->tile;
	Trackdir td = front->GetVehicleTrackdir();
	uint travelled = 0;
	const auto contact = [&](TileIndex at, Trackdir direction) {
		for (const Train *part : VehiclesOnTile<VehicleType::Train>(at)) {
			if (part->Primary() == arrival->Primary() &&
					(part->track & TrackToTrackBits(TrackdirToTrack(direction)))) return true;
		}
		return false;
	};
	while (travelled <= EQUAL_SPEED_TURNOVER_GAP && probe_budget_remaining > 0) {
		probe_budget_remaining--;
		if (contact(tile,td)) return true;
		if (HasOnewaySignalBlockingTrackdir(tile,td) || IsRailDepotTile(tile) || !SignalAllowsRoute(follower,tile,td)) return false;
		AutoYieldTrack follow(follower);
		if (!follow.Follow(tile,td) || follow.new_td_bits == TRACKDIR_BIT_NONE || KillFirstBit(follow.new_td_bits) != TRACKDIR_BIT_NONE) return false;
		const Trackdir next_td = FindFirstTrackdir(follow.new_td_bits);
		if (follow.is_station) {
			const TileIndexDiff step = TileOffsByDiagDir(follow.exitdir);
			for (int i=follow.tiles_skipped; i>0; --i) {
				if (++travelled > EQUAL_SPEED_TURNOVER_GAP || probe_budget_remaining == 0) return false;
				probe_budget_remaining--;
				if (contact(follow.new_tile-step*i,next_td)) return true;
			}
		} else if (follow.tiles_skipped > 0) return false;
		travelled++;
		tile=follow.new_tile;td=next_td;
	}
	return false;
}

bool PassengerService(const Train *v)
{
	for (const Train *part=v->First(); part!=nullptr; part=part->Next()) {
		if (part->cargo_cap != 0 && part->cargo_type < NUM_CARGO && IsCargoInClass(part->cargo_type,CargoClass::Passengers)) return true;
	}
	return false;
}

/** Equal-speed turnover is normally for distant arrivals. Break a near deadlock
 * only when the priority follower is physically stopped behind that arrival. */
bool EqualSpeedTurnover(const Area &area, const Train *v, const MainApproach &arrival, const std::vector<MainApproach> &approaches)
{
	{
		AutoYieldPreferenceBudget budget;
		const Train *front=arrival.train->GetMovingFront(), *departing=v->GetMovingFront();
		std::optional<TraceRestrictRefId> clearance;
		const auto incoming_distance=RouteToRole(area,arrival.train,front->tile,front->GetVehicleTrackdir(),AutoYieldRole::Clear,false,std::nullopt,nullptr,nullptr,&clearance);
		const auto departing_distance=clearance ? RouteToRole(area,v,departing->tile,departing->GetVehicleTrackdir(),AutoYieldRole::Clear,false,clearance) : std::nullopt;
		if (incoming_distance && departing_distance && *incoming_distance > *departing_distance + EQUAL_SPEED_TURNOVER_GAP) return true;
	}
	if (v->cur_speed != 0 || arrival.train->cur_speed != 0) return false;
	for (const MainApproach &follower : approaches) {
		if (HasLaneClaim(area,follower.train) || StationPriorityPaused(area,follower.train) ||
				!FollowingThreat(area,follower,arrival) || !HigherPriority(area,follower.train,v,area.percent)) continue;
		if (StoppedQueueBehind(arrival.train,follower.train)) return true;
	}
	return false;
}

/** A long reservation can already pass Main and end beyond it. Prove the
 * actual continuous, owned forward route rather than searching forward from
 * that endpoint (or treating an unreserved branch as a commitment). */
static bool ReservedAutoYieldMainAhead(const Area &area, const Train *v, std::optional<TraceRestrictRefId> *matched_main = nullptr, AutoYieldRole role = AutoYieldRole::Main)
{
	const Train *front = v->GetMovingFront();
	TileIndex tile = front->tile;
	Trackdir td = front->GetVehicleTrackdir();
	if (!IsValidTrackdir(td) || !(GetReservedTrackbits(tile) & TrackToTrackBits(TrackdirToTrack(td)))) return false;
	const Train *owner = GetTrainForReservation(tile, TrackdirToTrack(td));
	if (owner == nullptr || owner->Primary() != v->Primary()) return false;
	std::set<std::pair<TileIndex, Trackdir>> seen;
	while (seen.size() < MAX_ROUTE_NODES && probe_budget_remaining > 0 && seen.emplace(tile, td).second) {
		probe_budget_remaining--;
		if (!(GetReservedTrackbits(tile) & TrackToTrackBits(TrackdirToTrack(td))) || HasOnewaySignalBlockingTrackdir(tile, td)) return false;
		if (AtRole(area, tile, td, role)) {
			if (matched_main != nullptr) *matched_main = MakeTraceRestrictRefId(tile, TrackdirToTrack(td));
			return true;
		}
		/* A committed siding or an already passed area is not a main route. */
		if (AtRole(area, tile, td, AutoYieldRole::Exit) || AtRole(area, tile, td, AutoYieldRole::Clear) || IsRailDepotTile(tile)) return false;
		AutoYieldTrack follow(v);
		if (!follow.Follow(tile, td) || follow.tiles_skipped != 0 || follow.is_tunnel || follow.is_bridge) return false;
		TrackdirBits reserved = TRACKDIR_BIT_NONE;
		for (TrackdirBits bits = follow.new_td_bits; bits != TRACKDIR_BIT_NONE; bits = KillFirstBit(bits)) {
			const Trackdir next = FindFirstTrackdir(bits);
			if (GetReservedTrackbits(follow.new_tile) & TrackToTrackBits(TrackdirToTrack(next))) reserved |= TrackdirToTrackdirBits(next);
		}
		if (reserved == TRACKDIR_BIT_NONE || KillFirstBit(reserved) != TRACKDIR_BIT_NONE) return false;
		tile = follow.new_tile;
		td = FindFirstTrackdir(reserved);
	}
	return false;
}

/** A read-only proposal. Actual departure still requires ordinary PBS reservation. */
struct EvacuationPlan {
	const Train *departing = nullptr;
	const Train *incoming = nullptr;
};
EvacuationPlan Evacuation(const Area &area, const std::vector<MainApproach> *known_approaches = nullptr, const Train *known_no_free_lane = nullptr)
{
	const std::vector<MainApproach> local_approaches = known_approaches == nullptr ? GetMainApproaches(area) : std::vector<MainApproach>{};
	const auto &approaches = known_approaches != nullptr ? *known_approaches : local_approaches;
	std::vector<MainApproach> incoming;
	std::vector<const Train *> overtaking_arrivals;
	for (const MainApproach &slow : approaches) {
		if (!area.detection->IsOccupant(slow.train->index)) continue;
		if (HasLaneClaim(area, slow.train)) continue;
		bool threatened = false, merging = false;
		for (const MainApproach &fast : approaches) {
			if (FollowingThreat(area, fast, slow, &merging)) threatened = true;
		}
		const bool overtaking = threatened && (slow.train == known_no_free_lane || merging || !FreeParallelMain(area, slow));
		if (overtaking) overtaking_arrivals.push_back(slow.train);
		/* The fastest train may itself require an occupied platform. It has
		 * no faster follower, so ordinary overtaking cannot free its station. */
		if (overtaking || (area.has_platform && slow.train->current_order.IsType(OT_GOTO_STATION))) incoming.push_back(slow);
	}
	/* Count distinct occupied/committed lanes and positively identified
	 * arrivals. When demand exceeds capacity, waiting for an actual stopped
	 * cycle wastes the frontmost train's opportunity to clear ahead. */
	std::set<VehicleID> demand;
	std::set<VehicleID> station_requests;
	for (const auto &[ref, lane] : area.lanes) for (VehicleID id : lane->occupants) {
		if (const Train *train = GetTrain(id); Active(train)) demand.insert(id);
	}
	for (const MainApproach &arrival : incoming) {
		bool needed = std::find(overtaking_arrivals.begin(), overtaking_arrivals.end(), arrival.train) != overtaking_arrivals.end();
		if (area.has_platform && arrival.train->current_order.IsType(OT_GOTO_STATION)) {
			AutoYieldPreferenceBudget preference_budget;
			for (const auto &[ref, lane] : area.lanes) if (ScheduledPlatformStop(arrival.train, GetPlatformInfo(area, arrival.train, ref))) {
				needed = true; station_requests.insert(arrival.train->index); break;
			}
		}
		if (needed) demand.insert(arrival.train->index);
	}
	SetApproachGaps(area,incoming,approaches);
	std::sort(incoming.begin(), incoming.end(), [&area, &station_requests](const MainApproach &a, const MainApproach &b) {
		const bool a_station = station_requests.contains(a.train->index), b_station = station_requests.contains(b.train->index);
		if (a_station != b_station) return a_station;
		if (a_station) return std::tuple{DetectionSpeed(area,a.train),HasPassengerPriority(a.train),a.priority_gap,a.distance,a.train->index} <
				std::tuple{DetectionSpeed(area,b.train),HasPassengerPriority(b.train),b.priority_gap,b.distance,b.train->index};
		return std::tuple{a.priority_gap,a.train->GetDisplayMaxSpeed(),HasPassengerPriority(a.train),a.distance,a.train->index} <
				std::tuple{b.priority_gap,b.train->GetDisplayMaxSpeed(),HasPassengerPriority(b.train),b.distance,b.train->index};
	});
	for (const MainApproach &arrival : incoming) {
		Debug(yapf, 7, "AutoYield area={} arrival-rank={} design={} gap={} distance={} station={}", area.id, arrival.train->index, arrival.train->GetDisplayMaxSpeed(), arrival.priority_gap, arrival.distance, station_requests.contains(arrival.train->index));
		std::vector<TraceRestrictRefId> requested_platforms;
		if (area.has_platform && arrival.train->current_order.IsType(OT_GOTO_STATION)) {
			for (const auto &[ref, lane] : area.lanes) {
				if (ScheduledPlatformStop(arrival.train, GetPlatformInfo(area, arrival.train, ref))) requested_platforms.push_back(ref);
			}
		}
		const bool station_access = !requested_platforms.empty();
		Debug(yapf, 7, "AutoYield area={} incoming={} station={} demand={}", area.id, arrival.train->index, station_access, demand.size());
		if (!station_access && std::find(overtaking_arrivals.begin(), overtaking_arrivals.end(), arrival.train) == overtaking_arrivals.end()) continue;
		/* Normal parking precedes every full-lane exception, including future
		 * capacity pressure. A temporarily occupied departure does not make an
		 * otherwise safe, long-enough parking lane full. Reuse a caller's own
		 * negative parking proof; an exhausted new query cannot justify eviction. */
		if (!station_access && arrival.train != known_no_free_lane) {
			if (FreeLane(area, arrival.train, false, false, std::nullopt, std::nullopt, false) || probe_budget_remaining == 0) continue;
		}
		if (!station_access) {
			uint maximum = 0;
			bool all_equal = true;
			for (const auto &[ref,lane] : area.lanes) for (VehicleID id : lane->occupants) if (const Train *occupant=GetTrain(id); occupant!=nullptr) {
				maximum = std::max<uint>(maximum,occupant->GetDisplayMaxSpeed());
				all_equal &= occupant->GetDisplayMaxSpeed()==arrival.train->GetDisplayMaxSpeed();
			}
			if (maximum!=0 && arrival.train->GetDisplayMaxSpeed()>=maximum && !all_equal) continue;
		}
		/* A free ordinary siding cannot satisfy a scheduled platform stop.
		 * Conversely, a genuinely free suitable platform needs no eviction. */
		if (station_access) {
			bool free_platform = false;
			/* A stopping arrival can park before a temporarily occupied exit.
			 * Require a free, long-enough inbound platform and a legal onward
			 * route, not a currently free departure. Do not evict another train
			 * merely because the shared exit is reserved by passing traffic. */
			for (TraceRestrictRefId ref : requested_platforms) if (FreeLane(area, arrival.train, false, false, ref, std::nullopt, false)) { free_platform = true; break; }
			if (free_platform) continue;
		}
		/* Unclaimed lanes may be physically occupied or reserved by trains
		 * outside this slot. They still consume capacity; do not enrol or
		 * propose departures for those trains. Count usable parking per arrival
		 * so a lane on an unreachable feeder is not a phantom alternative. */
		size_t unavailable = 0;
		{
			AutoYieldPreferenceBudget occupancy_budget;
			for (const auto &[ref, lane] : area.lanes) {
				if (lane->occupants.empty() && !FreeLane(area, arrival.train, true, false, ref)) unavailable++;
			}
		}
		const bool capacity_pressure = demand.size() + unavailable >= 3 && demand.size() > area.lanes.size() - unavailable;
		const Train *yielding_reservation = nullptr;
		const Train *requester_front = arrival.train->GetMovingFront();
		if (!station_access && IsPlainRailTile(requester_front->tile) && arrival.train->force_proceed==TFP_NONE &&
				!arrival.train->flags.Any({VehicleRailFlag::Reversing,VehicleRailFlag::SameDirDecouplePending,VehicleRailFlag::CoupleCheckReversePending}) &&
				ReservedAutoYieldMainAhead(area,arrival.train) &&
				RouteToRole(area,arrival.train,requester_front->tile,requester_front->GetVehicleTrackdir(),area.exit_only?AutoYieldRole::Exit:AutoYieldRole::Siding,
						false,std::nullopt,nullptr,nullptr,nullptr,std::nullopt,false,false,nullptr,true)) {
			/* Read-only joint proposal: the requester's own tentative Main path
			 * must not veto the departure needed to let it leave Main. Only its
			 * reservation is discounted here, never its body or another train.
			 * Native PBS still blocks actual departure until the simulation's
			 * existing pre-fork refresh relinquishes that owned route. */
			yielding_reservation=arrival.train;
		}
		struct TurnoverCandidate {
			const Train *train;
			bool platform;
			bool passenger;
			bool station_demand;
			uint follower_gap; ///< Proven route gap to the nearest threatening follower.
			uint64_t cost;
		};
		/* Resolve the new same-speed rule first. Comparing platform priority
		 * only on ties inside the old passenger-first comparator would create
		 * cycles across three candidates with different speeds. */
		std::map<uint, TurnoverCandidate> candidates_by_speed;
		bool arrival_temporarily_blocked = false;
		struct OccupiedCandidate { TraceRestrictRefId ref; VehicleID id; uint design; };
		std::vector<OccupiedCandidate> occupied;
		std::map<uint,size_t> same_design;
		for (const auto &[ref,lane] : area.lanes) for (VehicleID id : lane->occupants) {
			const Train *train=GetTrain(id);
			const uint design=train!=nullptr?train->GetDisplayMaxSpeed():0;
			occupied.push_back({ref,id,design});
			if(Active(train)&&!DoingStationWork(train))same_design[design]++;
		}
		/* Spend the bounded safety proof on the preferred speed group first.
		 * Physical lane numbering must not let a slower occupant exhaust it. */
		std::sort(occupied.begin(),occupied.end(),[](const OccupiedCandidate&a,const OccupiedCandidate&b){
			return std::tuple{UINT_MAX-a.design,a.id,a.ref}<std::tuple{UINT_MAX-b.design,b.id,b.ref};
		});
		for (const auto &[ref,id,design] : occupied) {
			const Train *v = GetTrain(id);
			const auto reject = [&](const char *reason) { Debug(yapf, 7, "AutoYield area={} turnover={} requester={} rejected={} budget={}", area.id, id, arrival.train->index, reason, probe_budget_remaining); };
			/* Never propose a departure the current station/coupling order cannot
			 * perform, even if a legacy platform's geometry is not provable. */
			if (!Active(v) || !area.detection->IsOccupant(id) || DoingStationWork(v)) { reject("inactive-or-station-work"); continue; }
			/* A reserved platform belongs to a train still approaching its own
			 * scheduled stop until the station order has actually completed. */
			if (v->current_order.IsType(OT_GOTO_STATION) && ScheduledPlatformStop(v, GetPlatformInfo(area, v, ref))) continue;
			const bool for_station = station_access && std::find(requested_platforms.begin(), requested_platforms.end(), ref) != requested_platforms.end();
			if (station_access && !for_station) continue;
			if (!for_station && DetectionSpeed(area,v) < DetectionSpeed(area,arrival.train)) continue;
			uint follower_gap = 0;
			if (capacity_pressure && (same_design[design]>1 || DetectionSpeed(area,v)==DetectionSpeed(area,arrival.train))) {
				AutoYieldPreferenceBudget preference_budget;
				const Train *front = v->GetMovingFront();
				std::optional<TraceRestrictRefId> clearance;
				const auto distance = RouteToRole(area, v, front->tile, front->GetVehicleTrackdir(), AutoYieldRole::Clear, false, std::nullopt, nullptr, nullptr, &clearance);
				uint gap = UINT_MAX;
				if (distance) for (const MainApproach &follower : approaches) {
					if (!FollowingThreat(area, follower, arrival) || !HigherPriority(area, follower.train, v, area.percent) || StationPriorityPaused(area, follower.train)) continue;
					std::optional<TraceRestrictRefId> following_clearance;
					const auto following_distance = MergeClearanceDistance(area,follower,&following_clearance);
					if (following_distance && following_clearance==clearance && *following_distance > *distance) gap = std::min(gap, *following_distance - *distance);
				}
				if (gap != UINT_MAX) follower_gap = gap;
			}
			/* Capacity pressure may safely clear an equal-speed front train
			 * before a queue stops. The existing passing, station-work and exact
			 * replacement-route checks below remain mandatory. */
			if (!for_station && DetectionSpeed(area,v) == DetectionSpeed(area,arrival.train) && follower_gap == 0 && !EqualSpeedTurnover(area,v,arrival,approaches)) continue;
			/* A faster train already ahead of this arrival must finish passing
			 * first. Missing approaches include trains past Main but whose tail
			 * has not cleared; distant/budget-limited followers remain behind. */
			bool passing = false;
			for (VehicleID other_id : area.detection->occupants) {
				const Train *other = GetTrain(other_id);
				/* This arrival cannot pass before entering its occupied platform.
				 * Exempt only that exact requester, never another passing train. */
				if (for_station && other == arrival.train) continue;
				if (!Active(other) ||
						!HigherPriority(area, other, v, area.percent) || StationPriorityPaused(area, other)) continue;
				if (DetectionSpeed(area, other) == DetectionSpeed(area, v) && !ClosePassenger(area, other, v)) continue;
				if (ProvenIndependentExits(area, other, v)) continue;
				/* A short priority stop must not be bypassed through the separate
				 * turnover exception while MustWait deliberately protects it. */
				if (ServingPlatform(area, other)) { passing = true; break; }
				if (HasLaneClaim(area, other)) continue;
				auto position = std::find_if(approaches.begin(), approaches.end(), [other](const MainApproach &a) { return a.train == other; });
				if (position == approaches.end()) { passing = true; break; }
				if (position->main && arrival.main && position->main == arrival.main && !position->clearance_only) {
					if (position->distance <= arrival.distance) { passing = true; break; }
				} else {
					const auto other_distance = MergeClearanceDistance(area, *position), arrival_distance = MergeClearanceDistance(area, arrival);
					if (!other_distance || !arrival_distance || *other_distance <= *arrival_distance) { passing = true; break; }
				}
			}
			if (passing) { reject("passing-priority-train"); continue; }
			if (!RouteToRole(area, v, v->GetMovingFront()->tile, v->GetMovingFront()->GetVehicleTrackdir(), AutoYieldRole::Clear, true,
					std::nullopt,nullptr,nullptr,nullptr,std::nullopt,false,false,yielding_reservation)) {
				/* Once the front passes Clear it cannot reach that marker again.
				 * Keep the incoming train's turn while the departing body still
				 * spans it; the existing tail event owns release of the live claim. */
				bool clearing = false;
				for (const Train *part = v; part != nullptr && probe_budget_remaining > 0; part = part->Next()) {
					probe_budget_remaining--;
					if (AtRole(area, part->tile, part->GetVehicleTrackdir(), AutoYieldRole::Clear)) { clearing = true; break; }
				}
				if (!clearing) { reject("blocked-departure"); continue; }
			}
			const Binding *binding = FindBinding(area, ref);
			if (binding == nullptr) continue;
			/* Prove this specific arrival can use this specific siding after v
			 * leaves. Ignore only v's physical occupancy and reservations in this
			 * hypothetical check; every other train and restriction still applies.
			 * Neither reservations nor slots are changed by this proposal. */
			const auto length = LaneParkingLength(area, arrival.train, *binding, v);
			if (!length || *length < arrival.train->gcache.cached_total_length + TILE_SIZE) { reject("replacement-length"); continue; }
			if (!RouteToRole(area, arrival.train, arrival.train->GetMovingFront()->tile, arrival.train->GetMovingFront()->GetVehicleTrackdir(),
					area.exit_only ? AutoYieldRole::Exit : AutoYieldRole::Siding, true, ref, nullptr, v)) {
				/* The closest arrival may still be behind a train which should
				 * continue Main first. Do not give that front train the released
				 * slot just because the requester's approach is occupied now. */
				arrival_temporarily_blocked |= RouteToRole(area, arrival.train, arrival.train->GetMovingFront()->tile,
						arrival.train->GetMovingFront()->GetVehicleTrackdir(), area.exit_only ? AutoYieldRole::Exit : AutoYieldRole::Siding,
						false, ref, nullptr, nullptr, nullptr, std::nullopt, false, true).has_value();
				reject("replacement-ingress"); continue;
			}
			/* Getting in is insufficient: this train must also be able to leave
			 * through the shared clearance route with its own rail compatibility. */
			if (!RouteToRole(area, arrival.train, GetTraceRestrictRefIdTileIndex(ref), binding->td,
					AutoYieldRole::Clear, true, std::nullopt, nullptr, v)) { reject("replacement-departure"); continue; }
			const auto turnover_cost = TurnoverCost(area, v, arrival.train, for_station, ref,yielding_reservation);
			if (!turnover_cost) { reject("arrival-risk-or-unknown"); continue; }
			const PlatformInfo platform = GetPlatformInfo(area, v, ref);
			bool station_demand = for_station;
			if (!station_demand && platform.marked && platform.known && platform.exit) {
				for (const MainApproach &following : approaches) {
					const Train *passenger = following.train;
					if (HasLaneClaim(area, passenger) || !PassengerService(passenger) ||
							!passenger->current_order.IsType(OT_GOTO_STATION)) continue;
					const bool stopping = std::any_of(platform.stations.begin(), platform.stations.end(), [passenger](StationID station) {
						return passenger->current_order.ShouldStopAtStation(passenger, station, false);
					});
					if (!stopping) continue;
					const Train *front = passenger->GetMovingFront();
					/* The siding fork can precede Main: prove access from the
					 * actual front rather than assuming Main can reach every lane. */
					if (RouteToRole(area, passenger, front->tile, front->GetVehicleTrackdir(), AutoYieldRole::Exit, false, platform.exit)) {
						station_demand = true;
						break;
					}
				}
			}
			const TurnoverCandidate candidate{v, platform.marked, PassengerService(v), station_demand, follower_gap, *turnover_cost};
			Debug(yapf, 7, "AutoYield area={} turnover={} requester={} accepted=true station={} speed={} cost={}", area.id, v->index, arrival.train->index, station_demand, DetectionSpeed(area, v), *turnover_cost);
			auto [entry, inserted] = candidates_by_speed.try_emplace(v->GetDisplayMaxSpeed(), candidate);
			const auto rank = [](const TurnoverCandidate &c) {
				return std::tuple{!c.station_demand, UINT_MAX - c.follower_gap, !c.platform, !c.passenger, c.cost, c.train->index};
			};
			if (!inserted && rank(candidate) < rank(entry->second)) entry->second = candidate;
		}
		/* Rank only departures which passed the actual route and risk checks.
		 * Normal turnover chooses design speed before distance/class/cost;
		 * required passenger platform access retains its separate policy. */
		const Train *best = nullptr, *best_platform = nullptr;
		std::tuple<uint, uint, bool, bool, uint64_t, VehicleID> best_rank;
		std::tuple<uint, bool, VehicleID> best_platform_rank;
		for (const auto &[speed, candidate] : candidates_by_speed) {
			/* A following scheduled passenger needs a platform, not an ordinary
			 * siding. Clear its fastest eligible platform occupant first; use
			 * passenger service only as a speed tie-break, never to skip work. */
			if (candidate.station_demand) {
				const auto rank = std::tuple{UINT_MAX - DetectionSpeed(area,candidate.train), !candidate.passenger, candidate.train->index};
				if (best_platform == nullptr || rank < best_platform_rank) {
					best_platform = candidate.train;
					best_platform_rank = rank;
				}
			}
			const auto rank = std::tuple{UINT_MAX - speed, UINT_MAX - candidate.follower_gap, !candidate.platform, !candidate.passenger, candidate.cost, candidate.train->index};
			if (best == nullptr || rank < best_rank) {
				best = candidate.train;
				best_rank = rank;
			}
		}
		if (best_platform != nullptr) return {best_platform, arrival.train};
		if (best != nullptr) return {best, arrival.train};
		if (!station_access && arrival_temporarily_blocked) return {};
		if (probe_budget_remaining == 0) return {};
	}
	return {};
}
/** A train already selected to leave need not detour through its siding.
 * Keep this a preference, with positive admission/clearance proofs; ordinary
 * orders and restrictions still decide whether the main can reach its target. */
bool PreferMainForDeparture(const Area &area, const Train *v)
{
	if (!HasLaneClaim(area, v) || DoingStationWork(v) ||
			!(v->current_order.IsType(OT_GOTO_DEPOT) || v->current_order.IsType(OT_GOTO_STATION))) return false;
	for (const auto &[ref, lane] : area.lanes) {
		if (!lane->IsOccupant(v->index)) continue;
		const PlatformInfo platform = GetPlatformInfo(area, v, ref);
		if ((platform.marked || v->current_order.IsType(OT_GOTO_STATION)) && RequiresPlatform(v, platform)) return false;
	}
	/* Most claimed trains are already inside their siding. Exclude them
	 * cheaply before repeating the bounded turnover proof. */
	const Train *front = v->GetMovingFront();
	if (!RouteToRole(area, v, front->tile, front->GetVehicleTrackdir(), AutoYieldRole::Main, true,
			std::nullopt, nullptr, nullptr, nullptr, std::nullopt, true)) return false;
	return Evacuation(area).departing == v;
}

void Acquire(TraceRestrictSlot *slot, const Train *v, const TraceRestrictProgramInput &input, TraceRestrictProgramResult &out)
{
	if (input.permitted_slot_operations & TRPISP_ACQUIRE_TEMP_STATE) {
		if (!slot->OccupyUsingTemporaryState(v->index, TraceRestrictSlotTemporaryState::GetCurrent())) out.flags |= TRPRF_WAIT_AT_PBS;
	} else if (input.permitted_slot_operations & (TRPISP_ACQUIRE | TRPISP_PBS_RES_END_ACQUIRE)) {
		if (!slot->Occupy(v)) out.flags |= TRPRF_WAIT_AT_PBS;
	}
}

/** Record only positively established conflicts after committing the siding
 * claim. The temporary PBS state is for the querying vehicle only: it must
 * never be used to index a different vehicle (the target). */
void RememberYieldTargets(const Area &area, const Train *v, TraceRestrictRefId ref, const TraceRestrictProgramInput &input, TraceRestrictProgramResult &out)
{
	if ((input.permitted_slot_operations & TRPISP_ACQUIRE_TEMP_STATE) || !(input.permitted_slot_operations & (TRPISP_ACQUIRE | TRPISP_PBS_RES_END_ACQUIRE))) return;
	auto lane = area.lanes.find(ref), targets = area.targets.find(ref);
	if (lane == area.lanes.end() || targets == area.targets.end() || !lane->second->IsOccupant(v->index)) return;
	const bool holding = YieldWaitInProgress(area, v);
	std::optional<std::vector<MainApproach>> approaches;
	auto participants = Participants(area, v);
	/* Fair, reproducible progress when one unresolved route uses the budget.
	 * Saved targets are skipped; new arrivals are not always examined last. */
	if (holding && !participants.empty()) std::rotate(participants.begin(),
			participants.begin() + (((_state_ticks.base() >> 3) + v->index.base()) % participants.size()), participants.end());
	for (VehicleID id : participants) {
		const Train *other = GetTrain(id);
		if (other == v || !Active(other) || targets->second->IsOccupant(id) || !HigherPriority(area, other, v, area.percent) || StationPriorityPaused(area, other)) continue;
		if (DetectionSpeed(area, other) == DetectionSpeed(area, v) && !ClosePassenger(area, other, v)) continue;
		/* Preserve the same positive following conflict that justified leaving
		 * Main. Requiring a second, full-outlet proof here can lose that decision
		 * at a station target or a distant fork before the lane is even reached. */
		bool admitted = false;
		if (!holding) {
			if (!approaches) approaches = GetMainApproaches(area, v);
			auto own = std::find_if(approaches->begin(), approaches->end(), [v](const MainApproach &a) { return a.train == v; });
			auto following = std::find_if(approaches->begin(), approaches->end(), [other](const MainApproach &a) { return a.train == other; });
			admitted = own != approaches->end() && following != approaches->end() && FollowingThreat(area, *following, *own);
			/* An intermediate PBS stop may make Exit the first committing
			 * marker only after the head has passed the inbound Main branch.
			 * Reuse the already proved follower before an optional long route
			 * footprint spends the budget. A committed lane's unique forward
			 * outlet supplies its comparison point, not a fictitious Main. */
			if (!admitted && own == approaches->end() && following != approaches->end()) {
				const auto own_outlet = ProveOutlet(area,v);
				if (own_outlet.complete && own_outlet.ref) {
					MainApproach waiting{v,UINT_MAX,std::nullopt,true};
					admitted = FollowingThreat(area,*following,waiting) && !CanClearBefore(area,v,other);
				}
			}
		}
		const auto conflict = admitted ? YieldConflict::SharedDirectedResource : ClassifyYieldConflict(area, v, other);
		if (holding && conflict == YieldConflict::Unknown && area.main_count == 1 && area.clear_count == 1 && area.detection->IsOccupant(id)) {
			/* A late train can still be hundreds of tiles before the fork. A
			 * truncated footprint is not the only positive evidence: use the
			 * existing Main approach plus its unique common outlet. This narrow
			 * fallback cannot infer a multi-outlet choice or skip a prior stop. */
			const auto own_outlet = ProveOutlet(area, v);
			if (own_outlet.complete && own_outlet.ref) {
				std::optional<TraceRestrictRefId> main;
				const Train *front = other->GetMovingFront();
				if (auto distance = RouteToRole(area, other, front->tile, front->GetVehicleTrackdir(), AutoYieldRole::Main, false, std::nullopt, nullptr, nullptr, &main)) {
					MainApproach incoming{other, *distance, main}, waiting{v, UINT_MAX, std::nullopt, true};
					if (main) {
						const Binding *binding = FindBinding(area,*main);
						if (binding != nullptr) incoming.target_before_main = MainTargetProvenUnreachable(other,GetTraceRestrictRefIdTileIndex(*main),binding->td);
					}
					admitted = FollowingThreat(area, incoming, waiting);
				}
			}
		}
		if (admitted || ((conflict == YieldConflict::SharedDirectedResource || conflict == YieldConflict::CrossingResource) && (holding || !CanClearBefore(area, v, other)))) Acquire(targets->second, other, input, out);
	}
}

void AcquireLane(const Area &area, TraceRestrictSlot *lane, const Train *v, const TraceRestrictProgramInput &input, TraceRestrictProgramResult &out)
{
	if (lane->occupants.empty() && !(input.permitted_slot_operations & TRPISP_ACQUIRE_TEMP_STATE) &&
			(input.permitted_slot_operations & (TRPISP_ACQUIRE | TRPISP_PBS_RES_END_ACQUIRE))) {
		/* Vehicle deletion removes indexed occupants directly. A subsequent
		 * occupant must never inherit the deleted train's outstanding targets. */
		auto targets = area.targets.find(lane->automatic_yield_signal);
		if (targets != area.targets.end()) targets->second->Clear();
	}
	Acquire(lane, v, input, out);
	RememberYieldTargets(area, v, lane->automatic_yield_signal, input, out);
}
} // namespace

bool IsAutoYieldInstruction(TraceRestrictItemType type)
{
	return (type >= TRIT_COND_AUTO_YIELD_FASTER && type <= TRIT_COND_AUTO_YIELD_LEAVE) || type == TRIT_AUTO_YIELD_AREA || type == TRIT_AUTO_YIELD_CLEAR;
}
void MigrateLegacyAutoYieldSpeedCaps(TraceRestrictProgram &program)
{
	for (auto iter : program.IterateInstructionsMutable()) {
		const auto item = iter.Instruction();
		if (item.GetType() == TRIT_AUTO_YIELD_AREA && item.GetAuxField() == to_underlying(AutoYieldRole::Approach)) {
			/* v1-v7 low bits were a removed detection distance, never a speed. */
			iter.SecondaryRef() &= 0xFFFF0000u;
		}
	}
}
void InvalidateAutoYieldAreas() { dirty = true; }

/** Signal direction is part of marker identity. Refresh at the synchronized
 * command boundary, before native re-reservation; a local YAPF cache generation
 * is neither saved nor necessarily equal on a joining client. */
void AutoYieldSignalChanged(TileIndex tile, Track track)
{
	bool relevant = dirty;
	if (const auto *program = GetExistingTraceRestrictProgram(tile, track)) {
		for (const auto iter : program->IterateInstructions()) {
			if (IsAutoYieldInstruction(iter.Instruction().GetType())) { relevant = true; break; }
		}
	}
	if (relevant) { InvalidateAutoYieldAreas(); UpdateAutoYieldAreas(); }
}
void ResetAutoYieldAreas() { areas.clear(); coordination_index.clear(); dirty = true; }

void AutoYieldSlotVacated(TraceRestrictSlotID slot_id, VehicleID vehicle)
{
	const TraceRestrictSlot *slot = TraceRestrictSlot::GetIfValid(slot_id);
	if (slot == nullptr || slot->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets)) return;
	if (slot->automatic_yield_area != 0) {
		if (slot->automatic_yield_signal == AUTO_YIELD_PENDING_REF) return;
		if (!dirty) {
			auto area = areas.find({CompanyID{slot->owner}, uint16_t(slot->automatic_yield_area - 1)});
			if (area != areas.end()) {
				auto targets = area->second.targets.find(slot->automatic_yield_signal);
				if (targets != area->second.targets.end()) targets->second->Clear();
			}
			return;
		}
		/* An actual lane cancellation owns the lifetime of its obligations.
		 * Temporary PBS rollback restores both slots without this callback. */
		for (TraceRestrictSlot *targets : TraceRestrictSlot::Iterate()) {
			if (targets->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets) && targets->owner == slot->owner &&
					targets->automatic_yield_area == slot->automatic_yield_area && targets->automatic_yield_signal == slot->automatic_yield_signal) targets->Clear();
		}
		return;
	}
	if (const Train *train = GetTrain(vehicle); train != nullptr && train->early_route.size() > 1 && train->early_route[1] == slot_id.base()) {
		const_cast<Train *>(train)->early_route.clear(); // Actual detector cancellation/tail-clear, never a dry run.
	}
	if (const Train *train = GetTrain(vehicle); train != nullptr && train->early_route_area == slot_id.base()) const_cast<Train *>(train)->early_route_area = UINT16_MAX;
	if (dirty) {
		/* Edits can invalidate cached pointers. Cancellation still releases its
		 * saved lane claims before the next registry rebuild. */
		for (TraceRestrictSlot *lane : TraceRestrictSlot::Iterate()) {
			if (lane->automatic_yield_area == slot_id.base() + 1 && lane->owner == slot->owner) lane->VacateByID(vehicle);
		}
		return;
	}
	auto area = areas.find({CompanyID{slot->owner}, slot_id.base()});
	if (area == areas.end()) return;
	if (area->second.pending != nullptr) area->second.pending->VacateByID(vehicle);
	for (const auto &[ref, lane] : area->second.lanes) lane->VacateByID(vehicle);
	for (const auto &[ref, targets] : area->second.targets) targets->VacateByID(vehicle);
	if (const Train *v = GetTrain(vehicle); v != nullptr && !RegisteredForYield(v)) {
		std::vector<TraceRestrictSlotID> memberships;
		TraceRestrictGetVehicleSlots(vehicle, memberships);
		for (auto id : memberships) {
			auto *target = TraceRestrictSlot::GetIfValid(id);
			if (target != nullptr && target->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets)) target->VacateByID(vehicle);
		}
	}
}

void UpdateAutoYieldSlotSignals(const TraceRestrictSlot *slot)
{
	if (dirty) return;
	const uint16_t id = slot->automatic_yield_area == 0 ? slot->index.base() : slot->automatic_yield_area - 1;
	auto area = areas.find({CompanyID{slot->owner}, id});
	if (area == areas.end()) return;
	for (const SignalReference &signal : area->second.signal_dependants) {
		if (IsValidTile(signal.tile) && ((IsPlainRailTile(signal.tile) && HasSignalOnTrack(signal.tile, signal.track)) ||
				(IsRailTunnelBridgeTile(signal.tile) && IsTunnelBridgeWithSignalSimulation(signal.tile) && IsTrackAcrossTunnelBridge(signal.tile, signal.track)))) {
			AddTrackToSignalBuffer(signal.tile, signal.track, GetTileOwner(signal.tile));
		}
	}
	UpdateSignalsInBuffer();
}

void UpdateAutoYieldAreas()
{
	multiple_outlets = false;
	areas.clear();
	coordination_index.clear();
	std::map<AreaKey, std::vector<SignalReference>> condition_dependants;
	for (const auto &[ref, mapping] : _tracerestrictprogram_mapping) {
		const TileIndex tile = GetTraceRestrictRefIdTileIndex(ref);
		const Track track = GetTraceRestrictRefIdTrack(ref);
		if (!IsValidTile(tile)) continue;
		Trackdir td = TrackToTrackdir(track);
		bool directional;
		if (IsPlainRailTile(tile) && HasSignalOnTrack(tile, track)) {
			directional = HasSignalOnTrackdir(tile, td) != HasSignalOnTrackdir(tile, ReverseTrackdir(td));
			if (!HasSignalOnTrackdir(tile, td)) td = ReverseTrackdir(td);
		} else if (IsRailTunnelBridgeTile(tile) && IsTunnelBridgeWithSignalSimulation(tile) && IsTrackAcrossTunnelBridge(tile, track)) {
			/* The restriction belongs to the ramp's simulated entrance/exit,
			 * not an ordinary rail signal. Keep bidirectional markers inert,
			 * just as on ordinary two-sided signals. */
			directional = !IsTunnelBridgeSignalSimulationBidirectional(tile);
			td = IsTunnelBridgeSignalSimulationEntrance(tile) ? GetTunnelBridgeEntranceTrackdir(tile) : GetTunnelBridgeExitTrackdir(tile);
		} else {
			continue;
		}
		const CompanyID owner{GetTileOwner(tile)};
		const TraceRestrictProgram *program = TraceRestrictProgram::Get(mapping.program_id);
		for (auto iter : program->IterateInstructions()) {
			const auto item = iter.Instruction();
			if (!IsAutoYieldInstruction(item.GetType())) continue;
			TraceRestrictSlot *slot = TraceRestrictSlot::GetIfValid(item.GetValue());
			if (slot == nullptr || slot->automatic_yield_area != 0 || slot->vehicle_type != VehicleType::Train || !slot->IsUsableByOwner(owner)) continue;
			const AreaKey key{CompanyID{slot->owner}, slot->index.base()};
			condition_dependants[key].emplace_back(tile, track);
			if (item.IsConditional() || !directional) continue;
			/* Markers require one direction; conditions can be used on either side. */
			Area &area = areas[key];
			area.owner = CompanyID{slot->owner};
			area.id = slot->index.base();
			area.detection = slot;
			const AutoYieldRole role = item.GetType() == TRIT_AUTO_YIELD_CLEAR ? AutoYieldRole::Clear : static_cast<AutoYieldRole>(item.GetAuxField());
			const bool platform = IsAutoYieldLaneRole(role) && (iter.Secondary() & AUTO_YIELD_PLATFORM);
			/* Clear is a single-word instruction. Its successor (or memory past
			 * the program) is not configuration and must never enter the area
			 * signature. Registration is meaningful only for Approach. */
			const auto registration = role == AutoYieldRole::Approach ? GetAutoYieldRegistration(iter.Secondary()) : AutoYieldRegistration::Reservation;
			area.bindings.push_back({ref, role, td, platform, role == AutoYieldRole::Approach ? uint16_t(iter.Secondary() & 0xFFFF) : uint16_t{0}, registration});
			area.has_platform |= platform;
			if (role == AutoYieldRole::Approach) area.percent = std::max<uint16_t>(area.percent, GetAutoYieldPercent(iter.Secondary()));
		}
	}
	for (auto &entry : areas) {
		Area &area = entry.second;
		std::sort(area.bindings.begin(), area.bindings.end(), [](const Binding &a, const Binding &b) {
			return std::tie(a.ref, a.role, a.td) < std::tie(b.ref, b.role, b.td);
		});
	}
	for (auto &entry : condition_dependants) {
		auto &dependants = entry.second;
		std::sort(dependants.begin(), dependants.end());
		dependants.erase(std::unique(dependants.begin(), dependants.end()), dependants.end());
	}
	std::map<std::tuple<CompanyID, uint16_t, uint32_t>, TraceRestrictSlot *> resources;
	decltype(resources) target_resources;
	for (TraceRestrictSlot *slot : TraceRestrictSlot::Iterate()) {
		if (slot->automatic_yield_area == 0) continue;
		auto &registry = slot->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets) ? target_resources : resources;
		registry[{CompanyID{slot->owner}, slot->automatic_yield_area, slot->automatic_yield_signal}] = slot;
	}
	for (auto &[key, area] : areas) {
		std::set<uint32_t> refs;
		/* Zero is a valid user slot ID. Internal lanes store ID + 1 to keep
		 * the existing zero == ordinary slot representation on disk. */
		const uint16_t resource_area = area.id + 1;
		std::set<std::pair<AutoYieldRole, TraceRestrictRefId>> unique_bindings;
		std::set<TraceRestrictRefId> unique_signals;
		uint approach_count = 0, clear_count = 0, exit_count = 0, main_count = 0;
		bool valid_bindings = true;
		for (const Binding &b : area.bindings) {
			if (!unique_bindings.emplace(b.role, b.ref).second || !unique_signals.emplace(b.ref).second) valid_bindings = false;
			if (b.role == AutoYieldRole::Siding) refs.insert(b.ref);
			approach_count += b.role == AutoYieldRole::Approach;
			clear_count += b.role == AutoYieldRole::Clear;
			exit_count += b.role == AutoYieldRole::Exit;
			main_count += b.role == AutoYieldRole::Main;
		}
		area.main_count = main_count;
		area.clear_count = clear_count;
		area.exit_only = refs.empty();
		if (area.exit_only) for (const Binding &b : area.bindings) if (b.role == AutoYieldRole::Exit) refs.insert(b.ref);
		area.signal_dependants = std::move(condition_dependants[key]);
		/* Invalid/incomplete layouts are inert, including resource allocation. */
		if (!valid_bindings || approach_count == 0 || approach_count > MAX_APPROACH_MARKERS || clear_count == 0 || clear_count > MAX_MAIN_MARKERS || exit_count == 0 || main_count == 0 || main_count > MAX_MAIN_MARKERS || refs.empty() || refs.size() > MAX_SIDINGS) continue;
		size_t missing = !resources.contains({area.owner, resource_area, AUTO_YIELD_PENDING_REF});
		for (uint32_t ref : refs) {
			missing += !resources.contains({area.owner, resource_area, ref});
			missing += !target_resources.contains({area.owner, resource_area, ref});
		}
		if (!TraceRestrictSlot::CanAllocateItem(missing)) continue;
		TraceRestrictSlot *&pending = resources[{area.owner, resource_area, AUTO_YIELD_PENDING_REF}];
		if (pending == nullptr) {
			pending = TraceRestrictSlot::Create(area.owner);
			pending->automatic_yield_area = resource_area;
			pending->automatic_yield_signal = AUTO_YIELD_PENDING_REF;
			pending->max_occupancy = UINT32_MAX;
			pending->name = "Automatic yielding pending " + std::to_string(area.id);
		}
		area.pending = pending;
		for (uint32_t ref : refs) {
			auto resource_key = std::make_tuple(area.owner, resource_area, ref);
			TraceRestrictSlot *&slot = resources[resource_key];
			if (slot == nullptr) {
				slot = TraceRestrictSlot::Create(area.owner);
				slot->automatic_yield_area = resource_area; slot->automatic_yield_signal = ref;
				slot->max_occupancy = 1;
				slot->name = "Automatic yielding " + std::to_string(area.id) + ":" + std::to_string(ref);
			}
			area.lanes[ref] = slot;
			TraceRestrictSlot *&targets = target_resources[resource_key];
			if (targets == nullptr) {
				targets = TraceRestrictSlot::Create(area.owner);
				targets->automatic_yield_area = resource_area; targets->automatic_yield_signal = ref;
				targets->flags.Set(TraceRestrictSlot::Flag::AutoYieldTargets);
				targets->max_occupancy = MAX_PARTICIPANTS;
				targets->name = "Automatic yielding targets " + std::to_string(area.id) + ":" + std::to_string(ref);
			}
			area.targets[ref] = targets;
			if (slot->occupants.empty()) targets->Clear();
		}
		area.enabled = true;
		multiple_outlets |= area.clear_count > 1;
		for (const auto &binding : area.bindings) {
			if (binding.role == AutoYieldRole::Approach) continue;
			const TileIndex tile = GetTraceRestrictRefIdTileIndex(binding.ref);
			const uint x = TileX(tile) / 128, y = TileY(tile) / 128;
			for (uint cx = x > 2 ? x - 2 : 0; cx <= x + 2; cx++) {
				for (uint cy = y > 2 ? y - 2 : 0; cy <= y + 2; cy++) coordination_index[{cx, cy}].insert(key);
			}
		}
	}
	std::vector<TraceRestrictSlot *> unused;
	for (const auto &[key, slot] : target_resources) {
		auto a = areas.find({std::get<0>(key), uint16_t(std::get<1>(key) - 1)});
		if (a == areas.end() || !a->second.targets.contains(std::get<2>(key))) unused.push_back(slot);
	}
	for (const auto &[key, slot] : resources) {
		auto a = areas.find({std::get<0>(key), uint16_t(std::get<1>(key) - 1)});
		if (a == areas.end() || (a->second.pending != slot && !a->second.lanes.contains(std::get<2>(key)))) unused.push_back(slot);
	}
	for (TraceRestrictSlot *slot : unused) {
		if (slot->automatic_yield_signal == AUTO_YIELD_PENDING_REF) {
			if (TraceRestrictSlot *source = TraceRestrictSlot::GetIfValid(slot->automatic_yield_area - 1)) {
				const auto occupants = slot->occupants;
				for (VehicleID id : occupants) source->VacateByID(id);
			}
		}
		TraceRestrictRemoveSlotID(slot->index); delete slot;
	}
	dirty = false;
}

/** Version 1 stored a numeric area per company and hid its detection slot.
 * Reuse that exact slot (and its occupants), making it visible and replacing
 * every numeric operand with the saved slot ID. No vehicle identity is guessed. */
bool MigrateAutoYieldAreasToSlots()
{
	std::map<AreaKey, TraceRestrictSlot *> detection;
	std::vector<TraceRestrictSlot *> legacy;
	for (TraceRestrictSlot *slot : TraceRestrictSlot::Iterate()) {
		if (slot->automatic_yield_area == 0) continue;
		legacy.push_back(slot);
		if (slot->automatic_yield_signal == AUTO_YIELD_DETECTION_REF) {
			detection[{CompanyID{slot->owner}, slot->automatic_yield_area}] = slot;
		}
	}
	std::map<TraceRestrictProgramID, CompanyID> owners;
	for (const auto &[ref, mapping] : _tracerestrictprogram_mapping) {
		owners.try_emplace(mapping.program_id, CompanyID{GetTileOwner(GetTraceRestrictRefIdTileIndex(ref))});
	}
	for (uint8_t c = 0; c < MAX_COMPANIES; c++) {
		for (const auto &backup : _tracerestrict_backups[CompanyID{c}].programs) owners.try_emplace(backup.program_id, CompanyID{c});
	}
	for (TraceRestrictProgram *program : TraceRestrictProgram::Iterate()) {
		for (auto iter : program->IterateInstructionsMutable()) {
			auto item = iter.InstructionRef();
			if (!IsAutoYieldInstruction(item.GetType())) continue;
			auto owner = owners.find(program->index);
			if (owner == owners.end() || item.GetValue() == 0) {
				item.SetValue(INVALID_TRACE_RESTRICT_SLOT_ID);
				continue;
			}
			const AreaKey key{owner->second, item.GetValue()};
			TraceRestrictSlot *&slot = detection[key];
			if (slot == nullptr) {
				if (!TraceRestrictSlot::CanAllocateItem()) return false;
				slot = TraceRestrictSlot::Create(owner->second);
				slot->max_occupancy = UINT32_MAX;
				slot->name = "Automatic yielding " + std::to_string(key.first.base()) + ":" + std::to_string(key.second);
			}
			item.SetValue(slot->index);
		}
		if (program->Validate().Failed()) return false;
	}
	/* Rewrite lane keys before exposing the old detection slots. */
	for (TraceRestrictSlot *slot : legacy) {
		if (slot->automatic_yield_signal == AUTO_YIELD_DETECTION_REF) continue;
		auto source = detection.find({CompanyID{slot->owner}, slot->automatic_yield_area});
		if (source == detection.end() || source->second == nullptr) return false;
		slot->automatic_yield_area = source->second->index.base() + 1;
	}
	for (const auto &[key, slot] : detection) {
		slot->automatic_yield_area = 0;
		slot->name = "Automatic yielding " + std::to_string(key.first.base()) + ":" + std::to_string(key.second);
	}
	InvalidateAutoYieldAreas();
	return true;
}

void AutoYieldSettingsChanged()
{
	InvalidateAutoYieldAreas();
	UpdateAutoYieldAreas();
	if (!_settings_game.vehicle.automatic_yield) {
		for (auto &[key, area] : areas) {
			if (area.pending != nullptr) {
				const auto occupants = area.pending->occupants;
				for (VehicleID id : occupants) area.detection->VacateByID(id);
			}
			for (const auto &[ref, lane] : area.lanes) lane->Clear();
		}
	}
	for (const auto &[key, area] : areas) UpdateAutoYieldSlotSignals(area.detection);
}

/** Reconsider a committed branch while an eligible alternative is still
 * reachable. A safe waiting tile before Main can already be beyond the fork,
 * so waiting for Main's next callback is too late. */
static void RefreshAutoYieldMainReservation(const Area &area, Train *v)
{
	if (!Active(v) || v->force_proceed != TFP_NONE ||
			v->flags.Any({VehicleRailFlag::Reversing, VehicleRailFlag::SameDirDecouplePending, VehicleRailFlag::CoupleCheckReversePending}) ||
			!(v->current_order.IsType(OT_GOTO_STATION) || v->current_order.IsType(OT_GOTO_WAYPOINT) || v->current_order.IsType(OT_GOTO_DEPOT))) return;
	const Train *front = v->GetMovingFront();
	if (!IsPlainRailTile(front->tile)) return;
	AutoYieldProbeBudget budget;
	budget.UseArea(area);
	const PBSTileInfo end = FollowTrainReservation(v, nullptr, FollowTrainReservationFlag::OkayUnused);
	Debug(yapf, 7, "AutoYield area={} train={} refresh-front={} end={} end-direction={}", area.id, v->index, front->tile, end.tile, uint(end.trackdir));
	if (!IsValidTile(end.tile) || end.tile == front->tile || !IsValidTrackdir(end.trackdir)) return;
	/* Only a positively owned straight continuation qualifies. Existing
	 * siding/platform reservations, opposing routes and exhausted proofs
	 * are left alone. Never reconsider a branch the head has already passed. */
	const Train *owner = GetTrainForReservation(end.tile, TrackdirToTrack(end.trackdir));
	if (owner == nullptr || owner->Primary() != v->Primary()) return;
	if (HasLaneClaim(area, v)) {
		std::optional<TraceRestrictRefId> exit;
		if (ReservedAutoYieldMainAhead(area, v, &exit, AutoYieldRole::Exit) && GetTraceRestrictRefIdTileIndex(*exit) != front->tile) {
			const Binding *binding = FindBinding(area, *exit);
			if (binding != nullptr) {
				TraceRestrictProgramInput input(GetTraceRestrictRefIdTileIndex(*exit), binding->td, nullptr, nullptr);
				TraceRestrictProgramResult result;
				/* A committed lane/owned forward route may predate the fix or a
				 * newly registered follower. Recheck only our own departure path. */
				input.permitted_slot_operations = TRPISP_ACQUIRE;
				for (const auto &[ref, lane] : area.lanes) if (lane->IsOccupant(v->index)) RememberYieldTargets(area, v, ref, input, result);
				input.permitted_slot_operations = TRPISP_NONE;
				GetExistingTraceRestrictProgram(input.tile, TrackdirToTrack(input.trackdir))->Execute(v, input, result);
				if (result.flags & TRPRF_WAIT_AT_PBS) {
					FreeTrainTrackReservation(v);
					TryPathReserve(v, true, false);
					return;
				}
			}
		}
		/* Reconsider an old siding commitment only before the fork and only
		 * for the selected safe departure. Keep its lane claim until normal
		 * tail clearance, so no follower can steal a still occupied route. */
		if (RouteToRole(area, v, end.tile, end.trackdir, AutoYieldRole::Main, false) || !PreferMainForDeparture(area, v)) return;
		FreeTrainTrackReservation(v);
		TryPathReserve(v, true, false);
		return;
	}
	std::optional<TraceRestrictRefId> reserved_main;
	bool reserved_siding = false;
	if (!ReservedAutoYieldMainAhead(area, v, &reserved_main)) {
		/* A reservation ending before Main still needs the older geometric
		 * proof. Once the owned prefix already passes Main, searching from its
		 * far endpoint can miss Main or loop through a depot to a remote siding. */
		if (!RouteToRole(area, v, end.tile, end.trackdir, AutoYieldRole::Main, false, std::nullopt, nullptr, nullptr, &reserved_main)) return;
		/* Reachability from the endpoint is not a siding commitment: before
		 * the fork both Main and Exit can be reachable. Inspect only our own
		 * continuous reservation for an actual controlled siding marker. */
		reserved_siding = ReservedAutoYieldMainAhead(area, v, nullptr, AutoYieldRole::Siding) ||
				ReservedAutoYieldMainAhead(area, v, nullptr, AutoYieldRole::Exit);
	}
	const auto approaches = GetMainApproaches(area, v, reserved_main);
	/* An exit reachable beyond the reservation endpoint is not proof that
	 * the owned prefix is already a siding. A detour may first borrow a fast
	 * feeder and only then turn back into that exit. Recheck it before merging. */
	if (reserved_siding && std::none_of(approaches.begin(), approaches.end(), [v](const MainApproach &a) {
		return a.train == v && a.candidate_detour;
	})) return;
	/* Reuse the actual free-ingress proofs from allocation; repeating them
	 * here can exhaust the budget after a correctly recognised long approach. */
	bool parking_free = false;
	if (!NeedsSiding(area, v, &approaches, &parking_free)) return;
	Debug(yapf, 7, "AutoYield area={} train={} refresh=needed parking={}", area.id, v->index, parking_free);
	/* A newly detected express can also require a full-lane turnover. An old
	 * straight reservation must not commit the requester beyond the fork while
	 * the selected occupant is clearing its siding. */
	if (!parking_free && Evacuation(area, &approaches, v).incoming != v) return;
	/* Run only in the simulation loop, never during loading or a speculative
	 * restriction query. Reuse ordinary PBS cancellation/reservation so body
	 * protection, slot rollback, lookahead and failure-to-stop handling remain
	 * with the existing train code; no pathfinding algorithm is replaced. */
	FreeTrainTrackReservation(v);
	TryPathReserve(v, true, false);
}

/** Walk back from every body part to the nearest area boundary. Release only
 * when every legal branch proves an outward crossing. Unmarked dead ends,
 * loops, depots, bridges and exhausted budgets retain membership. This also
 * repairs missed tail callbacks and a reversal back through an entry marker. */
bool ProvenOutsideArea(const Area &area, const Train *v)
{
	AutoYieldProbeBudget budget;
	AutoRestoreBackup guard(probe_depth, probe_depth + 1);
	std::map<std::pair<TileIndex, Trackdir>, uint8_t> state;
	const auto outside = [&](auto &&self, TileIndex tile, Trackdir td) -> bool {
		if (!IsValidTile(tile) || !IsValidTrackdir(td)) return false;
		const auto key = std::pair{tile, td};
		if (auto it = state.find(key); it != state.end()) return it->second == 2;
		if (probe_budget_remaining == 0 || state.size() >= MAX_ROUTE_NODES) return false;
		probe_budget_remaining--;
		state[key] = 1;
		for (const Binding &binding : area.bindings) {
			if (binding.ref != MakeTraceRestrictRefId(tile, TrackdirToTrack(td))) continue;
			const bool outward = (binding.role == AutoYieldRole::Clear && binding.td == ReverseTrackdir(td)) ||
					(binding.role == AutoYieldRole::Approach && binding.td == td);
			state[key] = outward ? 2 : 3;
			return outward;
		}
		if (IsRailDepotTile(tile)) return false;
		AutoYieldTrack follow(v);
		if (!follow.Follow(tile, td) || follow.new_td_bits == TRACKDIR_BIT_NONE || follow.tiles_skipped || follow.is_bridge || follow.is_tunnel) return false;
		for (auto bits = follow.new_td_bits; bits != TRACKDIR_BIT_NONE; bits = KillFirstBit(bits)) {
			if (!self(self, follow.new_tile, FindFirstTrackdir(bits))) return false;
		}
		state[key] = 2;
		return true;
	};
	for (const Train *part = v; part != nullptr; part = part->Next()) {
		if (probe_budget_remaining == 0) return false;
		probe_budget_remaining--;
		/* A body on a boundary still occupies the region until it clears. */
		for (const Binding &binding : area.bindings) {
			if (GetTraceRestrictRefIdTileIndex(binding.ref) == part->tile &&
					(part->track & TrackToTrackBits(GetTraceRestrictRefIdTrack(binding.ref)))) return false;
		}
		if (!outside(outside, part->tile, ReverseTrackdir(part->GetVehicleTrackdir()))) return false;
	}
	return true;
}

/** Prove that a disconnected/re-routed claim no longer belongs to this train.
 * Inspect the exclusive parking segment, including all its platform tiles.
 * Unknown geometry, the train's body and any unresolved reservation retain it. */
static bool ProvenOrphanLane(const Area &area, const Train *v, TraceRestrictRefId ref)
{
	const Binding *binding = FindBinding(area, ref);
	if (binding == nullptr || !Active(v)) return false;
	const Binding *exit = binding;
	if (binding->role != AutoYieldRole::Exit) {
		std::optional<TraceRestrictRefId> exit_ref;
		if (!RouteToRole(area, v, GetTraceRestrictRefIdTileIndex(ref), binding->td, AutoYieldRole::Exit, false,
				std::nullopt, nullptr, nullptr, &exit_ref) || !exit_ref) return false;
		exit = FindBinding(area, *exit_ref);
		if (exit == nullptr) return false;
	}
	const TileIndex exit_tile = GetTraceRestrictRefIdTileIndex(exit->ref);
	const Track exit_track = GetTraceRestrictRefIdTrack(exit->ref);
	if ((GetReservedTrackbits(exit_tile) & TrackToTrackBits(exit_track)) != TRACK_BIT_NONE ||
			TrainReservationPassesThroughTile(v, exit_tile, exit_track)) return false;
	auto body_on = [v](TileIndex tile, Trackdir td) {
		const TrackBits track = TrackToTrackBits(TrackdirToTrack(td));
		for (const Train *part : VehiclesOnTile<VehicleType::Train>(tile)) {
			if (part->Primary() == v->Primary() && ((part->track & track) != TRACK_BIT_NONE || TracksOverlap(part->track | track))) return true;
		}
		return false;
	};
	TileIndex tile = exit_tile;
	Trackdir td = ReverseTrackdir(exit->td);
	std::set<std::pair<TileIndex, Trackdir>> seen;
	bool boundary = false;
	while (seen.size() < MAX_ROUTE_NODES && probe_budget_remaining > 0) {
		if (!seen.emplace(tile, td).second) return false;
		probe_budget_remaining--;
		if (body_on(tile, td) || IsRailTunnelBridgeTile(tile)) return false;
		if (IsPlainRailTile(tile)) {
			if (TracksOverlap(GetTrackBits(tile))) { boundary = true; break; }
		} else if (!HasStationTileRail(tile)) {
			return false;
		}
		if (AtRole(area, tile, ReverseTrackdir(td), AutoYieldRole::Main)) { boundary = true; break; }
		AutoYieldTrack follower(v);
		if (!follower.Follow(tile, td)) { boundary = follower.err == AutoYieldTrack::ErrorCode::NoWay; break; }
		if (follower.is_tunnel || follower.is_bridge || (follower.tiles_skipped > 0 && !follower.is_station) ||
				size_t(follower.tiles_skipped) > probe_budget_remaining || follower.new_td_bits == TRACKDIR_BIT_NONE) return false;
		const TileIndexDiff step = TileOffsByDiagDir(follower.exitdir);
		for (int i = follower.tiles_skipped; i > 0; i--) {
			probe_budget_remaining--;
			if (body_on(follower.new_tile - step * i, FindFirstTrackdir(follower.new_td_bits))) return false;
		}
		if (KillFirstBit(follower.new_td_bits) != TRACKDIR_BIT_NONE) {
			for (TrackdirBits bits = follower.new_td_bits; bits != TRACKDIR_BIT_NONE; bits = KillFirstBit(bits)) {
				if (body_on(follower.new_tile, FindFirstTrackdir(bits))) return false;
			}
			boundary = true; break;
		}
		tile = follower.new_tile; td = FindFirstTrackdir(follower.new_td_bits);
	}
	if (!boundary) return false;
	bool incomplete = false;
	const Train *front = v->GetMovingFront();
	const auto reachable = RouteToRole(area, v, front->tile, front->GetVehicleTrackdir(), binding->role, false, ref, &incomplete);
	return !reachable && !incomplete;
}

/** A committed waiting train is positive evidence that this entry still
 * matters. Losing a PBS prefix while queueing is not a cancellation. Recheck
 * the legal forward approach without requiring track ownership; only a
 * complete negative proof can discard that saved dependency. No new contender
 * is enrolled here and manual source cancellation remains authoritative. */
static bool AwaitedPendingApproach(const Area &area, const Train *v)
{
	if (!Active(v)) return false;
	std::vector<TraceRestrictSlotID> memberships;
	TraceRestrictGetVehicleSlots(v->index, memberships);
	bool awaited = false;
	for (auto id : memberships) {
		const auto *slot = TraceRestrictSlot::GetIfValid(id);
		if (slot == nullptr || !slot->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets) || slot->automatic_yield_area == 0) continue;
		auto group = areas.find({CompanyID{slot->owner}, uint16_t(slot->automatic_yield_area - 1)});
		if (group == areas.end() || !group->second.enabled) continue;
		auto lane = group->second.lanes.find(slot->automatic_yield_signal);
		if (lane != group->second.lanes.end() && !lane->second->occupants.empty()) { awaited = true; break; }
	}
	if (!awaited) return false;
	AutoYieldProbeBudget budget;
	const Train *front = v->GetMovingFront();
	bool incomplete = false;
	const auto approach = RouteToRole(area, v, front->tile, front->GetVehicleTrackdir(), AutoYieldRole::Approach,
			false, std::nullopt, &incomplete, nullptr, nullptr, std::nullopt, false, true);
	return approach.has_value() || incomplete;
}

void AutoYieldGameTick()
{
	if (dirty) UpdateAutoYieldAreas();
	if (!_settings_game.vehicle.automatic_yield) return;
	/* Use saved ticks and stable vehicle IDs, rather than a local cache or
	 * slot iteration order, to spread bounded pre-fork rechecks over 8 ticks. */
	for (const auto &[key, area] : areas) {
		if (!area.enabled || area.detection == nullptr || area.detection->occupants.empty() || area.detection->occupants.size() > MAX_PARTICIPANTS) continue;
		if (area.detection->occupants.size() < 2 && Participants(area).size() < 2) continue;
		const auto occupants = area.detection->occupants;
		for (VehicleID id : occupants) {
			if (((_state_ticks.base() + id.base()) & 7) != 0) continue;
			Vehicle *vehicle = Vehicle::GetIfValid(id);
			if (vehicle != nullptr && vehicle->type == VehicleType::Train) {
				Train *v = Train::From(vehicle);
				/* A parked train may have no forward reservation, so refresh its
				 * live target set before the reservation-dependent early returns. */
				if (Active(v) && YieldWaitInProgress(area, v)) {
					AutoYieldProbeBudget budget;
					for (const auto &[ref, lane] : area.lanes) if (lane->IsOccupant(id)) {
						const Binding *binding = FindBinding(area, ref);
						if (binding == nullptr) continue;
						TraceRestrictProgramInput input(GetTraceRestrictRefIdTileIndex(ref), binding->td, nullptr, nullptr);
						TraceRestrictProgramResult result;
						input.permitted_slot_operations = TRPISP_ACQUIRE;
						RememberYieldTargets(area, v, ref, input, result);
					}
				}
				RefreshAutoYieldMainReservation(area, v);
			}
		}
	}
	if ((_state_ticks.base() & 63) != 0) return;
	/* Reconcile changed ownership/topology and only inspect registered vehicles. */
	for (auto &[key, area] : areas) {
		if (!area.enabled || area.detection == nullptr) continue;
		for (const auto &[ref, lane] : area.lanes) {
			if (!lane->occupants.empty()) {
				const VehicleID id = lane->occupants.front();
				AutoYieldProbeBudget budget;
				if (const Train *v = GetTrain(id); v != nullptr && ProvenOrphanLane(area, v, ref)) {
					Debug(yapf, 6, "AutoYield area={} lane={} train={} release=proved-orphan", area.id, ref, id);
					lane->VacateByID(id); // Only our logical claim; never the source slot or PBS.
				}
			}
			auto edges = area.targets.find(ref);
			if (edges == area.targets.end() || edges->second->occupants.empty()) continue;
			if (lane->occupants.empty()) { edges->second->Clear(); continue; }
			/* Reconcile one saved edge per lane, bounded independently of slot
			 * history. Query failures retain it; only explicit invalidation or
			 * a positive independent-corridor proof can remove it. */
			const VehicleID target_id = edges->second->occupants[(_state_ticks.base() >> 6) % edges->second->occupants.size()];
			const Train *target = GetTrain(target_id), *waiting = GetTrain(lane->occupants.front());
			bool remove = !Active(target) || !RegisteredForYield(target) || !Active(waiting);
			if (!remove) {
				AutoYieldProbeBudget budget;
				remove = ClassifyYieldConflict(area, waiting, target) == YieldConflict::Independent;
			}
			if (remove) {
				Debug(yapf, 6, "AutoYield area={} lane={} target={} release=invalid-or-independent", area.id, ref, target_id);
				edges->second->VacateByID(target_id);
			}
		}
		if (area.detection->occupants.empty()) continue;
		std::set<VehicleID> approaching;
		if (area.pending != nullptr && !area.pending->occupants.empty()) {
			/* Query each detector once, not once per train. The PBS owner and
			 * saved pending slot reconstruct the same answer on a joining client. */
			for (const Binding &binding : area.bindings) {
				if (binding.role != AutoYieldRole::Approach || binding.registration != AutoYieldRegistration::Reservation) continue;
				const TileIndex tile = GetTraceRestrictRefIdTileIndex(binding.ref);
				const Track track = GetTraceRestrictRefIdTrack(binding.ref);
				if (!(GetReservedTrackbits(tile) & TrackToTrackBits(track))) continue;
				if (const Train *owner = GetTrainForReservation(tile, track)) approaching.insert(owner->Primary()->index);
			}
		}
		/* A user slot can contain far more vehicles than the active-area limit.
		 * Reconcile a deterministic window, never copy/scan the entire slot.
		 * Derive it from the saved game tick so joining clients use the same window. */
		const auto &occupants = area.detection->occupants;
		const VehicleID cleanup_candidate = occupants[(_state_ticks.base() >> 6) % occupants.size()];
		const size_t count = std::min(occupants.size(), MAX_PARTICIPANTS);
		const size_t first = (((_state_ticks.base() >> 6) % occupants.size()) * MAX_PARTICIPANTS) % occupants.size();
		std::vector<VehicleID> ids;
		ids.reserve(count);
		for (size_t i = 0; i < count; i++) ids.push_back(occupants[(first + i) % occupants.size()]);
		for (VehicleID id : ids) {
			const Train *v = GetTrain(id);
			const bool pending_entry = area.pending != nullptr && area.pending->IsOccupant(id);
			bool reserved_entry = pending_entry && approaching.contains(id);
			/* The normal PBS owner lookup cannot find a head still hidden in a
			 * depot. Prove its own reserved route reaches this exact detector
			 * track instead; a reservation on a crossing track is insufficient. */
			if (!reserved_entry && pending_entry && v != nullptr && v->IsInDepot() && Active(v, true)) {
				for (const Binding &binding : area.bindings) {
					if (binding.role != AutoYieldRole::Approach || binding.registration != AutoYieldRegistration::Reservation) continue;
					if (TrainReservationPassesThroughTile(v, GetTraceRestrictRefIdTileIndex(binding.ref), GetTraceRestrictRefIdTrack(binding.ref))) {
						reserved_entry = true;
						break;
					}
				}
			}
			/* A successful departure can reserve past a detector while its head
			 * is still in the depot. Keep only that live, moving departure;
			 * parked depots and cancelled reservations still release their slot. */
			bool invalid = v == nullptr || !v->IsPrimaryVehicle() || v->IsVirtualOrCarried() || !IsValidTile(v->tile) ||
					(v->IsInDepot() && (!reserved_entry || v->vehstatus.Any({VehState::Stopped, VehState::Crashed})));
			/* Before physical entry, cancellation/rerouting must not leave a
			 * phantom contender. Physical entries additionally use the bounded
			 * whole-body outward-boundary proof below. */
			invalid |= pending_entry && !reserved_entry && !AwaitedPendingApproach(area, v);
			if (!invalid && !pending_entry && !reserved_entry && id == cleanup_candidate) invalid = ProvenOutsideArea(area, v);
			if (invalid) {
				Debug(yapf, 6, "AutoYield area={} train={} release=invalid-or-proved-outside pending={}", area.id, id, pending_entry);
				area.detection->VacateByID(id);
				/* A physical lane is still protected by PBS. Only invalid/deleted or
				 * departed records are released; no track reservations are touched. */
				for (const auto &[ref, lane] : area.lanes) lane->VacateByID(id);
			}
		}
	}
}

bool EvaluateAutoYield(const Train *v, uint16_t id, AutoYieldCondition condition, bool invert)
{
	if (AutoYieldRouteQuerySuppresses(id)) return false;
	AutoYieldProbeBudget budget;
	const Area *area = FindArea(v, id);
	if (!_settings_game.vehicle.automatic_yield || area == nullptr || !Active(v) || !area->detection->IsOccupant(v->index)) return false;
	budget.UseArea(*area);
	bool result = false;
	switch (condition) {
		case AutoYieldCondition::Faster:
		case AutoYieldCondition::NeedsSiding: result = NeedsSiding(*area, v); break;
		case AutoYieldCondition::FreeSiding: result = FreeLane(*area, v); break;
		case AutoYieldCondition::MustWait: result = MustWait(*area, v); break;
		case AutoYieldCondition::Full: result = !FreeLane(*area, v); break;
		case AutoYieldCondition::Evacuate: result = Evacuation(*area).departing == v; break;
		case AutoYieldCondition::MayLeave: result = !MustWait(*area, v) || Evacuation(*area).departing == v; break;
	}
	return result != invert;
}

void ExecuteAutoYield(const Train *v, const TraceRestrictProgramInput &input, TraceRestrictProgramResult &out, uint16_t id, AutoYieldRole role, bool &ignore_pathfinder_penalty)
{
	if (AutoYieldRouteQuerySuppresses(id)) return;
	AutoYieldProbeBudget budget;
	if (v == nullptr) return;
	struct DecisionLog {
		const Train *train;
		const TraceRestrictProgramInput &input;
		const TraceRestrictProgramResult &result;
		uint16_t area;
		AutoYieldRole role;
		~DecisionLog()
		{
			Debug(yapf, 7, "AutoYield area={} train={} signal={} direction={} target={} role={} permissions={} result={} budget={}",
					area, train->index, input.tile, uint(input.trackdir), train->dest_tile, uint(role), uint(input.permitted_slot_operations), uint(result.flags), probe_budget_remaining);
		}
	} log{v, input, out, id, role};
	const bool signal_in_direction = IsRailTunnelBridgeTile(input.tile) && IsTrackAcrossTunnelBridge(input.tile, TrackdirToTrack(input.trackdir)) ?
			((IsTunnelBridgeSignalSimulationEntrance(input.tile) && TrackdirEntersTunnelBridge(input.tile, input.trackdir)) ||
			 (IsTunnelBridgeSignalSimulationExit(input.tile) && TrackdirExitsTunnelBridge(input.tile, input.trackdir))) :
			HasSignalOnTrackdir(input.tile, input.trackdir);
	if (!signal_in_direction) return;
	if (role == AutoYieldRole::Clear) {
		/* Cleanup must work even when the area is disabled, incomplete or over
		 * its participant budget. Vacating also releases this train's lanes. */
		TraceRestrictSlot *slot = TraceRestrictSlot::GetIfValid(id);
		if (slot != nullptr && slot->automatic_yield_area == 0 && slot->vehicle_type == VehicleType::Train &&
				(input.permitted_slot_operations & TRPISP_RELEASE_BACK)) {
			slot->Vacate(v);
		}
		return;
	}
	/* PBS establishes a depot's departure reservation before moving its head
	 * onto open track. Detector registration must participate in that same
	 * transaction; all waiting/turnover decisions still exclude depot trains. */
	if (!_settings_game.vehicle.automatic_yield || !Active(v, role == AutoYieldRole::Approach)) return;
	const Area *area = FindArea(v, id);
	if (area == nullptr) return;
	if (role == AutoYieldRole::Approach && (input.input_flags.Test(TraceRestrictProgramInputFlag::NativeRouteChoice) || input.permitted_slot_operations != TRPISP_NONE)) {
		AutoYieldStageNativeRoute(v, input.tile, input.trackdir, id, input.permitted_slot_operations != TRPISP_NONE);
	}
	budget.UseArea(*area);
	if (role != AutoYieldRole::Approach && !area->detection->IsOccupant(v->index)) return;
	std::optional<EvacuationPlan> station_turnover;
	if (role == AutoYieldRole::Exit && area->has_platform && YieldWaitInProgress(*area, v)) {
		bool platform_requested = false;
		for (VehicleID candidate_id : area->detection->occupants) {
			const Train *candidate = GetTrain(candidate_id);
			if (!Active(candidate) || HasLaneClaim(*area, candidate) || !candidate->current_order.IsType(OT_GOTO_STATION)) continue;
			for (const auto &[ref, lane] : area->lanes) {
				if (ScheduledPlatformStop(candidate, GetPlatformInfo(*area, candidate, ref))) { platform_requested = true; break; }
			}
			if (platform_requested) break;
		}
		/* Resolve necessary platform access before optional route forecasts
		 * spend this evaluation's budget. Keep the result within this call:
		 * never rerun the same proposal with an exhausted budget at Exit. */
		if (platform_requested) station_turnover = Evacuation(*area);
	}
	if (role == AutoYieldRole::Main || role == AutoYieldRole::Exit) {
		/* Judge this candidate's continuation, not a different/unknown branch
		 * before its fork. Cancelling an old main reservation must not erase
		 * the conflict which justified a safe diversion on the next retry. */
		GetSplitRouteProof(*area, v, input.tile, input.trackdir);
	}
	if (role == AutoYieldRole::Exit) {
		/* This is a query about crossing this exit, potentially made while the
		 * train is still approaching a platform. Its current station target
		 * must not truncate the candidate departure before the controlled signal
		 * and turn a real merge into an "unknown" route. Only this evaluation's
		 * read-only outlet cache is anchored here; native routing is unchanged. */
		const auto proof = ComputeOutlet(*area, v, input.tile, input.trackdir);
		bool replaced = false;
		for (size_t i = 0; i < outlet_count; i++) if (outlet_cache[i].area == area && outlet_cache[i].train == v) {
			outlet_cache[i].proof = proof; replaced = true; break;
		}
		if (!replaced && outlet_count < outlet_cache.size()) outlet_cache[outlet_count++] = {area, v, proof};
	}
	if (IsAutoYieldLaneRole(role) && HasLaneClaim(*area, v)) {
		AutoYieldPreferenceBudget preference_budget;
		if (PreferMainForDeparture(*area, v)) out.penalty += 400; // Soft preference; retain a required/blocked-main fallback.
	}
	if (IsAutoYieldLaneRole(role) && area->has_platform && !HasLaneClaim(*area, v)) {
		const auto ref = MakeTraceRestrictRefId(input.tile, TrackdirToTrack(input.trackdir));
		bool ordinary_available = false;
		{
			AutoYieldPreferenceBudget preference_budget;
			const PlatformInfo platform = GetPlatformInfo(*area, v, ref);
			if (platform.marked && !RequiresPlatform(v, platform)) {
				ordinary_available = FreeLane(*area, v, false, true);
				if (!ordinary_available && platform.known) {
					/* Resolve legacy exit markers back to their entrance claim. A
					 * cost preference never grants admission: prove length, exclusive
					 * occupancy and a clear departure route before cancelling costs. */
					std::optional<TraceRestrictRefId> lane_ref;
					if (area->lanes.contains(ref)) {
						lane_ref = ref;
					} else if (role == AutoYieldRole::Exit) {
						for (const auto &[candidate_ref, lane] : area->lanes) {
							if (GetPlatformInfo(*area, v, candidate_ref).exit == ref) { lane_ref = candidate_ref; break; }
						}
					}
					if (lane_ref && FreeLane(*area, v, false, false, lane_ref) && NeedsSiding(*area, v)) ignore_pathfinder_penalty = true;
				}
			}
		}
		/* Soft preference only: retain platform fallback when ordinary sidings
		 * are occupied, unreachable, too short, or not provably available. */
		if (ordinary_available) {
			out.flags |= TRPRF_DENY | TRPRF_WAIT_AT_PBS;
			return;
		}
	}
	if (role == AutoYieldRole::Approach) {
		const Binding *binding = FindBinding(*area, MakeTraceRestrictRefId(input.tile, TrackdirToTrack(input.trackdir)));
		if (binding == nullptr) return;
		const auto mode = binding->registration;
		const auto permissions = input.permitted_slot_operations;
		const bool physical = permissions & (TRPISP_RELEASE_FRONT | TRPISP_RELEASE_BACK);
		if (physical && !(permissions & TRPISP_ACQUIRE_TEMP_STATE)) {
			/* Conventional bridge signalling has no advance PBS reservation
			 * callback at its entrance. Physical entry is authoritative there;
			 * never invent a claim during speculative path search. Realistic
			 * braking continues to use the existing reservation-end callback. */
			const bool bridge_entry = mode == AutoYieldRegistration::Reservation && _settings_game.vehicle.train_braking_model != TBM_REALISTIC && IsRailTunnelBridgeTile(input.tile) &&
					(permissions & TRPISP_RELEASE_FRONT);
			const bool crossed = bridge_entry || mode == AutoYieldRegistration::Front ? (permissions & TRPISP_RELEASE_FRONT) :
					mode == AutoYieldRegistration::Tail && (permissions & TRPISP_RELEASE_BACK);
			if (crossed) area->detection->Occupy(v);
			/* Physical passage confirms a PBS reservation claim, but never reclaims
			 * cancelled membership in reservation mode. Tail mode waits for tail. */
			if (mode != AutoYieldRegistration::Tail || (permissions & TRPISP_RELEASE_BACK)) area->pending->VacateByID(v->index);
			return;
		}
		if (mode != AutoYieldRegistration::Reservation) {
			if (!area->detection->OccupyDryRun(v->index)) out.flags |= TRPRF_WAIT_AT_PBS;
			return;
		}
		const bool member = area->detection->IsOccupant(v->index);
		bool committed = input.permitted_slot_operations & TRPISP_PBS_RES_END_ACQUIRE;
		/* Some ordinary PBS entry callbacks run before attempting a reservation.
		 * They may confirm an existing owned path, never invent a successful one. */
		if (!committed && (input.permitted_slot_operations & TRPISP_ACQUIRE) &&
				(GetReservedTrackbits(input.tile) & TrackToTrackBits(TrackdirToTrack(input.trackdir)))) {
			const Train *owner = GetTrainForReservation(input.tile, TrackdirToTrack(input.trackdir));
			committed = owner != nullptr && owner->Primary() == v->Primary();
		}
		if (input.permitted_slot_operations & TRPISP_ACQUIRE_TEMP_STATE) {
			Acquire(area->detection, v, input, out);
			if (!member && area->detection->IsOccupant(v->index)) Acquire(area->pending, v, input, out);
		} else if (committed) {
			if (area->detection->Occupy(v)) {
				if (!member) area->pending->Occupy(v);
			} else out.flags |= TRPRF_WAIT_AT_PBS;
		} else if (!area->detection->OccupyDryRun(v->index)) out.flags |= TRPRF_WAIT_AT_PBS;
	} else if (role == AutoYieldRole::Main) {
		/* A Main before the selected platform's inlet is an approach resource,
		 * not the stopping train's bypass. Denying it to divert into that same
		 * downstream platform denies every valid station route. Leave this
		 * prefix to PBS; the platform Exit still checks all real dependencies.
		 * Only a completed multi-outlet native stop plan proves this case. */
		if (area->clear_count > 1 && area->has_platform && v->current_order.IsType(OT_GOTO_STATION) &&
				AutoYieldNativeStopNeedsApproach(v, id, input.tile, input.trackdir)) {
			Debug(yapf, 7, "AutoYield area={} train={} main={} priority=native-PBS reason=selected-platform-approach", id, v->index, input.tile);
			return;
		}
		if (PrioritisePassengerExit(*area, v)) {
			out.flags |= TRPRF_WAIT_AT_PBS | TRPRF_PBS_RES_END_WAIT;
			return;
		}
		/* Keep the admission and evacuation decisions within the same bounded
		 * proof. Repeating the approach scan could spend the budget before
		 * rediscovering a departure which the siding's exit already permits. */
		if (HasLaneClaim(*area, v) && PreferMainForDeparture(*area, v)) return;
		/* Evaluate the Main on this candidate route, not the nearest marker
		 * on a different feeder. Otherwise a slow train may borrow a fast
		 * approach before discovering the conflict after merging. */
		const auto candidate_main = MakeTraceRestrictRefId(input.tile, TrackdirToTrack(input.trackdir));
		const auto approaches = GetMainApproaches(*area, v, candidate_main);
		/* A speculative candidate must not invent a turnover which the siding
		 * exit cannot reproduce. Full-lane evacuation retains its shared,
		 * route-independent participant classification. */
		const bool detour = std::any_of(approaches.begin(), approaches.end(), [v](const MainApproach &a) {
			return a.train == v && a.candidate_detour;
		});
		const auto *evacuation_approaches = detour ? nullptr : &approaches;
		bool parking_free = false;
		if (NeedsSiding(*area, v, &approaches, &parking_free)) {
			/* Parking must be free; its shared departure may legitimately be
			 * reserved by the very express we are about to wait for. */
			if (parking_free) out.flags |= TRPRF_DENY | TRPRF_WAIT_AT_PBS | TRPRF_PBS_RES_END_WAIT;
			/* Reserving up to Main would already commit the straight branch.
			 * Keep the reservation endpoint before the fork while a selected
			 * departure clears, so the next retry can actually enter its siding. */
			else if (Evacuation(*area, evacuation_approaches, v).incoming == v) out.flags |= TRPRF_WAIT_AT_PBS | TRPRF_PBS_RES_END_WAIT;
		} else if (area->has_platform && v->current_order.IsType(OT_GOTO_STATION) && Evacuation(*area, evacuation_approaches).incoming == v) {
			/* Keep the station requester before the fork while its platform is
			 * being freed, just as for the existing overtaking turnover. */
			out.flags |= TRPRF_WAIT_AT_PBS | TRPRF_PBS_RES_END_WAIT;
		}
	} else if (role == AutoYieldRole::Siding) {
		auto lane = area->lanes.find(MakeTraceRestrictRefId(input.tile, TrackdirToTrack(input.trackdir)));
		if (lane == area->lanes.end()) return;
		if (!lane->second->occupants.empty() && !lane->second->IsOccupant(v->index)) { out.flags |= TRPRF_DENY | TRPRF_WAIT_AT_PBS; return; }
		AcquireLane(*area, lane->second, v, input, out);
	} else if (role == AutoYieldRole::Exit) {
		if (area->exit_only) {
			const auto ref = MakeTraceRestrictRefId(input.tile, TrackdirToTrack(input.trackdir));
			auto lane = area->lanes.find(ref);
			const Binding *binding = FindBinding(*area, ref);
			if (lane == area->lanes.end() || binding == nullptr) return;
			const auto length = SidingParkingLength(*area, v, *binding);
			if (!length) return;
			if (*length < v->gcache.cached_total_length + TILE_SIZE || !lane->second->OccupyDryRun(v->index)) {
				out.flags |= TRPRF_DENY | TRPRF_WAIT_AT_PBS | TRPRF_PBS_RES_END_WAIT;
				return;
			}
			/* End-position dry runs only test exclusivity; the existing PBS
			 * reservation-end callback commits the claim after success. */
			AcquireLane(*area, lane->second, v, input, out);
		}
		for (const auto &[ref, lane] : area->lanes) if (lane->IsOccupant(v->index)) RememberYieldTargets(*area, v, ref, input, out);
		const bool station_departure = station_turnover && station_turnover->departing == v;
		if (!station_departure && MustWait(*area, v) && (station_turnover ? station_turnover->departing : Evacuation(*area).departing) != v) out.flags |= TRPRF_WAIT_AT_PBS;
	}
}

bool GetAutoYieldRouteArea(const Train *v, uint16_t id, AutoYieldRouteArea &result)
{
	/* Pure metadata lookup must also work inside a read-only route proof.
	 * FindArea's recursion barrier protects decisions, not registry metadata. */
	if (v == nullptr || dirty) return false;
	const TraceRestrictSlot *slot = TraceRestrictSlot::GetIfValid(id);
	if (slot == nullptr || slot->automatic_yield_area != 0 || slot->vehicle_type != VehicleType::Train) return false;
	const auto found = areas.find({CompanyID{slot->owner}, id});
	const Area *area = found == areas.end() ? nullptr : &found->second;
	/* This extension deliberately never changes a single-outlet area's route
	 * selection, priority, reservations or detector lifecycle. */
	if (area == nullptr || !area->enabled || area->clear_count < 2) return false;
	result.slot = id; result.clears.clear();
	result.registered = area->detection != nullptr && area->detection->IsOccupant(v->index);
	result.signature = 2166136261u;
	for (const auto &binding : area->bindings) {
		for (uint32_t value : {uint32_t(binding.ref), uint32_t(binding.role), uint32_t(binding.td), uint32_t(binding.platform), uint32_t(binding.speed_cap), uint32_t(binding.registration), uint32_t(area->percent)}) result.signature = (result.signature ^ value) * 16777619u;
		if (binding.role == AutoYieldRole::Clear) result.clears.emplace_back(GetTraceRestrictRefIdTileIndex(binding.ref), binding.td);
	}
	return result.clears.size() > 1;
}

bool AutoYieldRouteAreasAvailable() { return !dirty && multiple_outlets; }
