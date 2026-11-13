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
#include "pathfinder/follow_track.hpp"
#include "sl/saveload.h"
#include <map>

#include "safeguards.h"

/* Derived lookup indexes. The authoritative records live in Aircraft and are saved. */
static std::map<TileIndex, VehicleID> _aircraft_tile_owners;
static std::map<TileIndex, VehicleID> _aircraft_terminal_owners;
static std::map<TileIndex, VehicleID> _aircraft_runway_owners;

static bool IsTaxiTile(TileIndex tile)
{
	return IsValidTile(tile) && IsAirportTile(tile) && MayHaveAirTracks(tile);
}

void ClearAircraftReservationIndexes()
{
	_aircraft_tile_owners.clear();
	_aircraft_terminal_owners.clear();
	_aircraft_runway_owners.clear();
}

bool IsAircraftTerminalAvailable(const Aircraft *v, TileIndex tile)
{
	auto it = _aircraft_terminal_owners.find(tile);
	if (it != _aircraft_terminal_owners.end() && it->second != v->index) return false;
	auto occupied = _aircraft_tile_owners.find(tile);
	if (occupied != _aircraft_tile_owners.end() && occupied->second != v->index) return false;
	return IsTaxiTile(tile);
}

bool TryClaimAircraftTerminal(Aircraft *v, TileIndex tile)
{
	if (!IsAircraftTerminalAvailable(v, tile)) {
		return false;
	}
	if (v->allocated_terminal != tile) ReleaseAircraftTerminal(v);
	_aircraft_terminal_owners[tile] = v->index;
	v->allocated_terminal = tile;
	return true;
}

void ReleaseAircraftTerminal(Aircraft *v)
{
	auto it = _aircraft_terminal_owners.find(v->allocated_terminal);
	if (it != _aircraft_terminal_owners.end() && it->second == v->index) _aircraft_terminal_owners.erase(it);
	v->allocated_terminal = INVALID_TILE;
}

bool HasAircraftTileReservation(const Aircraft *v, TileIndex tile, TrackBits tracks)
{
	for (const auto &r : v->reserved_path) {
		if (r.tile == tile) return (r.tracks & tracks) == tracks;
	}
	return false;
}

/* Half-track envelopes overlap the opposite half-track on an adjacent tile. */
static bool AssociatedTracksAvailable(const Aircraft *v, TileIndex tile, Track track)
{
	if (IsDiagonalTrack(track)) return true;
	static const Direction directions[TRACK_END] = {
		Direction::Invalid, Direction::Invalid, Direction::N, Direction::S, Direction::W, Direction::E,
	};
	TileIndex neighbour = TileAddByDir(tile, directions[track]);
	if (!IsTaxiTile(neighbour) || !IsAirportTileOfStation(neighbour, GetStationIndex(tile)) || IsHangar(neighbour)) return false;
	if (IsRunway(neighbour) && GetReservationAsRunway(neighbour)) {
		auto runway = _aircraft_runway_owners.find(neighbour);
		if (runway == _aircraft_runway_owners.end() || runway->second != v->index) return false;
	}
	auto owner = _aircraft_tile_owners.find(neighbour);
	if (owner != _aircraft_tile_owners.end() && owner->second == v->index) return true;
	return !HasAirportTrackReserved(neighbour, TrackToOppositeTrack(track));
}

bool TryReserveAircraftTile(Aircraft *v, TileIndex tile, TrackBits tracks)
{
	if (!IsTaxiTile(tile) || tracks == TRACK_BIT_NONE) return false;
	if ((GetAirportTileTracks(tile) & tracks) != tracks) {
		return false;
	}
	if (IsRunway(tile) && GetReservationAsRunway(tile)) {
		auto runway_owner = _aircraft_runway_owners.find(tile);
		if (runway_owner == _aircraft_runway_owners.end() || runway_owner->second != v->index) return false;
	}
	if (IsApron(tile) && !IsAircraftTerminalAvailable(v, tile)) return false;

	auto owner = _aircraft_tile_owners.find(tile);
	if (owner != _aircraft_tile_owners.end() && owner->second != v->index) {
		return false;
	}
	if (owner == _aircraft_tile_owners.end() && HasAirportTrackReserved(tile)) {
		return false;
	}
	TrackBits check = tracks;
	while (check != TRACK_BIT_NONE) {
		Track track = RemoveFirstTrack(&check);
		if (!AssociatedTracksAvailable(v, tile, track)) {
			return false;
		}
	}

	for (auto &r : v->reserved_path) {
		if (r.tile != tile) continue;
		r.tracks |= tracks;
		SetAirportTracksReservation(tile, tracks);
		return true;
	}
	if (v->reserved_path.size() >= 2) return false;
	v->reserved_path.push_back({tile, tracks});
	_aircraft_tile_owners[tile] = v->index;
	SetAirportTracksReservation(tile, tracks);
	return true;
}

