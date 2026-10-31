/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint.cpp Tests for copying infrastructure into a blueprint. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../air_map.h"
#include "../airport_cmd.h"
#include "../settings_type.h"
#include "../newgrf_airport.h"
#include "../air.h"
#include "../depot_base.h"
#include "../aircraft.h"
#include "../engine_base.h"
#include "../order_func.h"
#include "../core/random_func.hpp"
#include "../blueprint.h"
#include "../blueprint_cmd.h"
#include "../company_func.h"
#include "../company_base.h"
#include "../tracerestrict.h"
#include "../map_func.h"
#include "../rail_map.h"
#include "../station_base.h"
#include "../station_map.h"
#include "../newgrf.h"
#include "../newgrf_newsignals.h"
#include "../newgrf_object.h"
#include "../object_base.h"
#include "../object_map.h"
#include "../core/backup_type.hpp"
#include "../scope.h"
#include "../table/strings.h"

#include "../safeguards.h"

extern std::vector<ObjectSpec> _object_specs;
extern AirTypeInfo _airtypes[AIRTYPE_END];
extern void UpdateAircraftOnUpdatedAirport(const Station *st);

/** GRFIDs used by the NewGRF style/object tests below. */
static constexpr GrfID TEST_GRFID("\x12\x34\x56\x78");
static constexpr GrfID TEST_GRFID_ALT("\x87\x65\x43\x21");
static constexpr GrfID TEST_GRFID_DEAD("\xDE\xAD\xBE\xEF");
static constexpr GrfID TEST_GRFID_1("\x11\x11\x22\x22");
static constexpr GrfID TEST_GRFID_2("\x22\x22\x33\x33");

TEST_CASE("Blueprint - signal identity follows remapped and enabled style slots")
{
	AutoRestoreBackup mappings(_new_signal_style_mapping, AutoRestoreBackupNoNewValueTag{});
	AutoRestoreBackup styles(_new_signal_styles, AutoRestoreBackupNoNewValueTag{});
	AutoRestoreBackup<uint8_t> count(_num_new_signal_styles, 4);
	AutoRestoreBackup<uint16_t> enabled(_enabled_new_signal_styles_mask, 1 << 4);
	GRFFile grf;
	_new_signal_style_mapping.fill({});
	_new_signal_styles.fill({});
	_new_signal_style_mapping[3] = {TEST_GRFID, 9};
	_new_signal_styles[3].grffile = &grf;
	CHECK(ResolveBlueprintSignalStyle({TEST_GRFID, 9}) == 4);
	CHECK(ResolveBlueprintSignalStyle({}) == 0);
	CHECK_FALSE(ResolveBlueprintSignalStyle({TEST_GRFID, 10}).has_value());
	std::swap(_new_signal_style_mapping[3], _new_signal_style_mapping[1]);
	std::swap(_new_signal_styles[3], _new_signal_styles[1]);
	_enabled_new_signal_styles_mask = 1 << 2;
	CHECK(ResolveBlueprintSignalStyle({TEST_GRFID, 9}) == 2);
	AllocateMap(64, 64);
	auto cleanup = scope_guard([] { DeallocateMap(); });
	AutoRestoreBackup local(_local_company, OWNER_BEGIN);
	TileIndex tile = TileXY(10, 10);
	MakeRailNormal(tile, OWNER_BEGIN, TRACK_BIT_X, RAILTYPE_RAIL);
	SB(_m[tile].m5, 6, 2, to_underlying(RailTileType::Signals));
	SetPresentSignals(tile, SignalAlongTrackdir(TrackToTrackdir(TRACK_X)));
	SetSignalStyle(tile, TRACK_X, 2);
	Blueprint bp;
	CopyAreaToBlueprint(TileArea(tile, 1, 1), bp);
	REQUIRE(bp.signals.size() == 1);
	CHECK(bp.signals[0].spec == BlueprintSpecRef{TEST_GRFID, 9});
	_enabled_new_signal_styles_mask = 0;
	CHECK_FALSE(ResolveBlueprintSignalStyle({TEST_GRFID, 9}).has_value());
}

TEST_CASE("Blueprint - complete multitile objects copy once and incomplete selections skip them")
{
	AllocateMap(64, 64);
	REQUIRE(Object::GetNumItems() == 0);
	AutoRestoreBackup specs(_object_specs, std::vector<ObjectSpec>(6));
	AutoRestoreBackup local(_local_company, OWNER_BEGIN);
	AutoRestoreBackup current(_current_company, OWNER_BEGIN);
	auto cleanup = scope_guard([] { ClearBlueprintUndoStack(); _object_pool.CleanPool(); DeallocateMap(); });
	ObjectSpec &spec = _object_specs[5];
	spec.grf_prop.grfid = TEST_GRFID;
	spec.grf_prop.local_id = 333;
	spec.views = 4;
	spec.size = 0x32;
	Object *object = Object::Create(5, nullptr, TileArea(TileXY(10, 10), 2, 3), CalTime::Date{0}, 2);
	for (TileIndex tile : object->location) MakeObject(tile, OWNER_BEGIN, object->index, WaterClass::Invalid, 0);
	Blueprint bp;
	CHECK(CopyAreaToBlueprint(TileArea(TileXY(9, 9), 4, 5), bp) == 1);
	REQUIRE(bp.objects.size() == 1);
	CHECK(bp.width == 2);
	CHECK(bp.height == 3);
	CHECK(bp.objects[0].offset.x == 0);
	CHECK(bp.objects[0].offset.y == 0);
	CHECK(bp.objects[0].spec == BlueprintSpecRef{TEST_GRFID, 333});
	CHECK(bp.objects[0].view == 2);
	CHECK(CopyAreaToBlueprint(TileArea(TileXY(10, 10), 1, 3), bp) == 0);
	CHECK(CopyAreaToBlueprint(TileArea(TileXY(11, 10), 1, 3), bp) == 0);
	BeginBlueprintUndoGroup();
	RecordBlueprintUndoObject(object->location.tile);
	EndBlueprintUndoGroup(true);
	const BlueprintUndoRecord *record = GetBlueprintUndoRecord();
	REQUIRE(record != nullptr);
	CHECK(record->built.width == 2);
	CHECK(record->built.height == 3);
	std::vector<TileIndex> tiles;
	CollectBlueprintUndoTiles(*record, {}, tiles);
	CHECK(tiles.size() == 6);
	BlueprintUndoFilters filter;
	filter.objects = false;
	Blueprint filtered = record->built;
	FilterBlueprintUndoData(filtered, filter);
	CHECK(filtered.IsEmpty());
}

TEST_CASE("Blueprint - objects and signal identities survive all eight transformations")
{
	Blueprint bp;
	bp.width = 5;
	bp.height = 4;
	bp.objects.push_back({{1, 1}, {TEST_GRFID, 12}, 0, 4, 2, 3});
	bp.signals.push_back({{0, 0}, TRACK_X, SignalType::Path, SignalVariant::Electric, true, false, {TEST_GRFID_ALT, 8}});
	for (uint8_t rotation = 0; rotation < 4; ++rotation) for (bool reflected : {false, true}) {
		Blueprint transformed = TransformBlueprint(bp, rotation, reflected);
		REQUIRE(transformed.objects.size() == 1);
		const auto &object = transformed.objects[0];
		CHECK(object.spec == bp.objects[0].spec);
		CHECK(object.w == (rotation % 2 ? 3 : 2));
		CHECK(object.h == (rotation % 2 ? 2 : 3));
		CHECK(object.view % 2 == rotation % 2);
		CHECK(object.offset.x >= 0);
		CHECK(object.offset.y >= 0);
		CHECK(object.offset.x + object.w <= transformed.width);
		CHECK(object.offset.y + object.h <= transformed.height);
		CHECK(transformed.signals[0].spec == bp.signals[0].spec);
		Blueprint restored = TransformBlueprint(transformed, reflected ? rotation : (4 - rotation) % 4, reflected);
		CHECK(restored.objects[0].view == 0);
		CHECK(restored.objects[0].offset.x == 1);
		CHECK(restored.objects[0].offset.y == 1);
	}
}

TEST_CASE("Blueprint - missing objects and signal styles fail before terrain changes")
{
	AllocateMap(64, 64);
	auto cleanup = scope_guard([] { DeallocateMap(); });
	BlueprintPasteData data;
	data.blueprint.width = data.blueprint.height = 1;
	data.terraform_mode = BlueprintTerraformMode::Full;
	data.blueprint.corner_heights.assign(4, 0);
	data.height_offset = 3;
	StringID error;
	SECTION("object") {
		data.blueprint.objects.push_back({{0, 0}, {TEST_GRFID_DEAD, 12}, 0, 1, 1, 1});
		error = STR_BLUEPRINT_ERROR_MISSING_OBJECT;
	}
	SECTION("signal") {
		data.blueprint.signals.push_back({{0, 0}, TRACK_X, SignalType::Path, SignalVariant::Electric, true, false, {TEST_GRFID_DEAD, 12}});
		error = STR_BLUEPRINT_ERROR_MISSING_SIGNAL_STYLE;
	}
	TileIndex tile = TileXY(10, 10);
	uint height = TileHeight(tile);
	CommandCost result = CmdPasteBlueprint(DoCommandFlag::Execute, tile, data);
	CHECK(result.Failed());
	CHECK(result.GetErrorMessage() == error);
	CHECK(TileHeight(tile) == height);
}

TEST_CASE("Blueprint - version four payload has no signal styles or objects")
{
	std::vector<uint8_t> bytes;
	BufferSerialisationRef writer(bytes);
	writer.Send_generic_seq(uint16_t{UINT16_MAX}, uint16_t{1}, uint16_t{1}, std::string{"Version four"});
	writer.Send_generic_integer(uint32_t{0}); // tracks
	writer.Send_generic_integer(uint32_t{1}); // old signal, without identity
	SerialiseBlueprintOffset(writer, {0, 0});
	writer.Send_generic_seq(TRACK_X, SignalType::Path, SignalVariant::Electric, true, false);
	for (int i = 0; i < 13; ++i) writer.Send_generic_integer(uint32_t{0});
	DeserialisationBuffer reader(bytes.data(), bytes.size());
	Blueprint bp;
	REQUIRE(DeserialiseBlueprint(reader, bp, {}));
	REQUIRE(bp.signals.size() == 1);
	CHECK(bp.signals[0].spec.grfid == GrfID{});
	CHECK(bp.signals[0].along);
	CHECK(bp.objects.empty());
}

