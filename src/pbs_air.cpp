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
#include "sl/extended_ver_sl.h"
#include <cstdio>

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
		LogAircraftReservationState(v, "terminal-claim-denied", tile);
		return false;
	}
	if (v->allocated_terminal != tile) ReleaseAircraftTerminal(v);
	if (!v->ignore_airport_reservations) _aircraft_terminal_owners[tile] = v->index;
	v->allocated_terminal = tile;
	fprintf(stderr, "[AIR-RES] terminal-claimed veh=%u tile=%u current=%u\n", v->index.base(), tile.base(), v->tile.base());
	return true;
}

void ReleaseAircraftTerminal(Aircraft *v)
{
	auto it = _aircraft_terminal_owners.find(v->allocated_terminal);
	if (it != _aircraft_terminal_owners.end() && it->second == v->index) {
		fprintf(stderr, "[AIR-RES] terminal-release veh=%u tile=%u current=%u\n", v->index.base(), v->allocated_terminal.base(), v->tile.base());
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

void LogAircraftReservationState(const Aircraft *v, const char *event, TileIndex tile)
{
	auto log_aircraft = [&](const Aircraft *aircraft, const char *role) {
		fprintf(stderr, "[AIR-RES] %s role=%s veh=%u tick=%u state=%u tile=%u pos=%d,%d dir=%u td=%u next_td=%u hidden=%u goal=%u gate=%u runway=%u booked=%zu route=%zu virtual=%zu jam=%u block=%u phase=%u ignore=%u\n",
			event, role, aircraft->index.base(), (uint)aircraft->tick_counter, (uint)aircraft->state, aircraft->tile.base(),
			aircraft->x_pos, aircraft->y_pos, (uint)aircraft->direction, (uint)aircraft->trackdir, (uint)aircraft->next_trackdir,
			(uint)aircraft->vehstatus.Test(VehState::Hidden), aircraft->GetNextTile().base(), aircraft->allocated_terminal.base(),
			aircraft->reserved_runway.base(), aircraft->reserved_path.size(), aircraft->planned_path.size(), aircraft->virtual_path.size(),
			(uint)aircraft->taxi_jam_ticks, (uint)aircraft->taxi_block_reason, (uint)HasBit(aircraft->flags, VAF_TAXI_PHASING), (uint)aircraft->ignore_airport_reservations);
		for (const auto &record : aircraft->virtual_path) {
			fprintf(stderr, "[AIR-RES] virtual veh=%u tile=%u trackdirs=%04x\n", aircraft->index.base(), record.tile.base(), (uint)record.trackdirs);
		}
		for (const auto &record : aircraft->reserved_path) {
			fprintf(stderr, "[AIR-RES] held veh=%u tile=%u tracks=%02x\n", aircraft->index.base(), record.tile.base(), (uint)record.tracks);
		}
		for (const auto &node : aircraft->planned_path) {
			fprintf(stderr, "[AIR-RES] plan veh=%u tile=%u td=%u choice=%u\n", aircraft->index.base(), node.tile.base(), (uint)node.td, (uint)node.choice);
		}
	};
	log_aircraft(v, "requester");
	if (tile == INVALID_TILE) tile = v->tile;
	if (!IsTaxiTile(tile)) return;
	auto log_tile = [&](TileIndex current) {
		if (!IsTaxiTile(current)) return;
		auto taxi = _aircraft_tile_owners.find(current);
		auto gate = _aircraft_terminal_owners.find(current);
		auto runway = _aircraft_runway_owners.find(current);
		fprintf(stderr, "[AIR-RES] tile event=%s veh=%u tile=%u xy=%u,%u built=%02x map=%02x runway_flag=%u taxi_owners=%zu gate_owner=%u runway_owner=%u\n",
			event, v->index.base(), current.base(), TileX(current), TileY(current), (uint)GetAirportTileTracks(current),
			(uint)GetReservedAirportTracks(current), (uint)(IsRunway(current) && GetReservationAsRunway(current)),
			taxi == _aircraft_tile_owners.end() ? 0 : taxi->second.size(),
			gate == _aircraft_terminal_owners.end() ? VehicleID::Invalid().base() : gate->second.base(),
			runway == _aircraft_runway_owners.end() ? VehicleID::Invalid().base() : runway->second.base());
		std::set<VehicleID> peers;
		if (taxi != _aircraft_tile_owners.end()) {
			for (const auto &[id, tracks] : taxi->second) {
				fprintf(stderr, "[AIR-RES] owner tile=%u veh=%u tracks=%02x\n", current.base(), id.base(), (uint)tracks);
				peers.insert(id);
			}
		}
			auto virtuals = _aircraft_virtual_bookings.find(current);
			if (virtuals != _aircraft_virtual_bookings.end()) {
				for (const auto &[id, trackdirs] : virtuals->second) {
					fprintf(stderr, "[AIR-RES] virtual-owner tile=%u veh=%u trackdirs=%04x\n", current.base(), id.base(), (uint)trackdirs);
					peers.insert(id);
				}
			}
			if (gate != _aircraft_terminal_owners.end()) peers.insert(gate->second);
		if (runway != _aircraft_runway_owners.end()) peers.insert(runway->second);
		for (const Aircraft *other : VehiclesOnTile<VehicleType::Aircraft>(current)) {
			if (other->IsNormalAircraft()) peers.insert(other->index);
		}
		for (VehicleID id : peers) {
			const Vehicle *other = Vehicle::GetIfValid(id);
			if (other != nullptr && other != v && other->type == VehicleType::Aircraft && Aircraft::From(other)->IsNormalAircraft()) log_aircraft(Aircraft::From(other), "peer");
		}
	};
	log_tile(tile);
	for (Track track = TRACK_UPPER; track < TRACK_END; ++track) log_tile(TileAddByDir(tile, _aircraft_envelope_directions[track]));
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

/* This classification never picks a winner from the unordered physical tile hash. */
static AircraftTaxiBlock CheckAircraftTaxiTile(const Aircraft *v, TileIndex tile, bool phasing, bool check_runway = true)
{
	if (!IsTaxiTile(tile)) return AircraftTaxiBlock::Protected;
	if (v->ignore_airport_reservations) return AircraftTaxiBlock::None;
	auto runway = _aircraft_runway_owners.find(tile);
	if (check_runway && ((runway != _aircraft_runway_owners.end() && runway->second != v->index) ||
			(IsRunway(tile) && GetReservationAsRunway(tile) && runway == _aircraft_runway_owners.end()))) return AircraftTaxiBlock::Protected;
	if (GetReservedAirportTracks(tile) != AircraftReservedTracks(tile)) return AircraftTaxiBlock::Protected;
	AircraftTaxiBlock block = AircraftTaxiBlock::None;
	auto inspect = [&](const Aircraft *other) {
		if (other == v || (other != nullptr && other->ignore_airport_reservations)) return;
		bool shareable = IsAircraftTaxiSharingAllowed(v, tile, other);
		if (phasing && shareable) return;
		block = MergeAircraftBlock(block, shareable ? AircraftTaxiBlock::Traffic : AircraftTaxiBlock::Protected);
	};
	auto owners = _aircraft_tile_owners.find(tile);
	if (owners != _aircraft_tile_owners.end()) {
		for (const auto &[id, tracks] : owners->second) {
			const Vehicle *other = Vehicle::GetIfValid(id);
			inspect(other != nullptr && other->type == VehicleType::Aircraft ? Aircraft::From(other) : nullptr);
		}
	}
	for (const Aircraft *other : VehiclesOnTile<VehicleType::Aircraft>(tile)) {
		if (other->IsNormalAircraft() && other->tile == tile && !other->vehstatus.Test(VehState::Hidden) && !other->IsAircraftFlying()) inspect(other);
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
		if (GetReservedAirportTracks(neighbour) != AircraftReservedTracks(neighbour)) return AircraftTaxiBlock::Protected;
		for (const auto &[id, tracks] : _aircraft_tile_owners.at(neighbour)) {
			if (id == v->index || !HasTrack(tracks, track)) continue;
			const Vehicle *vehicle = Vehicle::GetIfValid(id);
			const Aircraft *other = vehicle != nullptr && vehicle->type == VehicleType::Aircraft ? Aircraft::From(vehicle) : nullptr;
			const bool shareable = IsAircraftTaxiSharingAllowed(v, tile, other) && !IsApron(neighbour) && !IsHangar(neighbour);
			if (!phasing || !shareable) block = MergeAircraftBlock(block, shareable ? AircraftTaxiBlock::Traffic : AircraftTaxiBlock::Protected);
		}
	}
	return block;
}

/* A half-track sweeps only the facing half-track of its diagonal neighbour.
 * A centre stand or a diagonal track there is not part of that envelope. */
static AircraftTaxiBlock CheckAircraftAssociatedTrack(const Aircraft *v, TileIndex tile, Track track, bool phasing)
{
	TileIndex neighbour = TileAddByDir(tile, _aircraft_envelope_directions[track]);
	if (!IsTaxiTile(neighbour) || !IsAirportTileOfStation(neighbour, GetStationIndex(tile)) || IsHangar(neighbour)) return AircraftTaxiBlock::Protected;
	if (v->ignore_airport_reservations) return AircraftTaxiBlock::None;
	auto runway = _aircraft_runway_owners.find(neighbour);
	if (IsRunway(neighbour) && GetReservationAsRunway(neighbour) &&
			(runway == _aircraft_runway_owners.end() || runway->second != v->index)) return AircraftTaxiBlock::Protected;
	if (GetReservedAirportTracks(neighbour) != AircraftReservedTracks(neighbour)) return AircraftTaxiBlock::Protected;

	Track opposite = TrackToOppositeTrack(track);
	AircraftTaxiBlock block = AircraftTaxiBlock::None;
	auto inspect = [&](const Aircraft *other) {
		if (other == v || (other != nullptr && other->ignore_airport_reservations)) return;
		bool shareable = IsAircraftTaxiSharingAllowed(v, neighbour, other) && !IsApron(tile) && !IsHangar(tile);
		if (!phasing || !shareable) block = MergeAircraftBlock(block, shareable ? AircraftTaxiBlock::Traffic : AircraftTaxiBlock::Protected);
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
		fprintf(stderr, "[AIR-RES] half-envelope-blocked veh=%u tile=%u track=%u neighbour=%u opposite=%u block=%u\n",
			v->index.base(), tile.base(), (uint)track, neighbour.base(), (uint)opposite, (uint)block);
	}
	return block;
}

static AircraftTaxiBlock CheckAircraftTaxiTracks(const Aircraft *v, TileIndex tile, TrackBits tracks, bool phasing)
{
	if (!IsTaxiTile(tile)) return AircraftTaxiBlock::Protected;
	AircraftTaxiBlock block = MergeAircraftBlock(CheckAircraftTaxiTile(v, tile, phasing),
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
	LogAircraftReservationState(v, block == AircraftTaxiBlock::Traffic ? "taxi-traffic-blocked" : "taxi-protected-blocked", tile);
	return false;
}

bool IsAircraftTaxiEnvelopeAvailable(Aircraft *v, TileIndex tile)
{
	return RecordAircraftTaxiBlock(v, MergeAircraftBlock(CheckAircraftTaxiTile(v, tile, HasBit(v->flags, VAF_TAXI_PHASING)),
			CheckIncomingAircraftEnvelopes(v, tile, HasBit(v->flags, VAF_TAXI_PHASING))), tile);
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

bool TryReserveAircraftTile(Aircraft *v, TileIndex tile, TrackBits tracks, TrackdirBits travel)
{
	if (!IsTaxiTile(tile) || tracks == TRACK_BIT_NONE) return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, tile);
	if (v->ignore_airport_reservations) {
		if ((GetAirportTileTracks(tile) & tracks) != tracks ||
				(!v->planned_path.empty() && !HasAircraftVirtualTileReservation(v, tile, travel))) return false;
		for (auto &record : v->reserved_path) {
			if (record.tile == tile) { record.tracks |= tracks; return true; }
		}
		if (v->reserved_path.size() >= 2) return false;
		v->reserved_path.push_back({tile, tracks});
		return true;
	}
	TrackBits held = AircraftOwnedTracks(v, tile);
	TrackBits recorded = TRACK_BIT_NONE;
	for (const auto &r : v->reserved_path) { if (r.tile == tile) recorded |= r.tracks; }
	if (held != recorded || GetReservedAirportTracks(tile) != AircraftReservedTracks(tile)) return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, tile);
	/* A taxi window cannot precede, bypass, or invent its virtual route. Direct
	 * ground acquisition (landing/building) has no planned route yet. */
	if (!v->planned_path.empty() && (travel == TRACKDIR_BIT_NONE || TrackdirBitsToTrackBits(travel) != tracks ||
			(AircraftOwnedVirtualTrackdirs(v, tile) & travel) != travel)) {
		LogAircraftReservationState(v, "real-without-virtual", tile);
		return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, tile);
	}
	/* Existing committed tracks remain usable when another aircraft phases past. */
	if ((held & tracks) == tracks) return true;
	if (IsApron(tile) && !IsAircraftTerminalAvailable(v, tile)) return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, tile);
	if (!RecordAircraftTaxiBlock(v, CheckAircraftTaxiTracks(v, tile, tracks, HasBit(v->flags, VAF_TAXI_PHASING)), tile)) return false;
	for (auto &r : v->reserved_path) {
		if (r.tile != tile) continue;
		r.tracks |= tracks;
		_aircraft_tile_owners[tile][v->index] = r.tracks;
		SetAirportTracksReservation(tile, tracks);
		return true;
	}
	if (v->reserved_path.size() >= 2) return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, tile);
	v->reserved_path.push_back({tile, tracks});
	_aircraft_tile_owners[tile][v->index] = tracks;
	SetAirportTracksReservation(tile, tracks);
	fprintf(stderr, "[AIR-RES] acquire veh=%u tile=%u tracks=%02x phase=%u owners=%zu\n", v->index.base(), tile.base(), (uint)tracks,
		(uint)HasBit(v->flags, VAF_TAXI_PHASING), _aircraft_tile_owners[tile].size());
	return true;
}

