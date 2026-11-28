/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../aircraft.h"
#include "../aircraft_cmd.h"
#include "../air_map.h"
#include "../airport.h"
#include "../airport_cmd.h"
#include "../company_base.h"
#include "../company_func.h"
#include "../core/backup_type.hpp"
#include "../date_func.h"
#include "../departures_gui.h"
#include "../depot_base.h"
#include "../depot_map.h"
#include "../engine_base.h"
#include "../engine_func.h"
#include "../network/network.h"
#include "../pbs_air.h"
#include "../settings_type.h"
#include "../sl/extended_ver_sl.h"
#include "../station_base.h"
#include "../scope.h"
#include "../vehicle_cmd.h"
#include "../vehicle_func.h"
#include "../vehiclelist.h"
#include "../table/strings.h"

#include "../safeguards.h"

static Aircraft *MakeReservationAircraft(TileIndex tile)
{
	Aircraft *v = Aircraft::Create();
	Aircraft *shadow = Aircraft::Create();
	v->subtype = AIR_AIRCRAFT;
	shadow->subtype = AIR_SHADOW;
	v->SetNext(shadow);
	v->tile = tile;
	v->state = AS_IDLE;
	v->trackdir = TRACKDIR_X_SW;
	v->next_trackdir = INVALID_TRACKDIR;
	v->direction = Direction::SW;
	v->next_pos.pos = AP_DEFAULT;
	v->UpdateNextTile(tile);
	return v;
}

static Station *MakeReservationAirport(TrackBits tracks = TRACK_BIT_CROSS, uint x = 10, uint y = 10)
{
	const TileIndex origin = TileXY(x, y);
	Station *st = Station::Create(origin);
	st->facilities = {StationFacility::Airport};
	st->airport.tile = origin;
	st->airport.w = 12;
	st->airport.h = 4;
	for (TileIndex tile : st->airport) {
		MakeAirport(tile, OWNER_BEGIN, st->index, 0, WaterClass::Invalid);
		SetAirportTileType(tile, ATT_SIMPLE_TRACK);
		SetAirportTileTracks(tile, tracks);
	}
	return st;
}

TEST_CASE("Aircraft rolling reservations - exclusive current and one tile ahead")
{
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	/* A junction grant can protect a third tile even without a real reservation
	 * there. Use a straight taxiway to isolate the rolling window. */
	MakeReservationAirport(TRACK_BIT_X);
	Aircraft *a = MakeReservationAircraft(TileXY(10, 10));
	a->planned_path = {{TileXY(10,10), TRACKDIR_X_SW, false}, {TileXY(11,10), TRACKDIR_X_SW, false}, {TileXY(12,10), TRACKDIR_X_SW, false}};
	CHECK_FALSE(ExtendAircraftTaxiReservation(a));
	REQUIRE(TryReserveAircraftTaxiPath(a));
	REQUIRE(ExtendAircraftTaxiReservation(a));
	CHECK(a->reserved_path.size() == 2);
	CHECK(HasAircraftTileReservation(a, a->tile, TRACK_BIT_X));
	CHECK(HasAircraftTileReservation(a, TileXY(11, 10), TRACK_BIT_X));
	CHECK(HasAircraftVirtualTileReservation(a, TileXY(12, 10), TRACKDIR_BIT_X_SW));
	CHECK(GetReservedAirportTracks(TileXY(12,10)) == TRACK_BIT_NONE);
	Aircraft *b = MakeReservationAircraft(TileXY(12, 10));
	CHECK_FALSE(TryReserveAircraftTile(b, TileXY(11,10), TRACK_BIT_X));
	REQUIRE(TryReserveAircraftTile(b, TileXY(12,10), TRACK_BIT_X));
	ReleaseAircraftTile(a, TileXY(10,10), TRACK_BIT_ALL);
	a->tile = TileXY(11,10);
	a->planned_path.pop_front();
	CHECK_FALSE(ExtendAircraftTaxiReservation(a));
	CHECK(HasAircraftTileReservation(a, TileXY(11,10), TRACK_BIT_X));
	CHECK(HasAircraftTileReservation(b, TileXY(12,10), TRACK_BIT_X));
	ReleaseAircraftTaxiPath(a, false);
	CHECK(GetReservedAirportTracks(TileXY(12,10)) == TRACK_BIT_X);
	ReleaseAircraftTaxiPath(b, false);
}

TEST_CASE("Aircraft rolling reservations - gate claim survives until release")
{
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	MakeReservationAirport();
	Aircraft *a = MakeReservationAircraft(TileXY(10,10));
	Aircraft *b = MakeReservationAircraft(TileXY(11,10));
	TileIndex gate = TileXY(20,10);
	REQUIRE(TryClaimAircraftTerminal(a, gate));
	CHECK_FALSE(TryClaimAircraftTerminal(b, gate));
	CHECK(GetReservedAirportTracks(gate) == TRACK_BIT_NONE); // Claim is not a remote taxi reservation.
	AutoRestoreBackup version(_sl_xv_feature_versions[XSLFI_AIRCRAFT_ROLLING_RESERVATION], uint16_t{9});
	AfterLoadAircraftReservations(false); // New snapshot: rebuild indexes, never change reservations.
	CHECK_FALSE(TryClaimAircraftTerminal(b, gate));
	ReleaseAircraftTerminal(a);
	CHECK(TryClaimAircraftTerminal(b, gate));
	ReleaseAircraftTerminal(b);
}