TEST_CASE("Blueprint - object footprints and view indices are validated before building")
{
	AllocateMap(64, 64);
	auto cleanup = scope_guard([] { DeallocateMap(); });
	BlueprintPasteData data;
	data.blueprint.width = data.blueprint.height = 2;
	data.blueprint.objects.push_back({{0, 0}, {TEST_GRFID, 1}, 0, 4, 1, 1});
	SECTION("outside bounds") { data.blueprint.objects[0].w = 3; }
	SECTION("zero width") { data.blueprint.objects[0].w = 0; }
	SECTION("invalid view") { data.blueprint.objects[0].view = 255; }
	SECTION("invalid view count") { data.blueprint.objects[0].views = 3; }
	CHECK(CmdPasteBlueprint(DoCommandFlag::Execute, TileXY(10, 10), data).Failed());
	Blueprint transformed = TransformBlueprint(data.blueprint, 1, true);
	CHECK(transformed.objects.size() == 1); // Malformed imported views must not index a direction table.
}

TEST_CASE("Blueprint - copy respects area and ownership")
{
	AllocateMap(64, 64);
	_local_company = OWNER_BEGIN;

	/* An own track, a foreign track, and an own track outside the copied area. */
	MakeRailNormal(TileXY(10, 10), OWNER_BEGIN, TRACK_BIT_X, RAILTYPE_RAIL);
	MakeRailNormal(TileXY(11, 10), OWNER_NONE, TRACK_BIT_Y, RAILTYPE_RAIL);
	MakeRailNormal(TileXY(20, 20), OWNER_BEGIN, TRACK_BIT_X, RAILTYPE_RAIL);

	Blueprint blueprint;
	uint count = CopyAreaToBlueprint(TileArea(TileXY(9, 9), TileXY(12, 12)), blueprint);

	CHECK(count == 1);
	CHECK(!blueprint.IsEmpty());

	/* The blueprint is shrunk to the single copied track tile. */
	CHECK(blueprint.width == 1);
	CHECK(blueprint.height == 1);

	REQUIRE(blueprint.rail_tracks.size() == 1);
	CHECK(blueprint.rail_tracks[0].offset.x == 0);
	CHECK(blueprint.rail_tracks[0].offset.y == 0);
	CHECK(blueprint.rail_tracks[0].track == TRACK_X);
	CHECK(blueprint.rail_tracks[0].railtype == RAILTYPE_RAIL);

	CHECK(blueprint.corner_heights.size() == 2 * 2);
}

TEST_CASE("Blueprint - name persists across Clear() and re-copy")
{
	AllocateMap(64, 64);
	_local_company = OWNER_BEGIN;

	MakeRailNormal(TileXY(5, 5), OWNER_BEGIN, TRACK_BIT_X, RAILTYPE_RAIL);

	Blueprint blueprint;
	blueprint.name = "My Layout";

	/* Re-copying (which clears the blueprint first) must not lose the name. */
	CopyAreaToBlueprint(TileArea(TileXY(5, 5), TileXY(5, 5)), blueprint);
	CHECK(blueprint.name == "My Layout");
	REQUIRE(blueprint.rail_tracks.size() == 1);

	blueprint.Clear();
	CHECK(blueprint.name == "My Layout");
	CHECK(blueprint.IsEmpty());
}

TEST_CASE("Blueprint - copy shrinks to the smallest area containing all elements")
{
	AllocateMap(64, 64);
	_local_company = OWNER_BEGIN;

	/* Two tracks spanning a 2x3 area inside a much larger selection. */
	MakeRailNormal(TileXY(40, 40), OWNER_BEGIN, TRACK_BIT_X, RAILTYPE_RAIL);
	MakeRailNormal(TileXY(41, 42), OWNER_BEGIN, TRACK_BIT_Y, RAILTYPE_RAIL);

	Blueprint blueprint;
	uint count = CopyAreaToBlueprint(TileArea(TileXY(35, 36), TileXY(46, 47)), blueprint);

	CHECK(count == 2);
	CHECK(blueprint.width == 2);
	CHECK(blueprint.height == 3);

	REQUIRE(blueprint.rail_tracks.size() == 2);
	CHECK(blueprint.rail_tracks[0].offset.x == 0);
	CHECK(blueprint.rail_tracks[0].offset.y == 0);
	CHECK(blueprint.rail_tracks[1].offset.x == 1);
	CHECK(blueprint.rail_tracks[1].offset.y == 2);

	CHECK(blueprint.corner_heights.size() == 3 * 4);
}

TEST_CASE("Blueprint - copy of empty area yields empty blueprint")
{
	AllocateMap(64, 64);
	_local_company = OWNER_BEGIN;

	Blueprint blueprint;
	blueprint.rail_tracks.push_back({{0, 0}, TRACK_X, RAILTYPE_RAIL}); // Must be cleared by the copy.

	uint count = CopyAreaToBlueprint(TileArea(TileXY(30, 30), TileXY(33, 33)), blueprint);

	CHECK(count == 0);
	CHECK(blueprint.IsEmpty());
	CHECK(blueprint.rail_tracks.empty());
	CHECK(blueprint.width == 4);
}

TEST_CASE("Blueprint - track transformation")
{
	/* Four rotations or two reflections are the identity. */
	for (Track track = TRACK_BEGIN; track < TRACK_END; track++) {
		CHECK(TransformBlueprintTrack(track, 4, false) == track);
		CHECK(TransformBlueprintTrack(TransformBlueprintTrack(track, 0, true), 0, true) == track);
	}

	/* Rotating 90 degrees anticlockwise moves the north corner to the west. */
	CHECK(TransformBlueprintTrack(TRACK_X, 1, false) == TRACK_Y);
	CHECK(TransformBlueprintTrack(TRACK_Y, 1, false) == TRACK_X);
	CHECK(TransformBlueprintTrack(TRACK_UPPER, 1, false) == TRACK_LEFT);
	CHECK(TransformBlueprintTrack(TRACK_LEFT, 1, false) == TRACK_LOWER);

	/* Reflection against the NW-SE axis keeps X/Y and swaps upper/left, lower/right. */
	CHECK(TransformBlueprintTrack(TRACK_X, 0, true) == TRACK_X);
	CHECK(TransformBlueprintTrack(TRACK_UPPER, 0, true) == TRACK_LEFT);

	/* Reflect + rotate twice equals a reflection against the NE-SW axis. */
	CHECK(TransformBlueprintTrack(TRACK_UPPER, 2, true) == TRACK_RIGHT);
	CHECK(TransformBlueprintTrack(TRACK_X, 2, true) == TRACK_X);
}

TEST_CASE("Blueprint - point transformation")
{
	/* Identity. */
	CHECK(TransformBlueprintPoint({2, 1}, 4, 3, 0, false).x == 2);
	CHECK(TransformBlueprintPoint({2, 1}, 4, 3, 0, false).y == 1);

	/* One anticlockwise quarter turn in a 4x3 box: (x, y) -> (3 - 1 - y, x). */
	TileIndexDiffC p = TransformBlueprintPoint({2, 1}, 4, 3, 1, false);
	CHECK(p.x == 1);
	CHECK(p.y == 2);

	/* The north corner (0, 0) moves to the west corner. */
	p = TransformBlueprintPoint({0, 0}, 4, 3, 1, false);
	CHECK(p.x == 2);
	CHECK(p.y == 0);

	/* Four quarter turns are the identity. */
	p = TransformBlueprintPoint({3, 2}, 4, 3, 4, false);
	CHECK(p.x == 3);
	CHECK(p.y == 2);

	/* Reflection against the NW-SE axis mirrors the X coordinate. */
	p = TransformBlueprintPoint({0, 2}, 4, 3, 0, true);
	CHECK(p.x == 3);
	CHECK(p.y == 2);
}

TEST_CASE("Blueprint - direction, axis and road bit transformation")
{
	/* Rotation by 90 degrees anticlockwise: NE -> NW -> SW -> SE -> NE. */
	CHECK(TransformBlueprintDiagDir(DiagDirection::NE, 1, false) == DiagDirection::NW);
	CHECK(TransformBlueprintDiagDir(DiagDirection::NW, 1, false) == DiagDirection::SW);
	CHECK(TransformBlueprintDiagDir(DiagDirection::NE, 4, false) == DiagDirection::NE);

	/* Reflection against the NW-SE axis: NE and SW swap, NW and SE stay. */
	CHECK(TransformBlueprintDiagDir(DiagDirection::NE, 0, true) == DiagDirection::SW);
	CHECK(TransformBlueprintDiagDir(DiagDirection::SE, 0, true) == DiagDirection::SE);

	CHECK(TransformBlueprintAxis(Axis::X, 1) == Axis::Y);
	CHECK(TransformBlueprintAxis(Axis::X, 2) == Axis::X);

	CHECK(TransformBlueprintRoadBits(ROAD_NE, 1, false) == ROAD_NW);
	CHECK(TransformBlueprintRoadBits(ROAD_X, 1, false) == ROAD_Y);
	CHECK(TransformBlueprintRoadBits(ROAD_ALL, 1, true) == ROAD_ALL);
	CHECK(TransformBlueprintRoadBits(ROAD_NE | ROAD_SE, 0, true) == (ROAD_SW | ROAD_SE));
}