void ReleaseAircraftTile(Aircraft *v, TileIndex tile, TrackBits tracks)
{
	for (auto it = v->reserved_path.begin(); it != v->reserved_path.end(); ++it) {
		if (it->tile != tile) continue;
		if (v->ignore_airport_reservations) {
			it->tracks &= ~tracks;
			if (it->tracks == TRACK_BIT_NONE) v->reserved_path.erase(it);
			return;
		}
		auto owners = _aircraft_tile_owners.find(tile);
		if (owners == _aircraft_tile_owners.end() || owners->second.find(v->index) == owners->second.end() || owners->second.at(v->index) != it->tracks) {
			LogAircraftReservationState(v, "release-stale-record", tile);
			v->reserved_path.erase(it);
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
		fprintf(stderr, "[AIR-RES] release veh=%u tile=%u clear=%02x remaining=%02x\n", v->index.base(), tile.base(), (uint)remove, (uint)AircraftReservedTracks(tile));
		if (IsTaxiTile(tile)) {
			while (remove != TRACK_BIT_NONE) RemoveAirportTrackReservation(tile, RemoveFirstTrack(&remove));
		}
		return;
	}
}

static AircraftTaxiBlock CheckAircraftVirtualTile(const Aircraft *v, TileIndex tile, TrackdirBits want)
{
	if (!IsTaxiTile(tile) || want == TRACKDIR_BIT_NONE ||
			(GetAirportTileTracks(tile) & TrackdirBitsToTrackBits(want)) != TrackdirBitsToTrackBits(want)) return AircraftTaxiBlock::Protected;
	/* The runway lock governs real entry, not a future virtual route. */
	if (GetReservedAirportTracks(tile) != AircraftReservedTracks(tile)) return AircraftTaxiBlock::Protected;
	for (Track track : SetTrackBitIterator(TrackdirBitsToTrackBits(want))) {
		if (IsDiagonalTrack(track)) continue;
		TileIndex neighbour = TileAddByDir(tile, _aircraft_envelope_directions[track]);
		if (!IsTaxiTile(neighbour) || !IsAirportTileOfStation(neighbour, GetStationIndex(tile)) || IsHangar(neighbour)) return AircraftTaxiBlock::Protected;
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
		if ((others & TrackdirToTrackdirBits(ReverseTrackdir(td))) != TRACKDIR_BIT_NONE) return AircraftTaxiBlock::Traffic;
	}
	return AircraftTaxiBlock::None;
}

bool IsAircraftVirtualTileAvailable(const Aircraft *v, TileIndex tile, Trackdir td)
{
	return IsTaxiTile(v->tile) && IsTaxiTile(tile) && IsAirportTileOfStation(tile, GetStationIndex(v->tile)) &&
			IsValidTrackdir(td) && CheckAircraftVirtualTile(v, tile, TrackdirToTrackdirBits(td)) == AircraftTaxiBlock::None;
}

bool IsAircraftVirtualDestinationAvailable(const Aircraft *v, TileIndex tile)
{
	return CheckAircraftTaxiTile(v, tile, false, false) == AircraftTaxiBlock::None &&
			CheckIncomingAircraftEnvelopes(v, tile, false, TRACK_BIT_CROSS) == AircraftTaxiBlock::None;
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
	if (!v->virtual_path.empty()) fprintf(stderr, "[AIR-RES] virtual-release veh=%u tiles=%zu\n", v->index.base(), v->virtual_path.size());
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
			fprintf(stderr, "[AIR-RES] virtual-denied veh=%u tile=%u want=%04x block=%u route=%zu\n",
				v->index.base(), tile.base(), (uint)trackdirs, (uint)block, desired.size());
			return RecordAircraftTaxiBlock(v, block, tile);
		}
	}
	const auto &destination = v->planned_path.back();
	/* A shared virtual route never grants permission to stop on a real occupant. */
	if (!RecordAircraftTaxiBlock(v, MergeAircraftBlock(CheckAircraftTaxiTile(v, destination.tile, false, false),
			CheckIncomingAircraftEnvelopes(v, destination.tile, false, TRACK_BIT_CROSS)), destination.tile)) return false;
	if (IsApron(destination.tile) && !IsAircraftTerminalAvailable(v, destination.tile)) return RecordAircraftTaxiBlock(v, AircraftTaxiBlock::Protected, destination.tile);

	ReleaseAircraftVirtualPath(v);
	for (const auto &[tile, trackdirs] : desired) {
		v->virtual_path.push_back({tile, trackdirs});
		if (!v->ignore_airport_reservations) _aircraft_virtual_bookings[tile][v->index] = trackdirs;
		if (_show_airport_tracks) MarkTileDirtyByTile(tile);
	}
	fprintf(stderr, "[AIR-RES] virtual-acquired veh=%u from=%u to=%u tiles=%zu nodes=%zu\n",
		v->index.base(), v->tile.base(), destination.tile.base(), desired.size(), v->planned_path.size());
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
		fprintf(stderr, "[AIR-RES] virtual-prune veh=%u tile=%u before=%04x remaining=%04x\n",
			v->index.base(), it->tile.base(), (uint)it->trackdirs, (uint)keep);
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
				LogAircraftReservationState(v, "taxi-phase-end");
			}
		}
		return;
	}
	const bool eligible = !v->vehstatus.Any({VehState::Stopped, VehState::Hidden, VehState::Crashed}) &&
			(v->state == AS_IDLE || v->state == AS_RUNNING) && !v->current_order.IsType(OT_LOADING) && !v->current_order.IsType(OT_WAITING) &&
			v->next_trackdir == INVALID_TRACKDIR;
	if (eligible && v->taxi_block_reason == AircraftTaxiBlock::Traffic) {
		if (v->taxi_jam_ticks < UINT16_MAX) ++v->taxi_jam_ticks;
		if (v->taxi_jam_ticks > 1480 && !HasBit(v->flags, VAF_TAXI_PHASING)) {
			SetBit(v->flags, VAF_TAXI_PHASING);
			v->ClearWaitTime();
			LogAircraftReservationState(v, "taxi-phase-start");
		}
	} else {
		v->taxi_jam_ticks = 0;
	}
}


