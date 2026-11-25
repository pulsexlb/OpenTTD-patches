/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file pbs_air.cpp Directed rolling reservations for aircraft. */

#include "stdafx.h"
#include "air_map.h"
#include "aircraft.h"
#include "pbs_air.h"
#include "airport.h"
#include "pathfinder/yapf/yapf.h"
#include "station_base.h"
#include "window_func.h"
#include "pathfinder/follow_track.hpp"
#include "sl/saveload.h"
#include <map>
#include <set>
#include <tuple>
#include "sl/extended_ver_sl.h"
#include "scope.h"
#include "date_func.h"

#include "safeguards.h"

/* Derived indexes; saved per-aircraft records are authoritative. */
using AircraftTileOwners = std::map<VehicleID, TrackBits>;
static std::map<TileIndex, AircraftTileOwners> _aircraft_tile_owners;
static std::map<TileIndex, VehicleID> _aircraft_terminal_owners;
static std::map<TileIndex, VehicleID> _aircraft_runway_owners;
/* A complete directed taxi route is acquired atomically. Virtual reservations
 * may share a track in the same direction, or cross on different tracks.
 * The exclusive two-tile real window follows only the acquired route. */
static std::map<TileIndex, std::map<VehicleID, TrackdirBits>> _aircraft_virtual_bookings;

struct AircraftJunctionGeometry {
	std::map<TileIndex, std::set<TileIndex>> regions;
	std::map<TileIndex, std::set<TileIndex>> tiles;
};
static std::map<StationID, AircraftJunctionGeometry> _aircraft_junction_geometry;
static std::set<VehicleID> _aircraft_junction_owners;
static std::map<VehicleID, Aircraft *> _aircraft_junction_requests;
static std::set<VehicleID> _aircraft_junction_legacy;
static bool _aircraft_junction_loading = false;

struct AircraftJunctionState {
	std::vector<AircraftJunctionAction> actions;
	explicit AircraftJunctionState(const Aircraft *v) : actions(v->junction_actions) {}
};
static void RestoreAircraftJunctions(Aircraft *v, const AircraftJunctionState &saved);

static bool AircraftJunctionTracksConflict(TrackBits a, TrackBits b);
static void SyncAircraftJunctionRecords(Aircraft *v);
static bool CheckAircraftJunctionAccess(const Aircraft *v, TileIndex tile, TrackBits tracks = TRACK_BIT_ALL);
static bool TryReserveAircraftJunctions(Aircraft *v, TileIndex tile, TrackBits tracks, TileIndex runway = INVALID_TILE);
static void PruneAircraftJunctions(Aircraft *v, bool keep_current = true);
static void ReleaseAircraftJunctions(Aircraft *v);
static void CancelAircraftJunctionRequest(Aircraft *v);
static void RefreshAircraftLegacyJunctionFootprint(Aircraft *v);
static void FinishAircraftLegacyJunctionAction(Aircraft *v);

static const Direction _aircraft_envelope_directions[TRACK_END] = {
	Direction::Invalid, Direction::Invalid, Direction::N, Direction::S, Direction::W, Direction::E,
};

static bool IsTaxiTile(TileIndex tile)
{
	return IsValidTile(tile) && IsAirportTile(tile) && MayHaveAirTracks(tile);
}

static TrackBits AircraftReservedTracks(TileIndex tile)
{
	TrackBits tracks = TRACK_BIT_NONE;
	auto owners = _aircraft_tile_owners.find(tile);
	if (owners != _aircraft_tile_owners.end()) {
		for (const auto &[id, held] : owners->second) tracks |= held;
	}
	return tracks;
}

static TrackBits AircraftOwnedTracks(const Aircraft *v, TileIndex tile)
{
	auto owners = _aircraft_tile_owners.find(tile);
	if (owners == _aircraft_tile_owners.end()) return TRACK_BIT_NONE;
	auto owner = owners->second.find(v->index);
	return owner == owners->second.end() ? TRACK_BIT_NONE : owner->second;
}

void ClearAircraftReservationIndexes()
{
	_aircraft_tile_owners.clear();
	_aircraft_terminal_owners.clear();
	_aircraft_runway_owners.clear();
	_aircraft_virtual_bookings.clear();
	_aircraft_junction_geometry.clear();
	_aircraft_junction_owners.clear();
	_aircraft_junction_requests.clear();
	_aircraft_junction_legacy.clear();
}

void InvalidateAircraftJunctionGeometry(const Station *st)
{
	for (auto it = _aircraft_junction_requests.begin(); it != _aircraft_junction_requests.end();) {
		Aircraft *v = it->second;
		++it;
		if ((IsTaxiTile(v->tile) && GetStationIndex(v->tile) == st->index) ||
				(!v->junction_request_regions.empty() && IsTaxiTile(v->junction_request_regions.front()) &&
				GetStationIndex(v->junction_request_regions.front()) == st->index)) CancelAircraftJunctionRequest(v);
	}
	_aircraft_junction_geometry.erase(st->index);
}

static TileIndex AircraftJunctionStep(TileIndex tile, Direction dir)
{
	const auto delta = TileIndexDiffCByDir(dir);
	return TileAddWrap(tile, delta.x, delta.y);
}

static std::vector<TileIndex> RunwayTiles(TileIndex extreme);

static const AircraftJunctionGeometry &GetAircraftJunctionGeometry(StationID station)
{
	auto [cached, inserted] = _aircraft_junction_geometry.try_emplace(station);
	if (!inserted) return cached->second;
	auto &geometry = cached->second;
	const Station *st = Station::GetIfValid(station);
	if (st == nullptr) return geometry;
	for (TileIndex tile : st->airport) {
		if (!IsTaxiTile(tile) || !IsAirportTileOfStation(tile, station) || IsHangar(tile) || IsHeliportTile(tile)) continue;
		const TrackBits tracks = GetAirportTileTracks(tile);
		uint8_t exits = 0;
		std::set<TileIndex> connected;
		for (TrackdirBits bits = TrackBitsToTrackdirBits(tracks); bits != TRACKDIR_BIT_NONE; bits = KillFirstBit(bits)) {
			const Trackdir td = static_cast<Trackdir>(FindFirstBit(bits));
			CFollowTrackAirport follower(INVALID_OWNER);
			if (follower.Follow(tile, td) && IsTaxiTile(follower.new_tile) && IsAirportTileOfStation(follower.new_tile, station)) {
				SetBit(exits, to_underlying(TrackdirToExitdir(td)));
				connected.insert(follower.new_tile);
			}
		}
		if (!TracksOverlap(tracks) || CountBits(exits) < 3) continue;
		auto &zone = geometry.regions[tile];
		zone.insert(tile);
		zone.insert(connected.begin(), connected.end());
		/* Include only built half-track envelopes, not every nearby tile.
		 * Shared approach tiles do not merge otherwise independent junctions. */
		for (Track track : SetTrackBitIterator(tracks)) {
			if (IsDiagonalTrack(track)) continue;
			TileIndex neighbour = AircraftJunctionStep(tile, _aircraft_envelope_directions[track]);
			if (IsTaxiTile(neighbour) && IsAirportTileOfStation(neighbour, station) &&
					HasTrack(GetAirportTileTracks(neighbour), TrackToOppositeTrack(track))) zone.insert(neighbour);
		}
	}
	for (const auto &[key, tiles] : geometry.regions) {
		for (TileIndex tile : tiles) geometry.tiles[tile].insert(key);
	}
	return geometry;
}

bool IsAircraftTerminalAvailable(const Aircraft *v, TileIndex tile)
{
	if (v->ignore_airport_reservations) return IsTaxiTile(tile);
	auto it = _aircraft_terminal_owners.find(tile);
	if (it != _aircraft_terminal_owners.end() && it->second != v->index) return false;
	auto occupied = _aircraft_tile_owners.find(tile);
	if (occupied != _aircraft_tile_owners.end()) {
		for (const auto &[id, tracks] : occupied->second) {
			if (id != v->index) return false;
		}
	}
	return IsTaxiTile(tile);
}

bool TryClaimAircraftTerminal(Aircraft *v, TileIndex tile)
{
	if (!IsAircraftTerminalAvailable(v, tile)) {
		return false;
	}
	if (v->allocated_terminal != tile) ReleaseAircraftTerminal(v);
	if (!v->ignore_airport_reservations) _aircraft_terminal_owners[tile] = v->index;
	v->allocated_terminal = tile;
	return true;
}

void ReleaseAircraftTerminal(Aircraft *v)
{
	auto it = _aircraft_terminal_owners.find(v->allocated_terminal);
	if (it != _aircraft_terminal_owners.end() && it->second == v->index) {
		_aircraft_terminal_owners.erase(it);
	}
	v->allocated_terminal = INVALID_TILE;
}

bool HasAircraftTileReservation(const Aircraft *v, TileIndex tile, TrackBits tracks)
{
	if (!IsTaxiTile(tile)) return false;
	if (v->ignore_airport_reservations) {
		for (const auto &record : v->reserved_path) {
			if (record.tile == tile) return (record.tracks & tracks) == tracks;
		}
		return false;
	}
	TrackBits held = AircraftOwnedTracks(v, tile);
	if (held == TRACK_BIT_NONE || (held & tracks) != tracks) return false;
	for (const auto &r : v->reserved_path) {
		if (r.tile == tile) return r.tracks == held && (GetReservedAirportTracks(tile) & tracks) == tracks;
	}
	return false;
}

static AircraftTaxiBlock MergeAircraftBlock(AircraftTaxiBlock a, AircraftTaxiBlock b)
{
	return static_cast<AircraftTaxiBlock>(std::max(to_underlying(a), to_underlying(b)));
}

static bool IsAircraftTaxiSharingAllowed(const Aircraft *v, TileIndex tile, const Aircraft *other)
{
	return !IsApron(tile) && !IsHangar(tile) && !v->vehstatus.Test(VehState::Hidden) &&
			other != nullptr && !other->vehstatus.Test(VehState::Crashed) &&
			(other->state == AS_RUNNING || other->state == AS_IDLE);
}

/**
 * Phasing is a temporary escape from a prolonged jam, not a license to drive
 * through another aircraft. Two phasing aircraft still see each other: only
 * the soft reservation barriers drop, never the physical body conflict.
 *
 * The arbitration must be a pure function of each aircraft's own state so both
 * sides independently reach the same verdict; otherwise A waits for B and B
 * waits for A. The strict (x_pos, y_pos, index) key gives exactly that order:
 * the aircraft further back along the taxiway yields, and the vehicle index
 * breaks the tie when two bodies sit at the identical spot.
 */
static bool AircraftPhasingConcedes(const Aircraft *v, const Aircraft *other)
{
	if (other == nullptr || other == v) return false;
	if (!HasBit(v->flags, VAF_TAXI_PHASING) || !HasBit(other->flags, VAF_TAXI_PHASING)) return false;
	/* A manual "ignore reservations" aircraft never participates; the player
	 * asked for it to pass through and nothing may push back against that. */
	if (v->ignore_airport_reservations || other->ignore_airport_reservations) return false;
	return std::tie(v->x_pos, v->y_pos, v->index) > std::tie(other->x_pos, other->y_pos, other->index);
}

/* This classification never picks a winner from the unordered physical tile hash. */
static AircraftTaxiBlock CheckAircraftTaxiTile(const Aircraft *v, TileIndex tile, bool phasing, bool check_runway = true, TrackBits want = TRACK_BIT_ALL, bool check_junction = true)
{
	if (!IsTaxiTile(tile)) return AircraftTaxiBlock::Protected;
	if (v->ignore_airport_reservations) return AircraftTaxiBlock::None;
	/* A timed-out aircraft ignores reservations, not geometry or dirty map
	 * records. Junction grants and runway owner claims are both "reserved". */
	if (check_junction && !CheckAircraftJunctionAccess(v, tile, want)) {
		return AircraftTaxiBlock::Protected;
	}
	auto runway = _aircraft_runway_owners.find(tile);
	if (check_runway && !phasing && ((runway != _aircraft_runway_owners.end() && runway->second != v->index) ||
			(IsRunway(tile) && GetReservationAsRunway(tile) && runway == _aircraft_runway_owners.end()))) {
		return AircraftTaxiBlock::Protected;
	}
	if (GetReservedAirportTracks(tile) != AircraftReservedTracks(tile)) {
		return AircraftTaxiBlock::Protected;
	}
	AircraftTaxiBlock block = AircraftTaxiBlock::None;
	auto inspect = [&](const Aircraft *other) {
		if (other == v || (other != nullptr && other->ignore_airport_reservations)) return;
		bool shareable = IsAircraftTaxiSharingAllowed(v, tile, other);
		if (phasing) {
			/* Two phasing bodies may not overlap. Only the one that loses the
			 * deterministic arbitration treats the other as a hard block. */
			if (!AircraftPhasingConcedes(v, other)) return;
			block = MergeAircraftBlock(block, AircraftTaxiBlock::Protected);
			return;
		}
		block = MergeAircraftBlock(block, shareable ? AircraftTaxiBlock::Traffic : AircraftTaxiBlock::Protected);
	};
	auto owners = _aircraft_tile_owners.find(tile);
	if (owners != _aircraft_tile_owners.end()) {
		for (const auto &[id, tracks] : owners->second) {
			if (!AircraftJunctionTracksConflict(want, tracks)) continue;
			const Vehicle *other = Vehicle::GetIfValid(id);
			inspect(other != nullptr && other->type == VehicleType::Aircraft ? Aircraft::From(other) : nullptr);
		}
	}
	for (const Aircraft *other : VehiclesOnTile<VehicleType::Aircraft>(tile)) {
		if (!other->IsNormalAircraft() || other->tile != tile || other->vehstatus.Test(VehState::Hidden) || other->IsAircraftFlying()) continue;
		TrackBits body = TRACK_BIT_NONE;
		if (IsValidTrackdir(other->trackdir)) body |= TrackToTrackBits(TrackdirToTrack(other->trackdir));
		if (IsValidTrackdir(other->next_trackdir)) body |= TrackToTrackBits(TrackdirToTrack(other->next_trackdir));
		if (body == TRACK_BIT_NONE || AircraftJunctionTracksConflict(want, body)) inspect(other);
	}
	return block;
}

