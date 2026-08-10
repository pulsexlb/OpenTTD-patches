/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file map_type.h Types related to maps. */

#ifndef MAP_TYPE_H
#define MAP_TYPE_H

/**
 * The per-tile data structures (Tile::TileBase and Tile::TileExtended)
 * are defined in map_func.h as part of the Tile wrapper class.
 * Look at docs/landscape.html for the exact meaning of the members.
 */

/**
 * A pair-construct of a TileIndexDiff.
 *
 * This can be used to save the difference between to
 * tiles as a pair of x and y value.
 */
struct TileIndexDiffC {
	int16_t x;      ///< The x value of the coordinate
	int16_t y;      ///< The y value of the coordinate

	bool operator==(const TileIndexDiffC &) const = default;
};

/**
 * An unsigned pair-construct of a TileIndexDiff.
 *
 * This can be used to save the difference between to
 * tiles as a pair of x and y value.
 */
struct TileIndexDiffCUnsigned {
	uint32_t x;      ///< The x value of the coordinate
	uint32_t y;      ///< The y value of the coordinate

	bool operator==(const TileIndexDiffCUnsigned &) const = default;
};

/** Minimal and maximal map width and height */
static const uint MIN_MAP_SIZE_BITS  = 6;                        ///< Minimal size of map is equal to 2 ^ MIN_MAP_SIZE_BITS
static const uint MAX_MAP_SIZE_BITS  = 20;                       ///< Maximal size of map is equal to 2 ^ MAX_MAP_SIZE_BITS
static const uint MAX_MAP_TILES_BITS = 28;                       ///< Maximal number of tiles in a map is equal to 2 ^ MAX_MAP_TILES_BITS.
static const uint MIN_MAP_SIZE       = 1U << MIN_MAP_SIZE_BITS;  ///< Minimal map size = 64
static const uint MAX_MAP_SIZE       = 1U << MAX_MAP_SIZE_BITS;  ///< Maximal map size = 1M
static const uint MAX_MAP_TILES      = 1U << MAX_MAP_TILES_BITS; ///< Maximal number of tiles in a map = 256M (16k x 16k)

/** Argument for CmdLevelLand describing what to do. */
enum LevelMode : uint8_t {
	LM_LEVEL, ///< Level the land.
	LM_LOWER, ///< Lower the land.
	LM_RAISE, ///< Raise the land.
};

#endif /* MAP_TYPE_H */
