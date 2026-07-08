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

std::map<VehicleID, CrossingMap> StationCrossingTracker::_vehicle_crossings;
std::map<VehicleID, std::set<StationID>> StationCrossingTracker::_vehicle_inside;
std::map<VehicleID, StationCrossingRecording> StationCrossingTracker::_vehicle_recording;
std::map<VehicleID, VehicleOrderID> StationCrossingTracker::_vehicle_prev_order;

/**
 * Open a fresh segment starting at the given order index.
 */
/* static */ void StationCrossingTracker::OpenSegment(Vehicle *v, VehicleOrderID from)
{
	StationCrossingRecording &rec = _vehicle_recording[v->index];
	rec = StationCrossingRecording{};
	rec.from_order = from;
	rec.start_tick = _state_ticks;
}
/* static */ void StationCrossingTracker::Tick()
{
	for (Vehicle *v : Vehicle::Iterate()) {
		if (v->type != VehicleType::Train || !v->IsPrimaryVehicle()) continue;
		if (v->cur_speed == 0) continue; // Only trains that are actually moving.
		if (v->tile == INVALID_TILE) continue;
		const VehicleOrderID n = v->GetNumOrders();
		const VehicleOrderID cur = v->cur_real_order_index;
		/* Detect order advance: train reached a new order (forward, backward via
		 * conditional, or cyclic wrap). Any change in cur_real_order_index is a
		 * boundary. */
		auto prev_it = _vehicle_prev_order.find(v->index);
		bool advanced = false;
		if (prev_it == _vehicle_prev_order.end()) {
			advanced = true; // First sight of this vehicle.
		} else if (n > 0 && cur != prev_it->second) {
			advanced = true;
		}
		_vehicle_prev_order[v->index] = cur;

		StationCrossingRecording &rec = _vehicle_recording[v->index];

		if (advanced && n > 0) {
			/* Commit the just-finished segment under its from_order key. The full
			 * crossing set for this traversal replaces any previous recording for the
			 * same segment, so re-running the loop updates the record instead of
			 * duplicating it (per from_order, each station keeps only its latest). */
			if (rec.from_order != INVALID_VEH_ORDER_ID) {
				CrossingMap &cm = _vehicle_crossings[v->index];
				cm[rec.from_order] = std::move(rec.buf);
			}
			OpenSegment(v, cur);
		} else if (rec.from_order == INVALID_VEH_ORDER_ID && n > 0) {
			OpenSegment(v, cur);
		}

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

		auto &inside = _vehicle_inside[v->index];

		/* Newly entered stations: record "start crossing" with travel time from segment start. */
		for (StationID sid : now_inside) {
			if (inside.find(sid) == inside.end()) {
				Ticks travel = static_cast<Ticks>((_state_ticks - rec.start_tick).AsTicks());
				rec.buf.emplace_back(sid, StationCrossingEntry{_state_ticks, travel, v->tile});
			}
		}

		inside = now_inside;
	}
}

/* static */ void StationCrossingTracker::OnStationRemoved(StationID st_id)
{
	for (auto &kv : _vehicle_crossings) {
		for (auto &seg : kv.second) {
			auto &vec = seg.second;
			vec.erase(std::remove_if(vec.begin(), vec.end(), [st_id](const auto &p) { return p.first == st_id; }), vec.end());
		}
	}
	for (auto &kv : _vehicle_recording) {
		auto &vec = kv.second.buf;
		vec.erase(std::remove_if(vec.begin(), vec.end(), [st_id](const auto &p) { return p.first == st_id; }), vec.end());
	}
	for (auto &kv : _vehicle_inside) kv.second.erase(st_id);
}

/* static */ void StationCrossingTracker::OnVehicleRemoved(VehicleID vid)
{
	_vehicle_crossings.erase(vid);
	_vehicle_inside.erase(vid);
	_vehicle_recording.erase(vid);
	_vehicle_prev_order.erase(vid);
}

/**
 * Remove the keyed segment and shift all keys strictly greater than it down by one.
 */
static void ShiftSegmentsDown(std::map<VehicleID, CrossingMap> &map, VehicleID vid, VehicleOrderID removed)
{
	auto it = map.find(vid);
	if (it == map.end()) return;
	CrossingMap &cm = it->second;
	cm.erase(removed);
	CrossingMap rebuilt;
	for (auto &kv : cm) {
		VehicleOrderID k = kv.first;
		if (k > removed) k = static_cast<VehicleOrderID>(k - 1);
		rebuilt[k].insert(rebuilt[k].end(), kv.second.begin(), kv.second.end());
	}
	cm = std::move(rebuilt);
}

