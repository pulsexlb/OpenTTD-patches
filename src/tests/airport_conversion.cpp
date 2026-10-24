/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file airport_conversion.cpp Tests for converting airports into custom airports. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../air.h"
#include "../air_map.h"
#include "../airport.h"
#include "../airport_cmd.h"
#include "../company_func.h"
#include "../core/backup_type.hpp"
#include "../depot_base.h"
#include "../map_func.h"
#include "../newgrf_airport.h"
#include "../scope.h"
#include "../settings_type.h"
#include "../station_base.h"
#include "../station_map.h"
#include "../table/strings.h"

#include "../safeguards.h"

/**
 * Create a station with an airport of the given type, and lay out its tiles from the
 * airport spec the same way CmdBuildAirport does, but without going through the
 * economy (money, town rating, town noise).
 *
 * @param tile Top corner of the airport.
 * @param airport_type Type of the airport to create.
 * @param layout Layout of the airport to create.
 * @param rotation Rotation of the airport to create.
 * @param airtype Air type to build the tiles with, or INVALID_AIRTYPE for the air type of the spec.
 * @return The created station.
 */
static Station *CreateSpecAirport(TileIndex tile, uint8_t airport_type, uint8_t layout, DiagDirection rotation, AirType airtype)
{
	const AirportSpec *as = AirportSpec::Get(airport_type);
	REQUIRE(as != nullptr);
	REQUIRE(layout < as->layouts.size());

	const uint w = as->layouts[layout].size_x;
	const uint h = as->layouts[layout].size_y;
	const bool rotated = to_underlying(rotation) % 2 != 0;

	Station *st = Station::Create(tile);
	st->facilities = {StationFacility::Airport};
	st->rect.BeforeAddRect(tile, rotated ? h : w, rotated ? w : h, StationRect::ADD_FORCE);
	st->airport.tile = tile;
	st->airport.w = static_cast<uint16_t>(rotated ? h : w);
	st->airport.h = static_cast<uint16_t>(rotated ? w : h);
	st->airport.type = airport_type;
	st->airport.layout = layout;
	st->airport.rotation = static_cast<Direction>(to_underlying(rotation));

	for (TileIndex t : st->airport) {
		MakeAirport(t, OWNER_BEGIN, st->index, 0, WaterClass::Invalid);
	}

	st->LoadAirportTilesFromSpec(st->airport, rotation, airtype);
	return st;
}

/**
 * Count the airport tiles of a station.
 * @param st The station to count the tiles of.
 * @return The number of airport tiles of the station.
 */
static uint CountAirportTiles(const Station *st)
{
	uint tiles = 0;
	for (TileIndex t : st->airport) {
		if (st->TileBelongsToAirport(t)) tiles++;
	}
	return tiles;
}

/**
 * Check that the airport tiles of a station can be drawn without the airport spec,
 * i.e. that they are all drawn through their air type.
 * @param st The station to check.
 * @param airtype The air type every tile is expected to have.
 * @return The number of airport tiles of the station.
 */
static uint CheckAirtypeDrawnTiles(const Station *st, AirType airtype)
{
	uint tiles = 0;
	for (TileIndex t : st->airport) {
		if (!st->TileBelongsToAirport(t)) continue;
		tiles++;

		CHECK(HasAirtypeGfx(t));
		CHECK(GetAirType(t) == airtype);
	}
	return tiles;
}

TEST_CASE("Airport - converting a stock airport to a custom airport keeps its air type")
{
	AllocateMap(64, 64);
	AirportSpec::ResetAirports(); // Populate the airport specs, which the tests below rely on.
	auto cleanup = scope_guard([] { _station_pool.CleanPool(); _depot_pool.CleanPool(); DeallocateMap(); });
	AutoRestoreBackup current(_current_company, OWNER_BEGIN);
	AutoRestoreBackup enabled(_settings_game.station.allow_modify_airports, true);

	Station *st = CreateSpecAirport(TileXY(10, 10), AT_LARGE, 0, DiagDirection::NE, INVALID_AIRTYPE);
	const AirType airtype = st->airport.air_type;
	CHECK(airtype == AirportSpec::Get(AT_LARGE)->airtype);
	REQUIRE(st->airport.HasHangar());
	const uint tiles = CountAirportTiles(st);
	REQUIRE(tiles > 0);

	/* The airport is still a spec-based airport, so its tiles use the graphics of the spec. */
	CHECK_FALSE(HasAirtypeGfx(st->airport.tile));

	REQUIRE(st->ConvertAirportToCustom(true));

	CHECK(st->airport.type == AT_CUSTOM);
	CHECK(st->airport.layout == 0);
	CHECK(st->airport.air_type == airtype);
	/* Converting must not add or lose tiles, nor lose the hangar. */
	CHECK(CheckAirtypeDrawnTiles(st, airtype) == tiles);
	CHECK(st->airport.HasHangar());

	/* Converting again must be a no-op. */
	CHECK_FALSE(st->ConvertAirportToCustom(true));
}

