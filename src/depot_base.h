/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file depot_base.h Base for all depots (except hangars). */

#ifndef DEPOT_BASE_H
#define DEPOT_BASE_H

#include "depot_map.h"
#include "external_city_type.h"
#include "core/pool_type.hpp"
#include "core/tinystring_type.hpp"

typedef Pool<Depot, DepotID, 64> DepotPool;
extern DepotPool _depot_pool;

struct Depot : DepotPool::PoolItem<&_depot_pool> {
	/* DepotID index member of DepotPool is 2 bytes. */
	uint16_t town_cn = 0; ///< The N-1th depot for this town (consecutive number)
	TileIndex xy = INVALID_TILE;
	Town *town = nullptr;
	TinyString name{};
	CalTime::Date build_date{}; ///< Date of construction
	bool is_external_connection = false; ///< External portal: draw its rail, without a depot building.
	ExternalCityID external_city = ExternalCityID::Invalid(); ///< Legacy v3 load input; runtime destinations are resolved from boundary regions.

	Depot(DepotID index, TileIndex xy = INVALID_TILE) : PoolItemBase(index), xy(xy), build_date(CalTime::CurDate()) {}
	~Depot();

	static inline Depot *GetByTile(TileIndex tile)
	{
		return Depot::Get(GetDepotIndex(tile));
	}

	static bool IsExternalConnection(TileIndex tile)
	{
		if (!IsValidTile(tile) || (!IsRailDepotTile(tile) && !IsRoadDepotTile(tile) && !IsShipDepotTile(tile))) return false;
		const Depot *depot = Depot::GetIfValid(GetDepotIndex(tile));
		return depot != nullptr && depot->is_external_connection;
	}
	/** Inward-facing opening shared by railway and road external entrances. */
	static DiagDirection GetExternalDirection(TileIndex tile)
	{
		if (!IsExternalConnection(tile)) return DiagDirection::Invalid;
		if (IsShipDepotTile(tile)) {
			const TileIndex north = GetShipDepotNorthTile(tile);
			const TileIndex south = GetOtherShipDepotTile(north);
			const Axis axis = GetShipDepotAxis(north);
			/* An external water entrance occupies the edge tile and its inner neighbour. */
			if (axis == Axis::X) return TileX(south) == Map::SizeX() - 2 ? DiagDirection::NE : DiagDirection::SW;
			return TileY(south) == Map::SizeY() - 2 ? DiagDirection::NW : DiagDirection::SE;
		}
		return IsRailDepotTile(tile) ? GetRailDepotDirection(tile) : GetRoadDepotDirection(tile);
	}

	/**
	 * Is the "type" of depot the same as the given depot,
	 * i.e. are both a rail, road or ship depots?
	 * @param d The depot to compare to.
	 * @return true iff their types are equal.
	 */
	inline bool IsOfType(const Depot *d) const
	{
		return GetTileType(d->xy) == GetTileType(this->xy);
	}
};

#endif /* DEPOT_BASE_H */
