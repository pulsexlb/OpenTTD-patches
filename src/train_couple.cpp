/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file train_couple.cpp Handling of train coupling and decoupling. */

#include "stdafx.h"
#include "train_internal.h"
#include "train_couple.h"
#include "depot_base.h"
#include "error.h"
#include "articulated_vehicles.h"
#include "command_func.h"
#include "pathfinder/yapf/yapf.hpp"
#include "news_func.h"
#include "company_func.h"
#include "newgrf_sound.h"
#include "newgrf_text.h"
#include "strings_func.h"
#include "viewport_func.h"
#include "vehicle_func.h"
#include "sound_func.h"
#include "ai/ai.hpp"
#include "game/game.hpp"
#include "newgrf_station.h"
#include "effectvehicle_func.h"
#include "network/network.h"
#include "network/network_sync.h"
#include "core/random_func.hpp"
#include "company_base.h"
#include "group.h"
#include "newgrf.h"
#include "roadveh_transport.h"
#include "infrastructure_func.h"
#include "order_backup.h"
#include "zoom_func.h"
#include "newgrf_debug.h"
#include "framerate_type.h"
#include "tracerestrict.h"
#include "tracerestrict_early_route.h"
#include "tbtr_template_vehicle_func.h"
#include "autoreplace_func.h"
#include "engine_func.h"
#include "bridge_signal_map.h"
#include "scope_info.h"
#include "scope.h"
#include "core/checksum_func.hpp"
#include "debug_dbg_assert.h"
#include "debug_settings.h"
#include "train_settings.h"
#include "train_speed_adaptation.h"
#include "event_logs.h"
#include "misc_cmd.h"
#include "tile_cmd.h"
#include "train_cmd.h"
#include "vehicle_cmd.h"
#include "tbtr_template_vehicle_cmd.h"
#include "script/api/script_event_types.hpp"
#include "3rdparty/cpp-btree/btree_map.h"
#include "widgets/vehicle_widget.h"
#include "table/strings.h"
#include "table/train_cmd.h"
#include "safeguards.h"

/* Forward declarations for functions defined further down in this file. */
static void MoveTrainToPosition(Train *v, int x, int y);
static bool RepairVehicleTrackBit(Train *w, const Train *hint);

btree::btree_map<VehicleID, uint64_t> _couple_search_last_fail;

/** A failed couple target search is retried only after this many vehicle ticks.
 *  This must be a vehicle-tick clock, not the date-tick counter: with a day
 *  length factor the date tick advances only once per DayLengthFactor vehicle
 *  ticks, which would stretch the retry interval by that factor. */
constexpr uint64_t COUPLE_FAIL_RETRY_INTERVAL = 60;

/**
 * A train waiting to be coupled holds only the reservation of the tiles its
 * body occupies. Everything beyond its moving front (its drive-away
 * direction) is released once, so its partner can reserve a path up to the
 * contact point with the regular reservation machinery, while the body tiles
 * still keep other trains from driving through the consist. Beyond the
 * moving back lies the path of the just decoupled partner part, which is
 * deliberately left alone.
 *
 * The release never crosses a signal, a track choice or a tile occupied by
 * any vehicle (including station/bridge tiles the track follower would skip
 * across), so it can never eat into another train's reservation.
 * @param consist %Train waiting for its coupling partner.
 */
void HoldWaitingTrainBody(Train *consist)
{
	assert(consist->IsPrimaryVehicle());

	if (!consist->couple_body_hold) {
		/* Only release when nobody is homing in on us yet: afterwards the
		 * approaching partner's own reservation may be adjacent to ours and
		 * must not be touched. */
		if (const Train *claimant = GetValidCoupleClaimant(consist); claimant != nullptr && claimant->current_order.IsType(OT_GOTO_COUPLE)) return;
		consist->couple_body_hold = true;

		consist->lookahead.reset();

		/* Only clear outward from the moving front, i.e. in the drive-away
		 * direction. Beyond the moving back lies the path of the just
		 * decoupled partner part (the original consist's forward reservation
		 * is always ahead of its former front), which must not be touched:
		 * eating it strands that train in front of the next signal. */
		Train *end = consist->GetMovingFront();
		if (end->track != TRACK_BIT_WORMHOLE && end->track != TRACK_BIT_DEPOT) {
			Trackdir outward = end->GetVehicleTrackdir();
			CFollowTrackRail ft(consist, consist->GetIndirectCompatibleRailTypes());
			TileIndex tile = end->tile;
			Trackdir td = outward;
			while (ft.Follow(tile, td)) {
				if (KillFirstBit(ft.new_td_bits) != TRACKDIR_BIT_NONE) break;
				Trackdir ntd = FindFirstTrackdir(ft.new_td_bits);
				TileIndex ntile = ft.new_tile;
				if (!HasReservedTracks(ntile, TrackToTrackBits(TrackdirToTrack(ntd)))) break;
				bool occupied = false;
				for (Train *w : VehiclesOnTile<VehicleType::Train>(ntile)) occupied = true;
				if (occupied) break;
				/* The follower may have jumped across station/bridge tiles.
				 * A vehicle standing on a skipped tile must stop the walk:
				 * clearing onwards would eat the reservation of that train's
				 * path. */
				bool skipped_occupied = false;
				if (ft.tiles_skipped != 0) {
					TileIndexDiff sdiff = TileOffsByDiagDir(ft.exitdir);
					for (TileIndex st = ntile - sdiff * ft.tiles_skipped; st != ntile; st += sdiff) {
						for (Train *w : VehiclesOnTile<VehicleType::Train>(st)) skipped_occupied = true;
					}
					if (skipped_occupied) break;
				}
				if (IsRailStationTile(ntile)) {
					/* The follower jumped across the platform: clear the strip
					 * back towards the entry, then continue from the far end. */
					TileIndex t = ntile;
					TileIndexDiff back = TileOffsByDiagDir(ReverseDiagDir(TrackdirToExitdir(ntd)));
					for (;;) {
						bool strip_occupied = false;
						for (Train *w : VehiclesOnTile<VehicleType::Train>(t)) strip_occupied = true;
						if (strip_occupied) break;
						if (!HasStationReservation(t)) break;
						SetRailStationReservation(t, false);
						TileIndex prev = t + back;
						if (!IsCompatibleTrainStationTile(prev, ntile)) break;
						t = prev;
					}
				} else {
					UnreserveRailTrack(ntile, TrackdirToTrack(ntd));
				}
				tile = ntile;
				td = ntd;
				/* The chain ends at a signal facing our direction. */
				if (IsTileType(ntile, TileType::Railway) && HasSignalOnTrackdir(ntile, ntd)) break;
			}
		}
	}

	/* Keep every body tile held. */
	for (Train *u = consist->First(); u != nullptr; u = u->Next()) {
		if (u->track == TRACK_BIT_WORMHOLE || u->track == TRACK_BIT_DEPOT) continue;
		if ((GetReservedTrackbits(u->tile) & u->track) != u->track) {
			bool ok = TryReserveRailTrack(u->tile, TrackdirToTrack(u->GetVehicleTrackdir()), false);
		}
	}
}
void ClearCoupleSearchLastFail()
{
	_couple_search_last_fail.clear();
}
/**
 * Whether any consist is currently waiting for a couple. The couple target
 * search walks the entire rail network when no partner is reachable; this
 * cheap pre-check avoids that scan entirely when nobody is waiting at all.
 */
bool IsHoldingForCouple(const Train *train)
{
	return train->current_order.IsType(OT_WAIT_COUPLE) &&
			(!train->current_order.HasWaitCoupleStation() ||
			(train->last_station_visited == train->current_order.GetWaitCoupleStation() && train->cur_speed == 0 &&
			GetTrainCouplingStation(train) == train->current_order.GetWaitCoupleStation()));
}
/** Own only memberships acquired here; cancelling an order cannot return an unrelated slot. */
void UpdateWaitCoupleSlot(Train *train)
{
	const TraceRestrictSlotID desired = IsHoldingForCouple(train) && !train->vehstatus.Test(VehState::Crashed) ? train->current_order.GetWaitCoupleSlot() : TraceRestrictSlotID::Invalid();
	if (train->wait_couple_acquired_slot != desired) {
		if (auto *old = TraceRestrictSlot::GetIfValid(train->wait_couple_acquired_slot)) old->Vacate(train);
		train->wait_couple_acquired_slot = TraceRestrictSlotID::Invalid();
	}
	if (desired == TraceRestrictSlotID::Invalid() || train->cur_speed != 0) return;
	auto *slot = TraceRestrictSlot::GetIfValid(desired);
	if (slot == nullptr || slot->vehicle_type != VehicleType::Train || !slot->IsUsableByOwner(train->owner) || slot->IsOccupant(train->index)) return;
	if (slot->Occupy(train)) train->wait_couple_acquired_slot = desired;
}
/** A full automatic slot keeps the body stopped, but offers no routing target. */
static bool WaitCoupleSlotReady(const Train *train)
{
	const auto id = train->current_order.GetWaitCoupleSlot();
	if (id == TraceRestrictSlotID::Invalid()) return true;
	const auto *slot = TraceRestrictSlot::GetIfValid(id);
	return slot != nullptr && slot->vehicle_type == VehicleType::Train && slot->IsUsableByOwner(train->owner) && slot->IsOccupant(train->index);
}
static bool HasWaitingCoupleTrain()
{
	for (Vehicle *v : Vehicle::Iterate()) {
		if (v->type != VehicleType::Train || !v->IsPrimaryVehicle()) continue;
		if (v->vehstatus.Test(VehState::Crashed)) continue;
		if (IsHoldingForCouple(Train::From(v)) && WaitCoupleSlotReady(Train::From(v))) return true;
	}
	return false;
}
/**
 * If a foreign consist on \a tile is a plausible waiting couple target, return
 * its primary. This is only a cheap pre-check; the couple pathfinder performs
 * the full validation (cargo, speed, arrangement, claim, ...).
 */
const Train *GetWaitingCoupleCandidateOnTile(const Train *v, TileIndex tile)
{
	for (const Train *u : VehiclesOnTile<VehicleType::Train>(tile)) {
		Train *candidate = u->Primary();
		if (candidate == v->Primary()) continue;
		if (!candidate->current_order.IsType(OT_WAIT_COUPLE)) continue;
		if (!IsHoldingForCouple(candidate) || !WaitCoupleSlotReady(candidate)) continue;
		if (!IsTrainCouplingAllowed(v->owner, candidate->owner)) continue;
		return candidate;
	}
	return nullptr;
}
/**
 * Find the track to take when going to couple with another train.
 * @param v The train.
 * @param do_track_reservation Whether to reserve the path.
 * @param couple_target Optionally returns the contact-end vehicle of the
 *   waiting train the path leads to.
 * @param couple_cost Optionally returns the path cost of the found route.
 * @param only_target Optional exclusive target. Used when the target is already
 *   known to be the consist blocking a reservation.
 * @return The track to take, or #INVALID_TRACK if no path was found.
 */
Track DoTrainCouplePathfind(const Train *v, bool do_track_reservation, Train **couple_target, uint32_t *couple_cost,
		const Train *only_target)
{
	const VehicleID id = v->Primary()->index;
	/* Vehicle ticks, not date ticks: _scaled_tick_counter advances once per
	 * simulation iteration and is restored from the savegame (see afterload). */
	const uint64_t now = _scaled_tick_counter;

	/* Nobody is waiting for a couple anywhere: no target can be found, so do
	 * not walk the whole network to prove it. */
	if (!HasWaitingCoupleTrain()) {
		if (couple_target != nullptr) *couple_target = nullptr;
		if (couple_cost != nullptr) *couple_cost = 0;
		return INVALID_TRACK;
	}

	/* Drop entries for trains that no longer exist. */
	if (_couple_search_last_fail.size() > 128) {
		for (auto i = _couple_search_last_fail.begin(); i != _couple_search_last_fail.end();) {
			if (Train::GetIfValid(i->first) == nullptr) {
				i = _couple_search_last_fail.erase(i);
			} else {
				++i;
			}
		}
	}

	/* A search that found no reachable partner is expensive; retry it only
	 * after an interval instead of every track choice. */
	auto it = _couple_search_last_fail.find(id);
	if (it != _couple_search_last_fail.end()) {
		/* The tick counter is saved/restored with the game, so guard against it
		 * moving backwards relative to a cached timestamp. */
		if (it->second > now) it->second = now;
		else if (now < it->second + COUPLE_FAIL_RETRY_INTERVAL) {
			if (couple_target != nullptr) *couple_target = nullptr;
			if (couple_cost != nullptr) *couple_cost = 0;
			return INVALID_TRACK;
		}
	}

	Track ret = YapfTrainCoupleTrack(v, !do_track_reservation, couple_target, couple_cost, nullptr, only_target);
	if (ret == INVALID_TRACK) {
		_couple_search_last_fail[id] = now;
	} else {
		_couple_search_last_fail.erase(id);
	}
	return ret;
}
bool IsCouplePartnerOf(const Train *consist, const Train *other)
{
	/* Reservation walkers can return any physical vehicle, including an
	 * articulated child or the rear contact end. Claims belong to consists. */
	consist = consist->Primary();
	other = other->Primary();
	if (consist->current_order.IsType(OT_GOTO_COUPLE)) {
		if (const Train *tgt = GetClaimedCoupleTarget(consist); tgt != nullptr && tgt->Primary() == other) return true;
	}
	if (const Train *claimant = GetValidCoupleClaimant(consist); claimant != nullptr && claimant->current_order.IsType(OT_GOTO_COUPLE) && claimant->Primary() == other) return true;
	return false;
}
/** A cut can leave the departing head and the waiting split part on one tile.
 * The waiting part can already have cleared JustDecoupled while retaining its
 * same-direction departure obligation. Only exclude a whole vehicle strictly
 * behind the departing end, on the same straight platform track and level.
 * Tile sharing alone is never proof that a vehicle ahead can be passed. */
bool IsDecoupledPartBehindDeparture(const Train *consist, const Train *part)
{
	const Train *front = consist->GetMovingFront();
	if (!consist->flags.Any({VehicleRailFlag::JustDecoupled, VehicleRailFlag::SameDirDecouplePending}) ||
			!part->Primary()->flags.Any({VehicleRailFlag::JustDecoupled, VehicleRailFlag::SameDirDecouplePending}) ||
			front->tile != part->tile || !IsRailStationTile(front->tile) ||
			front->track != part->track || front->z_pos != part->z_pos ||
			!IsDiagonalDirection(front->GetMovingDirection())) return false;
	const TileIndexDiffC delta = TileIndexDiffCByDir(front->GetMovingDirection());
	const int dx = part->x_pos - front->x_pos, dy = part->y_pos - front->y_pos;
	const int along = dx * delta.x + dy * delta.y;
	const int across = dx * delta.y - dy * delta.x;
	const int separation = CeilDiv(front->gcache.cached_veh_length + part->gcache.cached_veh_length, 2);
	return across == 0 && along <= -separation;
}
/**
 * Whether \a other is the former partner of a just-performed decouple, still
 * standing on the cut tile both parts necessarily share: the position where the
 * two parts meet. That partner does not stand in our way there. Both flags have
 * to count: an opposite-direction part carries JustDecoupled, a same-direction
 * part carries SameDirDecouplePending instead, and those two wait for each
 * other, so only exempting one of them deadlocks both.
 * @param consist The train whose front tile is the cut tile.
 * @param other   The vehicle found on that tile.
 * @param tile    The tile the vehicle was found on.
 * @return \c true when this is the decouple partner on the shared cut tile.
 */
bool IsDecouplePartnerOnCutTile(const Train *consist, const Train *other, TileIndex tile)
{
	return tile == consist->GetMovingFront()->tile && IsDecoupledPartBehindDeparture(consist, other);
}
/**
 * Push every vehicle of a chain-ordered range one pixel forward along its own
 * facing, one vehicle at a time. Deliberately does NOT use TrainController's
 * segment walk: that follows GetMovingNext(), which honours each vehicle's
 * DrivingBackwards flag. A freshly merged consist can carry mixed flags, and
 * a segment walk would then turn around mid-chain, miss its nomove bound and
 * run off the end of the chain, crashing on the null vehicle.
 * @param first first vehicle to push.
 * @param last last vehicle to push, or nullptr to push up to the chain end.
 */
static void CoupleAdvanceVehicles(Train *first, Train *last)
{
	for (Train *w = first; w != nullptr; w = w->Next()) {
		/* nomove = w's own moving-next stops the walk after exactly w. */
		TrainController(w, w->GetMovingNext());
		if (w == last) break;
	}
}

int CoupleJointOffset(uint8_t front_len, uint8_t back_len, bool front_driving_backwards);

