/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file station_crossing.cpp Tick-level "train entering station catchment" tracker. */

#include "stdafx.h"

#include "station_crossing.h"

#include "station_base.h"
#include "vehicle_base.h"
#include "tilearea_type.h"
#include "map_func.h"
#include "date_func.h"
#include "core/pool_func.hpp"

#include "safeguards.h"

std::map<VehicleID, std::vector<std::pair<StationID, StationCrossingEntry>>> StationCrossingTracker::_vehicle_crossings;
std::map<VehicleID, std::set<StationID>> StationCrossingTracker::_vehicle_inside;

/* static */ void StationCrossingTracker::Tick()
{
	for (Vehicle *v : Vehicle::Iterate()) {
		if (v->type != VehicleType::Train || !v->IsPrimaryVehicle()) continue;
		if (v->cur_speed == 0) continue; // Only trains that are actually moving.
		if (v->tile == INVALID_TILE) continue;

		/* Compute the set of train stations whose catchment currently covers the
		 * vehicle tile. ForAllStationsAroundTiles recomputes coverage every call, so
		 * station build / update / removal is automatically perceived here. */
		std::set<StationID> now_inside;
		ForAllStationsAroundTiles(TileArea(v->tile, 1, 1), [&](Station *st, TileIndex) {
			if (!st->facilities.Test(StationFacility::Train)) return false;
			now_inside.insert(st->index);
			return false; // Keep checking all covering stations.
		});

		/* Exclude the current target station and the last visited station, so only
		 * true cross-station detections remain. */
		std::set<StationID> exclude;
		if (const Order *current_order = v->GetOrder(v->cur_real_order_index);
				current_order != nullptr && current_order->IsType(OT_GOTO_STATION)) {
			exclude.insert(current_order->GetDestination().ToStationID());
		}
		if (v->last_station_visited != StationID::Invalid()) exclude.insert(v->last_station_visited);
		for (StationID sid : exclude) now_inside.erase(sid);

		auto &recs = _vehicle_crossings[v->index];
		auto &inside = _vehicle_inside[v->index];

		/* Newly entered stations: record "start crossing". */
		for (StationID sid : now_inside) {
			if (inside.find(sid) == inside.end()) {
				recs.emplace_back(sid, StationCrossingEntry{_state_ticks, v->tile});
			}
		}

		inside = now_inside;
	}
}

/* static */ void StationCrossingTracker::OnStationRemoved(StationID st_id)
{
	for (auto &kv : _vehicle_crossings) {
		auto &vec = kv.second;
		vec.erase(std::remove_if(vec.begin(), vec.end(), [st_id](const auto &p) { return p.first == st_id; }), vec.end());
	}
	for (auto &kv : _vehicle_inside) kv.second.erase(st_id);
}

/* static */ void StationCrossingTracker::OnVehicleRemoved(VehicleID vid)
{
	_vehicle_crossings.erase(vid);
	_vehicle_inside.erase(vid);
}

/* static */ std::vector<std::pair<StationID, StationCrossingEntry>> StationCrossingTracker::GetVehicleCrossings(VehicleID vid)
{
	auto it = _vehicle_crossings.find(vid);
	if (it == _vehicle_crossings.end()) return {};
	return std::vector<std::pair<StationID, StationCrossingEntry>>(it->second.begin(), it->second.end());
}

/* static */ void StationCrossingTracker::Clear()
{
	_vehicle_crossings.clear();
	_vehicle_inside.clear();
}