/**
 * Shift all keys >= inserted up by one (making room for the new order index).
 */
static void ShiftSegmentsUp(std::map<VehicleID, CrossingMap> &map, VehicleID vid, VehicleOrderID inserted)
{
	auto it = map.find(vid);
	if (it == map.end()) return;
	CrossingMap &cm = it->second;
	CrossingMap rebuilt;
	for (auto &kv : cm) {
		VehicleOrderID k = kv.first;
		if (k >= inserted) k = static_cast<VehicleOrderID>(k + 1);
		rebuilt[k].insert(rebuilt[k].end(), kv.second.begin(), kv.second.end());
	}
	cm = std::move(rebuilt);
}

/* static */ void StationCrossingTracker::OnOrderDeleted(VehicleID vid, VehicleOrderID deleted_index)
{
	for (Vehicle *u = Vehicle::GetIfValid(vid); u != nullptr; u = u->NextShared()) {
		VehicleID id = u->index;
		auto it = _vehicle_crossings.find(id);
		if (it != _vehicle_crossings.end()) {
			if (deleted_index > 0) it->second.erase(static_cast<VehicleOrderID>(deleted_index - 1));
			it->second.erase(deleted_index);
		}
		ShiftSegmentsDown(_vehicle_crossings, id, deleted_index);

		auto rit = _vehicle_recording.find(id);
		if (rit != _vehicle_recording.end()) {
			if (rit->second.from_order == deleted_index ||
					(deleted_index > 0 && rit->second.from_order == static_cast<VehicleOrderID>(deleted_index - 1))) {
				rit->second = StationCrossingRecording{};
			} else if (rit->second.from_order > deleted_index) {
				rit->second.from_order = static_cast<VehicleOrderID>(rit->second.from_order - 1);
			}
		}
		auto pit = _vehicle_prev_order.find(id);
		if (pit != _vehicle_prev_order.end() && pit->second > deleted_index) {
			pit->second = static_cast<VehicleOrderID>(pit->second - 1);
		}
	}
}

/* static */ void StationCrossingTracker::OnOrderInserted(VehicleID vid, VehicleOrderID inserted_index)
{
	for (Vehicle *u = Vehicle::GetIfValid(vid); u != nullptr; u = u->NextShared()) {
		VehicleID id = u->index;
		ShiftSegmentsUp(_vehicle_crossings, id, inserted_index);
		auto rit = _vehicle_recording.find(id);
		if (rit != _vehicle_recording.end() && rit->second.from_order != INVALID_VEH_ORDER_ID && rit->second.from_order >= inserted_index) {
			rit->second.from_order = static_cast<VehicleOrderID>(rit->second.from_order + 1);
		}
		auto pit = _vehicle_prev_order.find(id);
		if (pit != _vehicle_prev_order.end() && pit->second >= inserted_index) {
			pit->second = static_cast<VehicleOrderID>(pit->second + 1);
		}
	}
}

/* static */ void StationCrossingTracker::OnOrderMoved(VehicleID vid, VehicleOrderID from, VehicleOrderID to)
{
	OnOrderDeleted(vid, from);
	OnOrderInserted(vid, to);
}

/* static */ std::vector<std::tuple<VehicleOrderID, StationID, StationCrossingEntry>> StationCrossingTracker::GetVehicleCrossings(VehicleID vid)
{
	std::vector<std::tuple<VehicleOrderID, StationID, StationCrossingEntry>> result;
	/* Only the committed segments are returned; the in-progress recording for the
	 * current order segment is intentionally excluded until the train advances. */
	auto it = _vehicle_crossings.find(vid);
	if (it != _vehicle_crossings.end()) {
		for (auto &seg : it->second) {
			for (auto &p : seg.second) {
				result.emplace_back(seg.first, p.first, p.second);
			}
		}
	}
	return result;
}

/* static */ void StationCrossingTracker::Clear()
{
	_vehicle_crossings.clear();
	_vehicle_inside.clear();
	_vehicle_recording.clear();
	_vehicle_prev_order.clear();
}
