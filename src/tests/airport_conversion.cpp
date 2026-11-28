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
#include "../clear_map.h"
#include "../company_base.h"
#include "../company_func.h"
#include "../core/backup_type.hpp"
#include "../core/random_func.hpp"
#include "../depot_base.h"
#include "../map_func.h"
#include "../network/network.h"
#include "../newgrf.h"
#include "../newgrf_airport.h"
#include "../newgrf_airporttiles.h"
#include "../scope.h"
#include "../settings_type.h"
#include "../station_base.h"
#include "../station_func.h"
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

TEST_CASE("Airport - spec construction validates complete directed runways")
{
	AllocateMap(64, 64);
	AirportSpec::ResetAirports();
	auto cleanup = scope_guard([] { DeallocateMap(); });
	const TileIndex tile = TileXY(10, 10);
	const auto check_layout = [&](const AirportTileLayout &layout, DiagDirection rotation) {
		const bool swap_axes = (to_underlying(rotation) % 2) != 0;
		BitmapTileArea occupied;
		occupied.Initialize(TileArea(tile, swap_axes ? layout.size_y : layout.size_x, swap_axes ? layout.size_x : layout.size_y));
		return AddAirportTileTableToBitmapTileArea(layout, &occupied, rotation, 1);
	};
	for (uint8_t type = AT_SMALL; type < NEW_AIRPORT_OFFSET; type++) {
		for (const auto &layout : AirportSpec::Get(type)->layouts) {
			for (DiagDirection rotation = DiagDirection::Begin; rotation < DiagDirection::End; ++rotation) {
				CAPTURE(type, to_underlying(rotation));
				CHECK(check_layout(layout, rotation).Succeeded());
			}
		}
	}

	AirportTileLayout broken = AirportSpec::Get(AT_SMALL)->layouts[0];
	auto middle = std::ranges::find(broken.tiles, ATT_RUNWAY_MIDDLE, &AirportTileTable::type);
	REQUIRE(middle != broken.tiles.end());
	SECTION("gap") { middle->type = ATT_INVALID; }
	SECTION("wrong direction") { middle->runway_directions = ReverseDir(middle->runway_directions); }
	SECTION("unmatched crossing branch") { middle->runway_directions = Direction::N; }
	CHECK(check_layout(broken, DiagDirection::NE).Failed());
}

