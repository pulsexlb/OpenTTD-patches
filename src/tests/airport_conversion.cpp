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
#include "../animated_tile.h"
#include "../blueprint_cmd.h"
#include "../company_func.h"
#include "../core/backup_type.hpp"
#include "../core/random_func.hpp"
#include "../depot_base.h"
#include "../map_func.h"
#include "../newgrf.h"
#include "../newgrf_airport.h"
#include "../newgrf_airporttiles.h"
#include "../scope.h"
#include "../settings_type.h"
#include "../station_base.h"
#include "../station_map.h"
#include "../script/api/script_airport.hpp"
#include "../town.h"
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

static constexpr GrfID TEST_AIRPORT_GRF("\x12\x34\xA1\xF7");
static constexpr GrfID OTHER_AIRPORT_GRF("\x12\x34\xA1\xF8");

TEST_CASE("Airport - reloading an unchanged material preserves its frame, ANIT and RNG")
{
	AllocateMap(64, 64);
	AutoRestoreBackup animated(_animated_tiles, AutoRestoreBackupNoNewValueTag{});
	_animated_tiles.clear();
	auto cleanup = scope_guard([] { _station_pool.CleanPool(); DeallocateMap(); });
	AutoRestoreBackup mappings(_airporttile_mngr.mappings, AutoRestoreBackupNoNewValueTag{});
	_airporttile_mngr.ResetMapping();
	constexpr StationGfx slot = NEW_AIRPORTTILE_OFFSET;
	AutoRestoreBackup spec(*const_cast<AirportTileSpec *>(AirportTileSpec::Get(slot)), AutoRestoreBackupNoNewValueTag{});
	GRFFile grf;
	grf.grfid = TEST_AIRPORT_GRF;
	CallbackResultSpriteGroup built_callback(3);
	AirportTileSpec newgrf{};
	newgrf.enabled = true;
	newgrf.grf_prop.SetGRFFile(&grf);
	newgrf.grf_prop.local_id = 2;
	newgrf.grf_prop.subst_id = 0;
	newgrf.grf_prop.override_id = INVALID_AIRPORTTILE;
	newgrf.grf_prop.SetSpriteGroup(StandardSpriteGroup::Default, &built_callback);
	newgrf.animation.status = AnimationStatus::Looping;
	newgrf.animation.frames = 5;
	newgrf.animation.speed = 4;
	newgrf.animation.triggers.Set(AirportAnimationTrigger::Built);
	_airporttile_mngr.SetEntitySpec(std::move(newgrf));

	TileIndex tile = TileXY(10, 10);
	TileIndex other = TileXY(11, 10);
	Station *st = Station::Create(tile);
	st->airport.type = AT_CUSTOM;
	st->airport.Add(tile);
	st->airport.Add(other);
	for (TileIndex t : st->airport) {
		MakeAirport(t, OWNER_BEGIN, st->index, 0, WaterClass::Invalid);
		SetAirGfxType(t, true);
		SetAirportTileType(t, ATT_INFRASTRUCTURE_NO_CATCH);
		SetTileAirportGfx(t, ATTG_NO_CATCH_RADAR);
	}

	const AirportTileGraphics graphics{TEST_AIRPORT_GRF, 2};
	REQUIRE(ResolveAirportTileGraphics(graphics) == slot);
	InstallAirportTileGraphics(st, tile, &graphics);
	SavedRandomSeeds seeds;
	SaveRandomSeeds(&seeds);
	auto restore_rng = scope_guard([&] { RestoreRandomSeeds(seeds); });
	InitializeBuiltAirportTiles(std::span(&tile, 1));
	CHECK(GetAnimationFrame(tile) == 3); // New construction runs the Built callback.

	auto &appearance = st->airport.tile_graphics.at(tile);
	GameRandomSeedChecker random;
	SetAnimationFrame(tile, 19); // Action 2 can legally return frames beyond the nominal limit.
	_animated_tiles.erase(tile); // A looping spec can be stopped by a callback.
	ReconcileAirportTileGraphicsAnimation(tile, appearance);
	CHECK(GetAnimationFrame(tile) == 19);
	CHECK(_animated_tiles.find(tile) == _animated_tiles.end());
	_animated_tiles[tile] = {7, false};
	_animated_tiles[other] = {5, false};
	SetAnimationFrame(other, 6);
	ReconcileAirportTileGraphicsAnimation(tile, appearance);
	CHECK(_animated_tiles.find(tile)->second.speed == 7);

	const auto mapping = _airporttile_mngr.mappings[slot];
	_airporttile_mngr.mappings[slot] = {};
	ReconcileAirportTileGraphicsAnimation(tile, appearance);
	CHECK(appearance.uses_fallback);
	CHECK(appearance.graphics == graphics);
	CHECK(GetAnimationFrame(tile) == 0);
	CHECK(AirportTileSpec::GetByTile(tile) == AirportTileSpec::GetByTile(other));
	REQUIRE(_animated_tiles.find(tile) != _animated_tiles.end());
	CHECK(_animated_tiles.find(tile)->second.speed == AirportTileSpec::GetByTile(other)->animation.speed);

	/* Simulate resaving and loading the persisted fallback bit and its ANIT state. */
	const auto saved_appearance = appearance;
	st->airport.tile_graphics.erase(tile);
	auto &loaded = st->airport.tile_graphics.emplace(tile, saved_appearance).first->second;
	SetAnimationFrame(tile, 8);
	_animated_tiles.erase(tile);
	ReconcileAirportTileGraphicsAnimation(tile, loaded);
	CHECK(GetAnimationFrame(tile) == 8);
	CHECK(_animated_tiles.find(tile) == _animated_tiles.end());

	_airporttile_mngr.mappings[slot] = mapping;
	ReconcileAirportTileGraphicsAnimation(tile, loaded);
	CHECK_FALSE(loaded.uses_fallback);
	CHECK(GetAnimationFrame(tile) == 0); // Recovery does not run Built again.
	REQUIRE(_animated_tiles.find(tile) != _animated_tiles.end());
	CHECK(_animated_tiles.find(tile)->second.speed == 4);
	CHECK(GetAnimationFrame(other) == 6);
	CHECK(_animated_tiles.find(other)->second.speed == 5);
	CHECK(random.Check());
}

