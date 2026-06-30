/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file league_type.h Basic types related to league tables. */

#ifndef LEAGUE_TYPE_H
#define LEAGUE_TYPE_H

#include "core/pool_id_type.hpp"

/** Types of the possible link targets. */
enum class LinkType : uint8_t {
	None = 0, ///< No link
	Tile = 1, ///< Link a tile
	Industry = 2, ///< Link an industry
	Town = 3, ///< Link a town
	Company = 4, ///< Link a company
	StoryPage = 5, ///< Link a story page
};

typedef uint32_t LinkTargetID; ///< Contains either tile, industry ID, town ID, story page ID or company ID

struct Link {
	LinkType type = LinkType::None;
	LinkTargetID target = 0;

	Link() {}
	Link(LinkType type, LinkTargetID target) : type{type}, target{target} {}
};

struct LeagueTableIDTag : public PoolIDTraits<uint8_t, 255, 0xFF> {};
using LeagueTableID = PoolID<LeagueTableIDTag>; ///< ID of a league table
struct LeagueTable;

struct LeagueTableElementIDTag : public PoolIDTraits<uint16_t, 64000, 0xFFFF> {};
using LeagueTableElementID = PoolID<LeagueTableElementIDTag>; ///< ID of a league table
struct LeagueTableElement;

#endif /* LEAGUE_TYPE_H */
