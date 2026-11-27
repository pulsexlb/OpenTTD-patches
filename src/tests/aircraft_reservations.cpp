/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../aircraft.h"
#include "../air_map.h"
#include "../airport.h"
#include "../core/backup_type.hpp"
#include "../engine_base.h"
#include "../pbs_air.h"
#include "../sl/extended_ver_sl.h"
#include "../station_base.h"
#include "../scope.h"

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

static Station *MakeReservationAirport(TrackBits tracks = TRACK_BIT_CROSS)
{
	Station *st = Station::Create(TileXY(10, 10));
	st->facilities = {StationFacility::Airport};
	st->airport.tile = TileXY(10, 10);
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
	AfterLoadAircraftReservations(false); // New snapshot: rebuild indexes, never change reservations.
	CHECK_FALSE(TryClaimAircraftTerminal(b, gate));
	ReleaseAircraftTerminal(a);
	CHECK(TryClaimAircraftTerminal(b, gate));
	ReleaseAircraftTerminal(b);
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
	REQUIRE(GetAircraftRunwayEnd(start) == end);
	CHECK_FALSE(HasAircraftRunwayExit(a, start, false));
	SetAirportTileTracks(TileXY(12, 10), TRACK_BIT_CROSS);
	SetAirportTileTracks(TileXY(12, 11), TRACK_BIT_CROSS);
	CHECK(HasAircraftRunwayExit(a, start, false));
	CHECK_FALSE(HasAircraftRunwayExit(a, start, true));
	CHECK(HasAircraftTileReservation(blocker, apron, TRACK_BIT_X));
	CHECK(a->planned_path.empty());
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