/**
 * Pixel step of a vehicle's motion. Vehicles flagged DrivingBackwards move
 * against their facing, so their motion delta is the negated direction delta.
 */
static TileIndexDiffC CoupleMotionDelta(const Train *w)
{
	TileIndexDiffC dd = TileIndexDiffCByDir(w->direction);
	if (w->IsDrivingBackwards()) {
		dd.x = -dd.x;
		dd.y = -dd.y;
	}
	return dd;
}

/**
 * Unit motion delta along the vehicle's own track, pointing from \a v towards
 * \a target. Unlike CoupleMotionDelta() this does not consult the vehicle's
 * stored direction: after a pairwise-swap turnaround a vehicle's direction is
 * inherited from its swap partner and can disagree with the track at its new
 * position on curves, which shoves re-spacing vehicles off the rail axis.
 * The track bit, in contrast, always matches the position it was swapped with.
 * Falls back to CoupleMotionDelta() where no track bit is usable.
 */
static TileIndexDiffC TrackAxisToward(const Train *v, const Train *target)
{
	if (!(v->track & TRACK_BIT_WORMHOLE) && v->track != TRACK_BIT_DEPOT) {
		TrackBits tb = v->track & TRACK_BIT_MASK;
		/* Only trust the track bit when it actually exists on the tile. */
		if (tb != TRACK_BIT_NONE && (TrackdirBitsToTrackBits(GetTileTrackdirBits(v->tile, TransportType::Rail, 0)) & tb) == tb) {
			int bx = target->x_pos - v->x_pos;
			int by = target->y_pos - v->y_pos;
			int best = INT_MIN;
			TileIndexDiffC axis{0, 0};
			for (Track t : SetTrackBitIterator(tb)) {
				Trackdir td = TrackToTrackdir(t);
				for (int i = 0; i < 2; i++) {
					TileIndexDiffC dd = TileIndexDiffCByDir(DiagDirToDir(TrackdirToExitdir(td)));
					int score = dd.x * bx + dd.y * by;
					if (score > best) {
						best = score;
						axis = dd;
					}
					td = ReverseTrackdir(td);
				}
			}
			return axis;
		}
	}
	return CoupleMotionDelta(v);
}

/**
 * Expected centre distance of the chain-adjacent pair (\a a, \a a->Next()).
 * CalcNextVehicleOffset() cannot be used here: for DrivingBackwards vehicles
 * it pairs the vehicle with GetMovingNext() (its chain predecessor), not with
 * its chain successor.
 */
static int CouplePairOffset(const Train *a)
{
	/* [FIX-couple-misalign] The movement pipeline maintains chain-order
	 * FORWARD rounding (la/2 + (lb+1)/2) regardless of the driving direction
	 * (verified by the pxp-spacing probe: backwards-driven pairs sit at the
	 * forward value). The previous IsDrivingBackwards rounding here disagreed
	 * for asymmetric odd/even pairs (1px), so AdvanceWagonsAfterCouple planted
	 * the seam 1px off and the first movement tick snapped it back -- the
	 * visible wagon interpenetration after a couple. */
	return CoupleJointOffset(a->gcache.cached_veh_length, a->Next()->gcache.cached_veh_length, false);
}

/**
 * Move \a v so that its Chebyshev distance to \a target equals \a offset,
 * stepping ONE PIXEL at a time along the track axis that points towards the
 * target (recomputed every step; a stale track bit is repaired from the chain
 * geometry as we go). Single-axis steps follow the track through curves,
 * unlike placing the whole offset in one straight jump, which cuts across
 * curve tiles and lands vehicles off the rail.
 */
void RespaceVehicleTo(Train *v, const Train *target, int offset)
{
	for (int guard = 0; guard < 64; guard++) {
		int dx = target->x_pos - v->x_pos;
		int dy = target->y_pos - v->y_pos;
		int dist = std::max(abs(dx), abs(dy));
		if (dist == offset) return;
		TileIndexDiffC dd = TrackAxisToward(v, target);
		if (dist < offset) {
			dd.x = -dd.x;
			dd.y = -dd.y;
		}
		MoveTrainToPosition(v, v->x_pos + dd.x, v->y_pos + dd.y);
		RepairVehicleTrackBit(v, nullptr);
	}
}

/**
 * Advance the wagons of the consist after coupling, so that they are positioned
 * correctly on the track.
 */
static void AdvanceWagonsAfterCouple(Train *v)
{
	Train *u_first = v->Next();
	/* Target spacing: rounding applies from whichever vehicle enforces this
	 * pair after the merge. Using the approach-phase rounding here would
	 * accept a 1px-off seam that motion never corrects again. */
	int difference = !v->IsDrivingBackwards()
		? CoupleJointOffset(v->gcache.cached_veh_length, u_first->gcache.cached_veh_length, false)
		: CoupleJointOffset(u_first->gcache.cached_veh_length, v->gcache.cached_veh_length, true);

	/* Contact coupling can land a pixel or two inside/outside the partner when
	 * the approach step jumps over the exact-contact sample. Fix the seam by
	 * re-spacing the MOVING half (the chain up to and including v) instead of
	 * the waiting half: the moving consist approached along this very track,
	 * so adjusting it only ever moves it over ground it already occupied -- it
	 * cannot be pushed off the rail end. The waiting half often parks flush
	 * against the platform/rail end, and moving it pushed its far wagon past
	 * the end of the track onto a rail-less tile.
	 * TrainController must not be used here: its movement-time side effects
	 * (line-end checks, station entry, turnarounds) must not run mid-couple,
	 * and with the freshly merged consist's mixed states they resolve into
	 * garbage (invalid directions, off-track wagons). */
	RespaceVehicleTo(v, u_first, difference);

	for (Train *b = v; b->Previous() != nullptr; b = b->Previous()) {
		Train *a = b->Previous();
		RespaceVehicleTo(a, b, CouplePairOffset(a));
	}
}

/**
 * Check whether both ends of the train are standing on the same station
 * platform. Only actual vehicle positions are used; vehicle lengths (NewGRF
 * callback driven) are deliberately not consulted, so articulated parts and
 * non-standard vehicle lengths cannot make the check fail.
 */
bool TrainFitStation(const Train *v)
{
	if (!IsRailStationTile(v->tile)) return false;
	if (!IsRailStationTile(v->Last()->tile)) return false;
	return GetStationIndex(v->tile) == GetStationIndex(v->Last()->tile);
}

/** Station at which a consist can wait for another coupling. */
StationID GetTrainCouplingStation(const Train *v)
{
	v = v->First();
	if (TrainFitStation(v)) return GetStationIndex(v->tile);
	/* Coupling can leave the first engine just beyond the platform. Require
	 * every remaining part on one station, with the engine on the adjacent
	 * rail tile. Do not accept an unrelated train standing outside a station. */
	const Train *unit_main = Train::From(v->GetUnitMain());
	const Train *next = v->GetNextVehicle();
	if (!unit_main->IsEngine() || next == nullptr || !IsPlainRailTile(v->tile) ||
			!IsRailStationTile(next->tile) || DistanceManhattan(v->tile, next->tile) != 1) return StationID::Invalid();
	const StationID station = GetStationIndex(next->tile);
	if (v->Primary()->last_station_visited != station) return StationID::Invalid();
	for (const Train *u = next; u != nullptr; u = u->Next()) {
		if (!IsRailStationTile(u->tile) || GetStationIndex(u->tile) != station) return StationID::Invalid();
	}
	return station;
}

/**
 * Check if the train can decouple at its current position.
 */
static bool CanDecouple(Train *v)
{
	/* Both checks must anchor at the physical chain head: the consist carrier
	 * may sit mid-chain or at the tail, and both TrainFitStation and
	 * CountVehiclesInChain otherwise only see the part trailing the primary. */
	Train *head = v->First();
	if (!TrainFitStation(head)) {
		Debug(desync, 1, "CanDecouple: veh={} fail fitstation tile=({},{}) last=({},{})", v->index, TileX(v->tile), TileY(v->tile), TileX(v->Last()->tile), TileY(v->Last()->tile));
		return false;
	}
	if (CountVehiclesInChain(head) < 2) {
		Debug(desync, 1, "CanDecouple: veh={} fail count={}", v->index, CountVehiclesInChain(head));
		return false;
	}
	/* The cut is counted from the physical chain head, not from the consist
	 * carrier; a primary sitting mid-chain or at the tail still has units
	 * ahead of it, so anchor this check at the head too. */
	if (v->First()->GetNextUnit() == nullptr) {
		Debug(desync, 1, "CanDecouple: veh={} fail no next unit", v->index);
		return false;
	}
	return true;
}

/**
 * Automatically determine how many vehicles to decouple.
 */
static uint GetDecoupleVehicleAuto(Train *v)
{
	/* The cut is applied relative to the physical chain head, so the count has to
	 * start there as well; the consist carrier may sit mid-chain after a couple. */
	v = v->First();
	uint engines_front = 0;
	uint engines_back = 0;
	uint units = 0;
	bool has_wagons = false;
	for (Train *t = v; t != nullptr; t = t->GetNextVehicle()) {
		units++;
		const Train *main = Train::From(t->GetUnitMain());
		if (main->IsEngine()) {
			if (has_wagons) {
				engines_back++;
			} else {
				engines_front++;
			}
		} else {
			has_wagons = true;
		}
	}
	if (engines_front > 0 && has_wagons) return engines_front;
	if (engines_back > 0 && has_wagons) return units - engines_back;
	return 1;
}

/**
 * Find the vehicle where the consist should be split.
 */
Train *GetDecoupleVehicleForCount(Train *v, uint num_decouple)
{
	if (num_decouple == 0) num_decouple = GetDecoupleVehicleAuto(v);
	Train *head = v->First();
	Train *ret = head->GetNextVehicle();
	/* The editor can query single-unit consists, unlike the execution caller. */
	if (ret == nullptr) return nullptr;

	/* The split count keeps that many units in the first part; a consist has
	 * only one fewer internal cut boundary than units, so a count covering the
	 * whole consist means "no split": the entire train stays the first part. */
	uint total_units = 0;
	for (Train *t = head; t != nullptr; t = t->GetNextVehicle()) total_units++;
	if (num_decouple >= total_units) return nullptr;

	/* GetNextVehicle() skips whole multi-part units, so the cut can never land
	 * inside a dual-headed or articulated unit. */
	for (uint i = 1; i < num_decouple && ret->GetNextVehicle() != nullptr; i++) {
		ret = ret->GetNextVehicle();
	}
	std::vector<const Train *> open_dual_heads;
	for (const Train *part = head; part != ret; part = part->Next()) {
		if (!part->IsMultiheaded() || part->other_multiheaded_part == nullptr) continue;
		auto other = std::find(open_dual_heads.begin(), open_dual_heads.end(), part->other_multiheaded_part);
		if (other == open_dual_heads.end()) {
			open_dual_heads.push_back(part);
		} else {
			open_dual_heads.erase(other);
		}
	}
	if (!open_dual_heads.empty()) return nullptr;
	return ret;
}

/** Enumerate representable cuts using exactly the execution order's counting rules. */
std::vector<DecoupleCut> GetDecoupleCuts(Train *v)
{
	std::vector<DecoupleCut> cuts;
	uint count = 1;
	for (Train *next = v->First()->GetNextVehicle(); next != nullptr && count <= 127; next = next->GetNextVehicle(), count++) {
		if (GetDecoupleVehicleForCount(v, count) == next) cuts.push_back({static_cast<uint8_t>(count), next});
	}
	return cuts;
}

static Train *GetDecoupleVehicle(Train *v)
{
	/* Order data lives on the primary vehicle; the unit walk must start at the
	 * physical chain head, which may be a different vehicle when the primary
	 * sits mid-chain after a couple. */
	Train *prim = v->Primary();
	/* The split count lives on the decouple marker behind the current order.
	 * A stale or misaligned index leaves no marker there (the arrival then
	 * counts as a plain stop); report "no cut" instead of dereferencing it. */
	const Order *decouple_order = (prim->orders != nullptr) ? prim->orders->GetOrderAt(prim->cur_implicit_order_index + 1) : nullptr;
	if (decouple_order == nullptr || !decouple_order->IsType(OT_DECOUPLE)) return nullptr;
	return GetDecoupleVehicleForCount(v, decouple_order->GetNumDecouple());
}

/**
 * Try to split off the rear part of the consist.
 */
/**
 * The start/stop check callback is evaluated on the primary vehicle, i.e. the
 * first engine of the (trial) chain; fall back to the chain head for
 * engine-less chains.
 */
static Train *GetStartStopCheckVehicle(Train *head)
{
	for (Train *t = head; t != nullptr; t = t->Next()) {
		if (t->IsEngine()) return t;
	}
	return head;
}

/**
 * Recompute property 0x25 user data (optionally overridden by callback 0x36)
 * for the trial chain, mirroring Train::ConsistChanged, so consist variables
 * such as var 0x42 reflect the hypothetical consist.
 */
static void RefreshTrainUserDefData(Train *head)
{
	for (Train *u = head; u != nullptr; u = u->Next()) {
		u->tcache.user_def_data = GetVehicleProperty(u, PROP_TRAIN_USER_DATA, RailVehInfo(u->engine_type)->user_def_data);
	}
}

/**
 * Make every vehicle's last-loading-station record authoritative before
 * couple/decouple surgery. When the consist does not carry the
 * LastLoadStationSeparate flag only the primary's record is maintained and the
 * per-vehicle values may be stale; propagate the primary's record so that
 * after the split each part keeps a correct, per-vehicle attribution. With
 * the flag already set every vehicle's record is authoritative and is kept.
 */
static void PropagateLastLoadingStation(Train *primary)
{
	if (primary->vehicle_flags.Test(VehicleFlag::LastLoadStationSeparate)) return;
	StationID station = primary->last_loading_station;
	StateTicks tick = primary->last_loading_tick;
	for (Train *w = primary->First(); w != nullptr; w = w->Next()) {
		w->last_loading_station = station;
		w->last_loading_tick = tick;
	}
}

/**
 * Normalize the last-loading-station records of a resulting consist part after
 * couple/decouple surgery: when all vehicles agree on the record the part can
 * keep the compact (non-separate) representation, otherwise per-vehicle records
 * are used via LastLoadStationSeparate so VehicleIncreaseStats attributes each
 * wagon's link to the station it actually loaded at.
 */
static void NormalizeLastLoadingStation(Train *primary)
{
	Train *head = primary->First();
	StationID first = head->last_loading_station;
	bool differ = false;
	for (Train *w = head; w != nullptr; w = w->Next()) {
		if (w->last_loading_station != first) {
			differ = true;
			break;
		}
	}
	if (!differ) {
		primary->last_loading_station = first;
		primary->last_loading_tick = head->last_loading_tick;
		primary->vehicle_flags.Reset(VehicleFlag::LastLoadStationSeparate);
	} else {
		primary->vehicle_flags.Set(VehicleFlag::LastLoadStationSeparate);
	}
}

/**
 * RAII guard for trial consist surgery (couple/decouple arrangement
 * validation). Snapshots the per-vehicle NewGRF caches of the involved
 * chains and the game random seed; at scope end the caches are restored and
 * a seed shift (NewGRF callbacks consuming randomness during the trial) is
 * rolled back, so every peer stays deterministic even in release builds
 * where the former assert would be a no-op.
 */
struct TrialConsistGuard {
	struct TrialCache {
		Train *vehicle;
		NewGRFCache grf_cache;
		uint8_t user_def_data;
		uint8_t cached_veh_flags;
		Vehicle *unit_main;
		Train *primary;
		uint8_t subtype;
		Direction direction;
		bool driving_backwards;
		bool flipped;
	};

	std::vector<TrialCache> caches;
	SavedRandomSeeds seeds;
	bool restore_structure = true;

	TrialConsistGuard()
	{
		SaveRandomSeeds(&this->seeds);
	}

	void SnapshotChain(const TrainList &chain)
	{
		for (Train *t : chain) {
			this->caches.push_back({t, t->grf_cache, t->tcache.user_def_data, t->vcache.cached_veh_flags,
					t->HasUnitMain() ? t->GetUnitMain() : nullptr, t->Primary(), t->subtype, t->direction,
					t->vehicle_flags.Test(VehicleFlag::DrivingBackwards), t->flags.Test(VehicleRailFlag::Flipped)});
		}
	}

