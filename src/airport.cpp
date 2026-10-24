/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file airport.cpp Functions related to airports. */

#include "stdafx.h"
#include "station_base.h"
#include "depot_base.h"
#include "air_map.h"
#include "settings_type.h"
#include "window_func.h"

#include "debug.h"

#include "table/airtypes.h"
#include "table/strings.h"
#include "table/airporttile_ids.h"
#include "table/airport_defaults.h"

#include "safeguards.h"


/** Helper type for lists/vectors of trains */
typedef std::vector<Aircraft *> AircraftList;

AirTypeInfo _airtypes[AIRTYPE_END];
std::vector<AirType> _sorted_airtypes;
bool _show_airport_tracks = false;


const AirportSpec AirportSpec::dummy = {{APC_BEGIN, 0}, {}, AIRTYPE_GRAVEL, 0, 0, 0, 0, 0, CalTime::MIN_YEAR, CalTime::MIN_YEAR, STR_NULL, ATP_TTDP_LARGE, 0, false, false, false, SubstituteGRFFileProps(AT_INVALID)};
const AirportSpec AirportSpec::custom = {{APC_CUSTOM, 0}, {}, AIRTYPE_GRAVEL, 0, 0, 0, 0, 0, CalTime::MIN_YEAR, CalTime::MIN_YEAR, STR_AIRPORT_CUSTOM, ATP_TTDP_LARGE, 0, false, false, false, SubstituteGRFFileProps(AT_INVALID)};


/**
 * Reset all air type information to its default values.
 */
