/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint.cpp Copying infrastructure from the map into a blueprint, and pasting it back. */

#include "stdafx.h"
#include <map>
#include "air.h"
#include "airport_cmd.h"
#include "blueprint.h"
#include "blueprint_cmd.h"
#include "bridge_map.h"
#include "command_func.h"
#include "company_func.h"
#include "core/math_func.hpp"
#include "direction_func.h"
#include "landscape_cmd.h"
#include "newgrf_roadstop.h"
#include "newgrf_station.h"
#include "newgrf_newsignals.h"
#include "newgrf_object.h"
#include "object_base.h"
#include "object_map.h"
#include "object_cmd.h"
#include "rail_cmd.h"
#include "rail_map.h"
#include "road_cmd.h"
#include "road_func.h"
#include "road_map.h"
#include "settings_type.h"
#include "signal_func.h"
#include "slope_type.h"
#include "station_base.h"
#include "station_cmd.h"
#include "station_map.h"
#include "air_map.h"
#include "terraform_cmd.h"
#include "tile_map.h"
#include "track_func.h"
#include "vehicle_func.h"
#include "tunnel_map.h"
#include "tunnelbridge_cmd.h"
#include "tunnelbridge_map.h"
#include "water_cmd.h"
#include "water_map.h"
#include "window_type.h"
#include "window_func.h"
#include "waypoint_cmd.h"
#include "table/strings.h"

#include "safeguards.h"

/** Every blueprint slot the player can copy into; each page owns the #NUM_BLUEPRINT_SLOTS slots starting at its page number times #NUM_BLUEPRINT_SLOTS. */
static Blueprint _blueprints[NUM_BLUEPRINT_TOTAL_SLOTS];

/**
 * Get the blueprint stored in a slot.
 * @param slot Slot number (0-based), counted across all pages.
 * @return The blueprint of that slot.
 */
Blueprint &GetBlueprint(uint slot)
{
	assert(slot < NUM_BLUEPRINT_TOTAL_SLOTS);
	return _blueprints[slot];
}

/** Get the total number of copied elements. */
uint Blueprint::CountItems() const
{
	return static_cast<uint>(this->rail_tracks.size() + this->signals.size() + this->rail_depots.size()
			+ this->tunnel_bridges.size() + this->roads.size() + this->road_depots.size()
			+ this->canals.size() + this->locks.size() + this->ship_depots.size()
			+ this->station_tiles.size() + this->road_stops.size() + this->docks.size()
			+ this->buoys.size() + this->airports.size() + this->objects.size());
}

/** Remove all contents from this blueprint, keeping its name (see #CopyAreaToBlueprint). */
void Blueprint::Clear()
{
	std::string kept_name = std::move(this->name);
	*this = Blueprint{};
	this->name = std::move(kept_name);
}

/**
 * Get the offset of a tile relative to the blueprint origin.
 * @param tile Tile within the copied area.
 * @param origin Northern tile of the copied area.
 * @return Offset of \a tile.
 */
static TileIndexDiffC BlueprintOffset(TileIndex tile, TileIndex origin)
{
	return {static_cast<int16_t>(TileX(tile) - TileX(origin)), static_cast<int16_t>(TileY(tile) - TileY(origin))};
}

/**
 * Copy rail tracks and signals of a plain rail tile (or its depot).
 * @param blueprint Blueprint to copy into.
 * @param tile Tile to copy from.
 * @param offset Offset of \a tile relative to the blueprint origin.
 */
static void CopyRailTile(Blueprint &blueprint, TileIndex tile, TileIndexDiffC offset)
{
	if (IsRailDepot(tile)) {
		blueprint.rail_depots.push_back({offset, GetRailDepotDirection(tile), GetRailType(tile)});
		return;
	}

	for (Track track : SetTrackBitIterator(GetTrackBits(tile))) {
		blueprint.rail_tracks.push_back({offset, track, GetRailType(tile)});

		if (HasSignalOnTrack(tile, track)) {
			Trackdir td = TrackToTrackdir(track);
			BlueprintSpecRef spec{};
			uint8_t style = GetSignalStyle(tile, track);
			if (style != 0) {
				const auto &mapping = _new_signal_style_mapping[style - 1];
				spec = {mapping.grfid, mapping.grf_local_id};
			}
			blueprint.signals.push_back({offset, track, GetSignalType(tile, track), GetSignalVariant(tile, track),
					HasSignalOnTrackdir(tile, td), HasSignalOnTrackdir(tile, ReverseTrackdir(td)), spec});
		}
	}
}

/**
 * Copy the road and tram pieces of a tile that the local company owns.
 * @param blueprint Blueprint to copy into.
 * @param tile Tile to copy from.
 * @param offset Offset of \a tile relative to the blueprint origin.
 * @param bits Road pieces to store for each owned road/tram type on the tile.
 */
static void CopyOwnedRoadBits(Blueprint &blueprint, TileIndex tile, TileIndexDiffC offset, RoadBits bits)
{
	for (RoadTramType rtt : {RoadTramType::Road, RoadTramType::Tram}) {
		RoadType rt = GetRoadType(tile, rtt);
		if (rt == INVALID_ROADTYPE) continue;
		if (GetRoadOwner(tile, rtt) != _local_company) continue;
		blueprint.roads.push_back({offset, bits, rt});
	}
}

/**
 * Copy a road tile (normal road, level crossing or road depot).
 * @param blueprint Blueprint to copy into.
 * @param tile Tile to copy from.
 * @param offset Offset of \a tile relative to the blueprint origin.
 */
static void CopyRoadTile(Blueprint &blueprint, TileIndex tile, TileIndexDiffC offset)
{
	switch (GetRoadTileType(tile)) {
		case RoadTileType::Normal:
			CopyOwnedRoadBits(blueprint, tile, offset, GetRoadBits(tile, RoadTramType::Road) | GetRoadBits(tile, RoadTramType::Tram));
			break;

		case RoadTileType::Crossing:
			/* The rail part belongs to the tile owner, the road part to the road owner(s). */
			if (GetTileOwner(tile) == _local_company) {
				blueprint.rail_tracks.push_back({offset, GetCrossingRailTrack(tile), GetRailType(tile)});
			}
			CopyOwnedRoadBits(blueprint, tile, offset, GetCrossingRoadBits(tile));
			break;

		case RoadTileType::Depot:
			if (GetTileOwner(tile) != _local_company) return;
			blueprint.road_depots.push_back({offset, GetRoadDepotDirection(tile),
					GetRoadTypeRoad(tile) != INVALID_ROADTYPE ? GetRoadTypeRoad(tile) : GetRoadTypeTram(tile)});
			break;

		default: NOT_REACHED();
	}
}

/**
 * Copy a tunnel or bridge if it is fully contained in the copied area.
 * @param blueprint Blueprint to copy into.
 * @param area The copied area.
 * @param tile Tunnel or bridge end tile.
 */
static void CopyTunnelBridgeTile(Blueprint &blueprint, const TileArea &area, TileIndex tile)
{
	if (GetTileOwner(tile) != _local_company) return;

	TileIndex other = IsTunnel(tile) ? GetOtherTunnelEnd(tile) : GetOtherBridgeEnd(tile);

	/* Record each tunnel/bridge only once, anchored at its northern end,
	 * and only when both ends are inside the copied area. */
	if (other < tile) return;
	if (!area.Contains(other)) return;

	BlueprintTunnelBridge tb{};
	tb.offset = BlueprintOffset(tile, area.tile);
	tb.other_end = BlueprintOffset(other, area.tile);
	tb.transport = GetTunnelBridgeTransportType(tile);
	tb.is_bridge = IsBridge(tile);
	tb.bridge_type = tb.is_bridge ? GetBridgeType(tile) : 0;
	tb.railtype = tb.transport == TransportType::Rail ? GetRailType(tile) : INVALID_RAILTYPE;
	tb.roadtype = tb.transport == TransportType::Road ? GetRoadTypeRoad(tile) : INVALID_ROADTYPE;
	tb.tramtype = tb.transport == TransportType::Road ? GetRoadTypeTram(tile) : INVALID_ROADTYPE;
	blueprint.tunnel_bridges.push_back(tb);
}

/**
 * Copy a water tile (canal, lock or ship depot).
 * @param blueprint Blueprint to copy into.
 * @param tile Tile to copy from.
 * @param offset Offset of \a tile relative to the blueprint origin.
 */
static void CopyWaterTile(Blueprint &blueprint, TileIndex tile, TileIndexDiffC offset)
{
	if (GetTileOwner(tile) != _local_company) return;

	if (IsCanal(tile)) {
		blueprint.canals.push_back({offset});
	} else if (IsLock(tile)) {
		/* Record the lock only once, at its middle part. */
		if (GetLockPart(tile) == LockPart::Middle) blueprint.locks.push_back({offset, GetLockDirection(tile)});
	} else if (IsShipDepot(tile)) {
		/* Record the depot only once, at its northern part. */
		if (GetShipDepotPart(tile) == DepotPart::North) blueprint.ship_depots.push_back({offset, GetShipDepotAxis(tile)});
	}
}

/**
 * Copy a station tile (rail station/waypoint, road stop, dock, buoy or airport).
 * @param blueprint Blueprint to copy into.
 * @param tile Tile to copy from.
 * @param offset Offset of \a tile relative to the blueprint origin.
 */
static void CopyStationTile(Blueprint &blueprint, TileIndex tile, TileIndexDiffC offset)
{
	if (GetTileOwner(tile) != _local_company) return;

	switch (GetStationType(tile)) {
		case StationType::Rail:
		case StationType::RailWaypoint: {
			BlueprintStationTile st{offset, IsRailWaypointTile(tile), GetRailStationAxis(tile), GetRailType(tile), GetStationIndex(tile)};
			if (const StationSpec *spec = GetStationSpec(tile); spec != nullptr) st.spec = {spec->grf_prop.grfid, spec->grf_prop.local_id};
			blueprint.station_tiles.push_back(st);
			break;
		}

		case StationType::Bus:
		case StationType::Truck:
		case StationType::RoadWaypoint: {
			BlueprintRoadStop stop{};
			stop.offset = offset;
			stop.type = GetStationType(tile);
			stop.drive_through = IsDriveThroughStopTile(tile);
			stop.dir = stop.drive_through ? DiagDirection::Invalid : GetBayRoadStopDir(tile);
			stop.axis = stop.drive_through ? GetDriveThroughStopAxis(tile) : Axis::Invalid;
			stop.roadtype = GetRoadTypeRoad(tile);
			stop.tramtype = GetRoadTypeTram(tile);
			stop.station = GetStationIndex(tile);
			if (const RoadStopSpec *spec = GetRoadStopSpec(tile); spec != nullptr) stop.spec = {spec->grf_prop.grfid, spec->grf_prop.local_id};
			blueprint.road_stops.push_back(stop);
			break;
		}

		case StationType::Dock:
			/* Record the dock only once, at its land part. */
			if (GetStationGfx(tile) < GFX_DOCK_BASE_WATER_PART) blueprint.docks.push_back({offset, GetDockDirection(tile), GetStationIndex(tile)});
			break;

		case StationType::Buoy:
			blueprint.buoys.push_back({offset});
			break;

		case StationType::Airport: {
			/* Record the airport only once, at its northern tile. */
			const Station *st = Station::GetByTile(tile);
			if (st->airport.tile != tile) break;

			BlueprintAirport airport{offset, st->airport.type, st->airport.layout,
					static_cast<uint8_t>(to_underlying(st->airport.rotation)), static_cast<uint8_t>(st->airport.air_type),
					st->airport.w, st->airport.h, st->index, {}};

			if (st->airport.type == AT_CUSTOM) {
				/* Custom (multitile) airports have no spec layout to rebuild from; capture every tile. */
				for (TileIndex t : st->airport) {
					if (!st->TileBelongsToAirport(t)) continue;
					if (GetTileOwner(t) != _local_company) continue;
					TileIndexDiffC rel = BlueprintOffset(t, tile);
					TileIndexDiffC toff{static_cast<int16_t>(offset.x + rel.x), static_cast<int16_t>(offset.y + rel.y)};
					if (toff.x < 0 || toff.y < 0) continue;
					airport.tiles.push_back({toff, static_cast<uint8_t>(Tile(t).m4()),
							static_cast<uint8_t>(Tile(t).m5()), static_cast<uint8_t>(Tile(t).m6()),
							static_cast<uint16_t>(Tile(t).m8() & ~(0x3F << 6))}); // clear reservation bits
				}
			}

			blueprint.airports.push_back(std::move(airport));
			break;
		}

		default: // Oilrigs and anything else is not copyable.
			break;
	}
}

/**
 * Shrink a freshly copied blueprint to the smallest rectangle that contains
 * all copied elements (including the extra tiles of multi-tile elements).
 * @param blueprint Blueprint to shrink; must not be empty.
 * @return The offset the blueprint contents were shifted by.
 */