	~TrialConsistGuard()
	{
		for (const TrialCache &cache : this->caches) {
			cache.vehicle->grf_cache = cache.grf_cache;
			cache.vehicle->tcache.user_def_data = cache.user_def_data;
			cache.vehicle->vcache.cached_veh_flags = cache.cached_veh_flags;
			if (this->restore_structure) {
				cache.vehicle->SetUnitMain(cache.unit_main);
				cache.vehicle->SetPrimary(cache.primary);
				cache.vehicle->subtype = cache.subtype;
				cache.vehicle->direction = cache.direction;
				cache.vehicle->vehicle_flags.Set(VehicleFlag::DrivingBackwards, cache.driving_backwards);
			}
			cache.vehicle->flags.Set(VehicleRailFlag::Flipped, cache.flipped);
		}
		if (this->seeds.random.state[0] != _random.state[0] || this->seeds.random.state[1] != _random.state[1]) {
			Debug(desync, 0, "Trial consist surgery shifted the random seed; restoring it");
			RestoreRandomSeeds(this->seeds);
		}
	}
};

static bool TryTrainDecouple(Train *v, Train *u)
{
	/* v and u are the physical chain heads of the front and rear parts of the
	 * split. The consist carrier (primary) may sit anywhere inside either
	 * part, so the backup must cover the whole physical chain; backing up from
	 * the primary would miss the head-side vehicles and RestoreTrainBackup
	 * would permanently sever them on the failure path. */
	TrainList original_src;

	TrialConsistGuard trial_guard;

	MakeTrainBackup(original_src, v);
	trial_guard.SnapshotChain(original_src);

	Train *first_param = nullptr;

	ArrangeTrains(&first_param, nullptr, &v, u, true, false);

	/* The NewGRF caches still hold values of the pre-split consist; invalidate
	 * them so the start/stop check sees the would-be resulting parts. */
	v->InvalidateNewGRFCacheOfChain();
	u->InvalidateNewGRFCacheOfChain();
	RefreshTrainUserDefData(v);
	RefreshTrainUserDefData(u);

	bool ok = true;
	CommandCost ret = ValidateTrains(nullptr, u, v, v, true);
	ok &= !ret.Failed();

	bool cc_u = u->CanConsistChange(CCF_ARRANGE_CHECK);
	bool cc_v = v->CanConsistChange(CCF_ARRANGE_CHECK);
	ok &= cc_u && cc_v;

	/* Both resulting parts must satisfy the NewGRF start/stop check. */
	CommandCost cb_front = CheckVehicleStartStopCallback(GetStartStopCheckVehicle(v));
	CommandCost cb_rear = CheckVehicleStartStopCallback(GetStartStopCheckVehicle(u));
	ok &= !cb_front.Failed() && !cb_rear.Failed();

	if (!ok) {
		/* Restore the train we had. */
		RestoreTrainBackup(original_src);
		v->ConsistChanged(CCF_ARRANGE_STATION);
		return false;
	}
	/* This caller intentionally keeps a successful split for DecoupleTrain. */
	trial_guard.restore_structure = false;
	/* Splitting creates a new train front; invalidate the tick caches or the new front will not be ticked. */
	InvalidateVehicleTickCaches();
	return true;
}

/** Which parts of the consist should load after decoupling. */

static bool IsDecoupleWaitMode(OrderDecoupleOrdersFlags mode)
{
	return mode == ODOF_LOAD_AND_WAIT || mode == ODOF_WAIT_FOR_COUPLE;
}

/** Prepare both copies before splitting or changing either part's orders. */
StringID PrepareDecoupleWaitOrders(const Train *v, std::unique_ptr<OrderList> (&copies)[2])
{
	const Order *decouple = v->GetOrder(v->cur_implicit_order_index + 1);
	if (decouple == nullptr || !decouple->IsType(OT_DECOUPLE)) return STR_DECOUPLE_DIAGNOSTIC_ORDER;
	const OrderDecoupleOrdersFlags modes[] = {decouple->GetDecoupleFirstOrdersType(), decouple->GetDecoupleSecondOrdersType()};
	if (!IsDecoupleWaitMode(modes[0]) && !IsDecoupleWaitMode(modes[1])) return STR_NULL;
	if (v->GetNumOrders() >= MAX_VEH_ORDER_ID) return STR_ERROR_TOO_MANY_ORDERS;

	uint needed = 0;
	for (uint i = 0; i < 2; ++i) {
		if (IsDecoupleWaitMode(modes[i])) {
			++needed;
		} else if (modes[i] == ODOF_EXECUTE_SCHEDULE || modes[i] == ODOF_LOAD_AND_SCHEDULE) {
			const OrderList *schedule = OrderList::GetIfValid(i == 0 ? decouple->GetDecoupleFirstScheduleID() : decouple->GetDecoupleSecondScheduleID());
			if (schedule != nullptr && schedule->IsPlayerCreated()) ++needed;
		}
	}
	if (!OrderList::CanAllocateItem(needed)) return STR_ERROR_NO_MORE_SPACE_FOR_ORDERS;

	for (uint i = 0; i < 2; ++i) {
		if (!IsDecoupleWaitMode(modes[i])) continue;
		copies[i].reset(OrderList::Create());
		OrderList *copy = copies[i].get();
		copy->GetOrderVector().reserve(v->GetNumOrders() + 1);
		for (const Order *order : v->Orders()) copy->GetOrderVector().emplace_back(*order);
		copy->GetScheduledDispatchScheduleSet() = v->orders->GetScheduledDispatchScheduleSet();
		copy->SetRouteOverlayColour(v->orders->GetRouteOverlayColour());
		copy->SetDispatchEnabled(v->vehicle_flags.Test(VehicleFlag::ScheduledDispatch));
		copy->SetSeparationEnabled(v->vehicle_flags.Test(VehicleFlag::TimetableSeparation));
	}
	return STR_NULL;
}

/** Install the independent copy and insert a new wait after this decouple, never deduplicating. */
static void InsertDecoupleWaitOrder(Train *v, std::unique_ptr<OrderList> &copy, VehicleOrderID wait_index)
{
	SetDecoupleWaitOrderList(v, copy.release());
	Order wait;
	wait.MakeWaitCouple();
	InsertOrder(v, std::move(wait), wait_index);
}


/**
 * Make a decoupled train part adopt a player-created order list as its own
 * schedule: both its orders and its primary order list become the schedule.
 * @param part the train part
 * @param schedule_id the schedule to adopt
 * @param load_at_station when valid, load/unload at this station on the first
 *                       pass only before running the schedule
 * @param load_order the order the part is running at the decouple station; when
 *                   given, its settings are copied onto the one-off station
 *                   order of the wrapper instead of starting from a bare one
 * @return true when the schedule was adopted
 */
static bool AdoptDecoupleSchedule(Train *part, OrderListID schedule_id, StationID load_at_station = StationID::Invalid(), const Order *load_order = nullptr)
{
	OrderList *ol = OrderList::GetIfValid(schedule_id);
	if (ol == nullptr || !ol->IsPlayerCreated() || !ol->IsVisibleToCompany(part->owner)) {
		return false;
	}


	/* Build a wrapper schedule holding an execute-schedule order for the target
	 * schedule; the part will run the target through the regular
	 * execute-schedule mechanism, restarting it after every full pass. The
	 * wrapper is a vehicle-owned list, so it is freed automatically when the
	 * part's orders are replaced or removed. */
	std::vector<Order> wrapper_orders;

	const bool load_before_schedule = load_at_station != StationID::Invalid();
	if (load_before_schedule) {
		Order station_order;
		if (load_order != nullptr && load_order->IsType(OT_GOTO_STATION)) {
			/* The one-off load has to do what the player configured for the decouple
			 * station itself, so take the whole order as a template: load types,
			 * per-cargo load/unload settings, refit cargo (including auto-refit),
			 * unload type, stop location, timings and any trace/restrict slot. */
			station_order = *load_order;
			station_order.SetDestination(load_at_station);
		} else {
			station_order.MakeGoToStation(load_at_station);
		}
		/* Splitting is driven by the OT_DECOUPLE order which follows in the original
		 * list; this copy lives in a list of its own and must not split again. */
		station_order.SetDecouple(ODF_NOTHING);
		station_order.SetNumDecouple(0);
		wrapper_orders.push_back(std::move(station_order));
	}

	/* Also the jump target of the unconditional jump appended below. */
	VehicleOrderID execute_index = static_cast<VehicleOrderID>(wrapper_orders.size());

	Order execute_order;
	execute_order.MakeExecuteSchedule();
	execute_order.SetDestination(ol->index);
	wrapper_orders.push_back(std::move(execute_order));

	if (load_before_schedule) {
		/* Jump straight back to the execute-schedule order, so the station order
		 * above is only run on the first pass. */
		Order repeat_order;
		repeat_order.MakeConditional(execute_index);
		repeat_order.SetConditionVariable(OrderConditionVariable::Unconditionally);
		wrapper_orders.push_back(std::move(repeat_order));
	}

	if (!OrderList::CanAllocateItem()) return false;

	DeleteVehicleOrders(part, false, true);

	part->orders = OrderList::Create(std::move(wrapper_orders), part);
	part->primary_order = part->orders->index;
	part->primary_order_index = INVALID_VEH_ORDER_ID;
	part->execute_schedule_stack.clear();
	part->cur_implicit_order_index = 0;
	part->cur_real_order_index = 0;
	part->cur_timetable_order_index = INVALID_VEH_ORDER_ID;
	part->UpdateRealOrderIndex();
	part->current_order.Free();
	if (!load_before_schedule) {
		/* The part starts a fresh schedule with no one-off station stop, so the
		 * per-leg timer must not carry the elapsed inbound leg of the decouple
		 * station into the first leg of the adopted schedule. The
		 * load-before-schedule variant keeps it: its copied station order is
		 * the arrival order, and BeginLoading still has to book the inbound
		 * leg onto it. */
		part->current_order_time = 0;
		part->current_loading_time = 0;
	}

	/* The wrapper mirrors the part's dispatch/separation state while it is the home list. */
	part->orders->SetDispatchEnabled(part->vehicle_flags.Test(VehicleFlag::ScheduledDispatch));
	part->orders->SetSeparationEnabled(part->vehicle_flags.Test(VehicleFlag::TimetableSeparation));
	return true;
}



/** Assign each part's orders; return the parts requesting a post-arrival lateness reset. */
uint8_t SplitOrders(Train *v, Train *u, uint8_t &load_trains, std::unique_ptr<OrderList> (&wait_orders)[2], uint32_t inbound_time)
{
	/* The decouple marker has to sit behind the implicit index, which every
	 * caller guarantees (see #TrainEnterStation). Re-check it instead of
	 * dereferencing a possibly stale index: without the marker the two parts'
	 * order modes cannot be determined at all. */
	const Order *decouple_marker = (v->orders != nullptr) ? v->orders->GetOrderAt(v->cur_implicit_order_index + 1) : nullptr;
	if (decouple_marker == nullptr || !decouple_marker->IsType(OT_DECOUPLE)) {
		Debug(desync, 1, "SplitOrders: veh={} has no decouple marker behind order {}", v->index, v->cur_implicit_order_index);
		return 0;
	}
	uint8_t reset_lateness = 0;
	const Order after_decouple_flags = *decouple_marker;
	const VehicleOrderID wait_index = v->cur_implicit_order_index + 2;
	const BaseConsist order_state = *v;


	if (v != u) {
		/* Both parts start with the inbound offset; only a successful independent
		 * schedule change below may request its post-arrival reset. */
		u->lateness_counter = order_state.lateness_counter;
		switch (after_decouple_flags.GetDecoupleSecondOrdersType()) {
			case ODOF_LOAD_AND_WAIT:
			case ODOF_KEEP_ORDERS:
				load_trains |= DECOUPLE_LOAD_SECOND;
				[[fallthrough]];
			case ODOF_WAIT_FOR_COUPLE:
			case ODOF_KEEP_ORDERS_NO_LOAD:
				u->orders = v->orders;
				u->primary_order = v->primary_order;
				u->primary_order_index = v->primary_order_index;
				u->execute_schedule_stack = v->execute_schedule_stack;
				/* A home list that no longer exists would leave the new part looking like it is
				 * on an execute-schedule detour; the next list allocated could then reuse that
				 * index and hand it a foreign list. Fall back to the list the part actually runs. */
				if (u->primary_order != OrderListID::Invalid() && OrderList::GetIfValid(u->primary_order) == nullptr) {
					u->primary_order = (u->orders != nullptr) ? u->orders->index : OrderListID::Invalid();
					u->primary_order_index = INVALID_VEH_ORDER_ID;
				}
				u->AddToShared(v);
				u->cur_real_order_index = v->cur_real_order_index;
				u->cur_implicit_order_index = v->cur_implicit_order_index;
				u->current_order = v->current_order;
				/* Everything that describes where the consist stands in the order list has to be
				 * taken over together with the real order index. Leaving any of it behind points
				 * the new part at the state it had before the split: the timetable index in
				 * particular then still refers to a pre-split order, and asserting that the two
				 * match (see #UpdateVehicleTimetable) aborts the game on the next station the
				 * part loads at. This applies to every mode that shares the list, not only to
				 * the ones that insert a wait order. */
				u->cur_timetable_order_index = order_state.cur_timetable_order_index;
				/* The per-leg timer is not taken over wholesale: the elapsed
				 * inbound leg to the decouple station is booked once, either
				 * already by TrainEnterStation or now by BeginLoading when this
				 * part loads here. When the part does not load, drop the timer
				 * so the leg is not charged to its first post-split leg. */
				u->current_order_time = (load_trains & DECOUPLE_LOAD_SECOND) != 0 ? inbound_time : 0;
				u->lateness_counter = order_state.lateness_counter;
				u->timetable_start = order_state.timetable_start;
				u->dispatch_records = order_state.dispatch_records;
				for (VehicleFlag flag : {VehicleFlag::TimetableStarted, VehicleFlag::AutofillTimetable,
						VehicleFlag::AutofillPreserveWaitTime, VehicleFlag::ScheduledDispatch,
						VehicleFlag::TimetableSeparation, VehicleFlag::AutomateTimetable}) {
					u->vehicle_flags.Set(flag, order_state.vehicle_flags.Test(flag));
				}
				if (wait_orders[1] != nullptr) {
					InsertDecoupleWaitOrder(u, wait_orders[1], wait_index);
				}
				if (!(load_trains & DECOUPLE_LOAD_SECOND)) u->IncrementImplicitOrderIndex();
				break;
			case ODOF_EXECUTE_SCHEDULE:
				if (AdoptDecoupleSchedule(u, after_decouple_flags.GetDecoupleSecondScheduleID()) && v != u && !after_decouple_flags.GetDecoupleKeepLateness(false)) reset_lateness |= DECOUPLE_LOAD_SECOND;
				break;
			case ODOF_LOAD_AND_SCHEDULE:
				/* Load/unload at this station once, then run the schedule, skipping
				 * the station order on all later passes. */
				load_trains |= DECOUPLE_LOAD_SECOND;
				if (AdoptDecoupleSchedule(u, after_decouple_flags.GetDecoupleSecondScheduleID(), v->last_station_visited, &v->current_order) && v != u && !after_decouple_flags.GetDecoupleKeepLateness(false)) reset_lateness |= DECOUPLE_LOAD_SECOND;
				break;
			default: NOT_REACHED();
		}
		ProcessOrders(u);
	}


	for (const Train *w = u->First(); w != nullptr; w = w->Next()) {
	}

	/* A failed physical split must not insert a wait or replace the original list. */
	if (v != u && wait_orders[0] != nullptr) InsertDecoupleWaitOrder(v, wait_orders[0], wait_index);
	switch (after_decouple_flags.GetDecoupleFirstOrdersType()) {
		case ODOF_LOAD_AND_WAIT:
		case ODOF_KEEP_ORDERS:
			load_trains |= DECOUPLE_LOAD_FIRST;
			break;
		case ODOF_WAIT_FOR_COUPLE:
		case ODOF_KEEP_ORDERS_NO_LOAD:
			v->IncrementImplicitOrderIndex();
			break;
		case ODOF_EXECUTE_SCHEDULE:
			if (AdoptDecoupleSchedule(v, after_decouple_flags.GetDecoupleFirstScheduleID()) && v != u && !after_decouple_flags.GetDecoupleKeepLateness(true)) reset_lateness |= DECOUPLE_LOAD_FIRST;
			break;
		case ODOF_LOAD_AND_SCHEDULE:
			/* Load/unload at this station once, then run the schedule, skipping
			 * the station order on all later passes. */
			load_trains |= DECOUPLE_LOAD_FIRST;
			if (AdoptDecoupleSchedule(v, after_decouple_flags.GetDecoupleFirstScheduleID(), v->last_station_visited, &v->current_order) && v != u && !after_decouple_flags.GetDecoupleKeepLateness(true)) reset_lateness |= DECOUPLE_LOAD_FIRST;
			break;
		default: NOT_REACHED();
	}
	ProcessOrders(v);
	return reset_lateness;
}