static AircraftTaxiBlock CheckIncomingAircraftEnvelopes(const Aircraft *v, TileIndex tile, bool phasing, TrackBits tracks = TRACK_BIT_ALL)
{
	AircraftTaxiBlock block = AircraftTaxiBlock::None;
	if (v->ignore_airport_reservations) return block;
	for (Track track = TRACK_UPPER; track < TRACK_END; ++track) {
		if (!HasTrack(tracks, TrackToOppositeTrack(track))) continue;
		TileIndex neighbour = TileAddByDir(tile, ReverseDir(_aircraft_envelope_directions[track]));
		if (!IsTaxiTile(neighbour) || !HasAirportTrackReserved(neighbour, track)) continue;
		if (GetReservedAirportTracks(neighbour) != AircraftReservedTracks(neighbour)) {
			return AircraftTaxiBlock::Protected;
		}
		for (const auto &[id, tracks] : _aircraft_tile_owners.at(neighbour)) {
			if (id == v->index || !HasTrack(tracks, track)) continue;
			const Vehicle *vehicle = Vehicle::GetIfValid(id);
			const Aircraft *other = vehicle != nullptr && vehicle->type == VehicleType::Aircraft ? Aircraft::From(vehicle) : nullptr;
			if (phasing) {
				if (!AircraftPhasingConcedes(v, other)) continue;
				block = MergeAircraftBlock(block, AircraftTaxiBlock::Protected);
				continue;
			}
			const bool shareable = IsAircraftTaxiSharingAllowed(v, tile, other) && !IsApron(neighbour) && !IsHangar(neighbour);
			block = MergeAircraftBlock(block, shareable ? AircraftTaxiBlock::Traffic : AircraftTaxiBlock::Protected);
		}
	}
	return block;
}

/* A half-track sweeps only the facing half-track of its diagonal neighbour.
 * A centre stand or a diagonal track there is not part of that envelope. */
static AircraftTaxiBlock CheckAircraftAssociatedTrack(const Aircraft *v, TileIndex tile, Track track, bool phasing)
{
	TileIndex neighbour = TileAddByDir(tile, _aircraft_envelope_directions[track]);
	if (!IsTaxiTile(neighbour) || !IsAirportTileOfStation(neighbour, GetStationIndex(tile)) || IsHangar(neighbour)) {
		return AircraftTaxiBlock::Protected;
	}
	if (v->ignore_airport_reservations) return AircraftTaxiBlock::None;
	if (!CheckAircraftJunctionAccess(v, neighbour, TrackToTrackBits(TrackToOppositeTrack(track)))) {
		 
		return AircraftTaxiBlock::Protected;
	}
	auto runway = _aircraft_runway_owners.find(neighbour);
	if (!phasing && IsRunway(neighbour) && GetReservationAsRunway(neighbour) &&
			(runway == _aircraft_runway_owners.end() || runway->second != v->index)) {
		 
		return AircraftTaxiBlock::Protected;
	}
	if (GetReservedAirportTracks(neighbour) != AircraftReservedTracks(neighbour)) {
		 
		return AircraftTaxiBlock::Protected;
	}

	Track opposite = TrackToOppositeTrack(track);
	AircraftTaxiBlock block = AircraftTaxiBlock::None;
	auto inspect = [&](const Aircraft *other) {
		if (other == v || (other != nullptr && other->ignore_airport_reservations)) return;
		if (phasing) {
			if (!AircraftPhasingConcedes(v, other)) return;
			block = MergeAircraftBlock(block, AircraftTaxiBlock::Protected);
			return;
		}
		bool shareable = IsAircraftTaxiSharingAllowed(v, neighbour, other) && !IsApron(tile) && !IsHangar(tile);
		block = MergeAircraftBlock(block, shareable ? AircraftTaxiBlock::Traffic : AircraftTaxiBlock::Protected);
	};
	auto owners = _aircraft_tile_owners.find(neighbour);
	if (owners != _aircraft_tile_owners.end()) {
		for (const auto &[id, tracks] : owners->second) {
			if (!HasTrack(tracks, opposite)) continue;
			const Vehicle *other = Vehicle::GetIfValid(id);
			inspect(other != nullptr && other->type == VehicleType::Aircraft ? Aircraft::From(other) : nullptr);
		}
	}
	for (const Aircraft *other : VehiclesOnTile<VehicleType::Aircraft>(neighbour)) {
		if (!other->IsNormalAircraft() || other->tile != neighbour || other->vehstatus.Test(VehState::Hidden) || other->IsAircraftFlying()) continue;
		if ((IsValidTrackdir(other->trackdir) && TrackdirToTrack(other->trackdir) == opposite) ||
				(IsValidTrackdir(other->next_trackdir) && TrackdirToTrack(other->next_trackdir) == opposite)) inspect(other);
	}
	if (block != AircraftTaxiBlock::None) {
	}
	return block;
}

static AircraftTaxiBlock CheckAircraftTaxiTracks(const Aircraft *v, TileIndex tile, TrackBits tracks, bool phasing)
{
	if (!IsTaxiTile(tile)) return AircraftTaxiBlock::Protected;
	AircraftTaxiBlock block = MergeAircraftBlock(CheckAircraftTaxiTile(v, tile, phasing, true, tracks == TRACK_BIT_NONE ? TRACK_BIT_ALL : tracks),
		CheckIncomingAircraftEnvelopes(v, tile, phasing, tracks == TRACK_BIT_NONE ? TRACK_BIT_ALL : tracks));
	/* TRACK_BIT_NONE requests a tile-level-only check (runway acquisition). */
	if (tracks == TRACK_BIT_NONE) return block;
	if ((GetAirportTileTracks(tile) & tracks) != tracks) return AircraftTaxiBlock::Protected;
	while (tracks != TRACK_BIT_NONE) {
		Track track = RemoveFirstTrack(&tracks);
		if (IsDiagonalTrack(track)) continue;
		block = MergeAircraftBlock(block, CheckAircraftAssociatedTrack(v, tile, track, phasing));
	}
	return block;
}

static bool RecordAircraftTaxiBlock(Aircraft *v, AircraftTaxiBlock block, TileIndex tile)
{
	if (block == AircraftTaxiBlock::None) return true;
	v->taxi_block_reason = MergeAircraftBlock(v->taxi_block_reason, block);
	return false;
}

bool IsAircraftTaxiEnvelopeAvailable(Aircraft *v, TileIndex tile, TrackBits tracks)
{
	return RecordAircraftTaxiBlock(v, MergeAircraftBlock(CheckAircraftTaxiTile(v, tile, HasBit(v->flags, VAF_TAXI_PHASING), true, tracks),
			CheckIncomingAircraftEnvelopes(v, tile, HasBit(v->flags, VAF_TAXI_PHASING), tracks)), tile);
}

static TrackdirBits AircraftOwnedVirtualTrackdirs(const Aircraft *v, TileIndex tile)
{
	auto bookings = _aircraft_virtual_bookings.find(tile);
	if (bookings == _aircraft_virtual_bookings.end()) return TRACKDIR_BIT_NONE;
	auto owner = bookings->second.find(v->index);
	if (owner == bookings->second.end()) return TRACKDIR_BIT_NONE;
	for (const auto &record : v->virtual_path) {
		if (record.tile == tile && record.trackdirs == owner->second) return record.trackdirs;
	}
	return TRACKDIR_BIT_NONE;
}

bool HasAircraftVirtualTileReservation(const Aircraft *v, TileIndex tile, TrackdirBits travel)
{
	if (v->ignore_airport_reservations) {
		TrackdirBits recorded = TRACKDIR_BIT_NONE;
		for (const auto &record : v->virtual_path) { if (record.tile == tile) recorded |= record.trackdirs; }
		return travel != TRACKDIR_BIT_NONE && (recorded & travel) == travel;
	}
	return travel != TRACKDIR_BIT_NONE && (AircraftOwnedVirtualTrackdirs(v, tile) & travel) == travel;
}

static bool TryReserveAircraftTaxiRunways(Aircraft *v, TileIndex tile, TrackBits tracks);
static void PruneAircraftTaxiRunways(Aircraft *v, bool keep_physical = true);
static void ReleaseAircraftTaxiRunways(Aircraft *v);
static void RestoreAircraftTaxiRunways(Aircraft *v, const std::vector<TileIndex> &runways);

/**
 * Runway start tile this aircraft is taxiing out to, or INVALID_TILE when it
 * is not a departure. The secondary vehicle carries the planned goal state.
 */
static TileIndex GetAircraftTakeoffHead(const Aircraft *v)
{
	if (v == nullptr || !v->IsNormalAircraft() || v->IsAircraftFlying() ||
			v->vehstatus.Test(VehState::Hidden)) return INVALID_TILE;
	const Aircraft *next = v->Next();
	if (next == nullptr || next->state != AS_START_TAKEOFF) return INVALID_TILE;
	TileIndex goal = v->GetNextTile();
	if (!IsValidTile(goal) || !IsTaxiTile(goal) || !IsRunwayStart(goal)) return INVALID_TILE;
	return goal;
}

/**
 * Two aircraft may not claim real tracks on the same runway while both are
 * taxiing out to its head. Any real reservation on that axis counts, including
 * one that is merely crossing the runway on its way to the head.
 */
static bool HasAircraftTakeoffRunwayConflict(const Aircraft *v, TileIndex tile)
{
	if (!IsValidTile(tile) || !IsRunway(tile)) return false;
	const TileIndex head = GetAircraftTakeoffHead(v);
	if (head == INVALID_TILE) return false;
	const auto tiles = RunwayTiles(head);
	for (const Aircraft *other : Aircraft::Iterate()) {
		if (other == v || !other->IsNormalAircraft()) continue;
		if (GetAircraftTakeoffHead(other) != head) continue;
		for (const auto &record : other->reserved_path) {
			if (std::find(tiles.begin(), tiles.end(), record.tile) != tiles.end()) return true;
		}
	}
	return false;
}

bool TryReserveAircraftTile(Aircraft *v, TileIndex tile, TrackBits tracks, TrackdirBits travel)
{
	if (!IsTaxiTile(tile) || tracks == TRACK_BIT_NONE) return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, tile);
	const AircraftJunctionState previous_junctions(v);
	const auto previous_runways = v->taxi_runways;
	bool acquired = false;
	auto rollback = scope_guard([&]() {
		if (acquired) return;
		RestoreAircraftJunctions(v, previous_junctions);
		RestoreAircraftTaxiRunways(v, previous_runways);
	});
	if (!TryReserveAircraftJunctions(v, tile, tracks)) return false;
	if (v->ignore_airport_reservations) {
		if ((GetAirportTileTracks(tile) & tracks) != tracks ||
				(!v->planned_path.empty() && !HasAircraftVirtualTileReservation(v, tile, travel))) return false;
		for (auto &record : v->reserved_path) {
			if (record.tile == tile) {
				if (!TryReserveAircraftTaxiRunways(v, tile, record.tracks | tracks)) return false;
				record.tracks |= tracks;
				acquired = true;
				return true;
			}
		}
		if (v->reserved_path.size() >= 2) return false;
		if (!TryReserveAircraftTaxiRunways(v, tile, tracks)) return false;
		v->reserved_path.push_back({tile, tracks});
		acquired = true;
		return true;
	}
	TrackBits held = AircraftOwnedTracks(v, tile);
	TrackBits recorded = TRACK_BIT_NONE;
	for (const auto &r : v->reserved_path) { if (r.tile == tile) recorded |= r.tracks; }
	if (held != recorded || GetReservedAirportTracks(tile) != AircraftReservedTracks(tile)) {
		 
		return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, tile);
	}
	/* A taxi window cannot precede, bypass, or invent its virtual route. Direct
	 * ground acquisition (landing/building) has no planned route yet. */
	if (!v->planned_path.empty() && (travel == TRACKDIR_BIT_NONE || TrackdirBitsToTrackBits(travel) != tracks ||
			(AircraftOwnedVirtualTrackdirs(v, tile) & travel) != travel)) {
		 
		 
		return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, tile);
	}
	/* Even existing real tracks restored from an old save need a complete
	 * runway lock before they can be used. Phasing cannot bypass this lock. */
	if ((held & tracks) == tracks) {
		acquired = TryReserveAircraftTaxiRunways(v, tile, tracks);
		return acquired;
	}
	if (held == TRACK_BIT_NONE && v->reserved_path.size() >= 2) {
		 
		return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, tile);
	}
	if (IsApron(tile) && !IsAircraftTerminalAvailable(v, tile)) {
		 
		return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, tile);
	}
	/* A second departure may not start claiming real tracks on a runway that
	 * another aircraft is already using to taxi out to the same head. This
	 * check only guards new tiles of the window an aircraft does not hold yet. */
	if (held == TRACK_BIT_NONE && HasAircraftTakeoffRunwayConflict(v, tile)) {
		 
		return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, tile);
	}
	if (!RecordAircraftTaxiBlock(v, CheckAircraftTaxiTracks(v, tile, tracks, HasBit(v->flags, VAF_TAXI_PHASING)), tile)) return false;
	if (!TryReserveAircraftTaxiRunways(v, tile, held | tracks)) return false;
	for (auto &r : v->reserved_path) {
		if (r.tile != tile) continue;
		r.tracks |= tracks;
		_aircraft_tile_owners[tile][v->index] = r.tracks;
		SetAirportTracksReservation(tile, tracks);
		acquired = true;
		return true;
	}
	if (v->reserved_path.size() >= 2) return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, tile);
	v->reserved_path.push_back({tile, tracks});
	_aircraft_tile_owners[tile][v->index] = tracks;
	SetAirportTracksReservation(tile, tracks);
	acquired = true;
	return true;
}