static TileIndexDiffC ShrinkBlueprintToContents(Blueprint &blueprint)
{
	int min_x = blueprint.width - 1;
	int min_y = blueprint.height - 1;
	int max_x = 0;
	int max_y = 0;

	auto include = [&](int x, int y) {
		x = Clamp(x, 0, blueprint.width - 1);
		y = Clamp(y, 0, blueprint.height - 1);
		min_x = std::min(min_x, x);
		min_y = std::min(min_y, y);
		max_x = std::max(max_x, x);
		max_y = std::max(max_y, y);
	};

	for (const BlueprintObject &v : blueprint.objects) {
		include(v.offset.x, v.offset.y);
		include(v.offset.x + v.w - 1, v.offset.y + v.h - 1);
	}
	for (const BlueprintRailTrack &v : blueprint.rail_tracks) include(v.offset.x, v.offset.y);
	for (const BlueprintSignal &v : blueprint.signals) include(v.offset.x, v.offset.y);
	for (const BlueprintRailDepot &v : blueprint.rail_depots) include(v.offset.x, v.offset.y);
	for (const BlueprintTunnelBridge &v : blueprint.tunnel_bridges) {
		include(v.offset.x, v.offset.y);
		include(v.other_end.x, v.other_end.y);
	}
	for (const BlueprintRoad &v : blueprint.roads) include(v.offset.x, v.offset.y);
	for (const BlueprintRoadDepot &v : blueprint.road_depots) include(v.offset.x, v.offset.y);
	for (const BlueprintCanal &v : blueprint.canals) include(v.offset.x, v.offset.y);
	for (const BlueprintLock &v : blueprint.locks) {
		TileIndexDiffC d = TileIndexDiffCByDiagDir(v.dir);
		include(v.offset.x - d.x, v.offset.y - d.y);
		include(v.offset.x, v.offset.y);
		include(v.offset.x + d.x, v.offset.y + d.y);
	}
	for (const BlueprintShipDepot &v : blueprint.ship_depots) {
		include(v.offset.x, v.offset.y);
		include(v.offset.x + (v.axis == Axis::X ? 1 : 0), v.offset.y + (v.axis == Axis::Y ? 1 : 0));
	}
	for (const BlueprintStationTile &v : blueprint.station_tiles) include(v.offset.x, v.offset.y);
	for (const BlueprintRoadStop &v : blueprint.road_stops) include(v.offset.x, v.offset.y);
	for (const BlueprintDock &v : blueprint.docks) {
		TileIndexDiffC d = TileIndexDiffCByDiagDir(v.dir);
		include(v.offset.x, v.offset.y);
		include(v.offset.x + d.x, v.offset.y + d.y);
	}
	for (const BlueprintBuoy &v : blueprint.buoys) include(v.offset.x, v.offset.y);
	for (const BlueprintAirport &v : blueprint.airports) {
		include(v.offset.x, v.offset.y);
		include(v.offset.x + v.w - 1, v.offset.y + v.h - 1);
		for (const BlueprintAirportTile &apt : v.tiles) include(apt.offset.x, apt.offset.y);
	}

	auto shift = [&](TileIndexDiffC &offset) {
		offset.x = static_cast<int16_t>(offset.x - min_x);
		offset.y = static_cast<int16_t>(offset.y - min_y);
	};
	for (BlueprintObject &v : blueprint.objects) shift(v.offset);
	for (BlueprintRailTrack &v : blueprint.rail_tracks) shift(v.offset);
	for (BlueprintSignal &v : blueprint.signals) shift(v.offset);
	for (BlueprintRailDepot &v : blueprint.rail_depots) shift(v.offset);
	for (BlueprintTunnelBridge &v : blueprint.tunnel_bridges) {
		shift(v.offset);
		shift(v.other_end);
	}
	for (BlueprintRoad &v : blueprint.roads) shift(v.offset);
	for (BlueprintRoadDepot &v : blueprint.road_depots) shift(v.offset);
	for (BlueprintCanal &v : blueprint.canals) shift(v.offset);
	for (BlueprintLock &v : blueprint.locks) shift(v.offset);
	for (BlueprintShipDepot &v : blueprint.ship_depots) shift(v.offset);
	for (BlueprintStationTile &v : blueprint.station_tiles) shift(v.offset);
	for (BlueprintRoadStop &v : blueprint.road_stops) shift(v.offset);
	for (BlueprintDock &v : blueprint.docks) shift(v.offset);
	for (BlueprintBuoy &v : blueprint.buoys) shift(v.offset);
	for (BlueprintAirport &v : blueprint.airports) {
		shift(v.offset);
		for (BlueprintAirportTile &apt : v.tiles) shift(apt.offset);
	}

	blueprint.width = static_cast<uint16_t>(max_x - min_x + 1);
	blueprint.height = static_cast<uint16_t>(max_y - min_y + 1);

	return {static_cast<int16_t>(min_x), static_cast<int16_t>(min_y)};
}

/**
 * Copy all infrastructure of the local company in an area into a blueprint.
 * The blueprint is shrunk to the smallest rectangle containing the copied
 * elements; empty margins of the selected area are not kept.
 * @param area Map area to copy.
 * @param blueprint Blueprint to copy into; existing contents are replaced.
 * @return Number of copied elements.
 */
uint CopyAreaToBlueprint(TileArea area, Blueprint &blueprint)
{
	blueprint.Clear();
	area.ClampToMap();
	blueprint.width = area.w;
	blueprint.height = area.h;

	for (TileIndex tile : area) {
		TileIndexDiffC offset = BlueprintOffset(tile, area.tile);

		switch (GetTileType(tile)) {
			case TileType::Railway:
				if (GetTileOwner(tile) != _local_company) break;
				CopyRailTile(blueprint, tile, offset);
				break;

			case TileType::Road:
				CopyRoadTile(blueprint, tile, offset);
				break;

			case TileType::TunnelBridge:
				CopyTunnelBridgeTile(blueprint, area, tile);
				break;

			case TileType::Water:
				CopyWaterTile(blueprint, tile, offset);
				break;

			case TileType::Station:
				CopyStationTile(blueprint, tile, offset);
				break;

			case TileType::Object: {
				if (GetTileOwner(tile) != _local_company && GetTileOwner(tile) != OWNER_NONE) break;
				const Object *object = Object::GetByTile(tile);
				const ObjectSpec *spec = ObjectSpec::Get(object->type);
				/* Copy a complete object once; built-in HQ/statue commands have special side effects. */
				if (object->location.tile != tile || spec->grf_prop.grfid == 0) break;
				if (TileX(tile) + object->location.w > TileX(area.tile) + area.w ||
						TileY(tile) + object->location.h > TileY(area.tile) + area.h) break;
				blueprint.objects.push_back({offset, {spec->grf_prop.grfid, spec->grf_prop.local_id},
						object->view, spec->views, static_cast<uint8_t>(object->location.w), static_cast<uint8_t>(object->location.h)});
				break;
			}

			default: // Terrain, houses, industries, ... are not copyable.
				break;
		}
	}

	uint count = blueprint.CountItems();
	uint ox = TileX(area.tile);
	uint oy = TileY(area.tile);

	if (count > 0) {
		TileIndexDiffC shifted = ShrinkBlueprintToContents(blueprint);
		ox += shifted.x;
		oy += shifted.y;
	}

	/* Store the landscape: heights of all tile corners of the area, for terraforming on paste. */
	blueprint.corner_heights.reserve((blueprint.width + 1) * (blueprint.height + 1));
	for (uint y = 0; y <= blueprint.height; y++) {
		for (uint x = 0; x <= blueprint.width; x++) {
			blueprint.corner_heights.push_back(TileHeight(TileXY(std::min(ox + x, Map::MaxX()), std::min(oy + y, Map::MaxY()))));
		}
	}

	return count;
}

/**
 * Transform a point of a blueprint: reflect against the NW-SE axis first (if requested),
 * then rotate anticlockwise in quarter turns.
 * @param point Point to transform, coordinates within [0, bounds_x) x [0, bounds_y).
 * @param bounds_x Size of the point grid along the X axis.
 * @param bounds_y Size of the point grid along the Y axis.
 * @param rotation Number of quarter turns anticlockwise (0..3).
 * @param reflected Whether to reflect before rotating.
 * @return The transformed point; for odd rotations the bounds are swapped.
 */
TileIndexDiffC TransformBlueprintPoint(TileIndexDiffC point, uint16_t bounds_x, uint16_t bounds_y, uint8_t rotation, bool reflected)
{
	if (reflected) point.x = static_cast<int16_t>(bounds_x - 1 - point.x);

	for (uint i = 0; i < rotation % 4u; i++) {
		/* Rotate 90 degrees anticlockwise: north corner moves to the west. */
		point = {static_cast<int16_t>(bounds_y - 1 - point.y), point.x};
		std::swap(bounds_x, bounds_y);
	}

	return point;
}

/**
 * Transform a track piece the same way as #TransformBlueprintPoint transforms points.
 * @param track Track piece to transform.
 * @param rotation Number of quarter turns anticlockwise (0..3).
 * @param reflected Whether to reflect against the NW-SE axis before rotating.
 * @return The transformed track piece.
 */
Track TransformBlueprintTrack(Track track, uint8_t rotation, bool reflected)
{
	/* Reflection against the NW-SE axis: the NE and SW tile sides swap. */
	static const Track reflect_map[TRACK_END] = {TRACK_X, TRACK_Y, TRACK_LEFT, TRACK_RIGHT, TRACK_UPPER, TRACK_LOWER};
	/* Rotation by 90 degrees anticlockwise: the north corner moves to the west. */
	static const Track rotate_map[TRACK_END] = {TRACK_Y, TRACK_X, TRACK_LEFT, TRACK_RIGHT, TRACK_LOWER, TRACK_UPPER};

	if (reflected) track = reflect_map[track];
	for (uint i = 0; i < rotation % 4u; i++) track = rotate_map[track];

	return track;
}

/**
 * Transform a diagonal direction the same way as #TransformBlueprintPoint transforms points.
 * @param dir Direction to transform.
 * @param rotation Number of quarter turns anticlockwise (0..3).
 * @param reflected Whether to reflect against the NW-SE axis before rotating.
 * @return The transformed direction.
 */
DiagDirection TransformBlueprintDiagDir(DiagDirection dir, uint8_t rotation, bool reflected)
{
	/* Reflection against the NW-SE axis: NE and SW swap, NW and SE stay. */
	static const DiagDirection reflect_map[to_underlying(DiagDirection::End)] = {DiagDirection::SW, DiagDirection::SE, DiagDirection::NE, DiagDirection::NW};

	if (reflected) dir = reflect_map[to_underlying(dir)];
	/* Rotation by 90 degrees anticlockwise: NE -> NW -> SW -> SE -> NE. */
	return static_cast<DiagDirection>((to_underlying(dir) + 3 * (rotation % 4u)) % 4u);
}

/**
 * Transform an axis the same way as #TransformBlueprintPoint transforms points.
 * @param axis Axis to transform.
 * @param rotation Number of quarter turns anticlockwise (0..3).
 * @return The transformed axis (reflections never change an axis).
 */
Axis TransformBlueprintAxis(Axis axis, uint8_t rotation)
{
	return (rotation % 2) != 0 ? OtherAxis(axis) : axis;
}

/**
 * Transform road pieces the same way as #TransformBlueprintPoint transforms points.
 * @param bits Road pieces to transform.
 * @param rotation Number of quarter turns anticlockwise (0..3).
 * @param reflected Whether to reflect against the NW-SE axis before rotating.
 * @return The transformed road pieces.
 */
RoadBits TransformBlueprintRoadBits(RoadBits bits, uint8_t rotation, bool reflected)
{
	RoadBits result = ROAD_NONE;
	for (DiagDirection dir = DiagDirection::Begin; dir < DiagDirection::End; dir++) {
		if ((bits & DiagDirToRoadBits(dir)) == ROAD_NONE) continue;
		result |= DiagDirToRoadBits(TransformBlueprintDiagDir(dir, rotation, reflected));
	}
	return result;
}

/**
 * Order two offsets so that the first one is the more northern map tile.
 * @param a First offset (in/out).
 * @param b Second offset (in/out).
 */
static void SortBlueprintOffsets(TileIndexDiffC &a, TileIndexDiffC &b)
{
	if (b.y < a.y || (b.y == a.y && b.x < a.x)) std::swap(a, b);
}

/**
 * Create a transformed copy of a blueprint: all offsets, orientations and the
 * stored landscape are reflected/rotated; multi-tile elements are re-anchored
 * at their northern tile.
 * @param blueprint Blueprint to transform.
 * @param rotation Number of quarter turns anticlockwise (0..3).
 * @param reflected Whether to reflect against the NW-SE axis before rotating.
 * @return The transformed blueprint.
 */
Blueprint TransformBlueprint(const Blueprint &blueprint, uint8_t rotation, bool reflected)
{
	Blueprint result;
	bool swap_axes = (rotation % 2) != 0;
	result.width = swap_axes ? blueprint.height : blueprint.width;
	result.height = swap_axes ? blueprint.width : blueprint.height;

	/* Transform a tile offset within the blueprint area. */
	auto tile_offset = [&](TileIndexDiffC offset) {
		return TransformBlueprintPoint(offset, blueprint.width, blueprint.height, rotation, reflected);
	};

	for (const BlueprintRailTrack &rt : blueprint.rail_tracks) {
		result.rail_tracks.push_back({tile_offset(rt.offset), TransformBlueprintTrack(rt.track, rotation, reflected), rt.railtype});
	}

	for (const BlueprintSignal &sig : blueprint.signals) {
		/* Transform the canonical trackdir of the track; when it maps onto the
		 * reversed trackdir of the new track, the two signal directions swap. */
		Trackdir td = TrackToTrackdir(sig.track);
		Track new_track = TransformBlueprintTrack(sig.track, rotation, reflected);
		Trackdir new_td = TrackExitdirToTrackdir(new_track, TransformBlueprintDiagDir(TrackdirToExitdir(td), rotation, reflected));
		bool swapped = new_td != TrackToTrackdir(new_track);
		result.signals.push_back({tile_offset(sig.offset), new_track, sig.type, sig.variant,
				swapped ? sig.against : sig.along, swapped ? sig.along : sig.against, sig.spec});
	}

	for (const BlueprintRailDepot &depot : blueprint.rail_depots) {
		result.rail_depots.push_back({tile_offset(depot.offset), TransformBlueprintDiagDir(depot.dir, rotation, reflected), depot.railtype});
	}

	for (BlueprintTunnelBridge tb : blueprint.tunnel_bridges) {
		tb.offset = tile_offset(tb.offset);
		tb.other_end = tile_offset(tb.other_end);
		SortBlueprintOffsets(tb.offset, tb.other_end);
		result.tunnel_bridges.push_back(tb);
	}

	for (const BlueprintRoad &road : blueprint.roads) {
		result.roads.push_back({tile_offset(road.offset), TransformBlueprintRoadBits(road.bits, rotation, reflected), road.roadtype});
	}

	for (const BlueprintRoadDepot &depot : blueprint.road_depots) {
		result.road_depots.push_back({tile_offset(depot.offset), TransformBlueprintDiagDir(depot.dir, rotation, reflected), depot.roadtype});
	}

	for (const BlueprintCanal &canal : blueprint.canals) {
		result.canals.push_back({tile_offset(canal.offset)});
	}

	for (const BlueprintLock &lock : blueprint.locks) {
		/* Locks are anchored at their (symmetric) middle part. */
		result.locks.push_back({tile_offset(lock.offset), TransformBlueprintDiagDir(lock.dir, rotation, reflected)});
	}

	for (const BlueprintShipDepot &depot : blueprint.ship_depots) {
		/* Transform both halves and re-anchor at the northern one. */
		TileIndexDiffC a = depot.offset;
		TileIndexDiffC b = {static_cast<int16_t>(a.x + (depot.axis == Axis::X ? 1 : 0)), static_cast<int16_t>(a.y + (depot.axis == Axis::Y ? 1 : 0))};
		a = tile_offset(a);
		b = tile_offset(b);
		SortBlueprintOffsets(a, b);
		result.ship_depots.push_back({a, TransformBlueprintAxis(depot.axis, rotation)});
	}

	for (const BlueprintStationTile &st : blueprint.station_tiles) {
		result.station_tiles.push_back({tile_offset(st.offset), st.is_waypoint, TransformBlueprintAxis(st.axis, rotation), st.railtype, st.station, st.spec});
	}

	for (BlueprintRoadStop stop : blueprint.road_stops) {
		stop.offset = tile_offset(stop.offset);
		if (stop.drive_through) {
			stop.axis = TransformBlueprintAxis(stop.axis, rotation);
		} else {
			stop.dir = TransformBlueprintDiagDir(stop.dir, rotation, reflected);
		}
		result.road_stops.push_back(stop);
	}

	for (const BlueprintDock &dock : blueprint.docks) {
		result.docks.push_back({tile_offset(dock.offset), TransformBlueprintDiagDir(dock.dir, rotation, reflected), dock.station});
	}

	for (const BlueprintBuoy &buoy : blueprint.buoys) {
		result.buoys.push_back({tile_offset(buoy.offset)});
	}

	for (BlueprintAirport airport : blueprint.airports) {
		/* Transform two opposite corners of the airport area and re-anchor at the northern corner.
		 * The layout itself is not rotated; airport layouts are fixed by their spec. */
		TileIndexDiffC a = airport.offset;
		TileIndexDiffC b = {static_cast<int16_t>(a.x + airport.w - 1), static_cast<int16_t>(a.y + airport.h - 1)};
		a = tile_offset(a);
		b = tile_offset(b);
		airport.offset = {std::min(a.x, b.x), std::min(a.y, b.y)};
		if (swap_axes) std::swap(airport.w, airport.h);
		for (BlueprintAirportTile &apt : airport.tiles) apt.offset = tile_offset(apt.offset);
		result.airports.push_back(airport);
	}

	for (BlueprintObject object : blueprint.objects) {
		TileIndexDiffC a = tile_offset(object.offset);
		TileIndexDiffC b = tile_offset({static_cast<int16_t>(object.offset.x + object.w - 1), static_cast<int16_t>(object.offset.y + object.h - 1)});
		object.offset = {std::min(a.x, b.x), std::min(a.y, b.y)};
		if (swap_axes) std::swap(object.w, object.h);
		if ((object.views == 2 || object.views == 4) && object.view < object.views) {
			object.view = to_underlying(TransformBlueprintDiagDir(static_cast<DiagDirection>(object.view), rotation, reflected)) % object.views;
		}
		result.objects.push_back(object);
	}

	/* Transform the corner height grid; it has one more point than tiles in each direction. */
	if (blueprint.corner_heights.empty()) return result;
	result.corner_heights.resize((result.width + 1) * (result.height + 1), 0);
	for (uint y = 0; y <= blueprint.height; y++) {
		for (uint x = 0; x <= blueprint.width; x++) {
			TileIndexDiffC p = TransformBlueprintPoint({static_cast<int16_t>(x), static_cast<int16_t>(y)},
					blueprint.width + 1, blueprint.height + 1, rotation, reflected);
			result.corner_heights[p.y * (result.width + 1) + p.x] = blueprint.corner_heights[y * (blueprint.width + 1) + x];
		}
	}

	return result;
}