/**
 * Decouple the rear part of the consist at the current station.
 * @param v The consist.
 * @param[out] consist_in_rear Set to true when the consist carrier ended up in
 *                             the rear physical part (engine-less head side),
 *                             i.e. the decoupled part is the physical front.
 * @return The head of the decoupled part, or \c v if decoupling failed.
 */
Train *DecoupleTrain(Train *v, bool &consist_in_rear, StringID &failure_reason)
{
	v = v->Primary();
	consist_in_rear = false;
	failure_reason = STR_NULL;
	if (!CanDecouple(v)) {
		failure_reason = !TrainFitStation(v) ? STR_DECOUPLE_DIAGNOSTIC_PLATFORM : STR_DECOUPLE_DIAGNOSTIC_TOO_SHORT;
		Debug(desync, 1, "DecoupleTrain: veh={} CANNOT decouple tile=({},{})", v->index, TileX(v->tile), TileY(v->tile));
		return v;
	}

	Train *u = GetDecoupleVehicle(v);
	{
		const Order *after_decouple = (v->orders != nullptr) ? v->orders->GetOrderAt(v->cur_implicit_order_index + 1) : nullptr;
		Debug(desync, 1, "DecoupleTrain: veh={} split u={} num={}", v->index, u != nullptr ? u->index.base() : -1,
				after_decouple != nullptr ? after_decouple->GetNumDecouple() : -1);
	}
	if (u == nullptr) {
		/* GetDecoupleVehicle returns null both when there is no decouple marker
		 * behind the current order and when the requested split count covers the
		 * whole consist. In the latter case the split is vacuous: keep the whole
		 * train as the first part and report success instead of a cut failure. */
		const Order *after_decouple = (v->orders != nullptr) ? v->orders->GetOrderAt(v->cur_implicit_order_index + 1) : nullptr;
		if (after_decouple != nullptr && after_decouple->IsType(OT_DECOUPLE)) {
			uint num_decouple = after_decouple->GetNumDecouple();
			if (num_decouple == 0) num_decouple = GetDecoupleVehicleAuto(v);
			uint total_units = 0;
			for (Train *t = v->First(); t != nullptr; t = t->GetNextVehicle()) total_units++;
			if (num_decouple >= total_units) {
				v->decouple_part = 1;
				return v;
			}
		}
		failure_reason = STR_DECOUPLE_DIAGNOSTIC_CUT;
		return v;
	}

	/* The physical chain head of the front part. The consist carrier may sit
	 * mid-chain (engine-less head side), so all surgery is anchored at the
	 * physical heads, never at the primary. */
	Train *front_head = v->First();
	const GroupID inherited_group = v->group_id;
	std::vector<Train *> slot_holders;
	std::vector<TraceRestrictSlotID> inherited_slots;
	for (Train *part = front_head; part != nullptr; part = part->Next()) {
		std::vector<TraceRestrictSlotID> slots;
		TraceRestrictGetVehicleSlots(part->index, slots);
		if (slots.empty()) continue;
		for (TraceRestrictSlotID slot : slots) {
			if (std::find(inherited_slots.begin(), inherited_slots.end(), slot) == inherited_slots.end()) inherited_slots.push_back(slot);
		}
		slot_holders.push_back(part);
	}

	/* Preserve each wagon's last-loading-station attribution across the split
	 * instead of dropping it: the decoupled part keeps recording links from
	 * the station it actually loaded at on its very first trip. */
	PropagateLastLoadingStation(v);

	if (!TryTrainDecouple(front_head, u)) {
		failure_reason = STR_DECOUPLE_DIAGNOSTIC_ARRANGEMENT;
		return v;
	}

	/* Which physical part hosts the consist carrier? When the head side is
	 * engine-less, the primary sits in the rear part and the roles swap: the
	 * front part is the one that gets split off, while the primary's part
	 * keeps the original orders. */
	for (Train *w = u; w != nullptr; w = w->Next()) {
		if (w == v) {
			consist_in_rear = true;
			break;
		}
	}
	Train *dec_head = consist_in_rear ? front_head : u;
	Train *consist_head = consist_in_rear ? u : front_head;
	/* This train stays at the station and keeps on running the original orders. */
	v->decouple_part = 1;
	/* The decoupled part now runs a separate schedule: mark it as the second part. */
	dec_head->decouple_part = 2;

	/* The new part has its own logical controller, independent of its physical
	 * head. Keep the head marker even when the engine carrier sits behind it. */
	Train *dec_primary = nullptr;
	for (Train *w = dec_head; w != nullptr; w = w->Next()) {
		if (w->IsEngine()) { dec_primary = w; break; }
		if (dec_primary == nullptr && !w->IsArticulatedPart() && !w->IsRearDualheaded()) dec_primary = w;
	}
	if (dec_primary == nullptr) dec_primary = dec_head;
	if (!dec_primary->IsEngine()) dec_primary->SetFrontWagon();
	BindTrainPrimary(dec_head, dec_primary);
	BindTrainPrimary(consist_head, v);

	/* Existing claims can belong to a marked physical head rather than the
	 * controller. Move those claims to their resulting part's primary without
	 * releasing/reacquiring them or changing ordinary slot capacity rules. */
	for (Train *holder : slot_holders) {
		Train *primary = holder->Primary();
		if (holder == primary) continue;
		TraceRestrictTransferVehicleOccupantInAllSlots(holder->index, primary->index);
		holder->vehicle_flags.Reset(VehicleFlag::HaveSlot);
		primary->vehicle_flags.Set(VehicleFlag::HaveSlot);
	}
	/* Slot claims are inherited by both parts regardless of company ownership;
	 * unlike vehicle groups, they are not tied to the original carrier's owner. */
	for (TraceRestrictSlotID slot_id : inherited_slots) {
		TraceRestrictSlot *slot = TraceRestrictSlot::Get(slot_id);
		for (Train *primary : {v, dec_primary}) {
			slot->Occupy(primary);
		}
	}
	NormalizeLastLoadingStation(dec_primary);
	NormalizeLastLoadingStation(v);
	dec_primary->decouple_part = 2;
	SetTrainGroupID(v, inherited_group);
	SetTrainGroupID(dec_primary, dec_primary->owner == v->owner ? inherited_group : DEFAULT_GROUP);

	/* The old carrier never changed identity, so only the new identity is added. */

	/* Normalise both parts from their physical heads so the primary pointers
	 * of the head-side vehicles (which sit before the primary) are refreshed
	 * too; normalising from the mid-chain primary would leave them stale. */
	NormaliseTrainHead(dec_head, CCF_COUPLE);
	NormaliseTrainHead(consist_head, CCF_COUPLE);
	GroupStatistics::CountVehicle(dec_primary, 1);

	/* The split created a new chain front; invalidate the vehicle tick caches
	 * so both parts are ticked from now on (same as couple/flip do). */
	InvalidateVehicleTickCaches();

	InvalidateWindowClassesData(WindowClass::TrainList);


	dec_head->vehstatus.Reset(VehState::Stopped);
	v->vehstatus.Reset(VehState::Stopped);
	RecordSyncEvent(NSRE_COUPLE);
	return dec_head;
}

/**
 * Check whether coupling the moving consist onto the waiting consist would
 * produce a valid train, using the same backup/arrange/validate/restore logic
 * as depot vehicle moves (CmdMoveRailVehicle). No change is kept.
 * @param v_phys physical chain head of the moving train.
 * @param u_phys physical chain head of the waiting train.
 * @return true if the merged consist passes attachment validation.
 */
static bool CoupleOrderLoadOk(const Order &order, const Train *t)
{
	/* The expensive consist scan below only matters when the order actually
	 * constrains the load; ODC_ANY must not pay for it (this check runs per
	 * candidate inside the couple pathfinder). */
	switch (order.GetCoupleLoad()) {
		case ODC_ANY: return true;
		case ODC_IS_EMPTY:
		case ODC_IS_FULL: break;
		default: NOT_REACHED();
	}

	struct ConsistPart {
		uint32_t stored;
		uint16_t cargo_cap;
		CargoType cargo_type;
		uint32_t carried_tonnes;
	};
	std::vector<ConsistPart> parts;
	for (const Train *u = t->First(); u != nullptr; u = u->Next()) {
		parts.push_back({u->cargo.StoredCount(), u->cargo_cap, u->cargo_type, 0});
	}

	/* RVTransportGetPartCargoAmount() walks every vehicle in the game once per
	 * consist part, which is O(chain * vehicles) per candidate and stalls the
	 * game when the couple pathfinder probes a station platform. Gather the
	 * tonnes carried on all parts of this consist in a single global pass
	 * instead; the result is identical. */
	{
		btree::btree_map<uint32_t, size_t> host_slot;
		size_t i = 0;
		for (const Train *u = t->First(); u != nullptr; u = u->Next(), i++) {
			host_slot[u->index.base()] = i;
		}
		for (const Vehicle *v : Vehicle::Iterate()) {
			if ((v->rv_transport_flags & RVTF_TRANSPORTED) == 0) continue;
			auto it = host_slot.find(v->transported_host_part.base());
			if (it == host_slot.end()) continue;
			parts[it->second].carried_tonnes += v->transported_weight;
		}
	}

	uint cargo = 0;
	uint capacity = 0;
	for (const ConsistPart &p : parts) {
		uint amount = p.stored;
		if (p.carried_tonnes != 0 && IsValidCargoType(p.cargo_type)) {
			const CargoSpec *cs = CargoSpec::Get(p.cargo_type);
			if (cs->weight != 0) {
				amount = std::min<uint32_t>(p.stored + p.carried_tonnes * 16 / cs->weight, p.cargo_cap);
			}
		}
		cargo += amount;
		capacity += p.cargo_cap;
	}

	switch (order.GetCoupleLoad()) {
		case ODC_IS_EMPTY: return cargo == 0;
		case ODC_IS_FULL: return capacity > 0 && cargo == capacity;
		default: NOT_REACHED();
	}
}

static bool CoupleCargoOk(const Order &order, const Train *t)
{
	if (!order.HasCoupleCargoType()) return true;
	CargoType cargo_type = order.GetCoupleCargoType();
	for (const Train *v = t; v != nullptr; v = v->Next()) {
		if (v->cargo_type == cargo_type && v->cargo_cap > 0) return true;
	}
	return false;
}

static bool CoupleNumOk(const Order &order, const Train *t)
{
	if (order.GetNumCouple() == 0) return true;

	uint count = 0;
	for (const Train *u = t->First(); u != nullptr; u = u->GetNextUnit()) count++;
	return order.GetNumCouple() == count;
}

static bool CoupleSlotOk(const Order &order, const Train *t)
{
	TraceRestrictSlotID slot = order.GetCoupleSlot();
	if (slot == TraceRestrictSlotID::Invalid()) return true;
	const TraceRestrictSlot *rs = TraceRestrictSlot::GetIfValid(slot);
	if (rs == nullptr) return false;
	return rs->IsOccupant(t->index);
}

static bool CoupleSlotGroupOk(const Order &order, const Train *t)
{
	TraceRestrictSlotGroupID group = order.GetCoupleSlotGroup();
	if (group == TraceRestrictSlotGroupID::Invalid()) return true;
	const TraceRestrictSlotGroup *sg = TraceRestrictSlotGroup::GetIfValid(group);
	if (sg == nullptr) return false;
	return TraceRestrictIsVehicleInSlotGroup(sg, t->owner, t);
}

static bool CoupleGroupOk(const Order &order, const Train *t)
{
	GroupID group = order.GetCoupleGroup();
	if (group == GroupID::Invalid()) return true;
	/* Hierarchical: a consist placed in a sub-group of the selected group matches too. */
	return GroupIsInGroup(t->group_id, group);
}

static bool WaitCouplePartnerSlotOk(const Order &wait_order, const Train *t)
{
	TraceRestrictSlotID slot = wait_order.GetWaitCouplePartnerSlot();
	if (slot == TraceRestrictSlotID::Invalid()) return true;
	const TraceRestrictSlot *rs = TraceRestrictSlot::GetIfValid(slot);
	if (rs == nullptr) return false;
	return rs->IsOccupant(t->index);
}

static bool WaitCouplePartnerSlotGroupOk(const Order &wait_order, const Train *t)
{
	TraceRestrictSlotGroupID group = wait_order.GetWaitCouplePartnerSlotGroup();
	if (group == TraceRestrictSlotGroupID::Invalid()) return true;
	const TraceRestrictSlotGroup *sg = TraceRestrictSlotGroup::GetIfValid(group);
	if (sg == nullptr) return false;
	return TraceRestrictIsVehicleInSlotGroup(sg, t->owner, t);
}

static bool WaitCouplePartnerGroupOk(const Order &wait_order, const Train *t)
{
	GroupID group = wait_order.GetWaitCouplePartnerGroup();
	if (group == GroupID::Invalid()) return true;
	/* Hierarchical: a consist placed in a sub-group of the selected group matches too. */
	return GroupIsInGroup(t->group_id, group);
}

/** Check the waiting train's active service number against the couple order's restriction. */
static bool CoupleServiceNumberOk(const Order &order, const Train *t)
{
	const std::string_view required = order.GetCoupleServiceNumber();
	if (required.empty()) return true;
	const std::string active = t->GetActiveServiceNumber();
	return std::string_view(active) == required;
}

static bool CoupleStationOk(const Order &order, TileIndex contact_tile)
{
	if (!order.HasCoupleStation()) return true;
	return IsRailStationTile(contact_tile) && GetStationIndex(contact_tile) == order.GetCoupleStation();
}

CoupleCandidateResult GetCoupleVolatileConditionResult(const Train *moving, const Order &order,
		Train *rep, TileIndex contact_tile);

/** The couple order backing an active approach or an early checkpoint claim. */
const Order *GetClaimCoupleOrder(const Train *moving)
{
	/* An unscheduled depot trip overrides the route that backed an early claim. */
	if (moving->current_order.IsType(OT_GOTO_DEPOT) && !moving->current_order.GetDepotOrderType().Test(OrderDepotTypeFlag::PartOfOrders)) return nullptr;
	if (moving->current_order.IsType(OT_GOTO_COUPLE)) return &moving->current_order;
	if (moving->orders == nullptr) return nullptr;
	auto find_order = [](const OrderList *list, VehicleOrderID position) -> const Order * {
		if (list == nullptr) return nullptr;
		for (VehicleOrderID i = position; i < list->GetNumOrders(); ++i) {
			const Order *order = list->GetOrderAt(i);
			if (order == nullptr || !order->IsType(OT_GOTO_COUPLE) || !order->HasCoupleCheckPoint()) continue;
			for (VehicleOrderID j = 0; j <= position; ++j) {
				const Order *earlier = list->GetOrderAt(j);
				if (earlier != nullptr && (earlier->IsType(OT_GOTO_STATION) || earlier->IsType(OT_GOTO_WAYPOINT)) &&
						earlier->GetDestination().ToStationID() == order->GetCoupleCheckPoint()) return order;
			}
		}
		return nullptr;
	};
	if (const Order *order = find_order(moving->orders, moving->cur_real_order_index)) return order;
	for (auto it = moving->execute_schedule_stack.rbegin(); it != moving->execute_schedule_stack.rend(); ++it) {
		if (const Order *order = find_order(OrderList::GetIfValid(OrderListID{uint16_t(*it)}), *it >> 16)) return order;
	}
	if (moving->IsExecutingSchedule()) {
		if (const Order *order = find_order(OrderList::GetIfValid(moving->primary_order), moving->primary_order_index)) return order;
	}
	return nullptr;
}

/** Leave an early couple checkpoint when a depot trip overrides the schedule. */
void CancelTrainCoupleCheckpoint(Train *train)
{
	train = train->Primary();
	if (Train *target = Train::GetIfValid(train->couple_target); target != nullptr) {
		Train *carrier = target->Primary();
		if (carrier->couple_claimant == train->index) {
			carrier->couple_claimant = VehicleID::Invalid();
			carrier->couple_claim_cost = 0;
		}
	}
	train->couple_target = VehicleID::Invalid();
	/* Cancel only the reversal belonging to the overridden checkpoint. */
	if (train->flags.Test(VehicleRailFlag::CoupleCheckReversePending)) train->reverse_distance = 0;
	train->flags.Reset({VehicleRailFlag::CoupleCheckReversePending, VehicleRailFlag::CoupleCheckReservationReleased, VehicleRailFlag::CoupleDirectionChecked});
}

/**
 * Query whether the train is currently held by (or has claimed a partner for)
 * the early couple check at the checkpoint belonging to \a order.
 * Read-only probe for GUI diagnostics; no claim or reservation state is changed.
 * @param moving the consist in question.
 * @param order the GOTO_COUPLE order of \a moving that owns the check point.
 * @return true when the early check backed by \a order is engaged.
 */