TEST_CASE("Aircraft stand claims cannot strand virtual routes beyond the stand")
{
	extern bool HandleAircraftMovement(Aircraft *v);
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	MakeReservationAirport(TRACK_BIT_X);
	const TileIndex approach = TileXY(10, 11);
	const TileIndex stand = TileXY(13, 11);
	const TileIndex junction = TileXY(14, 11);
	const TileIndex goal = TileXY(15, 11);
	SetAirportTileType(stand, ATT_APRON_NORMAL);
	SetAirportTileTracks(junction, TRACK_BIT_CROSS);
	SetAirportTileTracks(TileXY(14, 12), TRACK_BIT_Y);
	Aircraft *transit = MakeReservationAircraft(approach);
	Aircraft *parking = MakeReservationAircraft(TileXY(13, 10));
	for (uint x = TileX(approach); x <= TileX(goal); ++x) {
		transit->planned_path.push_back({TileXY(x, TileY(approach)), TRACKDIR_X_SW, false});
	}

	SECTION("a claimed stand prevents booking the junction beyond it") {
		REQUIRE(TryClaimAircraftTerminal(parking, stand));
		CHECK_FALSE(IsAircraftVirtualTileAvailable(transit, stand, TRACKDIR_X_SW));
		CHECK_FALSE(TryReserveAircraftTaxiPath(transit));
		CHECK(transit->virtual_path.empty());
		CHECK(GetAircraftVirtualBookedTracks(junction) == TRACK_BIT_NONE);
		ReleaseAircraftTerminal(parking);
		REQUIRE(TryReserveAircraftTaxiPath(transit));
		CHECK(HasAircraftVirtualTileReservation(transit, junction, TRACKDIR_BIT_X_SW));
	}
	SECTION("an admitted body route prevents a new parking claim") {
		REQUIRE(TryReserveAircraftTaxiPath(transit));
		CHECK(IsAircraftTerminalAvailable(parking, stand)); // Real entry is a different check.
		CHECK_FALSE(CanClaimAircraftTerminal(parking, stand));
		CHECK_FALSE(TryClaimAircraftTerminal(parking, stand));
		ReleaseAircraftTaxiPath(transit, false);
		CHECK(TryClaimAircraftTerminal(parking, stand));
	}
	SECTION("edge-turn sweeps and new stand claims exclude each other") {
		const TileIndex turn = TileXY(12, 11);
		transit->tile = turn;
		transit->planned_path = {{turn, TRACKDIR_X_SW, false}, {turn, TRACKDIR_X_NE, false}};
		REQUIRE(TryClaimAircraftTerminal(parking, stand));
		CHECK_FALSE(TryReserveAircraftTaxiPath(transit));
		CHECK(transit->virtual_path.empty());
		ReleaseAircraftTerminal(parking);

		REQUIRE(TryReserveAircraftTaxiPath(transit));
		CHECK(HasAircraftVirtualTileReservation(transit, stand, TRACKDIR_BIT_X_SW));
		CHECK_FALSE(TryClaimAircraftTerminal(parking, stand));
		ReleaseAircraftTaxiPath(transit, false);
		CHECK(TryClaimAircraftTerminal(parking, stand));
	}
	SECTION("an older sweep is dropped at a safe stop so the parked plane can leave") {
		const TileIndex turn = TileXY(12, 11);
		transit->tile = turn;
		transit->planned_path = {{turn, TRACKDIR_X_SW, false}, {turn, TRACKDIR_X_NE, false}};
		REQUIRE(TryReserveAircraftTaxiPath(transit));
		transit->state = AS_RUNNING;
		transit->reserved_path = {{turn, TRACK_BIT_X}};
		SetAirportTrackReservation(turn, TRACK_X);
		parking->tile = stand;
		parking->state = AS_APRON;
		parking->reserved_path = {{stand, TRACK_BIT_X}};
		SetAirportTrackReservation(stand, TRACK_X);
		parking->allocated_terminal = stand; // State produced by the old permissive claim rule.
		AutoRestoreBackup version(_sl_xv_feature_versions[XSLFI_AIRCRAFT_ROLLING_RESERVATION], uint16_t{9});
		AfterLoadAircraftReservations(false);
		REQUIRE(HasAircraftVirtualTileReservation(transit, stand, TRACKDIR_BIT_X_SW));
		CHECK_FALSE(IsAircraftVirtualTileAvailable(parking, stand, TRACKDIR_X_NE));
		transit->next_trackdir = TRACKDIR_X_NE;
		CHECK_FALSE(ShouldReplanAircraftApronTransit(transit));
		transit->next_trackdir = INVALID_TRACKDIR;
		REQUIRE(ShouldReplanAircraftApronTransit(transit));
		REQUIRE(HandleAircraftMovement(transit));
		CHECK(transit->state == AS_IDLE);
		CHECK(GetAircraftVirtualBookedTracks(stand) == TRACK_BIT_NONE);
		CHECK(IsAircraftVirtualTileAvailable(parking, stand, TRACKDIR_X_NE));
		CHECK(HasAircraftTileReservation(transit, turn, TRACK_BIT_X));
		CHECK(HasAircraftTileReservation(parking, stand, TRACK_BIT_X));
		CHECK(parking->allocated_terminal == stand);
	}
}

TEST_CASE("Aircraft blocked behind another releases an empty remote stand")
{
	extern bool HandleAircraftMovement(Aircraft *v);
	const uint16_t version = GENERATE(uint16_t{5}, uint16_t{6}, uint16_t{7}, uint16_t{8}, uint16_t{9});
	CAPTURE(version);
	AutoRestoreBackup feature(_sl_xv_feature_versions[XSLFI_AIRCRAFT_ROLLING_RESERVATION], version);
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	MakeReservationAirport(TRACK_BIT_X);
	const TileIndex origin = TileXY(10, 11);
	const TileIndex ahead = TileXY(11, 11);
	const TileIndex stand = TileXY(13, 11);
	const TileIndex goal = TileXY(14, 11);
	SetAirportTileType(stand, ATT_APRON_NORMAL);
	Aircraft *rear = MakeReservationAircraft(origin);
	Aircraft *front = MakeReservationAircraft(ahead);
	REQUIRE(TryReserveAircraftTile(rear, origin, TRACK_BIT_X));
	REQUIRE(TryReserveAircraftTile(front, ahead, TRACK_BIT_X));
	for (uint x = TileX(origin); x <= TileX(goal); ++x) rear->planned_path.push_back({TileXY(x, 11), TRACKDIR_X_SW, false});
	REQUIRE(TryReserveAircraftTaxiPath(rear));
	rear->state = AS_RUNNING;
	rear->x_pos = TileX(origin) * TILE_SIZE + 7;
	rear->y_pos = TileY(origin) * TILE_SIZE + 8;
	rear->UpdateNextTile(goal);

	AfterLoadAircraftReservations(false);
	CHECK(rear->junction_legacy == (version < 9));
	REQUIRE(HasAircraftVirtualTileReservation(rear, stand, TRACKDIR_BIT_X_SW));
	CHECK(IsAircraftTerminalAvailable(front, stand));
	CHECK_FALSE(CanClaimAircraftTerminal(front, stand));
	CHECK_FALSE(ShouldReplanAircraftApronTransit(rear));
	for (int i = 0; i < 64 && !ShouldReplanAircraftApronTransit(rear); ++i) REQUIRE(HandleAircraftMovement(rear));
	REQUIRE(ShouldReplanAircraftApronTransit(rear));
	rear->next_trackdir = TRACKDIR_X_NE;
	CHECK_FALSE(ShouldReplanAircraftApronTransit(rear));
	rear->next_trackdir = INVALID_TRACKDIR;
	SetBit(rear->flags, VAF_PUSHBACK);
	CHECK_FALSE(ShouldReplanAircraftApronTransit(rear));
	ClrBit(rear->flags, VAF_PUSHBACK);

	REQUIRE(HandleAircraftMovement(rear));
	CHECK(rear->state == AS_IDLE);
	CHECK(rear->IsWaiting());
	CHECK_FALSE(rear->junction_legacy);
	CHECK(rear->planned_path.empty());
	CHECK(GetAircraftVirtualBookedTracks(goal) == TRACK_BIT_NONE);
	CHECK(GetAircraftVirtualBookedTracks(stand) == TRACK_BIT_NONE);
	CHECK(HasAircraftTileReservation(rear, origin, TRACK_BIT_X));
	CHECK(HasAircraftTileReservation(front, ahead, TRACK_BIT_X));
	CHECK(TryClaimAircraftTerminal(front, stand));
}