/**
 * Group the per-tile station records of a blueprint into maximal rectangles of
 * equal kind (axis, waypoint flag, rail type, original station), suitable for
 * build commands.
 * @param blueprint Blueprint (must already be transformed).
 * @return The station rectangles.
 */
std::vector<BlueprintStationRect> GroupStationTiles(const Blueprint &blueprint)
{
	std::vector<BlueprintStationRect> result;
	uint w = blueprint.width;
	uint h = blueprint.height;

	struct Cell {
		bool present = false;
		bool used = false;
		bool is_waypoint = false;
		Axis axis = Axis::X;
		RailType railtype = INVALID_RAILTYPE;
		StationID station = StationID::Invalid();
		BlueprintSpecRef spec{};

		bool SameKind(const Cell &other) const
		{
			return this->present && !this->used && this->is_waypoint == other.is_waypoint && this->axis == other.axis && this->railtype == other.railtype && this->station == other.station && this->spec == other.spec;
		}
	};
	std::vector<Cell> grid(w * h);

	for (const BlueprintStationTile &st : blueprint.station_tiles) {
		Cell &cell = grid[st.offset.y * w + st.offset.x];
		cell.present = true;
		cell.is_waypoint = st.is_waypoint;
		cell.axis = st.axis;
		cell.railtype = st.railtype;
		cell.station = st.station;
		cell.spec = st.spec;
	}

	for (uint y = 0; y < h; y++) {
		for (uint x = 0; x < w; x++) {
			Cell &cell = grid[y * w + x];
			if (!cell.present || cell.used) continue;

			/* Greedily grow a rectangle of equal tiles: first along X, then along Y. */
			uint len_x = 1;
			while (x + len_x < w && grid[y * w + x + len_x].SameKind(cell)) len_x++;

			uint len_y = 1;
			for (; y + len_y < h; len_y++) {
				bool row_matches = true;
				for (uint dx = 0; dx < len_x; dx++) {
					if (!grid[(y + len_y) * w + x + dx].SameKind(cell)) {
						row_matches = false;
						break;
					}
				}
				if (!row_matches) break;
			}

			for (uint dy = 0; dy < len_y; dy++) {
				for (uint dx = 0; dx < len_x; dx++) {
					grid[(y + dy) * w + x + dx].used = true;
				}
			}

			result.push_back({{static_cast<int16_t>(x), static_cast<int16_t>(y)},
					static_cast<uint16_t>(len_x), static_cast<uint16_t>(len_y), cell.is_waypoint, cell.axis, cell.railtype, cell.station, cell.spec});
		}
	}

	return result;
}

/**
 * Pick the fastest bridge type currently available for a bridge of the given length.
 * @param bridge_len Length of the bridge, excluding the ramp tiles.
 * @param fallback Bridge type to keep when no type is available at all.
 * @return The available bridge type with the highest speed limit (ties: the cheaper one).
 */
static BridgeType FindFastestBridgeType(uint bridge_len, BridgeType fallback)
{
	BridgeType best = fallback;
	const BridgeSpec *best_spec = nullptr;

	for (BridgeType bt = 0; bt < MAX_BRIDGES; bt++) {
		if (CheckBridgeAvailability(bt, bridge_len).Failed()) continue;
		const BridgeSpec *spec = GetBridgeSpec(bt);
		if (best_spec == nullptr || spec->speed > best_spec->speed || (spec->speed == best_spec->speed && spec->price < best_spec->price)) {
			best = bt;
			best_spec = spec;
		}
	}

	return best;
}

/**
 * Prepare a blueprint for the paste command: apply the transformation, the
 * paste filters and the rail/signal/bridge options, all of which are pure
 * client-side choices. The result carries everything the command needs.
 * @param blueprint Blueprint to paste (as copied).
 * @param options Filters, transformation and terraform settings to apply.
 * @param convert_to Rail type to convert rail to (when the option is enabled).
 * @return The pre-processed paste data.
 */
BlueprintPasteData PrepareBlueprintPaste(const Blueprint &blueprint, const BlueprintPasteOptions &options, RailType convert_to)
{
	BlueprintPasteData data;
	data.terraform_mode = options.terraform_mode;
	data.height_offset = options.height_offset;

	Blueprint &bp = data.blueprint;
	bp = TransformBlueprint(blueprint, options.rotation, options.reflected);

	if (!options.paste_rail) {
		bp.rail_tracks.clear();
		bp.signals.clear();
		bp.rail_depots.clear();
	}
	if (!options.paste_road) {
		bp.roads.clear();
		bp.road_depots.clear();
	}
	if (!options.paste_water) {
		bp.canals.clear();
		bp.locks.clear();
		bp.ship_depots.clear();
	}
	if (!options.paste_air) bp.airports.clear();
	std::erase_if(bp.tunnel_bridges, [&](const BlueprintTunnelBridge &tb) {
		switch (tb.transport) {
			case TransportType::Rail: return !options.paste_rail;
			case TransportType::Road: return !options.paste_road;
			default: return !options.paste_water;
		}
	});
	if (!options.with_stations || !options.paste_rail) bp.station_tiles.clear();
	if (!options.with_stations || !options.paste_road) bp.road_stops.clear();
	if (!options.with_stations || !options.paste_water) {
		bp.docks.clear();
		bp.buoys.clear();
	}

	if (options.convert_railtype && convert_to != INVALID_RAILTYPE) {
		for (BlueprintRailTrack &rt : bp.rail_tracks) rt.railtype = convert_to;
		for (BlueprintRailDepot &depot : bp.rail_depots) depot.railtype = convert_to;
		for (BlueprintTunnelBridge &tb : bp.tunnel_bridges) {
			if (tb.transport == TransportType::Rail) tb.railtype = convert_to;
		}
		for (BlueprintStationTile &st : bp.station_tiles) st.railtype = convert_to;
	}

	if (options.mirror_signals) {
		for (BlueprintSignal &sig : bp.signals) std::swap(sig.along, sig.against);
	}

	if (options.upgrade_bridges) {
		for (BlueprintTunnelBridge &tb : bp.tunnel_bridges) {
			if (!tb.is_bridge || tb.transport == TransportType::Water) continue;
			/* Length between the ramps, as CheckBridgeAvailability expects it. */
			uint bridge_len = std::max(abs(tb.other_end.x - tb.offset.x), abs(tb.other_end.y - tb.offset.y)) - 1;
			tb.bridge_type = FindFastestBridgeType(bridge_len, tb.bridge_type);
		}
	}

	/* The landscape is only needed when terraforming. */
	if (data.terraform_mode == BlueprintTerraformMode::None) bp.corner_heights.clear();

	return data;
}

/**
 * Check that a (possibly network-received) blueprint only contains offsets
 * within its own bounds and valid values where this file indexes by them.
 * @param bp Blueprint to check.
 * @return True when the blueprint is safe to paste.
 */
static bool BlueprintOffsetsValid(const Blueprint &bp)
{
	auto ok = [&](const TileIndexDiffC &offset) {
		return offset.x >= 0 && offset.y >= 0 && offset.x < bp.width && offset.y < bp.height;
	};

	for (const BlueprintRailTrack &v : bp.rail_tracks) if (!ok(v.offset) || !IsValidTrack(v.track)) return false;
	for (const BlueprintSignal &v : bp.signals) if (!ok(v.offset) || !IsValidTrack(v.track)) return false;
	for (const BlueprintRailDepot &v : bp.rail_depots) if (!ok(v.offset)) return false;
	for (const BlueprintTunnelBridge &v : bp.tunnel_bridges) if (!ok(v.offset) || !ok(v.other_end)) return false;
	for (const BlueprintRoad &v : bp.roads) if (!ok(v.offset)) return false;
	for (const BlueprintRoadDepot &v : bp.road_depots) if (!ok(v.offset)) return false;
	for (const BlueprintCanal &v : bp.canals) if (!ok(v.offset)) return false;
	for (const BlueprintLock &v : bp.locks) if (!ok(v.offset) || !IsValidDiagDirection(v.dir)) return false;
	for (const BlueprintShipDepot &v : bp.ship_depots) if (!ok(v.offset)) return false;
	for (const BlueprintStationTile &v : bp.station_tiles) if (!ok(v.offset) || !IsValidAxis(v.axis)) return false;
	for (const BlueprintRoadStop &v : bp.road_stops) if (!ok(v.offset) || (v.drive_through ? !IsValidAxis(v.axis) : !IsValidDiagDirection(v.dir))) return false;
	for (const BlueprintDock &v : bp.docks) if (!ok(v.offset) || !IsValidDiagDirection(v.dir)) return false;
	for (const BlueprintBuoy &v : bp.buoys) if (!ok(v.offset)) return false;
	for (const BlueprintAirport &v : bp.airports) {
		if (!ok(v.offset)) return false;
		for (const BlueprintAirportTile &apt : v.tiles) if (!ok(apt.offset)) return false;
	}

	for (const BlueprintObject &v : bp.objects) {
		if (!ok(v.offset) || v.spec.grfid == 0 || v.w == 0 || v.h == 0 || v.w > 15 || v.h > 15 ||
				v.offset.x + v.w > bp.width || v.offset.y + v.h > bp.height ||
				(v.views != 1 && v.views != 2 && v.views != 4) || v.view >= v.views) return false;
	}
	return true;
}

/**
 * Undoable pastes per company, oldest first.
 *
 * Client-side scratch state (see #BlueprintUndoRecord): every client fills its
 * own copy for every player's paste, but only the client that issued a paste
 * ever reads its record back. Keying by company rather than testing against
 * #_local_company keeps client-dependent branches out of the command itself.
 */
static std::array<std::vector<BlueprintUndoRecord>, MAX_COMPANIES> _blueprint_undo_stacks;

/**
 * Drop expired undo records and trim the stack to its depth limit.
 * @param stack Stack to prune.
 */
static void PruneBlueprintUndoStack(std::vector<BlueprintUndoRecord> &stack)
{
	auto now = std::chrono::steady_clock::now();
	std::erase_if(stack, [&](const BlueprintUndoRecord &record) { return now - record.when > BLUEPRINT_UNDO_LIFETIME; });
	if (stack.size() > MAX_BLUEPRINT_UNDO_DEPTH) stack.erase(stack.begin(), stack.end() - MAX_BLUEPRINT_UNDO_DEPTH);
}

/**
 * Remember what a paste built, so that it can be undone later.
 * @param record What the paste built; ignored when it holds nothing undoable.
 */
static void PushBlueprintUndoRecord(BlueprintUndoRecord &&record)
{
	if (record.IsEmpty() || _current_company.base() >= MAX_COMPANIES) return;

	record.when = std::chrono::steady_clock::now();
	std::vector<BlueprintUndoRecord> &stack = _blueprint_undo_stacks[_current_company.base()];
	stack.push_back(std::move(record));
	PruneBlueprintUndoStack(stack);

	/* The undo button and the history list only read the stack while they
	 * repaint, so tell them that there is now something to undo. */
	InvalidateWindowClassesData(WindowClass::BlueprintUndoToolbar, 0, true);
	InvalidateWindowClassesData(WindowClass::BlueprintUndoList, 0, true);
}

/**
 * Work reported by the leaf commands of the top-level command currently running.
 *
 * While a group is open the offsets hold absolute map coordinates rather than
 * offsets from an origin; there is no origin yet, because it is the bounding box
 * of everything the action turns out to touch. #EndBlueprintUndoGroup re-bases
 * them once that is known.
 */
static Blueprint _undo_group;
static bool _undo_group_open = false;

/**
 * Corners this action has moved, and the height each had before it did.
 *
 * Kept separately from #_undo_group because the record stores heights as a dense
 * grid over its whole area, which cannot be laid out until the area is known.
 */
static std::map<TileIndex, uint8_t> _undo_group_terraform;

/** Start collecting what a top-level command builds. */
void BeginBlueprintUndoGroup()
{
	_undo_group.Clear();
	_undo_group_terraform.clear();
	_undo_group_open = true;
}