bool IsCoupleCheckpointEngaged(const Train *moving, const Order *order)
{
	Train *prim = moving->Primary();
	if (GetClaimCoupleOrder(const_cast<Train *>(prim)) != order) return false;
	/* Engaged once a partner is claimed, or the checkpoint hold has taken
	 * effect: the forward reservation was released waiting for a partner,
	 * or a checkpoint reversal is still pending. */
	return prim->couple_target != VehicleID::Invalid()
			|| prim->flags.Test(VehicleRailFlag::CoupleCheckReservationReleased)
			|| prim->flags.Test(VehicleRailFlag::CoupleCheckReversePending);
}

/**
 * Check a waiting consist's exclusive claim.
 * Changing the target or any volatile order condition invalidates the claim.
 */
Train *GetValidCoupleClaimant(const Train *carrier)
{
	Train *claimant = Train::GetIfValid(carrier->couple_claimant);
	if (claimant == nullptr) return nullptr;

	/* The claim only counts while the claimant is still approaching this
	 * consist; anything else (order left, different target, crash) releases it. */
	const Order *couple_order = GetClaimCoupleOrder(claimant);
	if (couple_order == nullptr) return nullptr;
	if (claimant->vehstatus.Test(VehState::Crashed)) return nullptr;
	Train *tgt = Train::GetIfValid(claimant->couple_target);
	if (tgt == nullptr || tgt->Primary() != carrier) return nullptr;

	/* The partner can stop qualifying at any time while we approach it (most
	 * notably it can lose its 路签 to another consist, or fill up). Note this
	 * must not go through #GetCoupleCandidateResult: that one asks
	 * #CoupleClaimBlocks, which lands back here. */
	CoupleCandidateResult vol = GetCoupleVolatileConditionResult(claimant, *couple_order, tgt->First(), tgt->tile);
	if (vol != CoupleCandidateResult::Valid) {
		/* Queries are also made by GUI diagnostics and NewGRF inspectors.
		 * Report an invalid claim without changing simulation state; the game
		 * tick's speed update / target selection performs the actual cleanup. */
		return nullptr;
	}

	return claimant;
}

/**
 * Get the couple target this moving consist has already validly claimed, if any.
 * A consist that has claimed a waiting train keeps approaching it instead of
 * re-running the couple pathfinder and re-claiming every tick; only when the
 * claim is stale (target gone, not waiting, crashed, or no longer held by this
 * consist) should the caller search afresh and re-claim.
 * @param moving the approaching consist.
 * @return the claimed contact-end vehicle, or nullptr when there is no valid claim.
 */
Train *GetClaimedCoupleTarget(const Train *moving)
{
	Train *tgt = Train::GetIfValid(moving->couple_target);
	if (tgt == nullptr) return nullptr;
	Train *carrier = tgt->Primary();
	/* The claim is only usable while the target is still a live, waiting consist
	 * and the claim is still held by this moving consist. */
	if (carrier->vehstatus.Test(VehState::Crashed)) return nullptr;
	if (!IsHoldingForCouple(carrier)) return nullptr;
	if (GetValidCoupleClaimant(carrier) != moving->Primary()) return nullptr;
	return tgt;
}

const Train *GetCoupleClaimant(const Train *carrier)
{
	return GetValidCoupleClaimant(carrier->Primary());
}

/**
 * Check whether a waiting consist is already reserved for another train.
 * @param carrier Primary of the waiting consist.
 * @param moving the approaching consist challenging the claim.
 * @param claim_cost path cost of the challenger's route to the waiting consist.
 * @return true when the claim blocks the challenger.
 */
static bool CoupleClaimBlocks(const Train *carrier, const Train *moving, [[maybe_unused]] uint32_t claim_cost)
{
	Train *claimant = GetValidCoupleClaimant(carrier);
	if (claimant == nullptr) return false;
	if (claimant == moving->Primary()) return false;
	/* A selected partner remains exclusive, including during intermediate orders. */
	return true;
}

static Train *SelectPowerCoupleClaimant(Train *moving, Train *carrier, Train *&contact, uint32_t &claim_cost);

void ClaimCoupleTarget(Train *moving, Train *carrier, uint32_t claim_cost)
{
	Train *prim = moving->Primary();
	if (GetClaimCoupleOrder(prim) == nullptr) return;

	Train *claimant = GetValidCoupleClaimant(carrier);
	if (claimant == prim) {
		carrier->couple_claim_cost = claim_cost;
		return;
	}
	if (claimant != nullptr) return;

	/* Only the first claim is arbitrated; never take a live claim from a train. */
	Train *contact = Train::GetIfValid(prim->couple_target);
	Train *selected = SelectPowerCoupleClaimant(prim, carrier, contact, claim_cost);
	if (selected != prim) {
		selected->couple_target = contact->index;
		prim = selected;
	}
	carrier->couple_claimant = prim->index;
	carrier->couple_claim_cost = claim_cost;
	prim->flags.Reset(VehicleRailFlag::CoupleDirectionChecked);
}

/**
 * Check the *volatile* conditions of a couple order against a waiting train.
 *
 * These are all cheap state comparisons (the partner can change any of them
 * while the approaching consist is still on its way: it can lose its 路签,
 * finish loading, get refitted, drive off, ...), so they are re-checked every
 * tick for an established claim. The expensive geometric check
 * (#IsCoupleArrangementValid, which re-arranges both chains and runs the NewGRF
 * start/stop callbacks) is deliberately NOT part of this: it belongs to target
 * selection and to the actual contact, never to the per-tick claim validation.
 *
 * @param moving the approaching consist.
 * @param order the GOTO_COUPLE order of \a moving.
 * @param rep physical chain head of the waiting train.
 * @param contact_tile tile where the coupling is expected to happen.
 * @return #CoupleCandidateResult::Valid, or the first condition that fails.
 */
CoupleCandidateResult GetCoupleVolatileConditionResult(const Train *moving, const Order &order,
		Train *rep, TileIndex contact_tile)
{
	Train *carrier = rep->Primary();

	if (!IsHoldingForCouple(carrier)) return CoupleCandidateResult::NotWaiting;
	if (!WaitCoupleSlotReady(carrier)) return CoupleCandidateResult::NotWaiting;
	if (carrier->vehstatus.Test(VehState::Crashed)) return CoupleCandidateResult::Crashed;
	if (carrier->vehstatus.Test(VehState::Stopped)) return CoupleCandidateResult::Stopped;
	if (!IsTrainCouplingAllowed(moving->owner, carrier->owner)) return CoupleCandidateResult::Owner;
	const uint16_t companies = carrier->current_order.GetWaitCoupleCompanyMask();
	if (companies != 0 && !HasBit(companies, moving->owner.base())) return CoupleCandidateResult::WaitingCompany;
	const uint8_t traction = carrier->current_order.GetWaitCoupleTraction();
	const Train *locomotive = moving->Primary();
	if (traction != 0 && (!locomotive->IsEngine() ||
			to_underlying(RailVehInfo(locomotive->engine_type)->engclass) + 1 != traction)) {
		return CoupleCandidateResult::Traction;
	}
	/* Selection criteria the waiting train imposes on the approaching consist. */
	if (!WaitCouplePartnerSlotOk(carrier->current_order, moving)) return CoupleCandidateResult::WaitPartnerSlot;
	if (!WaitCouplePartnerSlotGroupOk(carrier->current_order, moving)) return CoupleCandidateResult::WaitPartnerSlotGroup;
	if (!carrier->current_order.MatchesWaitCouplePower(moving->gcache.cached_power)) return CoupleCandidateResult::WaitPartnerPower;
	if (!WaitCouplePartnerGroupOk(carrier->current_order, moving)) return CoupleCandidateResult::WaitPartnerGroup;
	/* Compare the consist capability, not temporary shunting, signal or track limits.
	 * This read-only check is shared by checkpoint claims, routing and contact. */
	const uint moving_speed = moving->Primary()->GetDisplayMaxSpeed();
	const uint waiting_speed = carrier->GetDisplayMaxSpeed();
	if (order.GetCoupleRejectSlower() && moving_speed < waiting_speed) return CoupleCandidateResult::Slower;
	if (order.GetCoupleRejectFaster() && moving_speed > waiting_speed) return CoupleCandidateResult::Faster;
	for (uint row = 0; row < 2; row++) {
		const uint8_t comparator = order.GetCoupleSpeedComparator(row);
		if (comparator == 0) continue;
		if (comparator > 6 || order.GetCoupleSpeedUnit(row) >= 6 || !OrderConditionCompare(static_cast<OrderConditionComparator>(comparator - 1),
				ConvertKmhishSpeedToIntegerDisplaySpeed(waiting_speed, order.GetCoupleSpeedUnit(row)), order.GetCoupleSpeedValue(row))) return CoupleCandidateResult::SpeedRange;
	}
	if (!CoupleOrderLoadOk(order, rep)) return CoupleCandidateResult::Load;
	if (!CoupleCargoOk(order, rep)) return CoupleCandidateResult::Cargo;
	if (!CoupleNumOk(order, rep)) return CoupleCandidateResult::UnitCount;
	if (!CoupleSlotOk(order, carrier)) return CoupleCandidateResult::Slot;
	if (!CoupleSlotGroupOk(order, carrier)) return CoupleCandidateResult::SlotGroup;
	if (!CoupleGroupOk(order, carrier)) return CoupleCandidateResult::Group;
	if (!CoupleServiceNumberOk(order, carrier)) return CoupleCandidateResult::ServiceNumber;
	if (!CoupleStationOk(order, contact_tile) &&
			(!order.HasCoupleStation() || GetTrainCouplingStation(rep) != order.GetCoupleStation())) return CoupleCandidateResult::Station;
	if (GetTrainCouplingStation(rep) == StationID::Invalid()) return CoupleCandidateResult::Platform;
	return CoupleCandidateResult::Valid;
}

/** Necessary conditions only; a negative result proves that no route can couple. */
bool HasPotentialCoupleTarget(const Train *moving)
{
	moving = moving->Primary();
	if (!moving->current_order.IsType(OT_GOTO_COUPLE)) return false;
	/* Do not cache a negative result: a target can start waiting, change cargo,
	 * regain a slot, or lose another claimant during this same game tick. This
	 * only rejects impossible destinations; YAPF still verifies reachability,
	 * arrangement and the best direction for every surviving candidate. */
	const Train *claimed = GetClaimedCoupleTarget(moving);
	if (claimed != nullptr) return true;
	for (const Train *candidate : Train::Iterate()) {
		if (!candidate->IsPrimaryVehicle() || candidate == moving || !IsHoldingForCouple(candidate)) continue;
		if (CoupleClaimBlocks(candidate, moving, UINT32_MAX)) continue;
		if (GetCoupleVolatileConditionResult(moving, moving->current_order, candidate->First(), candidate->First()->tile) == CoupleCandidateResult::Valid) return true;
	}
	return false;
}

/**
 * Validate a waiting train as a coupling partner for the moving consist and
 * return the end of its chain expected to make contact first.
 * @param moving the approaching consist.
 * @param rep physical chain head of the waiting train.
 * @param contact_tile tile where the coupling is expected to happen.
 * @param respect_claim when true, a waiting train already claimed by another
 *   approaching consist is rejected (target selection); the claim is ignored
 *   on the physical contact paths so a reached partner always couples.
 * @param claim_cost path cost of the moving consist's route when selecting.
 * @return the contact-end vehicle of the waiting train, or nullptr if invalid.
 */
CoupleCandidateResult GetCoupleCandidateResult(const Train *moving, const Order &order,
		Train *rep, TileIndex contact_tile, bool respect_claim, uint32_t claim_cost)
{
	Train *carrier = rep->Primary();

	if (!order.IsType(OT_GOTO_COUPLE)) return CoupleCandidateResult::NotCoupleOrder;
	if (respect_claim && CoupleClaimBlocks(carrier, moving, claim_cost)) return CoupleCandidateResult::Claimed;
	CoupleCandidateResult volatile_result = GetCoupleVolatileConditionResult(moving, order, rep, contact_tile);
	if (volatile_result != CoupleCandidateResult::Valid) return volatile_result;
	if (!IsCoupleArrangementValid(const_cast<Train *>(moving), rep)) return CoupleCandidateResult::Arrangement;
	return CoupleCandidateResult::Valid;
}

/** Only compete before departure, after loading/reversal is complete. */
static const Order *GetReadyPowerCoupleOrder(const Train *train, bool arriving_at_checkpoint = false)
{
	if (!train->IsPrimaryVehicle() || train->IsVirtualOrCarried() || !IsValidTile(train->tile) ||
			!IsValidTile(train->GetMovingFront()->tile) || !train->railtypes.Any() ||
			train->IsInDepot() || train->GetMovingFront()->IsInDepot() || train->reverse_distance != 0 ||
			train->vehstatus.Any({VehState::Stopped, VehState::Crashed}) ||
			train->flags.Any({VehicleRailFlag::ConsistBreakdown, VehicleRailFlag::Reversing}) || train->gcache.cached_power == 0) return nullptr;
	/* The checkpoint flag is set only once its arrival and reversal work has
	 * completed and it is actually waiting for a partner, not while loading. */
	if (train->current_order.IsType(OT_GOTO_COUPLE)) return train->cur_speed == 0 ? &train->current_order : nullptr;
	if (!arriving_at_checkpoint && (train->cur_speed != 0 || !train->flags.Test(VehicleRailFlag::CoupleCheckReservationReleased))) return nullptr;
	const Order *couple = GetClaimCoupleOrder(train);
	if (couple == nullptr || !couple->HasCoupleCheckPoint()) return nullptr;
	/* Passing the entire reverse waypoint can advance the order index. The
	 * pending flag still identifies the completed checkpoint until departure. */
	if (train->flags.Test(VehicleRailFlag::CoupleCheckReversePending)) return couple;
	const Order *current = train->orders != nullptr ? train->GetOrder(train->cur_real_order_index) : nullptr;
	if (couple == nullptr || current == nullptr || !couple->HasCoupleCheckPoint() ||
			(!current->IsType(OT_GOTO_STATION) && !current->IsType(OT_GOTO_WAYPOINT)) ||
			current->GetDestination() != couple->GetCoupleCheckPoint()) return nullptr;
	const Train *front = train->GetMovingFront();
	if ((IsRailStationTile(front->tile) || IsRailWaypointTile(front->tile)) && GetStationIndex(front->tile) == couple->GetCoupleCheckPoint()) return couple;
	if (train->current_order.IsAnyLoadingType() && train->last_station_visited == couple->GetCoupleCheckPoint()) return couple;
	return nullptr;
}

/** A checkpoint claim can resume through ordinary intermediate waypoints.
 * ProcessOrders does not permit automatic reversing for such an order change,
 * so explicitly resolve the first post-checkpoint leg once. Do not consume the
 * check while a saved pending reversal still has its completed checkpoint as
 * the real order: ProcessOrders can restore it before advancing next tick. */
bool ShouldCheckCoupleCheckpointDirection(const Train *train)
{
	if (train->couple_target == VehicleID::Invalid() || train->reverse_distance != 0 ||
			train->flags.Any({VehicleRailFlag::Reversing, VehicleRailFlag::CoupleDirectionChecked, VehicleRailFlag::JustDecoupled}) ||
			(!train->current_order.IsType(OT_GOTO_WAYPOINT) && !train->current_order.IsType(OT_GOTO_STATION))) return false;
	/* A blocked direction check must not consume this claim's one-shot retry. */
	if (_settings_game.difficulty.train_flip_reverse_allowed == TrainFlipReversingAllowed::EndOfLineOnly ||
			train->GetMovingFront()->track == TRACK_BIT_DEPOT) return false;
	const Order *couple = GetClaimCoupleOrder(train);
	if (couple == nullptr || !couple->HasCoupleCheckPoint() ||
			train->current_order.GetDestination().ToStationID() == couple->GetCoupleCheckPoint()) return false;
	const uint interval = std::max<uint>(1, _settings_game.pf.path_backoff_interval);
	if (train->cur_speed != 0 && _scaled_tick_counter % interval != train->index.base() % interval) return false;
	return GetClaimedCoupleTarget(train) != nullptr;
}

/** Power arbitration is opt-in through either speed bound of this coupling order. */
static bool HasCoupleSpeedCheck(const Order &order)
{
	return order.GetCoupleRejectSlower() || order.GetCoupleRejectFaster() ||
			order.GetCoupleSpeedComparator(0) != 0 || order.GetCoupleSpeedComparator(1) != 0;
}