TEST_CASE("Airport - converting a NewGRF airport keeps the air type of the airport")
{
	AllocateMap(64, 64);
	AirportSpec::ResetAirports(); // Populate the airport specs, which the tests below rely on.
	auto cleanup = scope_guard([] { _station_pool.CleanPool(); _depot_pool.CleanPool(); DeallocateMap(); });
	AutoRestoreBackup current(_current_company, OWNER_BEGIN);
	AutoRestoreBackup enabled(_settings_game.station.allow_modify_airports, true);

	/* Simulate a NewGRF airport: it substitutes the small airport (which is a gravel
	 * airport), but it is built with the water air type. Converting it must keep the
	 * water air type of the airport, and not fall back to the air type of the stock
	 * airport it substituted. */
	AirportSpec &ngrf = *AirportSpec::GetWithoutOverride(NEW_AIRPORT_OFFSET);
	AutoRestoreBackup spec(ngrf, *AirportSpec::GetWithoutOverride(AT_SMALL));
	ngrf.enabled = true;
	ngrf.airtype = AIRTYPE_WATER;
	ngrf.grf_prop.subst_id = AT_SMALL;
	ngrf.grf_prop.override_id = AT_INVALID;
	ngrf.grf_prop.local_id = 0;
	CHECK(AirportSpec::GetWithoutOverride(AT_SMALL)->airtype == AIRTYPE_GRAVEL);
	CHECK(AirportSpec::Get(NEW_AIRPORT_OFFSET)->airtype == AIRTYPE_WATER);

	Station *st = CreateSpecAirport(TileXY(10, 10), NEW_AIRPORT_OFFSET, 0, DiagDirection::NE, ngrf.airtype);
	REQUIRE(st->airport.air_type == AIRTYPE_WATER);
	const uint tiles = CountAirportTiles(st);
	REQUIRE(tiles > 0);

	REQUIRE(st->ConvertAirportToCustom(true));

	CHECK(st->airport.type == AT_CUSTOM);
	CHECK(st->airport.air_type == AIRTYPE_WATER);
	CHECK(CheckAirtypeDrawnTiles(st, AIRTYPE_WATER) == tiles);
}

TEST_CASE("Airport - converting an airport built in a rotated orientation keeps its tiles")
{
	AllocateMap(64, 64);
	AirportSpec::ResetAirports(); // Populate the airport specs, which the tests below rely on.
	auto cleanup = scope_guard([] { _station_pool.CleanPool(); _depot_pool.CleanPool(); DeallocateMap(); });
	AutoRestoreBackup current(_current_company, OWNER_BEGIN);
	AutoRestoreBackup enabled(_settings_game.station.allow_modify_airports, true);

	/* The small airport is not square, so the rotation is visible in its dimensions. */
	Station *st = CreateSpecAirport(TileXY(10, 10), AT_SMALL, 0, DiagDirection::SW, INVALID_AIRTYPE);
	const uint16_t w = st->airport.w;
	const uint16_t h = st->airport.h;
	REQUIRE(w != h);
	const AirType airtype = st->airport.air_type;
	const uint tiles = CountAirportTiles(st);
	REQUIRE(tiles > 0);

	REQUIRE(st->ConvertAirportToCustom(true));

	CHECK(st->airport.type == AT_CUSTOM);
	CHECK(st->airport.w == w);
	CHECK(st->airport.h == h);
	CHECK(st->airport.rotation == static_cast<Direction>(to_underlying(DiagDirection::SW)));
	CHECK(CheckAirtypeDrawnTiles(st, airtype) == tiles);
}