/**
 * Stop collecting without recording anything.
 *
 * Used by #CmdPasteBlueprint, which builds through the same leaf commands but
 * assembles its own record; without this its work would be recorded twice.
 */
void AbandonBlueprintUndoGroup()
{
	_undo_group.Clear();
	_undo_group_terraform.clear();
	_undo_group_open = false;
}

/**
 * Finish a top-level command and turn what it built into an undo record.
 * @param success Whether the command succeeded; a failed one records nothing.
 */
void EndBlueprintUndoGroup(bool success)
{
	if (!_undo_group_open) return;
	_undo_group_open = false;

	if (!success || (_undo_group.IsEmpty() && _undo_group_terraform.empty())) {
		_undo_group.Clear();
		_undo_group_terraform.clear();
		return;
	}

	/* The record is stored relative to an origin, so find the bounding box of
	 * everything this action touched and re-base onto its northern corner. */
	int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
	auto extend = [&](TileIndexDiffC offset) {
		min_x = std::min<int>(min_x, offset.x);
		max_x = std::max<int>(max_x, offset.x);
		min_y = std::min<int>(min_y, offset.y);
		max_y = std::max<int>(max_y, offset.y);
	};
	for (const BlueprintObject &v : _undo_group.objects) {
		extend(v.offset);
		extend({static_cast<int16_t>(v.offset.x + v.w - 1), static_cast<int16_t>(v.offset.y + v.h - 1)});
	}
	for (const BlueprintRailTrack &v : _undo_group.rail_tracks) extend(v.offset);
	for (const BlueprintSignal &v : _undo_group.signals) extend(v.offset);
	for (const BlueprintRailDepot &v : _undo_group.rail_depots) extend(v.offset);
	for (const BlueprintRoad &v : _undo_group.roads) extend(v.offset);
	for (const BlueprintRoadDepot &v : _undo_group.road_depots) extend(v.offset);
	for (const BlueprintTunnelBridge &v : _undo_group.tunnel_bridges) {
		extend(v.offset);
		extend(v.other_end);
	}
	for (const BlueprintCanal &v : _undo_group.canals) extend(v.offset);
	for (const BlueprintLock &v : _undo_group.locks) extend(v.offset);
	for (const BlueprintShipDepot &v : _undo_group.ship_depots) extend(v.offset);
	for (const BlueprintBuoy &v : _undo_group.buoys) extend(v.offset);
	for (const BlueprintDock &v : _undo_group.docks) extend(v.offset);
	for (const BlueprintAirport &v : _undo_group.airports) {
		extend(v.offset);
		for (const BlueprintAirportTile &apt : v.tiles) extend(apt.offset);
	}
	for (const BlueprintStationTile &v : _undo_group.station_tiles) extend(v.offset);
	for (const BlueprintRoadStop &v : _undo_group.road_stops) extend(v.offset);
	/* Moved corners take part too: an action that only shifts land has no
	 * elements to derive an area from, and would otherwise be dropped. */
	for (const auto &[corner, height] : _undo_group_terraform) {
		extend({static_cast<int16_t>(TileX(corner)), static_cast<int16_t>(TileY(corner))});
	}

	uint width = static_cast<uint>(max_x - min_x + 1);
	uint height = static_cast<uint>(max_y - min_y + 1);

	/* An action spread wider than a blueprint can describe is simply not
	 * recorded. Undoing it would need a record shape this one cannot express,
	 * and dropping it is safer than storing something the undo command would
	 * reject anyway. Only very long drags reach this. */
	if (width > MAX_BLUEPRINT_DIMENSION || height > MAX_BLUEPRINT_DIMENSION) {
		_undo_group.Clear();
		_undo_group_terraform.clear();
		return;
	}

	auto rebase = [&](TileIndexDiffC &offset) {
		offset.x = static_cast<int16_t>(offset.x - min_x);
		offset.y = static_cast<int16_t>(offset.y - min_y);
	};
	for (BlueprintObject &v : _undo_group.objects) rebase(v.offset);
	for (BlueprintRailTrack &v : _undo_group.rail_tracks) rebase(v.offset);
	for (BlueprintSignal &v : _undo_group.signals) rebase(v.offset);
	for (BlueprintRailDepot &v : _undo_group.rail_depots) rebase(v.offset);
	for (BlueprintRoad &v : _undo_group.roads) rebase(v.offset);
	for (BlueprintRoadDepot &v : _undo_group.road_depots) rebase(v.offset);
	for (BlueprintTunnelBridge &v : _undo_group.tunnel_bridges) {
		rebase(v.offset);
		rebase(v.other_end);
	}
	for (BlueprintCanal &v : _undo_group.canals) rebase(v.offset);
	for (BlueprintLock &v : _undo_group.locks) rebase(v.offset);
	for (BlueprintShipDepot &v : _undo_group.ship_depots) rebase(v.offset);
	for (BlueprintBuoy &v : _undo_group.buoys) rebase(v.offset);
	for (BlueprintDock &v : _undo_group.docks) rebase(v.offset);
	for (BlueprintAirport &v : _undo_group.airports) {
		rebase(v.offset);
		for (BlueprintAirportTile &apt : v.tiles) rebase(apt.offset);
	}
	for (BlueprintStationTile &v : _undo_group.station_tiles) rebase(v.offset);
	for (BlueprintRoadStop &v : _undo_group.road_stops) rebase(v.offset);

	/* Lay the moved corners out as the dense grid the record stores: a corner
	 * this action did not touch keeps the height it has now, so restoring it is
	 * a no-op. */
	if (!_undo_group_terraform.empty()) {
		_undo_group.corner_heights.resize(static_cast<size_t>(width + 1) * (height + 1));
		for (uint cy = 0; cy <= height; cy++) {
			for (uint cx = 0; cx <= width; cx++) {
				if (min_x + cx > Map::MaxX() || min_y + cy > Map::MaxY()) continue;
				TileIndex corner = TileXY(min_x + cx, min_y + cy);
				auto it = _undo_group_terraform.find(corner);
				_undo_group.corner_heights[cy * (width + 1) + cx] =
						it != _undo_group_terraform.end() ? it->second : static_cast<uint8_t>(TileHeight(corner));
			}
		}
	}
	_undo_group_terraform.clear();

	BlueprintUndoRecord record;
	record.origin = TileXY(min_x, min_y);
	record.built = std::move(_undo_group);
	record.built.width = static_cast<uint16_t>(width);
	record.built.height = static_cast<uint16_t>(height);

	_undo_group.Clear();
	PushBlueprintUndoRecord(std::move(record));
}

/** Absolute map position of a tile, as an offset placeholder for an open group. */
static TileIndexDiffC UndoGroupPos(TileIndex tile)
{
	return {static_cast<int16_t>(TileX(tile)), static_cast<int16_t>(TileY(tile))};
}

/**
 * Record a track piece that was just laid.
 * @param tile Tile the track is on.
 * @param track Track piece that was added.
 * @param railtype Rail type it was built with.
 */
void RecordBlueprintUndoRailTrack(TileIndex tile, Track track, RailType railtype)
{
	if (!_undo_group_open) return;
	_undo_group.rail_tracks.push_back({UndoGroupPos(tile), track, railtype});
}

/**
 * Record signals that were just placed on a track that carried none.
 *
 * Signals that replaced existing ones are not reported by the caller: the
 * original cannot be put back, so removing the replacement would not be an undo.
 *
 * @param tile Tile the signals are on.
 * @param track Track piece they sit on.
 */
void RecordBlueprintUndoSignal(TileIndex tile, Track track)
{
	if (!_undo_group_open) return;
	_undo_group.signals.push_back({UndoGroupPos(tile), track, SignalType::Block, SignalVariant::Electric, false, false});
}

/** Record a newly built complete NewGRF object. Never used to decide simulation state. */
void RecordBlueprintUndoObject(TileIndex tile)
{
	if (!_undo_group_open) return;
	const Object *object = Object::GetByTile(tile);
	const ObjectSpec *spec = ObjectSpec::Get(object->type);
	if (spec->grf_prop.grfid == 0) return;
	_undo_group.objects.push_back({UndoGroupPos(tile), {spec->grf_prop.grfid, spec->grf_prop.local_id},
			object->view, spec->views, static_cast<uint8_t>(object->location.w), static_cast<uint8_t>(object->location.h)});
}

/**
 * Record a rail depot that was just built.
 * @param tile Tile of the depot.
 * @param dir Direction it exits to.
 * @param railtype Rail type it was built with.
 */
void RecordBlueprintUndoRailDepot(TileIndex tile, DiagDirection dir, RailType railtype)
{
	if (!_undo_group_open) return;
	_undo_group.rail_depots.push_back({UndoGroupPos(tile), dir, railtype});
}

/**
 * Record road or tram bits that were just added to a tile.
 * @param tile Tile the bits are on.
 * @param bits Bits that were actually added; nothing is recorded when empty.
 * @param roadtype Road or tram type they belong to.
 */
void RecordBlueprintUndoRoad(TileIndex tile, RoadBits bits, RoadType roadtype)
{
	if (!_undo_group_open || bits == ROAD_NONE) return;
	_undo_group.roads.push_back({UndoGroupPos(tile), bits, roadtype});
}

/**
 * Record a road depot that was just built.
 * @param tile Tile of the depot.
 * @param dir Direction it exits to.
 * @param roadtype Road or tram type it was built with.
 */
void RecordBlueprintUndoRoadDepot(TileIndex tile, DiagDirection dir, RoadType roadtype)
{
	if (!_undo_group_open) return;
	_undo_group.road_depots.push_back({UndoGroupPos(tile), dir, roadtype});
}

/**
 * Record a tunnel or bridge that was just built.
 *
 * Only the two ends are recorded; clearing either takes the whole span with it,
 * and the span itself belongs to no tile the undo needs to name.
 *
 * @param tile One end of the span.
 * @param other_end The other end.
 * @param is_bridge True for a bridge, false for a tunnel.
 * @param transport What it carries; the undo filters need it to tell a rail
 *                  bridge from a road one.
 */
void RecordBlueprintUndoTunnelBridge(TileIndex tile, TileIndex other_end, bool is_bridge, TransportType transport)
{
	if (!_undo_group_open) return;
	BlueprintTunnelBridge tb{};
	tb.offset = UndoGroupPos(tile);
	tb.other_end = UndoGroupPos(other_end);
	tb.is_bridge = is_bridge;
	tb.transport = transport;
	_undo_group.tunnel_bridges.push_back(tb);
}

/**
 * Record a canal tile that was just dug.
 * @param tile Tile that became a canal.
 */
void RecordBlueprintUndoCanal(TileIndex tile)
{
	if (!_undo_group_open) return;
	_undo_group.canals.push_back({UndoGroupPos(tile)});
}

/**
 * Record a lock that was just built.
 * @param tile Middle tile of the lock.
 * @param dir Direction of the lock.
 */
void RecordBlueprintUndoLock(TileIndex tile, DiagDirection dir)
{
	if (!_undo_group_open) return;
	_undo_group.locks.push_back({UndoGroupPos(tile), dir});
}

/**
 * Record a ship depot that was just built.
 * @param tile Northern tile of the depot.
 * @param axis Axis of the depot.
 */
void RecordBlueprintUndoShipDepot(TileIndex tile, Axis axis)
{
	if (!_undo_group_open) return;
	_undo_group.ship_depots.push_back({UndoGroupPos(tile), axis});
}

/**
 * Record a buoy that was just built.
 * @param tile Tile of the buoy.
 */
void RecordBlueprintUndoBuoy(TileIndex tile)
{
	if (!_undo_group_open) return;
	_undo_group.buoys.push_back({UndoGroupPos(tile)});
}

/**
 * Record a dock that was just built.
 * @param tile Land tile of the dock.
 * @param dir Direction of the dock.
 */
void RecordBlueprintUndoDock(TileIndex tile, DiagDirection dir)
{
	if (!_undo_group_open) return;
	_undo_group.docks.push_back({UndoGroupPos(tile), dir, StationID::Invalid()});
}

/**
 * Record an airport that was just built.
 * @param tile Northern tile of the airport.
 * @param type Airport type.
 * @param layout Airport layout.
 * @param w Size along the X axis.
 * @param h Size along the Y axis.
 */
void RecordBlueprintUndoAirport(TileIndex tile, uint8_t type, uint8_t layout, uint8_t rotation, uint8_t air_type, uint16_t w, uint16_t h)
{
	if (!_undo_group_open) return;
	_undo_group.airports.push_back({UndoGroupPos(tile), type, layout, rotation, air_type, w, h, StationID::Invalid(), {}});
}

void RecordBlueprintUndoCustomAirportTile(TileIndex tile, AirType air_type)
{
	if (!_undo_group_open) return;

	StationID station = IsAirportTile(tile) ? GetStationIndex(tile) : StationID::Invalid();
	for (BlueprintAirport &v : _undo_group.airports) {
		if (v.type != AT_CUSTOM || v.station != station) continue;
		for (const BlueprintAirportTile &apt : v.tiles) {
			if (apt.offset == UndoGroupPos(tile)) return; // already recorded
		}
		v.tiles.push_back({UndoGroupPos(tile), static_cast<uint8_t>(Tile(tile).m4()),
				static_cast<uint8_t>(Tile(tile).m5()), static_cast<uint8_t>(Tile(tile).m6()),
				static_cast<uint16_t>(Tile(tile).m8() & ~(0x3F << 6))});
		return;
	}
	_undo_group.airports.push_back({UndoGroupPos(tile), AT_CUSTOM, 0,
			static_cast<uint8_t>(to_underlying(Direction::N)), static_cast<uint8_t>(air_type), 1, 1, station,
			{{UndoGroupPos(tile), static_cast<uint8_t>(Tile(tile).m4()), static_cast<uint8_t>(Tile(tile).m5()),
					static_cast<uint8_t>(Tile(tile).m6()), static_cast<uint16_t>(Tile(tile).m8() & ~(0x3F << 6))}}});
}

/**
 * Record one tile of a rail station or waypoint that was just built.
 *
 * Tiles are recorded individually and regrouped into rectangles when the undo
 * runs, which is how the removal commands take them.
 *
 * @param tile The tile.
 * @param is_waypoint True for a waypoint tile, false for a station tile.
 * @param axis Axis of the platform.
 * @param railtype Rail type of the platform.
 */
void RecordBlueprintUndoStationTile(TileIndex tile, bool is_waypoint, Axis axis, RailType railtype)
{
	if (!_undo_group_open) return;
	_undo_group.station_tiles.push_back({UndoGroupPos(tile), is_waypoint, axis, railtype, StationID::Invalid()});
}