void ReleaseAircraftTile(Aircraft *v, TileIndex tile, TrackBits tracks)
{
	for (auto it = v->reserved_path.begin(); it != v->reserved_path.end(); ++it) {
		if (it->tile != tile) continue;
		if (v->ignore_airport_reservations) {
			it->tracks &= ~tracks;
			if (it->tracks == TRACK_BIT_NONE) v->reserved_path.erase(it);
			PruneAircraftTaxiRunways(v);
			return;
		}
		auto owners = _aircraft_tile_owners.find(tile);
		if (owners == _aircraft_tile_owners.end() || owners->second.find(v->index) == owners->second.end() || owners->second.at(v->index) != it->tracks) {
			 
			v->reserved_path.erase(it);
			PruneAircraftTaxiRunways(v);
			return;
		}
		TrackBits before = AircraftReservedTracks(tile);
		it->tracks &= ~tracks;
		if (it->tracks == TRACK_BIT_NONE) {
			owners->second.erase(v->index);
			v->reserved_path.erase(it);
		} else {
			owners->second[v->index] = it->tracks;
		}
		if (owners->second.empty()) _aircraft_tile_owners.erase(owners);
		TrackBits remove = before & ~AircraftReservedTracks(tile);
		if (IsTaxiTile(tile)) {
			while (remove != TRACK_BIT_NONE) RemoveAirportTrackReservation(tile, RemoveFirstTrack(&remove));
		}
		PruneAircraftTaxiRunways(v);
		return;
	}
}

static AircraftTaxiBlock CheckAircraftVirtualTile(const Aircraft *v, TileIndex tile, TrackdirBits want)
{
	if (!IsTaxiTile(tile) || want == TRACKDIR_BIT_NONE ||
			(GetAirportTileTracks(tile) & TrackdirBitsToTrackBits(want)) != TrackdirBitsToTrackBits(want)) {
		 
		return AircraftTaxiBlock::Protected;
	}
	/* The runway lock governs real entry, not a future virtual route. */
	if (GetReservedAirportTracks(tile) != AircraftReservedTracks(tile)) {
		 
		return AircraftTaxiBlock::Protected;
	}
	for (Track track : SetTrackBitIterator(TrackdirBitsToTrackBits(want))) {
		if (IsDiagonalTrack(track)) continue;
		TileIndex neighbour = TileAddByDir(tile, _aircraft_envelope_directions[track]);
		if (!IsTaxiTile(neighbour) || !IsAirportTileOfStation(neighbour, GetStationIndex(tile)) || IsHangar(neighbour)) {
			 
			return AircraftTaxiBlock::Protected;
		}
	}

	if (v->ignore_airport_reservations) return AircraftTaxiBlock::None;
	TrackdirBits others = TRACKDIR_BIT_NONE;
	auto booking = _aircraft_virtual_bookings.find(tile);
	if (booking != _aircraft_virtual_bookings.end()) {
		for (const auto &[id, bits] : booking->second) {
			if (id != v->index) others |= bits;
		}
	}
	for (TrackdirBits bits = want; bits != TRACKDIR_BIT_NONE; bits = KillFirstBit(bits)) {
		Trackdir td = static_cast<Trackdir>(FindFirstBit(bits));
		if ((others & TrackdirToTrackdirBits(ReverseTrackdir(td))) != TRACKDIR_BIT_NONE) {
			return AircraftTaxiBlock::Traffic;
		}
	}
	return AircraftTaxiBlock::None;
}

bool IsAircraftVirtualTileAvailable(const Aircraft *v, TileIndex tile, Trackdir td)
{
	return IsTaxiTile(v->tile) && IsTaxiTile(tile) && IsAirportTileOfStation(tile, GetStationIndex(v->tile)) &&
			IsValidTrackdir(td) && CheckAircraftVirtualTile(v, tile, TrackdirToTrackdirBits(td)) == AircraftTaxiBlock::None;
}

bool IsAircraftVirtualDestinationAvailable(const Aircraft *v, TileIndex tile, TrackBits tracks)
{
	return CheckAircraftTaxiTile(v, tile, false, false, tracks) == AircraftTaxiBlock::None &&
			CheckIncomingAircraftEnvelopes(v, tile, false, tracks) == AircraftTaxiBlock::None;
}

static std::map<TileIndex, TrackdirBits> AircraftPlannedTrackdirs(const Aircraft *v)
{
	std::map<TileIndex, TrackdirBits> result;
	for (size_t i = 0; i < v->planned_path.size(); ++i) {
		const auto &node = v->planned_path[i];
		if (!IsValidTrackdir(node.td)) continue;
		result[node.tile] |= TrackdirToTrackdirBits(node.td);
		if (i + 1 == v->planned_path.size()) continue;
		const auto &turn = v->planned_path[i + 1];
		if (turn.tile != node.tile || turn.choice) continue;
		/* An edge turn temporarily continues into the adjacent tile before
		 * returning on the next local track. Book that swept direction too. */
		CFollowTrackAirport follower(v);
		if (follower.Follow(node.tile, node.td)) {
			result[follower.new_tile] |= follower.new_td_bits;
		}
	}
	return result;
}

static void ReleaseAircraftVirtualPath(Aircraft *v)
{
	for (const auto &record : v->virtual_path) {
		auto booking = _aircraft_virtual_bookings.find(record.tile);
		if (booking == _aircraft_virtual_bookings.end() || booking->second.count(v->index) == 0) continue;
		booking->second.erase(v->index);
		if (booking->second.empty()) _aircraft_virtual_bookings.erase(booking);
		if (_show_airport_tracks && IsTaxiTile(record.tile)) MarkTileDirtyByTile(record.tile);
	}
	v->virtual_path.clear();
}

bool TryReserveAircraftTaxiPath(Aircraft *v)
{
	if (!IsTaxiTile(v->tile) || v->planned_path.empty() || v->planned_path.front().tile != v->tile) return false;
	const auto desired = AircraftPlannedTrackdirs(v);
	for (size_t i = 0; i < v->planned_path.size(); ++i) {
		const auto &node = v->planned_path[i];
		if (!IsValidTrackdir(node.td) || !IsTaxiTile(node.tile) ||
				!IsAirportTileOfStation(node.tile, GetStationIndex(v->tile))) return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, node.tile);
		if (i == 0 || v->planned_path[i - 1].tile == node.tile) continue;
		const auto &previous = v->planned_path[i - 1];
		CFollowTrackAirport follower(v);
		if (!follower.Follow(previous.tile, previous.td) || follower.new_tile != node.tile ||
				(follower.new_td_bits & TrackdirToTrackdirBits(node.td)) == TRACKDIR_BIT_NONE) return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, node.tile);
	}
	/* Check every tile before publishing any booking. No partial prefix, and no
	 * second arbitration when the real window subsequently advances. */
	for (const auto &[tile, trackdirs] : desired) {
		AircraftTaxiBlock block = CheckAircraftVirtualTile(v, tile, trackdirs);
		if (block != AircraftTaxiBlock::None) {
			return RecordAircraftTaxiBlock(v, block, tile);
		}
	}
	const auto &destination = v->planned_path.back();
	/* A shared virtual route never grants permission to stop on a real occupant. */
	if (!RecordAircraftTaxiBlock(v, MergeAircraftBlock(CheckAircraftTaxiTile(v, destination.tile, false, false, TrackToTrackBits(TrackdirToTrack(destination.td))),
			CheckIncomingAircraftEnvelopes(v, destination.tile, false, TrackToTrackBits(TrackdirToTrack(destination.td)))), destination.tile)) return false;
	if (IsApron(destination.tile) && !IsAircraftTerminalAvailable(v, destination.tile)) return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, destination.tile);

	ReleaseAircraftVirtualPath(v);
	for (const auto &[tile, trackdirs] : desired) {
		v->virtual_path.push_back({tile, trackdirs});
		if (!v->ignore_airport_reservations) _aircraft_virtual_bookings[tile][v->index] = trackdirs;
		if (_show_airport_tracks) MarkTileDirtyByTile(tile);
	}
	return true;
}

void PruneAircraftVirtualReservations(Aircraft *v)
{
	/* Movement only removes passed steps; it never re-acquires or steals a
	 * direction. Retain a tile if it is visited again later in the route. */
	const auto desired = AircraftPlannedTrackdirs(v);
	for (auto it = v->virtual_path.begin(); it != v->virtual_path.end();) {
		auto remaining = desired.find(it->tile);
		TrackdirBits keep = remaining == desired.end() ? TRACKDIR_BIT_NONE : remaining->second;
		if (keep == it->trackdirs) { ++it; continue; }
		assert((keep & it->trackdirs) == keep);
		if (v->ignore_airport_reservations) {
			if (keep == TRACKDIR_BIT_NONE) { it = v->virtual_path.erase(it); }
			else { it->trackdirs = keep; ++it; }
			continue;
		}
		auto booking = _aircraft_virtual_bookings.find(it->tile);
		assert(booking != _aircraft_virtual_bookings.end() && booking->second.at(v->index) == it->trackdirs);
		if (_show_airport_tracks && IsTaxiTile(it->tile)) MarkTileDirtyByTile(it->tile);
		if (keep == TRACKDIR_BIT_NONE) {
			booking->second.erase(v->index);
			if (booking->second.empty()) _aircraft_virtual_bookings.erase(booking);
			it = v->virtual_path.erase(it);
		} else {
			it->trackdirs = booking->second[v->index] = keep;
			++it;
		}
	}
	PruneAircraftTaxiRunways(v);
}

TrackBits GetAircraftVirtualBookedTracks(TileIndex tile)
{
	TrackBits tracks = TRACK_BIT_NONE;
	auto booking = _aircraft_virtual_bookings.find(tile);
	if (booking != _aircraft_virtual_bookings.end()) {
		for (const auto &[id, trackdirs] : booking->second) tracks |= TrackdirBitsToTrackBits(trackdirs);
	}
	return tracks;
}

void UpdateAircraftTaxiTraffic(Aircraft *v, bool moved)
{
	if (moved) {
		v->taxi_jam_ticks = 0;
		v->taxi_block_reason = AircraftTaxiBlock::None;
		if (HasBit(v->flags, VAF_TAXI_PHASING)) {
			/* Phasing ends once the strict exclusive window is clean again. */
			bool clean = true;
			for (const auto &record : v->reserved_path) {
				if (CheckAircraftTaxiTracks(v, record.tile, record.tracks, false) != AircraftTaxiBlock::None) { clean = false; break; }
			}
			if (clean) {
				ClrBit(v->flags, VAF_TAXI_PHASING);
				 
			}
		}
		return;
	}
	const bool eligible = !v->vehstatus.Any({VehState::Stopped, VehState::Hidden, VehState::Crashed}) &&
			(v->state == AS_IDLE || v->state == AS_RUNNING) && !v->current_order.IsType(OT_LOADING) && !v->current_order.IsType(OT_WAITING) &&
			v->next_trackdir == INVALID_TRACKDIR;
	/* Both shareable traffic and hard protected reservations can be waited out:
	 * a sustained block of either kind earns the same temporary pass. Strict
	 * geometry and map consistency checks stay in force while phasing. */
	if (eligible && v->taxi_block_reason != AircraftTaxiBlock::None) {
		if (v->taxi_jam_ticks < UINT16_MAX) ++v->taxi_jam_ticks;
		if (v->taxi_jam_ticks > 1480 && !HasBit(v->flags, VAF_TAXI_PHASING)) {
			SetBit(v->flags, VAF_TAXI_PHASING);
			v->ClearWaitTime();
			 
		}
	} else {
		v->taxi_jam_ticks = 0;
	}
}


void ReleaseAircraftTaxiPath(Aircraft *v, bool keep_current, bool keep_request)
{
	for (size_t i = 0; i < v->reserved_path.size();) {
		const auto r = v->reserved_path[i];
		if (keep_current && r.tile == v->tile) { ++i; continue; }
		ReleaseAircraftTile(v, r.tile, r.tracks);
	}
	ReleaseAircraftVirtualPath(v);
	v->planned_path.clear();
	v->path.clear();
	ClrBit(v->flags, VAF_PUSHBACK);
	v->pushback_origin = v->pushback_target = INVALID_TILE;
	v->taxi_blocked_ticks = 0;
	if (!keep_current || v->allocated_terminal != v->tile) ReleaseAircraftTerminal(v);
	if (!keep_current) {
		v->taxi_jam_ticks = 0;
		v->taxi_block_reason = AircraftTaxiBlock::None;
		ClrBit(v->flags, VAF_TAXI_PHASING);
	} else if (v->state != AS_RUNNING && v->next_trackdir == INVALID_TRACKDIR &&
			(v->x_pos & 15) == 8 && (v->y_pos & 15) == 8 && IsValidTrackdir(v->trackdir) && IsDiagonalTrackdir(v->trackdir)) {
		/* Failed departure attempts must not leave their extra origin tracks
		 * behind. At rest only the actually occupied centre track remains. */
		TrackBits keep = TrackToTrackBits(TrackdirToTrack(v->trackdir));
		ReleaseAircraftTile(v, v->tile, TRACK_BIT_ALL & ~keep);
	}
	if (keep_request && v->junction_request_tick == INVALID_STATE_TICKS && !v->junction_regions.empty()) {
		v->junction_requests = v->junction_actions;
		v->junction_request_tick = _state_ticks;
		SyncAircraftJunctionRecords(v);
	}
	if (v->junction_legacy) {
		RefreshAircraftLegacyJunctionFootprint(v);
		if (!keep_request && v->reserved_runway == INVALID_TILE && v->planned_path.empty() &&
				(!keep_current || v->state != AS_RUNNING || v->Next()->state != AS_START_TAKEOFF) &&
				(v->next_trackdir == INVALID_TRACKDIR || !keep_current) && !HasBit(v->flags, VAF_PUSHBACK)) FinishAircraftLegacyJunctionAction(v);
	}
	PruneAircraftJunctions(v, keep_current);
	if (!keep_request) CancelAircraftJunctionRequest(v);
	if (keep_current) PruneAircraftTaxiRunways(v);
	else ReleaseAircraftTaxiRunways(v);
}