TEST_CASE("Aircraft rolling reservations - turns reserve complete tile groups")
{
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	MakeReservationAirport();
	Aircraft *a = MakeReservationAircraft(TileXY(10,10));
	a->planned_path = {{TileXY(10,10),TRACKDIR_X_SW,false}, {TileXY(11,10),TRACKDIR_X_SW,false},
		{TileXY(11,10),TRACKDIR_Y_SE,true}, {TileXY(11,11),TRACKDIR_Y_SE,false}};
	CHECK_FALSE(ExtendAircraftTaxiReservation(a));
	REQUIRE(TryReserveAircraftTaxiPath(a));
	REQUIRE(ExtendAircraftTaxiReservation(a));
	CHECK(GetReservedAirportTracks(TileXY(11,10)) == TRACK_BIT_CROSS);
	CHECK(GetReservedAirportTracks(TileXY(11,11)) == TRACK_BIT_NONE);
	const auto original = a->planned_path;
	const auto occupancy = GetReservedAirportTracks(TileXY(11,10));
	AutoRestoreBackup version(_sl_xv_feature_versions[XSLFI_AIRCRAFT_ROLLING_RESERVATION], uint16_t{9});
	AfterLoadAircraftReservations(false);
	CHECK(a->planned_path.size() == original.size());
	CHECK(GetReservedAirportTracks(TileXY(11,10)) == occupancy);
	CHECK(ExtendAircraftTaxiReservation(a));
	ReleaseAircraftTaxiPath(a, false);
}

TEST_CASE("Aircraft junction access distinguishes swept half-tracks from body tracks")
{
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	MakeReservationAirport();
	const TileIndex origin = TileXY(13, 12);
	const TileIndex junction = TileAddByDir(origin, Direction::W);
	SetAirportTileTracks(origin, TRACK_BIT_LEFT);
	SetAirportTileTracks(junction, TRACK_BIT_CROSS | TRACK_BIT_RIGHT);
	Aircraft *a = MakeReservationAircraft(origin);
	Aircraft *b = MakeReservationAircraft(junction);

	SECTION("an unrelated diagonal grant does not block the adjacent sweep") {
		REQUIRE(TryReserveAircraftTile(b, junction, TRACK_BIT_Y));
		REQUIRE_FALSE(b->junction_actions.empty());
		CHECK(TryReserveAircraftTile(a, origin, TRACK_BIT_LEFT));
		CHECK(HasAircraftTileReservation(a, origin, TRACK_BIT_LEFT));
		CHECK(HasAircraftTileReservation(b, junction, TRACK_BIT_Y));
		CHECK_FALSE(TryReserveAircraftTile(a, junction, TRACK_BIT_X)); // Body tracks still intersect.
	}
	SECTION("the facing half-track still blocks the adjacent sweep") {
		REQUIRE(TryReserveAircraftTile(b, junction, TRACK_BIT_RIGHT));
		REQUIRE_FALSE(b->junction_actions.empty());
		CHECK_FALSE(TryReserveAircraftTile(a, origin, TRACK_BIT_LEFT));
		CHECK_FALSE(HasAircraftTileReservation(a, origin, TRACK_BIT_LEFT));
	}
}

TEST_CASE("Aircraft reserves consecutive junctions before entering the first")
{
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	MakeReservationAirport(TRACK_BIT_X);
	const TileIndex first = TileXY(11, 11);
	const auto build_junction = [](uint x) {
		SetAirportTileTracks(TileXY(x, 11), TRACK_BIT_CROSS);
		SetAirportTileTracks(TileXY(x, 10), TRACK_BIT_Y);
		SetAirportTileTracks(TileXY(x, 12), TRACK_BIT_Y);
	};
	build_junction(11);
	Aircraft *a = MakeReservationAircraft(TileXY(10, 11));
	for (uint x = 10; x <= 16; ++x) a->planned_path.push_back({TileXY(x, 11), TRACKDIR_X_SW, false});

	SECTION("the second junction is part of the first junction's exit") {
		build_junction(12);
		/* Occupy only the second junction's exit, not any tile of the first action. */
		Aircraft *b = MakeReservationAircraft(TileXY(13, 11));
		REQUIRE(TryReserveAircraftTile(b, b->tile, TRACK_BIT_X));
		REQUIRE(TryReserveAircraftTaxiPath(a));
		CHECK_FALSE(ExtendAircraftTaxiReservation(a));
		CHECK(a->junction_actions.empty());
		CHECK(a->reserved_path.empty());
		CHECK(a->junction_requests.size() == 2);
		CHECK(HasAircraftTileReservation(b, b->tile, TRACK_BIT_X));

		ReleaseAircraftTaxiPath(b, false);
		REQUIRE(ExtendAircraftTaxiReservation(a));
		REQUIRE(a->junction_actions.size() == 2);
		CHECK(a->junction_actions[0].junction == first);
		CHECK(a->junction_actions[1].junction == TileXY(12, 11));
		CHECK(a->junction_requests.empty());
		CHECK(a->reserved_path.size() == 2);
		CHECK(GetReservedAirportTracks(TileXY(12, 11)) == TRACK_BIT_NONE);
	}
	SECTION("a separate waiting tile keeps later junctions independent") {
		build_junction(13);
		Aircraft *b = MakeReservationAircraft(TileXY(14, 11));
		REQUIRE(TryReserveAircraftTile(b, b->tile, TRACK_BIT_X));
		REQUIRE(TryReserveAircraftTaxiPath(a));
		REQUIRE(ExtendAircraftTaxiReservation(a));
		REQUIRE(a->junction_actions.size() == 1);
		CHECK(a->junction_actions.front().junction == first);
		CHECK(HasAircraftTileReservation(b, b->tile, TRACK_BIT_X));
	}
}

TEST_CASE("Aircraft rolling reservations - old load discards orphan taxi reservations")
{
	REQUIRE(Engine::GetNumItems() == 0);
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _engine_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	Station *airport = MakeReservationAirport(TRACK_BIT_X);
	Aircraft *a = MakeReservationAircraft(TileXY(10, 10));
	/* Legacy migration tries to replan idle aircraft, which requires an engine
	 * even when this airport has no reachable gate. */
	a->engine_type = a->Next()->engine_type = Engine::Create(VehicleType::Aircraft, 0)->index;
	a->targetairport = airport->index;
	a->state = AS_RUNNING;
	SetAirportTrackReservation(TileXY(10,10), TRACK_X);
	SetAirportTrackReservation(TileXY(11,10), TRACK_X);
	SetAirportTrackReservation(TileXY(12,10), TRACK_X);
	AutoRestoreBackup version(_sl_xv_feature_versions[XSLFI_AIRCRAFT_ROLLING_RESERVATION], uint16_t{0});
	AfterLoadAircraftReservations(true);
	CHECK(a->state == AS_IDLE);
	CHECK(a->reserved_path.size() == 1);
	CHECK(HasAircraftTileReservation(a, a->tile, TRACK_BIT_X));
	CHECK(GetReservedAirportTracks(TileXY(10,10)) == TRACK_BIT_X);
	CHECK(GetReservedAirportTracks(TileXY(11,10)) == TRACK_BIT_NONE);
	CHECK(GetReservedAirportTracks(TileXY(12,10)) == TRACK_BIT_NONE);
	ReleaseAircraftTaxiPath(a, false);
}