TEST_CASE("Airport - converting keeps the air type the airport was built with")
{
	AllocateMap(64, 64);
	AirportSpec::ResetAirports(); // Populate the airport specs, which the tests below rely on.
	auto cleanup = scope_guard([] { _station_pool.CleanPool(); _depot_pool.CleanPool(); DeallocateMap(); });
	AutoRestoreBackup current(_current_company, OWNER_BEGIN);
	AutoRestoreBackup enabled(_settings_game.station.allow_modify_airports, true);

	/* An airport built with an air type other than the default one of its spec. */
	Station *st = CreateSpecAirport(TileXY(10, 10), AT_LARGE, 0, DiagDirection::NE, AIRTYPE_YELLOW);
	REQUIRE(st->airport.air_type == AIRTYPE_YELLOW);
	REQUIRE(AirportSpec::Get(AT_LARGE)->airtype != AIRTYPE_YELLOW);

	/* The air type is not saved; it is derived from the tiles when loading a game. */
	st->airport.air_type = INVALID_AIRTYPE;

	REQUIRE(st->ConvertAirportToCustom(true));

	CHECK(st->airport.air_type == AIRTYPE_YELLOW);
	CHECK(CheckAirtypeDrawnTiles(st, AIRTYPE_YELLOW) > 0);
}

TEST_CASE("Airport - loading a savegame from another patch pack rebuilds its airport tiles")
{
	AllocateMap(64, 64);
	AirportSpec::ResetAirports(); // Populate the airport specs, which the tests below rely on.
	auto cleanup = scope_guard([] { _station_pool.CleanPool(); _depot_pool.CleanPool(); DeallocateMap(); });
	AutoRestoreBackup current(_current_company, OWNER_BEGIN);
	AutoRestoreBackup enabled(_settings_game.station.allow_modify_airports, false);

	/* Savegames from other patch packs store the airport tile gfx in m5 and do not store the
	 * rotation of the airport, so the tiles have to be rebuilt from the airport spec. */
	const TileIndex tile = TileXY(10, 10);
	Station *st = Station::Create(tile);
	st->facilities = {StationFacility::Airport};
	st->rect.BeforeAddRect(tile, 4, 3, StationRect::ADD_FORCE);
	st->airport.tile = tile;
	st->airport.w = 4;
	st->airport.h = 3;
	st->airport.type = AT_SMALL;
	st->airport.layout = 0;
	st->airport.rotation = Direction::Invalid;
	for (TileIndex t : st->airport) {
		MakeAirport(t, OWNER_BEGIN, st->index, 0, WaterClass::Invalid);
		Tile(t).m5() = 0x2A; // An old style airport tile gfx.
	}

	AfterLoadSetAirportTileTypes();

	/* The tiles are in the self-describing format now, drawn with the graphics of the spec. */
	CHECK(st->airport.air_type == AIRTYPE_GRAVEL);
	uint tiles = 0;
	for (TileIndex t : st->airport) {
		if (!st->TileBelongsToAirport(t)) continue;
		tiles++;
		CHECK(GetAirType(t) == AIRTYPE_GRAVEL);
		CHECK_FALSE(HasAirtypeGfx(t));
	}
	CHECK(tiles > 0);
}

TEST_CASE("Airport - converting does not touch stations without a convertible airport")
{
	AllocateMap(64, 64);
	AirportSpec::ResetAirports(); // Populate the airport specs, which the tests below rely on.
	auto cleanup = scope_guard([] { _station_pool.CleanPool(); _depot_pool.CleanPool(); DeallocateMap(); });
	AutoRestoreBackup enabled(_settings_game.station.allow_modify_airports, true);

	/* A station without an airport. */
	Station *st = Station::Create(TileXY(10, 10));
	CHECK_FALSE(st->ConvertAirportToCustom());

	/* Oil rigs are not converted, their heliport is drawn as a built-in tile. */
	Station *oilrig = Station::Create(TileXY(20, 20));
	oilrig->airport.tile = oilrig->xy;
	oilrig->airport.w = 1;
	oilrig->airport.h = 1;
	oilrig->airport.type = AT_OILRIG;
	CHECK_FALSE(oilrig->ConvertAirportToCustom());
	CHECK(oilrig->airport.type == AT_OILRIG);

	/* An airport which is a custom airport already is left alone. */
	Station *custom = Station::Create(TileXY(30, 30));
	custom->airport.tile = custom->xy;
	custom->airport.w = 1;
	custom->airport.h = 1;
	custom->airport.type = AT_CUSTOM;
	CHECK_FALSE(custom->ConvertAirportToCustom());
}