/** Speed fit precedes power; full power priority only applies to an exact speed match. */
static auto GetCoupleClaimPriority(const Train *train, uint waiting_speed)
{
	const uint speed = train->GetDisplayMaxSpeed();
	const uint gap = speed > waiting_speed ? speed - waiting_speed : waiting_speed - speed;
	return std::make_tuple(gap, speed > waiting_speed, gap == 0 ? UINT32_MAX - train->gcache.cached_power : 0u);
}

/** For slower wagons, slightly favour a lower-powered overspeed locomotive;
 * underspeed locomotives retain their small high-power preference. Both are
 * bounded below one tile, never an unconditional power rank. Integer arithmetic
 * is deterministic; native path costs and reservations are unchanged. */
static int64_t GetCoupleClaimRouteScore(const Train *train, uint32_t cost, uint waiting_speed)
{
	const uint speed = train->GetDisplayMaxSpeed();
	if (speed == waiting_speed) return cost;
	const uint64_t power = train->gcache.cached_power;
	const uint power_weight = power * YAPF_TILE_LENGTH / (power + 5000);
	return speed > waiting_speed ? int64_t(cost) + power_weight : int64_t(cost) - power_weight;
}

/** Resolve ready competitors without changing their orders, claims or reservations. */
static Train *SelectPowerCoupleClaimant(Train *moving, Train *carrier, Train *&contact, uint32_t &claim_cost)
{
	const Order *order = GetReadyPowerCoupleOrder(moving, true);
	/* Disabled orders keep ordinary coupling and avoid a fleet scan entirely. */
	if (order == nullptr || !HasCoupleSpeedCheck(*order) || contact == nullptr || contact->Primary() != carrier) return moving;
	const uint waiting_speed = carrier->GetDisplayMaxSpeed();
	const auto original_priority = GetCoupleClaimPriority(moving, waiting_speed);
	struct Candidate {
		Train *train;
		const Order *criteria;
		std::tuple<uint, bool, uint32_t> priority;
		uint distance;
	};
	std::vector<Candidate> candidates;
	for (Train *other : Train::Iterate()) {
		if (other == moving || !other->IsPrimaryVehicle()) continue;
		const auto priority = GetCoupleClaimPriority(other, waiting_speed);
		if (priority > original_priority) continue;
		const Order *criteria = GetReadyPowerCoupleOrder(other);
		if (criteria == nullptr || !HasCoupleSpeedCheck(*criteria) || GetClaimedCoupleTarget(other) != nullptr) continue;
		if (GetCoupleVolatileConditionResult(other, *criteria, carrier->First(), contact->tile) != CoupleCandidateResult::Valid) continue;
		candidates.push_back({other, criteria, priority, DistanceManhattan(other->tile, carrier->tile)});
	}
	if (candidates.empty()) return moving;
	/* Only the first bounded set will be searched. Partial sorting preserves the
	 * same deterministic winners without sorting the unused fleet tail. */
	static constexpr uint MAX_PROBES = 16;
	static constexpr uint MAX_QUERY_NODES = 1024;
	const size_t count = std::min<size_t>(candidates.size(), MAX_PROBES);
	std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end(), [](const Candidate &a, const Candidate &b) {
		return std::tie(a.priority, a.distance, a.train->index) < std::tie(b.priority, b.distance, b.train->index);
	});

	Train *best = moving;
	auto best_priority = original_priority;
	bool best_route_known = claim_cost != 0;
	for (size_t i = 0; i < count; ++i) {
		const Candidate &candidate = candidates[i];
		Train *other = candidate.train;
		if (candidate.priority > best_priority) break;
		if (candidate.priority == best_priority && !best_route_known) {
			Train *own_end = nullptr;
			uint32_t own_cost = 0;
			if (YapfTrainCoupleTrack(best, true, &own_end, &own_cost, order, carrier, MAX_QUERY_NODES) == INVALID_TRACK || own_end == nullptr) break;
			claim_cost = own_cost;
			best_route_known = true;
		}
		Train *end = nullptr;
		uint32_t cost = 0;
		/* Explicit criteria avoid temporarily changing a checkpoint's active order. */
		if (YapfTrainCoupleTrack(other, true, &end, &cost, candidate.criteria, carrier, MAX_QUERY_NODES) == INVALID_TRACK || end == nullptr) continue;
		if (candidate.priority == best_priority &&
				std::make_tuple(GetCoupleClaimRouteScore(other, cost, waiting_speed), cost, other->index) >=
				std::make_tuple(GetCoupleClaimRouteScore(best, claim_cost, waiting_speed), claim_cost, best->index)) continue;
		best = other;
		best_priority = candidate.priority;
		contact = end;
		claim_cost = cost;
		best_route_known = true;
	}

	return best;
}

/** Bounded soft preference, in tiles: matching speed never makes another valid partner ineligible. */
uint GetCoupleSpeedPreferencePenalty(const Train *moving, const Train *waiting, const Order *criteria)
{
	const uint a = moving->Primary()->GetDisplayMaxSpeed();
	const uint b = waiting->Primary()->GetDisplayMaxSpeed();
	const uint gap = a > b ? a - b : b - a;
	const uint penalty = std::min<uint>((gap + 3) / 4, 24);
	/* Only enabled speed checks reduce distance's relative influence (4/5).
	 * Integer scoring stays deterministic and bounded at 30 tiles. Preserve
	 * the previous disabled-order score and the ordinary route-cost metric. */
	return HasCoupleSpeedCheck(criteria != nullptr ? *criteria : moving->current_order) ? (penalty * 5 + 3) / 4 : penalty;
}

/** Select a checkpoint partner deterministically, avoiding expensive checks for worse candidates. */
Train *FindBestCoupleCheckpointTarget(Train *moving, const Order &couple)
{
	Train *best = nullptr;
	std::tuple<uint64_t, uint, uint32_t, uint32_t> best_key{};
	for (Train *candidate : Train::Iterate()) {
		if (!candidate->IsPrimaryVehicle() || candidate->Primary() == moving->Primary() || candidate->vehstatus.Test(VehState::Crashed)) continue;
		const uint penalty = GetCoupleSpeedPreferencePenalty(moving, candidate, &couple);
		for (Train *end : {candidate->First(), candidate->First()->Last()}) {
			const uint distance = DistanceManhattan(end->tile, moving->tile);
			const auto key = std::make_tuple(uint64_t(distance) + penalty, penalty, candidate->index.base(), end->index.base());
			if (best != nullptr && key >= best_key) continue;
			if (GetCoupleCandidateResult(moving, couple, candidate->First(), end->tile, true, 0) != CoupleCandidateResult::Valid) continue;
			best = end;
			best_key = key;
		}
	}
	return best;
}

/** Select and exclusively claim a matching waiting consist, or report that no partner is ready. */
bool ClaimOrWaitForCoupleOrder(Train *moving, const Order &couple)
{
	if (GetClaimedCoupleTarget(moving) != nullptr) {
		moving->flags.Reset(VehicleRailFlag::CoupleCheckReservationReleased);
		return false;
	}
	if (Train *target = FindBestCoupleCheckpointTarget(moving, couple); target != nullptr) {
		moving->couple_target = target->index;
		ClaimCoupleTarget(moving, target->Primary(), 0);
		if (GetClaimedCoupleTarget(moving) != nullptr) {
			moving->flags.Reset(VehicleRailFlag::CoupleCheckReservationReleased);
			return false;
		}
		moving->couple_target = VehicleID::Invalid();
	}
	/* The checkpoint can be several orders before the actual couple order.
	 * Stop and release its existing forward reservation just as an unmatched
	 * OT_GOTO_COUPLE does, so other trains may use the route while we wait. */
	moving->cur_speed = 0;
	moving->subspeed = 0;
	/* Only release once. Repeating this walk after another train reserves the
	 * newly freed route could otherwise clear that train's reservation. */
	if (!moving->flags.Test(VehicleRailFlag::CoupleCheckReservationReleased)) {
		FreeTrainTrackReservation(moving);
		moving->flags.Set(VehicleRailFlag::CoupleCheckReservationReleased);
	}
	return true;
}

bool TrainShouldWaitForCoupleAtCheckpoint(Train *moving, bool *at_checkpoint, StationID arrived)
{
	if (at_checkpoint != nullptr) *at_checkpoint = false;
	if (moving->current_order.IsType(OT_GOTO_DEPOT)) return false;
	if (moving->orders == nullptr || moving->vehstatus.Test(VehState::Crashed)) return false;
	const Order *current = moving->GetOrder(moving->cur_real_order_index);
	if (current == nullptr || (!current->IsType(OT_GOTO_STATION) && !current->IsType(OT_GOTO_WAYPOINT))) return false;
	const Train *front = moving->GetMovingFront();
	if (current->IsType(OT_GOTO_WAYPOINT) && current->GetWaypointFlags().Test(OrderWaypointFlag::Stop) && moving->cur_speed != 0) return false;
	const StationID point = current->GetDestination().ToStationID();
	if (arrived != StationID::Invalid()) {
		if (arrived != point) return false;
	} else if (moving->current_order.IsType(OT_LOADING) && current->IsType(OT_GOTO_STATION) && moving->last_station_visited == point && moving->cur_speed == 0) {
		/* A long train may have its moving front beyond the platform tile while
		 * loading. The active station order still identifies the checkpoint. */
	} else if ((!IsRailStationTile(front->tile) && !IsRailWaypointTile(front->tile)) || GetStationIndex(front->tile) != point) {
		return false;
	}
	const Order *couple = nullptr;
	for (VehicleOrderID i = moving->cur_real_order_index + 1; i < moving->GetNumOrders(); ++i) {
		const Order *next = moving->GetOrder(i);
		if (next != nullptr && next->IsType(OT_GOTO_COUPLE) && next->HasCoupleCheckPoint() && next->GetCoupleCheckPoint() == current->GetDestination().ToStationID()) { couple = next; break; }
	}
	if (couple == nullptr) return false;
	if (at_checkpoint != nullptr) *at_checkpoint = true;
	if (current->IsType(OT_GOTO_WAYPOINT) && current->GetWaypointFlags().Test(OrderWaypointFlag::Reverse) && moving->reverse_distance > 0) {
		moving->flags.Set(VehicleRailFlag::CoupleCheckReversePending);
		return false;
	}
	return ClaimOrWaitForCoupleOrder(moving, *couple);
}

Train *ValidateCoupleCandidate(const Train *moving, Train *rep, TileIndex contact_tile,
		bool respect_claim, uint32_t claim_cost, const Order *criteria)
{
	const Order &order = criteria != nullptr ? *criteria : moving->Primary()->current_order;
	if (GetCoupleCandidateResult(moving, order, rep, contact_tile, respect_claim, claim_cost) != CoupleCandidateResult::Valid) return nullptr;

	/* The waiting train's closest end to the contact point is met first. */
	Train *first = rep;
	Train *last = rep->Last();
	return DistanceManhattan(first->tile, contact_tile) <= DistanceManhattan(last->tile, contact_tile) ? last : first;
}

/**
 * Scan a station platform for a valid coupling partner.
 * @param moving the approaching consist.
 * @param tile tile the couple pathfinder wants to reach.
 * @param td trackdir of arrival.
 * @param respect_claim whether an existing couple claim on the waiting train
 *   blocks this consist (see #ValidateCoupleCandidate).
 * @param claim_cost path cost of the moving consist's route when selecting.
 * @return contact-end vehicle of a waiting train, or nullptr.
 */
Train *ResolveCoupleTargetStation(const Train *moving, TileIndex tile, Trackdir td,
		bool respect_claim, uint32_t claim_cost, const Order *criteria, const Train *only_target)
{
	if (!IsRailStationTile(tile)) return nullptr;

	Train *best = nullptr;
	std::vector<VehicleID> seen;
	std::tuple<uint64_t, uint, uint32_t> best_key{};
	bool best_clear = false;
	TileIndexDiff diff = TileOffsByDiagDir(TrackdirToExitdir(ReverseTrackdir(td)));
	for (TileIndex st_tile = tile; IsCompatibleTrainStationTile(st_tile, tile); st_tile += diff) {
		for (Train *t : VehiclesOnTile<VehicleType::Train>(st_tile)) {
			if (t->vehstatus.Test(VehState::Crashed)) continue;
			if (only_target != nullptr && t->Primary() != only_target->Primary()) continue;
			/* A long waiting consist can occupy many platform tiles. Trial surgery
			 * and approach probing must run once per consist, not once per wagon. */
			const VehicleID id = t->Primary()->index;
			if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
			seen.push_back(id);
			Train *target = ValidateCoupleCandidate(moving, t->First(), st_tile, respect_claim, claim_cost, criteria);
			if (target != nullptr) {
				const uint penalty = GetCoupleSpeedPreferencePenalty(moving, target);
				const auto key = std::make_tuple(uint64_t(DistanceManhattan(tile, target->tile)) + penalty, penalty, target->index.base());
				/* The approach walk only ranks candidates, it must never veto them.
				 * It follows the cheapest tile path, which is not necessarily the
				 * route the train will take: the pathfinder applies turn, signal
				 * and reservation costs the walk does not model. A train queued on
				 * some platform track the walk happens to cross therefore used to
				 * reject every candidate, leaving the consist without a target and
				 * permanently stuck even though a clear route existed. A clear
				 * approach is preferred, but a reported obstruction is only a
				 * fallback: the regular PBS reservation makes the train wait
				 * instead of driving through another consist. */
				const bool path_clear = IsCoupleApproachPathClearTo(moving, target);
				if (best == nullptr || (path_clear && !best_clear) || (path_clear == best_clear && key < best_key)) {
					best = target;
					best_key = key;
					best_clear = path_clear;
				}
			}
		}
	}
	return best;
}

/**
 * Reverse the trial chain order and orientation the way ReverseTrainForCouple
 * would. Positions are untouched; the guard restores directions, driving state
 * and Flipped after the NewGRF check, and RestoreTrainBackup undoes the relink.
 */
static void TrialReverseChainOrder(Train *head)
{
	Train *primary = head->Primary();
	std::vector<Train *> order;
	for (Train *t = head; t != nullptr; t = t->Next()) {
		order.push_back(t);
		t->direction = ReverseDir(t->direction);
		t->vehicle_flags.Reset(VehicleFlag::DrivingBackwards);
		/* The guard restores the same Flipped changes as the real reversal. */
		if (t->PreserveFacingOnCoupleReverse()) t->flags.Flip(VehicleRailFlag::Flipped);
	}
	std::reverse(order.begin(), order.end());
	for (size_t i = 0; i < order.size(); i++) {
		order[i]->SetNextRaw(i + 1 < order.size() ? order[i + 1] : nullptr);
		order[i]->SetPreviousRaw(i > 0 ? order[i - 1] : nullptr);
	}
	Train *new_first = order.front();
	Train *new_last = order.back();
	for (Train *t = new_first; t != nullptr; t = t->Next()) {
		t->SetFirst(new_first);
		t->SetLast(new_last);
	}
	BindTrainPrimary(new_first, primary);
	ResetChainNonFrontMarkers(new_first);
}

/**
 * Mirror Couple()'s orientation phase on the trial chains: decide from the
 * same inputs (the moving primary's driving direction and the primaries'
 * directions) which sides a real couple would flip, and flip the trial chain
 * order accordingly, so the arrangement the NewGRF start/stop check evaluates
 * here is the one the actual couple produces.
 */
static void TrialApplyCoupleOrientation(Train **v_phys, Train **u_phys)
{
	Train *v_prim = (*v_phys)->Primary();
	Train *u_prim = (*u_phys)->Primary();

	/* v's own driving state decides whether it must be turned around. */
	Direction v_dir = v_prim->direction;
	if (!v_prim->IsDrivingBackwards()) {
		TrialReverseChainOrder(*v_phys);
		*v_phys = (*v_phys)->First();
		v_dir = v_prim->direction;
	}

	/* u couples as-is when it faces the (possibly flipped) v within 45 degrees. */
	DirDiff dir_diff = DirDifference(v_dir, u_prim->direction);
	if (dir_diff != DirDiff::Same && dir_diff != DirDiff::Right45 && dir_diff != DirDiff::Left45) {
		TrialReverseChainOrder(*u_phys);
		*u_phys = (*u_phys)->First();
	}
}