void LiftAirportPathReservation(Aircraft *v, bool skip_first_track)
{
	ReleaseAircraftTaxiPath(v, skip_first_track);
}

bool ExtendAircraftTaxiReservation(Aircraft *v)
{
	if (v->planned_path.empty()) return false;
	TileIndex tile = v->planned_path.front().tile;
	if (tile != v->tile) return false;
	TrackBits current = TRACK_BIT_NONE;
	TrackdirBits current_tds = TRACKDIR_BIT_NONE;
	size_t i = 0;
	while (i < v->planned_path.size() && v->planned_path[i].tile == tile) {
		current |= TrackToTrackBits(TrackdirToTrack(v->planned_path[i].td));
		current_tds |= TrackdirToTrackdirBits(v->planned_path[i].td);
		++i;
	}
	TileIndex next = INVALID_TILE;
	TrackBits ahead = TRACK_BIT_NONE;
	TrackdirBits ahead_tds = TRACKDIR_BIT_NONE;
	if (i != v->planned_path.size()) {
		next = v->planned_path[i].tile;
		while (i < v->planned_path.size() && v->planned_path[i].tile == next) {
			ahead |= TrackToTrackBits(TrackdirToTrack(v->planned_path[i].td));
			ahead_tds |= TrackdirToTrackdirBits(v->planned_path[i].td);
			++i;
		}
	}
	v->taxi_block_reason = AircraftTaxiBlock::None;
	const AircraftJunctionState previous_junctions(v);
	const auto previous_runways = v->taxi_runways;
	if (!TryReserveAircraftJunctions(v, next, ahead)) return false;
	TrackBits previous = TRACK_BIT_NONE;
	for (const auto &record : v->reserved_path) { if (record.tile == tile) previous |= record.tracks; }
	if (!TryReserveAircraftTile(v, tile, current, current_tds)) {
		RestoreAircraftJunctions(v, previous_junctions);
		RestoreAircraftTaxiRunways(v, previous_runways);
		return false;
	}
	if (next == INVALID_TILE || TryReserveAircraftTile(v, next, ahead, ahead_tds)) return true;
	TrackBits added = current & ~previous;
	if (added != TRACK_BIT_NONE) {
		ReleaseAircraftTile(v, tile, added);
	}
	RestoreAircraftJunctions(v, previous_junctions);
	RestoreAircraftTaxiRunways(v, previous_runways);
	return false;
}

static TileIndex AircraftRunwayStep(TileIndex tile, DiagDirection dir)
{
	const auto delta = TileIndexDiffCByDiagDir(dir);
	return TileAddWrap(tile, delta.x, delta.y);
}

static std::vector<TileIndex> RunwayTiles(TileIndex extreme)
{
	std::vector<TileIndex> result;
	if (!IsTaxiTile(extreme) || !IsRunwayExtreme(extreme)) return result;
	const StationID station = GetStationIndex(extreme);
	const Track axis = DiagDirToDiagTrack(GetRunwayExtremeDirection(extreme));
	DiagDirection dir = GetRunwayExtremeDirection(extreme);
	if (IsRunwayEnd(extreme)) dir = ReverseDiagDir(dir);
	for (TileIndex tile = extreme; IsTaxiTile(tile) && IsAirportTileOfStation(tile, station) && IsRunway(tile) && HasTrack(GetRunwayTracks(tile), axis);
			tile = AircraftRunwayStep(tile, dir)) {
		result.push_back(tile);
		if (tile != extreme && IsRunwayExtreme(tile)) {
			if (DiagDirToDiagTrack(GetRunwayExtremeDirection(tile)) != axis || IsRunwayEnd(tile) == IsRunwayEnd(extreme)) return {};
			return result;
		}
	}
	return {};
}

uint GetAircraftRunwayLength(TileIndex extreme)
{
	return static_cast<uint>(RunwayTiles(extreme).size());
}

uint GetStationMaxLandingRunwayLength(const Station *st)
{
	if (st == nullptr) return 0;
	uint best = 0;
	for (TileIndex extreme : st->airport.runways) {
		if (!IsTaxiTile(extreme) || !IsRunwayStart(extreme) || !IsLandingTypeTile(extreme)) continue;
		uint length = GetAircraftRunwayLength(extreme);
		if (length > best) best = length;
	}
	return best;
}

/* Resolve every runway crossing this tile without using the unbounded legacy
 * endpoint walk. A crossing tile belongs to both runway axes. */
static bool CollectAircraftRunways(TileIndex tile, std::set<TileIndex> &runways)
{
	if (!IsTaxiTile(tile)) return false;
	if (!IsRunway(tile)) return true;
	const StationID station = GetStationIndex(tile);
	for (Track axis : SetTrackBitIterator(GetRunwayTracks(tile))) {
		const DiagDirection dir = TrackdirToExitdir(TrackToTrackdir(axis));
		TileIndex endpoint = tile;
		while (IsTaxiTile(endpoint) && IsAirportTileOfStation(endpoint, station) && IsRunway(endpoint) && HasTrack(GetRunwayTracks(endpoint), axis)) {
			if (IsRunwayExtreme(endpoint)) break;
			endpoint = AircraftRunwayStep(endpoint, dir);
		}
		const auto tiles = RunwayTiles(endpoint);
		if (tiles.empty() || std::find(tiles.begin(), tiles.end(), tile) == tiles.end()) return false;
		runways.insert(IsRunwayStart(endpoint) ? endpoint : tiles.back());
	}
	return true;
}

static bool CollectAircraftTaxiRunways(const Aircraft *v, std::set<TileIndex> &runways)
{
	/* No whole-runway claim exists during taxiing any more: not from the
	 * real window, not from the physical tile, not from a junction. Keeping
	 * this empty drains every taxi_runways entry on the next prune, so an
	 * aircraft that has left the runway stops holding its whole axis. */
	(void)v;
	(void)runways;
	return true;
}

static std::set<TileIndex> AircraftLockedRunwayTiles(const Aircraft *v)
{
	std::set<TileIndex> tiles;
	for (TileIndex extreme : v->taxi_runways) {
		for (TileIndex tile : RunwayTiles(extreme)) tiles.insert(tile);
	}
	for (TileIndex tile : RunwayTiles(v->reserved_runway)) tiles.insert(tile);
	return tiles;
}

static void ReleaseAircraftRunwayTiles(const Aircraft *v, const std::set<TileIndex> &tiles)
{
	if (v->ignore_airport_reservations) return;
	const auto retained = AircraftLockedRunwayTiles(v);
	for (TileIndex tile : tiles) {
		if (retained.count(tile) != 0) continue;
		auto owner = _aircraft_runway_owners.find(tile);
		if (owner == _aircraft_runway_owners.end() || owner->second != v->index) continue;
		if (IsTaxiTile(tile) && IsRunway(tile)) {
			SetReservationAsRunway(tile, false);
			if (_show_airport_tracks) MarkTileDirtyByTile(tile);
		}
		_aircraft_runway_owners.erase(owner);
	}
}

static void PruneAircraftTaxiRunways(Aircraft *v, bool keep_physical)
{
	if (v->taxi_runways.empty()) return;
	/* The old edge track has already been pruned from the route while the
	 * sprite is still turning through its swept space. Keep these locks until
	 * rotation completes, including across save/load. */
	if (v->state == AS_RUNNING && v->next_trackdir != INVALID_TRACKDIR) return;
	std::set<TileIndex> required;
	(void)keep_physical;
	CollectAircraftTaxiRunways(v, required);
	const auto previous = AircraftLockedRunwayTiles(v);
	std::erase_if(v->taxi_runways, [&](TileIndex extreme) { return required.count(extreme) == 0; });
	ReleaseAircraftRunwayTiles(v, previous);
}

static void ReleaseAircraftTaxiRunways(Aircraft *v)
{
	const auto previous = AircraftLockedRunwayTiles(v);
	v->taxi_runways = v->junction_runways;
	ReleaseAircraftRunwayTiles(v, previous);
}

/* Construction must also respect local records of ignored aircraft. These
 * records do not block other aircraft, but still depend on the map geometry. */
TrackBits GetAircraftAirportEditTracks(TileIndex tile)
{
	if (!IsTaxiTile(tile)) return TRACK_BIT_NONE;
	for (const Aircraft *v : Aircraft::Iterate()) {
		if (!v->IsNormalAircraft()) continue;
		if (std::binary_search(v->junction_tiles.begin(), v->junction_tiles.end(), tile) ||
				std::binary_search(v->junction_legacy_tiles.begin(), v->junction_legacy_tiles.end(), tile)) return TRACK_BIT_ALL;
	}
	TrackBits tracks = GetReservedAirportTracks(tile) | GetAircraftVirtualBookedTracks(tile);
	if (_aircraft_terminal_owners.count(tile) != 0 || _aircraft_runway_owners.count(tile) != 0 ||
			(IsRunway(tile) && GetReservationAsRunway(tile))) return TRACK_BIT_ALL;
	for (const Aircraft *v : Aircraft::Iterate()) {
		if (!v->IsNormalAircraft()) continue;
		if (v->allocated_terminal == tile) return TRACK_BIT_ALL;
		if (v->tile == tile && !v->vehstatus.Test(VehState::Hidden) && !v->IsAircraftFlying()) return TRACK_BIT_ALL;
		if (!v->ignore_airport_reservations) continue;
		for (const auto &record : v->reserved_path) { if (record.tile == tile) tracks |= record.tracks; }
		for (const auto &record : v->virtual_path) { if (record.tile == tile) tracks |= TrackdirBitsToTrackBits(record.trackdirs); }
		for (const auto &node : v->planned_path) { if (node.tile == tile && IsValidTrackdir(node.td)) tracks |= TrackToTrackBits(TrackdirToTrack(node.td)); }
		if (AircraftLockedRunwayTiles(v).count(tile) != 0) return TRACK_BIT_ALL;
	}
	return tracks;
}

/**
 * Check whether adding a track changes an aircraft's admitted junction footprint
 * or the swept directions of a planned edge turn, including distant turns and
 * ignored aircraft. Ordinary tile occupancy does not prevent track additions.
 * @param tile Tile to change.
 * @param track Track to add.
 * @return True iff the addition conflicts with existing movement geometry.
 */
bool HasAircraftAirportTrackAdditionConflict(TileIndex tile, Track track)
{
	if (!IsTaxiTile(tile)) return false;
	const TrackdirBits added = TrackBitsToTrackdirBits(TrackToTrackBits(track));
	for (const Aircraft *v : Aircraft::Iterate()) {
		if (!v->IsNormalAircraft()) continue;
		if (std::binary_search(v->junction_tiles.begin(), v->junction_tiles.end(), tile) ||
				std::binary_search(v->junction_legacy_tiles.begin(), v->junction_legacy_tiles.end(), tile)) return true;
		for (size_t i = 0; i + 1 < v->planned_path.size(); ++i) {
			const auto &node = v->planned_path[i];
			const auto &turn = v->planned_path[i + 1];
			if (turn.tile != node.tile || turn.choice || !IsValidTrackdir(node.td)) continue;
			CFollowTrackAirport follower(v);
			if (follower.Follow(node.tile, node.td) && follower.new_tile == tile &&
					(added & DiagdirReachesTrackdirs(follower.exitdir) & ~follower.new_td_bits) != TRACKDIR_BIT_NONE) return true;
		}
	}
	return false;
}

static AircraftTaxiBlock CheckAircraftRunwayTile(const Aircraft *v, TileIndex tile)
{
	/* The whole runway tile remains exclusive, but only its actual runway
	 * tracks interact with half-track envelopes on neighbouring taxi tiles.
	 * Do NOT consider other aircraft's junction actions here: the runway is
	 * protected by its own whole-axis lock and by real tile owners. A ground
	 * aircraft holding the entrance junction while waiting for the runway
	 * head would otherwise block the takeoff it cannot perform either — an
	 * ABBA deadlock. TryReserveAircraftJunctions handles this aircraft's own
	 * runway-head action separately. */
	return MergeAircraftBlock(CheckAircraftTaxiTile(v, tile, false, true, TRACK_BIT_ALL, false),
		CheckIncomingAircraftEnvelopes(v, tile, false, GetRunwayTracks(tile)));
}

static bool AircraftJunctionResting(const Aircraft *v)
{
	return v->vehstatus.Test(VehState::Hidden) || (v->planned_path.empty() &&
			!v->IsAircraftFlying() && IsTaxiTile(v->tile) && (IsApron(v->tile) || IsHangar(v->tile)) &&
			v->next_trackdir == INVALID_TRACKDIR);
}

static void RefreshAircraftLegacyJunctionFootprint(Aircraft *v)
{
	if (!v->junction_legacy) return;
	std::set<TileIndex> tiles;
	auto collect = [&](TileIndex tile, TrackBits tracks) {
		if (!IsTaxiTile(tile)) return;
		tiles.insert(tile);
		for (Track track : SetTrackBitIterator(tracks)) {
			if (IsDiagonalTrack(track)) continue;
			TileIndex swept = AircraftJunctionStep(tile, _aircraft_envelope_directions[track]);
			if (IsTaxiTile(swept) && IsAirportTileOfStation(swept, GetStationIndex(tile))) tiles.insert(swept);
		}
	};
	for (const auto &r : v->reserved_path) collect(r.tile, r.tracks);
	for (const auto &r : v->virtual_path) collect(r.tile, TrackdirBitsToTrackBits(r.trackdirs));
	for (const auto &node : v->planned_path) {
		if (IsValidTrackdir(node.td)) collect(node.tile, TrackToTrackBits(TrackdirToTrack(node.td)));
	}
	if (!v->IsAircraftFreelyFlying() && !v->vehstatus.Test(VehState::Hidden)) {
		collect(v->tile, IsValidTrackdir(v->trackdir) ? TrackToTrackBits(TrackdirToTrack(v->trackdir)) : TRACK_BIT_NONE);
		if (IsValidTrackdir(v->next_trackdir)) collect(v->tile, TrackToTrackBits(TrackdirToTrack(v->next_trackdir)));
	}
	for (TileIndex tile : RunwayTiles(v->reserved_runway)) collect(tile, TRACK_BIT_NONE);
	for (TileIndex extreme : v->taxi_runways) {
		for (TileIndex tile : RunwayTiles(extreme)) collect(tile, TRACK_BIT_NONE);
	}
	v->junction_legacy_tiles.assign(tiles.begin(), tiles.end());
	_aircraft_junction_legacy.insert(v->index);
}