TEST_CASE("Airport - blueprint spec identity follows remapped slots and rejects unknown airports")
{
	AllocateMap(64, 64);
	AirportSpec::ResetAirports();
	auto cleanup = scope_guard([] { AirportSpec::ResetAirports(); DeallocateMap(); });
	AutoRestoreBackup mappings(_airport_mngr.mappings, AutoRestoreBackupNoNewValueTag{});
	_airport_mngr.ResetMapping();
	constexpr uint8_t original_slot = NEW_AIRPORT_OFFSET;
	constexpr uint8_t remapped_slot = original_slot + 1;
	GRFFile grf;
	grf.grfid = TEST_AIRPORT_GRF;
	AirportSpec &spec = *AirportSpec::GetWithoutOverride(remapped_slot);
	spec = *AirportSpec::GetWithoutOverride(AT_SMALL);
	spec.grf_prop.SetGRFFile(&grf);
	spec.grf_prop.local_id = 42;
	_airport_mngr.mappings[remapped_slot] = {TEST_AIRPORT_GRF, 42, AT_SMALL};
	_airport_mngr.mappings[original_slot] = {OTHER_AIRPORT_GRF, 42, AT_SMALL};

	Blueprint bp;
	bp.width = bp.height = 4;
	bp.airports.push_back({{0, 0}, original_slot, 0, 0, AIRTYPE_GRAVEL, 4, 3, StationID::Invalid(), {}, {TEST_AIRPORT_GRF, 42}});
	std::vector<uint8_t> buffer;
	SerialiseBlueprint(BufferSerialisationRef(buffer), bp);
	DeserialisationBuffer reader(buffer.data(), buffer.size());
	Blueprint decoded;
	REQUIRE(DeserialiseBlueprint(reader, decoded, {}));
	REQUIRE(decoded.airports.size() == 1);
	CHECK(decoded.airports[0].spec == bp.airports[0].spec);
	CHECK(ResolveBlueprintAirportType(decoded.airports[0]) == remapped_slot);

	/* A v7 blueprint stored just the runtime slot, without the identity. */
	std::vector<uint8_t> legacy_buffer;
	BufferSerialisationRef legacy_writer(legacy_buffer);
	legacy_writer.Send_generic(uint16_t{UINT16_MAX - 3});
	legacy_writer.Send_generic_seq(bp.width, bp.height, bp.name);
	for (uint i = 0; i < 13; ++i) legacy_writer.Send_generic_integer(0U); // Vectors preceding airports.
	legacy_writer.Send_generic_integer(1U);
	const auto &item = bp.airports[0];
	SerialiseBlueprintOffset(legacy_writer, item.offset);
	legacy_writer.Send_generic_seq(item.type, item.layout, item.rotation, item.air_type, item.w, item.h, item.station);
	legacy_writer.Send_generic_integer(0U); // Airport tiles.
	legacy_writer.Send_generic_integer(0U); // Corner heights.
	legacy_writer.Send_generic_integer(0U); // Objects.
	DeserialisationBuffer legacy_reader(legacy_buffer.data(), legacy_buffer.size());
	Blueprint old;
	REQUIRE(DeserialiseBlueprint(legacy_reader, old, {}));
	REQUIRE(old.airports.size() == 1);
	CHECK_FALSE(ResolveBlueprintAirportType(old.airports[0]).has_value());
	old.airports[0].type = AT_SMALL;
	CHECK(ResolveBlueprintAirportType(old.airports[0]) == AT_SMALL);

	/* A stale runtime slot points to a different, buildable stock substitute. A
	 * missing exact GRF must fail preflight before the requested terraforming. */
	_airport_mngr.mappings[remapped_slot] = {};
	BlueprintPasteData paste;
	paste.blueprint = std::move(decoded);
	paste.blueprint.corner_heights.assign(25, 2);
	paste.terraform_mode = BlueprintTerraformMode::Full;
	TileIndex origin = TileXY(10, 10);
	const auto tile_type = GetTileType(origin);
	const auto tile_height = TileHeight(origin);
	GameRandomSeedChecker random;
	for (DoCommandFlags flags : {DoCommandFlags{}, DoCommandFlags{DoCommandFlag::Execute}}) {
		const auto result = CmdPasteBlueprint(flags, origin, paste);
		REQUIRE(result.Failed());
		CHECK(result.GetErrorMessage() == STR_BLUEPRINT_ERROR_MISSING_AIRPORT);
	}
	CHECK(GetTileType(origin) == tile_type);
	CHECK(TileHeight(origin) == tile_height);
	CHECK(random.Check());
}

TEST_CASE("Airport - script noise and nearest town are safe on maps without towns")
{
	AllocateMap(64, 64);
	AirportSpec::ResetAirports();
	auto cleanup = scope_guard([] { DeallocateMap(); });
	REQUIRE(Town::GetNumItems() == 0);
	AutoRestoreBackup noise(_settings_game.economy.station_noise_level, true);
	const TileIndex tile = TileXY(10, 10);
	CHECK(ScriptAirport::GetNoiseLevelIncrease(tile, ScriptAirport::AT_SMALL) == 0);
	CHECK(ScriptAirport::GetNearestTown(tile, ScriptAirport::AT_SMALL) == TownID::Invalid());
	uint distance = 0;
	CHECK(AirportGetNearestTown(BitmapTileArea{}, distance) == nullptr);
	CHECK(distance == UINT_MAX - 1);
}