void ReleaseAircraftTile(Aircraft *v, TileIndex tile, TrackBits tracks)
{
	for (auto it = v->reserved_path.begin(); it != v->reserved_path.end(); ++it) {
		if (it->tile != tile) continue;
		TrackBits remove = it->tracks & tracks;
		if (IsTaxiTile(tile)) {
			while (remove != TRACK_BIT_NONE) RemoveAirportTrackReservation(tile, RemoveFirstTrack(&remove));
		}
		it->tracks &= ~tracks;
		if (it->tracks == TRACK_BIT_NONE) {
			auto owner = _aircraft_tile_owners.find(tile);
			if (owner != _aircraft_tile_owners.end() && owner->second == v->index) _aircraft_tile_owners.erase(owner);
			v->reserved_path.erase(it);
		}
		return;
	}
}

void ReleaseAircraftTaxiPath(Aircraft *v, bool keep_current)
{
	for (size_t i = 0; i < v->reserved_path.size();) {
		const auto r = v->reserved_path[i];
		if (keep_current && r.tile == v->tile) { ++i; continue; }
		ReleaseAircraftTile(v, r.tile, r.tracks);
	}
	v->planned_path.clear();
	v->path.clear();
	v->taxi_blocked_ticks = 0;
	if (!keep_current || v->allocated_terminal != v->tile) ReleaseAircraftTerminal(v);
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
	size_t i = 0;
	while (i < v->planned_path.size() && v->planned_path[i].tile == tile) {
		current |= TrackToTrackBits(TrackdirToTrack(v->planned_path[i++].td));
	}
	if (!TryReserveAircraftTile(v, tile, current)) return false;
	if (i == v->planned_path.size()) return true;
	TileIndex next = v->planned_path[i].tile;
	TrackBits ahead = TRACK_BIT_NONE;
	while (i < v->planned_path.size() && v->planned_path[i].tile == next) {
		ahead |= TrackToTrackBits(TrackdirToTrack(v->planned_path[i++].td));
	}
	return TryReserveAircraftTile(v, next, ahead);
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
	for (TileIndex tile : tiles) {
		auto runway = _aircraft_runway_owners.find(tile);
		if (runway != _aircraft_runway_owners.end() && runway->second != v->index) return false;
		auto taxi = _aircraft_tile_owners.find(tile);
		if (taxi != _aircraft_tile_owners.end() && taxi->second != v->index) return false;
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
	return true;
}

void ReleaseAircraftRunway(Aircraft *v)
{
	if (v->reserved_runway == INVALID_TILE) return;
	const auto endpoint = v->reserved_runway;
	const auto tiles = RunwayTiles(endpoint);
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
				continue;
			}
			SetReservationAsRunway(tile, false);
			if (_show_airport_tracks) MarkTileDirtyByTile(tile);
			_aircraft_runway_owners.erase(it);
		}
	}
	v->reserved_runway = INVALID_TILE;
}

Trackdir GetFreeAirportTrackdir(TileIndex tile, Trackdir preferred_trackdir)
{
	if (!IsTaxiTile(tile) || !IsValidTrackdir(preferred_trackdir)) return INVALID_TRACKDIR;
	if (HasAirportTrackReserved(tile)) return INVALID_TRACKDIR;
	TrackBits tracks = GetAirportTileTracks(tile) & TRACK_BIT_CROSS;
	if (HasTrack(tracks, TrackdirToTrack(preferred_trackdir))) return preferred_trackdir;
	if (tracks == TRACK_BIT_NONE) return INVALID_TRACKDIR;
	Trackdir td = TrackToTrackdir(RemoveFirstTrack(&tracks));
	CFollowTrackAirport follower(INVALID_OWNER);
	return follower.Follow(tile, td) ? td : ReverseTrackdir(td);
}

