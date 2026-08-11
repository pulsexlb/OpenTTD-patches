/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file script_depotlist.cpp Implementation of ScriptDepotList and friends. */

#include "../../stdafx.h"
#include "script_depotlist.hpp"
#include "../../depot_base.h"
#include "../../station_base.h"

#include "../../safeguards.h"

ScriptDepotList::ScriptDepotList(ScriptTile::TransportType transport_type)
{
	EnforceDeityOrCompanyModeValid_Void();
	::TileType tile_type;
	switch (transport_type) {
		default: return;

		case ScriptTile::TRANSPORT_ROAD:  tile_type = ::TileType::Road; break;
		case ScriptTile::TRANSPORT_RAIL:  tile_type = ::TileType::Railway; break;
		case ScriptTile::TRANSPORT_WATER: tile_type = ::TileType::Water; break;

		case ScriptTile::TRANSPORT_AIR: {
			/* Hangars are depots in the depot pool for multitile airports. */
			bool is_deity = ScriptCompanyMode::IsDeity();
			::CompanyID owner = ScriptObject::GetCompany();
			for (const Station *st : Station::Iterate()) {
				if ((is_deity || st->owner == owner) && st->airport.hangar != nullptr && st->airport.hangar->xy != INVALID_TILE) {
					this->AddItem(st->airport.hangar->xy.base());
				}
			}
			return;
		}
	}

	/* Handle 'standard' depots. */
	bool is_deity = ScriptCompanyMode::IsDeity();
	::CompanyID owner = ScriptObject::GetCompany();
	for (const Depot *depot : Depot::Iterate()) {
		if ((is_deity || ::GetTileOwner(depot->xy) == owner) && ::IsTileType(depot->xy, tile_type)) this->AddItem(depot->xy.base());
	}
}