static void FinishAircraftLegacyJunctionAction(Aircraft *v)
{
	if (!v->junction_legacy) return;
	v->junction_legacy = false;
	v->junction_legacy_tiles.clear();
	_aircraft_junction_legacy.erase(v->index);
}

static void SyncAircraftJunctionRecords(Aircraft *v)
{
	std::set<TileIndex> regions, tiles, requests, request_tiles;
	v->junction_entered = false;
	for (const auto &action : v->junction_actions) {
		regions.insert(action.junction);
		tiles.insert(action.tiles.begin(), action.tiles.end());
		v->junction_entered |= action.entered;
	}
	for (const auto &action : v->junction_requests) {
		requests.insert(action.junction);
		request_tiles.insert(action.tiles.begin(), action.tiles.end());
	}
	v->junction_regions.assign(regions.begin(), regions.end());
	v->junction_tiles.assign(tiles.begin(), tiles.end());
	v->junction_request_regions.assign(requests.begin(), requests.end());
	v->junction_request_tiles.assign(request_tiles.begin(), request_tiles.end());
	v->junction_runways.clear();
	if (!v->ignore_airport_reservations && !v->junction_actions.empty()) _aircraft_junction_owners.insert(v->index);
	else _aircraft_junction_owners.erase(v->index);
	if (!v->ignore_airport_reservations && !v->junction_requests.empty()) _aircraft_junction_requests[v->index] = v;
	else _aircraft_junction_requests.erase(v->index);
}

static bool AircraftJunctionTracksConflict(TrackBits a, TrackBits b)
{
	for (Track track : SetTrackBitIterator(a)) {
		if (TrackOverlapsTracks(b, track)) return true;
	}
	return false;
}

static void AddAircraftJunctionFootprint(AircraftJunctionAction &action, TileIndex tile, TrackBits tracks, TrackBits swept = TRACK_BIT_NONE)
{
	auto it = std::lower_bound(action.tiles.begin(), action.tiles.end(), tile);
	const size_t i = it - action.tiles.begin();
	if (it == action.tiles.end() || *it != tile) {
		action.tiles.insert(it, tile);
		action.tracks.insert(action.tracks.begin() + i, tracks);
		action.swept.insert(action.swept.begin() + i, swept);
	} else {
		action.tracks[i] |= tracks;
		action.swept[i] |= swept;
	}
}

static void AddAircraftJunctionTrack(AircraftJunctionAction &action, TileIndex tile, TrackBits tracks)
{
	AddAircraftJunctionFootprint(action, tile, tracks);
	for (Track track : SetTrackBitIterator(tracks)) {
		if (IsDiagonalTrack(track)) continue;
		TileIndex neighbour = AircraftJunctionStep(tile, _aircraft_envelope_directions[track]);
		if (IsTaxiTile(neighbour) && IsAirportTileOfStation(neighbour, GetStationIndex(tile))) {
			AddAircraftJunctionFootprint(action, neighbour, TRACK_BIT_NONE, TrackToTrackBits(TrackToOppositeTrack(track)));
		}
	}
}

static bool AircraftJunctionActionsConflict(const AircraftJunctionAction &a, const AircraftJunctionAction &b)
{
	for (size_t i = 0, j = 0; i < a.tiles.size() && j < b.tiles.size();) {
		if (a.tiles[i] < b.tiles[j]) { ++i; continue; }
		if (b.tiles[j] < a.tiles[i]) { ++j; continue; }
		if (AircraftJunctionTracksConflict(a.tracks[i], b.tracks[j]) ||
				(a.swept[i] & (b.tracks[j] | b.swept[j])) != TRACK_BIT_NONE ||
				(b.swept[j] & a.tracks[i]) != TRACK_BIT_NONE) return true;
		++i; ++j;
	}
	return false;
}

static AircraftJunctionAction AircraftJunctionPhysicalFootprint(const Aircraft *v)
{
	AircraftJunctionAction physical;
	if (v->vehstatus.Test(VehState::Hidden) || v->IsAircraftFlying() || !IsTaxiTile(v->tile)) return physical;
	TrackBits tracks = TRACK_BIT_NONE;
	if (IsValidTrackdir(v->trackdir)) tracks |= TrackToTrackBits(TrackdirToTrack(v->trackdir));
	if (IsValidTrackdir(v->next_trackdir)) tracks |= TrackToTrackBits(TrackdirToTrack(v->next_trackdir));
	/* The forward real tile and future turn tracks are not physical occupancy. */
	if (tracks != TRACK_BIT_NONE) AddAircraftJunctionTrack(physical, v->tile, tracks);
	return physical;
}

static bool CheckAircraftJunctionAccess(const Aircraft *v, TileIndex tile, TrackBits tracks)
{
	if (v->ignore_airport_reservations) return true;
	if (!v->junction_legacy) {
		for (VehicleID id : _aircraft_junction_legacy) {
			const Aircraft *other = Aircraft::GetIfValid(id);
			if (other != nullptr && other != v && !other->ignore_airport_reservations &&
					std::binary_search(other->junction_legacy_tiles.begin(), other->junction_legacy_tiles.end(), tile)) {
				 
				return false;
			}
		}
	}
	for (VehicleID id : _aircraft_junction_owners) {
		const Aircraft *owner = Aircraft::GetIfValid(id);
		if (owner == nullptr || owner == v) continue;
		for (const auto &action : owner->junction_actions) {
			auto it = std::lower_bound(action.tiles.begin(), action.tiles.end(), tile);
			if (it == action.tiles.end() || *it != tile) continue;
			const size_t i = it - action.tiles.begin();
			if (AircraftJunctionTracksConflict(tracks, action.tracks[i]) || (tracks & action.swept[i]) != TRACK_BIT_NONE) {
				return false;
			}
		}
	}
	return true;
}

static void CancelAircraftJunctionRequest(Aircraft *v)
{
	v->junction_requests.clear();
	v->junction_request_tick = INVALID_STATE_TICKS;
	SyncAircraftJunctionRecords(v);
}

static void ReleaseAircraftJunctions(Aircraft *v)
{
	v->junction_actions.clear();
	SyncAircraftJunctionRecords(v);
	PruneAircraftTaxiRunways(v);
}

static void PruneAircraftJunctions(Aircraft *v, bool keep_current)
{
	if (v->junction_actions.empty()) return;
	if (v->vehstatus.Test(VehState::Crashed) && keep_current) return;
	if ((!keep_current && v->reserved_runway == INVALID_TILE) ||
			(v->IsAircraftFlying() && v->reserved_runway == INVALID_TILE && v->reserved_path.empty()) ||
			(AircraftJunctionResting(v) && v->reserved_runway == INVALID_TILE)) {
		ReleaseAircraftJunctions(v);
		return;
	}
	/* A body that has driven into the crossing but cannot advance for a very
	 * long time is parked on it (e.g. behind an opposing virtual route).
	 * Route pruning alone can never clear its action: the occupied window is
	 * the aircraft's own tile, so the grant is kept forever and every follower
	 * deadlocks behind it. Drop the stale action once the jam saturates. The
	 * physical single-tile reservations remain, so no collision is possible. */
	if (v->taxi_jam_ticks >= 8192 && v->next_trackdir == INVALID_TRACKDIR && v->planned_path.empty() == false) {
		ReleaseAircraftJunctions(v);
		return;
	}
	/* Route pruning precedes completion of the sprite's turn. */
	if (v->next_trackdir != INVALID_TRACKDIR || HasBit(v->flags, VAF_PUSHBACK)) return;
	for (auto it = v->junction_actions.begin(); it != v->junction_actions.end();) {
		bool occupied = false;
		for (const auto &record : v->reserved_path) {
			/* The exit tile is part of the action footprint only to protect
			 * the swept turn, not to keep the crossing claimed after the
			 * aircraft has driven out of it. Once the body has actually
			 * entered this action, a window that now holds only the exit tile
			 * means the aircraft is parked beyond the crossing (e.g. waiting
			 * at the runway head for takeoff). Holding the action for that
			 * strands every other aircraft waiting at the junction. */
			if (it->entered && record.tile == it->exit_tile) continue;
			AircraftJunctionAction physical;
			AddAircraftJunctionTrack(physical, record.tile, record.tracks);
			if (!AircraftJunctionActionsConflict(*it, physical)) continue;
			occupied = true;
			if (record.tile == v->tile) it->entered = true;
		}
		if (!occupied && (it->entered || v->planned_path.empty())) {
			it = v->junction_actions.erase(it);
		} else ++it;
	}
	SyncAircraftJunctionRecords(v);
}

static void RestoreAircraftJunctions(Aircraft *v, const AircraftJunctionState &saved)
{
	if (v->junction_actions == saved.actions) return;
	v->junction_actions = saved.actions;
	SyncAircraftJunctionRecords(v);
}

void ReleaseAircraftStalledReservations(Aircraft *v)
{
	/* A failed early-exit probe is not a stalled taxi route. The landing
	 * controller owns the roll's runway lock until a safe handoff succeeds. */
	if (v->state == AS_LANDED || v->IsAircraftFlying() || v->vehstatus.Test(VehState::Hidden)) return;
	if (!IsTaxiTile(v->tile) || !IsRunway(v->tile)) return;
	/* An idle aircraft can legitimately keep the one physical tile it
	 * occupies. Anything beyond that is a stranded claim; the old strict
	 * "no reservations at all" test never fired for those aircraft. */
	if (!v->planned_path.empty()) return;
	if (!v->reserved_path.empty() &&
			(v->reserved_path.size() != 1 || v->reserved_path.front().tile != v->tile)) return;

	/* With no admitted window there is nothing to use the junction grant for.
	 * Keeping it only blocks the same crossing for every other aircraft. */
	if (!v->junction_regions.empty()) {
		ReleaseAircraftJunctions(v);
		CancelAircraftJunctionRequest(v);
	}

	/* A stopped aircraft physically occupies one runway axis at this tile.
	 * Locks on any other axis cannot help it and only strand opposite traffic.
	 * Old saves may name the same runway by its end rather than its start. */
	std::set<TileIndex> physical;
	if (!CollectAircraftRunways(v->tile, physical)) return;
	auto is_physical = [&](TileIndex extreme) {
		const auto tiles = RunwayTiles(extreme);
		return !tiles.empty() && physical.count(IsRunwayStart(extreme) ? tiles.front() : tiles.back()) != 0;
	};
	const auto locked = AircraftLockedRunwayTiles(v);
	std::erase_if(v->taxi_runways, [&](TileIndex extreme) { return !is_physical(extreme); });
	if (v->reserved_runway != INVALID_TILE && !is_physical(v->reserved_runway)) v->reserved_runway = INVALID_TILE;
	ReleaseAircraftRunwayTiles(v, locked);
}

static bool SameAircraftJunctionTraversal(const AircraftJunctionAction &a, const AircraftJunctionAction &b)
{
	if (a.junction != b.junction || a.exit_tile != b.exit_tile || a.exit_td != b.exit_td) return false;
	/* Passed entry nodes disappear from planned_path. The saved grant remains
	 * authoritative when the current candidate is only its remaining suffix. */
	for (size_t i = 0; i < b.tiles.size(); ++i) {
		auto it = std::lower_bound(a.tiles.begin(), a.tiles.end(), b.tiles[i]);
		if (it == a.tiles.end() || *it != b.tiles[i]) return false;
		const size_t j = it - a.tiles.begin();
		if ((a.tracks[j] & b.tracks[i]) != b.tracks[i] || (a.swept[j] & b.swept[i]) != b.swept[i]) return false;
	}
	return true;
}

static bool BuildAircraftJunctionAction(const Aircraft *v, TileIndex junction, AircraftJunctionAction &action)
{
	/* Only consider a junction encountered by the immediate route window.
	 * Membership in a neighbouring junction's approach is not a traversal. */
	const auto &route = v->planned_path;
	size_t crossing = route.size();
	uint steps = 0;
	for (size_t i = 0; i < route.size(); ++i) {
		if (i != 0 && route[i].tile != route[i - 1].tile && ++steps > 1) break;
		if (route[i].tile == junction) { crossing = i; break; }
		if (!IsValidTrackdir(route[i].td)) return false;
		if (i + 1 < route.size() && route[i + 1].tile == route[i].tile && !route[i + 1].choice) {
			CFollowTrackAirport follower(v);
			if (follower.Follow(route[i].tile, route[i].td) && follower.new_tile == junction) { crossing = i; break; }
		}
		if (!IsDiagonalTrackdir(route[i].td) &&
				AircraftJunctionStep(route[i].tile, _aircraft_envelope_directions[TrackdirToTrack(route[i].td)]) == junction) {
			crossing = i;
			break;
		}
	}
	if (crossing == route.size()) return false;
	const TileIndex crossing_tile = route[crossing].tile;
	size_t begin = crossing;
	while (begin > 0 && route[begin - 1].tile == crossing_tile) --begin;
	if (begin > 0) {
		const TileIndex entry = route[begin - 1].tile;
		do { --begin; } while (begin > 0 && route[begin - 1].tile == entry);
	}
	size_t end = crossing + 1;
	while (end < route.size() && route[end].tile == crossing_tile) ++end;
	if (end < route.size()) {
		const TileIndex exit = route[end].tile;
		do { ++end; } while (end < route.size() && route[end].tile == exit);
	}
	action.junction = junction;
	action.entry_tile = route[begin].tile;
	action.entry_td = route[begin].td;
	action.exit_tile = route[end - 1].tile;
	action.exit_td = route[end - 1].td;
	for (size_t i = begin; i < end; ++i) {
		const auto &node = route[i];
		if (!IsTaxiTile(node.tile) || !IsValidTrackdir(node.td)) return false;
		AddAircraftJunctionTrack(action, node.tile, TrackToTrackBits(TrackdirToTrack(node.td)));
		if (i + 1 < end && route[i + 1].tile == node.tile && !route[i + 1].choice) {
			CFollowTrackAirport follower(v);
			if (!follower.Follow(node.tile, node.td)) return false;
			/* An edge turn visits this space without changing the physical tile. */
			AddAircraftJunctionFootprint(action, follower.new_tile, TRACK_BIT_NONE, TrackdirBitsToTrackBits(follower.new_td_bits));
		}
	}
	return true;
}