/* Validate without changing the saved simulation state; rebuild indexes only. */
void AfterLoadAircraftReservations(bool migrate)
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
			v->planned_path.clear();
			v->path.clear();
			v->reserved_runway = INVALID_TILE;
			v->allocated_terminal = INVALID_TILE;
			v->pushback_origin = v->pushback_target = INVALID_TILE;
			ClrBit(v->flags, VAF_PUSHBACK);
			v->taxi_progress = v->pushback_progress = v->taxi_blocked_ticks = 0;
			const bool runway = v->state == AS_DESCENDING || v->state == AS_FLYING_LANDING ||
					v->state == AS_LANDED || v->state == AS_TAKEOFF_BEFORE_FLYING || v->state == AS_FLYING_TAKEOFF;
			if (runway && !v->IsHelicopter()) {
				TileIndex extreme = v->GetNextTile();
				if (!IsTaxiTile(extreme) || !IsRunwayExtreme(extreme)) SlErrorCorrupt("Aircraft runway endpoint missing during migration");
				if (!TryReserveAircraftRunway(v, extreme)) SlErrorCorrupt("Overlapping aircraft runway reservations during migration");
				continue;
			}
			/* A holding fixed-wing aircraft has not committed to landing. */
			if (v->IsAircraftFreelyFlying() || (!v->IsHelicopter() && v->state == AS_ON_HOLD_APPROACHING)) continue;
			if (v->vehstatus.Test(VehState::Hidden) || !IsTaxiTile(v->tile)) continue;
			Trackdir td = IsValidTrackdir(v->next_trackdir) ? v->next_trackdir : v->trackdir;
			if (!IsValidTrackdir(td)) SlErrorCorrupt("Invalid aircraft trackdir during migration");
			TrackBits tracks = TrackToTrackBits(TrackdirToTrack(td));
			if (!TryReserveAircraftTile(v, v->tile, tracks)) SlErrorCorrupt("Overlapping aircraft during reservation migration");
			if (IsApron(v->tile) && (IsTerminalState(v->state) || v->current_order.IsType(OT_LOADING))) {
				if (!TryClaimAircraftTerminal(v, v->tile)) SlErrorCorrupt("Overlapping aircraft terminal claims");
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
		for (const auto &r : v->reserved_path) {
			if (!IsTaxiTile(r.tile) || r.tracks == TRACK_BIT_NONE ||
					(GetAirportTileTracks(r.tile) & r.tracks) != r.tracks ||
					(GetReservedAirportTracks(r.tile) & r.tracks) != r.tracks ||
					!_aircraft_tile_owners.emplace(r.tile, v->index).second) {
				SlErrorCorrupt("Invalid or overlapping saved aircraft reservation");
			}
		}
		if (v->allocated_terminal != INVALID_TILE && (!IsTaxiTile(v->allocated_terminal) ||
				!_aircraft_terminal_owners.emplace(v->allocated_terminal, v->index).second)) SlErrorCorrupt("Invalid aircraft terminal claim");
		if (v->reserved_runway != INVALID_TILE) {
			const auto tiles = RunwayTiles(v->reserved_runway);
			if (tiles.empty()) SlErrorCorrupt("Invalid saved aircraft runway");
			for (TileIndex tile : tiles) {
				if (!GetReservationAsRunway(tile) || !_aircraft_runway_owners.emplace(tile, v->index).second) SlErrorCorrupt("Invalid saved aircraft runway reservation");
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
		if (occupied != _aircraft_tile_owners.end() && occupied->second != gate_owner) SlErrorCorrupt("Aircraft gate claim conflicts with occupant");
	}
	for (const auto &[tile, runway_owner] : _aircraft_runway_owners) {
		auto taxi = _aircraft_tile_owners.find(tile);
		if (taxi != _aircraft_tile_owners.end() && taxi->second != runway_owner) SlErrorCorrupt("Aircraft taxi reservation overlaps another aircraft runway");
	}
}