void ReleaseAircraftTaxiPath(Aircraft *v, bool keep_current)
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
	TrackBits previous = AircraftOwnedTracks(v, tile);
	if (!TryReserveAircraftTile(v, tile, current, current_tds)) return false;
	if (next == INVALID_TILE || TryReserveAircraftTile(v, next, ahead, ahead_tds)) return true;
	TrackBits added = AircraftOwnedTracks(v, tile) & ~previous;
	if (added != TRACK_BIT_NONE) {
		fprintf(stderr, "[AIR-RES] window-rollback veh=%u tile=%u added=%02x retained=%02x next=%u\n",
			v->index.base(), tile.base(), (uint)added, (uint)previous, next.base());
		ReleaseAircraftTile(v, tile, added);
	}
	return false;
}

static std::vector<TileIndex> RunwayTiles(TileIndex extreme)
{
	std::vector<TileIndex> result;
	if (!IsTaxiTile(extreme) || !IsRunwayExtreme(extreme)) return result;
	const StationID station = GetStationIndex(extreme);
	DiagDirection dir = GetRunwayExtremeDirection(extreme);
	if (IsRunwayEnd(extreme)) dir = ReverseDiagDir(dir);
	for (TileIndex tile = extreme; IsTaxiTile(tile) && IsAirportTileOfStation(tile, station) && IsRunway(tile);
			tile = TileAdd(tile, TileOffsByDiagDir(dir))) {
		result.push_back(tile);
		if (tile != extreme && IsRunwayExtreme(tile)) return result;
	}
	return {};
}