bool IsCoupleArrangementValid(Train *v_phys, Train *u_phys)
{
	/* The pathfinder operates on the Primary, which may be in the middle of
	 * the physical chain. Back up and arrange complete consists only; otherwise
	 * RestoreTrainBackup would detach every vehicle preceding the Primary. */
	v_phys = v_phys->First();
	u_phys = u_phys->First();

	TrainList original_src;
	TrainList original_dst;

	TrialConsistGuard trial_guard;

	MakeTrainBackup(original_src, v_phys);
	MakeTrainBackup(original_dst, u_phys);
	trial_guard.SnapshotChain(original_src);
	trial_guard.SnapshotChain(original_dst);

	/* A real couple flips one or both consists before merging (see Couple())
	 * so the trial must evaluate the flipped arrangement, not the current one. */
	TrialApplyCoupleOrientation(&v_phys, &u_phys);

	Train *u_head = u_phys;
	Train *v = v_phys;
	Train *v_last = v_phys->Last();

	ArrangeTrains(&v, v_last, &u_head, u_phys, true, false);

	/* The NewGRF caches still hold values of the two pre-couple consists;
	 * invalidate them so the start/stop check sees the merged consist. */
	v->InvalidateNewGRFCacheOfChain();
	RefreshTrainUserDefData(v);

	bool ok = !CheckTrainAttachment(v).Failed();
	ok &= v->CanConsistChange(CCF_ARRANGE_CHECK);

	/* Target selection gate: the merged consist's primary must satisfy the
	 * NewGRF start/stop check, otherwise this couple target is rejected. */
	Train *merged_prim = GetStartStopCheckVehicle(v);
	CommandCost cb = CheckVehicleStartStopCallback(merged_prim);
	ok &= !cb.Failed();

	/* Restore both physical chains and their tail caches after the trial. */
	RestoreChainFromList(original_src);
	RestoreChainFromList(original_dst);

	return ok;
}

/**
 * Repair a stale track bit of one vehicle: the bit must exist on the vehicle's
 * tile. Pick the bit of that tile which connects towards the vehicle's chain
 * neighbour's tile -- on curve tiles the two candidate tracks connect
 * different corner pairs, so the chain geometry identifies the right one
 * without trusting the (possibly swap-inherited) direction. Falls back to the
 * tile's only bit. Returns false only when the tile has no rail at all or no
 * bit can be adopted.
 */
static bool RepairVehicleTrackBit(Train *w, const Train *hint)
{
	if (w->track & TRACK_BIT_WORMHOLE) return true;
	if (w->track == TRACK_BIT_DEPOT) return true;
	TrackBits tile_bits = TrackdirBitsToTrackBits(GetTileTrackdirBits(w->tile, TransportType::Rail, 0));
	if (tile_bits == TRACK_BIT_NONE) return false;
	if ((tile_bits & w->track) != TRACK_BIT_NONE) return true;

	const Train *n = (hint != nullptr && hint != w) ? hint : ((w->Next() != nullptr) ? w->Next() : w->Previous());
	TrackBits cand = TRACK_BIT_NONE;
	if (n != nullptr && n->tile != w->tile) {
		DiagDirection want = DiagdirBetweenTiles(w->tile, n->tile);
		if (IsValidDiagDirection(want)) {
			for (Track t : SetTrackBitIterator(tile_bits)) {
				Trackdir td = TrackToTrackdir(t);
				if (TrackdirToExitdir(td) == want || TrackdirToExitdir(ReverseTrackdir(td)) == want) {
					cand = TrackToTrackBits(t);
					break;
				}
			}
		}
	}
	if (cand == TRACK_BIT_NONE && HasExactlyOneBit(tile_bits)) cand = tile_bits;
	if (cand == TRACK_BIT_NONE) return false;
	Debug(desync, 0, "ChainOnRail: veh={} repairs track {:X} -> {:X} on tile=({},{})",
			w->index, to_underlying(w->track), to_underlying(cand), TileX(w->tile), TileY(w->tile));
	w->track = cand;
	return true;
}

/**
 * Verify that every vehicle of a freshly rearranged chain stands on a tile
 * that actually carries the vehicle's track; repair stale track bits where
 * possible. Only a genuinely rail-less tile (e.g. a wagon pushed past the end
 * of the track) rejects the couple.
 */
static bool ChainOnRail(Train *head)
{
	for (Train *w = head; w != nullptr; w = w->Next()) {
		if (!RepairVehicleTrackBit(w, nullptr)) {
			Debug(desync, 0, "ChainOnRail: veh={} stands on unusable tile=({},{}) track={:X} xy=({},{})",
					w->index, TileX(w->tile), TileY(w->tile), to_underlying(w->track), w->x_pos, w->y_pos);
			return false;
		}
	}
	return true;
}

/**
 * Try to couple the two consists together.
 */
static bool TryTrainCouple(Train *v, Train *u)
{
	TrainList original_src;
	TrainList original_dst;

	TrialConsistGuard trial_guard;

	MakeTrainBackup(original_src, v);
	MakeTrainBackup(original_dst, u);
	trial_guard.SnapshotChain(original_src);
	trial_guard.SnapshotChain(original_dst);

	Train *u_head = u;
	Train *v_last = v->Last();

	ArrangeTrains(&v, v_last, &u_head, u, true, false);

	/* The NewGRF caches still hold values of the two pre-couple consists;
	 * invalidate them so the start/stop check sees the merged consist. */
	v->InvalidateNewGRFCacheOfChain();
	RefreshTrainUserDefData(v);

	CommandCost ret = CheckTrainAttachment(v);
	bool ok = v->CanConsistChange(CCF_ARRANGE_CHECK);
	/* Geometric safety net: the arranged chain must stand on actual track. */
	ok &= ChainOnRail(v);

	/* The merged consist's primary must satisfy the NewGRF start/stop check. */
	Train *merged_prim = GetStartStopCheckVehicle(v);
	CommandCost cb_merged = CheckVehicleStartStopCallback(merged_prim);
	ok &= !cb_merged.Failed();

	if (ret.Failed() || !ok) {
		/* Restore the train we had. */
		RestoreTrainBackup(original_src);
		RestoreTrainBackup(original_dst);

		v->ConsistChanged(CCF_ARRANGE_STATION);
		u->ConsistChanged(CCF_ARRANGE_STATION);
		return false;
	}

	/* This caller intentionally keeps the successful merge. */
	trial_guard.restore_structure = false;
	/* Coupling removes a train front; invalidate the tick caches or the merged chain will be ticked twice. */
	InvalidateVehicleTickCaches();
	return true;
}

/**
 * Move a train vehicle to the given pixel position, keeping its tile, height and
 * vehicle hash consistent with the new position.
 * @param v vehicle to move.
 * @param x new x position.
 * @param y new y position.
 */
/** Direction of travel (facing of a forward-driving vehicle) from a pixel delta. */
static Direction DirectionFromDelta(int dx, int dy)
{
	/* World x/y are rotated relative to the viewport direction names; match
	 * TileIndexDiffCByDir rather than treating them as screen coordinates. */
	if (dx > 0) return dy > 0 ? Direction::S : (dy < 0 ? Direction::W : Direction::SW);
	if (dx < 0) return dy > 0 ? Direction::E : (dy < 0 ? Direction::N : Direction::NE);
	return dy > 0 ? Direction::SE : Direction::NW;
}

static void MoveTrainToPosition(Train *v, int x, int y)
{
	if (v->x_pos == x && v->y_pos == y) return;

	TileIndex tile = TileVirtXY(x, y);
	bool changed_tile = (tile != v->tile);

	if (changed_tile && !(v->track & TRACK_BIT_WORMHOLE) && v->track != TRACK_BIT_DEPOT
			&& !IsRailDepotTile(tile) && !IsTileType(tile, TileType::TunnelBridge)) {
		/* The couple/flip re-spacing moves vehicles along their own track
		 * axis, which can cross a tile boundary on a curve. VehicleEnterTile
		 * does not update a train's track/direction on plain rail tiles, so
		 * without this fix the vehicle would keep the old tile's track bit
		 * and direction while standing on the new tile -- a state the next
		 * movement tick resolves into garbage (e.g. an invalid direction).
		 * Adopt the track bit and direction that match how we just moved. */
		if (abs(x - v->x_pos) <= (int)TILE_SIZE && abs(y - v->y_pos) <= (int)TILE_SIZE) {
			Direction motion_dir = DirectionFromDelta(x - v->x_pos, y - v->y_pos);
			/* Spacing can move a vehicle back along its track. That correction
			 * is not a reversal: keep the consist's original running orientation
			 * while adapting its track axis to the destination tile. */
			const TileIndexDiffC forward = TileIndexDiffCByDir(v->GetMovingDirection());
			if ((x - v->x_pos) * forward.x + (y - v->y_pos) * forward.y < 0) motion_dir = ReverseDir(motion_dir);
			TrackBits desired = TrackToTrackBits(TrackdirToTrack(DiagDirToDiagTrackdir(DirToDiagDir(motion_dir))));
			TrackBits available = TrackdirBitsToTrackBits(GetTileTrackdirBits(tile, TransportType::Rail, 0));
			TrackBits match = available & desired;
			if (match != TRACK_BIT_NONE) {
				v->track = match;
				v->direction = v->IsDrivingBackwards() ? ReverseDir(motion_dir) : motion_dir;
			}
		} else {
			/* A teleport across the map (e.g. the ferry unload placement puts a
			 * consist straight from the carrier onto a platform) is not an
			 * along-track move: deriving track/direction from its delta would
			 * turn every vehicle against the facing its caller just set. The
			 * caller has laid out track bit and facing already; only repair a
			 * track bit that does not exist on the new tile. */
			TrackBits available = TrackdirBitsToTrackBits(GetTileTrackdirBits(tile, TransportType::Rail, 0));
			if (available != TRACK_BIT_NONE && !(available & v->track)) v->track = TrackToTrackBits(FindFirstTrack(available));
		}
	}

	v->x_pos = x;
	v->y_pos = y;
	v->z_pos = GetSlopePixelZ(x, y, true);
	v->tile = tile;

	if (changed_tile) {
		/* The vehicle changed tile: run the usual enter tile update. */
		UpdateStatusAfterSwap(v, false);
	} else {
		/* Moved inside its tile: update the sprite and the vehicle hash without
		 * re-entering the (unchanged) tile, like the single vehicle case of
		 * ReverseTrainNoSwapVehicles(). */
		v->InvalidateImageCache();
		v->UpdateIsDrawn();
		v->UpdatePosition();
		v->UpdateViewport(true, true);
	}
}

void TrainMoveToPosition(Train *v, int x, int y)
{
	MoveTrainToPosition(v, x, y);
}

/**
 * Re-space a consist half that was just mirrored in place by
 * ReverseTrainNoSwapVehicles().
 *
 * Mirroring re-pairs the adjacent vehicles, and with variable length consists
 * the rounding that governs each pair's spacing swaps with the pairing, so
 * consecutive vehicles can end up a pixel too tight or too wide. Correcting
 * that by pushing chunks around does not converge: a push moves every vehicle
 * of the chunk, which disturbs pairs that were already correct, and for the half
 * that trails at the joint every push is directed towards the partner consist,
 * so the half creeps into it.
 *
 * Instead lay the half out in a single pass from the end that must not move (the
 * joint end, where the partner consist is standing) towards the other end: every
 * vehicle is placed at exactly the joint offset from its chain neighbour.
 *
 * @param half_first chain head of the flipped half.
 * @param joint_at_tail whether the end that must keep its position is the chain
 *                      tail (\c true, the moving consist: its tail meets the
 *                      partner) or the chain head (\c false, the waiting consist:
 *                      its head meets the partner).
 */
static void RespaceFlippedHalf(Train *half_first, bool joint_at_tail)
{
	if (joint_at_tail) {
		/* Lay out towards the chain head: every predecessor takes its position
		 * from its already placed successor. RespaceVehicleTo steps pixel by
		 * pixel along the vehicle's own track, which follows curve tiles. */
		for (Train *b = half_first->Last(); b->Previous() != nullptr; b = b->Previous()) {
			Train *a = b->Previous();
			RespaceVehicleTo(a, b, CouplePairOffset(a));
		}
	} else {
		/* Lay out towards the chain tail: every successor takes its position from
		 * its already placed predecessor. */
		for (Train *a = half_first; a->Next() != nullptr; a = a->Next()) {
			Train *b = a->Next();
			RespaceVehicleTo(b, a, CouplePairOffset(a));
		}
	}
}

/** Correct small spacing errors on a stopped, straight consist after a length-changing merge. */
bool RepairStoppedTrainSpacing(Train *consist)
{
	consist = consist->Primary();
	if (consist->cur_speed != 0) return false;
	Train *front = consist->GetMovingFront();
	const TileIndexDiffC delta = CoupleMotionDelta(front);
	if (delta.x == 0 && delta.y == 0) return false;
	const Train *back = consist->GetMovingBack();
	const int min_x = std::min(front->x_pos, back->x_pos);
	const int max_x = std::max(front->x_pos, back->x_pos);
	const int min_y = std::min(front->y_pos, back->y_pos);
	const int max_y = std::max(front->y_pos, back->y_pos);
	std::vector<std::pair<Train *, Point>> positions;
	Point position{front->x_pos, front->y_pos};
	bool changed = false;
	for (Train *v = front; v != nullptr; v = v->GetMovingNext()) {
		/* Do not straighten curves, slopes, depots, or tunnel/bridge spans. */
		if (v->direction != front->direction || v->IsDrivingBackwards() != front->IsDrivingBackwards() ||
				v->z_pos != front->z_pos || (v->track & (TRACK_BIT_WORMHOLE | TRACK_BIT_DEPOT)) ||
				(v->x_pos - front->x_pos) * delta.y != (v->y_pos - front->y_pos) * delta.x) return false;
		/* Stay inside the occupied footprint, and reject large/disconnected gaps. */
		if (position.x < min_x || position.x > max_x || position.y < min_y || position.y > max_y ||
				abs(position.x - v->x_pos) > VEHICLE_LENGTH || abs(position.y - v->y_pos) > VEHICLE_LENGTH) return false;
		const TileIndex target_tile = TileVirtXY(position.x, position.y);
		if (target_tile != v->tile) {
			const bool same_platform = IsRailStationTile(v->tile) && IsRailStationTile(target_tile) && IsCompatibleTrainStationTile(target_tile, v->tile);
			const bool same_rail = IsPlainRailTile(v->tile) && IsPlainRailTile(target_tile) &&
					GetTileOwner(target_tile) == v->owner && (GetTrackBits(target_tile) & v->track) == v->track;
			if ((!same_platform && !same_rail) || GetSlopePixelZ(position.x, position.y, true) != v->z_pos) return false;
		}
		changed |= position.x != v->x_pos || position.y != v->y_pos;
		positions.emplace_back(v, position);
		if (v->GetMovingNext() != nullptr) {
			const int offset = v->CalcNextVehicleOffset();
			position.x -= delta.x * offset;
			position.y -= delta.y * offset;
		}
	}
	if (!changed) return false;
	FreeTrainTrackReservation(consist);
	for (const auto &[v, target] : positions) MoveTrainToPosition(v, target.x, target.y);
	consist->lookahead.reset();
	if (IsHoldingForCouple(consist)) HoldWaitingTrainBody(consist);
	else TryPathReserve(consist, true, true);
	return true;
}

/**
 * Reverse a train for coupling by flipping the consist in place.
 *
 * The chain order is reversed without exchanging multi-part members' positions.
 * Unit-main pointers keep their identity, and the moving tail or waiting head
 * anchors the subsequent spacing correction.
 */
void ReverseTrainForCouple(Train *v, bool joint_at_tail)
{
	/* Physical reversal always operates on the whole chain; callers may pass
	 * the primary vehicle which can sit mid-chain. */
	v = v->First();
	ReverseTrainNoSwapVehicles(v);
	/* The flip reverses the chain order, so the local pointer now sits
	 * mid-chain; re-anchor at the new chain head before re-spacing. */
	v = v->First();
	/* The pairwise swap can leave a vehicle with its swap partner's track bit,
	 * which does not exist on the partner's tile on curve tiles. Repair stale
	 * bits BEFORE re-spacing: RespaceFlippedHalf derives its step axes from
	 * the track bits, and a stale bit walks vehicles off the rail. */
	ChainOnRail(v);
	RespaceFlippedHalf(v, joint_at_tail);
	/* Re-spacing can move vehicles across tile boundaries; repair again so
	 * the seam fix in AdvanceWagonsAfterCouple also sees valid track. */
	ChainOnRail(v);
}

/**
 * Make the moving consist take over the waiting consist's schedule: the order
 * list (incl. shared chain membership), the primary order list, the execution
 * position and the dispatch/timetable state are all taken from the waiting
 * consist instead of keeping the moving consist's own schedule.
 * @param v the surviving (moving) consist's primary vehicle
 * @param u the waiting consist's primary vehicle
 */
