/* This file is part of OpenTTD and is licensed under the GNU General Public License version 2. */
/** @file ground3d.h Ground texture layers for the 3D terrain: one layer per authoritative ground state. */
#ifndef VIDEO_GROUND3D_H
#define VIDEO_GROUND3D_H

#include <algorithm>
#include <cstdint>

namespace Ground3D {
/** Texture array layers, in the order of the generated texture table. */
enum Layer : uint8_t {
	GRASS_0, GRASS_1, GRASS_2, GRASS_3, ///< Grass density 0 (bare) to 3 (full).
	ROUGH,
	ROCKS,
	FIELD_0, ///< Farm field type 0 to 8 (\c GetFieldType), FIELD_0 + type.
	FIELD_8 = FIELD_0 + 8,
	SNOW_0, SNOW_3 = SNOW_0 + 3, ///< Snow density 0 to 3.
	DESERT_0, DESERT_3 = DESERT_0 + 3, ///< Desert density 0 to 3.
	BEACH,
	WATER,
	CONCRETE,
	LAYER_COUNT,
};

/** Map value for tiles that never blend into neighbours, such as water. */
static constexpr uint8_t NO_BLEND = 0x80;

/** Ground under a clear tile, matching the sprite choice in \c DrawTile_Clear. */
enum class Kind : uint8_t { Grass, Rough, Rocks, Fields, Snow, Desert, Beach, Water, Concrete };

constexpr Layer LayerOf(Kind kind, unsigned density = 3, unsigned field_type = 0)
{
	density = std::min(density, 3U);
	switch (kind) {
		case Kind::Grass: return static_cast<Layer>(GRASS_0 + density);
		case Kind::Rough: return ROUGH;
		case Kind::Rocks: return ROCKS;
		/* Field types above 8 have no sprite; classic draws them as bare farmland. */
		case Kind::Fields: return static_cast<Layer>(FIELD_0 + (field_type <= 8 ? field_type : 0));
		case Kind::Snow: return static_cast<Layer>(SNOW_0 + density);
		case Kind::Desert: return static_cast<Layer>(DESERT_0 + density);
		case Kind::Beach: return BEACH;
		case Kind::Water: return WATER;
		case Kind::Concrete: return CONCRETE;
	}
	return GRASS_3;
}
}

#endif