TEST_CASE("Blueprint - runway direction transformation")
{
	/* Rotation by 90 degrees anticlockwise: N -> W -> S -> E -> N. */
	CHECK(TransformBlueprintDirection(Direction::N, 1, false) == Direction::W);
	CHECK(TransformBlueprintDirection(Direction::E, 1, false) == Direction::N);
	CHECK(TransformBlueprintDirection(Direction::NE, 1, false) == Direction::NW);
	CHECK(TransformBlueprintDirection(Direction::N, 4, false) == Direction::N);

	/* Reflection against the NW-SE axis: NE and SW swap, as do N/W and E/S. */
	CHECK(TransformBlueprintDirection(Direction::NE, 0, true) == Direction::SW);
	CHECK(TransformBlueprintDirection(Direction::N, 0, true) == Direction::W);
	CHECK(TransformBlueprintDirection(Direction::SE, 0, true) == Direction::SE);
	CHECK(TransformBlueprintDirection(Direction::NW, 0, true) == Direction::NW);
}

TEST_CASE("Blueprint - custom airport tiles rotate their tile data")
{
	/* Hangar on the north edge opening to the south-west, with an X track. */
	BlueprintAirportTile hangar{{0, 0}, 0, static_cast<uint8_t>(ATT_HANGAR_STANDARD << 4), 0,
			static_cast<uint16_t>(TRACK_BIT_X | (to_underlying(DiagDirection::SW) << 14))};
	TransformBlueprintAirportTile(hangar, 1, false);
	CHECK(hangar.m5 == static_cast<uint8_t>(ATT_HANGAR_STANDARD << 4));
	CHECK(GB(hangar.m8, 0, 6) == TRACK_BIT_Y);
	CHECK(GB(hangar.m8, 14, 2) == to_underlying(DiagDirection::SE));

	/* Runway middle: the packed direction N becomes W. */
	BlueprintAirportTile runway{{1, 0}, 0, static_cast<uint8_t>(ATT_RUNWAY_MIDDLE << 4), 0,
			static_cast<uint16_t>(to_underlying(Direction::N) << 12)};
	TransformBlueprintAirportTile(runway, 1, false);
	CHECK(GB(runway.m8, 12, 3) == to_underlying(Direction::W));

	/* Runway start: its direction rotates by one quarter turn as well. */
	BlueprintAirportTile runway_start{{2, 0}, 0, static_cast<uint8_t>(ATT_RUNWAY_START_ALLOW_LANDING << 4), 0,
			static_cast<uint16_t>(to_underlying(DiagDirection::NE) << 12)};
	TransformBlueprintAirportTile(runway_start, 1, false);
	CHECK(GB(runway_start.m8, 12, 2) == to_underlying(DiagDirection::NW));

	/* Four quarter turns restore the captured tile exactly, reservation bits included. */
	BlueprintAirportTile original{{3, 0}, 0x11, static_cast<uint8_t>((ATT_APRON_NORMAL << 4) | 0x3), 0x82,
			static_cast<uint16_t>(TRACK_BIT_UPPER | TRACK_BIT_LEFT | (1 << 6) | (to_underlying(DiagDirection::NW) << 14))};
	BlueprintAirportTile rotated = original;
	for (int i = 0; i < 4; i++) TransformBlueprintAirportTile(rotated, 1, false);
	CHECK(rotated.offset.x == original.offset.x);
	CHECK(rotated.offset.y == original.offset.y);
	CHECK(rotated.m4 == original.m4);
	CHECK(rotated.m5 == original.m5);
	CHECK(rotated.m6 == original.m6);
	CHECK(rotated.m8 == original.m8);
}

TEST_CASE("Blueprint - transforming a blueprint moves elements consistently")
{
	/* A 3x2 blueprint with a single X track in its north corner. */
	Blueprint blueprint;
	blueprint.width = 3;
	blueprint.height = 2;
	blueprint.rail_tracks.push_back({{0, 0}, TRACK_X, RAILTYPE_RAIL});
	blueprint.corner_heights.assign(4 * 3, 0);
	REQUIRE(blueprint.rail_tracks.size() == 1);

	/* Rotate 90 degrees anticlockwise: the 3x2 area becomes 2x3, the track at
	 * (0, 0) moves to the west corner (1, 0) and becomes a Y track. */
	Blueprint rotated = TransformBlueprint(blueprint, 1, false);
	CHECK(rotated.width == 2);
	CHECK(rotated.height == 3);
	REQUIRE(rotated.rail_tracks.size() == 1);
	CHECK(rotated.rail_tracks[0].offset.x == 1);
	CHECK(rotated.rail_tracks[0].offset.y == 0);
	CHECK(rotated.rail_tracks[0].track == TRACK_Y);
	CHECK(rotated.corner_heights.size() == 3 * 4);

	/* Rotating four times gives back the original. */
	Blueprint full_circle = TransformBlueprint(TransformBlueprint(rotated, 3, false), 0, false);
	CHECK(full_circle.width == 3);
	REQUIRE(full_circle.rail_tracks.size() == 1);
	CHECK(full_circle.rail_tracks[0].offset.x == 0);
	CHECK(full_circle.rail_tracks[0].offset.y == 0);
	CHECK(full_circle.rail_tracks[0].track == TRACK_X);
}

TEST_CASE("Blueprint - non-rectangular custom airport can be copied")
{
	AllocateMap(64, 64);
	auto cleanup = scope_guard([] { _station_pool.CleanPool(); DeallocateMap(); });
	AutoRestoreBackup local(_local_company, OWNER_BEGIN);

	/* An L-shaped custom airport: the north-west corner of its bounding box (5, 5) is a hole. */
	Station *st = Station::Create(TileXY(5, 5));
	st->owner = _local_company;
	st->facilities = {StationFacility::Airport};
	st->airport.type = AT_CUSTOM;

	const std::vector<TileIndex> tiles = {
		TileXY(6, 5), TileXY(7, 5),
		TileXY(5, 6), TileXY(6, 6), TileXY(7, 6),
		TileXY(5, 7),
	};
	for (TileIndex t : tiles) {
		MakeAirport(t, _local_company, st->index, 0, WaterClass::Invalid);
		SetAirType(t, AIRTYPE_ASPHALT);
		SetAirportTileType(t, ATT_SIMPLE_TRACK);
		SetAirportTileTracks(t, TRACK_BIT_Y);
		st->rect.BeforeAddTile(t, StationRect::ADD_FORCE);
	}
	st->UpdateAirportDataStructure();

	/* The anchor is the bounding box corner, which is not an airport tile here. */
	REQUIRE(st->airport.tile == TileXY(5, 5));
	REQUIRE(!IsAirportTile(st->airport.tile));

	/* Selecting the bounding box copies the whole airport even though the anchor is a hole. */
	Blueprint blueprint;
	CopyAreaToBlueprint(TileArea(TileXY(5, 5), TileXY(7, 7)), blueprint);
	REQUIRE(blueprint.airports.size() == 1);
	CHECK(blueprint.airports[0].station == st->index);
	CHECK(blueprint.airports[0].type == AT_CUSTOM);
	CHECK(blueprint.airports[0].tiles.size() == tiles.size());
	CHECK(blueprint.airports[0].offset.x == 0);
	CHECK(blueprint.airports[0].offset.y == 0);

	/* A selection covering only the south-east part still yields the whole airport, re-based
	 * so that every tile offset is inside the resulting blueprint. */
	Blueprint partial;
	CopyAreaToBlueprint(TileArea(TileXY(6, 6), TileXY(7, 7)), partial);
	REQUIRE(partial.airports.size() == 1);
	CHECK(partial.airports[0].tiles.size() == tiles.size());
	CHECK(partial.width == 3);
	CHECK(partial.height == 3);
	for (const BlueprintAirportTile &apt : partial.airports[0].tiles) {
		CHECK(apt.offset.x >= 0);
		CHECK(apt.offset.y >= 0);
		CHECK(apt.offset.x < partial.width);
		CHECK(apt.offset.y < partial.height);
	}
}

TEST_CASE("Blueprint - two tracks with signals on one tile")
{
	AllocateMap(64, 64);
	_local_company = OWNER_BEGIN;

	MakeRailNormal(TileXY(5, 5), OWNER_BEGIN, TRACK_BIT_X | TRACK_BIT_Y, RAILTYPE_RAIL);

	Blueprint blueprint;
	uint count = CopyAreaToBlueprint(TileArea(TileXY(5, 5), TileXY(5, 5)), blueprint);

	CHECK(count == 2);
	CHECK(blueprint.width == 1);
	CHECK(blueprint.height == 1);
	REQUIRE(blueprint.rail_tracks.size() == 2);
	CHECK(blueprint.rail_tracks[0].offset.x == 0);
	CHECK(blueprint.rail_tracks[0].offset.y == 0);
	CHECK(blueprint.signals.empty());
}