TEST_CASE("Aircraft taxi phasing never bypasses a real owner")
{
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	MakeReservationAirport();
	Aircraft *a = MakeReservationAircraft(TileXY(10, 10));
	Aircraft *b = MakeReservationAircraft(TileXY(11, 10));
	REQUIRE(TryReserveAircraftTile(b, b->tile, TRACK_BIT_X));
	SetBit(a->flags, VAF_TAXI_PHASING);
	CHECK_FALSE(TryReserveAircraftTile(a, b->tile, TRACK_BIT_X));
	SetBit(b->flags, VAF_TAXI_PHASING);
	CHECK_FALSE(IsAircraftTaxiEnvelopeAvailable(a, b->tile, TRACK_BIT_X));
	CHECK_FALSE(TryReserveAircraftTile(a, b->tile, TRACK_BIT_X));
	b->vehstatus.Set(VehState::Stopped);
	StopAircraftTaxiTraffic(b);
	CHECK_FALSE(HasBit(b->flags, VAF_TAXI_PHASING));
	CHECK(HasAircraftTileReservation(b, b->tile, TRACK_BIT_X));
	CHECK_FALSE(TryReserveAircraftTile(a, b->tile, TRACK_BIT_X));
}

TEST_CASE("Aircraft stale grants expire without losing physical protection or pending FIFO")
{
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	MakeReservationAirport();
	Aircraft *a = MakeReservationAircraft(TileXY(10, 10));
	const TileIndex ahead = TileXY(11, 10);
	a->planned_path = {{a->tile, TRACKDIR_X_SW, false}, {ahead, TRACKDIR_X_SW, false}};
	REQUIRE(TryReserveAircraftTaxiPath(a));
	REQUIRE(ExtendAircraftTaxiReservation(a));
	a->state = AS_RUNNING;
	const AircraftJunctionAction action{ahead, a->tile, ahead, TRACKDIR_X_SW, TRACKDIR_X_SW, false, {ahead}, {TRACK_BIT_X}, {TRACK_BIT_NONE}};
	a->junction_actions = {action};
	a->junction_requests = {action};
	a->junction_request_tick = StateTicks{42};
	a->taxi_jam_ticks = 8191;
	a->taxi_block_reason = AircraftTaxiBlock::Traffic;
	UpdateAircraftTaxiTraffic(a, false);
	CHECK(a->junction_actions.empty());
	CHECK(a->junction_requests == std::vector{action});
	CHECK(a->junction_request_tick == StateTicks{42});
	CHECK(a->taxi_jam_ticks == 0);
	CHECK(HasAircraftTileReservation(a, a->tile, TRACK_BIT_X));
	CHECK(GetReservedAirportTracks(ahead) == TRACK_BIT_NONE);
	CHECK(HasAircraftVirtualTileReservation(a, ahead, TRACKDIR_BIT_X_SW));
}

TEST_CASE("Aircraft yields a mutual junction lock only when real occupancy permits it")
{
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	MakeReservationAirport(TRACK_BIT_X);
	Aircraft *a = MakeReservationAircraft(TileXY(10, 11));
	Aircraft *b = MakeReservationAircraft(TileXY(16, 11));
	REQUIRE(TryReserveAircraftTile(a, a->tile, TRACK_BIT_X));
	REQUIRE(TryReserveAircraftTile(b, b->tile, TRACK_BIT_X));
	const auto make_action = [](TileIndex first, TileIndex last) {
		AircraftJunctionAction action{first, first, last, TRACKDIR_X_SW, TRACKDIR_X_SW, true, {first}, {TRACK_BIT_X}, {TRACK_BIT_NONE}};
		if (last != first) {
			action.tiles.push_back(last);
			action.tracks.push_back(TRACK_BIT_X);
			action.swept.push_back(TRACK_BIT_NONE);
		}
		return action;
	};
	const TileIndex a_exit = TileXY(11, 11);
	const TileIndex b_entry = TileXY(15, 11);
	a->junction_actions = {make_action(a->tile, a_exit)};
	b->junction_actions = {make_action(b_entry, b->tile)};
	b->junction_requests = {make_action(a_exit, a_exit)};
	a->junction_request_tick = StateTicks{10};
	b->junction_request_tick = StateTicks{20};
	bool clear = true;
	SECTION("only a soft claim blocks the older request") { a->junction_requests = {make_action(b_entry, b_entry)}; }
	SECTION("the older request also conflicts with the parked aircraft") {
		a->junction_requests = {make_action(b_entry, b->tile)};
		clear = false;
	}
	AutoRestoreBackup version(_sl_xv_feature_versions[XSLFI_AIRCRAFT_ROLLING_RESERVATION], uint16_t{9});
	AfterLoadAircraftReservations(false);
	a->taxi_block_reason = b->taxi_block_reason = AircraftTaxiBlock::Protected;
	UpdateAircraftTaxiTraffic(a, false);
	CHECK(a->junction_actions.size() == 1);
	UpdateAircraftTaxiTraffic(b, false);
	CHECK(b->junction_actions.empty() == clear);
	CHECK(HasAircraftTileReservation(b, b->tile, TRACK_BIT_X));
	CHECK(b->junction_request_tick == StateTicks{20});
}

TEST_CASE("Aircraft runway-head admission uses the runway axis without a built taxi track")
{
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	MakeReservationAirport();
	const TileIndex head = TileXY(12, 11);
	const TileIndex end = TileXY(13, 11);
	SetAirportTileType(head, ATT_RUNWAY_START_ALLOW_LANDING);
	SetAirportTileType(end, ATT_RUNWAY_END);
	SetRunwayExtremeDirection(head, DiagDirection::SW);
	SetRunwayExtremeDirection(end, DiagDirection::SW);
	/* A taxi junction on the head, but without a taxi track on the runway axis. */
	SetAirportTileTracks(head, TRACK_BIT_ALL & ~TRACK_BIT_X);
	Aircraft *waiting = MakeReservationAircraft(TileXY(10, 10));
	Aircraft *a = MakeReservationAircraft(TileXY(16, 11));
	const AircraftJunctionAction action{head, head, head, TRACKDIR_X_SW, TRACKDIR_X_SW, false, {head}, {TRACK_BIT_X}, {TRACK_BIT_NONE}};
	AutoRestoreBackup version(_sl_xv_feature_versions[XSLFI_AIRCRAFT_ROLLING_RESERVATION], uint16_t{9});

	SECTION("the saved FIFO request still blocks newer admission") {
		waiting->junction_requests = {action};
		waiting->junction_request_tick = StateTicks{0};
		/* Equal timestamps are broken by vehicle ID (waiting was allocated first). */
		AutoRestoreBackup ticks(_state_ticks, StateTicks{0});
		AfterLoadAircraftReservations(false);
		CHECK(waiting->junction_request_tick == StateTicks{0});
		REQUIRE_FALSE(TryReserveAircraftRunway(a, head));
		CHECK(a->junction_requests == std::vector{action});
		StopAircraftTaxiTraffic(waiting);
		CHECK_FALSE(TryReserveAircraftTile(a, head, TRACK_BIT_X));
		REQUIRE(TryReserveAircraftRunway(a, head));
		CHECK(GetReservationAsRunway(head));
		CHECK(GetReservationAsRunway(end));
	}
	SECTION("real occupancy still blocks the runway") {
		REQUIRE(TryReserveAircraftTile(waiting, head, TRACK_BIT_Y));
		CHECK_FALSE(TryReserveAircraftRunway(a, head));
		CHECK(HasAircraftTileReservation(waiting, head, TRACK_BIT_Y));
	}
	SECTION("a saved taxi traversal cannot use the exception for an unbuilt track") {
		auto taxi = action;
		taxi.exit_tile = end;
		taxi.tiles.push_back(end);
		taxi.tracks.push_back(TRACK_BIT_X);
		taxi.swept.push_back(TRACK_BIT_NONE);
		waiting->junction_actions = {taxi};
		CHECK_THROWS(AfterLoadAircraftReservations(false));
	}
}

