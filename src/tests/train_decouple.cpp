/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file train_decouple.cpp Regression tests for read-only coupling checks and decoupling cuts. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../train.h"
#include "../train_cmd.h"
#include "../aircraft.h"
#include "../engine_base.h"
#include "../newgrf.h"
#include "../newgrf_spritegroup.h"
#include "../settings_type.h"
#include "../core/random_func.hpp"
#include "../core/backup_type.hpp"
#include "../map_func.h"
#include "../station_map.h"
#include "../scope.h"
#include "../gfx_type.h"
#include "../sl/saveload.h"
#include "../sl/saveload_filter.h"

extern std::string _savegame_format;

/** Capture a real savegame to verify the network snapshot's binary representation. */
struct CoupleSnapshotCapture : SaveFilter {
	std::vector<uint8_t> bytes;
	CoupleSnapshotCapture() : SaveFilter(nullptr) {}
	void Write(uint8_t *data, size_t size) override { bytes.insert(bytes.end(), data, data + size); }
};

TEST_CASE("Aircraft - state transitions refresh callback caches even when stopped")
{
	REQUIRE(Engine::GetNumItems() == 0);
	REQUIRE(Vehicle::GetNumItems() == 0);
	REQUIRE(Engine::CanAllocateItem());
	Engine *engine = Engine::Create(VehicleType::Aircraft, 0);
	GRFFile grf;
	grf.grf_version = 8;
	engine->grf_prop.grffile = &grf;
	engine->cb36_properties_used = 0;
	SetBit(engine->cb36_properties_used, PROP_AIRCRAFT_SPEED);
	CallbackResultSpriteGroup ground(100);
	CallbackResultSpriteGroup flight(200);
	engine->grf_prop.SetSpriteGroup(CargoGRFFileProps::SG_DEFAULT, &ground);
	REQUIRE(Aircraft::CanAllocateItem(2));
	Aircraft *aircraft = Aircraft::Create();
	Aircraft *shadow = Aircraft::Create();
	aircraft->subtype = AIR_AIRCRAFT;
	shadow->subtype = AIR_SHADOW;
	aircraft->engine_type = shadow->engine_type = engine->index;
	aircraft->SetNext(shadow);
	aircraft->vehstatus.Set(VehState::Stopped);
	aircraft->cur_speed = 0;
	aircraft->state = AS_HANGAR;
	UpdateAircraftCache(aircraft);
	CHECK(aircraft->vcache.cached_max_speed == 1280);
	CHECK(aircraft->last_cached_state == AS_HANGAR);
	engine->grf_prop.SetSpriteGroup(CargoGRFFileProps::SG_DEFAULT, &flight);
	aircraft->state = AS_FLYING;
	CHECK(aircraft->Tick());
	CHECK(aircraft->vcache.cached_max_speed == 2560);
	CHECK(aircraft->last_cached_state == AS_FLYING);
	_vehicle_pool.CleanPool();
	_engine_pool.CleanPool();
}

class TrainDecoupleFixture {
protected:
	Engine *engine;

	TrainDecoupleFixture()
	{
		REQUIRE(_vehicle_pool.items == 0);
		REQUIRE(_engine_pool.items == 0);
		REQUIRE(Engine::CanAllocateItem());
		engine = Engine::Create(VehicleType::Train, 0);
	}

	~TrainDecoupleFixture()
	{
		_vehicle_pool.CleanPool();
		_engine_pool.CleanPool();
	}

	Train *MakeTrain()
	{
		REQUIRE(Train::CanAllocateItem());
		Train *v = Train::Create();
		v->SetEngine();
		v->engine_type = engine->index;
		v->gcache.cached_veh_length = VEHICLE_LENGTH;
		return v;
	}
};