static void AdoptCoupleWaitingSchedule(Train *v, Train *u)
{
	if (u->orders == nullptr) return;
	OrderList *adopted = u->orders;

	if (v->orders != adopted) {
		/* Leave our own order list first. */
		if (v->IsOrderListShared()) {
			v->RemoveFromShared();
		} else if (v->orders->IsPlayerCreated()) {
			v->orders->RemoveVehicle(v);
		} else if (OrderListIsSomeonesHome(v->orders)) {
			/* Another vehicle away on an execute-schedule detour calls this list home, so it has
			 * to survive. Only detach from it; leaving the vehicle in the chain with no orders
			 * would make a later join allocate a fresh empty list for it. */
			v->orders->RemoveVehicle(v);
		} else {
			v->orders->FreeChain(false);
		}
		v->orders = adopted;
		adopted->AssignVehicle(v);
	}

	/* Take over the waiting consist's schedule state wholesale. */
	v->primary_order = u->primary_order;
	v->primary_order_index = u->primary_order_index;
	v->execute_schedule_stack = u->execute_schedule_stack;
	v->cur_real_order_index = u->cur_real_order_index;
	v->cur_implicit_order_index = u->cur_implicit_order_index;
	v->cur_timetable_order_index = u->cur_timetable_order_index;
	/* Do not take over the waiting consist's per-leg timer: it counts the whole
	 * wait-for-couple dwell, which must not be charged to the first leg the
	 * merged consist measures. The approach leg of the couple order was already
	 * booked and cleared in Couple() above. */
	v->current_order_time = 0;
	v->lateness_counter = u->lateness_counter;
	v->timetable_start = u->timetable_start;
	v->dispatch_records = u->dispatch_records;
	u->dispatch_records.clear();
	v->vehicle_flags.Set(VehicleFlag::ScheduledDispatch, u->vehicle_flags.Test(VehicleFlag::ScheduledDispatch));
	v->vehicle_flags.Set(VehicleFlag::TimetableSeparation, u->vehicle_flags.Test(VehicleFlag::TimetableSeparation));
	v->vehicle_flags.Set(VehicleFlag::AutomateTimetable, u->vehicle_flags.Test(VehicleFlag::AutomateTimetable));
	v->vehicle_flags.Set(VehicleFlag::SeparationActive, u->vehicle_flags.Test(VehicleFlag::SeparationActive));

	/* The taken-over position sits on the order the waiting consist was held
	 * at -- usually 'wait for couple'. Advance past it, otherwise the merged
	 * consist would re-enter the wait and never continue. */
	if (u->current_order.IsType(OT_WAIT_COUPLE)) {
		v->IncrementRealOrderIndex();
	}

	/* Let the next ProcessOrders pick up the order at the taken-over position. */
	v->current_order.Free();
	v->SetDestTile(INVALID_TILE);
	v->last_station_visited = u->last_station_visited;
}

/**
 * Couple the train \a u onto the train \a v.
 */
void Couple(Train *v, Train *u)
{
	/* v may arrive as the physical moving front (Last() when driving backwards,
	 * which for an articulated train is a non-primary part). All order handling
	 * (IncrementImplicitOrderIndex / ProcessOrders) and NormaliseTrainHead must
	 * run on the primary vehicle, so normalise to the chain heads first. */
	v = v->Primary();
	u = u->Primary();
	if (!IsTrainCouplingAllowed(v->owner, u->owner)) return;


	/* Stop the order/speed carrier before processing successor orders. The
	 * moving endpoint can be a wagon, especially after a previous coupling.
	 * A wait-at-station successor must see a stopped train; otherwise it
	 * clears last_station_visited and sends the consist out to re-enter. */
	v->cur_speed = 0;
	v->subspeed = 0;
	v->progress = 0;

	/* Book the approach leg of the couple order and reset the per-leg timer.
	 * Reaching a couple order never passes through TrainEnterStation or
	 * BeginLoading, so without this the approach time is neither recorded in
	 * the timetable nor removed from the timer; the merged consist would
	 * charge the whole approach (and anything waited before it) to its first
	 * post-couple leg. When the waiting consist's schedule is adopted below,
	 * the booking on this order's list is simply abandoned together with this
	 * schedule; the timer reset is what matters there. */
	if (v->current_order.IsType(OT_GOTO_COUPLE)) {
		if (v->cur_timetable_order_index == INVALID_VEH_ORDER_ID) v->cur_timetable_order_index = v->cur_real_order_index;
		if (v->cur_timetable_order_index == v->cur_real_order_index) UpdateVehicleTimetable(v, true);
	}

	/* The couple order may instruct the consist to adopt the waiting
	 * consist's schedule; current_order is still that order here. */
	const std::string waiting_service_number = u->GetActiveServiceNumber();
	const bool moving_has_service_number = !v->service_number_suppressed && !v->GetActiveServiceNumber().empty();
	bool adopt_waiting_schedule = v->current_order.IsType(OT_GOTO_COUPLE) && v->current_order.GetCoupleUseWaitingSchedule();

	/*
	 * Orientation phase: v will stay the front of the merged consist
	 * (ClearFrontWagon/ClearFrontEngine + NormaliseTrainHead below), so both
	 * trains must face the same way BEFORE the merge. All reversals here are
	 * full physical flips (direction + position + tile, via
	 * ReverseTrainForCouple); no direction adjustments happen after this
	 * point.
	 */

	/* Which end of v reaches the joint: v is the moving train and u is
	 * stationary, so v's own driving state decides it. If v is driving
	 * backwards its tail meets u (no flip needed); otherwise its head meets
	 * u, so v must be turned around and approach tail-first. */
	if (!v->IsDrivingBackwards()) {
		/* Its tail meets the waiting consist after the flip. */
		ReverseTrainForCouple(v, true);
		v = v->Primary();
	}

	/* u must face the (possibly reversed) v within 45 degrees to couple as-is. */
	DirDiff dir_diff = DirDifference(v->direction, u->direction);
	if (dir_diff != DirDiff::Same && dir_diff != DirDiff::Right45 && dir_diff != DirDiff::Left45) {
		/* The waiting consist's head is the end that meets the partner. */
		ReverseTrainForCouple(u, false);
		u = u->Primary();
	} else {
		u = u->Primary();
	}


	/* The couple happened at the station of the GOTO_COUPLE order; record it
	 * as the last visited station (the movement-loop coupling path never goes
	 * through TrainEnterStation). Without this the implicit order bookkeeping
	 * on departure runs with an invalid station and derails order advancement. */
	v->last_station_visited = v->current_order.GetDestination().ToStationID();

	/* The coupled consist's front (u) becomes a non-primary wagon of the merged
	 * consist, so any trace restrict slot it held must be transferred to the
	 * survivor front, otherwise the slot ends up pointing at a non-primary
	 * vehicle. Same convention as autoreplace.
	 *
	 * This has to happen BEFORE the order advance below: that advance executes
	 * the next order, so a slot-release order right after the couple ("return
	 * the slot") runs there, and it vacates the slot for the surviving consist's
	 * own primary. If the slot is still registered under u at that point, the
	 * release is a no-op and the merged consist would keep holding a slot that
	 * it already gave back. */
	const TraceRestrictSlotID auto_return_slot = u->wait_couple_acquired_slot;
	const auto *auto_return = TraceRestrictSlot::GetIfValid(auto_return_slot);
	const bool approach_already_holds_slot = auto_return != nullptr && auto_return->IsOccupant(v->index);
	const bool transferred_slots = u->vehicle_flags.Test(VehicleFlag::HaveSlot);
	if (transferred_slots) {
		TraceRestrictTransferVehicleOccupantInAllSlots(u->index, v->index);
		u->vehicle_flags.Reset(VehicleFlag::HaveSlot);
		v->vehicle_flags.Set(VehicleFlag::HaveSlot);
	}

	/* The couple order is done, so load the one after it. This must not happen
	 * when the consist takes over the waiting consist's schedule: that schedule
	 * is about to replace ours wholesale, and running our own next order first
	 * leaves its effects behind for the replacement not to undo. A slot release
	 * right after the couple is the visible case, vacating a slot the transfer
	 * above has just moved to us. The adopted successor is loaded further down
	 * instead, see the adopt_waiting_schedule block. */
	if (!adopt_waiting_schedule) {
		v->IncrementImplicitOrderIndex();
		ProcessOrders(v);
	}

	/* TryTrainCouple performs chain surgery (MakeTrainBackup/ArrangeTrains) and
	 * needs the physical chain head; the primary vehicle is only the identity
	 * for user-facing messages below. */
	Train *v_phys = v->First();
	Train *u_phys = u->First();

	Train *v_last = v_phys->Last();

	/* Preserve each part's last-loading-station attribution across the merge:
	 * after coupling, VehicleIncreaseStats can keep attributing every wagon's
	 * links to the station it actually loaded at, even though the two parts
	 * may have loaded at different stations. */
	PropagateLastLoadingStation(v);
	PropagateLastLoadingStation(u);

	if (!TryTrainCouple(v_phys, u_phys)) {
		if (v->owner == _local_company) {
			AddVehicleAdviceNewsItem(AdviceType::Order, GetEncodedString(STR_NEWS_ORDER_COUPLE_FAILED, v->index, u->index), v->index);
		}
		/* The couple did not happen, so give the waiting consist its slots back. */
		if (transferred_slots && v->vehicle_flags.Test(VehicleFlag::HaveSlot)) {
			TraceRestrictTransferVehicleOccupantInAllSlots(v->index, u->index);
			v->vehicle_flags.Reset(VehicleFlag::HaveSlot);
			u->vehicle_flags.Set(VehicleFlag::HaveSlot);
		}
		return;
	}

	/* Chain merge succeeded. Release only the acquired waiting membership;
	 * a slot already held independently by the approaching train survives. */
	if (auto *slot = TraceRestrictSlot::GetIfValid(auto_return_slot)) {
		slot->Vacate(u);
		if (!approach_already_holds_slot) slot->Vacate(v);
	}
	u->wait_couple_acquired_slot = TraceRestrictSlotID::Invalid();

	/* The absorbed consist's primary pointers must follow the surviving
	 * primary, otherwise vehicles of the absorbed part resolve their consist
	 * info (orders, name, group) from a stale mid-chain vehicle. */
	Train *v_prim = v->Primary();
	for (Train *w = v->First(); w != nullptr; w = w->Next()) {
		w->SetPrimary(v_prim);
	}
	/* The merged consist keeps the last-loading-station records of both parts;
	 * switch to per-vehicle attribution when they loaded at different
	 * stations. */
	NormalizeLastLoadingStation(v_prim);

	/* The consist windows keyed to u are left to NormaliseTrainHead() below: it re-keys exactly
	 * one window per class to the primary of the merged chain, which keeps the window of v (the
	 * part the player was driving) and closes the one of u, or moves u's over to v when v had no
	 * window open at all. */
	DeleteNewGRFInspectWindow(GrfSpecFeature::Trains, u->index.base());
	SetWindowDirty(WindowClass::Company, _current_company);

	/* Delete orders, group stuff and the unit number as we're not the front of any vehicle anymore. */

	/* The moving consist may take over the waiting consist's schedule; do this
	 * before the waiting consist's orders are removed, so its state can be
	 * copied and the surviving consist can join its shared chain. */
	if (adopt_waiting_schedule) AdoptCoupleWaitingSchedule(v, u);

	DeleteVehicleOrders(u);
	u->ReleaseUnitNumber();
	GroupStatistics::CountVehicle(u, -1);

	v->profit_this_year += u->profit_this_year;
	v->profit_last_year += u->profit_last_year;

	u->profit_last_year = 0;
	u->profit_this_year = 0;

	u->ClearFrontWagon();
	u->ClearFrontEngine();

	/* Normalise from the physical chain head, not from the primary: the vehicles ahead of the
	 * primary have to be in the chain for their windows to be re-keyed as well. */
	NormaliseTrainHead(v->First(), CCF_COUPLE);
	RecordSyncEvent(NSRE_COUPLE);

	/* The train is now one consist again: it is no longer a decoupled part. */
	v->Primary()->decouple_part = 0;
	if (!moving_has_service_number && !waiting_service_number.empty()) {
		v->Primary()->fixed_service_number = waiting_service_number;
		v->Primary()->service_number_suppressed = false;
	}
	InvalidateWindowClassesData(WindowClass::DepartureBoard);

	/* The merge moved vehicles -- and the primary that carries the breakdown
	 * bookkeeping -- between the two consists, so recompute it for the merged
	 * consist; see #RefreshConsistBreakdownState. The absorbed consist's own copy
	 * of the flag went out of use with its front engine above. */
	RefreshConsistBreakdownState(v->Primary());

	/* [FIX-couple] Post-couple flag cleanup. Direction is NOT touched here: it
	 * is maintained by the standard movement pipeline (AdvanceWagonsAfterCouple
	 * -> TrainController recomputes per-vehicle direction from the track every
	 * tick), so any manual override (e.g. forcing u->direction to a quantized
	 * travel axis) desyncs direction from the vehicle's pixels and produces the
	 * 45-degree visual skew seen before. Only clear stale flags that would make
	 * the next tick trigger an unrequested auto-reversal.
	 *
	 * Iterate from the physical chain head: the primary may sit mid-chain, so
	 * starting the walk at it would leave the vehicles ahead of it - which this
	 * cleanup is meant to cover just as much - carrying a stale flag. */
	for (Train *w = v->First(); w != nullptr; w = w->Next()) {
		w->flags.Reset(VehicleRailFlag::Reversed);
	}
	/* Reversing is a property of the consist as a whole: it is set on and read
	 * from the primary (see the CheckReverseTrain calls and the movement
	 * handler), so it is cleared there instead of on every vehicle. */
	v->flags.Reset(VehicleRailFlag::Reversing);

	/* If the absorbed consist was queued for loading/unloading at a station,
	 * drop the stale entry: it can never leave the queue by itself anymore
	 * and its load_unload_ticks would eventually hit zero and assert. */
	u->RemoveFromLoadingQueues();
	AdvanceWagonsAfterCouple(v_last);

	/* Stale look-ahead/reservations from before the flip: rebuild them for
	 * the combined train's new position and direction, otherwise it drives
	 * blindly (and may end up in a depot) until it hits a PBS signal. */
	v->Primary()->lookahead.reset();
	if (u != nullptr) u->lookahead.reset();
	if (adopt_waiting_schedule) {
		/* AdoptCoupleWaitingSchedule took over u's schedule and freed
		 * current_order, only fixing the order index. Load the successor
		 * order now so the reservation and reversal checks below operate on
		 * the adopted destination instead of leaving the merged consist
		 * orderless. Otherwise a head-couple is oriented by no order at all
		 * and its first forward reservation fails, reporting the consist as
		 * lost before it ever departs. This mirrors the adopt-off path, where
		 * the ProcessOrders in Couple() already loaded the next order. */
		ProcessOrders(v);
	}
	InvalidateWindowClassesData(WindowClass::TrainList);
	/* The physical chain order is now correct (orientation is fixed before
	 * the merge), so give the consist the usual chance to reverse in the
	 * station, e.g. when its next order points the other way. CheckReverseTrain
	 * only sets Reversing; the actual turnaround still happens at the usual
	 * spot in the movement handler (speed zero + Reversing flag). Do not reserve
	 * ahead until this decision is made: ReverseTrainDirection will reserve in
	 * the new direction when the turnaround completes. */
	Train *consist = v->Primary();
	consist->cur_speed = 0;
	consist->subspeed = 0;
	consist->progress = 0;
	if (IsHoldingForCouple(consist)) {
		FreeTrainTrackReservation(consist);
		HoldWaitingTrainBody(consist);
		return;
	}
	if (CheckReverseTrain(consist)) {
		consist->flags.Set(VehicleRailFlag::Reversing);
	} else {
		/* No turnaround pending: re-check the signals and rebuild the merged
		 * consist's reservation now, exactly like a completed turnaround does. */
		RecheckSignalsAndTryReserve(consist);
	}
}

/**
 * Centre-to-centre distance between the two vehicles that meet at a coupling
 * joint when their ends just touch. Follows the same rounding rule as
 * Train::CalcNextVehicleOffset(): for a vehicle of odd length the half towards
 * the front is one unit longer, so the leading vehicle rounds up its own half
 * only when driving backwards, and the following vehicle gets the complementary
 * rounding.
 * @param front_len              Length (in vehicle units) of the leading vehicle.
 * @param back_len               Length (in vehicle units) of the following vehicle.
 * @param front_driving_backwards Whether the leading vehicle is driving backwards.
 * @return Distance in vehicle units (same units as x_pos/y_pos offsets).
 */
int CoupleJointOffset(uint8_t front_len, uint8_t back_len, bool front_driving_backwards)
{
	uint8_t rounding = front_driving_backwards ? 1 : 0;
	return (front_len + rounding) / 2 + (back_len + 1 - rounding) / 2;
}
