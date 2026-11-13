/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License, version 2.
 */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../aircraft.h"
#include "../air_map.h"
#include "../airport.h"
#include "../pbs_air.h"
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

static Station *MakeReservationAirport()
{
	Station *st = Station::Create(TileXY(10, 10));
	st->facilities = {StationFacility::Airport};
	st->airport.tile = TileXY(10, 10);
	st->airport.w = 12;
	st->airport.h = 4;
	for (TileIndex tile : st->airport) {
		MakeAirport(tile, OWNER_BEGIN, st->index, 0, WaterClass::Invalid);
		SetAirportTileType(tile, ATT_SIMPLE_TRACK);
		SetAirportTileTracks(tile, TRACK_BIT_CROSS);
	}
	return st;
}

TEST_CASE("Aircraft rolling reservations - exclusive current and one tile ahead")
{
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	MakeReservationAirport();
	Aircraft *a = MakeReservationAircraft(TileXY(10, 10));
	Aircraft *b = MakeReservationAircraft(TileXY(12, 10));
	a->planned_path = {{TileXY(10,10), TRACKDIR_X_SW, false}, {TileXY(11,10), TRACKDIR_X_SW, false}, {TileXY(12,10), TRACKDIR_X_SW, false}};
	REQUIRE(ExtendAircraftTaxiReservation(a));
	CHECK(a->reserved_path.size() == 2);
	CHECK(GetReservedAirportTracks(TileXY(12,10)) == TRACK_BIT_NONE);
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

TEST_CASE("Aircraft rolling reservations - old load discards orphan taxi reservations")
{
	AllocateMap(64, 64);
	ClearAircraftReservationIndexes();
	auto cleanup = scope_guard([] { ClearAircraftReservationIndexes(); _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	MakeReservationAirport();
	Aircraft *a = MakeReservationAircraft(TileXY(10,10));
	a->state = AS_RUNNING;
	SetAirportTrackReservation(TileXY(10,10), TRACK_X);
	SetAirportTrackReservation(TileXY(11,10), TRACK_X);
	SetAirportTrackReservation(TileXY(12,10), TRACK_X);
	AfterLoadAircraftReservations(true);
	CHECK(a->state == AS_IDLE);
	CHECK(GetReservedAirportTracks(TileXY(10,10)) == TRACK_BIT_X);
	CHECK(GetReservedAirportTracks(TileXY(11,10)) == TRACK_BIT_NONE);
	CHECK(GetReservedAirportTracks(TileXY(12,10)) == TRACK_BIT_NONE);
	CHECK(a->reserved_path.size() == 1);
	ReleaseAircraftTaxiPath(a, false);
}