/**
 * Record a road stop or road waypoint that was just built.
 *
 * The road underneath is recorded separately, where the stop laid it: an undo
 * removes the stop without touching the road, and only what the stop itself
 * brought may come out with it.
 *
 * @param tile Tile of the stop.
 * @param type StationType::Bus, StationType::Truck or StationType::RoadWaypoint.
 * @param drive_through Whether it is a drive-through stop.
 * @param dir Entrance direction (bay stops).
 * @param axis Axis (drive-through stops and waypoints).
 */
void RecordBlueprintUndoRoadStop(TileIndex tile, StationType type, bool drive_through, DiagDirection dir, Axis axis)
{
	if (!_undo_group_open) return;
	_undo_group.road_stops.push_back({UndoGroupPos(tile), type, drive_through, dir, axis, INVALID_ROADTYPE, INVALID_ROADTYPE, StationID::Invalid()});
}

/**
 * Record a corner that is about to be moved, and the height it has now.
 *
 * The first height seen for a corner wins: a single action can move the same
 * corner more than once -- a paste sweeps the area repeatedly -- and it is the
 * height before the action started that has to come back.
 *
 * @param corner Tile whose northern corner is moving.
 * @param height_before Its height before this action.
 */
void RecordBlueprintUndoTerraform(TileIndex corner, uint8_t height_before)
{
	if (!_undo_group_open) return;
	_undo_group_terraform.emplace(corner, height_before);
}

/** Forget every undoable paste; called when a game is started or loaded. */
void ClearBlueprintUndoStack()
{
	for (std::vector<BlueprintUndoRecord> &stack : _blueprint_undo_stacks) stack.clear();
}

/**
 * Get the most recent undoable paste of the local company.
 *
 * Unlike the recording side this is a client-side query, so it asks about
 * #_local_company: it exists to drive this client's toolbar, not a command.
 * Expired records are dropped here, which is also what keeps them from sitting
 * in memory once the toolbar is open.
 *
 * @return The most recent record, or \c nullptr when there is nothing left to undo.
 */
const BlueprintUndoRecord *GetBlueprintUndoRecord()
{
	if (_local_company.base() >= MAX_COMPANIES) return nullptr;

	std::vector<BlueprintUndoRecord> &stack = _blueprint_undo_stacks[_local_company.base()];
	PruneBlueprintUndoStack(stack);
	return stack.empty() ? nullptr : &stack.back();
}

/**
 * Get everything the local company can still undo, oldest first.
 *
 * Expired records are dropped here, so the caller always sees a live list.
 *
 * @return The stack; empty when there is nothing to undo.
 */
const std::vector<BlueprintUndoRecord> &GetBlueprintUndoStack()
{
	static const std::vector<BlueprintUndoRecord> none;
	if (_local_company.base() >= MAX_COMPANIES) return none;

	std::vector<BlueprintUndoRecord> &stack = _blueprint_undo_stacks[_local_company.base()];
	PruneBlueprintUndoStack(stack);
	return stack;
}

/**
 * List every map tile a record covers, for highlighting it on the map.
 * @param record The record to walk.
 * @param[in,out] tiles Tiles are appended; duplicates are possible.
 */
void CollectBlueprintUndoTiles(const BlueprintUndoRecord &record, const BlueprintUndoFilters &filters, std::vector<TileIndex> &tiles)
{
	const Blueprint &bp = record.built;
	TileIndex origin = record.origin;

	auto add = [&](TileIndexDiffC offset) {
		if (TileX(origin) + offset.x > Map::MaxX() || TileY(origin) + offset.y > Map::MaxY()) return;
		tiles.push_back(TileAddXY(origin, offset.x, offset.y));
	};
	if (filters.objects) for (const BlueprintObject &v : bp.objects) {
		for (uint y = 0; y < v.h; ++y) for (uint x = 0; x < v.w; ++x) {
			add({static_cast<int16_t>(v.offset.x + x), static_cast<int16_t>(v.offset.y + y)});
		}
	}

	/* Gated exactly as #FilterBlueprintUndoData gates the removal itself, so the
	 * tiles marked on the map are the tiles an undo would actually touch. */
	if (filters.rail) {
		for (const BlueprintRailTrack &v : bp.rail_tracks) add(v.offset);
		for (const BlueprintSignal &v : bp.signals) add(v.offset);
		for (const BlueprintRailDepot &v : bp.rail_depots) add(v.offset);
		for (const BlueprintStationTile &v : bp.station_tiles) add(v.offset);
	}
	if (filters.road) {
		for (const BlueprintRoad &v : bp.roads) add(v.offset);
		for (const BlueprintRoadDepot &v : bp.road_depots) add(v.offset);
		for (const BlueprintRoadStop &v : bp.road_stops) add(v.offset);
	}
	if (filters.water) {
		for (const BlueprintCanal &v : bp.canals) add(v.offset);
		for (const BlueprintBuoy &v : bp.buoys) add(v.offset);
	}

	/* The kinds that cover more than the tile they are anchored at. */
	for (const BlueprintTunnelBridge &v : bp.tunnel_bridges) {
		/* A bridge or tunnel belongs to whatever it carries. */
		switch (v.transport) {
			case TransportType::Rail: if (!filters.rail) continue; break;
			case TransportType::Road: if (!filters.road) continue; break;
			case TransportType::Water: if (!filters.water) continue; break;
			default: break;
		}
		add(v.offset);
		add(v.other_end);
	}
	if (filters.water) {
		for (const BlueprintLock &v : bp.locks) {
			TileIndexDiffC d = TileIndexDiffCByDiagDir(v.dir);
			add({static_cast<int16_t>(v.offset.x - d.x), static_cast<int16_t>(v.offset.y - d.y)});
			add(v.offset);
			add({static_cast<int16_t>(v.offset.x + d.x), static_cast<int16_t>(v.offset.y + d.y)});
		}
		for (const BlueprintShipDepot &v : bp.ship_depots) {
			add(v.offset);
			add({static_cast<int16_t>(v.offset.x + (v.axis == Axis::X ? 1 : 0)), static_cast<int16_t>(v.offset.y + (v.axis == Axis::Y ? 1 : 0))});
		}
		for (const BlueprintDock &v : bp.docks) {
			TileIndexDiffC d = TileIndexDiffCByDiagDir(v.dir);
			add(v.offset);
			add({static_cast<int16_t>(v.offset.x + d.x), static_cast<int16_t>(v.offset.y + d.y)});
		}
	}
	if (filters.air) {
		for (const BlueprintAirport &v : bp.airports) {
			if (!v.tiles.empty()) {
				for (const BlueprintAirportTile &apt : v.tiles) add(apt.offset);
				continue;
			}
			for (uint dy = 0; dy < v.h; dy++) {
				for (uint dx = 0; dx < v.w; dx++) {
					add({static_cast<int16_t>(v.offset.x + dx), static_cast<int16_t>(v.offset.y + dy)});
				}
			}
		}
	}

	/* Restoring the landscape covers the whole recorded area, not any one tile,
	 * so an action that only moved land would otherwise highlight nothing. */
	if (!bp.corner_heights.empty()) {
		for (uint dy = 0; dy < bp.height; dy++) {
			for (uint dx = 0; dx < bp.width; dx++) {
				add({static_cast<int16_t>(dx), static_cast<int16_t>(dy)});
			}
		}
	}
}

/**
 * Keep only the kinds of infrastructure the filters allow.
 * @param[in,out] built What was built; the disallowed kinds are dropped.
 * @param filters Kinds an undo may remove.
 */
void FilterBlueprintUndoData(Blueprint &built, const BlueprintUndoFilters &filters)
{
	if (!filters.rail) {
		built.rail_tracks.clear();
		built.signals.clear();
		built.rail_depots.clear();
		built.station_tiles.clear();
	}
	if (!filters.road) {
		built.roads.clear();
		built.road_depots.clear();
		built.road_stops.clear();
	}
	if (!filters.water) {
		built.canals.clear();
		built.locks.clear();
		built.ship_depots.clear();
		built.docks.clear();
		built.buoys.clear();
	}
	if (!filters.air) built.airports.clear();
	if (!filters.objects) built.objects.clear();

	/* A bridge or tunnel belongs to whatever it carries. */
	std::erase_if(built.tunnel_bridges, [&](const BlueprintTunnelBridge &tb) {
		switch (tb.transport) {
			case TransportType::Rail: return !filters.rail;
			case TransportType::Road: return !filters.road;
			case TransportType::Water: return !filters.water;
			default: return false;
		}
	});
}

/** Resolve an object by its stable GRF identity, independent of the local object index. */
const ObjectSpec *ResolveBlueprintObject(const BlueprintSpecRef &spec)
{
	if (spec.grfid == 0) return nullptr;
	return ObjectClass::GetByGrf(spec.grfid, spec.local_id);
}

/** Match the whole object before clearing it; never remove an unrelated replacement. */
static bool BlueprintObjectMatches(TileIndex tile, const BlueprintObject &v)
{
	if (!IsTileType(tile, TileType::Object)) return false;
	const Object *object = Object::GetByTile(tile);
	const ObjectSpec *spec = ObjectSpec::Get(object->type);
	return object->location.tile == tile && object->location.w == v.w && object->location.h == v.h &&
			object->view == v.view && spec->grf_prop.grfid == v.spec.grfid && spec->grf_prop.local_id == v.spec.local_id;
}

/**
 * Drop from a record everything an undo has actually taken off the map.
 * Unremoved items stay undoable when only part of an action succeeded.
 * @param[in,out] record Record to trim.
 */
static void TrimBlueprintUndoRecord(BlueprintUndoRecord &record)
{
	Blueprint &bp = record.built;
	TileIndex origin = record.origin;

	auto on_map = [&](TileIndexDiffC offset) {
		return TileX(origin) + offset.x <= Map::MaxX() && TileY(origin) + offset.y <= Map::MaxY();
	};
	auto target = [&](TileIndexDiffC offset) { return TileAddXY(origin, offset.x, offset.y); };
	auto ours = [&](TileIndex tile) { return IsTileOwner(tile, _local_company); };
	std::erase_if(bp.objects, [&](const BlueprintObject &v) {
		return !on_map(v.offset) || !ours(target(v.offset)) || !BlueprintObjectMatches(target(v.offset), v);
	});

	std::erase_if(bp.rail_tracks, [&](const BlueprintRailTrack &v) {
		if (!on_map(v.offset)) return true;
		TileIndex t = target(v.offset);
		return !(IsPlainRailTile(t) && ours(t) && HasTrack(t, v.track));
	});
	std::erase_if(bp.signals, [&](const BlueprintSignal &v) {
		if (!on_map(v.offset)) return true;
		TileIndex t = target(v.offset);
		return !(IsTileType(t, TileType::Railway) && ours(t) && HasSignalOnTrack(t, v.track));
	});
	std::erase_if(bp.rail_depots, [&](const BlueprintRailDepot &v) {
		return !on_map(v.offset) || !IsRailDepotTile(target(v.offset)) || !ours(target(v.offset));
	});
	std::erase_if(bp.road_depots, [&](const BlueprintRoadDepot &v) {
		return !on_map(v.offset) || !IsRoadDepotTile(target(v.offset)) || !ours(target(v.offset));
	});
	std::erase_if(bp.roads, [&](const BlueprintRoad &v) {
		if (!on_map(v.offset)) return true;
		return (GetAnyRoadBits(target(v.offset), GetRoadTramType(v.roadtype)) & v.bits) == ROAD_NONE;
	});
	std::erase_if(bp.tunnel_bridges, [&](const BlueprintTunnelBridge &v) {
		return !on_map(v.offset) || !IsTileType(target(v.offset), TileType::TunnelBridge) || !ours(target(v.offset));
	});
	std::erase_if(bp.station_tiles, [&](const BlueprintStationTile &v) {
		if (!on_map(v.offset)) return true;
		TileIndex t = target(v.offset);
		return !((v.is_waypoint ? IsRailWaypointTile(t) : IsRailStationTile(t)) && ours(t));
	});
	std::erase_if(bp.road_stops, [&](const BlueprintRoadStop &v) {
		if (!on_map(v.offset)) return true;
		TileIndex t = target(v.offset);
		return !(IsAnyRoadStopTile(t) && ours(t));
	});
	std::erase_if(bp.canals, [&](const BlueprintCanal &v) {
		if (!on_map(v.offset)) return true;
		TileIndex t = target(v.offset);
		return !(IsTileType(t, TileType::Water) && IsCanal(t) && ours(t));
	});
	std::erase_if(bp.locks, [&](const BlueprintLock &v) {
		if (!on_map(v.offset)) return true;
		TileIndex t = target(v.offset);
		return !(IsTileType(t, TileType::Water) && IsLock(t) && ours(t));
	});
	std::erase_if(bp.ship_depots, [&](const BlueprintShipDepot &v) {
		return !on_map(v.offset) || !IsShipDepotTile(target(v.offset)) || !ours(target(v.offset));
	});
	std::erase_if(bp.docks, [&](const BlueprintDock &v) {
		return !on_map(v.offset) || !IsDockTile(target(v.offset)) || !ours(target(v.offset));
	});
	std::erase_if(bp.buoys, [&](const BlueprintBuoy &v) {
		return !on_map(v.offset) || !IsBuoyTile(target(v.offset));
	});
	std::erase_if(bp.airports, [&](BlueprintAirport &v) {
		if (!v.tiles.empty()) {
			std::erase_if(v.tiles, [&](const BlueprintAirportTile &apt) {
				TileIndex t = target(apt.offset);
				return !on_map(apt.offset) || !IsAirportTile(t) || !ours(t);
			});
			return v.tiles.empty();
		}
		return !on_map(v.offset) || !IsAirportTile(target(v.offset)) || !ours(target(v.offset));
	});

	/* The landscape is done with only once every corner is back where it was. */
	if (!bp.corner_heights.empty()) {
		bool restored = true;
		for (uint cy = 0; cy <= bp.height && restored; cy++) {
			for (uint cx = 0; cx <= bp.width; cx++) {
				if (TileX(origin) + cx > Map::MaxX() || TileY(origin) + cy > Map::MaxY()) continue;
				if (TileHeight(TileAddXY(origin, cx, cy)) != bp.corner_heights[cy * (bp.width + 1) + cx]) {
					restored = false;
					break;
				}
			}
		}
		if (restored) bp.corner_heights.clear();
	}
}

/**
 * Take out of a record what an undo has just removed from the map.
 *
 * With every filter on this drops the whole record. With some off it keeps what
 * was filtered out, so that the rest stays undoable afterwards rather than
 * being stranded: undoing only the rail of a mixed action must not throw the
 * road away with it.
 *
 * Matched on the origin rather than simply taking the newest entry, because an
 * action performed between posting an undo and that undo coming back would
 * otherwise be the one consumed.
 *
 * @param origin Origin of the record that was undone.
 * @param filters Kinds the undo was allowed to remove.
 */