static bool CheckAircraftJunctionActionPhysical(const Aircraft *v, const AircraftJunctionAction &action)
{
	/* Validate geometry, but do not let a manual ignore request rewrite public
	 * ownership or wait behind a physical aircraft. */
	if (v->ignore_airport_reservations) return std::all_of(action.tiles.begin(), action.tiles.end(), [](TileIndex tile) { return IsTaxiTile(tile); });
	for (size_t i = 0; i < action.tiles.size(); ++i) {
		const TileIndex tile = action.tiles[i];
		if (!IsTaxiTile(tile)) return false;
		/* Body tracks overlap geometrically; a swept half only conflicts with
		 * its facing half. Diagonal tracks on the neighbour are independent. */
		if (action.tracks[i] != TRACK_BIT_NONE &&
				CheckAircraftTaxiTracks(v, tile, action.tracks[i], false) != AircraftTaxiBlock::None) {
			return false;
		}
		if (action.swept[i] == TRACK_BIT_NONE || v->ignore_airport_reservations) continue;
		if (GetReservedAirportTracks(tile) != AircraftReservedTracks(tile)) return false;
		auto owners = _aircraft_tile_owners.find(tile);
		if (owners != _aircraft_tile_owners.end()) {
			for (const auto &[id, tracks] : owners->second) {
				const Aircraft *other = Aircraft::GetIfValid(id);
				if (id != v->index && (other == nullptr || !other->ignore_airport_reservations) &&
						(tracks & action.swept[i]) != TRACK_BIT_NONE) {
					 
					return false;
				}
			}
		}
		for (const Aircraft *other : VehiclesOnTile<VehicleType::Aircraft>(tile)) {
			if (other == v || !other->IsNormalAircraft() || other->ignore_airport_reservations ||
					other->vehstatus.Test(VehState::Hidden) || other->IsAircraftFlying()) continue;
			TrackBits tracks = TRACK_BIT_NONE;
			if (IsValidTrackdir(other->trackdir)) tracks |= TrackToTrackBits(TrackdirToTrack(other->trackdir));
			if (IsValidTrackdir(other->next_trackdir)) tracks |= TrackToTrackBits(TrackdirToTrack(other->next_trackdir));
			if ((tracks & action.swept[i]) != TRACK_BIT_NONE) return false;
		}
	}
	return true;
}

static bool TryReserveAircraftJunctions(Aircraft *v, TileIndex tile, TrackBits tracks, TileIndex runway)
{
	if (_aircraft_junction_loading || v->junction_legacy) return true;
	const TileIndex origin = runway != INVALID_TILE ? runway : (IsTaxiTile(tile) ? tile : v->tile);
	if (!IsTaxiTile(origin)) return true;
	const auto &geometry = GetAircraftJunctionGeometry(GetStationIndex(origin));
	std::set<TileIndex> near;
	auto collect = [&](TileIndex current, TrackBits bits) {
		auto found = geometry.tiles.find(current);
		if (found != geometry.tiles.end()) near.insert(found->second.begin(), found->second.end());
		for (Track track : SetTrackBitIterator(bits)) {
			if (IsDiagonalTrack(track)) continue;
			found = geometry.tiles.find(AircraftJunctionStep(current, _aircraft_envelope_directions[track]));
			if (found != geometry.tiles.end()) near.insert(found->second.begin(), found->second.end());
		}
	};
	if (runway != INVALID_TILE) {
		/* The runway has its own physical lock. Only its local entry is a
		 * junction action, never its full length or its distant exits. */
		collect(runway, TRACK_BIT_NONE);
	} else {
		if (tile == INVALID_TILE && AircraftJunctionResting(v)) return true;
		if (IsTaxiTile(v->tile) && !v->IsAircraftFlying()) {
			collect(v->tile, IsValidTrackdir(v->trackdir) ? TrackToTrackBits(TrackdirToTrack(v->trackdir)) : TRACK_BIT_NONE);
		}
		for (const auto &r : v->reserved_path) collect(r.tile, r.tracks);
		if (tile != INVALID_TILE) collect(tile, tracks);
	}
	std::vector<AircraftJunctionAction> desired;
	for (TileIndex key : near) {
		AircraftJunctionAction action;
		if (runway != INVALID_TILE) {
			/* A remote approach tile beside a runway is not being crossed. */
			if (key != runway) continue;
			action.junction = key;
			action.entry_tile = action.exit_tile = runway;
			action.entry_td = action.exit_td = DiagDirToDiagTrackdir(GetRunwayExtremeDirection(runway));
			AddAircraftJunctionTrack(action, runway, GetRunwayTracks(runway));
		} else if (!BuildAircraftJunctionAction(v, key, action)) {
			if (!v->planned_path.empty() || tile != key || tracks == TRACK_BIT_NONE) continue;
			action.junction = key;
			action.entry_tile = action.exit_tile = tile;
			AddAircraftJunctionTrack(action, tile, tracks);
			TrackBits first = tracks;
			action.entry_td = action.exit_td = TrackToTrackdir(RemoveFirstTrack(&first));
		}
		const bool held = std::any_of(v->junction_actions.begin(), v->junction_actions.end(), [&](const auto &grant) {
			return SameAircraftJunctionTraversal(grant, action);
		});
		if (!held) {
			desired.push_back(std::move(action));
		}
	}
	if (desired.empty()) {
		CancelAircraftJunctionRequest(v);
		return true;
	}
	if (desired.size() > 64 || v->junction_actions.size() + desired.size() > 64) return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, origin);
	/* Replanning the same local action keeps its queue position. An unrelated
	 * crossing starts a new request; there is no remote route-wide priority. */
	bool same_request = false;
	for (const auto &pending : v->junction_requests) {
		for (const auto &action : desired) same_request |= pending.junction == action.junction;
	}
	if (!same_request || v->junction_request_tick == INVALID_STATE_TICKS) v->junction_request_tick = _state_ticks;
	v->junction_requests = desired;
	SyncAircraftJunctionRecords(v);
	if (!v->ignore_airport_reservations) {
		const AircraftJunctionAction physical = AircraftJunctionPhysicalFootprint(v);
		for (VehicleID id : _aircraft_junction_owners) {
			const Aircraft *other = Aircraft::GetIfValid(id);
			if (other == nullptr || other == v) continue;
			for (const auto &action : desired) {
				for (const auto &grant : other->junction_actions) {
					if (!AircraftJunctionActionsConflict(action, grant)) continue;
					return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, origin);
				}
			}
		}
		for (const auto &[id, other] : _aircraft_junction_requests) {
			if (other == v || other->ignore_airport_reservations || other->IsAircraftFlying() ||
					other->vehstatus.Any({VehState::Stopped, VehState::Crashed}) ||
					std::pair(other->junction_request_tick, id) >= std::pair(v->junction_request_tick, v->index)) continue;
			/* FIFO decides who may ENTER a crossing, not whether an aircraft
			 * that already holds an action may continue its traversal into the
			 * next one. A traversal legitimately spans several junctions, so
			 * comparing grant.junction == action.junction is too narrow: an
			 * owner holding junction B while requesting the next junction A
			 * gets held by FIFO while the waiter at A is held by the owner's
			 * existing grant — an ABBA deadlock. Owners finish; they release
			 * the action as soon as their window clears the conflict zone. */
			if (!v->junction_actions.empty()) break;
			/* The waiter's entire request is atomic. If we block any part of it,
			 * its FIFO priority must not prevent us from clearing that space.
			 * Grant conflicts and physical clearance still apply to our departure. */
			if (std::any_of(other->junction_requests.begin(), other->junction_requests.end(), [&](const auto &pending) {
				return AircraftJunctionActionsConflict(physical, pending);
			})) continue;
			for (const auto &action : desired) {
				for (const auto &pending : other->junction_requests) {
					if (!AircraftJunctionActionsConflict(action, pending)) continue;
					return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, origin);
				}
			}
		}
	}
	for (const auto &action : desired) {
		if (!CheckAircraftJunctionActionPhysical(v, action)) return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, origin);
	}
	for (auto &action : desired) {
		v->junction_actions.push_back(std::move(action));
	}
	CancelAircraftJunctionRequest(v);
	return true;
}

bool EnsureAircraftJunctionAdmission(Aircraft *v)
{
	if (v->junction_legacy) {
		RefreshAircraftLegacyJunctionFootprint(v);
		if (v->planned_path.empty() && v->reserved_runway == INVALID_TILE && v->state != AS_START_TAKEOFF &&
				v->next_trackdir == INVALID_TRACKDIR && !HasBit(v->flags, VAF_PUSHBACK)) FinishAircraftLegacyJunctionAction(v);
		else return true;
	}
	/* Edge turns are admitted before their old route node is removed. No
	 * sliding reconstruction or new acquisition is needed during rotation. */
	if (!v->junction_actions.empty() && (v->next_trackdir != INVALID_TRACKDIR || HasBit(v->flags, VAF_PUSHBACK))) return true;
	PruneAircraftJunctions(v);
	if (v->junction_regions.empty() && (AircraftJunctionResting(v) || (v->state == AS_IDLE && v->planned_path.empty() && v->reserved_runway == INVALID_TILE))) return true;
	return TryReserveAircraftJunctions(v, INVALID_TILE, TRACK_BIT_NONE);
}

static bool TryReserveAircraftTaxiRunways(Aircraft *v, TileIndex tile, TrackBits tracks)
{
	/* Taxiing never claims a whole runway. Single-tile real reservations are
	 * the only ground claim; the full runway is taken only for the physical
	 * takeoff/landing roll in TryReserveAircraftRunway. */
	(void)v;
	(void)tile;
	(void)tracks;
	return true;
}

bool EnsureAircraftTaxiRunways(Aircraft *v)
{
	PruneAircraftTaxiRunways(v);
	return TryReserveAircraftTaxiRunways(v, INVALID_TILE, TRACK_BIT_NONE);
}

static void RestoreAircraftTaxiRunways(Aircraft *v, const std::vector<TileIndex> &runways)
{
	const auto previous = AircraftLockedRunwayTiles(v);
	v->taxi_runways = runways;
	ReleaseAircraftRunwayTiles(v, previous);
	if (!v->ignore_airport_reservations) {
		for (TileIndex tile : AircraftLockedRunwayTiles(v)) {
			_aircraft_runway_owners[tile] = v->index;
			SetReservationAsRunway(tile, true);
			if (_show_airport_tracks) MarkTileDirtyByTile(tile);
		}
	}
}

bool TryReserveAircraftRunway(Aircraft *v, TileIndex extreme)
{
	const auto tiles = RunwayTiles(extreme);
	if (tiles.empty()) return false;
	const AircraftJunctionState previous_junctions(v);
	const auto previous_runways = v->taxi_runways;
	bool acquired = false;
	auto rollback = scope_guard([&]() {
		if (acquired) return;
		RestoreAircraftJunctions(v, previous_junctions);
		RestoreAircraftTaxiRunways(v, previous_runways);
	});
	if (!TryReserveAircraftJunctions(v, INVALID_TILE, TRACK_BIT_NONE, extreme)) return false;
	if (v->ignore_airport_reservations) {
		v->reserved_runway = extreme;
		ReleaseAircraftTaxiPath(v, false);
		acquired = true;
		 
		return true;
	}
	for (TileIndex tile : tiles) {
		/* Virtual taxi routes do not block takeoff/landing. The complete runway
		 * lock still prevents their real windows from entering until released. */
		AircraftTaxiBlock block = CheckAircraftRunwayTile(v, tile);
		if (block != AircraftTaxiBlock::None) {
			 
			return false;
		}
		auto runway = _aircraft_runway_owners.find(tile);
		if (runway != _aircraft_runway_owners.end() && runway->second != v->index) return false;
		auto taxi = _aircraft_tile_owners.find(tile);
		if (taxi != _aircraft_tile_owners.end() && (taxi->second.size() != 1 || taxi->second.begin()->first != v->index)) return false;
		if (runway == _aircraft_runway_owners.end() && GetReservationAsRunway(tile)) return false;
		if (taxi == _aircraft_tile_owners.end() && HasAirportTrackReserved(tile)) return false;
	}
	if (v->reserved_runway != INVALID_TILE) ReleaseAircraftRunway(v);
	v->reserved_runway = extreme;
	ReleaseAircraftTaxiPath(v, false);
	for (TileIndex tile : tiles) {
		_aircraft_runway_owners[tile] = v->index;
		SetReservationAsRunway(tile, true);
		if (_show_airport_tracks) MarkTileDirtyByTile(tile);
	}
	 
	acquired = true;
	return true;
}

void ReleaseAircraftRunway(Aircraft *v)
{
	if (v->reserved_runway == INVALID_TILE) return;
	std::set<TileIndex> tiles;
	for (TileIndex tile : RunwayTiles(v->reserved_runway)) tiles.insert(tile);
	v->reserved_runway = INVALID_TILE;
	if (tiles.empty()) {
		/* The layout may already have been removed. Retain any independent
		 * taxi locks, and clear only this aircraft's remaining index records. */
		for (const auto &[tile, owner] : _aircraft_runway_owners) { if (owner == v->index) tiles.insert(tile); }
	}
	ReleaseAircraftRunwayTiles(v, tiles);
	if (v->junction_legacy) {
		RefreshAircraftLegacyJunctionFootprint(v);
		if (v->reserved_path.empty() && (v->IsAircraftFlying() || v->planned_path.empty())) FinishAircraftLegacyJunctionAction(v);
	}
	PruneAircraftJunctions(v);
}