TEST_CASE("Blueprint - paste data serialization round-trip")
{
	BlueprintPasteData data;
	data.terraform_mode = BlueprintTerraformMode::Minimal;
	data.height_offset = -3;

	Blueprint &bp = data.blueprint;
	bp.width = 3;
	bp.height = 2;
	bp.name = "Test Junction";
	bp.rail_tracks.push_back({{1, 0}, TRACK_LOWER, RAILTYPE_MONO});
	bp.signals.push_back({{2, 1}, TRACK_X, SignalType::Path, SignalVariant::Semaphore, true, false, {TEST_GRFID_1, 7}});
	bp.objects.push_back({{0, 0}, {TEST_GRFID_2, 500}, 1, 4, 3, 2});
	bp.tunnel_bridges.push_back({{0, 0}, {2, 0}, TransportType::Road, true, 5, INVALID_RAILTYPE, ROADTYPE_ROAD, INVALID_ROADTYPE});
	bp.station_tiles.push_back({{0, 1}, false, Axis::Y, RAILTYPE_ELECTRIC, StationID{42}, {TEST_GRFID, 321}});
	bp.road_stops.push_back({{1, 1}, StationType::Bus, true, DiagDirection::Invalid, Axis::X, ROADTYPE_ROAD, INVALID_ROADTYPE, StationID{43}, {TEST_GRFID_ALT, 654}});
	bp.corner_heights = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

	std::vector<uint8_t> buffer;
	{
		BufferSerialisationRef writer(buffer);
		data.Serialise(writer);
	}
	BlueprintPasteData result;
	{
		DeserialisationBuffer reader(buffer.data(), buffer.size());
		REQUIRE(result.Deserialise(reader, StringValidationSettings{}));
	}

	CHECK(result.terraform_mode == BlueprintTerraformMode::Minimal);
	CHECK(result.height_offset == -3);
	CHECK(result.blueprint.width == 3);
	CHECK(result.blueprint.height == 2);
	CHECK(result.blueprint.name == "Test Junction");

	REQUIRE(result.blueprint.rail_tracks.size() == 1);
	CHECK(result.blueprint.rail_tracks[0].offset.x == 1);
	CHECK(result.blueprint.rail_tracks[0].offset.y == 0);
	CHECK(result.blueprint.rail_tracks[0].track == TRACK_LOWER);
	CHECK(result.blueprint.rail_tracks[0].railtype == RAILTYPE_MONO);

	REQUIRE(result.blueprint.signals.size() == 1);
	CHECK(result.blueprint.signals[0].type == SignalType::Path);
	CHECK(result.blueprint.signals[0].variant == SignalVariant::Semaphore);
	CHECK(result.blueprint.signals[0].along);
	CHECK(!result.blueprint.signals[0].against);
	CHECK(result.blueprint.signals[0].spec == bp.signals[0].spec);
	REQUIRE(result.blueprint.objects.size() == 1);
	CHECK(result.blueprint.objects[0].spec == bp.objects[0].spec);
	CHECK(result.blueprint.objects[0].view == 1);
	CHECK(result.blueprint.objects[0].views == 4);
	CHECK(result.blueprint.objects[0].w == 3);
	CHECK(result.blueprint.objects[0].h == 2);

	REQUIRE(result.blueprint.tunnel_bridges.size() == 1);
	CHECK(result.blueprint.tunnel_bridges[0].other_end.x == 2);
	CHECK(result.blueprint.tunnel_bridges[0].is_bridge);
	CHECK(result.blueprint.tunnel_bridges[0].transport == TransportType::Road);
	CHECK(result.blueprint.tunnel_bridges[0].roadtype == ROADTYPE_ROAD);
	CHECK(result.blueprint.tunnel_bridges[0].tramtype == INVALID_ROADTYPE);

	REQUIRE(result.blueprint.station_tiles.size() == 1);
	CHECK(result.blueprint.station_tiles[0].axis == Axis::Y);
	CHECK(result.blueprint.station_tiles[0].railtype == RAILTYPE_ELECTRIC);
	CHECK(result.blueprint.station_tiles[0].station == StationID{42});
	CHECK(result.blueprint.station_tiles[0].spec == bp.station_tiles[0].spec);
	REQUIRE(result.blueprint.road_stops.size() == 1);
	CHECK(result.blueprint.road_stops[0].spec == bp.road_stops[0].spec);

	REQUIRE(result.blueprint.corner_heights.size() == 12);
	CHECK(result.blueprint.corner_heights[11] == 12);
	CHECK(result.blueprint.roads.empty());
	CHECK(result.blueprint.airports.empty());
}

TEST_CASE("Blueprint - NewGRF styles survive transforms and stay separate in station rectangles")
{
	Blueprint bp;
	bp.width = 3;
	bp.height = 1;
	bp.station_tiles.push_back({{0, 0}, false, Axis::X, RAILTYPE_RAIL, StationID{1}, {TEST_GRFID, 1}});
	bp.station_tiles.push_back({{1, 0}, false, Axis::X, RAILTYPE_RAIL, StationID{1}, {TEST_GRFID, 2}});
	bp.station_tiles.push_back({{2, 0}, true, Axis::X, RAILTYPE_RAIL, StationID{2}, {TEST_GRFID_ALT, 3}});
	bp.road_stops.push_back({{0, 0}, StationType::RoadWaypoint, true, DiagDirection::Invalid, Axis::X, ROADTYPE_ROAD, INVALID_ROADTYPE, StationID{3}, {TEST_GRFID_ALT, 4}});
	for (uint rotation = 0; rotation < 4; ++rotation) {
		for (bool reflected : {false, true}) {
			Blueprint transformed = TransformBlueprint(bp, rotation, reflected);
			auto rects = GroupStationTiles(transformed);
			REQUIRE(rects.size() == 3);
			for (const auto &st : transformed.station_tiles) {
				auto rect = std::find_if(rects.begin(), rects.end(), [&](const auto &r) { return r.offset.x == st.offset.x && r.offset.y == st.offset.y; });
				REQUIRE(rect != rects.end());
				CHECK(rect->spec == st.spec);
				CHECK(rect->w * rect->h == 1);
			}
			for (size_t i = 0; i < bp.station_tiles.size(); ++i) CHECK(transformed.station_tiles[i].spec == bp.station_tiles[i].spec);
			CHECK(transformed.road_stops[0].spec == bp.road_stops[0].spec);
		}
	}
}

TEST_CASE("Blueprint - legacy station and road stop payloads remain readable")
{
	std::vector<uint8_t> bytes;
	BufferSerialisationRef writer(bytes);
	writer.Send_generic_seq(uint16_t{2}, uint16_t{1}, std::string{"Legacy"});
	for (int i = 0; i < 9; ++i) writer.Send_generic_integer(uint32_t{0});
	writer.Send_generic_integer(uint32_t{1});
	SerialiseBlueprintOffset(writer, {0, 0});
	writer.Send_generic_seq(false, Axis::X, RAILTYPE_RAIL, StationID{12});
	writer.Send_generic_integer(uint32_t{1});
	SerialiseBlueprintOffset(writer, {1, 0});
	writer.Send_generic_seq(StationType::Bus, true, DiagDirection::Invalid, Axis::Y, ROADTYPE_ROAD, INVALID_ROADTYPE, StationID{13});
	for (int i = 0; i < 4; ++i) writer.Send_generic_integer(uint32_t{0});
	DeserialisationBuffer reader(bytes.data(), bytes.size());
	Blueprint bp;
	REQUIRE(DeserialiseBlueprint(reader, bp, StringValidationSettings{}));
	CHECK_FALSE(reader.error);
	CHECK(bp.name == "Legacy");
	REQUIRE(bp.station_tiles.size() == 1);
	REQUIRE(bp.road_stops.size() == 1);
	CHECK(bp.station_tiles[0].station == StationID{12});
	CHECK(bp.road_stops[0].station == StationID{13});
	CHECK(bp.station_tiles[0].spec.grfid == GrfID{});
	CHECK(bp.road_stops[0].spec.grfid == GrfID{});
}

TEST_CASE("Blueprint - missing NewGRF styles fail before changing the landscape")
{
	AllocateMap(64, 64);
	BlueprintPasteData data;
	data.blueprint.width = data.blueprint.height = 1;
	SECTION("rail station") {
		data.blueprint.station_tiles.push_back({{0, 0}, false, Axis::X, RAILTYPE_RAIL, StationID{1}, {TEST_GRFID_DEAD, 123}});
	}
	SECTION("road waypoint") {
		data.blueprint.road_stops.push_back({{0, 0}, StationType::RoadWaypoint, true, DiagDirection::Invalid, Axis::X, ROADTYPE_ROAD, INVALID_ROADTYPE, StationID{1}, {TEST_GRFID_DEAD, 123}});
	}
	const TileIndex tile = TileXY(10, 10);
	const TileType before = GetTileType(tile);
	CommandCost result = CmdPasteBlueprint(DoCommandFlag::Execute, tile, data);
	CHECK(result.Failed());
	CHECK(result.GetErrorMessage() == STR_BLUEPRINT_ERROR_MISSING_NEWGRF);
	CHECK(GetTileType(tile) == before);
	DeallocateMap();
}

TEST_CASE("Blueprint - undo data serialization round-trip")
{
	/* The undo payload reuses Blueprint but means something different by it: the
	 * parts a paste actually built, and the corner heights as they stood before
	 * it terraformed. */
	BlueprintUndoData data;
	data.restore_terrain = true;

	Blueprint &built = data.built;
	built.width = 2;
	built.height = 2;
	built.rail_tracks.push_back({{0, 0}, TRACK_X, RAILTYPE_RAIL});
	built.roads.push_back({{1, 0}, ROAD_NW | ROAD_NE, ROADTYPE_ROAD});
	built.station_tiles.push_back({{1, 1}, true, Axis::X, RAILTYPE_ELECTRIC, StationID{7}});
	built.airports.push_back({{0, 1}, 3, 1, 0, 0, 4, 6, StationID{9}});
	built.corner_heights = {2, 3, 4, 5, 6, 7, 8, 9, 10};

	std::vector<uint8_t> buffer;
	{
		BufferSerialisationRef writer(buffer);
		data.Serialise(writer);
	}
	BlueprintUndoData result;
	{
		DeserialisationBuffer reader(buffer.data(), buffer.size());
		REQUIRE(result.Deserialise(reader, StringValidationSettings{}));
	}

	CHECK(result.restore_terrain);
	CHECK(result.built.width == 2);
	CHECK(result.built.height == 2);

	REQUIRE(result.built.rail_tracks.size() == 1);
	CHECK(result.built.rail_tracks[0].track == TRACK_X);
	CHECK(result.built.rail_tracks[0].railtype == RAILTYPE_RAIL);

	/* Only the bits a paste really added are recorded, so a partial set has to
	 * survive the trip intact. */
	REQUIRE(result.built.roads.size() == 1);
	CHECK(result.built.roads[0].offset.x == 1);
	CHECK(result.built.roads[0].bits == (ROAD_NW | ROAD_NE));
	CHECK(result.built.roads[0].roadtype == ROADTYPE_ROAD);

	REQUIRE(result.built.station_tiles.size() == 1);
	CHECK(result.built.station_tiles[0].is_waypoint);
	CHECK(result.built.station_tiles[0].station == StationID{7});

	REQUIRE(result.built.airports.size() == 1);
	CHECK(result.built.airports[0].type == 3);
	CHECK(result.built.airports[0].layout == 1);
	CHECK(result.built.airports[0].w == 4);
	CHECK(result.built.airports[0].h == 6);

	/* The corner grid stays (width + 1) x (height + 1), so an undo can index it
	 * exactly the way the paste indexed the blueprint it came from. */
	REQUIRE(result.built.corner_heights.size() == 9);
	CHECK(result.built.corner_heights[8] == 10);

	CHECK(result.built.signals.empty());
	CHECK(result.built.canals.empty());
}