bool TryReserveAircraftRunway(Aircraft *v, TileIndex extreme)
{
	const auto tiles = RunwayTiles(extreme);
	if (tiles.empty()) return false;
	if (v->ignore_airport_reservations) {
		ReleaseAircraftTaxiPath(v, false);
		v->reserved_runway = extreme;
		LogAircraftReservationState(v, "runway-ignore-acquired", extreme);
		return true;
	}
	for (TileIndex tile : tiles) {
		/* Virtual taxi routes do not block takeoff/landing. The complete runway
		 * lock still prevents their real windows from entering until released. */
		if (CheckAircraftTaxiTracks(v, tile, TRACK_BIT_NONE, false) != AircraftTaxiBlock::None) {
			LogAircraftReservationState(v, "runway-real-occupancy-blocked", tile);
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
	ReleaseAircraftTaxiPath(v, false);
	v->reserved_runway = extreme;
	for (TileIndex tile : tiles) {
		_aircraft_runway_owners[tile] = v->index;
		SetReservationAsRunway(tile, true);
		if (_show_airport_tracks) MarkTileDirtyByTile(tile);
	}
	LogAircraftReservationState(v, "runway-acquired", extreme);
	return true;
}

void ReleaseAircraftRunway(Aircraft *v)
{
	if (v->reserved_runway == INVALID_TILE) return;
	if (v->ignore_airport_reservations) { v->reserved_runway = INVALID_TILE; return; }
	const auto endpoint = v->reserved_runway;
	const auto tiles = RunwayTiles(endpoint);
	LogAircraftReservationState(v, "runway-release-begin", endpoint);
	if (tiles.empty()) {
		/* The owning airport command may already have removed the layout.
		 * Clear only this aircraft's index records, not another owner's tiles. */
		for (auto it = _aircraft_runway_owners.begin(); it != _aircraft_runway_owners.end();) {
			if (it->second != v->index) { ++it; continue; }
			if (IsTaxiTile(it->first) && IsRunway(it->first)) {
				SetReservationAsRunway(it->first, false);
				if (_show_airport_tracks) MarkTileDirtyByTile(it->first);
			}
			it = _aircraft_runway_owners.erase(it);
		}
	} else {
		for (TileIndex tile : tiles) {
			auto it = _aircraft_runway_owners.find(tile);
			if (it == _aircraft_runway_owners.end() || it->second != v->index) {
				LogAircraftReservationState(v, "runway-release-owner-mismatch", tile);
				continue;
			}
			fprintf(stderr, "[AIR-RES] runway-release veh=%u tile=%u\n", v->index.base(), tile.base());
			SetReservationAsRunway(tile, false);
			if (_show_airport_tracks) MarkTileDirtyByTile(tile);
			_aircraft_runway_owners.erase(it);
		}
	}
	v->reserved_runway = INVALID_TILE;
}

bool IsAircraftGroundTileAvailable(const Aircraft *v, TileIndex tile)
{
	return CheckAircraftTaxiTracks(v, tile, TRACK_BIT_NONE, false) == AircraftTaxiBlock::None;
}

bool IsAircraftRunwayAvailable(const Aircraft *v, TileIndex extreme)
{
	const auto tiles = RunwayTiles(extreme);
	if (tiles.empty()) return false;
	for (TileIndex tile : tiles) { if (!IsAircraftGroundTileAvailable(v, tile)) return false; }
	return true;
}

void EnableAircraftIgnoreReservations(Aircraft *v)
{
	if (v->ignore_airport_reservations) return;
	/* Retain movement records, but remove only this aircraft's public ownership.
	 * Other aircraft's map bits and runway locks must never be cleared. */
	auto real = v->reserved_path;
	auto virtuals = v->virtual_path;
	TileIndex gate = v->allocated_terminal;
	TileIndex runway = v->reserved_runway;
	for (const auto &record : real) ReleaseAircraftTile(v, record.tile, record.tracks);
	ReleaseAircraftVirtualPath(v);
	ReleaseAircraftTerminal(v);
	ReleaseAircraftRunway(v);
	v->reserved_path = std::move(real);
	v->virtual_path = std::move(virtuals);
	v->allocated_terminal = gate;
	v->reserved_runway = runway;
	/* An ignored aircraft keeps its local movement records but has no public
	 * real, virtual, terminal or runway owner entries. */
	v->ignore_airport_reservations = true;
	v->taxi_block_reason = AircraftTaxiBlock::None;
	v->taxi_blocked_ticks = v->taxi_jam_ticks = 0;
	v->ClearWaitTime();
	LogAircraftReservationState(v, "manual-ignore-enabled");
}

void ResetAircraftIgnoreReservations(Aircraft *v)
{
	if (!v->ignore_airport_reservations) return;
	ReleaseAircraftTaxiPath(v, false);
	ReleaseAircraftRunway(v);
	v->ignore_airport_reservations = false;
	InvalidateWindowData(WindowClass::VehicleView, v->index);
	LogAircraftReservationState(v, "manual-ignore-takeoff-reset");
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

/* Validate without changing the saved simulation state; rebuild indexes only. */
void AfterLoadAircraftReservations(bool migrate, bool migrate_virtual)
{
	ClearAircraftReservationIndexes();
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
			v->reserved_runway = INVALID_TILE;
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
					fprintf(stderr, "[AIR-RES] migrate-runway-skip veh=%u state=%u tile=%u endpoint=%u\n",
						v->index.base(), (uint)v->state, v->tile.base(), extreme.base());
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
					fprintf(stderr, "[AIR-RES] migrate-gate-skip veh=%u tile=%u\n", v->index.base(), v->tile.base());
				}
			} else {
				fprintf(stderr, "[AIR-RES] migrate-taxi-skip veh=%u state=%u tile=%u td=%u\n",
					v->index.base(), (uint)v->state, v->tile.base(), (uint)td);
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
		return;
	}

	for (Aircraft *v : Aircraft::Iterate()) {
		if (!v->IsNormalAircraft()) continue;
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
				if (!v->ignore_airport_reservations && (!GetReservationAsRunway(tile) || !_aircraft_runway_owners.emplace(tile, v->index).second)) SlErrorCorrupt("Invalid saved aircraft runway reservation");
			}
		}
		if (HasBit(v->flags, VAF_PUSHBACK) && (v->state != AS_RUNNING ||
				!IsTaxiTile(v->pushback_origin) || !IsTaxiTile(v->pushback_target) || v->planned_path.empty())) SlErrorCorrupt("Invalid saved aircraft pushback");
		if (v->state == AS_RUNNING && (v->planned_path.empty() || v->planned_path.front().tile != v->tile)) SlErrorCorrupt("Aircraft route does not start at its current tile");
		if (v->state == AS_RUNNING && !HasAircraftTileReservation(v, v->tile, TRACK_BIT_NONE)) SlErrorCorrupt("Taxiing aircraft has no current tile reservation");
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
		if (taxi != _aircraft_tile_owners.end() && (taxi->second.size() != 1 || taxi->second.begin()->first != runway_owner)) SlErrorCorrupt("Aircraft taxi reservation overlaps another aircraft runway");
	}
	if (migrate_virtual) {
		/* v1-v3 do not record edge-turn swept directions. Convert once;
		 * conflicting old plans wait on their actual tile rather than refusing
		 * the save or taking a partial prefix of another aircraft's route. */
		for (Aircraft *v : Aircraft::Iterate()) {
			if (!v->IsNormalAircraft()) continue;
			v->virtual_path.clear();
			if (SlXvIsFeatureMissing(XSLFI_AIRCRAFT_ROLLING_RESERVATION, 2)) {
				v->taxi_jam_ticks = 0;
				v->taxi_block_reason = AircraftTaxiBlock::None;
				ClrBit(v->flags, VAF_TAXI_PHASING);
			}
			if (v->state == AS_RUNNING && TryReserveAircraftTaxiPath(v)) continue;
			if (v->planned_path.empty()) continue;
			fprintf(stderr, "[AIR-RES] migrate-virtual-wait veh=%u tile=%u nodes=%zu\n", v->index.base(), v->tile.base(), v->planned_path.size());
			ReleaseAircraftTaxiPath(v, !v->vehstatus.Test(VehState::Hidden));
			if (v->state != AS_RUNNING) continue;
			v->state = AS_IDLE;
			v->cur_speed = 0;
			v->taxi_progress = v->pushback_progress = 0;
			v->next_pos.pos = AP_DEFAULT;
			v->UpdateNextTile(INVALID_TILE);
			v->ClearWaitTime();
		}
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
}