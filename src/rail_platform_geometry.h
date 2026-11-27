/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file rail_platform_geometry.h Distances along a connected station platform. */

#ifndef RAIL_PLATFORM_GEOMETRY_H
#define RAIL_PLATFORM_GEOMETRY_H

#include "station_map.h"

/** Advance from a station platform to the connected piece, including across bends. */
bool AdvanceRailPlatform(RailPlatformPosition &p);

/** Distance to the platform exit, including the entire origin piece, in train movement steps. */
int GetRailPlatformLengthAhead(RailPlatformPosition origin);

#endif /* RAIL_PLATFORM_GEOMETRY_H */