bool IsAircraftGroundTileAvailable(const Aircraft *v, TileIndex tile)
{
	return CheckAircraftTaxiTracks(v, tile, TRACK_BIT_NONE, false) == AircraftTaxiBlock::None;
}

bool IsAircraftRunwayAvailable(const Aircraft *v, TileIndex extreme)
{
	const auto tiles = RunwayTiles(extreme);
	if (tiles.empty()) return false;
	/* Use the same full-axis check as acquisition; distant junction grants
	 * are not runway locks. Local head admission is checked when committing. */
	for (TileIndex tile : tiles) { if (CheckAircraftRunwayTile(v, tile) != AircraftTaxiBlock::None) return false; }
	return true;
}

void EnableAircraftIgnoreReservations(Aircraft *v)
{
	if (v->ignore_airport_reservations) return;
	/* Retain movement records, but remove only this aircraft's public ownership.
	 * Other aircraft's map bits and runway locks must never be cleared. */
	auto real = v->reserved_path;
	auto virtuals = v->virtual_path;
	auto taxi_runways = v->taxi_runways;
	const AircraftJunctionState junctions(v);
	CancelAircraftJunctionRequest(v);
	ReleaseAircraftJunctions(v);
	TileIndex gate = v->allocated_terminal;
	TileIndex runway = v->reserved_runway;
	for (const auto &record : real) ReleaseAircraftTile(v, record.tile, record.tracks);
	ReleaseAircraftVirtualPath(v);
	ReleaseAircraftTerminal(v);
	ReleaseAircraftTaxiRunways(v);
	ReleaseAircraftRunway(v);
	v->reserved_path = std::move(real);
	v->virtual_path = std::move(virtuals);
	v->taxi_runways = std::move(taxi_runways);
	v->allocated_terminal = gate;
	v->reserved_runway = runway;
	/* An ignored aircraft keeps its local movement records but has no public
	 * real, virtual, terminal or runway owner entries. */
	v->ignore_airport_reservations = true;
	RestoreAircraftJunctions(v, junctions);
	v->taxi_block_reason = AircraftTaxiBlock::None;
	v->taxi_blocked_ticks = v->taxi_jam_ticks = 0;
	v->ClearWaitTime();
	 
}

void ResetAircraftIgnoreReservations(Aircraft *v)
{
	if (!v->ignore_airport_reservations) return;
	ReleaseAircraftTaxiPath(v, false);
	ReleaseAircraftRunway(v);
	v->ignore_airport_reservations = false;
	InvalidateWindowData(WindowClass::VehicleView, v->index);
	 
}

Trackdir GetFreeAirportTrackdir(TileIndex tile, Trackdir preferred_trackdir, const Aircraft *v)
{
	if (!IsTaxiTile(tile) || !IsValidTrackdir(preferred_trackdir)) return INVALID_TRACKDIR;
	if (v != nullptr ? !IsAircraftGroundTileAvailable(v, tile) : HasAirportTrackReserved(tile)) return INVALID_TRACKDIR;
	TrackBits tracks = GetAirportTileTracks(tile) & TRACK_BIT_CROSS;
	if (HasTrack(tracks, TrackdirToTrack(preferred_trackdir))) return preferred_trackdir;
	if (tracks == TRACK_BIT_NONE) return INVALID_TRACKDIR;
	Trackdir td = TrackToTrackdir(RemoveFirstTrack(&tracks));
	CFollowTrackAirport follower(INVALID_OWNER);
	return follower.Follow(tile, td) ? td : ReverseTrackdir(td);
}

/* Migrate only stale local state that older builds could produce through
 * normal play. Current snapshots must retain their exact simulation state. */
static void MigrateAircraftReservationState(Aircraft *v)
{
	if (v->IsHelicopter() && v->IsAircraftFreelyFlying()) v->allocated_terminal = INVALID_TILE;
	if (!v->ignore_airport_reservations) return;
	bool valid = v->allocated_terminal == INVALID_TILE || IsTaxiTile(v->allocated_terminal);
	valid &= v->reserved_runway == INVALID_TILE || !RunwayTiles(v->reserved_runway).empty();
	for (const auto &record : v->reserved_path) {
		valid &= IsTaxiTile(record.tile) && record.tracks != TRACK_BIT_NONE &&
				(GetAirportTileTracks(record.tile) & record.tracks) == record.tracks;
	}
	for (size_t i = 0; i < v->planned_path.size(); ++i) {
		const auto &node = v->planned_path[i];
		if (!IsTaxiTile(node.tile) || !IsValidTrackdir(node.td) || !HasTrack(GetAirportTileTracks(node.tile), TrackdirToTrack(node.td))) {
			valid = false;
			continue;
		}
		if (i == 0 || node.tile == v->planned_path[i - 1].tile) continue;
		const auto &previous = v->planned_path[i - 1];
		if (!IsTaxiTile(previous.tile) || !IsValidTrackdir(previous.td)) { valid = false; continue; }
		CFollowTrackAirport follower(v);
		valid &= follower.Follow(previous.tile, previous.td) && follower.new_tile == node.tile &&
				(follower.new_td_bits & TrackdirToTrackdirBits(node.td)) != TRACKDIR_BIT_NONE;
	}
	if (valid) return;
	const bool airborne = v->IsAircraftFlying() || v->state == AS_TAKEOFF_BEFORE_FLYING || !IsTaxiTile(v->tile);
	const TileIndex runway = !RunwayTiles(v->reserved_runway).empty() ? v->reserved_runway : INVALID_TILE;
	ReleaseAircraftTaxiPath(v, false);
	v->reserved_runway = runway;
	v->taxi_progress = v->pushback_progress = 0;
	v->next_trackdir = v->Next()->next_trackdir = INVALID_TRACKDIR;
	v->next_pos.pos = AP_DEFAULT;
	v->ClearWaitTime();
	if (airborne) {
		v->state = AS_FLYING_NO_DEST;
		v->reserved_runway = INVALID_TILE;
		v->UpdateNextTile(v->tile);
	} else {
		v->state = AS_IDLE;
		v->cur_speed = v->subspeed = v->progress = 0;
		v->UpdateNextTile(INVALID_TILE);
		if (IsValidTrackdir(v->trackdir)) {
			TryReserveAircraftTile(v, v->tile, TrackToTrackBits(TrackdirToTrack(v->trackdir)));
		}
	}
}

static void InitializeAircraftTaxiRunwaysAfterLoad()
{
	/* Mark every pre-v8 in-progress action before any new aircraft can acquire
	 * a grant. There is no winner election among saved occupants. */
	if (SlXvIsFeatureMissing(XSLFI_AIRCRAFT_ROLLING_RESERVATION, 9)) {
		for (Aircraft *v : Aircraft::Iterate()) {
			if (!v->IsNormalAircraft()) continue;
			v->junction_legacy = !v->vehstatus.Test(VehState::Hidden) &&
					(!v->planned_path.empty() || v->reserved_runway != INVALID_TILE ||
					v->state == AS_START_TAKEOFF || v->state == AS_LANDED ||
					(v->state <= AS_RUNNING && (v->next_trackdir != INVALID_TRACKDIR ||
					(IsTaxiTile(v->tile) && !IsApron(v->tile) && !IsHangar(v->tile)))) ||
					v->state == AS_FLYING_HELICOPTER_TAKEOFF || v->state == AS_FLYING_HELICOPTER_LANDING);
			v->junction_legacy_tiles.clear();
			RefreshAircraftLegacyJunctionFootprint(v);
		}
	}
	_aircraft_junction_loading = false;
	for (Aircraft *v : Aircraft::Iterate()) {
		if (!v->IsNormalAircraft()) continue;
		const AircraftTaxiBlock reason = v->taxi_block_reason;
		/* v6 already has authoritative runway records. Do not add locks or
		 * reset movement when only the junction feature needs upgrading. */
		if (SlXvIsFeatureMissing(XSLFI_AIRCRAFT_ROLLING_RESERVATION, 6)) EnsureAircraftTaxiRunways(v);
		v->taxi_block_reason = reason;
	}
}