TEST_CASE("Blueprint - an undo record counts heights as content")
{
	BlueprintUndoRecord record;
	CHECK(record.IsEmpty());

	/* A paste that only terraformed built nothing, but still has something to
	 * undo, so the corner heights alone must keep the record alive. */
	record.built.corner_heights = {1, 2, 3, 4};
	CHECK(!record.IsEmpty());

	record.built.corner_heights.clear();
	record.built.canals.push_back({{0, 0}});
	CHECK(!record.IsEmpty());
}

TEST_CASE("Blueprint - an undo group re-bases onto its bounding box")
{
	/* Leaf commands report absolute tiles as they build; closing the group turns
	 * them into a record with an origin and offsets, which is the shape the undo
	 * command takes. One user action is one group, so a drag becomes one step. */
	AllocateMap(64, 64);
	ClearBlueprintUndoStack();
	_current_company = OWNER_BEGIN;
	_local_company = OWNER_BEGIN;

	BeginBlueprintUndoGroup();
	RecordBlueprintUndoRailTrack(TileXY(10, 12), TRACK_X, RAILTYPE_RAIL);
	RecordBlueprintUndoRailTrack(TileXY(13, 12), TRACK_X, RAILTYPE_MONO);
	RecordBlueprintUndoRailDepot(TileXY(10, 14), DiagDirection::NE, RAILTYPE_RAIL);
	RecordBlueprintUndoRoad(TileXY(11, 13), ROAD_NW | ROAD_NE, ROADTYPE_ROAD);
	/* A bridge is the only kind with a second coordinate, and it is the far end
	 * that decides the bounding box here. */
	RecordBlueprintUndoTunnelBridge(TileXY(12, 12), TileXY(15, 12), true, TransportType::Rail);
	EndBlueprintUndoGroup(true);

	const BlueprintUndoRecord *record = GetBlueprintUndoRecord();
	REQUIRE(record != nullptr);

	/* Bounding box is x 10..15, y 12..14. */
	CHECK(record->origin == TileXY(10, 12));
	CHECK(record->built.width == 6);
	CHECK(record->built.height == 3);

	REQUIRE(record->built.roads.size() == 1);
	CHECK(record->built.roads[0].offset.x == 1);
	CHECK(record->built.roads[0].offset.y == 1);
	CHECK(record->built.roads[0].bits == (ROAD_NW | ROAD_NE));

	REQUIRE(record->built.tunnel_bridges.size() == 1);
	CHECK(record->built.tunnel_bridges[0].offset.x == 2);
	CHECK(record->built.tunnel_bridges[0].other_end.x == 5);
	CHECK(record->built.tunnel_bridges[0].is_bridge);

	REQUIRE(record->built.rail_tracks.size() == 2);
	CHECK(record->built.rail_tracks[0].offset.x == 0);
	CHECK(record->built.rail_tracks[0].offset.y == 0);
	CHECK(record->built.rail_tracks[1].offset.x == 3);
	CHECK(record->built.rail_tracks[1].railtype == RAILTYPE_MONO);

	REQUIRE(record->built.rail_depots.size() == 1);
	CHECK(record->built.rail_depots[0].offset.x == 0);
	CHECK(record->built.rail_depots[0].offset.y == 2);
	CHECK(record->built.rail_depots[0].dir == DiagDirection::NE);

	/* The filters decide what an undo is allowed to remove. A bridge follows
	 * whatever it carries, which is why the transport type is recorded. */
	Blueprint filtered = record->built;
	BlueprintUndoFilters road_only;
	road_only.rail = false;
	road_only.water = false;
	road_only.air = false;
	FilterBlueprintUndoData(filtered, road_only);

	CHECK(filtered.rail_tracks.empty());
	CHECK(filtered.rail_depots.empty());
	CHECK(filtered.tunnel_bridges.empty()); // it carries rail
	REQUIRE(filtered.roads.size() == 1);

	ClearBlueprintUndoStack();
}

TEST_CASE("Blueprint - a failed or abandoned undo group records nothing")
{
	AllocateMap(64, 64);
	ClearBlueprintUndoStack();
	_current_company = OWNER_BEGIN;
	_local_company = OWNER_BEGIN;

	/* A command that failed built nothing worth taking back. */
	BeginBlueprintUndoGroup();
	RecordBlueprintUndoRailTrack(TileXY(5, 5), TRACK_X, RAILTYPE_RAIL);
	EndBlueprintUndoGroup(false);
	CHECK(GetBlueprintUndoRecord() == nullptr);

	/* Abandoning stands the collection down: a paste assembles its own record,
	 * and anything reported after that must not turn into a second one. */
	BeginBlueprintUndoGroup();
	AbandonBlueprintUndoGroup();
	RecordBlueprintUndoRailTrack(TileXY(5, 5), TRACK_X, RAILTYPE_RAIL);
	EndBlueprintUndoGroup(true);
	CHECK(GetBlueprintUndoRecord() == nullptr);

	ClearBlueprintUndoStack();
}

TEST_CASE("Blueprint - undo rejects a payload that does not describe itself")
{
	/* The undo payload arrives from a client and has to be assumed forgeable.
	 * These checks are what stands between a bad one and an out-of-bounds index:
	 * the corner grid is addressed by the recorded width and height, and station
	 * offsets address a grid of that size in GroupStationTiles. All of them run
	 * before any game state is touched, which is what lets them be tested here. */
	AllocateMap(64, 64);

	BlueprintUndoData data;
	data.built.width = 2;
	data.built.height = 2;
	data.built.rail_tracks.push_back({{0, 0}, TRACK_X, RAILTYPE_RAIL});

	/* An origin off the map. */
	CHECK(CmdUndoBlueprintPaste({}, TileIndex{64 * 64}, data).Failed());

	/* A corner grid that is not (width + 1) x (height + 1). */
	BlueprintUndoData bad_grid = data;
	bad_grid.built.corner_heights.assign(4, 0);
	CHECK(CmdUndoBlueprintPaste({}, TileXY(10, 10), bad_grid).Failed());

	/* An element sitting outside the area it claims to belong to. */
	BlueprintUndoData bad_offset = data;
	bad_offset.built.rail_tracks[0].offset = {5, 5};
	CHECK(CmdUndoBlueprintPaste({}, TileXY(10, 10), bad_offset).Failed());

	/* A station tile outside the area, which would otherwise index the grouping
	 * grid out of bounds. */
	BlueprintUndoData bad_station = data;
	bad_station.built.station_tiles.push_back({{9, 9}, false, Axis::X, RAILTYPE_RAIL, StationID{1}});
	CHECK(CmdUndoBlueprintPaste({}, TileXY(10, 10), bad_station).Failed());

	/* A width outside the permitted range. */
	BlueprintUndoData bad_size = data;
	bad_size.built.width = 0;
	CHECK(CmdUndoBlueprintPaste({}, TileXY(10, 10), bad_size).Failed());
}

TEST_CASE("Blueprint - undo regroups recorded station tiles into one rectangle")
{
	/* An undo records station tiles one at a time and leans on GroupStationTiles
	 * to turn them back into the rectangles the removal commands take. A block
	 * that came apart here would make an undo remove the wrong tiles. */
	Blueprint bp;
	bp.width = 4;
	bp.height = 3;

	for (int16_t y = 0; y < 3; y++) {
		for (int16_t x = 0; x < 2; x++) {
			bp.station_tiles.push_back({{x, y}, false, Axis::Y, RAILTYPE_RAIL, StationID{1}});
		}
	}

	std::vector<BlueprintStationRect> rects = GroupStationTiles(bp);

	REQUIRE(rects.size() == 1);
	CHECK(rects[0].offset.x == 0);
	CHECK(rects[0].offset.y == 0);
	CHECK(rects[0].w == 2);
	CHECK(rects[0].h == 3);
	CHECK(!rects[0].is_waypoint);
	CHECK(rects[0].axis == Axis::Y);
	CHECK(rects[0].station == StationID{1});
}

TEST_CASE("Blueprint - undo keeps unlike station tiles in separate rectangles")
{
	/* Adjacent tiles of two different stations, and a waypoint next to a station,
	 * must not be merged: a removal command takes one rectangle of one station,
	 * so a merged rectangle would reach beyond what it is meant to remove. */
	Blueprint bp;
	bp.width = 3;
	bp.height = 1;
	bp.station_tiles.push_back({{0, 0}, false, Axis::X, RAILTYPE_RAIL, StationID{1}});
	bp.station_tiles.push_back({{1, 0}, false, Axis::X, RAILTYPE_RAIL, StationID{2}});
	bp.station_tiles.push_back({{2, 0}, true, Axis::X, RAILTYPE_RAIL, StationID{2}});

	std::vector<BlueprintStationRect> rects = GroupStationTiles(bp);

	REQUIRE(rects.size() == 3);
	for (const BlueprintStationRect &rect : rects) {
		CHECK(rect.w == 1);
		CHECK(rect.h == 1);
	}
}