TEST_CASE("Aircraft outside the map can still select a landing tile")
{
	REQUIRE(Engine::GetNumItems() == 0);
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _engine_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	Station *st = MakeReservationAirport();
	const TileIndex apron = TileXY(12, 10);
	SetAirportTileType(apron, ATT_APRON_NORMAL);
	st->airport.aprons.push_back(apron);
	Engine *engine = Engine::Create(VehicleType::Aircraft, 0);
	engine->VehInfo<AircraftVehicleInfo>().subtype = AIR_HELICOPTER;
	Aircraft *v = MakeReservationAircraft(apron);
	v->subtype = AIR_HELICOPTER;
	v->engine_type = v->Next()->engine_type = engine->index;
	v->current_order.MakeGoToStation(st->index);
	v->state = AS_ON_HOLD_WAITING;
	v->x_pos = 160;
	v->y_pos = -24;
	v->tile = TileVirtXY(v->x_pos, v->y_pos);
	REQUIRE_FALSE(IsValidTile(v->tile));
	REQUIRE(DistanceSquare(apron, v->tile) == UINT_MAX);
	CHECK(FindClosestLandingTile(v) == apron);
	CHECK(v->targetairport == st->index);
}

TEST_CASE("A holding helicopter selects a claimable alternate stand")
{
	extern void HandleAircraftFlying(Aircraft *v);
	REQUIRE(Engine::GetNumItems() == 0);
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _engine_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	Station *st = MakeReservationAirport(TRACK_BIT_X);
	const TileIndex near = TileXY(11, 10);
	const TileIndex far = TileXY(15, 10);
	SetAirportTileType(near, ATT_APRON_HELIPAD);
	SetAirportTileType(far, ATT_APRON_HELIPAD);
	st->airport.helipads = {near, far};
	Aircraft *transit = MakeReservationAircraft(TileXY(10, 10));
	transit->planned_path = {{transit->tile, TRACKDIR_X_SW, false}, {near, TRACKDIR_X_SW, false}, {TileXY(12, 10), TRACKDIR_X_SW, false}};
	REQUIRE(TryReserveAircraftTaxiPath(transit));

	Engine *engine = Engine::Create(VehicleType::Aircraft, 0);
	engine->VehInfo<AircraftVehicleInfo>().subtype = AIR_HELICOPTER;
	Aircraft *heli = MakeReservationAircraft(near);
	heli->subtype = AIR_HELICOPTER;
	heli->engine_type = heli->Next()->engine_type = engine->index;
	heli->current_order.MakeGoToStation(st->index);
	heli->state = AS_ON_HOLD_WAITING;
	heli->x_pos = TileX(near) * TILE_SIZE + 8;
	heli->y_pos = TileY(near) * TILE_SIZE + 8;
	heli->targetairport = st->index;
	CHECK(IsAircraftGroundTileAvailable(heli, near));
	CHECK_FALSE(CanClaimAircraftTerminal(heli, near));
	CHECK(FindClosestLandingTile(heli) == far);
	HandleAircraftFlying(heli);
	CHECK(heli->state == AS_ON_HOLD_APPROACHING);
	CHECK(heli->GetNextTile() == far);
	CHECK(heli->allocated_terminal == far);
	CHECK(HasAircraftTileReservation(heli, far, TRACK_BIT_X));
	CHECK(HasAircraftVirtualTileReservation(transit, near, TRACKDIR_BIT_X_SW));
}

TEST_CASE("Aircraft without a destination repairs an invalid holding anchor and keeps moving")
{
	extern bool HandleAircraftMovement(Aircraft *v);
	extern void HandleAircraftFlying(Aircraft *v);
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	Aircraft *v = MakeReservationAircraft(INVALID_TILE);
	v->x_pos = 160;
	v->y_pos = -24;
	v->tile = TileVirtXY(v->x_pos, v->y_pos);
	SECTION("losing the target outside the map") {
		v->state = AS_ON_HOLD_WAITING;
		AssignLandingTile(v, INVALID_TILE);
	}
	SECTION("loading a stuck flight") {
		v->state = AS_FLYING_NO_DEST;
		v->targetairport = StationID::Invalid();
		v->UpdateNextTile(INVALID_TILE);
		REQUIRE(HandleAircraftMovement(v));
	}
	CHECK(v->state == AS_FLYING_NO_DEST);
	const TileIndex anchor = v->GetNextTile();
	REQUIRE(IsValidTile(anchor));
	CHECK(TileY(anchor) == 1);
	CHECK((v->next_pos.x != v->x_pos || v->next_pos.y != v->y_pos));
	v->x_pos = v->next_pos.x;
	v->y_pos = v->next_pos.y;
	HandleAircraftFlying(v);
	CHECK(v->GetNextTile() == anchor);
	CHECK((v->next_pos.x != v->x_pos || v->next_pos.y != v->y_pos));
}

TEST_CASE("External aircraft keeps its holding waypoint on a failed retry")
{
	extern void HandleAircraftFlying(Aircraft *v);
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	Aircraft *v = MakeReservationAircraft(INVALID_TILE);
	v->state = AS_FLYING;
	v->external_landing = true;
	v->x_pos = 160;
	v->y_pos = -24;
	AssignLandingTile(v, INVALID_TILE);
	const TileIndex anchor = v->GetNextTile();
	v->x_pos = v->next_pos.x;
	v->y_pos = v->next_pos.y;
	HandleAircraftFlying(v);
	const auto next = v->next_pos;
	REQUIRE((next.x != v->x_pos || next.y != v->y_pos));
	AircraftUpdateNextPos(v); // The deterministic retry's destination refresh.
	CHECK(v->state == AS_FLYING_NO_DEST);
	CHECK(v->GetNextTile() == anchor);
	CHECK(v->next_pos.pos == next.pos);
	CHECK(v->next_pos.x == next.x);
	CHECK(v->next_pos.y == next.y);
}