TEST_CASE("Couple - early checkpoint survives order copy without changing coupling station")
{
	Order order;
	order.MakeGoToCouple();
	order.SetCoupleStation(StationID{27});
	order.SetCoupleCheckPoint(StationID{12});
	Order copy(order);
	CHECK(copy.HasCoupleCheckPoint());
	CHECK(copy.GetCoupleCheckPoint() == StationID{12});
	CHECK(copy.GetCoupleStation() == StationID{27});
	copy.SetCoupleCheckPoint(StationID::Invalid());
	CHECK_FALSE(copy.HasCoupleCheckPoint());
	CHECK(copy.GetCoupleStation() == StationID{27});
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - depot override releases checkpoint and exclusive partner")
{
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	moving->couple_target = waiting->index;
	waiting->couple_claimant = moving->index;
	waiting->couple_claim_cost = 42;
	moving->flags.Set({VehicleRailFlag::CoupleCheckReversePending, VehicleRailFlag::CoupleCheckReservationReleased});
	moving->reverse_distance = 16;
	CancelTrainCoupleCheckpoint(moving);
	CHECK(moving->couple_target == VehicleID::Invalid());
	CHECK(waiting->couple_claimant == VehicleID::Invalid());
	CHECK(waiting->couple_claim_cost == 0);
	CHECK(moving->reverse_distance == 0);
	CHECK_FALSE(moving->flags.Test(VehicleRailFlag::CoupleCheckReversePending));
	CHECK_FALSE(moving->flags.Test(VehicleRailFlag::CoupleCheckReservationReleased));
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - checkpoint cancellation preserves ordinary reversing and another claimant")
{
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	Train *other = MakeTrain();
	moving->couple_target = waiting->index;
	waiting->couple_claimant = other->index;
	waiting->couple_claim_cost = 42;
	moving->reverse_distance = 16;
	CancelTrainCoupleCheckpoint(moving);
	CHECK(moving->couple_target == VehicleID::Invalid());
	CHECK(waiting->couple_claimant == other->index);
	CHECK(waiting->couple_claim_cost == 42);
	CHECK(moving->reverse_distance == 16);
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - diagnostics do not clear a stale claim")
{
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	moving->current_order.MakeGoToCouple();
	moving->couple_target = waiting->index;
	waiting->couple_claimant = moving->index;
	waiting->couple_claim_cost = 42;
	/* The target is no longer waiting: querying it must report the invalid
	 * claim without modifying the network snapshot from GUI scope. */
	for (int i = 0; i < 2; ++i) {
		CHECK(GetCoupleClaimant(waiting) == nullptr);
		CHECK(moving->couple_target == waiting->index);
		CHECK(waiting->couple_claimant == moving->index);
		CHECK(waiting->couple_claim_cost == 42);
	}
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Train - speed inspection preserves claim and flags until a simulation update")
{
	AutoRestoreBackup acceleration(_settings_game.vehicle.train_acceleration_model, AccelerationModel::Original);
	AutoRestoreBackup<uint8_t> braking(_settings_game.vehicle.train_braking_model, TBM_ORIGINAL);
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	moving->current_order.MakeGoToCouple();
	moving->couple_target = waiting->index;
	waiting->couple_claimant = moving->index;
	moving->flags.Set(VehicleRailFlag::ConsistSpeedReduction);
	moving->cur_speed = 100;
	const auto flags = moving->flags;
	for (int i = 0; i < 2; ++i) {
		moving->GetCurrentMaxSpeedInfo();
		CHECK(moving->couple_target == waiting->index);
		CHECK(moving->flags == flags);
		CHECK(moving->cur_speed == 100);
	}
	moving->GetCurrentMaxSpeedInfoAndUpdate();
	CHECK(moving->couple_target == VehicleID::Invalid());
	CHECK_FALSE(moving->flags.Test(VehicleRailFlag::ConsistSpeedReduction));
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - inspecting approach speed does not brake the train")
{
	AutoRestoreBackup acceleration(_settings_game.vehicle.train_acceleration_model, AccelerationModel::Original);
	AutoRestoreBackup<uint8_t> braking(_settings_game.vehicle.train_braking_model, GENERATE(TBM_ORIGINAL, TBM_REALISTIC));
	AllocateMap(64, 64);
	auto map_cleanup = scope_guard([]() { DeallocateMap(); });
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	moving->current_order.MakeGoToCouple();
	moving->current_order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	waiting->current_order.MakeWaitCouple();
	moving->couple_target = waiting->index;
	waiting->couple_claimant = moving->index;
	waiting->tile = TileXY(1, 1);
	MakeRailStation(waiting->tile, OWNER_NONE, StationID{0}, Axis::X, 0, RAILTYPE_RAIL);
	moving->x_pos = 0;
	waiting->x_pos = VEHICLE_LENGTH + 1;
	moving->gcache.cached_max_track_speed = 500;
	moving->cur_speed = 500;
	REQUIRE(GetCoupleClaimant(waiting) == moving);
	const auto status = moving->vehstatus;
	for (int i = 0; i < 2; ++i) {
		const auto limit = moving->GetCurrentMaxSpeedInfo();
		CHECK(limit.strict_max_speed < 500);
		CHECK(moving->cur_speed == 500);
		CHECK(moving->vehstatus == status);
	}
	moving->GetCurrentMaxSpeedInfoAndUpdate();
	CHECK(moving->cur_speed < 500);
	CHECK(moving->vehstatus.Test(VehState::TrainSlowing));
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Decouple - single unit has no cut")
{
	Train *v = MakeTrain();
	for (uint count : {0, 1, 2, 127}) CHECK(GetDecoupleVehicleForCount(v, count) == nullptr);
	CHECK(GetDecoupleCuts(v).empty());
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - network snapshot preserves wide IDs and invalid targets")
{
	AllocateMap(64, 64);
	AutoRestoreBackup format(_savegame_format, std::string{"none"});
	AutoRestoreBackup cursor(_cursor.sprites, std::vector<CursorSprite>{{0, 0, 0, 0}});
	Train *a = Train::CreateAtIndex(VehicleID{0x10001});
	Train *b = Train::CreateAtIndex(VehicleID{0x20002});
	a->engine_type = b->engine_type = engine->index;
	a->couple_target = b->index;
	a->couple_claim_cost = 123;
	b->couple_claimant = a->index;
	b->couple_body_hold = true;
	auto snapshot = std::make_shared<CoupleSnapshotCapture>();
	const SaveLoadResult result = SaveWithFilter(snapshot, false, SMF_NET_SERVER);
	DeallocateMap();
	REQUIRE(result == SaveLoadResult::Ok);
	const std::array<uint8_t, 4> marker{'C', 'P', 'L', 'M'};
	/* SLXI also mentions CPLM by name; the actual state chunk is the last occurrence. */
	auto chunk = std::find_end(snapshot->bytes.begin(), snapshot->bytes.end(), marker.begin(), marker.end());
	REQUIRE(chunk != snapshot->bytes.end());
	REQUIRE(std::distance(chunk, snapshot->bytes.end()) >= 46);
	auto read32 = [&](size_t offset) {
		return (uint32_t(chunk[offset]) << 24) | (uint32_t(chunk[offset + 1]) << 16) | (uint32_t(chunk[offset + 2]) << 8) | chunk[offset + 3];
	};
	CHECK(read32(4) == 38); // 4-byte count and two 17-byte records.
	CHECK(read32(8) == 2);
	CHECK(read32(12) == 0x10001);
	CHECK(read32(16) == 0x20002);
	CHECK(read32(20) == VehicleID::Invalid().base());
	CHECK(read32(24) == 123);
	CHECK(chunk[28] == 0);
	CHECK(read32(29) == 0x20002);
	CHECK(read32(33) == VehicleID::Invalid().base());
	CHECK(read32(37) == 0x10001);
	CHECK(chunk[45] == 1);
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Decouple - cuts preserve legacy dual-head order counts")
{
	Train *front = MakeTrain();
	Train *rear = MakeTrain();
	Train *a = MakeTrain();
	Train *b = MakeTrain();
	front->SetMultiheaded();
	rear->SetMultiheaded();
	rear->ClearEngine();
	front->other_multiheaded_part = rear;
	rear->other_multiheaded_part = front;
	front->SetNext(rear);
	rear->SetNext(a);
	a->SetNext(b);

	const auto cuts = GetDecoupleCuts(front);
	REQUIRE(cuts.size() == 2);
	CHECK(cuts[0].num_keep == 2);
	CHECK(cuts[0].vehicle == a);
	CHECK(cuts[1].num_keep == 3);
	CHECK(cuts[1].vehicle == b);
	CHECK(GetDecoupleVehicleForCount(front, 1) == nullptr);
	for (const DecoupleCut &cut : cuts) CHECK(GetDecoupleVehicleForCount(front, cut.num_keep) == cut.vehicle);
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Decouple - articulated parts stay together and primary may be behind cut")
{
	Train *head = MakeTrain();
	Train *part = MakeTrain();
	Train *engine = MakeTrain();
	head->ClearEngine();
	head->SetWagon();
	part->ClearEngine();
	part->SetArticulatedPart();
	head->SetNext(part);
	part->SetNext(engine);
	for (Train *v : {head, part, engine}) v->SetPrimary(engine);

	const auto cuts = GetDecoupleCuts(engine);
	REQUIRE(cuts.size() == 1);
	CHECK(cuts[0].num_keep == 1);
	CHECK(cuts[0].vehicle == engine);
	CHECK(GetDecoupleVehicleForCount(engine, 1) == engine);
	/* Preserve the execution path's existing clamping for large order values. */
	CHECK(GetDecoupleVehicleForCount(engine, 127) == engine);
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - callback trial restores both consists on success and rejection")
{
	AutoRestoreBackup length_limit(_settings_game.vehicle.max_train_length, uint8_t{8});
	GRFFile grf;
	grf.grf_version = 8;
	engine->grf_prop.grffile = &grf;
	engine->cb36_properties_used = 0;
	SetBit(engine->cb36_properties_used, PROP_TRAIN_USER_DATA);
	CallbackResultSpriteGroup result(0x400);
	DeterministicSpriteGroup group(SpriteGroupID::Invalid());
	group.size = DSG_SIZE_DWORD;
	group.var_scope = VSG_SCOPE_SELF;
	DeterministicSpriteGroupAdjust adjust{};
	adjust.variable = 0x40;
	adjust.and_mask = UINT32_MAX;
	group.adjusts.push_back(adjust);
	group.default_group = &result;
	engine->grf_prop.SetSpriteGroup(CargoGRFFileProps::SG_DEFAULT, &group);

	Train *a = MakeTrain();
	Train *b = MakeTrain();
	Train *c = MakeTrain();
	Train *d = MakeTrain();
	a->SetNext(b);
	c->SetNext(d);
	/* Exercise a primary that is not the physical chain head. */
	a->SetPrimary(b);
	b->SetPrimary(b);
	c->SetPrimary(c);
	d->SetPrimary(c);
	c->couple_claimant = b->index;
	b->couple_target = c->index;
	c->couple_claim_cost = 123;

	for (Train *v : {a, b, c, d}) {
		v->tcache.user_def_data = 0xA5;
		v->grf_cache.position_consist_length = 12345;
		v->grf_cache.cache_valid = 1;
	}
	const NewGRFCache before = a->grf_cache;
	const uint8_t flags_before = a->vcache.cached_veh_flags;
	GameRandomSeedChecker random_checker;
	bool allowed = true;
	SECTION("accepted") {}
	SECTION("attachment rejected") {
		result.result = 0x402;
		allowed = false;
	}

	for (int attempt = 0; attempt < 2; attempt++) {
		CHECK(IsCoupleArrangementValid(b, c) == allowed);
		CHECK(a->Next() == b);
		CHECK(b->Next() == nullptr);
		CHECK(c->Next() == d);
		CHECK(d->Next() == nullptr);
		CHECK(b->First() == a);
		CHECK(d->First() == c);
		CHECK(a->Last() == b);
		CHECK(c->Last() == d);
		CHECK(a->Primary() == b);
		CHECK(d->Primary() == c);
		for (Train *v : {a, b, c, d}) {
			CHECK(v->tcache.user_def_data == 0xA5);
			CHECK(v->grf_cache == before);
			CHECK(v->vcache.cached_veh_flags == flags_before);
		}
		CHECK(c->couple_claimant == b->index);
		CHECK(b->couple_target == c->index);
		CHECK(c->couple_claim_cost == 123);
		CHECK(random_checker.Check());
	}
}

TEST_CASE("Decouple - same-direction exit bits")
{
	Order order;
	order.MakeDecouple();

	CHECK(!order.GetDecoupleFirstSameDirExit());
	CHECK(!order.GetDecoupleSecondSameDirExit());

	order.SetDecoupleFirstSameDirExit(true);
	CHECK(order.GetDecoupleFirstSameDirExit());
	CHECK(!order.GetDecoupleSecondSameDirExit());

	order.SetDecoupleSecondSameDirExit(true);
	CHECK(order.GetDecoupleFirstSameDirExit());
	CHECK(order.GetDecoupleSecondSameDirExit());

	order.SetDecoupleFirstSameDirExit(false);
	CHECK(!order.GetDecoupleFirstSameDirExit());
	CHECK(order.GetDecoupleSecondSameDirExit());

	/* The bits must not collide with the schedule IDs sharing xdata. */
	order.SetDecoupleFirstScheduleID(OrderListID{1234});
	order.SetDecoupleSecondScheduleID(OrderListID{5678});
	CHECK(order.GetDecoupleFirstScheduleID() == OrderListID{1234});
	CHECK(order.GetDecoupleSecondScheduleID() == OrderListID{5678});
	CHECK(order.GetDecoupleSecondSameDirExit());
	order.SetDecoupleFirstSameDirExit(true);
	CHECK(order.GetDecoupleFirstScheduleID() == OrderListID{1234});
	CHECK(order.GetDecoupleSecondScheduleID() == OrderListID{5678});
}

TEST_CASE("Decouple - procedural orders do not keep timetable incomplete")
{
	Order decouple;
	decouple.MakeDecouple();
	CHECK(decouple.HasNoTimetableTimes());
	CHECK(decouple.IsCompletelyTimetabled());
	Order wait;
	wait.MakeWaitCouple();
	CHECK(wait.HasNoTimetableTimes());
	CHECK(wait.IsCompletelyTimetabled());
}

TEST_CASE("Coupling wait - station target is explicit and old orders still wait in place")
{
	Order wait;
	wait.MakeWaitCouple();
	CHECK_FALSE(wait.HasWaitCoupleStation());
	CHECK(wait.GetWaitCoupleStation() == StationID::Invalid());
	wait.SetWaitCoupleStation(StationID{123});
	CHECK(wait.HasWaitCoupleStation());
	CHECK(wait.GetWaitCoupleStation() == StationID{123});
	CHECK(wait.ShouldStopAtStation(StationID::Invalid(), StationID{123}, false));
	CHECK_FALSE(wait.ShouldStopAtStation(StationID::Invalid(), StationID{124}, false));
	wait.SetWaitCoupleStation(StationID::Invalid());
	CHECK_FALSE(wait.HasWaitCoupleStation());
}
