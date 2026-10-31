/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file elrail_func.h Header file for electrified rail specific functions. */

#ifndef ELRAIL_FUNC_H
#define ELRAIL_FUNC_H

#include "rail.h"
#include "settings_type.h"
#include "transparency.h"

struct TileInfo;

/**
 * Test if a rail type has catenary.
 * @param rt Rail type to test.
 * @return \c true iff the rail type has catenary.
 */
inline bool HasRailCatenary(RailType rt)
{
	return GetRailTypeInfo(rt)->flags.Test(RailTypeFlag::Catenary);
}

/**
 * Test if we should draw rail catenary.
 * @param rt Rail type to test.
 * @return \c true iff catenary should be drawn for this rail type.
 */
inline bool HasRailCatenaryDrawn(RailType rt, RailType secondary = INVALID_RAILTYPE)
{
	return !IsInvisibilitySet(TransparencyOption::Catenary) && !_settings_game.vehicle.disable_elrails && (HasRailCatenary(rt) || (secondary != INVALID_RAILTYPE && HasRailCatenary(secondary)));
}

void DrawRailCatenary(const TileInfo *ti);
void DrawRailCatenaryOnTunnel(const TileInfo *ti);
void DrawRailCatenaryOnBridge(const TileInfo *ti);

/** A catenary pylon or wire on one tile, in world pixels, where the classic view draws it. */
struct RailCatenaryPiece {
	bool pylon; ///< A pylon at \c a whose cantilever reaches the pylon control point \c b; otherwise a wire from \c a to \c b.
	uint8_t pylons; ///< Wire only: bit 0 is set when a pylon holds the wire at \c a, bit 1 at \c b.
	int ax, ay, az, bx, by, bz;
	bool operator==(const RailCatenaryPiece &) const = default;
};
void CollectRailCatenary(TileIndex tile, std::vector<RailCatenaryPiece> &pieces);

void SettingsDisableElrail(int32_t new_value);
void UpdateDisableElrailSettingState(bool disable, bool update_vehicles);

#endif /* ELRAIL_FUNC_H */
