/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file air.h Air specific functions. */

#ifndef AIR_H
#define AIR_H

#include "track_type.h"
#include "gfx_type.h"
#include "core/bitmath_func.hpp"
#include "economy_func.h"
#include "slope_type.h"
#include "strings_type.h"
#include "air_type.h"

/**
 * This struct contains all the info that is needed to draw and construct tracks.
 */
class AirTypeInfo {
public:
	/**
	 * Struct containing the main sprites. @note not all sprites are listed, but only
	 * the ones used directly in the code
	 */
	struct {
		SpriteID ground[20];           ///< ground sprite
		SpriteID infra_catch[2][5][4]; ///< non-snowed/snowed + building number + rotation
		SpriteID wind[4][4];
		SpriteID radar[12];
		SpriteID infra_no_catch[4][4]; // transmitter, snowed transmitter, tower, snowed tower
		SpriteID runways[24]; // 2 normal + 1 cross + 4 ends
		SpriteID aprons[10];
		SpriteID hangars[12];
	} base_sprites;

	/**
	 * struct containing the sprites for the airport GUI. @note only sprites referred to
	 * directly in the code are listed
	 */
	struct {
		SpriteID add_airport_tiles;
		SpriteID build_track_tile;
		SpriteID change_airtype;
		SpriteID build_catchment_infra;
		SpriteID build_noncatchment_infra;
		SpriteID define_landing_runway;
		SpriteID define_nonlanding_runway;
		SpriteID build_apron;
		SpriteID build_helipad;
		SpriteID build_heliport;
		SpriteID build_hangar;
	} gui_sprites;

	/**
	 * struct containing the sprites for the airport GUI. @note only sprites referred to
	 * directly in the code are listed
	 */
	struct {
		SpriteID add_airport_tiles;
		SpriteID build_track_tile;
		SpriteID change_airtype;
		SpriteID build_catchment_infra;
		SpriteID build_noncatchment_infra;
		SpriteID define_landing_runway;
		SpriteID define_nonlanding_runway;
		SpriteID build_apron;
		SpriteID build_helipad;
		SpriteID build_heliport;
		SpriteID build_hangar;
	} cursor;

	struct {
		StringID name;
		StringID toolbar_caption;
		StringID menu_text;
		StringID replace_text;
	} strings;

	/** sprite number difference between a piece of track on a snowy ground and the corresponding one on normal ground */
	SpriteID snow_offset;

	/**
	 * Original airtype number to use when drawing non-newgrf airtypes, or when drawing stations.
	 */
	uint8_t fallback_airtype;

	/**
	 * Cost multiplier for building this air type
	 */
	uint16_t cost_multiplier;

	/**
	 * Cost multiplier for maintenance of this air type
	 */
	uint16_t maintenance_multiplier;

	/**
	 * Maximum speed for vehicles travelling on this air type
	 */
	uint16_t max_speed;

	/**
	 * Unique 32 bit air type identifier
	 */
	AirTypeLabel label;

	/**
	 * Colour on mini-map
	 */
	uint8_t map_colour;

	/**
	 * The sorting order of this airtype for the toolbar dropdown.
	 */
	uint8_t sorting_order;

	/**
	 * Catchment area radius.
	 */
	uint8_t catchment_radius;

	/**
	 * Max number of runways.
	 */
	uint8_t max_num_runways;

	/**
	 * Minimum runway length in tiles.
	 */
	uint8_t min_runway_length;

	/**
	 * Base noise level. Each station has this noise level plus the noise created by each runway.
	 * Example: if base noise is 5 and there are 4 runways and runway level is 6,
	 *                  total noise level of the airport is 5 + 4 * 6 = 29
	 */
	uint8_t base_noise_level;

	/**
	 * Runway noise level.
	 */
	uint8_t runway_noise_level;

	/**
	 * Heliport availability.
	 */
	bool heliport_availability;

	/**
	 * Build airports on water.
	 */
	bool build_on_water;
};


/**
 * Returns a pointer to the AirType information for a given airtype
 * @param airtype the air type which the information is requested for
 * @return The pointer to the AirTypeInfo
 */
static inline const AirTypeInfo *GetAirTypeInfo(const AirType airtype)
{
	extern AirTypeInfo _airtypes[AIRTYPE_END];
	assert(airtype < AIRTYPE_END);
	return &_airtypes[airtype];
}

/**
 * Returns the cost of building the specified airtype.
 * @param airtype The airtype being built.
 * @return The cost multiplier.
 */
static inline Money AirBuildCost(AirType airtype)
{
	assert(airtype < AIRTYPE_END);
	return (_price[Price::BuildStationAirport] * GetAirTypeInfo(airtype)->cost_multiplier) >> 3;
}

/**
 * Returns the 'cost' of clearing the specified airtype.
 * @param airtype The airtype being removed.
 * @return The cost.
 */
static inline Money AirClearCost(AirType airtype)
{
	/* Clearing airport tiles in fact earns money, but if the build cost is set
	 * very low then a loophole exists where money can be made.
	 * In this case we limit the removal earnings to 3/4s of the build
	 * cost.
	 */
	assert(airtype < AIRTYPE_END);
	return std::max(_price[Price::ClearStationAirport], -AirBuildCost(airtype) * 3 / 4);
}

/**
 * Calculates the cost of air conversion
 * @param from The airtype we are converting from
 * @param to   The airtype we are converting to
 * @return Cost per TrackBit
 */
static inline Money AirConvertCost(AirType from, AirType to)
{
	return AirBuildCost(to) + AirClearCost(from);
}

/**
 * Calculates the maintenance cost of a number of track bits.
 * @param airtype The airtype to get the cost of.
 * @param num Number of track bits of this airtype.
 * @param total_num Total number of track bits of all airtypes.
 * @return Total cost.
 */
static inline Money AirMaintenanceCost(AirType airtype, uint32_t num, uint32_t total_num)
{
	assert(airtype < AIRTYPE_END);
	return (_price[Price::InfrastructureAirport] * GetAirTypeInfo(airtype)->maintenance_multiplier * num * (1 + IntSqrt(total_num))) >> 11; // 4 bits fraction for the multiplier and 7 bits scaling.
}

static inline bool AreHeliportsAvailable(AirType airtype)
{
	return GetAirTypeInfo(airtype)->heliport_availability;
}

Foundation GetAirFoundation(Slope tileh, TrackBits bits);


bool ValParamAirType(const AirType Air);

void ResetAirTypes();
void InitAirTypes();

extern std::vector<AirType> _sorted_airtypes;

void AfterLoadSetAirportTileTypes();

#endif /* AIR_H */