/* Validate without changing the saved simulation state; rebuild indexes only. */
void AfterLoadAircraftReservations(bool migrate, bool migrate_virtual)
{
	ClearAircraftReservationIndexes();
	_aircraft_junction_loading = true;
	auto loading_guard = scope_guard([]() { _aircraft_junction_loading = false; });
	/* Junction grants and queued requests in current snapshots are simulation
	 * state. Restore exactly; never arbitrate again while joining a server. */
	if (SlXvIsFeaturePresent(XSLFI_AIRCRAFT_ROLLING_RESERVATION, 9)) {
		for (Aircraft *v : Aircraft::Iterate()) {
			if (!v->IsNormalAircraft()) continue;
			if (!v->junction_legacy && !v->junction_legacy_tiles.empty()) SlErrorCorrupt("Aircraft legacy clearance without an action");
			if (v->junction_legacy && (!v->junction_actions.empty() || !v->junction_requests.empty())) SlErrorCorrupt("Legacy aircraft also has action admission");
			for (TileIndex tile : v->junction_legacy_tiles) {
				if (!IsTaxiTile(tile)) SlErrorCorrupt("Invalid saved legacy aircraft tile");
			}
			if (!std::is_sorted(v->junction_legacy_tiles.begin(), v->junction_legacy_tiles.end()) ||
					std::adjacent_find(v->junction_legacy_tiles.begin(), v->junction_legacy_tiles.end()) != v->junction_legacy_tiles.end()) SlErrorCorrupt("Invalid saved legacy aircraft clearance");
			if (v->junction_legacy) _aircraft_junction_legacy.insert(v->index);
			auto validate = [&](const std::vector<AircraftJunctionAction> &actions) {
				if (actions.size() > 64) SlErrorCorrupt("Too many aircraft junction actions");
				for (const auto &action : actions) {
					if (!IsTaxiTile(action.junction) || !IsValidTrackdir(action.entry_td) || !IsValidTrackdir(action.exit_td) ||
							action.tiles.empty() || action.tiles.size() > 64 || action.tiles.size() != action.tracks.size() || action.tiles.size() != action.swept.size() ||
							!std::is_sorted(action.tiles.begin(), action.tiles.end()) || std::adjacent_find(action.tiles.begin(), action.tiles.end()) != action.tiles.end()) SlErrorCorrupt("Invalid saved aircraft junction action");
					const StationID station = GetStationIndex(action.junction);
					/* An unused branch may have been removed since admission. The saved
					 * traversal remains valid even if this tile is no longer a junction. */
					if (!std::binary_search(action.tiles.begin(), action.tiles.end(), action.entry_tile) ||
							!std::binary_search(action.tiles.begin(), action.tiles.end(), action.exit_tile)) SlErrorCorrupt("Disconnected saved aircraft junction action");
					for (size_t i = 0; i < action.tiles.size(); ++i) {
						if (!IsTaxiTile(action.tiles[i]) || !IsAirportTileOfStation(action.tiles[i], station) ||
								(action.tracks[i] | action.swept[i]) == TRACK_BIT_NONE ||
								((action.tracks[i] | action.swept[i]) & ~TRACK_BIT_ALL) != TRACK_BIT_NONE ||
								(GetAirportTileTracks(action.tiles[i]) & action.tracks[i]) != action.tracks[i]) SlErrorCorrupt("Invalid aircraft junction action tracks");
					}
				}
			};
			validate(v->junction_actions);
			validate(v->junction_requests);
			if (v->junction_requests.empty() != (v->junction_request_tick == INVALID_STATE_TICKS)) SlErrorCorrupt("Invalid aircraft action request time");
			if (!v->ignore_airport_reservations) {
				for (VehicleID id : _aircraft_junction_owners) {
					const Aircraft *other = Aircraft::Get(id);
					for (const auto &a : v->junction_actions) {
						for (const auto &b : other->junction_actions) {
							if (AircraftJunctionActionsConflict(a, b)) SlErrorCorrupt("Conflicting saved aircraft junction actions");
						}
					}
				}
			}
			/* Only derived display/index records are rebuilt; grants and FIFO
			 * timestamps are restored without re-admission. */
			SyncAircraftJunctionRecords(v);
		}
	}
	if (migrate) {
		for (Station *st : Station::Iterate()) {
			for (TileIndex tile : st->airport) {
				if (!IsTaxiTile(tile) || !IsAirportTileOfStation(tile, st->index)) continue;
				TrackBits tracks = GetReservedAirportTracks(tile);
				while (tracks != TRACK_BIT_NONE) RemoveAirportTrackReservation(tile, RemoveFirstTrack(&tracks));
				if (IsRunway(tile)) SetReservationAsRunway(tile, false);
			}
		}
		/* Occupants are restored first, before anyone claims the tile ahead. */
		for (Aircraft *v : Aircraft::Iterate()) {
			if (!v->IsNormalAircraft()) continue;
			v->reserved_path.clear();
			v->virtual_path.clear();
			v->planned_path.clear();
			v->path.clear();
			v->junction_actions.clear();
			v->junction_requests.clear();
			v->junction_regions.clear();
			v->junction_tiles.clear();
			v->junction_runways.clear();
			v->junction_entered = false;
			CancelAircraftJunctionRequest(v);
			v->reserved_runway = INVALID_TILE;
			v->taxi_runways.clear();
			v->allocated_terminal = INVALID_TILE;
			v->pushback_origin = v->pushback_target = INVALID_TILE;
			ClrBit(v->flags, VAF_PUSHBACK);
			v->taxi_progress = v->pushback_progress = v->taxi_blocked_ticks = 0;
			v->taxi_jam_ticks = 0;
			v->taxi_block_reason = AircraftTaxiBlock::None;
			ClrBit(v->flags, VAF_TAXI_PHASING);
			const bool runway = v->state == AS_DESCENDING || v->state == AS_FLYING_LANDING ||
					v->state == AS_LANDED || v->state == AS_TAKEOFF_BEFORE_FLYING || v->state == AS_FLYING_TAKEOFF;
			if (runway && !v->IsHelicopter()) {
				TileIndex extreme = v->GetNextTile();
				/* Old saves can hold states this reconstruction cannot express
				 * (e.g. a stale runway endpoint). Never reject the save for that:
				 * leave the aircraft unreserved and let its controller wait or
				 * replan from the current state; physical occupancy still blocks
				 * other aircraft from the runway meanwhile. */
				if (!IsTaxiTile(extreme) || !IsRunwayExtreme(extreme) || !TryReserveAircraftRunway(v, extreme)) {
				}
				continue;
			}
			/* A holding fixed-wing aircraft has not committed to landing. */
			if (v->IsAircraftFreelyFlying() || (!v->IsHelicopter() && v->state == AS_ON_HOLD_APPROACHING)) continue;
			if (v->vehstatus.Test(VehState::Hidden) || !IsTaxiTile(v->tile)) continue;
			Trackdir td = IsValidTrackdir(v->next_trackdir) ? v->next_trackdir : v->trackdir;
			if (IsValidTrackdir(td) && TryReserveAircraftTile(v, v->tile, TrackToTrackBits(TrackdirToTrack(td)), TrackdirToTrackdirBits(td))) {
				if (IsApron(v->tile) && (IsTerminalState(v->state) || v->current_order.IsType(OT_LOADING)) &&
						!TryClaimAircraftTerminal(v, v->tile)) {
				}
			} else {
			}
			if (v->state == AS_RUNNING) {
				v->state = AS_IDLE;
				v->next_trackdir = INVALID_TRACKDIR;
				v->next_pos.pos = AP_DEFAULT;
				v->UpdateNextTile(INVALID_TILE);
				v->ClearWaitTime();
			}
		}
		for (Aircraft *v : Aircraft::Iterate()) {
			if (!v->IsNormalAircraft() || v->state != AS_IDLE || v->vehstatus.Test(VehState::Hidden) ||
					v->vehstatus.Test(VehState::Crashed) || !IsTaxiTile(v->tile) || !IsValidTrackdir(v->trackdir)) continue;
			extern AircraftState GetNextAircraftState(const Aircraft &);
			PBSTileInfo destination{};
			bool found = false;
			AircraftState wanted = GetNextAircraftState(*v);
			Trackdir first = YapfAircraftFindPath(v, destination, found, wanted, v->path);
			if (!found || !destination.okay || !IsValidTrackdir(first)) continue;
			if (!TryReserveAircraftTaxiPath(v) || (IsTerminalState(wanted) && !TryClaimAircraftTerminal(v, destination.tile))) {
				ReleaseAircraftTaxiPath(v, true);
				continue;
			}
			/* Retain an admitted virtual route if the first real window must wait. */
			ExtendAircraftTaxiReservation(v);
			v->state = AS_RUNNING;
			v->Next()->state = wanted;
			v->next_pos.pos = AP_DEFAULT;
			v->UpdateNextTile(destination.tile);
		}
		InitializeAircraftTaxiRunwaysAfterLoad();
		return;
	}

	for (Aircraft *v : Aircraft::Iterate()) {
		if (!v->IsNormalAircraft()) continue;
		if (SlXvIsFeatureMissing(XSLFI_AIRCRAFT_ROLLING_RESERVATION, 9)) {
			v->junction_actions.clear();
			v->junction_requests.clear();
			v->junction_regions.clear();
			v->junction_tiles.clear();
			v->junction_runways.clear();
			v->junction_entered = false;
			CancelAircraftJunctionRequest(v);
		}
		if (SlXvIsFeatureMissing(XSLFI_AIRCRAFT_ROLLING_RESERVATION, 5)) MigrateAircraftReservationState(v);
		if (v->reserved_path.size() > 2) SlErrorCorrupt("Aircraft rolling reservation exceeds two tiles");
		std::set<TileIndex> real_tiles;
		for (const auto &r : v->reserved_path) {
			/* Shared taxi occupancy is legal; duplicates within one aircraft are not. */
			if (!IsTaxiTile(r.tile) || r.tracks == TRACK_BIT_NONE ||
					(GetAirportTileTracks(r.tile) & r.tracks) != r.tracks ||
					(!v->ignore_airport_reservations && (GetReservedAirportTracks(r.tile) & r.tracks) != r.tracks) ||
					!real_tiles.insert(r.tile).second) {
				SlErrorCorrupt("Invalid or overlapping saved aircraft reservation");
			}
			if (!v->ignore_airport_reservations) _aircraft_tile_owners[r.tile][v->index] |= r.tracks;
		}
		if (v->allocated_terminal != INVALID_TILE && (!IsTaxiTile(v->allocated_terminal) ||
				(!v->ignore_airport_reservations && !_aircraft_terminal_owners.emplace(v->allocated_terminal, v->index).second))) SlErrorCorrupt("Invalid aircraft terminal claim");
		if (v->reserved_runway != INVALID_TILE) {
			const auto tiles = RunwayTiles(v->reserved_runway);
			if (tiles.empty()) SlErrorCorrupt("Invalid saved aircraft runway");
			for (TileIndex tile : tiles) {
				if (v->ignore_airport_reservations) continue;
				auto [owner, inserted] = _aircraft_runway_owners.emplace(tile, v->index);
				if (!GetReservationAsRunway(tile) || (!inserted && owner->second != v->index)) SlErrorCorrupt("Invalid saved aircraft runway reservation");
			}
		}
		if (v->taxi_runways.size() > 16) SlErrorCorrupt("Too many saved aircraft taxi runway locks");
		std::set<TileIndex> taxi_runways;
		for (TileIndex extreme : v->taxi_runways) {
			const auto tiles = RunwayTiles(extreme);
			if (tiles.empty() || !IsRunwayStart(extreme) || !taxi_runways.insert(extreme).second) SlErrorCorrupt("Invalid saved aircraft taxi runway lock");
			for (TileIndex tile : tiles) {
				if (v->ignore_airport_reservations) continue;
				auto [owner, inserted] = _aircraft_runway_owners.emplace(tile, v->index);
				if (!GetReservationAsRunway(tile) || (!inserted && owner->second != v->index)) SlErrorCorrupt("Conflicting saved aircraft taxi runway locks");
			}
		}
		if (!v->vehstatus.Test(VehState::Crashed)) {
			if (HasBit(v->flags, VAF_PUSHBACK) && (v->state != AS_RUNNING ||
					!IsTaxiTile(v->pushback_origin) || !IsTaxiTile(v->pushback_target) || v->planned_path.empty())) SlErrorCorrupt("Invalid saved aircraft pushback");
			if (v->state == AS_RUNNING && (v->planned_path.empty() || v->planned_path.front().tile != v->tile)) SlErrorCorrupt("Aircraft route does not start at its current tile");
			if (v->state == AS_RUNNING && !HasAircraftTileReservation(v, v->tile, TRACK_BIT_NONE)) {
				/* An admitted route may be stationary while its first real taxi window
				 * is blocked. Its complete directional booking is validated below. */
				const bool waiting_for_window = v->reserved_path.empty() && !v->virtual_path.empty() &&
						v->cur_speed == 0 && v->taxi_progress == 0 && v->pushback_progress == 0 &&
						v->next_trackdir == INVALID_TRACKDIR && !HasBit(v->flags, VAF_PUSHBACK) &&
						(v->taxi_block_reason == AircraftTaxiBlock::Traffic || v->taxi_block_reason == AircraftTaxiBlock::Protected);
				if (!waiting_for_window) SlErrorCorrupt("Taxiing aircraft has no current tile reservation");
			}
		}
		if (v->taxi_progress > 255 || v->pushback_progress > 255) SlErrorCorrupt("Invalid aircraft movement remainder");
		for (size_t i = 0; i < v->planned_path.size(); ++i) {
			const auto &n = v->planned_path[i];
			if (!IsTaxiTile(n.tile) || !IsValidTrackdir(n.td) || !HasTrack(GetAirportTileTracks(n.tile), TrackdirToTrack(n.td))) SlErrorCorrupt("Invalid saved aircraft route");
			if (i == 0) continue;
			const auto &previous = v->planned_path[i - 1];
			if (n.tile == previous.tile) continue;
			CFollowTrackAirport follower(v);
			if (!follower.Follow(previous.tile, previous.td) || follower.new_tile != n.tile ||
					(follower.new_td_bits & TrackdirToTrackdirBits(n.td)) == TRACKDIR_BIT_NONE) SlErrorCorrupt("Disconnected saved aircraft route");
		}
	}
	for (const auto &[tile, gate_owner] : _aircraft_terminal_owners) {
		auto occupied = _aircraft_tile_owners.find(tile);
		if (occupied != _aircraft_tile_owners.end() && (occupied->second.size() != 1 || occupied->second.begin()->first != gate_owner)) SlErrorCorrupt("Aircraft gate claim conflicts with occupant");
	}
	for (const auto &[tile, runway_owner] : _aircraft_runway_owners) {
		auto taxi = _aircraft_tile_owners.find(tile);
		if (taxi == _aircraft_tile_owners.end()) continue;
		for (const auto &[id, tracks] : taxi->second) {
			if (id == runway_owner) continue;
			/* Phasing can acquire real tracks under another aircraft's runway
			 * lock. Restore that transient overlap without changing either owner. */
			const Aircraft *v = Aircraft::Get(id);
			if (!HasBit(v->flags, VAF_TAXI_PHASING)) SlErrorCorrupt("Aircraft taxi reservation overlaps another aircraft runway");
		}
	}
	if (migrate_virtual) {
		/* Older builds did not protect swept geometry against construction.
		 * Convert once; conflicting old plans wait on their actual tile rather
		 * than refusing the save or taking a partial prefix of another route. */
		for (Aircraft *v : Aircraft::Iterate()) {
			if (!v->IsNormalAircraft()) continue;
			v->virtual_path.clear();
			if (SlXvIsFeatureMissing(XSLFI_AIRCRAFT_ROLLING_RESERVATION, 2)) {
				v->taxi_jam_ticks = 0;
				v->taxi_block_reason = AircraftTaxiBlock::None;
				ClrBit(v->flags, VAF_TAXI_PHASING);
			}
			if (v->state == AS_RUNNING && !v->vehstatus.Test(VehState::Crashed) && TryReserveAircraftTaxiPath(v)) continue;
			if (v->planned_path.empty()) continue;
			ReleaseAircraftTaxiPath(v, !v->vehstatus.Test(VehState::Hidden));
			if (v->state != AS_RUNNING) continue;
			v->state = AS_IDLE;
			v->cur_speed = 0;
			v->taxi_progress = v->pushback_progress = 0;
			v->next_pos.pos = AP_DEFAULT;
			v->UpdateNextTile(INVALID_TILE);
			v->ClearWaitTime();
		}
		InitializeAircraftTaxiRunwaysAfterLoad();
		return;
	}

	/* Exact saved records, including swept edge-turn directions, are restored
	 * without admission or arbitration in new saves and network snapshots. */
	for (Aircraft *v : Aircraft::Iterate()) {
		if (!v->IsNormalAircraft()) continue;
		auto desired = AircraftPlannedTrackdirs(v);
		if (v->virtual_path.size() != desired.size()) SlErrorCorrupt("Aircraft virtual reservation does not cover its route");
		for (const auto &record : v->virtual_path) {
			auto wanted = desired.find(record.tile);
			if (wanted == desired.end() || record.trackdirs == TRACKDIR_BIT_NONE ||
					(record.trackdirs & ~TRACKDIR_BIT_MASK) != TRACKDIR_BIT_NONE || record.trackdirs != wanted->second) {
				SlErrorCorrupt("Invalid saved aircraft virtual reservation");
			}
			if (!IsTaxiTile(record.tile) || (GetAirportTileTracks(record.tile) & TrackdirBitsToTrackBits(record.trackdirs)) != TrackdirBitsToTrackBits(record.trackdirs)) SlErrorCorrupt("Invalid saved aircraft virtual tracks");
			if (v->ignore_airport_reservations) { desired.erase(wanted); continue; }
			auto &owners = _aircraft_virtual_bookings[record.tile];
			for (const auto &[id, trackdirs] : owners) {
				for (TrackdirBits bits = record.trackdirs; bits != TRACKDIR_BIT_NONE; bits = KillFirstBit(bits)) {
					Trackdir td = static_cast<Trackdir>(FindFirstBit(bits));
					if ((trackdirs & TrackdirToTrackdirBits(ReverseTrackdir(td))) != TRACKDIR_BIT_NONE) SlErrorCorrupt("Opposing saved aircraft virtual reservations");
				}
			}
			if (!owners.emplace(v->index, record.trackdirs).second) SlErrorCorrupt("Duplicate saved aircraft virtual reservation");
			desired.erase(wanted);
		}
		if (!desired.empty()) SlErrorCorrupt("Incomplete saved aircraft virtual reservation");
		/* Current occupancy may also contain a resting/origin track; any tile
		 * ahead, however, must be part of the acquired virtual route. */
		for (const auto &record : v->reserved_path) {
			TrackBits virtual_tracks = v->ignore_airport_reservations ? TRACK_BIT_NONE : TrackdirBitsToTrackBits(AircraftOwnedVirtualTrackdirs(v, record.tile));
			if (v->ignore_airport_reservations) {
				for (const auto &virtual_record : v->virtual_path) { if (virtual_record.tile == record.tile) virtual_tracks |= TrackdirBitsToTrackBits(virtual_record.trackdirs); }
			}
			if (record.tile != v->tile && (virtual_tracks & record.tracks) != record.tracks) {
				SlErrorCorrupt("Aircraft real reservation lies outside its virtual route");
			}
		}
	}
	if (SlXvIsFeatureMissing(XSLFI_AIRCRAFT_ROLLING_RESERVATION, 9)) InitializeAircraftTaxiRunwaysAfterLoad();
}
