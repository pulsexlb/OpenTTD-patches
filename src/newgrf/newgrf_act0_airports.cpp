/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file newgrf_act0_airports.cpp NewGRF Action 0x00 handler for airports. */

#include "../stdafx.h"
#include "../debug.h"
#include "../newgrf_airporttiles.h"
#include "../newgrf_airport.h"
#include "newgrf_bytereader.h"
#include "newgrf_internal.h"
#include "newgrf_stringmapping.h"

#include <bitset>

#include "../safeguards.h"

/**
 * Define properties for airports
 * @param first Local ID of the first airport.
 * @param last Local ID of the last airport.
 * @param prop The property to change.
 * @param mapping_entry Variable mapping entry.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */

/**
 * Get the airtype a NewGRF airport belongs to.
 * @param airport Local airport id.
 * @return the airtype.
 */
static AirType GetConversionAirtype(uint airport)
{
	struct AirportTypesConversion {
		AirportTypes airport_type;
		AirType air_type;
	};

	switch (_cur_gps.grffile->grfid) {
		default:
			Debug(misc, 0, "Trying to load airports of unknown airtype from grffile with id {}", _cur_gps.grffile->grfid);
			return AIRTYPE_GRAVEL;
		case 16860225:
			return AIRTYPE_WATER;
		case 19680837: // North Korean Aviation Set: Small asphalt airports
			return AIRTYPE_ASPHALT;
		case 5259587: { // OpenGFX+ Airports
			/* This table indicates how to convert the airports provided in OpenGFX+Airports,
			* as long as it is the first NewGRF to be applied that modifies airports. */
			/* The "S" shows which airports have a (close) equivalent in original airports. */
			AirportTypesConversion opengfx_plus_airports[] = {
				{ AT_SMALL,          AIRTYPE_GRAVEL  }, // S NEW_AIRPORT_OFFSET +  0 Small gravel
				{ AT_SMALL,          AIRTYPE_WATER   }, //   NEW_AIRPORT_OFFSET +  1 Small water
				{ AT_SMALL,          AIRTYPE_ASPHALT }, //   NEW_AIRPORT_OFFSET +  2 Small asphalt
				{ AT_COMMUTER,       AIRTYPE_GRAVEL  }, //   NEW_AIRPORT_OFFSET +  3 Commuter gravel
				{ AT_COMMUTER,       AIRTYPE_ASPHALT }, // S NEW_AIRPORT_OFFSET +  4 Commuter asphalt
				{ AT_LARGE,          AIRTYPE_ASPHALT }, // S NEW_AIRPORT_OFFSET +  5 Large asphalt
				{ AT_METROPOLITAN,   AIRTYPE_ASPHALT }, // S NEW_AIRPORT_OFFSET +  6 City asphalt
				{ AT_INTERNATIONAL,  AIRTYPE_ASPHALT }, // S NEW_AIRPORT_OFFSET +  7 International asphalt
				{ AT_INTERCON,       AIRTYPE_ASPHALT }, // S NEW_AIRPORT_OFFSET +  8 Intercontinental asphalt -- Uses a different and non-rectangular layout.
				{ AT_HELIPORT,       AIRTYPE_ASPHALT }, // S NEW_AIRPORT_OFFSET +  9 Heliport
				{ AT_HELIDEPOT,      AIRTYPE_GRAVEL  }, //   NEW_AIRPORT_OFFSET + 10 Helidepot gravel
				{ AT_HELIDEPOT,      AIRTYPE_ASPHALT }, // S NEW_AIRPORT_OFFSET + 11 Helidepot asphalt
				{ AT_HELISTATION,    AIRTYPE_GRAVEL  }, //   NEW_AIRPORT_OFFSET + 12 Helistation gravel
				{ AT_HELISTATION,    AIRTYPE_ASPHALT }, // S NEW_AIRPORT_OFFSET + 13 Helistation asphalt
			};
			const uint num_opengfx_plus_airports = sizeof(opengfx_plus_airports)/sizeof(opengfx_plus_airports[0]);

			if (airport >= num_opengfx_plus_airports) NOT_REACHED();
			return opengfx_plus_airports[airport].air_type;
		}
	}
}

/**
 * Define properties for airports
 * @param first Local ID of the first airport.
 * @param last Local ID of the last airport.
 * @param prop The property to change.
 * @param mapping_entry Variable mapping entry.
 * @param buf The property value.
 * @return ChangeInfoResult.
 */