void ConsumeBlueprintUndoRecord(TileIndex origin, const BlueprintUndoFilters &filters)
{
	if (_local_company.base() >= MAX_COMPANIES) return;

	std::vector<BlueprintUndoRecord> &stack = _blueprint_undo_stacks[_local_company.base()];
	for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
		if (it->origin != origin) continue;

		auto forward = std::next(it).base();

		if (!filters.All()) {
			/* Keep exactly the kinds the undo was not allowed to touch. */
			BlueprintUndoFilters kept;
			kept.rail = !filters.rail;
			kept.road = !filters.road;
			kept.water = !filters.water;
			kept.air = !filters.air;
			kept.objects = !filters.objects;
			FilterBlueprintUndoData(forward->built, kept);
			forward->built.corner_heights.clear();
			if (!forward->built.IsEmpty()) return;
			stack.erase(forward);
			return;
		}

		TrimBlueprintUndoRecord(*forward);
		if (forward->IsEmpty()) stack.erase(forward);
		return;
	}
}

/** Running state while pasting or undoing: available money and the accumulated result. */
struct BlueprintCommandState {
	Money money;                              ///< Money still available for further parts.
	CommandCost total{ExpensesType::Construction}; ///< Accumulated cost of the parts handled so far.
	bool out_of_money = false;                ///< Whether work stopped due to lack of money.
	bool any_success = false;                 ///< Whether any part could be handled.
};

/** Get the #CommandCost from a command result that may carry additional return values. */
template <typename T>
static const CommandCost &BlueprintCmdCost(const T &result)
{
	if constexpr (std::is_same_v<std::decay_t<T>, CommandCost>) {
		return result;
	} else {
		return std::get<0>(result);
	}
}

/**
 * Handle one part of a blueprint: test it, check the money, then execute.
 * Used for both building a part and removing it again; parts that do not work
 * out are skipped without error spam.
 * @return Whether this part succeeded (or would succeed, when only testing).
 */
template <Commands Tcmd, typename... Targs>
static bool BlueprintDoCommand(BlueprintCommandState &state, DoCommandFlags flags, Targs... args)
{
	if (state.out_of_money) return false;

	auto test_result = Command<Tcmd>::Do(DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), args...);
	const CommandCost &test_cost = BlueprintCmdCost(test_result);
	if (test_cost.Failed()) return false;

	if (!flags.Test(DoCommandFlag::Execute)) {
		state.total.AddCost(test_cost.GetCost());
		state.any_success = true;
		return true;
	}

	if (test_cost.GetCost() > 0 && state.money < test_cost.GetCost()) {
		state.out_of_money = true;
		return false;
	}

	auto result = Command<Tcmd>::Do(flags, args...);
	const CommandCost &cost = BlueprintCmdCost(result);
	if (cost.Failed()) return false;

	state.money -= cost.GetCost();
	state.total.AddCost(cost.GetCost());
	state.any_success = true;
	return true;
}

/** Resolve a stable signal identity against the current game's style slots. */
std::optional<uint8_t> ResolveBlueprintSignalStyle(const BlueprintSpecRef &spec)
{
	if (spec.grfid == 0) return 0;
	for (uint8_t i = 0; i < _num_new_signal_styles; ++i) {
		const auto &mapping = _new_signal_style_mapping[i];
		if (mapping.grfid == spec.grfid && mapping.grf_local_id == spec.local_id &&
				_new_signal_styles[i].grffile != nullptr && HasBit(_enabled_new_signal_styles_mask, i + 1)) return i + 1;
	}
	return std::nullopt;
}

/**
 * Paste a blueprint onto the map.
 *
 * Landscape first: depending on the terraform mode, the tile corners of the
 * target area are raised/lowered towards the copied landscape (shifted by the
 * height offset). Then the infrastructure is built; parts that do not fit are
 * silently skipped. The blueprint is expected to be pre-transformed and
 * pre-filtered (see #PrepareBlueprintPaste).
 *
 * @param flags Type of operation.
 * @param origin Northern tile of the target area.
 * @param data Blueprint and landscape adjustment settings.
 * @return The cost of this operation or an error.
 */