TEST_CASE("Blueprint - full slot set serialization round-trip")
{
	/* The blueprint set file format wraps NUM_BLUEPRINT_SLOTS blueprints in a single
	 * length-prefixed vector, exactly like ReadBlueprintVector/WriteBlueprintVector
	 * already do for e.g. rail_tracks; this exercises that same generic mechanism one
	 * level deeper (a vector of Blueprint, each already containing several vectors). */
	std::vector<Blueprint> slots;
	slots.resize(NUM_BLUEPRINT_SLOTS);
	slots[0].name = "Empty but named";
	slots[2].width = 2;
	slots[2].height = 1;
	slots[2].name = "Junction";
	slots[2].rail_tracks.push_back({{0, 0}, TRACK_X, RAILTYPE_RAIL});
	slots[2].rail_tracks.push_back({{1, 0}, TRACK_X, RAILTYPE_RAIL});
	slots[2].corner_heights = {3, 3, 3, 3, 3, 3};
	slots[7].width = 1;
	slots[7].height = 1;
	slots[7].road_depots.push_back({{0, 0}, DiagDirection::NE, ROADTYPE_ROAD});

	std::vector<uint8_t> buffer;
	{
		BufferSerialisationRef writer(buffer);
		WriteBlueprintVector(writer, slots);
	}

	std::vector<Blueprint> result;
	{
		DeserialisationBuffer reader(buffer.data(), buffer.size());
		REQUIRE(ReadBlueprintVector(reader, result, StringValidationSettings{}));
	}

	REQUIRE(result.size() == NUM_BLUEPRINT_SLOTS);
	CHECK(result[0].name == "Empty but named");
	CHECK(result[0].IsEmpty());
	CHECK(result[1].name.empty());
	CHECK(result[1].IsEmpty());

	CHECK(result[2].name == "Junction");
	REQUIRE(result[2].rail_tracks.size() == 2);
	CHECK(result[2].rail_tracks[1].offset.x == 1);
	CHECK(result[2].corner_heights.size() == 6);

	REQUIRE(result[7].road_depots.size() == 1);
	CHECK(result[7].road_depots[0].dir == DiagDirection::NE);
	CHECK(result[7].road_depots[0].roadtype == ROADTYPE_ROAD);
}

TEST_CASE("Airport - rebuilding caches preserves saved hangar depots")
{
	AllocateMap(64, 64);
	REQUIRE(Station::GetNumItems() == 0);
	REQUIRE(Depot::GetNumItems() == 0);
	auto cleanup = scope_guard([] { _station_pool.CleanPool(); _depot_pool.CleanPool(); DeallocateMap(); });
	TileIndex tile = TileXY(10, 10);
	Station *st = Station::Create(tile);
	st->owner = OWNER_BEGIN;
	st->facilities = {StationFacility::Airport};
	st->airport.type = AT_CUSTOM;
	st->rect.BeforeAddTile(tile, StationRect::ADD_FORCE);
	MakeAirport(tile, OWNER_BEGIN, st->index, 0, WaterClass::Invalid);
	SetAirType(tile, AIRTYPE_ASPHALT);
	SetAirportTileType(tile, ATT_HANGAR_STANDARD);
	SetHangarDirection(tile, DiagDirection::NE);
	SetAirportTileTracks(tile, TRACK_BIT_X);
	st->UpdateAirportDataStructure();
	REQUIRE(st->airport.hangar != nullptr);
	const DepotID saved_id = st->airport.hangar->index;
	st->airport.hangar->name = "Saved hangar";
	SECTION("running game rebuild") {}
	SECTION("joining client restores the depot pool before the station cache") {
		st->airport.hangar = nullptr;
	}
	GameRandomSeedChecker random_checker;
	st->UpdateAirportDataStructure();
	REQUIRE(st->airport.hangar != nullptr);
	CHECK(st->airport.hangar->index == saved_id);
	CHECK(st->airport.hangar->name == "Saved hangar");
	CHECK(Depot::GetNumItems() == 1);
	CHECK(random_checker.Check());
	st->UpdateAirportDataStructure();
	CHECK(st->airport.hangar->index == saved_id);
	CHECK(Depot::GetNumItems() == 1);
}

TEST_CASE("Blueprint - runway flight reservations never copy but hangar orientation does")
{
	for (AirportTileType type : {ATT_RUNWAY_MIDDLE, ATT_RUNWAY_END, ATT_RUNWAY_START_NO_LANDING, ATT_RUNWAY_START_ALLOW_LANDING, ATT_HANGAR_STANDARD, ATT_APRON_NORMAL}) {
		BlueprintAirportTile tile{{0, 0}, 0, uint8_t(type << 4), 0, uint16_t(0x8000 | (0x3F << 6) | TRACK_BIT_X)};
		ClearBlueprintAirportReservations(tile);
		CHECK((tile.m8 & (0x3F << 6)) == 0);
		CHECK(HasBit(tile.m8, 15) == (type == ATT_HANGAR_STANDARD || type == ATT_APRON_NORMAL));
		CHECK((tile.m8 & TRACK_BIT_MASK) == TRACK_BIT_X);
		for (uint8_t rotation = 0; rotation < 4; rotation++) {
			BlueprintAirportTile rotated = tile;
			TransformBlueprintAirportTile(rotated, rotation, true);
			if (type >= ATT_RUNWAY_MIDDLE) CHECK_FALSE(HasBit(rotated.m8, 15));
		}
	}
}

TEST_CASE("Blueprint - invalid custom airport tile types fail before changing the map")
{
	AllocateMap(64, 64);
	auto cleanup = scope_guard([] { DeallocateMap(); });
	GameRandomSeedChecker random_checker;
	const TileIndex tile = TileXY(10, 10);
	const TileType old_type = GetTileType(tile);
	for (uint8_t type : {uint8_t(ATT_WAITING_POINT), uint8_t(9), uint8_t(11)}) {
		BlueprintPasteData data;
		data.blueprint.width = data.blueprint.height = 1;
		BlueprintAirport airport{{0, 0}, AT_CUSTOM, 0, 0, AIRTYPE_ASPHALT, 1, 1, StationID{0}, {}};
		airport.tiles.push_back({{0, 0}, 0, uint8_t(type << 4), 0, 0});
		data.blueprint.airports.push_back(airport);
		CHECK(CmdPasteBlueprint({}, tile, data).Failed());
		CHECK(CmdPasteBlueprint(DoCommandFlag::Execute, tile, data).Failed());
		CHECK(GetTileType(tile) == old_type);
		CHECK(random_checker.Check());
	}
}

TEST_CASE("Airport - equal surface sorting priorities use deterministic IDs")
{
	std::array<AirTypeInfo, AIRTYPE_END> original;
	std::copy_n(_airtypes, AIRTYPE_END, original.begin());
	AutoRestoreBackup sorted(_sorted_airtypes, AutoRestoreBackupNoNewValueTag{});
	auto cleanup = scope_guard([&] { std::copy(original.begin(), original.end(), _airtypes); });
	for (auto &type : _airtypes) type.label = 0;
	_airtypes[0].label = 1;
	_airtypes[1].label = 2;
	_airtypes[0].sorting_order = _airtypes[1].sorting_order = 10;
	InitAirTypes();
	REQUIRE(_sorted_airtypes.size() == 2);
	CHECK(_sorted_airtypes[0] == AirType{0});
	CHECK(_sorted_airtypes[1] == AirType{1});
}

TEST_CASE("Airport - edited hangar targets resolve stations before colliding depot IDs")
{
	AllocateMap(64, 64);
	auto cleanup = scope_guard([] {
		_vehicle_pool.CleanPool(); _engine_pool.CleanPool();
		_station_pool.CleanPool(); _depot_pool.CleanPool(); DeallocateMap();
	});
	Station *target = Station::CreateAtIndex(StationID{12}, TileXY(10, 10));
	Station *other = Station::CreateAtIndex(StationID{21}, TileXY(20, 20));
	for (Station *st : {target, other}) {
		st->facilities = {StationFacility::Airport};
		st->airport.type = AT_CUSTOM;
		st->rect.BeforeAddTile(st->xy, StationRect::ADD_FORCE);
		MakeAirport(st->xy, OWNER_BEGIN, st->index, 0, WaterClass::Invalid);
		SetAirType(st->xy, AIRTYPE_ASPHALT);
		SetAirportTileType(st->xy, ATT_HANGAR_STANDARD);
		SetHangarDirection(st->xy, DiagDirection::NE);
		SetAirportTileTracks(st->xy, TRACK_BIT_X);
	}
	other->airport.hangar = Depot::CreateAtIndex(DepotID{12}, other->xy);
	for (Station *st : {target, other}) st->UpdateAirportDataStructure();
	REQUIRE(target->airport.hangar->index != DepotID{target->index.base()});
	Engine *engine = Engine::Create(VehicleType::Aircraft, 0);
	Aircraft *v = Aircraft::Create();
	v->subtype = AIR_AIRCRAFT;
	v->state = AS_IDLE;
	v->engine_type = engine->index;
	v->targetairport = target->index;
	v->current_order.MakeGoToDepot(target->index, {});
	v->dest_tile = INVALID_TILE;
	SetBit(target->airport.flags, AFB_LANDING_RUNWAY);
	CHECK(GetOrderHangar(v->current_order) == target->airport.hangar);
	UpdateAircraftOnUpdatedAirport(target);
	CHECK(v->dest_tile == target->xy);
	/* Removing the intended hangar must not retarget to the colliding ID. */
	SetAirportTileType(target->xy, ATT_SIMPLE_TRACK);
	target->UpdateAirportDataStructure();
	CHECK(target->airport.hangar == nullptr);
	CHECK(GetOrderHangar(v->current_order) == nullptr);
}

