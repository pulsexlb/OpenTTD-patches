/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file pbs_air.h PBS support routines for aircraft crossing tiles of an airport or just flying. */

#ifndef PBS_AIR_H
#define PBS_AIR_H

#include "track_type.h"
#include "tile_type.h"

struct Aircraft;

bool IsAircraftVirtualTileAvailable(const Aircraft *v, TileIndex tile, Trackdir td);
bool IsAircraftVirtualDestinationAvailable(const Aircraft *v, TileIndex tile);
bool HasAircraftVirtualTileReservation(const Aircraft *v, TileIndex tile, TrackdirBits travel);
bool TryReserveAircraftTaxiPath(Aircraft *v);
void PruneAircraftVirtualReservations(Aircraft *v);
TrackBits GetAircraftVirtualBookedTracks(TileIndex tile);
TrackBits GetAircraftAirportEditTracks(TileIndex tile);
void UpdateAircraftTaxiTraffic(Aircraft *v, bool moved);
bool IsAircraftTaxiEnvelopeAvailable(Aircraft *v, TileIndex tile);

void EnableAircraftIgnoreReservations(Aircraft *v);
void ResetAircraftIgnoreReservations(Aircraft *v);
bool IsAircraftGroundTileAvailable(const Aircraft *v, TileIndex tile);
bool IsAircraftRunwayAvailable(const Aircraft *v, TileIndex extreme);
Trackdir GetFreeAirportTrackdir(TileIndex tile, Trackdir preferred_trackdir, const Aircraft *v = nullptr);
void LiftAirportPathReservation(Aircraft *v, bool skip_first_track);
bool IsAirportTileFree(TileIndex tile, Trackdir trackdir);
bool IsAircraftTerminalAvailable(const Aircraft *v, TileIndex tile);
bool TryClaimAircraftTerminal(Aircraft *v, TileIndex tile);
void ReleaseAircraftTerminal(Aircraft *v);
bool HasAircraftTileReservation(const Aircraft *v, TileIndex tile, TrackBits tracks);
bool TryReserveAircraftTile(Aircraft *v, TileIndex tile, TrackBits tracks, TrackdirBits travel = TRACKDIR_BIT_NONE);
void ReleaseAircraftTile(Aircraft *v, TileIndex tile, TrackBits tracks);
void ReleaseAircraftTaxiPath(Aircraft *v, bool keep_current);
bool ExtendAircraftTaxiReservation(Aircraft *v);
bool EnsureAircraftTaxiRunways(Aircraft *v);
bool TryReserveAircraftRunway(Aircraft *v, TileIndex extreme);
void ReleaseAircraftRunway(Aircraft *v);
void AfterLoadAircraftReservations(bool migrate, bool migrate_virtual = false);
void ClearAircraftReservationIndexes();

#endif /* PBS_AIR_H */