CommandCost CmdPasteBlueprint(DoCommandFlags flags, TileIndex origin, const BlueprintPasteData &data)
{
	const Blueprint &bp = data.blueprint;

	if (origin >= Map::Size()) return CMD_ERROR;
	if (bp.width < 1 || bp.height < 1 || bp.width > MAX_BLUEPRINT_DIMENSION || bp.height > MAX_BLUEPRINT_DIMENSION) return CMD_ERROR;
	if (bp.IsEmpty()) return CMD_ERROR;
	if (data.height_offset < -MAX_BLUEPRINT_HEIGHT_OFFSET || data.height_offset > MAX_BLUEPRINT_HEIGHT_OFFSET) return CMD_ERROR;
	if (data.terraform_mode != BlueprintTerraformMode::None && data.terraform_mode != BlueprintTerraformMode::Minimal && data.terraform_mode != BlueprintTerraformMode::Full) return CMD_ERROR;
	if (data.terraform_mode != BlueprintTerraformMode::None && bp.corner_heights.size() != static_cast<size_t>(bp.width + 1) * (bp.height + 1)) return CMD_ERROR;
	if (!BlueprintOffsetsValid(bp)) return CMD_ERROR;
	for (const BlueprintObject &object : bp.objects) {
		const ObjectSpec *spec = ResolveBlueprintObject(object.spec);
		if (spec == nullptr || !spec->IsEnabled()) return CommandCost(STR_BLUEPRINT_ERROR_MISSING_OBJECT);
		if (object.view >= spec->views || object.w != GB(spec->size, (object.view & 1) ? 4 : 0, 4) ||
				object.h != GB(spec->size, (object.view & 1) ? 0 : 4, 4)) return CommandCost(STR_BLUEPRINT_ERROR_OBJECT_ORIENTATION);
	}
	for (const BlueprintSignal &sig : bp.signals) {
		if (!ResolveBlueprintSignalStyle(sig.spec).has_value()) return CommandCost(STR_BLUEPRINT_ERROR_MISSING_SIGNAL_STYLE);
	}
	/* Check required styles before changing any landscape or building tracks. */
	for (const BlueprintStationTile &st : bp.station_tiles) {
		if (st.spec.grfid != 0 && StationClass::GetByGrf(st.spec.grfid, st.spec.local_id) == nullptr) return CommandCost(STR_BLUEPRINT_ERROR_MISSING_NEWGRF);
	}
	for (const BlueprintRoadStop &stop : bp.road_stops) {
		if (stop.spec.grfid != 0 && RoadStopClass::GetByGrf(stop.spec.grfid, stop.spec.local_id) == nullptr) return CommandCost(STR_BLUEPRINT_ERROR_MISSING_NEWGRF);
	}

	std::vector<BlueprintStationRect> station_rects = GroupStationTiles(bp);

	BlueprintCommandState state{GetAvailableMoneyForCommand()};

	/* Is the tile at this offset (still) on the map? */
	auto tile_valid = [&](TileIndexDiffC offset) {
		return TileX(origin) + offset.x <= Map::MaxX() && TileY(origin) + offset.y <= Map::MaxY();
	};
	auto target_tile = [&](TileIndexDiffC offset) {
		return TileAddXY(origin, offset.x, offset.y);
	};

	/* What actually goes in, so that this paste can be undone later. Filled on
	 * every run and only handed to the undo stack when this run really executes;
	 * a discarded record on a cost-estimate run is cheaper than gating each of
	 * the build sites below. */
	auto paste_road = [&](TileIndexDiffC offset, RoadBits bits, RoadType roadtype) {
		return BlueprintDoCommand<CMD_BUILD_ROAD>(state, flags, target_tile(offset), bits, roadtype, DisallowedRoadDirections{}, TownID::Invalid(), BuildRoadFlags{});
	};

	if (data.terraform_mode != BlueprintTerraformMode::None) {
		/* Determine which corners to terraform. */
		std::vector<bool> terraform_corner(static_cast<size_t>(bp.width + 1) * (bp.height + 1), data.terraform_mode == BlueprintTerraformMode::Full);
		if (data.terraform_mode == BlueprintTerraformMode::Minimal) {
			/* Mark the four corners of every tile that receives an element. */
			auto touch = [&](int x, int y) {
				if (x < 0 || y < 0 || x >= bp.width || y >= bp.height) return;
				terraform_corner[y * (bp.width + 1) + x] = true;
				terraform_corner[y * (bp.width + 1) + x + 1] = true;
				terraform_corner[(y + 1) * (bp.width + 1) + x] = true;
				terraform_corner[(y + 1) * (bp.width + 1) + x + 1] = true;
			};

			for (const BlueprintObject &v : bp.objects) {
				for (uint y = 0; y < v.h; ++y) for (uint x = 0; x < v.w; ++x) touch(v.offset.x + x, v.offset.y + y);
			}
			for (const BlueprintRailTrack &rt : bp.rail_tracks) touch(rt.offset.x, rt.offset.y);
			for (const BlueprintRailDepot &depot : bp.rail_depots) touch(depot.offset.x, depot.offset.y);
			for (const BlueprintRoad &road : bp.roads) touch(road.offset.x, road.offset.y);
			for (const BlueprintRoadDepot &depot : bp.road_depots) touch(depot.offset.x, depot.offset.y);
			for (const BlueprintCanal &canal : bp.canals) touch(canal.offset.x, canal.offset.y);
			for (const BlueprintLock &lock : bp.locks) {
				TileIndexDiffC d = TileIndexDiffCByDiagDir(lock.dir);
				touch(lock.offset.x - d.x, lock.offset.y - d.y);
				touch(lock.offset.x, lock.offset.y);
				touch(lock.offset.x + d.x, lock.offset.y + d.y);
			}
			for (const BlueprintShipDepot &depot : bp.ship_depots) {
				touch(depot.offset.x, depot.offset.y);
				touch(depot.offset.x + (depot.axis == Axis::X ? 1 : 0), depot.offset.y + (depot.axis == Axis::Y ? 1 : 0));
			}
			for (const BlueprintTunnelBridge &tb : bp.tunnel_bridges) {
				/* Only the two end tiles matter; the span stays untouched. */
				touch(tb.offset.x, tb.offset.y);
				touch(tb.other_end.x, tb.other_end.y);
			}
			for (const BlueprintStationRect &rect : station_rects) {
				for (uint dy = 0; dy < rect.h; dy++) {
					for (uint dx = 0; dx < rect.w; dx++) touch(rect.offset.x + dx, rect.offset.y + dy);
				}
			}
			for (const BlueprintRoadStop &stop : bp.road_stops) touch(stop.offset.x, stop.offset.y);
			for (const BlueprintDock &dock : bp.docks) {
				TileIndexDiffC d = TileIndexDiffCByDiagDir(dock.dir);
				touch(dock.offset.x, dock.offset.y);
				touch(dock.offset.x + d.x, dock.offset.y + d.y);
			}
			for (const BlueprintAirport &airport : bp.airports) {
				for (uint dy = 0; dy < airport.h; dy++) {
					for (uint dx = 0; dx < airport.w; dx++) touch(airport.offset.x + dx, airport.offset.y + dy);
				}
			}
		}

		/* The copied height at the target's north corner is pasted at the height of the
		 * pointed-at corner, shifted by the height offset; everything else follows. */
		int height_base = TileHeight(origin) + data.height_offset - bp.corner_heights[0];
		int height_limit = _settings_game.construction.map_height_limit;

		/* Terraform commands move a corner by at most one height level, so
		 * sweep the area repeatedly until nothing changes any more. */
		for (uint pass = 0; pass <= MAX_TILE_HEIGHT; pass++) {
			bool changed = false;
			for (uint cy = 0; cy <= bp.height; cy++) {
				for (uint cx = 0; cx <= bp.width; cx++) {
					if (!terraform_corner[cy * (bp.width + 1) + cx]) continue;
					if (TileX(origin) + cx > Map::MaxX() || TileY(origin) + cy > Map::MaxY()) continue;

					TileIndex corner_tile = TileAddXY(origin, cx, cy);
					int wanted = Clamp(bp.corner_heights[cy * (bp.width + 1) + cx] + height_base, 0, height_limit);
					uint current = TileHeight(corner_tile);
					if (static_cast<int>(current) == wanted) continue;

					BlueprintDoCommand<CMD_TERRAFORM_LAND>(state, flags, corner_tile, SLOPE_N, static_cast<int>(current) < wanted);
					if (TileHeight(corner_tile) != current) changed = true;
				}
			}
			if (!changed) break;
		}
	}

	/* Build everything; parts that do not fit are skipped without error spam.
	 * Water before rail/road (locks and canals form the bed for ship depots),
	 * tracks before signals, stations and airports last. */
	for (const BlueprintCanal &canal : bp.canals) {
		if (!tile_valid(canal.offset)) continue;
		BlueprintDoCommand<CMD_BUILD_CANAL>(state, flags, target_tile(canal.offset), target_tile(canal.offset), WaterClass::Canal, false);
	}
	for (const BlueprintLock &lock : bp.locks) {
		if (!tile_valid(lock.offset)) continue;
		BlueprintDoCommand<CMD_BUILD_LOCK>(state, flags, target_tile(lock.offset));
	}
	for (const BlueprintShipDepot &depot : bp.ship_depots) {
		if (!tile_valid(depot.offset)) continue;
		BlueprintDoCommand<CMD_BUILD_SHIP_DEPOT>(state, flags, target_tile(depot.offset), depot.axis);
	}

	for (const BlueprintRailTrack &rt : bp.rail_tracks) {
		if (!tile_valid(rt.offset)) continue;
		BlueprintDoCommand<CMD_BUILD_SINGLE_RAIL>(state, flags, target_tile(rt.offset), rt.railtype, rt.track, BuildRailTrackFlags{});
	}
	for (const BlueprintSignal &sig : bp.signals) {
		if (!tile_valid(sig.offset)) continue;
		Trackdir td = TrackToTrackdir(sig.track);
		uint8_t signals_copy = (sig.along ? SignalAlongTrackdir(td) : 0) | (sig.against ? SignalAgainstTrackdir(td) : 0);
		if (signals_copy == 0) continue;

		TileIndex signal_tile = target_tile(sig.offset);
		/* No Convert flag: the paste lays the track first, so there is never an
		 * existing signal to convert and Convert would be rejected as "no signals". */
		BlueprintDoCommand<CMD_BUILD_SINGLE_SIGNAL>(state, flags, signal_tile, sig.track, sig.type, sig.variant,
				*ResolveBlueprintSignalStyle(sig.spec), 1, BuildSignalFlags::None, SignalCycleGroups{}, 0, signals_copy);
	}
	for (const BlueprintRailDepot &depot : bp.rail_depots) {
		if (!tile_valid(depot.offset)) continue;
		BlueprintDoCommand<CMD_BUILD_TRAIN_DEPOT>(state, flags, target_tile(depot.offset), depot.railtype, depot.dir);
	}

	for (const BlueprintTunnelBridge &tb : bp.tunnel_bridges) {
		if (!tile_valid(tb.offset) || !tile_valid(tb.other_end)) continue;

		uint8_t road_rail_type = 0;
		switch (tb.transport) {
			case TransportType::Rail: road_rail_type = tb.railtype; break;
			case TransportType::Road: road_rail_type = tb.roadtype != INVALID_ROADTYPE ? tb.roadtype : tb.tramtype; break;
			default: break;
		}

		bool built;
		if (tb.is_bridge) {
			built = BlueprintDoCommand<CMD_BUILD_BRIDGE>(state, flags, target_tile(tb.other_end), target_tile(tb.offset), tb.transport, tb.bridge_type, road_rail_type, BuildBridgeFlags{});
		} else {
			built = BlueprintDoCommand<CMD_BUILD_TUNNEL>(state, flags, target_tile(tb.offset), tb.transport, road_rail_type);
		}

		/* Bridges/tunnels carrying both road and tram: add the second type across the span. */
		if (built && tb.transport == TransportType::Road && tb.roadtype != INVALID_ROADTYPE && tb.tramtype != INVALID_ROADTYPE) {
			Axis axis = tb.offset.x == tb.other_end.x ? Axis::Y : Axis::X;
			paste_road(tb.offset, AxisToRoadBits(axis), tb.tramtype);
		}
	}

	for (const BlueprintRoad &road : bp.roads) {
		if (!tile_valid(road.offset)) continue;
		paste_road(road.offset, road.bits, road.roadtype);
	}
	for (const BlueprintRoadDepot &depot : bp.road_depots) {
		if (!tile_valid(depot.offset)) continue;
		BlueprintDoCommand<CMD_BUILD_ROAD_DEPOT>(state, flags, target_tile(depot.offset), depot.roadtype, depot.dir);
	}

	/* Parts of one original station are joined into one new station again. */
	std::map<StationID, StationID> new_station_ids;
	auto join_target = [&](StationID original) {
		auto it = new_station_ids.find(original);
		return it == new_station_ids.end() ? NEW_STATION : it->second;
	};
	auto register_new_station = [&](StationID original, TileIndexDiffC offset) {
		if (!flags.Test(DoCommandFlag::Execute) || new_station_ids.contains(original)) return;
		TileIndex tile = target_tile(offset);
		if (IsTileType(tile, TileType::Station)) new_station_ids[original] = GetStationIndex(tile);
	};

	for (const BlueprintStationRect &rect : station_rects) {
		if (!tile_valid(rect.offset)) continue;
		StationClassID station_class = rect.is_waypoint ? STAT_CLASS_WAYP : STAT_CLASS_DFLT;
		uint16_t station_type = 0;
		if (rect.spec.grfid != 0) {
			const StationSpec *spec = StationClass::GetByGrf(rect.spec.grfid, rect.spec.local_id);
			if (spec == nullptr) continue; // Missing GRF: never substitute a different station.
			station_class = spec->class_index;
			station_type = spec->index;
		}
		if (rect.is_waypoint) {
			/* Waypoints can only be placed on existing straight tracks; lay those first. */
			Track track = AxisToTrack(rect.axis);
			for (uint dy = 0; dy < rect.h; dy++) {
				for (uint dx = 0; dx < rect.w; dx++) {
					TileIndexDiffC tile_offset = {static_cast<int16_t>(rect.offset.x + dx), static_cast<int16_t>(rect.offset.y + dy)};
					if (!tile_valid(tile_offset)) continue;
					BlueprintDoCommand<CMD_BUILD_SINGLE_RAIL>(state, flags, target_tile(tile_offset), rect.railtype, track, BuildRailTrackFlags{});
				}
			}

			/* One waypoint command covers one tile along the track (spanning parallel
			 * tracks), so build the rect slice by slice along its axis. */
			uint slices = rect.axis == Axis::X ? rect.w : rect.h;
			uint8_t slice_w = rect.axis == Axis::X ? 1 : static_cast<uint8_t>(rect.w);
			uint8_t slice_h = rect.axis == Axis::X ? static_cast<uint8_t>(rect.h) : 1;
			for (uint i = 0; i < slices; i++) {
				TileIndexDiffC slice = {static_cast<int16_t>(rect.offset.x + (rect.axis == Axis::X ? i : 0)),
						static_cast<int16_t>(rect.offset.y + (rect.axis == Axis::X ? 0 : i))};
				if (!tile_valid(slice)) continue;
				StationID join = join_target(rect.station);
				bool built = BlueprintDoCommand<CMD_BUILD_RAIL_WAYPOINT>(state, flags, target_tile(slice), rect.axis, slice_w, slice_h,
						station_class, station_type, join, true);
				if (!built && join != NEW_STATION) {
					/* Joining may be impossible (e.g. station spread); build separately. */
					built = BlueprintDoCommand<CMD_BUILD_RAIL_WAYPOINT>(state, flags, target_tile(slice), rect.axis, slice_w, slice_h,
							station_class, station_type, NEW_STATION, true);
				}
				if (built) register_new_station(rect.station, slice);
			}
		} else {
			uint8_t numtracks = static_cast<uint8_t>(rect.axis == Axis::X ? rect.h : rect.w);
			uint8_t plat_len = static_cast<uint8_t>(rect.axis == Axis::X ? rect.w : rect.h);
			StationID join = join_target(rect.station);

			bool built = BlueprintDoCommand<CMD_BUILD_RAIL_STATION>(state, flags, target_tile(rect.offset), rect.railtype, rect.axis, numtracks, plat_len,
					station_class, station_type, join, true);
			if (!built && join != NEW_STATION) {
				/* Joining may be impossible (e.g. distant join disabled); build separately. */
				built = BlueprintDoCommand<CMD_BUILD_RAIL_STATION>(state, flags, target_tile(rect.offset), rect.railtype, rect.axis, numtracks, plat_len,
						station_class, station_type, NEW_STATION, true);
			}
			if (built) register_new_station(rect.station, rect.offset);
		}
	}

	for (const BlueprintRoadStop &stop : bp.road_stops) {
		if (!tile_valid(stop.offset)) continue;
		RoadStopClassID stop_class = stop.type == StationType::RoadWaypoint ? ROADSTOP_CLASS_WAYP : ROADSTOP_CLASS_DFLT;
		uint16_t stop_type_index = 0;
		if (stop.spec.grfid != 0) {
			const RoadStopSpec *spec = RoadStopClass::GetByGrf(stop.spec.grfid, stop.spec.local_id);
			if (spec == nullptr) continue;
			stop_class = spec->class_index;
			stop_type_index = spec->index;
		}
		RoadType rt = stop.roadtype != INVALID_ROADTYPE ? stop.roadtype : stop.tramtype;
		DiagDirection ddir = stop.drive_through ? AxisToDiagDir(stop.axis) : stop.dir;
		StationID join = join_target(stop.station);
		bool built;

		if (stop.type == StationType::RoadWaypoint) {
			/* Road waypoints can only be placed on existing road; lay that first. */
			if (stop.roadtype != INVALID_ROADTYPE) {
				paste_road(stop.offset, AxisToRoadBits(stop.axis), stop.roadtype);
			}
			if (stop.tramtype != INVALID_ROADTYPE) {
				paste_road(stop.offset, AxisToRoadBits(stop.axis), stop.tramtype);
			}
			built = BlueprintDoCommand<CMD_BUILD_ROAD_WAYPOINT>(state, flags, target_tile(stop.offset), stop.axis, 1, 1,
					stop_class, stop_type_index, join, true);
			if (!built && join != NEW_STATION) {
				built = BlueprintDoCommand<CMD_BUILD_ROAD_WAYPOINT>(state, flags, target_tile(stop.offset), stop.axis, 1, 1,
						stop_class, stop_type_index, NEW_STATION, true);
			}
		} else {
			if (!IsValidDiagDirection(ddir)) continue;
			RoadStopType stop_type = stop.type == StationType::Truck ? RoadStopType::Truck : RoadStopType::Bus;

			built = BlueprintDoCommand<CMD_BUILD_ROAD_STOP>(state, flags, target_tile(stop.offset), 1, 1, stop_type, stop.drive_through,
					ddir, rt, stop_class, stop_type_index, join, true);
			if (!built && join != NEW_STATION) {
				built = BlueprintDoCommand<CMD_BUILD_ROAD_STOP>(state, flags, target_tile(stop.offset), 1, 1, stop_type, stop.drive_through,
						ddir, rt, stop_class, stop_type_index, NEW_STATION, true);
			}
			/* Drive-through stops carrying both road and tram: add the second type. */
			if (built && stop.drive_through && stop.roadtype != INVALID_ROADTYPE && stop.tramtype != INVALID_ROADTYPE) {
				paste_road(stop.offset, AxisToRoadBits(stop.axis), stop.tramtype);
			}
		}
		if (built) register_new_station(stop.station, stop.offset);
	}

	for (const BlueprintDock &dock : bp.docks) {
		if (!tile_valid(dock.offset)) continue;
		StationID join = join_target(dock.station);
		bool built = BlueprintDoCommand<CMD_BUILD_DOCK>(state, flags, target_tile(dock.offset), join, true);
		if (!built && join != NEW_STATION) {
			built = BlueprintDoCommand<CMD_BUILD_DOCK>(state, flags, target_tile(dock.offset), NEW_STATION, true);
		}
		if (built) register_new_station(dock.station, dock.offset);
	}
	for (const BlueprintBuoy &buoy : bp.buoys) {
		if (!tile_valid(buoy.offset)) continue;
		BlueprintDoCommand<CMD_BUILD_BUOY>(state, flags, target_tile(buoy.offset));
	}

	AirType fallback_airtype = _sorted_airtypes.empty() ? AIRTYPE_BEGIN : _sorted_airtypes.front();
	for (const BlueprintAirport &airport : bp.airports) {
		if (!tile_valid(airport.offset)) continue;
		StationID join = join_target(airport.station);

		if (!airport.tiles.empty()) {
			/* Custom (multitile) airport: create the tiles, then restore each tile's captured state. */
			AirType airtype = ValParamAirType(static_cast<AirType>(airport.air_type)) ? static_cast<AirType>(airport.air_type) : fallback_airtype;
			/* The airport commands use StationID::Invalid() for "build new", unlike the other
			 * station commands which still use NEW_STATION. */
			StationID airport_join = (join == NEW_STATION) ? StationID::Invalid() : join;
			/* Build in row-major order so every tile after the first is adjacent to one
			 * already placed; with adjacent=true that joins them all into one station,
			 * exactly like dragging the area out in the build-airport tool. Passing the
			 * station id instead would take the distant-join path (and needs its setting). */
			std::vector<BlueprintAirportTile> ordered = airport.tiles;
			std::sort(ordered.begin(), ordered.end(), [](const BlueprintAirportTile &a, const BlueprintAirportTile &b) {
				return a.offset.y != b.offset.y ? a.offset.y < b.offset.y : a.offset.x < b.offset.x;
			});
			/* Pass 1: create every tile. Restoring the captured m5/m8 has to wait, because
			 * CmdAddRemoveAirportTiles rescans and calls UpdateTracks after each tile, which
			 * masks a track pointing at a neighbour that is not built yet.
			 *
			 * Joining uses the explicit station id (distant join, on by default) rather than
			 * adjacency, so a split airport (disjoint groups of tiles) still becomes one station. */
			bool built = true;
			StationID station = airport_join;
			for (const BlueprintAirportTile &apt : ordered) {
				if (!tile_valid(apt.offset)) { built = false; continue; }
				TileIndex tile = target_tile(apt.offset);
				bool ok = BlueprintDoCommand<Commands::AddRemoveAirportTiles>(state, flags, tile, tile, true, airtype, station, false);
				if (!ok && station != StationID::Invalid()) {
					/* Distant join refused (setting off); fall back to adjacency/new station. */
					ok = BlueprintDoCommand<Commands::AddRemoveAirportTiles>(state, flags, tile, tile, true, airtype, StationID::Invalid(), false);
				}
				if (!ok) { built = false; continue; }
				/* The first tile creates the station; join the rest onto it. */
				if (flags.Test(DoCommandFlag::Execute) && station == StationID::Invalid() && IsAirportTile(tile)) {
					station = GetStationIndex(tile);
				}
			}

			/* Pass 2: now that all neighbours exist, restore each tile's captured state
			 * (gfx, tile type, rotation, tracks). Reservations start unset. */
			if (flags.Test(DoCommandFlag::Execute) && built) {
				for (const BlueprintAirportTile &apt : ordered) {
					if (!tile_valid(apt.offset)) continue;
					TileIndex tile = target_tile(apt.offset);
					if (!IsAirportTile(tile) || !IsTileOwner(tile, _current_company)) continue;
					Tile(tile).m4() = apt.m4;
					Tile(tile).m5() = apt.m5;
					/* Preserve the station type nibble (m6 bits 3-6); the rest is the
					 * airtype gfx flag and the ground counter. */
					Tile(tile).m6() = (Tile(tile).m6() & 0x78) | (apt.m6 & 0x87);
					Tile(tile).m8() = apt.m8 & ~(0x3F << 6);
					RecordBlueprintUndoCustomAirportTile(tile, airtype);
					MarkTileDirtyByTile(tile);
				}
				/* Rebuild the apron/runway/hangar structures and normalise tracks; the
				 * hangar Depot is created here, so no EnsureFreeHangar is needed. */
				Station *st = nullptr;
				for (const BlueprintAirportTile &apt : ordered) {
					if (!tile_valid(apt.offset)) continue;
					TileIndex t = target_tile(apt.offset);
					if (IsAirportTile(t)) { st = Station::GetByTile(t); break; }
				}
				if (st != nullptr && st->airport.tile != INVALID_TILE) st->UpdateAirportDataStructure();
			}
			if (built) register_new_station(airport.station, airport.offset);
			continue;
		}

		/* Spec airport: rebuild from type/layout, honouring the captured rotation and air type. */
		AirType airtype = ValParamAirType(static_cast<AirType>(airport.air_type)) ? static_cast<AirType>(airport.air_type) : fallback_airtype;
		/* jgrpp does not maintain Station::airport.rotation at runtime; fall back to north. */
		DiagDirection rotation = (airport.rotation <= 3) ? static_cast<DiagDirection>(airport.rotation) : DiagDirection::NE;
		StationID airport_join = (join == NEW_STATION) ? StationID::Invalid() : join;
		bool built = BlueprintDoCommand<CMD_BUILD_AIRPORT>(state, flags, target_tile(airport.offset), airport.type, airport.layout, airtype, rotation, airport_join, true);
		if (!built && airport_join != StationID::Invalid()) {
			built = BlueprintDoCommand<CMD_BUILD_AIRPORT>(state, flags, target_tile(airport.offset), airport.type, airport.layout, airtype, rotation, StationID::Invalid(), true);
		}
		if (built) register_new_station(airport.station, airport.offset);
	}

	for (const BlueprintObject &v : bp.objects) {
		if (!tile_valid(v.offset) || TileX(origin) + v.offset.x + v.w - 1 > Map::MaxX() ||
				TileY(origin) + v.offset.y + v.h - 1 > Map::MaxY()) continue;
		const ObjectSpec *spec = ResolveBlueprintObject(v.spec);
		BlueprintDoCommand<Commands::BuildObject>(state, flags, target_tile(v.offset), static_cast<ObjectType>(spec->Index()), v.view);
	}

	if (!state.any_success) return CMD_ERROR;
	return state.total;
}