TEST_CASE("Airport - invalid edit arguments fail without modifying map or random state")
{
	AllocateMap(64, 64);
	auto cleanup = scope_guard([] { DeallocateMap(); });
	AutoRestoreBackup enabled(_settings_game.station.allow_modify_airports, true);
	const TileIndex tile = TileXY(10, 10);
	MakeAirport(tile, OWNER_BEGIN, StationID{0}, 0, WaterClass::Invalid);
	SetAirType(tile, AIRTYPE_ASPHALT);
	SetAirportTileType(tile, ATT_SIMPLE_TRACK);
	SetAirGfxType(tile, true);
	const auto before = std::make_tuple(Tile(tile).m3(), Tile(tile).m4(), Tile(tile).m6(), Tile(tile).m8());
	GameRandomSeedChecker random_checker;
	for (DoCommandFlags flags : {DoCommandFlags{}, DoCommandFlags{DoCommandFlag::Execute}}) {
		for (AirportTileType type : {ATT_WAITING_POINT, ATT_SIMPLE_TRACK, ATT_RUNWAY_MIDDLE, ATT_APRON_BUILTIN_HELIPORT, static_cast<AirportTileType>(9), static_cast<AirportTileType>(255)}) {
			CHECK(CmdChangeAirportTiles(flags, tile, tile, AIRTYPE_ASPHALT, type, ATTG_DEFAULT_GFX, DiagDirection::NE, true, false).Failed());
		}
		CHECK(CmdChangeAirportTiles(flags, tile, tile, INVALID_AIRTYPE, ATT_APRON_NORMAL, ATTG_DEFAULT_GFX, DiagDirection::NE, true, false).Failed());
		CHECK(CmdChangeAirportTiles(flags, tile, tile, AIRTYPE_ASPHALT, ATT_APRON_NORMAL, ATTG_DEFAULT_GFX, static_cast<DiagDirection>(255), true, false).Failed());
		CHECK(CmdChangeAirportTiles(flags, tile, tile, AIRTYPE_ASPHALT, ATT_APRON_NORMAL, static_cast<AirportTiles>(255), DiagDirection::NE, true, false).Failed());
		CHECK(CmdAddRemoveTracksToAirport(flags, tile, tile, AIRTYPE_ASPHALT, true, static_cast<Track>(255)).Failed());
		CHECK(CmdAirportChangeTrackGFX(flags, tile, tile, AIRTYPE_ASPHALT, 255, false).Failed());
		CHECK(CmdChangeAirType(flags, INVALID_TILE, AIRTYPE_ASPHALT).Failed());
		CHECK(CmdChangeAirportTiles(flags, tile, INVALID_TILE, AIRTYPE_ASPHALT, ATT_APRON_NORMAL, ATTG_DEFAULT_GFX, DiagDirection::NE, true, false).Failed());
		CHECK(CmdAirportToggleGround(flags, INVALID_TILE, tile, AIRTYPE_ASPHALT, false).Failed());
		CHECK(CmdAddRemoveAirportTiles(flags, tile, INVALID_TILE, true, AIRTYPE_ASPHALT, StationID::Invalid(), false).Failed());
		CHECK(CmdBuildAirport(flags, tile, 255, 0, AIRTYPE_ASPHALT, DiagDirection::NE, StationID::Invalid(), false).Failed());
		CHECK(CmdBuildAirport(flags, tile, 0, 255, AIRTYPE_ASPHALT, DiagDirection::NE, StationID::Invalid(), false).Failed());
		CHECK(before == std::make_tuple(Tile(tile).m3(), Tile(tile).m4(), Tile(tile).m6(), Tile(tile).m8()));
		CHECK(random_checker.Check());
	}
}

TEST_CASE("Airport - duplicate legacy hangars resolve identically for server and joining client")
{
	AllocateMap(64, 64);
	REQUIRE(Station::GetNumItems() == 0);
	REQUIRE(Depot::GetNumItems() == 0);
	auto cleanup = scope_guard([] { _station_pool.CleanPool(); _depot_pool.CleanPool(); DeallocateMap(); });
	const TileIndex tile = TileXY(10, 10);
	Station *st = Station::Create(tile);
	st->rect.BeforeAddTile(tile, StationRect::ADD_FORCE);
	st->airport.type = AT_CUSTOM;
	MakeAirport(tile, OWNER_BEGIN, st->index, 0, WaterClass::Invalid);
	SetAirType(tile, AIRTYPE_ASPHALT);
	SetAirportTileType(tile, ATT_HANGAR_STANDARD);
	SetHangarDirection(tile, DiagDirection::NE);
	SetAirportTileTracks(tile, TRACK_BIT_X);
	REQUIRE(Depot::CanAllocateItem(2));
	Depot *first = Depot::Create(tile);
	Depot *second = Depot::Create(tile);
	first->name = "Original hangar";
	st->airport.hangar = second;
	st->UpdateAirportDataStructure();
	CHECK(st->airport.hangar == first);
	st->airport.hangar = nullptr;
	st->UpdateAirportDataStructure();
	CHECK(st->airport.hangar == first);
	CHECK(st->airport.hangar->name == "Original hangar");
	CHECK(Depot::GetNumItems() == 2); // Do not delete another persistent identity while rebuilding caches.
}

TEST_CASE("Blueprint - raw custom runways must be complete including crossing branches")
{
	BlueprintAirport airport{{0, 0}, AT_CUSTOM, 0, 0, AIRTYPE_ASPHALT, 5, 5, StationID{0}, {}};
	auto add = [&](int x, int y, AirportTileType type, uint direction) {
		airport.tiles.push_back({{static_cast<int16_t>(x), static_cast<int16_t>(y)}, 0, static_cast<uint8_t>(type << 4), 0, static_cast<uint16_t>(direction << 12)});
	};
	add(0, 2, ATT_RUNWAY_START_ALLOW_LANDING, to_underlying(DiagDirection::SW));
	add(1, 2, ATT_RUNWAY_MIDDLE, to_underlying(Direction::SW));
	add(2, 2, ATT_RUNWAY_MIDDLE, to_underlying(Direction::S)); // Crossing requires both branches.
	add(3, 2, ATT_RUNWAY_MIDDLE, to_underlying(Direction::SW));
	add(4, 2, ATT_RUNWAY_END, to_underlying(DiagDirection::SW));
	CHECK_FALSE(IsBlueprintAirportLayoutValid(airport));
	add(2, 0, ATT_RUNWAY_START_NO_LANDING, to_underlying(DiagDirection::SE));
	add(2, 1, ATT_RUNWAY_MIDDLE, to_underlying(Direction::SE));
	add(2, 3, ATT_RUNWAY_MIDDLE, to_underlying(Direction::SE));
	add(2, 4, ATT_RUNWAY_END, to_underlying(DiagDirection::SE));
	CHECK(IsBlueprintAirportLayoutValid(airport));
	airport.tiles.pop_back();
	CHECK_FALSE(IsBlueprintAirportLayoutValid(airport));
	airport.tiles.clear();
	add(1, 1, ATT_RUNWAY_END, 0);
	CHECK_FALSE(IsBlueprintAirportLayoutValid(airport));
	airport.tiles.clear();
	add(1, 1, ATT_SIMPLE_TRACK, 0);
	CHECK(IsBlueprintAirportLayoutValid(airport));
	airport.tiles.push_back(airport.tiles.front());
	CHECK_FALSE(IsBlueprintAirportLayoutValid(airport));
}

TEST_CASE("Blueprint - incomplete runway payload fails before map changes")
{
	AllocateMap(64, 64);
	auto cleanup = scope_guard([] { DeallocateMap(); });
	const TileIndex tile = TileXY(10, 10);
	const TileType before = GetTileType(tile);
	BlueprintPasteData data;
	data.blueprint.width = data.blueprint.height = 1;
	BlueprintAirport airport{{0, 0}, AT_CUSTOM, 0, 0, AIRTYPE_ASPHALT, 1, 1, StationID{0}, {}};
	airport.tiles.push_back({{0, 0}, 0, static_cast<uint8_t>(ATT_RUNWAY_START_ALLOW_LANDING << 4), 0, 0});
	data.blueprint.airports.push_back(airport);
	GameRandomSeedChecker random_checker;
	CHECK(CmdPasteBlueprint({}, tile, data).Failed());
	CHECK(CmdPasteBlueprint(DoCommandFlag::Execute, tile, data).Failed());
	CHECK(GetTileType(tile) == before);
	CHECK(random_checker.Check());
}

static uint32_t BlueprintRestrictionItem(TraceRestrictItemType type, uint16_t value = 0)
{
	TraceRestrictInstructionItem item{0};
	item.SetType(type);
	item.SetValue(value);
	return item.base();
}