void ResetAirTypes()
{
	static_assert(lengthof(_original_airtypes) <= lengthof(_airtypes));

	uint i = 0;
	for (; i < lengthof(_original_airtypes); i++) _airtypes[i] = _original_airtypes[i];

	static const AirTypeInfo empty_airtype = {
			{	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // Ground sprite
				  0, 0, 0, 0, 0, 0, 0, 0, 0, 0
				},
				{
					{	// Airport buildings with infrastructure: non-snowed/snowed + 5 building + 4 rotations
						{ 0, 0, 0, 0 },
						{ 0, 0, 0, 0 },
						{ 0, 0, 0, 0 },
						{ 0, 0, 0, 0 },
						{ 0, 0, 0, 0 }
					},
					{
						{ 0, 0, 0, 0 },
						{ 0, 0, 0, 0 },
						{ 0, 0, 0, 0 },
						{ 0, 0, 0, 0 },
						{ 0, 0, 0, 0 }
					}
				},
				{	// Airport animated flag    revise: maybe 4 sprites instead of 16 is enough
					{ 0, 0, 0, 0 },
					{ 0, 0, 0, 0 },
					{ 0, 0, 0, 0 },
					{ 0, 0, 0, 0 }
				},
				{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // Radar sprites
				{ // Infrastructure with no catchment (with 4 rotated sprites): transmitter, snowed transmitter, tower, snowed tower
					{ 0, 0, 0, 0 },
					{ 0, 0, 0, 0 },
					{ 0, 0, 0, 0 },
					{ 0, 0, 0, 0 }
				},
				{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // Runway sprites
				{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0  }, // Sprites for normal apron, helipad, 4 rotated sprites for heliports and 4 more for snowed heliports
				{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } // Hangar sprites
			},
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // Icons
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // Cursors
			{ 0, 0, 0, 0 }, // Strings
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, false };
	for (; i < lengthof(_airtypes); i++) _airtypes[i] = empty_airtype;
}

/**
 * Compare airtypes based on their sorting order.
 * @param first  The airtype to compare to.
 * @param second The airtype to compare.
 * @return True iff the first should be sorted before the second.
 */
static bool CompareAirTypes(const AirType &first, const AirType &second)
{
	const auto left = GetAirTypeInfo(first)->sorting_order;
	const auto right = GetAirTypeInfo(second)->sorting_order;
	return left != right ? left < right : first < second;
}

/**
 * Build the sorted list of buildable air types.
 */
void InitAirTypes()
{
	_sorted_airtypes.clear();
	for (AirType at = AIRTYPE_BEGIN; at != AIRTYPE_END; at++) {
		if (_airtypes[at].label != 0) {
			_sorted_airtypes.push_back(at);
		}
	}
	std::sort(_sorted_airtypes.begin(), _sorted_airtypes.end(), CompareAirTypes);
}

/**
 * Set the new airport layout, rotation and airtype of old airports
 * (even those built with OpenGRF+Airports).
 * @param st Station with the airport to update, if there is one.
 */
void ConvertOldAirportData(Station *st) {
	if (st->airport.tile == INVALID_TILE) return;

	st->airport.air_type = INVALID_AIRTYPE;

	if (st->airport.type < NEW_AIRPORT_OFFSET) return;

	if (st->airport.type == NEW_AIRPORT_OFFSET + 1) {
		/* Already existing water airports built on land cannot be converted properly.
		 * Handle them as standard small gravel airports. */
		st->airport.type = NEW_AIRPORT_OFFSET;
	}

	st->airport.layout   = 0;

	/* Old rotation dir values are 0, 2, 4, 6.
	 * Convert them to 0, 1, 2, 3 (DiagDirection values).
	 * Savegames written by other patch packs or by upstream do not store the rotation, so it
	 * is invalid there; those airports are never rotated, so fall back to north. */
	uint rotation = static_cast<uint>(st->airport.rotation);
	if (rotation > 6 || rotation % 2 != 0) {
		Debug(misc, 1, "Station {}: airport rotation {} is not a legacy rotation, using north", st->index, rotation);
		rotation = 0;
	}
	st->airport.rotation = static_cast<Direction>(rotation / 2);

	if (st->airport.type > 23) {
		/* OpenGRF+Airports includes airport types from NEW_AIRPORT_OFFSET to 23.
		 * Do not try to convert a completely unknown airport type. */
		Debug(misc, 0, "Unknown airport type: station {} airport type {}", st->index, st->airport.type);
		return;
	}
}

/**
 * Whether airports are converted into custom airports.
 * This is tied to the "allow modifying airports" (custom airport) setting: with the
 * feature enabled every airport is a custom airport, with the feature disabled
 * airports keep their spec-based form.
 */
bool ConvertAirportsToCustomEnabled()
{
	return _settings_game.station.allow_modify_airports;
}

/**
 * After loading an old savegame, update type and tracks of airport tiles.
 */
void AfterLoadSetAirportTileTypes()
{
	for (Station *st : Station::Iterate()) {
		/* Old savegames stored the taken airport blocks in this field, using the last block to flag
		 * the airport as closed. The blocks are meaningless here, so keep only the closed state. */
		st->airport.flags = HasBit(st->airport.flags, 63) ? AF_CLOSED_MANUAL : AF_NONE;

		ConvertOldAirportData(st);
		/* With the custom airport feature enabled the airport is rebuilt as a custom airport;
		 * otherwise the tiles are rebuilt from the airport spec as usual. */
		if (ConvertAirportsToCustomEnabled() && st->ConvertAirportToCustom()) continue;

		/* Savegames written by other patch packs or by upstream do not store the rotation of
		 * the airport; those airports are never rotated, so fall back to north. */
		uint8_t rotation = static_cast<uint8_t>(st->airport.rotation);
		if (rotation >= static_cast<uint8_t>(DiagDirection::End)) rotation = 0;
		st->airport.rotation = static_cast<Direction>(rotation);

		st->LoadAirportTilesFromSpec(st->airport, static_cast<DiagDirection>(rotation), st->airport.air_type);
		/* The tiles are in the new format now, so derive the airport data (and the hangar depot) from them. */
		st->UpdateAirportDataStructure();
	}
}

/** Clear data about infrastructure of airport */
void Station::ClearAirportDataInfrastructure() {
	this->airport.Clear();
	this->airport.num_tiles = 0;
	this->airport.air_type = INVALID_AIRTYPE;
	this->airport.aprons.clear();
	this->airport.helipads.clear();
	this->airport.heliports.clear();
	this->airport.runways.clear();
	/* Hangars are saved Depot pool items, not disposable cache entries.
	 * Keep their identity until UpdateAirportDataStructure checks the map. */
	ClrBit(this->airport.flags, AFB_HANGAR);
	ClrBit(this->airport.flags, AFB_LANDING_RUNWAY);
}

/**
 * Convert a tile into an oil rig built-in heliport airport tile.
 * @param t Tile to convert, may be an Industry tile (during creation) or a
 *          legacy StationType::Oilrig station tile (during load).
 * @param sid Station the tile shall belong to.
 * @param wc Water class for the tile, captured before making it a station tile.
 */
void MakeBuiltInHeliportTile(TileIndex t, StationID sid, WaterClass wc)
{
	MakeStation(t, OWNER_NONE, sid, StationType::Airport, 0, wc);

	const AirportSpec *as = AirportSpec::Get(AT_OILRIG);
	const AirportTileTable *tile_desc = &as->layouts[0].tiles[0];
	Tile(t).m4() = 0;
	Tile(t).m5() = 0;
	SetAirType(t, as->airtype);
	SetAirportTileType(t, tile_desc->type);
	SetAirportTileTracks(t, tile_desc->trackbits);
	SetApronType(t, tile_desc->apron_type);
}

void UpdateTracks(TileIndex tile)
{
	assert(IsAirportTile(tile));
	if (!MayHaveAirTracks(tile) || IsHangar(tile)) return;
	TrackBits tracks = GetAllowedTracks(tile) & GetAirportTileTracks(tile);
	if (tracks != GetAirportTileTracks(tile)) {
		Debug(misc, 0, "Removing invalid track on tile {}", tile);
	}

	SetAirportTileTracks(tile, tracks);
}

/**
 * Get the start or end of a runway.
 * @param tile Tile belonging a runway.
 * @param dir Direction to follow.
 * @return a runway extreme.
 */
TileIndex GetRunwayExtreme(TileIndex tile, DiagDirection dir)
{
	assert(IsAirportTile(tile) && IsRunway(tile));

	TileIndexDiff delta = TileOffsByDiagDir(dir);
	TileIndex t = tile;

	for (;;) {
		assert(IsAirportTile(t));
		assert(IsRunway(t));
		if (IsRunwayExtreme(t)) {
			DiagDirection last_dir = GetRunwayExtremeDirection(t);
			if (IsRunwayEnd(t)) {
				if (last_dir == dir) return t;
				assert(last_dir == ReverseDiagDir(dir));
			} else {
				assert(IsRunwayStart(t));
				if (last_dir == ReverseDiagDir(dir)) return t;
				assert(last_dir == dir);
			}
		}
		t += delta;
	}
}

/**
 * Check if a tile is a valid continuation of a runway.
 * The tile \a test_tile is a valid continuation to \a runway, if all of the following are true:
 * \li \a test_tile is an airport tile
 * \li \a test_tile and \a start_tile are in the same station
 * \li the tracks on \a test_tile and \a start_tile are in the same direction
 * @param test_tile Tile to test
 * @param start_tile Depot tile to compare with
 * @pre IsAirport && IsRunwayStart(start_tile)
 * @return true if the two tiles are compatible
 */
inline bool IsCompatibleRunwayTile(TileIndex test_tile, TileIndex start_tile)
{
	assert(IsAirportTile(start_tile) && IsRunwayStart(start_tile));
	return IsAirportTile(test_tile) &&
			GetStationIndex(test_tile) == GetStationIndex(start_tile) &&
			(GetRunwayTrackdirs(start_tile) & GetRunwayTrackdirs(test_tile)) != TRACKDIR_BIT_NONE;
}

/**
 * Get the runway length.
 * @param tile Runway start tile
 * @return the length of the runway.
 */
uint GetRunwayLength(TileIndex tile)
{
	assert(IsAirport(tile) && IsRunwayStart(tile));
	DiagDirection dir = GetRunwayExtremeDirection(tile);
	assert(IsValidDiagDirection(dir));

	uint length = 1;
	[[maybe_unused]] TileIndex start_tile = tile;

	do {
		length++;
		tile += TileOffsByDiagDir(dir);
		assert(IsCompatibleRunwayTile(tile, start_tile));
	} while (!IsRunwayEnd(tile));

	return length;
}

/**
 * Set the reservation for a complete station platform.
 * @pre IsRailStationTile(start)
 * @param tile starting tile of the platform
 * @param b the state the reservation should be set to
 */
void SetRunwayReservation(TileIndex tile, bool b)
{
	assert(IsRunwayExtreme(tile));
	DiagDirection runway_dir = GetRunwayExtremeDirection(tile);
	if (IsRunwayEnd(tile)) runway_dir = ReverseDiagDir(runway_dir);
	TileIndexDiff diff = TileOffsByDiagDir(runway_dir);

	do {
		assert(IsAirportTile(tile));
		assert(!HasAirportTrackReserved(tile));
		SetReservationAsRunway(tile, b);
		if (_show_airport_tracks) MarkTileDirtyByTile(tile);
		tile = TileAdd(tile, diff);
	} while (!IsRunwayExtreme(tile));

	SetReservationAsRunway(tile, b);
	if (_show_airport_tracks) MarkTileDirtyByTile(tile);
}

/**
 * Get the position in the tile spec of a tile of a tilearea.
 * @param tile one tile of the area of the airport.
 * @param tile_area of the airport.
 * @param rotation the rotation of the airport.
 */
uint RotatedAirportSpecPosition(const TileIndex tile, const TileArea tile_area, const DiagDirection rotation)
{
	/* Get the tile difference between current tile and northern tile of the airport.
	 * @revise the function TileIndexToTileIndexDiffC as it seems to return the difference
	 * between the first tile minus the second one (one would expect to be the opposite).
	 */
	TileIndexDiffC tile_diff = TileIndexToTileIndexDiffC(tile, tile_area.tile);

	switch (static_cast<uint>(rotation)) {
		case 0:
			break;
		case 1:
			tile_diff = {(int16_t)(tile_area.h - 1 - tile_diff.y), (int16_t)tile_diff.x};
			break;
		case 2:
			tile_diff = {(int16_t)(tile_area.w - 1 - tile_diff.x), (int16_t)(tile_area.h - 1 - tile_diff.y)};
			break;
		case 3:
			tile_diff = {(int16_t)tile_diff.y, (int16_t)(tile_area.w - 1 - tile_diff.x)};
			break;
		default: NOT_REACHED();
	}

	return tile_diff.x + tile_diff.y * (static_cast<uint>(rotation) % 2 == 0 ? tile_area.w : tile_area.h);
}

/**
 * Rotate the trackbits as indicated by a direction
 * @param track_bits to rotate.
 * @param dir indicating how to rotate track_bits.
 *             (0 -> no rotation,
 *              1 -> 90 clockwise,
 *              2 -> 180 clockwise,
 *              3 -> 270 clockwise).
 * @return the rotated trackbits.
 */
TrackBits RotateTrackBits(TrackBits track_bits, DiagDirection dir)
{
	static const TrackBits rotation_table[static_cast<size_t>(DiagDirection::End)][TRACK_END] = {
		{ TRACK_BIT_X, TRACK_BIT_Y, TRACK_BIT_UPPER, TRACK_BIT_LOWER, TRACK_BIT_LEFT,  TRACK_BIT_RIGHT },
		{ TRACK_BIT_Y, TRACK_BIT_X, TRACK_BIT_RIGHT, TRACK_BIT_LEFT,  TRACK_BIT_UPPER, TRACK_BIT_LOWER },
		{ TRACK_BIT_X, TRACK_BIT_Y, TRACK_BIT_LOWER, TRACK_BIT_UPPER, TRACK_BIT_RIGHT, TRACK_BIT_LEFT  },
		{ TRACK_BIT_Y, TRACK_BIT_X, TRACK_BIT_LEFT,  TRACK_BIT_RIGHT, TRACK_BIT_LOWER, TRACK_BIT_UPPER }
	};

	TrackBits rotated = TRACK_BIT_NONE;
	for (Track track : SetTrackBitIterator(track_bits)) {
		rotated |= rotation_table[static_cast<uint>(dir)][track];
	}

	return rotated;
}

void Station::LoadAirportTilesFromSpec(TileArea ta, DiagDirection rotation, AirType airtype, bool force_airtype_gfx)
{
	if (this->airport.tile == INVALID_TILE) return;

	const AirportSpec *as = this->airport.GetSpec();
	if (airtype == INVALID_AIRTYPE) airtype = as->airtype;
	this->airport.air_type = airtype;

	const AirportTileLayout &layout = as->layouts[this->airport.layout];

	uint expected_tiles = 0;
	for (const AirportTileTable &tile_desc : layout.tiles) {
		if (tile_desc.type != ATT_INVALID) expected_tiles++;
	}
	uint written_tiles = 0;

	for (Tile t : ta) {
		uint pos = RotatedAirportSpecPosition(t, ta, rotation);
		if (pos >= layout.tiles.size()) {
			/* The saved area is not the size of the airport layout; this should have been
			 * caught before calling this function, so do not write garbage. */
			Debug(misc, 1, "Station {}: tile {} ({}, {}) maps outside airport layout {} ({} tiles), skipping", this->index, (uint)t, TileX(t), TileY(t), this->airport.layout, (uint)layout.tiles.size());
			continue;
		}
		const AirportTileTable *airport_tile_desc = &layout.tiles[pos];
		if (airport_tile_desc->type == ATT_INVALID) continue;
		if (!this->TileBelongsToAirport(t)) {
			/* Not every tile in the airport spec area is an airport tile of this station, e.g. the
			 * heliport of an oil rig, or a tile of another station. Cannot be rewritten from the spec. */
			Debug(misc, 4, "Station {}: not rewriting tile {} ({}, {}) as it is not an airport tile of this station", this->index, (uint)t, TileX(t), TileY(t));
			continue;
		}

		t.m5() = 0;

		SB(_me[t].m6, 3, 3, to_underlying(StationType::Airport));
		SetAirType(t, airtype);
		SetAirportTileType(t, airport_tile_desc->type);

		bool airtype_gfx = force_airtype_gfx || airtype != as->airtype || airport_tile_desc->gfx[static_cast<size_t>(rotation)] == INVALID_AIRPORTTILE;
		SetAirGfxType(t, airtype_gfx);
		SetTileAirportGfx(t, airtype_gfx ? airport_tile_desc->at_gfx : airport_tile_desc->gfx[static_cast<size_t>(rotation)]);

		switch (GetAirportTileType(t)) {
			case ATT_INFRASTRUCTURE_WITH_CATCH:
			case ATT_INFRASTRUCTURE_NO_CATCH:
				SetAirportTileRotation(t, (DiagDirection)((static_cast<uint>(rotation) + static_cast<uint>(airport_tile_desc->dir)) % static_cast<uint>(DiagDirection::End)));
				if (!airtype_gfx) SetAirportGfxForAirtype(t, airport_tile_desc->at_gfx);
				break;

			case ATT_SIMPLE_TRACK:
				break;
			case ATT_HANGAR_STANDARD:
			case ATT_HANGAR_EXTENDED:
				SetHangarDirection(t, RotateDiagDir(airport_tile_desc->dir, rotation));
				break;

			case ATT_APRON_NORMAL:
			case ATT_APRON_HELIPAD:
			case ATT_APRON_HELIPORT:
			case ATT_APRON_BUILTIN_HELIPORT:
				SetAirportTileRotation(t, (DiagDirection)((static_cast<uint>(rotation) + static_cast<uint>(airport_tile_desc->dir)) % static_cast<uint>(DiagDirection::End)));
				break;

			case ATT_RUNWAY_MIDDLE:
				SB(t.m8(), 12, 2, to_underlying(RotateDirection(airport_tile_desc->runway_directions, rotation)));
				break;

			case ATT_RUNWAY_START_NO_LANDING:
			case ATT_RUNWAY_START_ALLOW_LANDING:
			case ATT_RUNWAY_END:
				SB(t.m8(), 12, 2, to_underlying(RotateDiagDir(airport_tile_desc->dir, rotation)));
				break;
			case ATT_WAITING_POINT:
				NOT_REACHED();
			default: NOT_REACHED();
		}

		if (!IsInfrastructure(t)) {
			SetAirportTileTracks(t, RotateTrackBits(airport_tile_desc->trackbits, rotation));
		}

		written_tiles++;
	}

	if (written_tiles != expected_tiles) {
		Debug(misc, 1, "Station {}: rebuilt {} airport tiles, but layout {} expects {} tiles", this->index, written_tiles, this->airport.layout, expected_tiles);
	} else {
		Debug(misc, 4, "Station {}: rebuilt {} airport tiles from layout {} (rotation {})", this->index, written_tiles, this->airport.layout, static_cast<uint>(rotation));
	}

	this->UpdateAirportDataStructure();
}

/**
 * Check whether the tiles of an airport are exactly the tiles the given layout
 * produces at the given rotation.
 * @param st The station to check.
 * @param layout The layout to check against.
 * @param rotation The rotation to check against.
 * @return true iff every airport tile of the station matches the layout.
 */
static bool AirportTilesMatchLayout(const Station *st, const AirportTileLayout &layout, uint8_t rotation)
{
	for (TileIndex t : st->airport) {
		if (!st->TileBelongsToAirport(t)) continue;
		uint pos = RotatedAirportSpecPosition(t, st->airport, static_cast<DiagDirection>(rotation));
		if (pos >= layout.tiles.size()) return false;
		const AirportTileTable &tile_desc = layout.tiles[pos];
		if (tile_desc.type == ATT_INVALID) return false;
		if (tile_desc.type != GetAirportTileType(t)) return false;
	}
	return true;
}

/**
 * Convert this station's airport into a custom (tile-defined) airport.
 *
 * The tiles are rebuilt from the airport spec layout using the air type graphics of
 * the stock airport the spec belongs to, and the station's airport type is changed to
 * AT_CUSTOM. Afterwards the airport no longer depends on the airport spec, and
 * therefore no longer on the NewGRF which provided that spec.
 *
 * The airport is only converted when the tiles which would be produced fit the
 * airport's footprint exactly; otherwise the existing tiles cannot be rewritten
 * correctly and the airport is left alone. That can happen when the spec no longer
 * describes these tiles, e.g. because the NewGRF which defined the airport is gone
 * (so the airport type maps to a different stock airport) or because the NewGRF
 * changed the layout of the airport.
 *
 * @param verify_tiles Also check that the existing tiles match the layout, and if they
 *        do not match the stored rotation, look for the rotation which does match.
 *        This is used for savegames which already store the tiles in the new format.
 * @return true iff the airport has been converted by this call.
 */
bool Station::ConvertAirportToCustom(bool verify_tiles)
{
	if (this->airport.tile == INVALID_TILE) return false;
	if (this->airport.type == AT_CUSTOM) return false;
	if (this->airport.type == AT_OILRIG) return false; // Oil rigs are not converted to custom airports.
	if (this->airport.type >= NUM_AIRPORTS) return false; // AT_INVALID, AT_DUMMY or garbage from a savegame.

	const AirportSpec *as = this->airport.GetSpec();
	if (!as->IsConvertibleToCustom()) {
		Debug(misc, 2, "Station {}: airport type {} is not convertible to a custom airport", this->index, static_cast<uint>(this->airport.type));
		return false;
	}

	uint8_t layout = this->airport.layout;
	if (layout >= as->layouts.size()) {
		/* Savegames of NewGRF airports which are no longer loaded may refer to a rotated layout. */
		if (this->airport.type < NEW_AIRPORT_OFFSET) {
			Debug(misc, 1, "Station {}: layout {} does not exist for airport type {}, not converting to a custom airport", this->index, static_cast<uint>(layout), static_cast<uint>(this->airport.type));
			return false;
		}
		layout = 0;
	}

	const AirportTileLayout &tile_layout = as->layouts[layout];

	/* The rotation is stored as a diagonal direction; normalise anything else (e.g. a
	 * Direction or an invalid value from an old savegame) to north. */
	uint8_t rotation = static_cast<uint8_t>(this->airport.rotation);
	if (rotation >= static_cast<uint8_t>(DiagDirection::End)) rotation = 0;

	auto rotation_fits = [&](uint8_t rot) -> bool {
		if (this->airport.w == 0 || this->airport.h == 0) return true; // The dimensions are adopted below.
		uint w = tile_layout.size_x;
		uint h = tile_layout.size_y;
		if (rot % 2 != 0) std::swap(w, h);
		if (this->airport.w != w || this->airport.h != h) return false;
		return !verify_tiles || AirportTilesMatchLayout(this, tile_layout, rot);
	};

	if (!rotation_fits(rotation)) {
		if (!verify_tiles) {
			Debug(misc, 1, "Station {}: airport area {}x{} does not match layout {} of airport type {}, not converting to a custom airport", this->index, this->airport.w, this->airport.h, static_cast<uint>(layout), static_cast<uint>(this->airport.type));
			return false;
		}

		/* The stored rotation does not describe the tiles (older savegames did not store
		 * it for airports built in a rotated orientation); find the rotation which does. */
		bool found = false;
		for (uint8_t rot = 0; rot < static_cast<uint8_t>(DiagDirection::End); rot++) {
			if (rot == rotation || !rotation_fits(rot)) continue;
			rotation = rot;
			found = true;
			break;
		}
		if (!found) {
			Debug(misc, 1, "Station {}: airport tiles do not match layout {} of airport type {}, not converting to a custom airport", this->index, static_cast<uint>(layout), static_cast<uint>(this->airport.type));
			return false;
		}
		Debug(misc, 1, "Station {}: airport type {} was built with rotation {}, correcting the stored rotation", this->index, static_cast<uint>(this->airport.type), static_cast<uint>(rotation));
	}

	/* Adopt the airport dimensions from the layout if they were not stored. */
	if (this->airport.w == 0 || this->airport.h == 0) {
		uint w = tile_layout.size_x;
		uint h = tile_layout.size_y;
		if (rotation % 2 != 0) std::swap(w, h);
		this->airport.w = static_cast<uint16_t>(w);
		this->airport.h = static_cast<uint16_t>(h);
	}

	const uint8_t old_type = this->airport.type;

	/* Keep the air type the airport currently uses, so that a converted airport keeps its
	 * surface: the player may have built it with another air type than the default one of
	 * the airport spec, or changed it afterwards. When the tiles are in the self-describing
	 * format they store the air type, otherwise (old saves) the air type of the spec is all
	 * there is. */
	AirType airtype = this->airport.air_type;
	if (airtype == INVALID_AIRTYPE && verify_tiles) {
		for (TileIndex t : this->airport) {
			if (!this->TileBelongsToAirport(t)) continue;
			airtype = GetAirType(t);
			break;
		}
	}
	if (airtype == INVALID_AIRTYPE || airtype >= AIRTYPE_END) airtype = as->GetCustomAirType();

	/* Rebuild the tiles with the air type graphics, i.e. without the (NewGRF) graphics of
	 * the layout, and then turn the airport into a custom airport. */
	this->airport.layout = layout;
	this->airport.rotation = static_cast<Direction>(rotation);
	this->LoadAirportTilesFromSpec(this->airport, static_cast<DiagDirection>(rotation), airtype, true);

	/* The airport no longer uses the spec, so drop the NewGRF persistent storage as well. */
	delete this->airport.psa;
	this->airport.psa = nullptr;

	this->airport.type = AT_CUSTOM;
	this->airport.layout = 0;

	Debug(misc, 2, "Station {}: converted airport type {} to a custom airport (air type {})", this->index, static_cast<uint>(old_type), static_cast<uint>(airtype));
	return true;
}

/** Update cached variables after loading a game or modifying an airport */
void Station::UpdateAirportDataStructure()
{
	this->ClearAirportDataInfrastructure();

	/* Legacy oil rigs were represented by a single StationType::Oilrig tile,
	 * which is not a StationType::Airport tile and thus would be missed by the
	 * generic airport tile scan below. In that case the airport's tile would be
	 * silently invalidated both when loading a game and when rebuilding the
	 * data after modifications, which makes helicopters unable to use the oil
	 * rig and desynchronizes joining clients. Upgrade legacy tiles to the
	 * modular built-in heliport representation, which keeps the same map
	 * features but is fully supported by the airport data structure. */
	TileArea ta(TileXY(this->rect.left, this->rect.top), TileXY(this->rect.right, this->rect.bottom));
	for (TileIndex t : ta) {
		if (IsTileType(t, TileType::Station) && IsOilRig(t) && GetStationIndex(t) == this->index) {
			WaterClass wc = GetWaterClass(t);
			MakeBuiltInHeliportTile(t, this->index, wc);
		}
	}

	/* Recover the airport area tile rescanning the rect of the station */
	/* At the same time, detect any hangar. */
	TileIndex first_hangar = INVALID_TILE;

	TileArea airport_area;
	for (TileIndex t : ta) {
		if (!this->TileBelongsToAirport(t)) continue;
		airport_area.Add(t);

		/* Savegames written by other patch packs, or older savegames of this patch pack, can
		 * contain airport tiles whose air type differs from the air type of the airport, or is
		 * not a valid air type at all. The air type of the airport is authoritative, so correct
		 * the tile instead of failing an assertion. */
		const AirType tile_air_type = static_cast<AirType>(GB(Tile(t).m3(), 0, 4));
		if (this->airport.air_type == INVALID_AIRTYPE && tile_air_type < AIRTYPE_END) this->airport.air_type = tile_air_type;

		if (this->airport.air_type != INVALID_AIRTYPE && tile_air_type != this->airport.air_type) {
			Debug(misc, 1, "Station {}: airport tile {} has air type {}, expected {}, correcting it",
					this->index, (uint)t.base(), static_cast<uint>(tile_air_type), static_cast<uint>(this->airport.air_type));
			SetAirType(t, this->airport.air_type);
		}

		if (first_hangar == INVALID_TILE && IsHangar(t)) first_hangar = t;
	}

	/* Reconnect a saved hangar before allocating anything: the station's
	 * pointer is not saved, but the Depot pool (including its IDs) is. */
	if (first_hangar != INVALID_TILE) {
		Depot *canonical = this->airport.hangar;
		for (Depot *depot : Depot::Iterate()) {
			if (!IsValidTile(depot->xy) || !this->TileBelongsToAirport(depot->xy) || !IsHangar(depot->xy)) continue;
			/* Legacy saves may contain duplicates. Both a running server and a
			 * joining client must choose the same identity, regardless of an old pointer. */
			if (canonical == nullptr || depot->index < canonical->index) canonical = depot;
		}
		this->airport.hangar = canonical;
	}

	/* Set/Clear depot. */
	if (first_hangar != INVALID_TILE && this->airport.hangar == nullptr) {
		if (!Depot::CanAllocateItem()) NOT_REACHED();
		this->airport.hangar = Depot::Create(first_hangar);
		this->airport.hangar->build_date = this->build_date;
		this->airport.hangar->town = this->town;
		SetBit(this->airport.flags, AFB_HANGAR);
	} else if (this->airport.hangar != nullptr) {
		if (first_hangar == INVALID_TILE) {
			ClrBit(this->airport.flags, AFB_HANGAR);
			delete this->airport.hangar;
			this->airport.hangar = nullptr;
		} else {
			SetBit(this->airport.flags, AFB_HANGAR);
			this->airport.hangar->xy = first_hangar;
		}
	} else {
		/* No hangar tile and no existing hangar Depot: clear any stale hangar flag so that
		 * HasHangar() does not report a hangar that no longer exists. */
		ClrBit(this->airport.flags, AFB_HANGAR);
	}

	if (airport_area.tile == INVALID_TILE) return;

	bool allow_landing = false;
	for (TileIndex t : airport_area) {
		if (!this->TileBelongsToAirport(t)) continue;
		this->airport.Add(t);
		this->airport.num_tiles++;

		assert(this->airport.air_type == INVALID_AIRTYPE || this->airport.air_type == GetAirType(t));

		if (!MayHaveAirTracks(t)) continue;

		UpdateTracks(t);

		switch (GetAirportTileType(t)) {
			case ATT_HANGAR_STANDARD:
			case ATT_HANGAR_EXTENDED:
				assert(this->airport.HasHangar());
				this->airport.hangar->xy = t;
				break;

			case ATT_APRON_NORMAL:
				this->airport.aprons.emplace_back(t);
				break;
			case ATT_APRON_HELIPAD:
				this->airport.helipads.emplace_back(t);
				break;
			case ATT_APRON_HELIPORT:
			case ATT_APRON_BUILTIN_HELIPORT:
				this->airport.heliports.emplace_back(t);
				break;

			case ATT_RUNWAY_START_ALLOW_LANDING:
				allow_landing = true;
				[[fallthrough]];
			case ATT_RUNWAY_START_NO_LANDING:
				this->airport.runways.emplace_back(t);
				break;

			default: break;
		}
	}

	if (this->airport.hangar != nullptr) InvalidateWindowData(WindowClass::BuildVehicle, this->airport.hangar->index);

	if (this->airport.HasLandingRunway() != allow_landing) {
		ToggleBit(this->airport.flags, AFB_LANDING_RUNWAY);
	}
}

/**
 * Return the tracks a tile could have.
 * It returns the tracks the tile has plus the extra tracks that
 * could also exist on the tile.
 * @param tile
 * @return The tracks the tile could have.
 */
TrackBits GetAllowedTracks(TileIndex tile)
{
	assert(IsAirportTile(tile));
	switch (GetAirportTileType(tile)) {
		case ATT_INFRASTRUCTURE_NO_CATCH:
		case ATT_INFRASTRUCTURE_WITH_CATCH:
			return TRACK_BIT_NONE;

		case ATT_HANGAR_STANDARD:
		case ATT_HANGAR_EXTENDED:
			return HasBit(Tile(tile).m8(), 15) ? TRACK_BIT_Y: TRACK_BIT_X;

		case ATT_APRON_HELIPORT:
		case ATT_APRON_BUILTIN_HELIPORT:
			return TRACK_BIT_CROSS;

		case ATT_APRON_NORMAL:
		case ATT_APRON_HELIPAD:
		case ATT_SIMPLE_TRACK:
		case ATT_RUNWAY_MIDDLE:
		case ATT_RUNWAY_END:
		case ATT_RUNWAY_START_NO_LANDING:
		case ATT_RUNWAY_START_ALLOW_LANDING: {
			TrackBits tracks = TRACK_BIT_ALL;

			const TrackBits rem_tracks[] = {
				~TRACK_BIT_UPPER,
				~(TRACK_BIT_UPPER | TRACK_BIT_RIGHT),
				~TRACK_BIT_RIGHT,
				~(TRACK_BIT_LOWER | TRACK_BIT_RIGHT),
				~TRACK_BIT_LOWER,
				~(TRACK_BIT_LOWER | TRACK_BIT_LEFT),
				~TRACK_BIT_LEFT,
				~(TRACK_BIT_UPPER | TRACK_BIT_LEFT),
			};

			for (Direction dir = Direction::Begin; dir < Direction::End; dir++) {
				TileIndex t = TileAddByDir(tile, dir);
				if (!IsValidTile(t) || !IsAirportTile(t) ||
					GetStationIndex(t) != GetStationIndex(tile) || !MayHaveAirTracks(t)) {
					tracks &= rem_tracks[static_cast<uint>(dir)];
					} else if (IsHangar(t)) {
						tracks &= rem_tracks[static_cast<uint>(dir)];
					}
			}

			return tracks;
		}

		default:
			/* Savegames from other patch packs, or older savegames of this patch pack, can contain
			 * airport tiles which are not in the multi-tile format; those are not usable as airport
			 * tiles, but they must not take down the game either. */
			Debug(misc, 1, "Station {}: airport tile {} has an invalid tile type {}, not allowing any tracks",
					GetStationIndex(tile).base(), tile.base(), (unsigned)GetAirportTileType(tile));
			return TRACK_BIT_NONE;
	}
}

/**
 * Get the sprite for an airport tile.
 * @param t Tile to get the sprite of.
 * @return AirportTile ID.
 */
StationGfx GetAirportGfx(TileIndex t)
{
	assert(IsTileType(t, TileType::Station));
	assert(IsAirport(t));

	if (!HasAirtypeGfx(t)) return GetTranslatedAirportTileID(GetTileAirportGfx(t));

	switch (GetAirportTileType(t)) {
		case ATT_INFRASTRUCTURE_NO_CATCH:
		case ATT_INFRASTRUCTURE_WITH_CATCH:
			return GetTileAirportGfx(t);

		case ATT_SIMPLE_TRACK:
			return (StationGfx)0;

		case ATT_HANGAR_STANDARD:
		case ATT_HANGAR_EXTENDED:
			return (StationGfx)0;

		case ATT_APRON_NORMAL:
		case ATT_APRON_HELIPAD:
		case ATT_APRON_HELIPORT:
		case ATT_APRON_BUILTIN_HELIPORT:
			switch (GetApronType(t)) {
				case APRON_APRON:
				case APRON_HELIPAD:
				case APRON_HELIPORT:
					return (StationGfx)0;
				case APRON_BUILTIN_HELIPORT:
					return (StationGfx)0; // oil rig heliport
				default: NOT_REACHED();
			}

		case ATT_RUNWAY_MIDDLE:
		case ATT_RUNWAY_START_NO_LANDING:
		case ATT_RUNWAY_START_ALLOW_LANDING:
		case ATT_RUNWAY_END: {
			return (StationGfx)0;
			break;
		}

		case ATT_WAITING_POINT:
			NOT_REACHED();
		default:
			NOT_REACHED();
	}
}