TEST_CASE("Airport - an incomplete runway has no length and its remnant can be cleared")
{
	AllocateMap(64, 64);
	AirportSpec::ResetAirports();
	ResetAirTypes();
	auto cleanup = scope_guard([] { _station_pool.CleanPool(); _depot_pool.CleanPool(); DeallocateMap(); });
	AutoRestoreBackup current(_current_company, OWNER_BEGIN);
	AutoRestoreBackup headless(_network_dedicated, true);
	AutoRestoreBackup enabled(_settings_game.station.allow_modify_airports, true);
	Station *st = CreateSpecAirport(TileXY(10, 10), AT_SMALL, 0, DiagDirection::NE, INVALID_AIRTYPE);
	st->owner = OWNER_BEGIN;
	REQUIRE_FALSE(st->airport.runways.empty());
	const TileIndex start = st->airport.runways.front();
	const DiagDirection dir = GetRunwayExtremeDirection(start);
	const TileIndex middle = TileAddByDiagDir(start, dir);
	const TileIndex end = GetRunwayExtreme(start, dir);
	REQUIRE(IsValidTile(end));
	REQUIRE(IsPlainRunway(middle));
	CHECK(GetRunwayLength(start) > 2);
	CHECK(GetRunwayExtreme(end, ReverseDiagDir(dir)) == start);
	const auto clear_start = [&](DoCommandFlags flags) {
		return CmdAddRemoveAirportTiles(flags, start, start, false, GetAirType(start), StationID::Invalid(), false);
	};
	CHECK(clear_start({}).Failed());

	SetAirportTileType(middle, ATT_SIMPLE_TRACK);
	CHECK(GetRunwayExtreme(start, dir) == INVALID_TILE);
	CHECK(GetRunwayExtreme(end, ReverseDiagDir(dir)) == INVALID_TILE);
	CHECK(GetRunwayLength(start) == 0);
	SetRunwayReservation(start, true);
	CHECK_FALSE(GetReservationAsRunway(start));
	CHECK(clear_start({}).Succeeded());
	CHECK(IsAirportTile(start));
	CHECK(clear_start(DoCommandFlag::Execute).Succeeded());
	CHECK_FALSE(IsAirportTile(start));
	CHECK(st->TileBelongsToAirport(middle));
	CHECK(st->TileBelongsToAirport(end));
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

extern AirTypeInfo _airtypes[AIRTYPE_END];

/** Flat test map with deterministic air type noise and two town centres. */
class AirportNoiseFixture {
protected:
	AutoRestoreBackup<bool> headless{_network_dedicated, true};
	AutoRestoreBackup<CompanyID> current{_current_company, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<CompanyID> local{_local_company, CompanyID::Invalid()};
	AutoRestoreBackup<uint8_t> tolerance{_settings_game.difficulty.town_council_tolerance, TOWN_COUNCIL_LENIENT};
	AutoRestoreBackup<uint8_t> divisor{_settings_game.economy.custom_airport_noise_divisor, 1};
	AutoRestoreBackup<uint8_t> spread{_settings_game.station.station_spread, 32};
	AutoRestoreBackup<bool> custom{_settings_game.station.allow_modify_airports, true};
	AutoRestoreBackup<bool> distant_join{_settings_game.station.distant_join_stations, true};
	AutoRestoreBackup<bool> enforce{_settings_game.economy.station_noise_level, false};
	AutoRestoreBackup<bool> no_expiry{_settings_game.station.never_expire_airports, true};
	AutoRestoreBackup<AirTypeInfo> asphalt{_airtypes[AIRTYPE_ASPHALT], AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<AirTypeInfo> gravel{_airtypes[AIRTYPE_GRAVEL], AutoRestoreBackupNoNewValueTag{}};
	Town *west = nullptr;
	Town *east = nullptr;
	SavedRandomSeeds saved_seeds;

	AirportNoiseFixture()
	{
		SaveRandomSeeds(&saved_seeds);
		AllocateMap(64, 64);
		AirportSpec::ResetAirports();
		for (uint y = 1; y < 63; y++) {
			for (uint x = 1; x < 63; x++) MakeClear(TileXY(x, y), ClearGround::Grass, 0);
		}
		Company *company = Company::Create();
		_current_company = company->index;
		company->money = 1000000;
		company->clear_limit = 64U << 16; // Raw Company::Create leaves the landscaping allowance at zero.
		west = Town::Create(TileXY(5, 10));
		east = Town::Create(TileXY(40, 10));
		RebuildTownKdtree();
		_airtypes[AIRTYPE_ASPHALT].base_noise_level = 8;
		_airtypes[AIRTYPE_GRAVEL].base_noise_level = 3;
		for (AirType type : {AIRTYPE_ASPHALT, AIRTYPE_GRAVEL}) {
			_airtypes[type].runway_noise_level = 0;
			_airtypes[type].build_on_water = false;
			_airtypes[type].heliport_availability = true;
		}
	}

	~AirportNoiseFixture()
	{
		_station_pool.CleanPool();
		_depot_pool.CleanPool();
		_town_pool.CleanPool();
		_company_pool.CleanPool();
		RebuildStationKdtree();
		RebuildTownKdtree();
		DeallocateMap();
		RestoreRandomSeeds(saved_seeds);
	}

	void CheckNoise(uint expected_west, uint expected_east)
	{
		CHECK(west->noise_reached == expected_west);
		CHECK(east->noise_reached == expected_east);
		/* Simulate rebuilding airport caches and recomputing town noise on load. */
		for (Station *st : Station::Iterate()) st->UpdateAirportDataStructure();
		UpdateAirportsNoise();
		CHECK(west->noise_reached == expected_west);
		CHECK(east->noise_reached == expected_east);
	}
};

TEST_CASE_METHOD(AirportNoiseFixture, "Airport noise - standalone spec heliport is silent in both build modes")
{
	const TileIndex pavement = TileXY(7, 10);
	REQUIRE(CmdAddRemoveAirportTiles(DoCommandFlag::Execute, pavement, pavement, true, AIRTYPE_ASPHALT, StationID::Invalid(), false).Succeeded());
	CheckNoise(8, 0);
	_settings_game.economy.station_noise_level = true;
	REQUIRE(west->MaxTownNoise() == 0);
	for (bool modifiable : {false, true}) {
		_settings_game.station.allow_modify_airports = modifiable;
		const TileIndex tile = TileXY(10, modifiable ? 14 : 10);
		AirportSpec *spec = AirportSpec::GetWithoutOverride(AT_HELIPORT);
		AutoRestoreBackup year(spec->min_year, CalTime::MIN_YEAR);
		const auto test = CmdBuildAirport({}, tile, AT_HELIPORT, 0, AIRTYPE_ASPHALT, DiagDirection::NE, StationID::Invalid(), true);
		REQUIRE(test.Succeeded());
		CHECK(west->noise_reached == 8);
		const auto execute = CmdBuildAirport(DoCommandFlag::Execute, tile, AT_HELIPORT, 0, AIRTYPE_ASPHALT, DiagDirection::NE, StationID::Invalid(), true);
		REQUIRE(execute.Succeeded());
		CHECK(test.GetCost() == execute.GetCost());
		CHECK(Station::GetByTile(tile)->airport.type == (modifiable ? AT_CUSTOM : AT_HELIPORT));
		CheckNoise(8, 0);
	}
}

TEST_CASE_METHOD(AirportNoiseFixture, "Airport noise - conversion and footprint edits replace exact town contributions")
{
	const TileIndex first = TileXY(19, 10);
	const TileIndex extension = TileXY(39, 10);
	REQUIRE(CmdAddRemoveAirportTiles(DoCommandFlag::Execute, first, first, true, AIRTYPE_ASPHALT, StationID::Invalid(), false).Succeeded());
	CheckNoise(7, 0); // 8 minus one distance step.

	_settings_game.economy.station_noise_level = true;
	const auto change = [&](DoCommandFlags flags, AirType type) { return CmdChangeAirType(flags, first, type); };
	const auto quieter = change({}, AIRTYPE_GRAVEL);
	REQUIRE(quieter.Succeeded()); // The town is still over the limit, but the change is quieter.
	CHECK(west->noise_reached == 7);
	const auto changed = change(DoCommandFlag::Execute, AIRTYPE_GRAVEL);
	REQUIRE(changed.Succeeded());
	CHECK(changed.GetCost() == quieter.GetCost());
	CheckNoise(2, 0);
	for (DoCommandFlags flags : {DoCommandFlags{}, DoCommandFlags{DoCommandFlag::Execute}}) {
		const auto louder = change(flags, AIRTYPE_ASPHALT);
		REQUIRE(louder.Failed());
		CHECK(louder.GetErrorMessage() == STR_ERROR_TOWN_AIRPORT_NOISE);
	}
	CheckNoise(2, 0);

	const StationID station = GetStationIndex(first);
	const auto extend = [&](DoCommandFlags flags) {
		return CmdAddRemoveAirportTiles(flags, extension, extension, true, AIRTYPE_GRAVEL, station, false);
	};
	REQUIRE(extend({}).Failed()); // The second town has no noise allowance.
	CHECK(west->noise_reached == 2);
	_settings_game.economy.station_noise_level = false;
	const auto estimate = extend({});
	REQUIRE(estimate.Succeeded());
	const auto added = extend(DoCommandFlag::Execute);
	REQUIRE(added.Succeeded());
	CHECK(estimate.GetCost() == added.GetCost());
	REQUIRE(GetStationIndex(extension) == station);
	CheckNoise(0, 3);
	REQUIRE(CmdAddRemoveAirportTiles({}, extension, extension, false, AIRTYPE_GRAVEL, StationID::Invalid(), false).Succeeded());
	CHECK(east->noise_reached == 3);
	REQUIRE(CmdAddRemoveAirportTiles(DoCommandFlag::Execute, extension, extension, false, AIRTYPE_GRAVEL, StationID::Invalid(), false).Succeeded());
	REQUIRE(IsAirportTileOfStation(first, station));
	CheckNoise(2, 0);
}

TEST_CASE_METHOD(AirportNoiseFixture, "Airport noise - custom blueprint tests final heliport noise and accounts for partial placement")
{
	BlueprintPasteData data;
	data.blueprint.width = data.blueprint.height = 1;
	BlueprintAirport airport{{0, 0}, AT_CUSTOM, 0, 0, AIRTYPE_ASPHALT, 1, 1, StationID{0}, {}};
	airport.tiles.push_back({{0, 0}, ATTG_DEFAULT_GFX, uint8_t(ATT_APRON_HELIPORT << 4), 0x80, TRACK_BIT_CROSS});
	data.blueprint.airports.push_back(airport);
	_settings_game.economy.station_noise_level = true;
	const TileIndex heliport = TileXY(10, 10);
	const auto test = CmdPasteBlueprint({}, heliport, data);
	REQUIRE(test.Succeeded());
	CHECK_FALSE(IsAirportTile(heliport));
	const auto pasted = CmdPasteBlueprint(DoCommandFlag::Execute, heliport, data);
	REQUIRE(pasted.Succeeded());
	CHECK(pasted.GetCost() == test.GetCost());
	REQUIRE(IsHeliportTile(heliport));
	CheckNoise(0, 0);

	/* A second target outside the map leaves the accepted tile as bare pavement. */
	_settings_game.economy.station_noise_level = false;
	data.blueprint.width = 5;
	data.blueprint.airports.front().w = 5;
	data.blueprint.airports.front().tiles.push_back({{4, 0}, ATTG_DEFAULT_GFX, uint8_t(ATT_SIMPLE_TRACK << 4), 0x80, 0});
	const TileIndex partial = TileXY(61, 10);
	const auto partial_test = CmdPasteBlueprint({}, partial, data);
	REQUIRE(partial_test.Succeeded());
	const auto partial_execute = CmdPasteBlueprint(DoCommandFlag::Execute, partial, data);
	REQUIRE(partial_execute.Succeeded());
	CHECK(partial_execute.GetCost() == partial_test.GetCost());
	REQUIRE(IsAirportTile(partial));
	CHECK(GetAirportTileType(partial) == ATT_SIMPLE_TRACK);
	CheckNoise(0, 6);
}