class BlueprintRestrictionFixture {
protected:
	AutoRestoreBackup<uint16_t> styles{_enabled_new_signal_styles_mask, uint16_t{1}};
	AutoRestoreBackup<CompanyID> company_backup{_current_company, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<CompanyID> local_backup{_local_company, AutoRestoreBackupNoNewValueTag{}};
	TileIndex source;
	BlueprintRestrictionFixture()
	{
		REQUIRE(Company::GetNumItems() == 0);
		REQUIRE(_tracerestrictprogram_mapping.empty());
		AllocateMap(64, 64);
		_current_company = _local_company = Company::Create()->index;
		Company::Get(_current_company)->money = 1000000;
		source = TileXY(10, 10);
		MakeSignal(source);
	}
	~BlueprintRestrictionFixture()
	{
		ClearTraceRestrictMapping();
		_tracerestrictprogram_pool.CleanPool();
		_company_pool.CleanPool();
		DeallocateMap();
	}
	void MakeSignal(TileIndex tile)
	{
		MakeRailNormal(tile, _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		SB(_m[tile].m5, 6, 2, to_underlying(RailTileType::Signals));
		SetPresentSignals(tile, SignalAlongTrackdir(TrackToTrackdir(TRACK_X)));
		SetSignalType(tile, TRACK_X, SignalType::Path);
	}
	TraceRestrictProgram *InstallSource()
	{
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(source, TRACK_X), true);
		program->items = {
			TraceRestrictProgramItem{BlueprintRestrictionItem(TRIT_GUI_LABEL, 0)},
			TraceRestrictProgramItem{BlueprintRestrictionItem(TRIT_COND_PBS_ENTRY_SIGNAL)},
			TraceRestrictProgramItem{TileXY(11, 10).base()},
			TraceRestrictProgramItem{BlueprintRestrictionItem(TRIT_PF_PENALTY, 500)},
			TraceRestrictProgramItem{BlueprintRestrictionItem(TRIT_COND_ENDIF)},
		};
		program->texts = std::make_unique<TraceRestrictProgramTexts>();
		program->texts->labels = {"Route note"};
		REQUIRE(program->Validate().Succeeded());
		return program;
	}
};

TEST_CASE_METHOD(BlueprintRestrictionFixture, "Blueprint - restriction snapshot survives source deletion and payload transport")
{
	InstallSource();
	BlueprintPasteData data;
	CopyAreaToBlueprint(TileArea(source, 2, 1), data.blueprint);
	REQUIRE(data.blueprint.signals.size() == 1);
	const auto &signal = data.blueprint.signals.front();
	REQUIRE(signal.restriction_tiles.size() == 1);
	CHECK(signal.restriction_tiles.front().item == 2);
	CHECK(data.blueprint.width == 2);
	CHECK(signal.restriction_labels == std::vector<std::string>{"Route note"});
	TraceRestrictRemoveProgramMapping(MakeTraceRestrictRefId(source, TRACK_X));
	std::vector<uint8_t> bytes;
	data.Serialise(BufferSerialisationRef(bytes));
	BlueprintPasteData transported;
	DeserialisationBuffer reader(bytes.data(), bytes.size());
	REQUIRE(transported.Deserialise(reader, {}));
	CHECK(transported.blueprint.signals.front().restriction_items == signal.restriction_items);
	CHECK(transported.blueprint.signals.front().restriction_labels == signal.restriction_labels);
	const TileIndex target = TileXY(20, 20);
	MakeRailNormal(target, _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
	GameRandomSeedChecker random_checker;
	auto paste_result = CmdPasteBlueprint(DoCommandFlag::Execute, target, transported);
	INFO("Paste error " << paste_result.GetErrorMessage().base());
	REQUIRE(paste_result.Succeeded());
	auto *pasted = GetTraceRestrictProgram(MakeTraceRestrictRefId(target, TRACK_X), false);
	REQUIRE(pasted != nullptr);
	CHECK(pasted->items[2].base() == TileXY(21, 20).base());
	CHECK(pasted->items[3].base() == signal.restriction_items[3]);
	CHECK(pasted->GetLabel(0) == "Route note");
	CHECK(IsRestrictedSignal(target));
	const TileIndex second = TileXY(30, 30);
	MakeRailNormal(second, _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
	REQUIRE(CmdPasteBlueprint(DoCommandFlag::Execute, second, transported).Succeeded());
	auto *independent = GetTraceRestrictProgram(MakeTraceRestrictRefId(second, TRACK_X), false);
	REQUIRE(independent != nullptr);
	CHECK(independent != pasted);
	pasted->items[3] = TraceRestrictProgramItem{BlueprintRestrictionItem(TRIT_PF_PENALTY, 100)};
	CHECK(independent->items[3].base() == signal.restriction_items[3]);
	BlueprintUndoData undo;
	undo.built.width = undo.built.height = 1;
	undo.built.signals.push_back({{0, 0}, TRACK_X, SignalType::Path, SignalVariant::Electric, true, false});
	REQUIRE(CmdUndoBlueprintPaste(DoCommandFlag::Execute, target, undo).Succeeded());
	CHECK(GetTraceRestrictProgram(MakeTraceRestrictRefId(target, TRACK_X), false) == nullptr);
	CHECK(GetTraceRestrictProgram(MakeTraceRestrictRefId(second, TRACK_X), false) == independent);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(BlueprintRestrictionFixture, "Blueprint - restriction directions and local references follow all eight transforms")
{
	auto *program = InstallSource();
	program->items.push_back(TraceRestrictProgramItem{BlueprintRestrictionItem(TRIT_COND_ENTRY_DIRECTION, 0)});
	program->items.push_back(TraceRestrictProgramItem{BlueprintRestrictionItem(TRIT_PF_DENY)});
	program->items.push_back(TraceRestrictProgramItem{BlueprintRestrictionItem(TRIT_COND_ENDIF)});
	Blueprint bp;
	CopyAreaToBlueprint(TileArea(source, 2, 1), bp);
	for (uint8_t rotation = 0; rotation < 4; ++rotation) for (bool mirror : {false, true}) {
		Blueprint transformed = TransformBlueprint(bp, rotation, mirror);
		const auto &signal = transformed.signals.front();
		const auto expected = TransformBlueprintPoint({1, 0}, 2, 1, rotation, mirror);
		CHECK(signal.restriction_tiles.front().offset.x == expected.x);
		CHECK(signal.restriction_tiles.front().offset.y == expected.y);
		CHECK(TraceRestrictInstructionItem{signal.restriction_items[5]}.GetValue() == to_underlying(TransformBlueprintDiagDir(DiagDirection::NE, rotation, mirror)));
		CHECK(signal.restriction_labels == bp.signals.front().restriction_labels);
	}
}

TEST_CASE_METHOD(BlueprintRestrictionFixture, "Blueprint - malformed restrictions fail before terrain changes")
{
	InstallSource();
	BlueprintPasteData data;
	CopyAreaToBlueprint(TileArea(source, 2, 1), data.blueprint);
	auto &signal = data.blueprint.signals.front();
	SECTION("truncated double instruction") { signal.restriction_items.resize(2); }
	SECTION("unbalanced conditional") { signal.restriction_items.pop_back(); }
	SECTION("missing label") { signal.restriction_labels.clear(); }
	SECTION("relocation points at action") { signal.restriction_tiles.front().item = 3; }
	SECTION("duplicate relocation") { signal.restriction_tiles.push_back(signal.restriction_tiles.front()); }
	SECTION("relocation outside bounds") { signal.restriction_tiles.front().offset.x = 2; }
	data.terraform_mode = BlueprintTerraformMode::Full;
	data.height_offset = 3;
	data.blueprint.corner_heights.assign(6, 0);
	const TileIndex target = TileXY(20, 20);
	const auto before = TileHeight(target);
	CHECK(CmdPasteBlueprint(DoCommandFlag::Execute, target, data).Failed());
	CHECK(TileHeight(target) == before);
	CHECK(GetTraceRestrictProgram(MakeTraceRestrictRefId(target, TRACK_X), false) == nullptr);
}

TEST_CASE_METHOD(BlueprintRestrictionFixture, "Blueprint - restriction install tests are read only and preserve existing programs")
{
	const auto *original = InstallSource();
	std::vector<TraceRestrictProgramItem> items{TraceRestrictProgramItem{BlueprintRestrictionItem(TRIT_PF_DENY)}};
	TileIndex target = TileXY(20, 20);
	MakeSignal(target);
	CHECK(ApplyTraceRestrictProgramSnapshot({}, target, TRACK_X, items, {}).Succeeded());
	CHECK(GetTraceRestrictProgram(MakeTraceRestrictRefId(target, TRACK_X), false) == nullptr);
	CHECK(ApplyTraceRestrictProgramSnapshot(DoCommandFlag::Execute, source, TRACK_X, items, {}).Succeeded());
	CHECK(GetTraceRestrictProgram(MakeTraceRestrictRefId(source, TRACK_X), false) == original);
	CHECK(original->items.size() == 5);
	CHECK(ApplyTraceRestrictProgramSnapshot(DoCommandFlag::Execute, target, TRACK_X, items, {}).Succeeded());
	CHECK(GetTraceRestrictProgram(MakeTraceRestrictRefId(target, TRACK_X), false) != original);
}

TEST_CASE("Blueprint - version five imports without restriction section")
{
	Blueprint bp;
	bp.width = bp.height = 1;
	bp.signals.push_back({{0, 0}, TRACK_X, SignalType::Path, SignalVariant::Electric, true, false});
	std::vector<uint8_t> bytes;
	SerialiseBlueprint(BufferSerialisationRef(bytes), bp);
	bytes[0] = uint8_t((UINT16_MAX - 1) & 0xFF);
	bytes[1] = uint8_t((UINT16_MAX - 1) >> 8);
	bytes.resize(bytes.size() - 3); // Three varuint zero counts for the single signal.
	DeserialisationBuffer reader(bytes.data(), bytes.size());
	Blueprint restored;
	REQUIRE(DeserialiseBlueprint(reader, restored, {}));
	REQUIRE(restored.signals.size() == 1);
	CHECK(restored.signals.front().restriction_items.empty());
}

TEST_CASE_METHOD(BlueprintRestrictionFixture, "Blueprint - restriction snapshot respects slot ownership and public access")
{
	REQUIRE(TraceRestrictSlot::GetNumItems() == 0);
	auto cleanup = scope_guard([] { _tracerestrictslot_pool.CleanPool(); });
	auto *own = TraceRestrictSlot::Create(_current_company);
	auto *public_slot = TraceRestrictSlot::Create(CompanyID{1});
	public_slot->flags.Set(TraceRestrictSlot::Flag::Public);
	auto *private_slot = TraceRestrictSlot::Create(CompanyID{1});
	std::vector<TraceRestrictProgramItem> items;
	for (const auto *slot : {own, public_slot, private_slot}) items.emplace_back(BlueprintRestrictionItem(TRIT_SLOT, slot->index.base()));
	REQUIRE(ApplyTraceRestrictProgramSnapshot(DoCommandFlag::Execute, source, TRACK_X, items, {}).Succeeded());
	auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(source, TRACK_X), false);
	REQUIRE(program != nullptr);
	CHECK(TraceRestrictInstructionItem{program->items[0].base()}.GetValue() == own->index.base());
	CHECK(TraceRestrictInstructionItem{program->items[1].base()}.GetValue() == public_slot->index.base());
	CHECK(TraceRestrictInstructionItem{program->items[2].base()}.GetValue() == INVALID_TRACE_RESTRICT_SLOT_ID.base());
}

TEST_CASE("Blueprint - restriction payload rejects truncation and excessive list counts")
{
	Blueprint bp;
	bp.width = bp.height = 1;
	bp.signals.push_back({{0, 0}, TRACK_X, SignalType::Path, SignalVariant::Electric, true, false});
	std::vector<uint8_t> bytes;
	SerialiseBlueprint(BufferSerialisationRef(bytes), bp);
	SECTION("truncated snapshot section") { bytes.pop_back(); }
	SECTION("excessive instruction list") {
		bytes.resize(bytes.size() - 3);
		BufferSerialisationRef writer(bytes);
		writer.Send_generic_integer(uint32_t{MAX_BLUEPRINT_VECTOR_LENGTH + 1});
	}
	DeserialisationBuffer reader(bytes.data(), bytes.size());
	Blueprint restored;
	CHECK_FALSE(DeserialiseBlueprint(reader, restored, {}));
}
