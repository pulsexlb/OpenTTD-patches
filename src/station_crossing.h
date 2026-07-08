/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file station_crossing.h Tick-level "train entering station catchment" tracker (C++ side). */

#ifndef STATION_CROSSING_H
#define STATION_CROSSING_H

#include "date_type.h"
#include "station_type.h"
#include "vehicle_type.h"
#include "tile_type.h"

#include <map>
#include <set>
#include <utility>
#include <vector>

struct Station;
struct Vehicle;

/**
 * A single "start crossing" record: the StateTicks at which a train first entered
 * the catchment area of a (train) station during one continuous pass.
 *
 * The same station may produce multiple records if the train re-enters its
 * catchment later. Only the entering moment is recorded (not leaving).
 */
struct StationCrossingEntry {
	StateTicks tick = StateTicks{0};   ///< StateTicks at the moment of entering.
	TileIndex tile = INVALID_TILE;     ///< Tile that triggered the record.
};

/**
 * Global station-crossing tracker.
 *
 * - Tick() is called every game frame and detects, for every moving train, which
 *   train-station catchments it has just entered. Only the "start crossing" time
 *   is recorded, and each pass appends a new record so the same station can be
 *   observed multiple times per journey. The current target station and the last
 *   visited station are excluded so reports reflect genuine cross-station behavior.
 * - Station build / update is perceived automatically every frame (ForAllStationsAroundTiles
 *   recomputes coverage each call). Station removal is handled by OnStationRemoved.
 * - Times are recorded as StateTicks, the same time base as v->timetable_start and the
 *   dispatch schedule's start tick, so the C++ side can compute
 *   offset = (tick - schedule_start).AsTicks() which is directly insertable into the
 *   dispatch schedule's timetable.
 */
class StationCrossingTracker {
public:
	/// Called every game frame from the main loop.
	static void Tick();

	/// Called when a station is removed: purge all records referring to it.
	static void OnStationRemoved(StationID st_id);

	/// Called when a vehicle is removed: purge its crossing state.
	static void OnVehicleRemoved(VehicleID vid);

	/// Get all crossing records for a vehicle. Each same station may appear multiple times.
	static std::vector<std::pair<StationID, StationCrossingEntry>> GetVehicleCrossings(VehicleID vid);

	/// Clear all state.
	static void Clear();

private:
	/// Per vehicle: ordered crossing entries, one record per crossing event.
	static std::map<VehicleID, std::vector<std::pair<StationID, StationCrossingEntry>>> _vehicle_crossings;
	/// Per vehicle: stations whose catchment currently contains the vehicle (for re-entry detection).
	static std::map<VehicleID, std::set<StationID>> _vehicle_inside;
};

#endif /* STATION_CROSSING_H */