TEST_CASE("External aircraft picks a fallback with a reachable landing exit")
{
	REQUIRE(Engine::GetNumItems() == 0);
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _engine_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	const auto make_airport = [](uint x, bool connected) {
		Station *st = MakeReservationAirport(TRACK_BIT_NONE, x, 10);
		st->owner = OWNER_BEGIN;
		const TileIndex start = TileXY(x, 10);
		const TileIndex end = TileXY(x + 3, 10);
		const TileIndex apron = TileXY(x + 4, 10);
		SetAirportTileType(start, ATT_RUNWAY_START_ALLOW_LANDING);
		SetAirportTileType(end, ATT_RUNWAY_END);
		SetRunwayExtremeDirection(start, DiagDirection::SW);
		SetRunwayExtremeDirection(end, DiagDirection::SW);
		for (uint i = 1; i < 3; ++i) {
			const TileIndex middle = TileXY(x + i, 10);
			SetAirportTileType(middle, ATT_RUNWAY_MIDDLE);
			AddPlainRunwayDirections(middle, DiagDirection::SW, true);
		}
		SetAirportTileTracks(end, TRACK_BIT_X);
		SetAirportTileType(apron, ATT_APRON_NORMAL);
		SetAirportTileTracks(apron, connected ? TRACK_BIT_X : TRACK_BIT_Y);
		st->airport.runways.push_back(start);
		st->airport.aprons.push_back(apron);
		SetBit(st->airport.flags, AFB_LANDING_RUNWAY);
		return st;
	};
	Station *near = make_airport(10, true);
	Station *far = make_airport(32, true);
	Aircraft *v = MakeReservationAircraft(near->airport.tile);
	Engine *engine = Engine::Create(VehicleType::Aircraft, 0);
	engine->VehInfo<AircraftVehicleInfo>().subtype = AIR_CTOL;
	v->engine_type = v->Next()->engine_type = engine->index;
	v->owner = OWNER_BEGIN;
	v->external_landing = true;
	v->external_return_airport = v->external_home_airport = near->index;
	v->external_return_airport_tile = v->external_home_tile = near->airport.tile;
	v->targetairport = near->index;
	v->current_order.MakeGoToStation(near->index);
	v->state = AS_FLYING;
	v->UpdateNextTile(near->airport.tile);
	REQUIRE(FindClosestLandingTile(v) == near->airport.tile);

	/* The start tile and length did not change, but the edit cut its taxi exit. */
	SetAirportTileTracks(near->airport.aprons.front(), TRACK_BIT_Y);
	AircraftUpdateNextPos(v);
	CHECK(v->state == AS_FLYING);
	CHECK(v->GetNextTile() == far->airport.tile);
	CHECK(v->targetairport == far->index);
	CHECK(v->current_order.GetDestination().ToStationID() == far->index);
}

TEST_CASE("External aircraft resumes its schedule after return hangar service")
{
	extern bool HandleAircraftState(Aircraft *v);
	REQUIRE(Engine::GetNumItems() == 0);
	AutoRestoreBackup headless(_network_dedicated, true);
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] {
		ClearAircraftReservationIndexes();
		_vehicle_pool.CleanPool();
		_orderlist_pool.CleanPool();
		_engine_pool.CleanPool();
		_station_pool.CleanPool();
		_depot_pool.CleanPool();
		DeallocateMap();
	});
	Station *st = MakeReservationAirport(TRACK_BIT_X);
	st->owner = OWNER_BEGIN;
	const TileIndex hangar = st->airport.tile;
	const TileIndex apron = TileXY(13, 10);
	SetAirportTileType(hangar, ATT_HANGAR_STANDARD);
	SetHangarDirection(hangar, DiagDirection::SW);
	st->airport.hangar = Depot::Create(hangar);
	SetAirportTileType(apron, ATT_APRON_NORMAL);
	st->airport.aprons.push_back(apron);
	SetBit(st->airport.flags, AFB_LANDING_RUNWAY);

	Aircraft *v = MakeReservationAircraft(hangar);
	v->sprite_seq.Clear(); // No sprites are loaded by the unit test runner.
	AutoRestoreBackup<uint16_t> runway_length(_settings_game.vehicle.plane_runway_length_factor, 0);
	Engine *engine = Engine::Create(VehicleType::Aircraft, 0);
	engine->VehInfo<AircraftVehicleInfo>().subtype = AIR_CTOL;
	v->engine_type = v->Next()->engine_type = engine->index;
	v->owner = OWNER_BEGIN;
	v->x_pos = v->next_pos.x;
	v->y_pos = v->next_pos.y;
	v->state = AS_HANGAR;
	v->external_landing = true;
	Order external;
	external.MakeExternalFlight();
	v->orders = OrderList::Create(std::move(external), v);
	v->primary_order = v->orders->index;
	Order next;
	next.MakeGoToStation(st->index);
	v->orders->InsertOrderAt(std::move(next), 1);
	/* The off-map trip already advanced the schedule to the native stop. */
	v->cur_real_order_index = v->cur_implicit_order_index = 1;

	SECTION("native depot entry consumes the temporary service order") {
		v->targetairport = st->index;
		v->trackdir = TRACKDIR_X_NE; // Arrive through the door, opposite the exit direction.
		v->current_order.MakeGoToDepot(st->index, {});
		REQUIRE(HandleAircraftState(v));
		REQUIRE(v->vehstatus.Test(VehState::Hidden));
		REQUIRE(v->current_order.IsType(OT_DUMMY));
		/* The vehicle loop releases the temporary autoreplace stop. */
		v->vehstatus.Reset(VehState::Stopped);
	}
	SECTION("a snapshot already contains the completed service order") {
		v->vehstatus.Set(VehState::Hidden);
		v->current_order.MakeDummy();
		v->UpdateNextTile(INVALID_TILE);
		AutoRestoreBackup version(_sl_xv_feature_versions[XSLFI_AIRCRAFT_ROLLING_RESERVATION], uint16_t{9});
		AfterLoadAircraftReservations(false);
	}
	SECTION("an apron is still occupied and the return order is live") {
		v->targetairport = st->index;
		v->vehstatus.Set(VehState::Hidden);
		v->current_order.MakeGoToStation(st->index);
		v->UpdateNextTile(INVALID_TILE);
		Aircraft *blocker = MakeReservationAircraft(apron);
		REQUIRE(TryClaimAircraftTerminal(blocker, apron));
		REQUIRE(v->Tick());
		CHECK(v->external_landing);
		CHECK(v->vehstatus.Test(VehState::Hidden));
		CHECK(v->current_order.IsType(OT_GOTO_STATION));
		CHECK(v->cur_real_order_index == 1);
		return;
	}

	REQUIRE(v->Tick());
	CHECK_FALSE(v->external_landing);
	CHECK(v->current_order.IsType(OT_GOTO_STATION));
	CHECK(v->current_order.GetDestination().ToStationID() == st->index);
	CHECK(v->cur_real_order_index == 1);
	CHECK_FALSE(v->vehstatus.Test(VehState::Hidden));
	CHECK(v->state == AS_RUNNING);
	CHECK(v->GetNextTile() == apron);
}