/**
 * Remove exactly the given road bits from one tile.
 *
 * There is no single-tile road removal command; #CMD_REMOVE_LONG_ROAD is the
 * only way in. On a one-tile drag it takes the halves of one axis, selected by
 * its two half-tile flags: neither flag gives the northern half alone, both give
 * the southern half alone, and end-half alone gives the whole axis. One call per
 * axis therefore expresses any combination of bits exactly — which matters,
 * because these are the bits the paste actually added and nothing else may come
 * out along with them.
 *
 * @param state Running money and cost state.
 * @param flags Type of operation.
 * @param tile Tile to remove the bits from.
 * @param bits Road bits to remove.
 * @param roadtype Road or tram type the bits belong to.
 */
static void UndoRemoveRoadBits(BlueprintCommandState &state, DoCommandFlags flags, TileIndex tile, RoadBits bits, RoadType roadtype)
{
	for (Axis axis : {Axis::X, Axis::Y}) {
		bool has_north = (bits & (axis == Axis::X ? ROAD_NE : ROAD_NW)) != ROAD_NONE;
		bool has_south = (bits & (axis == Axis::X ? ROAD_SW : ROAD_SE)) != ROAD_NONE;
		if (!has_north && !has_south) continue;

		BlueprintDoCommand<CMD_REMOVE_LONG_ROAD>(state, flags, tile, tile, roadtype, axis, !has_north, has_south);
	}
}

/**
 * Undo a blueprint paste: remove again what it built.
 *
 * Parts are removed in the reverse of the order they were built in, so that
 * nothing is taken out from under something still standing on it. Every part is
 * checked against the map first and skipped silently when it is no longer what
 * the paste left there — built over, occupied by a vehicle, cleared by hand —
 * the same way the paste itself skips what does not fit.
 *
 * Those checks are not merely defensive for the parts that are removed by
 * clearing their tile: #CMD_LANDSCAPE_CLEAR will clear whatever it is pointed
 * at, so without them a stale record could bulldoze something the paste never
 * built. Attributes that can change without the part ceasing to be the one that
 * was built — a converted rail type, for instance — are deliberately not
 * checked.
 *
 * This is a client-supplied payload and must be assumed forgeable, which is
 * safe because every removal below runs as the issuing company through a stock
 * command, and is ownership-checked and priced there.
 *
 * @param flags Type of operation.
 * @param origin Northern tile of the pasted area.
 * @param data The parts to remove, and whether to restore the landscape too.
 * @return The cost of this operation or an error.
 */
CommandCost CmdUndoBlueprintPaste(DoCommandFlags flags, TileIndex origin, const BlueprintUndoData &data)
{
	const Blueprint &bp = data.built;

	if (origin >= Map::Size()) return CMD_ERROR;
	if (bp.width < 1 || bp.height < 1 || bp.width > MAX_BLUEPRINT_DIMENSION || bp.height > MAX_BLUEPRINT_DIMENSION) return CMD_ERROR;
	if (!bp.corner_heights.empty() && bp.corner_heights.size() != static_cast<size_t>(bp.width + 1) * (bp.height + 1)) return CMD_ERROR;
	if (!BlueprintOffsetsValid(bp)) return CMD_ERROR;

	/* Undoing is not itself an action to be undone. Without this the terraforming
	 * below reports into the open group and every land undo pushes a fresh record
	 * of its own, which then sits at the top of the history. */
	AbandonBlueprintUndoGroup();

	BlueprintCommandState state{GetAvailableMoneyForCommand()};

	/* Is the tile at this offset (still) on the map? */
	auto tile_valid = [&](TileIndexDiffC offset) {
		return TileX(origin) + offset.x <= Map::MaxX() && TileY(origin) + offset.y <= Map::MaxY();
	};
	auto target_tile = [&](TileIndexDiffC offset) {
		return TileAddXY(origin, offset.x, offset.y);
	};

	/* Reverse of the build order in #CmdPasteBlueprint, so that nothing is taken
	 * out from under something still standing on it: stations before the track
	 * and road they sit on, road stops before their road, everything before the
	 * water it was built into. */
	for (const BlueprintObject &v : bp.objects) {
		if (!tile_valid(v.offset)) continue;
		TileIndex tile = target_tile(v.offset);
		if (!IsTileOwner(tile, _current_company) || !BlueprintObjectMatches(tile, v)) continue;
		BlueprintDoCommand<CMD_LANDSCAPE_CLEAR>(state, flags, tile);
	}

	for (const BlueprintAirport &airport : bp.airports) {
		if (!tile_valid(airport.offset)) continue;
		AirType airtype = ValParamAirType(static_cast<AirType>(airport.air_type)) ? static_cast<AirType>(airport.air_type) : AIRTYPE_BEGIN;

		if (!airport.tiles.empty()) {
			/* Custom airports: remove tile by tile; the station goes away with its last tile. */
			for (const BlueprintAirportTile &apt : airport.tiles) {
				if (!tile_valid(apt.offset)) continue;
				TileIndex tile = target_tile(apt.offset);
				if (!IsAirportTile(tile) || !IsTileOwner(tile, _current_company)) continue;
				BlueprintDoCommand<Commands::AddRemoveAirportTiles>(state, flags, tile, tile, false, airtype, StationID::Invalid(), false);
			}
			continue;
		}

		TileIndex tile = target_tile(airport.offset);
		if (!IsAirportTile(tile) || !IsTileOwner(tile, _current_company)) continue;
		BlueprintDoCommand<CMD_LANDSCAPE_CLEAR>(state, flags, tile);
	}

	for (const BlueprintBuoy &buoy : bp.buoys) {
		if (!tile_valid(buoy.offset)) continue;
		TileIndex tile = target_tile(buoy.offset);
		/* Buoys belong to no one, so there is no ownership to check here. */
		if (!IsBuoyTile(tile)) continue;
		BlueprintDoCommand<CMD_LANDSCAPE_CLEAR>(state, flags, tile);
	}

	for (const BlueprintDock &dock : bp.docks) {
		if (!tile_valid(dock.offset)) continue;
		TileIndex tile = target_tile(dock.offset);
		if (!IsDockTile(tile) || !IsTileOwner(tile, _current_company)) continue;
		BlueprintDoCommand<CMD_LANDSCAPE_CLEAR>(state, flags, tile);
	}

	for (const BlueprintRoadStop &stop : bp.road_stops) {
		if (!tile_valid(stop.offset)) continue;
		TileIndex tile = target_tile(stop.offset);
		if (!IsTileOwner(tile, _current_company)) continue;

		if (stop.type == StationType::RoadWaypoint) {
			if (!IsRoadWaypointTile(tile)) continue;
			BlueprintDoCommand<CMD_REMOVE_FROM_ROAD_WAYPOINT>(state, flags, tile, tile);
		} else {
			if (!IsAnyRoadStopTile(tile)) continue;
			RoadStopType stop_type = stop.type == StationType::Truck ? RoadStopType::Truck : RoadStopType::Bus;
			/* The road under the stop is left alone; whatever of it the paste laid
			 * was recorded separately and comes out with the roads below. */
			BlueprintDoCommand<CMD_REMOVE_ROAD_STOP>(state, flags, tile, 1, 1, stop_type, false);
		}
	}

	/* Station tiles were recorded one by one; group them back into the rectangles
	 * the removal commands work on, exactly as the paste grouped them to build. */
	for (const BlueprintStationRect &rect : GroupStationTiles(bp)) {
		if (!tile_valid(rect.offset)) continue;
		TileIndexDiffC last = {static_cast<int16_t>(rect.offset.x + rect.w - 1), static_cast<int16_t>(rect.offset.y + rect.h - 1)};
		if (!tile_valid(last)) continue;

		TileIndex start = target_tile(rect.offset);
		TileIndex end = target_tile(last);
		if (!IsTileOwner(start, _current_company)) continue;

		/* Keep the rail: these commands would otherwise clear the tile outright,
		 * taking with it any track that was already there when the paste built
		 * over it. What track the paste laid itself was recorded separately and
		 * comes out with the rail below, which leaves bare ground either way. */
		if (rect.is_waypoint) {
			if (!IsRailWaypointTile(start)) continue;
			BlueprintDoCommand<CMD_REMOVE_FROM_RAIL_WAYPOINT>(state, flags, start, end, true);
		} else {
			if (!IsRailStationTile(start)) continue;
			BlueprintDoCommand<CMD_REMOVE_FROM_RAIL_STATION>(state, flags, start, end, true);
		}
	}

	for (const BlueprintRoadDepot &depot : bp.road_depots) {
		if (!tile_valid(depot.offset)) continue;
		TileIndex tile = target_tile(depot.offset);
		if (!IsRoadDepotTile(tile) || !IsTileOwner(tile, _current_company)) continue;
		BlueprintDoCommand<CMD_LANDSCAPE_CLEAR>(state, flags, tile);
	}

	for (const BlueprintRoad &road : bp.roads) {
		if (!tile_valid(road.offset)) continue;
		UndoRemoveRoadBits(state, flags, target_tile(road.offset), road.bits, road.roadtype);
	}

	for (const BlueprintTunnelBridge &tb : bp.tunnel_bridges) {
		if (!tile_valid(tb.offset)) continue;
		TileIndex tile = target_tile(tb.offset);
		if (!IsTileType(tile, TileType::TunnelBridge) || !IsTileOwner(tile, _current_company)) continue;
		if (IsBridge(tile) != tb.is_bridge) continue;
		/* Clearing either end takes the whole span with it. */
		BlueprintDoCommand<CMD_LANDSCAPE_CLEAR>(state, flags, tile);
	}

	for (const BlueprintRailDepot &depot : bp.rail_depots) {
		if (!tile_valid(depot.offset)) continue;
		TileIndex tile = target_tile(depot.offset);
		if (!IsRailDepotTile(tile) || !IsTileOwner(tile, _current_company)) continue;
		BlueprintDoCommand<CMD_LANDSCAPE_CLEAR>(state, flags, tile);
	}

	/* Signals before the track they sit on. */
	for (const BlueprintSignal &sig : bp.signals) {
		if (!tile_valid(sig.offset)) continue;
		TileIndex tile = target_tile(sig.offset);
		if (!IsTileType(tile, TileType::Railway) || !IsTileOwner(tile, _current_company)) continue;
		if (!HasSignalOnTrack(tile, sig.track)) continue;
		BlueprintDoCommand<CMD_REMOVE_SINGLE_SIGNAL>(state, flags, tile, sig.track, RemoveSignalFlags{});
	}

	for (const BlueprintRailTrack &rt : bp.rail_tracks) {
		if (!tile_valid(rt.offset)) continue;
		TileIndex tile = target_tile(rt.offset);
		if (!IsPlainRailTile(tile) || !IsTileOwner(tile, _current_company)) continue;
		if (!HasTrack(tile, rt.track)) continue;
		BlueprintDoCommand<CMD_REMOVE_SINGLE_RAIL>(state, flags, tile, rt.track);
	}

	for (const BlueprintShipDepot &depot : bp.ship_depots) {
		if (!tile_valid(depot.offset)) continue;
		TileIndex tile = target_tile(depot.offset);
		if (!IsShipDepotTile(tile) || !IsTileOwner(tile, _current_company)) continue;
		BlueprintDoCommand<CMD_LANDSCAPE_CLEAR>(state, flags, tile);
	}

	for (const BlueprintLock &lock : bp.locks) {
		if (!tile_valid(lock.offset)) continue;
		TileIndex tile = target_tile(lock.offset);
		if (!IsTileType(tile, TileType::Water) || !IsLock(tile) || !IsTileOwner(tile, _current_company)) continue;
		BlueprintDoCommand<CMD_LANDSCAPE_CLEAR>(state, flags, tile);
	}

	for (const BlueprintCanal &canal : bp.canals) {
		if (!tile_valid(canal.offset)) continue;
		TileIndex tile = target_tile(canal.offset);
		if (!IsTileType(tile, TileType::Water) || !IsCanal(tile) || !IsTileOwner(tile, _current_company)) continue;
		BlueprintDoCommand<CMD_LANDSCAPE_CLEAR>(state, flags, tile);
	}

	/* The landscape goes back last, once everything standing on it is gone —
	 * terraforming under a station or a bridge head would simply fail.
	 *
	 * The recorded heights are absolute, so each corner is simply driven back to
	 * the height it had before the paste. Note what that means for a corner
	 * someone has terraformed since: it is put back too, because a height carries
	 * no record of who last changed it. Restoring the footprint to the shape it
	 * had before the paste is at least a predictable rule, and the toggle in the
	 * toolbar is there for players who would rather keep the land as it is.
	 *
	 * This can also destroy things in its own right — raising or lowering ground
	 * clears what stands on it — which is the other reason it is a toggle. */
	if (data.restore_terrain && !bp.corner_heights.empty()) {
		/* One terraform command moves a corner by at most one height level, so
		 * sweep until nothing changes any more, exactly as the paste did. */
		for (uint pass = 0; pass <= MAX_TILE_HEIGHT; pass++) {
			bool changed = false;
			for (uint cy = 0; cy <= bp.height; cy++) {
				for (uint cx = 0; cx <= bp.width; cx++) {
					if (TileX(origin) + cx > Map::MaxX() || TileY(origin) + cy > Map::MaxY()) continue;

					TileIndex corner_tile = TileAddXY(origin, cx, cy);
					int wanted = bp.corner_heights[cy * (bp.width + 1) + cx];
					uint current = TileHeight(corner_tile);
					if (static_cast<int>(current) == wanted) continue;

					BlueprintDoCommand<CMD_TERRAFORM_LAND>(state, flags, corner_tile, SLOPE_N, static_cast<int>(current) < wanted);
					if (TileHeight(corner_tile) != current) changed = true;
				}
			}
			if (!changed) break;
		}
	}

	if (!state.any_success) return CMD_ERROR;
	return state.total;
}