static ChangeInfoResult AirportChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = ChangeInfoResult::Success;

	if (last > NUM_AIRPORTS_PER_GRF) {
		GrfMsg(1, "AirportChangeInfo: Too many airports, trying id ({}), max ({}). Ignoring.", last, NUM_AIRPORTS_PER_GRF);
		return ChangeInfoResult::InvalidId;
	}

	/* Allocate airport specs if they haven't been allocated already. */
	if (_cur_gps.grffile->airportspec.size() < last) _cur_gps.grffile->airportspec.resize(last);

	AirType conversion_airtype = GetConversionAirtype(first);

	for (uint id = first; id < last; ++id) {
		AirportSpec *as = _cur_gps.grffile->airportspec[id].get();

		if (as == nullptr && prop != 0x08 && prop != 0x09) {
			GrfMsg(2, "AirportChangeInfo: Attempt to modify undefined airport {}, ignoring", id);
			return ChangeInfoResult::InvalidId;
		}

		switch (prop) {
			case 0x08: { // Modify original airport
				uint8_t subs_id = buf.ReadByte();
				if (subs_id == 0xFF) {
					/* Instead of defining a new airport, an airport id
					 * of 0xFF disables the old airport with the current id. */
					AirportSpec::GetWithoutOverride(id)->enabled = false;
					continue;
				} else if (subs_id >= NEW_AIRPORT_OFFSET) {
					/* The substitute id must be one of the original airports. */
					GrfMsg(2, "AirportChangeInfo: Attempt to use new airport {} as substitute airport for {}. Ignoring.", subs_id, id);
					continue;
				}

				/* Allocate space for this airport.
				 * Only need to do it once. If ever it is called again, it should not
				 * do anything */
				if (as == nullptr) {
					_cur_gps.grffile->airportspec[id] = std::make_unique<AirportSpec>(*AirportSpec::GetWithoutOverride(subs_id));
					as = _cur_gps.grffile->airportspec[id].get();

					as->enabled = true;
					as->grf_prop.local_id = id;
					as->grf_prop.subst_id = subs_id;
					as->grf_prop.SetGRFFile(_cur_gps.grffile);
					as->airtype = conversion_airtype;
					/* override the default airport */
					_airport_mngr.Add(id, _cur_gps.grffile->grfid, subs_id);
				}
				break;
			}

			case 0x0A: { // Set airport layout
				if (_cur_gps.grffile->grf_version <= 8) {
					/* Deal with the only NewGRF that modified airport layouts. */
					const uint max_airport_tiles = 4096; // 64 * 64, max station spread.
					[[maybe_unused]] uint num_tiles = as->layouts[0].size_x * as->layouts[0].size_y;
					assert(num_tiles <= max_airport_tiles);
					uint8_t num_layouts = buf.ReadByte();
					std::bitset<max_airport_tiles> defined_tiles;
					buf.ReadDWord();  // Total size of the definition, unneeded.

					as->layouts.resize(1);
					auto &layout = as->layouts[0];
					assert(layout.tiles.size() == num_tiles);

					for (uint8_t j = 0; j != num_layouts; ++j) {
						DiagDirection rotation = (DiagDirection)(buf.ReadByte() / 2); // rotation

						for (;;) {
							uint8_t x = buf.ReadByte(); // Offsets from northermost tile
							uint8_t y = buf.ReadByte();

							if (x == 0 && y == 0x80) break;

							// Get the corresponding offset for the non-rotated version.
							switch (static_cast<uint>(rotation)) {
								case 0:
									break;
								case 1:
									std::swap(x, y);
									x = as->layouts[0].size_x - 1 - x;
									break;
								case 2:
									x = as->layouts[0].size_x - 1 - x;
									y = as->layouts[0].size_y - 1 - y;
									break;
								case 3:
									std::swap(x, y);
									y = as->layouts[0].size_y - 1 - y;
									break;
								default:
									NOT_REACHED();
							}

							uint16_t table_index = as->layouts[0].size_x * y + x;
							assert(table_index < as->layouts[0].size_x * as->layouts[0].size_y);

							// Only keep track of first layout.
							if (j == 0) defined_tiles[table_index] = true;
							auto &tile = layout.tiles[table_index];

							tile.gfx[static_cast<size_t>(rotation)] = (AirportTiles)buf.ReadByte();

							if (tile.gfx[static_cast<size_t>(rotation)] == 0xFE) { // gfx
								int local_tile_id = buf.ReadWord(); // use a new tile for this GRFC
								/* Read the ID from the _airporttile_mngr. */
								uint16_t tempid = _airporttile_mngr.GetID(local_tile_id, _cur_gps.grffile->grfid);

								if (tempid == INVALID_AIRPORTTILE) {
									GrfMsg(2, "AirportChangeInfo: Attempt to use airport tile {} with airport id {}, not yet defined. Ignoring.", local_tile_id, id);
								} else {
									/* Declared as been valid, can be used */
									tile.gfx[static_cast<size_t>(rotation)] = (AirportTiles)tempid;
								}
							}
						}
					}

					/* Set the empty tiles if any. */
					for (int i = 0; i < as->layouts[0].size_x * as->layouts[0].size_y; i++) {
						if (defined_tiles[i]) continue;
						layout.tiles[i].type = ATT_INVALID;
					}

				} else {
					ret = ChangeInfoResult::Unknown;
				}
				break;
			}

			case 0x0C:
				as->min_year = CalTime::Year{buf.ReadWord()};
				as->max_year = CalTime::Year{buf.ReadWord()};
				if (as->max_year == 0xFFFF) as->max_year = CalTime::MAX_YEAR;
				break;

			case 0x0D:
				as->ttd_airport_type = (TTDPAirportType)buf.ReadByte();
				break;

			case 0x0E:
				buf.ReadByte(); // Old airport catchment
				break;

			case 0x0F:
				buf.ReadByte(); // Old airport noise
				break;

			case 0x10:
				AddStringForMapping(GRFStringID{buf.ReadWord()}, &as->name);
				break;

			case 0x11: // Maintenance cost factor
				buf.ReadWord();
				break;

			case 0x12: // Badge list
				as->badges = ReadBadgeList(buf, GrfSpecFeature::Airports);
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}
static ChangeInfoResult AirportTilesChangeInfo(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf)
{
	ChangeInfoResult ret = ChangeInfoResult::Success;

	if (last > NUM_AIRPORTTILES_PER_GRF) {
		GrfMsg(1, "AirportTileChangeInfo: Too many airport tiles loaded ({}), max ({}). Ignoring.", last, NUM_AIRPORTTILES_PER_GRF);
		return ChangeInfoResult::InvalidId;
	}

	/* Allocate airport tile specs if they haven't been allocated already. */
	if (_cur_gps.grffile->airtspec.size() < last) _cur_gps.grffile->airtspec.resize(last);

	for (uint id = first; id < last; ++id) {
		AirportTileSpec *tsp = _cur_gps.grffile->airtspec[id].get();

		if (prop != 0x08 && tsp == nullptr) {
			GrfMsg(2, "AirportTileChangeInfo: Attempt to modify undefined airport tile {}. Ignoring.", id);
			return ChangeInfoResult::InvalidId;
		}

		switch (prop) {
			case 0x08: { // Substitute airport tile type
				uint8_t subs_id = buf.ReadByte();
				if (subs_id >= NEW_AIRPORTTILE_OFFSET) {
					/* The substitute id must be one of the original airport tiles. */
					GrfMsg(2, "AirportTileChangeInfo: Attempt to use new airport tile {} as substitute airport tile for {}. Ignoring.", subs_id, id);
					continue;
				}

				/* Allocate space for this airport tile. */
				if (tsp == nullptr) {
					_cur_gps.grffile->airtspec[id] = std::make_unique<AirportTileSpec>(*AirportTileSpec::Get(subs_id));
					tsp = _cur_gps.grffile->airtspec[id].get();

					tsp->enabled = true;

					tsp->animation = {};

					tsp->grf_prop.local_id = id;
					tsp->grf_prop.subst_id = subs_id;
					tsp->grf_prop.SetGRFFile(_cur_gps.grffile);
					_airporttile_mngr.AddEntityID(id, _cur_gps.grffile->grfid, subs_id); // pre-reserve the tile slot
				}
				break;
			}

			case 0x09: { // Airport tile override
				uint8_t override_id = buf.ReadByte();

				/* The airport tile being overridden must be an original airport tile. */
				if (override_id >= NEW_AIRPORTTILE_OFFSET) {
					GrfMsg(2, "AirportTileChangeInfo: Attempt to override new airport tile {} with airport tile id {}. Ignoring.", override_id, id);
					continue;
				}

				_airporttile_mngr.Add(id, _cur_gps.grffile->grfid, override_id);
				break;
			}

			case 0x0E: // Callback mask
				tsp->callback_mask = static_cast<AirportTileCallbackMasks>(buf.ReadByte());
				break;

			case 0x0F: // Animation information
				tsp->animation.frames = buf.ReadByte();
				tsp->animation.status = static_cast<AnimationStatus>(buf.ReadByte());
				break;

			case 0x10: // Animation speed
				tsp->animation.speed = buf.ReadByte();
				break;

			case 0x11: // Animation triggers
				tsp->animation.triggers = static_cast<AirportAnimationTriggers>(buf.ReadByte());
				break;

			case 0x12: // Badge list
				tsp->badges = ReadBadgeList(buf, GrfSpecFeature::TramTypes);
				break;

			default:
				ret = HandleAction0PropertyDefault(buf, prop);
				break;
		}
	}

	return ret;
}

template <> ChangeInfoResult GrfChangeInfoHandler<GrfSpecFeature::Airports>::Reserve(uint, uint, int, const GRFFilePropertyRemapEntry *, ByteReader &) { return ChangeInfoResult::Unhandled; }
template <> ChangeInfoResult GrfChangeInfoHandler<GrfSpecFeature::Airports>::Activation(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf) { return AirportChangeInfo(first, last, prop, mapping_entry, buf); }

template <> ChangeInfoResult GrfChangeInfoHandler<GrfSpecFeature::AirportTiles>::Reserve(uint, uint, int, const GRFFilePropertyRemapEntry *, ByteReader &) { return ChangeInfoResult::Unhandled; }
template <> ChangeInfoResult GrfChangeInfoHandler<GrfSpecFeature::AirportTiles>::Activation(uint first, uint last, int prop, const GRFFilePropertyRemapEntry *mapping_entry, ByteReader &buf) { return AirportTilesChangeInfo(first, last, prop, mapping_entry, buf); }