TEST_CASE("Aircraft hangars - construction and occupancy use the physical hangar")
{
	extern bool HandleAircraftState(Aircraft *v);
	extern bool TryReservePath(Aircraft *v);
	extern CargoType _cargo_id_passengers;
	REQUIRE(Engine::GetNumItems() == 0);
	REQUIRE(Company::GetNumItems() == 0);
	AutoRestoreBackup headless(_network_dedicated, true);
	AutoRestoreBackup current_company(_current_company, OWNER_BEGIN);
	AutoRestoreBackup passenger_cargo(_cargo_id_passengers, CargoType{0});
	SavedRandomSeeds saved_seeds;
	SaveRandomSeeds(&saved_seeds);
	AllocateMap(64, 64);
	ResetVehicleHash();
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([&] {
		ClearAircraftReservationIndexes();
		_vehicle_pool.CleanPool();
		ResetVehicleHash();
		_engine_pool.CleanPool();
		_station_pool.CleanPool();
		_depot_pool.CleanPool();
		_company_pool.CleanPool();
		DeallocateMap();
		RestoreRandomSeeds(saved_seeds);
	});
	REQUIRE(Company::Create()->index == _current_company);
	Station *st = MakeReservationAirport(TRACK_BIT_X);
	st->owner = _current_company;
	st->airport.type = AT_CUSTOM;
	st->rect.BeforeAddRect(st->airport.tile, st->airport.w, st->airport.h, StationRect::ADD_FORCE);
	const TileIndex first = TileXY(10, 10);
	const TileIndex second = TileXY(10, 12);
	const TileIndex apron = TileXY(13, 10);
	for (TileIndex hangar : {first, second}) {
		SetAirportTileType(hangar, ATT_HANGAR_STANDARD);
		SetHangarDirection(hangar, DiagDirection::SW);
	}
	SetAirportTileType(apron, ATT_APRON_NORMAL);
	st->UpdateAirportDataStructure();
	REQUIRE(st->airport.HasHangar());
	REQUIRE(st->airport.hangar->xy == second);
	SetBit(st->airport.flags, AFB_LANDING_RUNWAY);
	CHECK(GetDepotDestinationIndex(first) == st->index);
	CHECK(GetDepotDestinationIndex(second) == st->index);

	Engine *engine = Engine::Create(VehicleType::Aircraft, 0);
	engine->info.cargo_type = CargoType{0};
	engine->VehInfo<AircraftVehicleInfo>().subtype = AIR_CTOL;
	Vehicle *a = nullptr;
	Vehicle *b = nullptr;

	SECTION("an occupied extended hangar rejects builds and refit quotes without side effects") {
		SetExtendedHangar(first, true);
		engine->company_avail.Set(_current_company);
		REQUIRE(IsEngineBuildable(engine->index, VehicleType::Aircraft, _current_company));
		REQUIRE(CmdBuildAircraft(first, DoCommandFlag::QueryCost, engine, &a).Succeeded());
		CHECK(a == nullptr);
		CHECK(Aircraft::GetNumItems() == 0);
		CHECK(GetReservedAirportTracks(first) == TRACK_BIT_NONE);
		auto *occupant = MakeReservationAircraft(first);
		occupant->owner = _current_company;
		occupant->state = AS_HANGAR;
		a = occupant;
		REQUIRE(TryReserveAircraftTile(occupant, first, TRACK_BIT_X, TRACKDIR_BIT_X_SW));
		const auto count = Aircraft::GetNumItems();
		SavedRandomSeeds before, after;
		SaveRandomSeeds(&before);
		CHECK(CmdBuildAircraft(first, DoCommandFlag::QueryCost, engine, &b).GetErrorMessage() == STR_ERROR_AIRPORT_PRESENT_AIRCRAFT);
		CHECK(CmdBuildAircraft(first, DoCommandFlag::Execute, engine, &b).GetErrorMessage() == STR_ERROR_AIRPORT_PRESENT_AIRCRAFT);
		CHECK(b == nullptr);
		CHECK(CmdBuildVehicle(DoCommandFlag::QueryCost, first, engine->index, false, CargoType{1}, ClientID::Invalid).GetErrorMessage() == STR_ERROR_AIRPORT_PRESENT_AIRCRAFT);
		SaveRandomSeeds(&after);
		CHECK(after.random.state[0] == before.random.state[0]);
		CHECK(after.random.state[1] == before.random.state[1]);
		CHECK(Aircraft::GetNumItems() == count);
		CHECK(HasAircraftTileReservation(Aircraft::From(a), first, TRACK_BIT_X));
		CHECK(GetReservedAirportTracks(second) == TRACK_BIT_NONE);
		return;
	}

	SECTION("removal cannot change the use of an extended hangar with a hidden aircraft") {
		REQUIRE(CmdBuildAircraft(first, DoCommandFlag::Execute, engine, &a).Succeeded());
		REQUIRE(a->vehstatus.Test(VehState::Hidden));
		SetExtendedHangar(first, true); // A hidden aircraft restored inside an extended hangar.
		REQUIRE(GetReservedAirportTracks(first) == TRACK_BIT_NONE);
		AutoRestoreBackup modify(_settings_game.station.allow_modify_airports, true);
		const AirType air_type = GetAirType(first);
		CHECK(CmdChangeAirportTiles({}, first, first, air_type, ATT_HANGAR_EXTENDED, ATTG_DEFAULT_GFX, DiagDirection::SW, false, false).GetErrorMessage() == STR_ERROR_AIRCRAFT_IN_THE_WAY);
		CHECK(CmdAddRemoveAirportTiles({}, first, first, false, air_type, StationID::Invalid(), false).Failed());
		return;
	}

	SECTION("standard hangars keep independent aircraft lists") {
		/* Stale depot UI actions must not read station fields from a non-hangar. */
		ShowDepotDeparturesWindow(INVALID_TILE, VehicleType::Aircraft);
		ShowDepotDeparturesWindow(apron, VehicleType::Aircraft);
		ShowDepotDeparturesWindow(first, VehicleType::Train);

		REQUIRE(CmdBuildAircraft(first, DoCommandFlag::Execute, engine, &a).Succeeded());
		REQUIRE(a->tile == first);
		REQUIRE(CmdBuildAircraft(second, DoCommandFlag::Execute, engine, &b).Succeeded());
		REQUIRE(b->tile == second);

		VehicleList list;
		const auto check_hangar = [&](TileIndex tile, const Vehicle *occupant) {
			BuildDepotVehicleList(VehicleType::Aircraft, tile, &list, nullptr);
			if (occupant == nullptr) {
				CHECK(list.empty());
			} else {
				REQUIRE(list.size() == 1);
				CHECK(list.front() == occupant);
			}
		};
		check_hangar(first, a);
		check_hangar(second, b);

		Aircraft *v = Aircraft::From(a);
		v->sprite_seq.Clear(); // No sprites are loaded by the unit test runner.
		v->vehstatus.Reset({VehState::Stopped, VehState::Hidden});
		v->state = AS_RUNNING;
		v->trackdir = TRACKDIR_X_NE; // Approach the door of the non-representative hangar.
		v->dest_tile = second;
		v->current_order.MakeGoToDepot(st->index, {});
		v->current_order.SetDepotExtraFlags(OrderDepotExtraFlag::Specific);
		v->UpdateNextTile(first);
		check_hangar(first, nullptr);
		REQUIRE(HandleAircraftState(v));
		REQUIRE(v->vehstatus.Test(VehState::Hidden));
		CHECK(v->current_order.IsType(OT_DUMMY));
		check_hangar(first, v);
		check_hangar(second, b);

		v->vehstatus.Reset(VehState::Stopped);
		v->current_order.MakeGoToStation(st->index);
		REQUIRE(TryReservePath(v));
		CHECK(v->GetNextTile() == apron);
		CHECK_FALSE(v->vehstatus.Test(VehState::Hidden));
		check_hangar(first, nullptr);
		check_hangar(second, b);
	}
}

