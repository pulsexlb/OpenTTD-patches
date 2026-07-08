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
#include "order_type.h"

#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

struct Station;
struct Vehicle;

/**
 * A single "start crossing" record: the first moment the train entered the
 * catchment area of a (train) station during travel from one order to the next.
 *
 * The same station may produce multiple records if the train re-enters its
 * catchment later. travel_time is the number of ticks elapsed since the segment's
 * starting order was reached (i.e. time taken to get here from that order), directly
 * usable as a timetable travel-time value for autofill.
 */
struct StationCrossingEntry {
	StateTicks tick = StateTicks{0};   ///< StateTicks at the moment of entering (debug/reference).
	Ticks      travel_time = 0;         ///< Ticks from the segment's starting order to this crossing.
	TileIndex  tile = INVALID_TILE;     ///< Tile that triggered the record.
};

/** Per-vehicle crossings: keyed by the order index the segment starts from. */
using CrossingMap = std::map<VehicleOrderID, std::vector<std::pair<StationID, StationCrossingEntry>>>;

/** In-progress recording for the current segment. */
struct StationCrossingRecording {
	VehicleOrderID from_order = INVALID_VEH_ORDER_ID;
	StateTicks start_tick = StateTicks{0};
	std::vector<std::pair<StationID, StationCrossingEntry>> buf;
};

/**
 * Global station-crossing tracker, segmented by order index.
 *
 * - Tick() is called every game frame and detects, for every moving train, which
 *   train-station catchments it has just entered. Crossings are grouped by the
 *   order the train is travelling FROM: _vehicle_crossings[vid][from_order] holds
 *   every station passed between from_order and the next order. The current target
 *   station and the last visited station are excluded so reports reflect genuine
 *   cross-station behavior.
 * - When the train advances to a new order, the in-progress recording is committed
 *   under its from_order key, and a fresh segment opens from the new order.
 * - Order list edits (insert / delete / move / clear) purge the affected segments;
 *   see OnOrderInserted / OnOrderDeleted / OnOrderMoved.
 * - Station build / update is perceived automatically every frame (ForAllStationsAroundTiles
 *   recomputes coverage each call). Station removal is handled by OnStationRemoved.
 */
class StationCrossingTracker {
public:
	/// Called every game frame from the main loop.
	static void Tick();

	/// Called when a station is removed: purge all records referring to it.
	static void OnStationRemoved(StationID st_id);

	/// Called when a vehicle is removed: purge its crossing state.
	static void OnVehicleRemoved(VehicleID vid);

	/// Called after an order is deleted from a vehicle's order list.
	static void OnOrderDeleted(VehicleID vid, VehicleOrderID deleted_index);
	/// Called after an order is inserted into a vehicle's order list.
	static void OnOrderInserted(VehicleID vid, VehicleOrderID inserted_index);
	/// Called after an order is moved (treated as delete at 'from' then insert at 'to').
	static void OnOrderMoved(VehicleID vid, VehicleOrderID from, VehicleOrderID to);

	/// Get all crossing records for a vehicle, flattened with their from_order.
	static std::vector<std::tuple<VehicleOrderID, StationID, StationCrossingEntry>> GetVehicleCrossings(VehicleID vid);

	/// Clear all state.
	static void Clear();

private:
	/// Per vehicle: crossings keyed by the order index the segment starts from.
	static std::map<VehicleID, CrossingMap> _vehicle_crossings;
	/// Per vehicle: stations whose catchment currently contains the vehicle (for re-entry detection).
	static std::map<VehicleID, std::set<StationID>> _vehicle_inside;
	/// Per vehicle: in-progress segment (from_order + start tick + buffer).
	static std::map<VehicleID, StationCrossingRecording> _vehicle_recording;
	/// Per vehicle: previous tick's cur_real_order_index, for advance detection.
	static std::map<VehicleID, VehicleOrderID> _vehicle_prev_order;
	/// Open a fresh segment starting at the given order index.
	static void OpenSegment(Vehicle *v, VehicleOrderID from);
};

#endif /* STATION_CROSSING_H */