TEST_CASE("Aircraft runway-end reachability is static and requires a connected exit")
{
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	Station *st = MakeReservationAirport();
	for (TileIndex tile : st->airport) SetAirportTileTracks(tile, TRACK_BIT_X);
	const TileIndex start = TileXY(10, 10);
	const TileIndex end = TileXY(11, 10);
	const TileIndex apron = TileXY(13, 11);
	SetAirportTileType(start, ATT_RUNWAY_START_ALLOW_LANDING);
	SetAirportTileType(end, ATT_RUNWAY_END);
	SetRunwayExtremeDirection(start, DiagDirection::SW);
	SetRunwayExtremeDirection(end, DiagDirection::SW);
	SetAirportTileType(apron, ATT_APRON_NORMAL);
	Aircraft *a = MakeReservationAircraft(TileXY(10, 12));
	Aircraft *blocker = MakeReservationAircraft(apron);
	REQUIRE(TryReserveAircraftTile(blocker, apron, TRACK_BIT_X));
	SetRunwayExtremeDirection(end, DiagDirection::NE);
	CHECK(GetAircraftRunwayEnd(start) == INVALID_TILE);
	CHECK(GetAircraftRunwayLength(start) == 0);
	SetRunwayExtremeDirection(end, DiagDirection::SW);
	REQUIRE(GetAircraftRunwayEnd(start) == end);
	CHECK_FALSE(HasAircraftRunwayExit(a, start, false));
	SetAirportTileTracks(TileXY(12, 10), TRACK_BIT_CROSS);
	SetAirportTileTracks(TileXY(12, 11), TRACK_BIT_CROSS);
	CHECK(HasAircraftRunwayExit(a, start, false));
	CHECK_FALSE(HasAircraftRunwayExit(a, start, true));
	CHECK(HasAircraftTileReservation(blocker, apron, TRACK_BIT_X));
	CHECK(a->planned_path.empty());
}

TEST_CASE("Aircraft can exit a braked landing roll through a connected intermediate taxiway")
{
	extern bool TryAircraftEarlyRunwayExit(Aircraft *v);
	REQUIRE(Engine::GetNumItems() == 0);
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _engine_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	Station *st = MakeReservationAirport(TRACK_BIT_NONE);
	const TileIndex start = TileXY(10, 10);
	const TileIndex middle = TileXY(11, 10);
	const TileIndex end = TileXY(12, 10);
	const TileIndex apron = TileXY(11, 11);
	SetAirportTileType(start, ATT_RUNWAY_START_ALLOW_LANDING);
	SetAirportTileType(middle, ATT_RUNWAY_MIDDLE);
	SetAirportTileType(end, ATT_RUNWAY_END);
	SetRunwayExtremeDirection(start, DiagDirection::SW);
	AddPlainRunwayDirections(middle, DiagDirection::SW, true);
	SetRunwayExtremeDirection(end, DiagDirection::SW);
	SetAirportTileTracks(middle, TRACK_BIT_CROSS);
	SetAirportTileTracks(end, TRACK_BIT_X);
	SetAirportTileType(apron, ATT_APRON_NORMAL);
	SetAirportTileTracks(apron, TRACK_BIT_Y);
	st->airport.aprons.push_back(apron);

	Aircraft *a = MakeReservationAircraft(middle);
	a->engine_type = a->Next()->engine_type = Engine::Create(VehicleType::Aircraft, 0)->index;
	a->targetairport = st->index;
	a->current_order.MakeGoToStation(st->index);
	REQUIRE(TryReserveAircraftRunway(a, start));
	a->state = AS_LANDED;
	a->UpdateNextTile(end);
	a->x_pos = TileX(middle) * TILE_SIZE + TILE_SIZE / 2;
	a->y_pos = TileY(middle) * TILE_SIZE + TILE_SIZE / 2;
	a->landing_brake_speed = 20;
	a->landing_roll_start_available = 2 * TILE_SIZE;
	a->landing_brake_distance = TILE_SIZE;

	/* The far end can taxi back to the same connection, but only if the
	 * parallel taxi track actually exists. A geometric shortcut is not enough. */
	REQUIRE(HasAircraftRunwayExit(a, start, false));
	SetAirportTileTracks(middle, TRACK_BIT_Y);
	CHECK_FALSE(HasAircraftRunwayExit(a, start, false));
	SetAirportTileTracks(middle, TRACK_BIT_CROSS);

	SECTION("the full side exit is available") {
		a->landing_brake_distance++;
		CHECK_FALSE(TryAircraftEarlyRunwayExit(a));
		a->landing_brake_distance--;
		REQUIRE(TryAircraftEarlyRunwayExit(a));
		CHECK(a->state == AS_RUNNING);
		CHECK(a->GetNextTile() == apron);
		CHECK(a->allocated_terminal == apron);
		CHECK(a->reserved_runway == INVALID_TILE);
		CHECK_FALSE(GetReservationAsRunway(end));
	}
	SECTION("a denied taxi route leaves the roll aimed at the far end") {
		Aircraft *blocking = MakeReservationAircraft(TileXY(14, 10));
		REQUIRE(TryClaimAircraftTerminal(blocking, apron));
		CHECK_FALSE(TryAircraftEarlyRunwayExit(a));
		CHECK(a->state == AS_LANDED);
		CHECK(a->GetNextTile() == end);
		CHECK(a->reserved_runway == start);
		CHECK(GetReservationAsRunway(end));
		CHECK(a->planned_path.empty());
	}
}

TEST_CASE("Aircraft landing exit rollback keeps the runway, real occupancy and queued intent")
{
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	MakeReservationAirport();
	const TileIndex start = TileXY(10, 10);
	const TileIndex end = TileXY(11, 10);
	SetAirportTileType(start, ATT_RUNWAY_START_ALLOW_LANDING);
	SetAirportTileType(end, ATT_RUNWAY_END);
	SetRunwayExtremeDirection(start, DiagDirection::SW);
	SetRunwayExtremeDirection(end, DiagDirection::SW);
	Aircraft *a = MakeReservationAircraft(end);
	REQUIRE(TryReserveAircraftRunway(a, start));
	REQUIRE(TryReserveAircraftTile(a, end, TRACK_BIT_X));
	a->state = AS_LANDED;
	const AircraftJunctionAction request{end, end, end, TRACKDIR_X_SW, TRACKDIR_X_SW, false, {end}, {TRACK_BIT_X}, {TRACK_BIT_NONE}};
	a->junction_requests = {request};
	a->junction_request_tick = StateTicks{42};
	CHECK_FALSE(ProbeAircraftTaxiExit(a, [](Aircraft *v) {
		REQUIRE(TryReserveAircraftTile(v, v->tile, TRACK_BIT_Y));
		REQUIRE(TryClaimAircraftTerminal(v, TileXY(14, 10)));
		return false;
	}));
	CHECK(a->reserved_runway == start);
	CHECK(GetReservationAsRunway(start));
	CHECK(GetReservationAsRunway(end));
	CHECK(GetReservedAirportTracks(end) == TRACK_BIT_X);
	CHECK(a->allocated_terminal == INVALID_TILE);
	CHECK(a->junction_requests == std::vector{request});
	CHECK(a->junction_request_tick == StateTicks{42});
}

TEST_CASE("Erasing a ghost aircraft reservation does not erase another owner")
{
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	MakeReservationAirport();
	const TileIndex tile = TileXY(11, 10);
	Aircraft *owner = MakeReservationAircraft(tile);
	Aircraft *ghost = MakeReservationAircraft(TileXY(10, 10));
	REQUIRE(TryReserveAircraftTile(owner, tile, TRACK_BIT_X));
	SetAirportTrackReservation(tile, TRACK_Y);
	ghost->reserved_path = {{tile, TRACK_BIT_CROSS}};
	ReleaseAircraftTaxiPath(ghost, false);
	CHECK(ghost->reserved_path.empty());
	CHECK(HasAircraftTileReservation(owner, tile, TRACK_BIT_X));
	CHECK(GetReservedAirportTracks(tile) == TRACK_BIT_X);
}
