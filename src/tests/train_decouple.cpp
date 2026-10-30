/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file train_decouple.cpp Regression tests for read-only coupling checks and decoupling cuts. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../train.h"
#include "../train_cmd.h"
#include "../company_base.h"
#include "../company_func.h"
#include "../core/pool_func.hpp"
#include "../station_base.h"
#include "../pathfinder/yapf/yapf.h"
#include "../pbs.h"
#include "../pathfinder/follow_track.hpp"
#include "../aircraft.h"
#include "../engine_base.h"
#include "../newgrf.h"
#include "../newgrf_spritegroup.h"
#include "../settings_type.h"
#include "../core/random_func.hpp"
#include "../core/backup_type.hpp"
#include "../map_func.h"
#include "../station_map.h"
#include "../rail_map.h"
#include "../roadveh_transport.h"
#include "../scope.h"
#include "../gfx_type.h"
#include "../sl/saveload.h"
#include "../sl/saveload_filter.h"
#include "../sl/saveload_func.h"
#include "../strings_func.h"

extern std::string _savegame_format;
extern bool ValidateVehicleTileHash(const Vehicle *v);
extern void UpdateVehicleTileHash(Vehicle *v, bool remove);

/** Capture a real savegame to verify the network snapshot's binary representation. */
struct CoupleSnapshotCapture : SaveFilter {
	std::vector<uint8_t> bytes;
	CoupleSnapshotCapture() : SaveFilter(nullptr) {}
	void Write(const uint8_t *data, size_t size) override { bytes.insert(bytes.end(), data, data + size); }
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
	a->execute_schedule_stack = {0x00120034, 0x00560078};
	b->couple_claimant = a->index;
	b->couple_body_hold = true;
	auto snapshot = std::make_shared<CoupleSnapshotCapture>();
	const SaveLoadResult result = SaveWithFilter(snapshot, false, SMF_NET_SERVER);
	DeallocateMap();
	REQUIRE(result == SaveLoadResult::Ok);
	/* Older clients must reject snapshots whose coupling criteria they cannot enforce. */
	for (const std::string field : {"current_order.couple_speed_value1", "current_order.couple_speed_value2", "current_order.couple_speed_unit1", "current_order.couple_speed_unit2", "current_order.couple_speed_comparator1", "current_order.couple_speed_comparator2"}) {
		CHECK(std::search(snapshot->bytes.begin(), snapshot->bytes.end(), field.begin(), field.end()) != snapshot->bytes.end());
	}
	const std::string speed_feature = "couple_speed_filter";
	CHECK(std::search(snapshot->bytes.begin(), snapshot->bytes.end(), speed_feature.begin(), speed_feature.end()) != snapshot->bytes.end());
	const std::array<uint8_t, 8> frames{0, 0x12, 0, 0x34, 0, 0x56, 0, 0x78};
	CHECK(std::search(snapshot->bytes.begin(), snapshot->bytes.end(), frames.begin(), frames.end()) != snapshot->bytes.end());
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
	group.size = DeterministicSpriteGroupSize::DWord;
	group.var_scope = VarSpriteGroupScope::Self;
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

TEST_CASE_METHOD(TrainDecoupleFixture, "Train - restore driving cab without overriding shunting policy")
{
	Backup<TrainFlipReversingAllowed> setting(_settings_game.difficulty.train_flip_reverse_allowed, TrainFlipReversingAllowed::All, FILE_LINE);
	Train *engine = MakeTrain();
	Train *wagon = MakeTrain();
	wagon->ClearEngine();
	engine->SetNext(wagon);
	engine->vehicle_flags.Set(VehicleFlag::DrivingBackwards);
	wagon->vehicle_flags.Set(VehicleFlag::DrivingBackwards);
	engine->tcache.cached_tflags |= TCF_NO_DRIVING_CAB;
	GameRandomSeedChecker random_checker;
	CHECK(engine->NeedsCabFlip());
	_settings_game.difficulty.train_flip_reverse_allowed = TrainFlipReversingAllowed::None;
	CHECK_FALSE(engine->NeedsCabFlip());
	_settings_game.difficulty.train_flip_reverse_allowed = TrainFlipReversingAllowed::EndOfLineOnly;
	CHECK_FALSE(engine->NeedsCabFlip());
	_settings_game.difficulty.train_flip_reverse_allowed = TrainFlipReversingAllowed::All;
	engine->current_order.MakeDummy();
	engine->current_order.MakeGoToCouple();
	CHECK_FALSE(engine->NeedsCabFlip());
	engine->current_order.MakeDummy();
	wagon->track = TRACK_BIT_WORMHOLE;
	CHECK_FALSE(engine->NeedsCabFlip());
	wagon->track = TRACK_BIT_X;
	CHECK(engine->NeedsCabFlip());
	CHECK(random_checker.Check());
	setting.Restore();
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - allow only an adjacent engine overhang")
{
	AllocateMap(64, 64);
	Train *front = MakeTrain();
	Train *rear = MakeTrain();
	front->SetNext(rear);
	front->tile = TileXY(10, 10);
	rear->tile = TileXY(11, 10);
	front->last_station_visited = StationID{7};
	MakeRailNormal(front->tile, OWNER_NONE, TRACK_BIT_X, RAILTYPE_RAIL);
	MakeRailStation(rear->tile, OWNER_NONE, StationID{7}, Axis::X, 0, RAILTYPE_RAIL);
	CHECK_FALSE(TrainFitStation(front));
	CHECK(GetTrainCouplingStation(front) == StationID{7});
	Train *articulated = MakeTrain();
	articulated->ClearEngine();
	articulated->SetArticulatedPart();
	front->SetNext(nullptr);
	front->SetNext(articulated);
	articulated->SetNext(rear);
	articulated->tile = front->tile;
	CHECK(GetTrainCouplingStation(front) == StationID{7});
	articulated->SetWagon();
	articulated->ClearArticulatedPart();
	CHECK(GetTrainCouplingStation(front) == StationID::Invalid());
	articulated->ClearWagon();
	articulated->SetArticulatedPart();

	front->last_station_visited = StationID{8};
	CHECK(GetTrainCouplingStation(front) == StationID::Invalid());
	front->last_station_visited = StationID{7};
	front->tile = TileXY(9, 10);
	MakeRailNormal(front->tile, OWNER_NONE, TRACK_BIT_X, RAILTYPE_RAIL);
	CHECK(GetTrainCouplingStation(front) == StationID::Invalid());
	DeallocateMap();
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Schedule - finishing any schedule returns to the primary list")
{
	Train *v = MakeTrain();
	auto make_list = []() {
		OrderList *list = OrderList::Create();
		list->SetCompany(CompanyID{0});
		list->InitializePlayerCreated();
		for (int i = 0; i < 3; ++i) { Order order; order.MakeDummy(); list->InsertOrderAt(std::move(order), i); }
		return list;
	};
	OrderList *root = make_list();
	OrderList *parent = make_list();
	OrderList *child = make_list();
	v->orders = child;
	child->AssignVehicle(v);
	v->primary_order = root->index;
	v->primary_order_index = 2;
	v->execute_schedule_stack.push_back(parent->index.base() | (1U << 16));
	SECTION("ordinary return") {}
	SECTION("stale frames from older nested semantics are ignored") {
		OrderList *completed = make_list();
		v->execute_schedule_stack.push_back(completed->index.base() | (3U << 16));
	}
	v->ReturnFromExecuteSchedule();
	CHECK(v->orders == root);
	CHECK(v->cur_real_order_index == 2);
	CHECK(v->primary_order == root->index);
	CHECK(v->primary_order_index == INVALID_VEH_ORDER_ID);
	CHECK_FALSE(v->IsExecutingSchedule());
	CHECK(v->execute_schedule_stack.empty());
	_vehicle_pool.CleanPool();
	_orderlist_pool.CleanPool();
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - a nested detour preserves its caller's claim")
{
	AllocateMap(64, 64);
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	OrderList *root = OrderList::Create();
	OrderList *parent = OrderList::Create();
	OrderList *child = OrderList::Create();
	for (OrderList *list : {root, parent, child}) list->SetCompany(CompanyID{0});
	Order checkpoint;
	checkpoint.MakeGoToStation(StationID{0});
	parent->GetOrderVector().push_back(std::move(checkpoint));
	Order couple;
	couple.MakeGoToCouple();
	couple.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	couple.SetCoupleCheckPoint(StationID{0});
	parent->GetOrderVector().push_back(std::move(couple));
	parent->RecalculateDerivedCounters();
	moving->orders = child;
	child->AssignVehicle(moving);
	moving->primary_order = root->index;
	moving->primary_order_index = 0;
	moving->execute_schedule_stack.push_back(parent->index.base() | (1U << 16));
	moving->couple_target = waiting->index;
	waiting->couple_claimant = moving->index;
	waiting->current_order.MakeWaitCouple();
	waiting->tile = TileXY(1, 1);
	MakeRailStation(waiting->tile, OWNER_NONE, StationID{0}, Axis::X, 0, RAILTYPE_RAIL);
	CHECK(GetCoupleClaimant(waiting) == moving);
	moving->execute_schedule_stack.clear();
	CHECK(GetCoupleClaimant(waiting) == nullptr);
	_vehicle_pool.CleanPool();
	_orderlist_pool.CleanPool();
	DeallocateMap();
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - station inspection preserves hashes and claims")
{
	AllocateMap(64, 64);
	Train *front = MakeTrain();
	Train *part = MakeTrain();
	Train *rear = MakeTrain();
	part->ClearEngine();
	part->SetArticulatedPart();
	front->SetNext(part);
	part->SetNext(rear);
	front->tile = part->tile = TileXY(10, 10);
	rear->tile = TileXY(11, 10);
	front->last_station_visited = StationID{7};
	MakeRailNormal(front->tile, OWNER_NONE, TRACK_BIT_X, RAILTYPE_RAIL);
	MakeRailStation(rear->tile, OWNER_NONE, StationID{7}, Axis::X, 0, RAILTYPE_RAIL);
	for (Train *v : {front, part, rear}) v->UpdatePosition();
	front->couple_target = rear->index;
	rear->couple_claimant = front->index;
	const auto flags = front->flags;
	GameRandomSeedChecker random_checker;
	for (int attempt = 0; attempt < 2; ++attempt) {
		CHECK(GetTrainCouplingStation(part) == StationID{7});
		for (const Train *v : {front, part, rear}) CHECK(ValidateVehicleTileHash(v));
		CHECK(front->couple_target == rear->index);
		CHECK(rear->couple_claimant == front->index);
		CHECK(front->flags == flags);
		CHECK(random_checker.Check());
	}
	DeallocateMap();
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Vehicle tile hash - carried trains stay off the track until unloaded")
{
	AllocateMap(64, 64);
	Train *carrier = MakeTrain();
	Train *carried = MakeTrain();
	carrier->tile = carried->tile = TileXY(10, 10);
	carrier->UpdatePosition();
	carried->UpdatePosition();
	REQUIRE(ValidateVehicleTileHash(carrier));
	REQUIRE(ValidateVehicleTileHash(carried));

	/* Boarding deliberately removes each part from the physical track index. */
	carried->rv_transport_flags |= Vehicle::RV_TRANSPORT_CARRIED;
	CHECK_FALSE(ValidateVehicleTileHash(carried));
	UpdateVehicleTileHash(carried, true);
	CHECK(ValidateVehicleTileHash(carried));
	CHECK(ValidateVehicleTileHash(carrier));
	CHECK(GetFirstVehicleOnTile(carrier->tile, VehicleType::Train) == carrier);

	/* Sprite/position refreshes while on board must not create a phantom train. */
	carried->tile = TileXY(12, 12);
	carried->UpdatePosition();
	CHECK(carried->hash_tile_current == INVALID_TILE);
	CHECK(GetFirstVehicleOnTile(carried->tile, VehicleType::Train) == nullptr);
	CHECK(ValidateVehicleTileHash(carried));
	CHECK(ValidateVehicleTileHash(carrier));

	/* Putting it down restores its occupancy, even on the carrier's tile. */
	carried->tile = carrier->tile;
	carried->rv_transport_flags &= ~Vehicle::RV_TRANSPORT_CARRIED;
	CHECK_FALSE(ValidateVehicleTileHash(carried));
	carried->UpdatePosition();
	CHECK(carried->hash_tile_current == carried->tile);
	CHECK(GetFirstVehicleOnTile(carried->tile, VehicleType::Train) == carried);
	CHECK(ValidateVehicleTileHash(carried));
	CHECK(ValidateVehicleTileHash(carrier));
	_vehicle_pool.CleanPool();
	DeallocateMap();
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Vehicle tile hash - loading repairs stale carried state on a following part")
{
	AllocateMap(64, 64);
	Train *front = MakeTrain();
	Train *part = MakeTrain();
	front->SetNext(part);
	front->tile = part->tile = TileXY(10, 10);
	front->UpdatePosition();
	/* A following part was skipped by the initial hash rebuild because its
	 * saved carried flag disagreed with the primary's ordinary state. */
	part->rv_transport_flags |= Vehicle::RV_TRANSPORT_CARRIED;
	part->vehstatus.Set(VehState::Hidden);
	REQUIRE(ValidateVehicleTileHash(part));
	RVTransportValidateAfterLoad();
	CHECK((part->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0);
	CHECK_FALSE(part->vehstatus.Test(VehState::Hidden));
	CHECK(part->hash_tile_current == part->tile);
	CHECK(ValidateVehicleTileHash(part));
	CHECK(ValidateVehicleTileHash(front));
	_vehicle_pool.CleanPool();
	DeallocateMap();
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - maximum speed filters apply to selection and existing claims")
{
	AllocateMap(64, 64);
	auto map_cleanup = scope_guard([]() { DeallocateMap(); });
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	moving->current_order.MakeGoToCouple();
	moving->current_order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	waiting->current_order.MakeWaitCouple();
	moving->vcache.cached_max_speed = GENERATE(100, 120, 140);
	waiting->vcache.cached_max_speed = 120;
	/* A stationary target and a shunting approach must still compare capabilities. */
	moving->cur_speed = 32;
	waiting->cur_speed = 0;
	moving->current_order.SetCoupleRejectSlower(GENERATE(false, true));
	moving->current_order.SetCoupleRejectFaster(GENERATE(false, true));
	const bool slower = moving->current_order.GetCoupleRejectSlower() && moving->vcache.cached_max_speed < 120;
	const bool faster = moving->current_order.GetCoupleRejectFaster() && moving->vcache.cached_max_speed > 120;
	const auto expected = slower ? CoupleCandidateResult::Slower : (faster ? CoupleCandidateResult::Faster : CoupleCandidateResult::Platform);
	waiting->tile = TileXY(1, 1);
	MakeRailNormal(waiting->tile, OWNER_NONE, TRACK_BIT_X, RAILTYPE_RAIL);
	GameRandomSeedChecker random_checker;
	/* Both routing selection and physical contact use the same speed check. */
	CHECK(GetCoupleCandidateResult(moving, moving->current_order, waiting, waiting->tile, true) == expected);
	CHECK(GetCoupleCandidateResult(moving, moving->current_order, waiting, waiting->tile, false) == expected);
	MakeRailStation(waiting->tile, OWNER_NONE, StationID{0}, Axis::X, 0, RAILTYPE_RAIL);
	moving->couple_target = waiting->index;
	waiting->couple_claimant = moving->index;
	waiting->couple_claim_cost = 42;
	CHECK(GetCoupleClaimant(waiting) == ((slower || faster) ? nullptr : moving));
	/* Inspecting invalid claims must not mutate simulation state from GUI scope. */
	CHECK(moving->couple_target == waiting->index);
	CHECK(waiting->couple_claimant == moving->index);
	CHECK(waiting->couple_claim_cost == 42);
	CHECK(random_checker.Check());
}

TEST_CASE("Couple - speed filters survive copying independently of other order flags")
{
	Order order;
	order.MakeGoToCouple();
	CHECK_FALSE(order.GetCoupleRejectSlower());
	CHECK_FALSE(order.GetCoupleRejectFaster());
	order.SetCoupleUseWaitingSchedule(true);
	order.SetCoupleLoad(ODC_IS_EMPTY);
	order.SetCoupleRejectSlower(true);
	order.SetCoupleRejectFaster(true);
	Order copy(order);
	CHECK(copy.GetCoupleRejectSlower());
	CHECK(copy.GetCoupleRejectFaster());
	copy.SetCoupleRejectSlower(false);
	CHECK_FALSE(copy.GetCoupleRejectSlower());
	CHECK(copy.GetCoupleRejectFaster());
	CHECK(copy.GetCoupleUseWaitingSchedule());
	CHECK(copy.GetCoupleLoad() == ODC_IS_EMPTY);
	CHECK(order.GetCoupleRejectSlower());
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - speed filtering uses the order carrier of each consist")
{
	AllocateMap(64, 64);
	auto map_cleanup = scope_guard([]() { DeallocateMap(); });
	Train *moving_front = MakeTrain();
	Train *moving = MakeTrain();
	Train *waiting_front = MakeTrain();
	Train *waiting = MakeTrain();
	moving_front->SetNext(moving);
	waiting_front->SetNext(waiting);
	moving_front->SetPrimary(moving);
	moving->SetPrimary(moving);
	waiting_front->SetPrimary(waiting);
	waiting->SetPrimary(waiting);
	moving->current_order.MakeGoToCouple();
	moving->current_order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	moving->current_order.SetCoupleRejectSlower(true);
	waiting->current_order.MakeWaitCouple();
	moving->vcache.cached_max_speed = 100;
	waiting->vcache.cached_max_speed = 120;
	/* Deliberately distinct values expose accidentally inspecting a wagon's cache. */
	moving_front->vcache.cached_max_speed = 200;
	waiting_front->vcache.cached_max_speed = 50;
	CHECK(GetCoupleCandidateResult(moving_front, moving->current_order, waiting_front, TileXY(1, 1), false) == CoupleCandidateResult::Slower);
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - custom speed comparisons filter candidates and claims")
{
	AllocateMap(64, 64);
	auto map_cleanup = scope_guard([]() { DeallocateMap(); });
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	moving->current_order.MakeGoToCouple();
	moving->current_order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	const uint row = GENERATE(0, 1);
	const uint8_t comparator = GENERATE(1, 2, 3, 4, 5, 6);
	const uint16_t speed = GENERATE(79, 80, 81);
	moving->current_order.SetCoupleSpeedComparator(row, comparator);
	moving->current_order.SetCoupleSpeedValue(row, 80);
	waiting->current_order.MakeWaitCouple();
	waiting->vcache.cached_max_speed = speed;
	waiting->cur_speed = 0;
	const bool accepted = comparator == 1 ? speed == 80 : comparator == 2 ? speed != 80 : comparator == 3 ? speed < 80 : comparator == 4 ? speed <= 80 : comparator == 5 ? speed > 80 : speed >= 80;
	waiting->tile = TileXY(1, 1);
	MakeRailNormal(waiting->tile, OWNER_NONE, TRACK_BIT_X, RAILTYPE_RAIL);
	GameRandomSeedChecker random_checker;
	const auto expected = accepted ? CoupleCandidateResult::Platform : CoupleCandidateResult::SpeedRange;
	CHECK(GetCoupleCandidateResult(moving, moving->current_order, waiting, waiting->tile, true) == expected);
	CHECK(GetCoupleCandidateResult(moving, moving->current_order, waiting, waiting->tile, false) == expected);
	MakeRailStation(waiting->tile, OWNER_NONE, StationID{0}, Axis::X, 0, RAILTYPE_RAIL);
	moving->couple_target = waiting->index;
	waiting->couple_claimant = moving->index;
	CHECK(GetCoupleClaimant(waiting) == (accepted ? moving : nullptr));
	CHECK(moving->couple_target == waiting->index);
	CHECK(waiting->couple_claimant == moving->index);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - both custom speed boundaries must match and disabled rows do not filter")
{
	AllocateMap(64, 64);
	auto map_cleanup = scope_guard([]() { DeallocateMap(); });
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	waiting->tile = TileXY(1, 1);
	MakeRailNormal(waiting->tile, OWNER_NONE, TRACK_BIT_X, RAILTYPE_RAIL);
	waiting->current_order.MakeWaitCouple();
	Order order;
	order.MakeGoToCouple();
	order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	CHECK(order.GetCoupleSpeedComparator(0) == 0);
	CHECK(order.GetCoupleSpeedComparator(1) == 0);
	order.SetCoupleSpeedComparator(0, 6); // >= 80
	order.SetCoupleSpeedValue(0, 80);
	order.SetCoupleSpeedComparator(1, 4); // <= 120
	order.SetCoupleSpeedValue(1, 120);
	waiting->vcache.cached_max_speed = GENERATE(0, 79, 80, 100, 120, 121, 65535);
	bool accepted = waiting->vcache.cached_max_speed >= 80 && waiting->vcache.cached_max_speed <= 120;
	CHECK(GetCoupleCandidateResult(moving, order, waiting, waiting->tile, false) == (accepted ? CoupleCandidateResult::Platform : CoupleCandidateResult::SpeedRange));
	Order copy(order);
	CHECK(copy.GetCoupleSpeedValue(0) == 80);
	CHECK(copy.GetCoupleSpeedValue(1) == 120);
	copy.SetCoupleSpeedComparator(0, 0);
	copy.SetCoupleSpeedComparator(1, 0);
	CHECK(GetCoupleCandidateResult(moving, copy, waiting, waiting->tile, false) == CoupleCandidateResult::Platform);
	CHECK(order.GetCoupleSpeedComparator(0) == 6);
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - persisted speed units ignore client locale and match displayed speeds")
{
	AllocateMap(64, 64);
	auto map_cleanup = scope_guard([]() { DeallocateMap(); });
	AutoRestoreBackup units(_settings_game.locale.units_velocity, static_cast<uint8_t>(GENERATE(0, 1, 2, 3, 4)));
	AutoRestoreBackup new_units(_settings_newgame.locale.units_velocity, _settings_game.locale.units_velocity);
	Train *moving = MakeTrain();
	Train *waiting = MakeTrain();
	waiting->tile = TileXY(1, 1);
	MakeRailNormal(waiting->tile, OWNER_NONE, TRACK_BIT_X, RAILTYPE_RAIL);
	waiting->current_order.MakeWaitCouple();
	waiting->vcache.cached_max_speed = 120;
	Order order;
	order.MakeGoToCouple();
	order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	order.SetCoupleSpeedComparator(0, 1);
	for (auto [unit, value] : {std::pair{0, 75}, std::pair{1, 120}, std::pair{2, 33}, std::pair{3, 4}, std::pair{4, 65}, std::pair{5, 2}}) {
		order.SetCoupleSpeedUnit(0, unit);
		order.SetCoupleSpeedValue(0, value);
		CHECK(ConvertKmhishSpeedToIntegerDisplaySpeed(120, static_cast<uint8_t>(unit)) == value);
		CHECK(GetCoupleCandidateResult(moving, order, waiting, waiting->tile, false) == CoupleCandidateResult::Platform);
		CHECK(ConvertVelocityUnitValue(value, unit, unit) == value);
		order.SetCoupleSpeedValue(0, value + 1);
		CHECK(GetCoupleCandidateResult(moving, order, waiting, waiting->tile, false) == CoupleCandidateResult::SpeedRange);
	}
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Couple - checkpoint speed preference is soft and preserves existing claims")
{
	AutoRestoreBackup length_limit(_settings_game.vehicle.max_train_length, uint8_t{8});
	AllocateMap(64, 64);
	auto map_cleanup = scope_guard([] { DeallocateMap(); });
	Train *moving = MakeTrain();
	Train *slow = MakeTrain();
	Train *matching = MakeTrain();
	for (Train *train : {moving, slow, matching}) train->SetFrontEngine();
	moving->tile = TileXY(10, 10);
	moving->vcache.cached_max_speed = matching->vcache.cached_max_speed = 120;
	slow->vcache.cached_max_speed = 80;
	moving->current_order.MakeGoToCouple();
	moving->current_order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	for (Train *train : {slow, matching}) {
		train->current_order.MakeWaitCouple();
		train->tile = TileXY(11, 10);
		MakeRailStation(train->tile, OWNER_NONE, StationID{0}, Axis::X, 0, RAILTYPE_RAIL);
	}
	CHECK(GetCoupleSpeedPreferencePenalty(moving, matching) == 0);
	CHECK(GetCoupleSpeedPreferencePenalty(moving, slow) == 10);
	GameRandomSeedChecker random_checker;
	CHECK(FindBestCoupleCheckpointTarget(moving, moving->current_order) == matching);
	/* Prefer the matching train across a moderate detour which outweighed the old score. */
	matching->tile = TileXY(19, 10);
	MakeRailStation(matching->tile, OWNER_NONE, StationID{0}, Axis::X, 0, RAILTYPE_RAIL);
	CHECK(FindBestCoupleCheckpointTarget(moving, moving->current_order) == matching);
	matching->vcache.cached_max_speed = UINT16_MAX;
	CHECK(GetCoupleSpeedPreferencePenalty(moving, matching) <= 24); // Large gaps cannot impose an unbounded detour.
	matching->vcache.cached_max_speed = 120;
	matching->current_order.MakeDummy();
	CHECK(FindBestCoupleCheckpointTarget(moving, moving->current_order) == slow);
	matching->current_order.MakeWaitCouple();
	matching->tile = TileXY(40, 10);
	MakeRailStation(matching->tile, OWNER_NONE, StationID{0}, Axis::X, 0, RAILTYPE_RAIL);
	CHECK(FindBestCoupleCheckpointTarget(moving, moving->current_order) == slow); // A large detour must not force a match.
	moving->couple_target = slow->index;
	slow->couple_claimant = moving->index;
	CHECK(GetCoupleClaimant(slow) == moving);
	CHECK(moving->couple_target == slow->index);
	CHECK(random_checker.Check());
}


class CouplePowerFixture : public TrainDecoupleFixture {
protected:
	AutoRestoreBackup<CompanyID> company_backup{_current_company, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<uint8_t> length_limit{_settings_game.vehicle.max_train_length, uint8_t{8}};
	AutoRestoreBackup<RailTypes> rail_direct{const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->compatible_railtypes, RailTypes{RAILTYPE_RAIL}};
	AutoRestoreBackup<RailTypes> rail_power{const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->powered_railtypes, RailTypes{RAILTYPE_RAIL}};
	AutoRestoreBackup<RailTypes> rail_compatibility{const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->indirect_compatible_railtypes, RailTypes{RAILTYPE_RAIL}};
	Train *low;
	Train *strong;
	Train *waiting;
	Station *station;
	CouplePowerFixture()
	{
		REQUIRE(Company::GetNumItems() == 0);
		REQUIRE(Station::GetNumItems() == 0);
		ResetVehicleHash();
		AllocateMap(64, 64);
		_current_company = Company::Create()->index;
		for (uint x = 5; x <= 30; x++) MakeRailNormal(TileXY(x, 10), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		for (uint x = 5; x < 20; x++) {
			MakeRailNormal(TileXY(x, 8), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
			MakeRailNormal(TileXY(x, 12), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		}
		MakeRailNormal(TileXY(20, 8), _current_company, TRACK_BIT_RIGHT, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20, 9), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20, 10), _current_company, TRACK_BIT_X | TRACK_BIT_LEFT | TRACK_BIT_LOWER, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20, 11), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20, 12), _current_company, TRACK_BIT_UPPER, RAILTYPE_RAIL);
		station = Station::Create(TileXY(25, 10));
		station->owner = _current_company;
		station->facilities = {StationFacility::Train};
		station->train_station = TileArea(station->xy, 1, 1);
		station->rect.BeforeAddTile(station->xy, StationRect::ADD_FORCE);
		MakeRailStation(station->xy, _current_company, station->index, Axis::X, 0, RAILTYPE_RAIL);
		low = MakeTrain(); strong = MakeTrain(); waiting = MakeTrain();
		Place(low, 8, 8); Place(strong, 12, 12); Place(waiting, 25);
		for (Train *train : {low, strong, waiting}) {
			train->SetFrontEngine();
			train->owner = _current_company;
			train->track = TRACK_BIT_X;
			train->direction = Direction::SW;
			train->compatible_railtypes.Set(RAILTYPE_RAIL);
			train->railtypes.Set(RAILTYPE_RAIL);
			train->vcache.cached_max_speed = 120;
			train->gcache.cached_total_length = VEHICLE_LENGTH;
			train->current_order.MakeGoToCouple();
			train->current_order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
			train->current_order.SetCoupleSpeedBoundMode(1, CoupleSpeedBoundMode::Manual);
			train->current_order.SetCoupleSpeedValue(1, UINT16_MAX);
		}
		low->gcache.cached_power = 1000;
		strong->gcache.cached_power = 2000;
		waiting->gcache.cached_power = 0;
		waiting->current_order.MakeWaitCouple();
		waiting->last_station_visited = station->index;
	}
	~CouplePowerFixture()
	{
		_vehicle_pool.CleanPool();
		ResetVehicleHash();
		_orderlist_pool.CleanPool();
		_station_pool.CleanPool();
		_company_pool.CleanPool();
		ClearOrderDestinationRefcountMap();
		DeallocateMap();
	}
	void Place(Train *train, uint x, uint y = 10)
	{
		train->tile = TileXY(x, y);
		train->x_pos = x * TILE_SIZE + 8;
		train->y_pos = y * TILE_SIZE + 8;
		train->z_pos = 0;
		UpdateVehicleTileHash(train, false);
	}
};

TEST_CASE_METHOD(CouplePowerFixture, "Couple power - ready stronger same-speed locomotive wins before claim")
{
	GameRandomSeedChecker random_checker;
	Train *contact = nullptr;
	uint32_t cost = 0;
	REQUIRE(YapfTrainCoupleTrack(strong, true, &contact, &cost, nullptr, waiting) != INVALID_TRACK);
	REQUIRE(contact == waiting);
	low->couple_target = waiting->index;
	ClaimCoupleTarget(low, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == strong);
	CHECK(strong->couple_target == waiting->index);
	CHECK(strong->current_order.IsType(OT_GOTO_COUPLE));
	CHECK(strong->cur_speed == 0);
	if (IsValidTile(strong->tile)) CHECK(GetRailReservationTrackBits(strong->tile) == TRACK_BIT_NONE);
	CHECK(random_checker.Check());
	for (Train *train : {low, strong, waiting}) CHECK(ValidateVehicleTileHash(train));
}


TEST_CASE_METHOD(CouplePowerFixture, "Couple power - unsuitable stronger locomotives never block normal claims")
{
	SECTION("query budget exhausted") {
		AutoRestoreBackup limit(_settings_game.pf.yapf.max_search_nodes, uint32_t{1});
		MakeRailNormal(TileXY(16, 12), _current_company, TRACK_BIT_X | TRACK_BIT_UPPER, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(16, 11), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		low->couple_target = waiting->index;
		ClaimCoupleTarget(low, waiting, 0);
		CHECK(GetCoupleClaimant(waiting) == low);
	}
	SECTION("fleet query budget exhausted") {
		for (uint i = 0; i < 16; i++) {
			Train *other = MakeTrain();
			Place(other, 10+i, 15);
			MakeRailNormal(other->tile, _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
			other->SetFrontEngine();
			other->owner = _current_company;
			other->track = TRACK_BIT_X;
			other->direction = Direction::SW;
			other->compatible_railtypes.Set(RAILTYPE_RAIL);
			other->railtypes.Set(RAILTYPE_RAIL);
			other->vcache.cached_max_speed = 120;
			other->gcache.cached_power = 5000;
			other->current_order = strong->current_order;
		}
		low->couple_target = waiting->index;
		ClaimCoupleTarget(low, waiting, 0);
		CHECK(GetCoupleClaimant(waiting) == low);
	}
	SECTION("disconnected track") {
		MakeRailNormal(TileXY(12, 15), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		Place(strong, 12, 15);
	}
	SECTION("virtual locomotive") { strong->SetVirtual(); UpdateVehicleTileHash(strong, false); }
	SECTION("carried locomotive") { strong->rv_transport_flags |= Vehicle::RV_TRANSPORT_CARRIED; UpdateVehicleTileHash(strong, false); }
	SECTION("invalid position") { UpdateVehicleTileHash(strong, true); strong->tile = INVALID_TILE; }
	SECTION("no rail types") { strong->railtypes = {}; }
	SECTION("manually stopped") { strong->vehstatus.Set(VehState::Stopped); }
	SECTION("crashed") { strong->vehstatus.Set(VehState::Crashed); }
	SECTION("breakdown") { strong->flags.Set(VehicleRailFlag::ConsistBreakdown); }
	SECTION("reversal incomplete") { strong->reverse_distance = 16; }
	SECTION("still moving") { strong->cur_speed = 10; }
	SECTION("no power") { strong->gcache.cached_power = 0; }
	SECTION("different maximum speed") { strong->vcache.cached_max_speed = 121; }
	SECTION("order filters reject this partner") { strong->current_order.SetCoupleRejectFaster(true); waiting->vcache.cached_max_speed = 80; }
	SECTION("still loading") { strong->current_order.MakeLoading(false); }
	GameRandomSeedChecker random_checker;
	low->couple_target = waiting->index;
	ClaimCoupleTarget(low, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == low);
	CHECK(strong->couple_target == VehicleID::Invalid());
	if (IsValidTile(strong->tile)) CHECK(GetRailReservationTrackBits(strong->tile) == TRACK_BIT_NONE);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(CouplePowerFixture, "Couple power - protect valid claims in both directions")
{
	SECTION("do not steal a weaker locomotive's active claim") {
		low->couple_target = waiting->index;
		waiting->couple_claimant = low->index;
		REQUIRE(GetCoupleClaimant(waiting) == low);
		strong->couple_target = waiting->index;
		ClaimCoupleTarget(strong, waiting, 100);
		CHECK(GetCoupleClaimant(waiting) == low);
		CHECK(low->couple_target == waiting->index);
	}
	SECTION("do not reassign a stronger locomotive with another valid partner") {
		Train *other = MakeTrain();
		Place(other, 25, 12);
		MakeRailStation(other->tile, _current_company, station->index, Axis::X, 0, RAILTYPE_RAIL);
		other->SetFrontEngine();
		other->track = TRACK_BIT_X;
		other->owner = _current_company;
		other->current_order.MakeWaitCouple();
		other->last_station_visited = station->index;
		strong->couple_target = other->index;
		other->couple_claimant = strong->index;
		REQUIRE(GetCoupleClaimant(other) == strong);
		low->couple_target = waiting->index;
		ClaimCoupleTarget(low, waiting, 0);
		CHECK(GetCoupleClaimant(waiting) == low);
		CHECK(GetCoupleClaimant(other) == strong);
		CHECK(strong->couple_target == other->index);
	}
}

TEST_CASE_METHOD(CouplePowerFixture, "Couple power - equal power uses route cost then stable vehicle ID")
{
	strong->gcache.cached_power = low->gcache.cached_power;
	SECTION("closer route wins") {
		low->couple_target = waiting->index;
		ClaimCoupleTarget(low, waiting, 0);
		CHECK(GetCoupleClaimant(waiting) == strong);
	}
	SECTION("equal route costs prefer lower ID regardless of first caller") {
		Place(strong, 8, 12);
		Train *contact = nullptr;
		uint32_t low_cost = 0, strong_cost = 0;
		REQUIRE(YapfTrainCoupleTrack(low, true, &contact, &low_cost, nullptr, waiting) != INVALID_TRACK);
		REQUIRE(YapfTrainCoupleTrack(strong, true, &contact, &strong_cost, nullptr, waiting) != INVALID_TRACK);
		REQUIRE(low_cost == strong_cost);
		REQUIRE(low->index < strong->index);
		strong->couple_target = waiting->index;
		ClaimCoupleTarget(strong, waiting, strong_cost);
		CHECK(GetCoupleClaimant(waiting) == low);
	}
}


TEST_CASE_METHOD(CouplePowerFixture, "Couple power - checkpoint contenders must be ready and keep their active order")
{
	const bool ready = GENERATE(false, true);
	Station *checkpoint = Station::Create(strong->tile);
	checkpoint->owner = _current_company;
	checkpoint->facilities = {StationFacility::Train};
	checkpoint->train_station = TileArea(checkpoint->xy, 1, 1);
	checkpoint->rect.BeforeAddTile(checkpoint->xy, StationRect::ADD_FORCE);
	MakeRailStation(checkpoint->xy, _current_company, checkpoint->index, Axis::X, 0, RAILTYPE_RAIL);
	Order point;
	point.MakeGoToStation(checkpoint->index);
	Order couple(strong->current_order);
	couple.SetCoupleCheckPoint(checkpoint->index);
	strong->orders = OrderList::Create(Order(point), strong);
	strong->orders->InsertOrderAt(std::move(couple), 1);
	strong->primary_order = strong->orders->index;
	strong->current_order = point;
	strong->cur_real_order_index = strong->cur_implicit_order_index = 0;
	strong->flags.Set(VehicleRailFlag::CoupleCheckReservationReleased, ready);
	const auto before_flags = strong->flags;
	GameRandomSeedChecker random_checker;
	low->couple_target = waiting->index;
	ClaimCoupleTarget(low, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == (ready ? strong : low));
	CHECK(strong->current_order.IsType(OT_GOTO_STATION));
	CHECK(strong->current_order.GetDestination() == checkpoint->index);
	CHECK(strong->cur_real_order_index == 0);
	CHECK(strong->cur_implicit_order_index == 0);
	CHECK(strong->flags == before_flags);
	CHECK(strong->execute_schedule_stack.empty());
	CHECK(random_checker.Check());
	for (uint x = 5; x <= 30; x++) {
		CHECK(GetRailReservationTrackBits(TileXY(x, 10)) == TRACK_BIT_NONE);
		CHECK(GetRailReservationTrackBits(TileXY(x, 12)) == TRACK_BIT_NONE);
	}
}


TEST_CASE_METHOD(CouplePowerFixture, "Couple power - an arriving checkpoint caller still considers a ready stronger locomotive")
{
	Station *checkpoint = Station::Create(low->tile);
	checkpoint->owner = _current_company;
	checkpoint->facilities = {StationFacility::Train};
	checkpoint->train_station = TileArea(checkpoint->xy, 1, 1);
	checkpoint->rect.BeforeAddTile(checkpoint->xy, StationRect::ADD_FORCE);
	MakeRailStation(checkpoint->xy, _current_company, checkpoint->index, Axis::X, 0, RAILTYPE_RAIL);
	Order point;
	point.MakeGoToStation(checkpoint->index);
	Order couple(low->current_order);
	couple.SetCoupleCheckPoint(checkpoint->index);
	low->orders = OrderList::Create(Order(point), low);
	low->orders->InsertOrderAt(std::move(couple), 1);
	low->primary_order = low->orders->index;
	low->current_order = point;
	low->cur_speed = 50;
	low->couple_target = waiting->index;
	REQUIRE(!low->flags.Test(VehicleRailFlag::CoupleCheckReservationReleased));
	ClaimCoupleTarget(low, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == strong);
	CHECK(low->current_order.IsType(OT_GOTO_STATION));
}

TEST_CASE_METHOD(CouplePowerFixture, "Couple power - a completed reverse checkpoint remains eligible after index advance")
{
	Order point;
	point.MakeGoToStation(station->index);
	Order couple(strong->current_order);
	couple.SetCoupleCheckPoint(station->index);
	strong->orders = OrderList::Create(Order(point), strong);
	strong->orders->InsertOrderAt(Order(point), 1);
	strong->orders->InsertOrderAt(std::move(couple), 2);
	strong->primary_order = strong->orders->index;
	strong->cur_real_order_index = strong->cur_implicit_order_index = 1;
	strong->current_order = point;
	strong->flags.Set({VehicleRailFlag::CoupleCheckReversePending, VehicleRailFlag::CoupleCheckReservationReleased});
	strong->reverse_distance = GENERATE(0, 16);
	low->couple_target = waiting->index;
	ClaimCoupleTarget(low, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == (strong->reverse_distance == 0 ? strong : low));
	CHECK(strong->cur_real_order_index == 1);
	CHECK(strong->cur_implicit_order_index == 1);
	CHECK(strong->current_order.IsType(OT_GOTO_STATION));
}

TEST_CASE_METHOD(CouplePowerFixture, "Couple power - disabled speed checks keep ordinary claims")
{
	SECTION("caller disabled") { low->current_order.SetCoupleSpeedBoundMode(1, CoupleSpeedBoundMode::Disabled); }
	SECTION("competitor disabled") { strong->current_order.SetCoupleSpeedBoundMode(1, CoupleSpeedBoundMode::Disabled); }
	GameRandomSeedChecker random_checker;
	low->couple_target = waiting->index;
	ClaimCoupleTarget(low, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == low);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(CouplePowerFixture, "Couple power - speed fit always precedes higher power")
{
	waiting->vcache.cached_max_speed = 80;
	low->vcache.cached_max_speed = 80;
	Train *caller = strong;
	Train *expected = low;
	SECTION("fast powerful caller yields to matching locomotive") { }
	SECTION("matching caller keeps priority") { caller = low; }
	SECTION("closer speed beats more power") { low->vcache.cached_max_speed = 90; }
	SECTION("equal gap favours not overspeeding") {
		waiting->vcache.cached_max_speed = 100;
	}
	SECTION("matching locomotive manually stopped") { low->vehstatus.Set(VehState::Stopped); expected = strong; }
	SECTION("matching locomotive unreachable") {
		Place(low, 8, 15);
		MakeRailNormal(low->tile, _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		expected = strong;
	}
	SECTION("matching locomotive rejects target") {
		low->current_order.SetCoupleSpeedValue(1, 1);
		expected = strong;
	}
	SECTION("existing fast claim remains exclusive") {
		strong->couple_target = waiting->index;
		waiting->couple_claimant = strong->index;
		caller = low;
		expected = strong;
	}
	GameRandomSeedChecker random_checker;
	caller->couple_target = waiting->index;
	ClaimCoupleTarget(caller, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == expected);
	CHECK(expected->couple_target == waiting->index);
	CHECK(random_checker.Check());
	for (Train *train : {low, strong, waiting}) {
		CHECK(ValidateVehicleTileHash(train));
		CHECK(GetRailReservationTrackBits(train->tile) == TRACK_BIT_NONE);
	}
}

TEST_CASE_METHOD(CouplePowerFixture, "Couple power - either speed bound enables priority")
{
	const auto mode = GENERATE(CoupleSpeedBoundMode::OwnMax, CoupleSpeedBoundMode::Manual);
	const uint row = GENERATE(0u, 1u);
	for (Train *train : {low, strong}) {
		train->current_order.SetCoupleSpeedBoundMode(0, CoupleSpeedBoundMode::Disabled);
		train->current_order.SetCoupleSpeedBoundMode(1, CoupleSpeedBoundMode::Disabled);
		train->current_order.SetCoupleSpeedBoundMode(row, mode);
		train->current_order.SetCoupleSpeedValue(row, row == 0 ? 0 : UINT16_MAX);
	}
	low->couple_target = waiting->index;
	ClaimCoupleTarget(low, waiting, 0);
	CHECK(GetCoupleClaimant(waiting) == strong);
}

/** A goto-couple train directly behind an unclaimed waiting partner. */
class CoupleBlockingPartnerFixture : public TrainDecoupleFixture {
protected:
	AutoRestoreBackup<CompanyID> company_backup{_current_company, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<uint8_t> length_limit{_settings_game.vehicle.max_train_length, uint8_t{8}};
	AutoRestoreBackup<RailTypes> rail_direct{const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->compatible_railtypes, RailTypes{RAILTYPE_RAIL}};
	AutoRestoreBackup<RailTypes> rail_power{const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->powered_railtypes, RailTypes{RAILTYPE_RAIL}};
	AutoRestoreBackup<RailTypes> rail_compatibility{const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->indirect_compatible_railtypes, RailTypes{RAILTYPE_RAIL}};
	Train *moving;
	Train *waiting;
	Station *station;

	CoupleBlockingPartnerFixture()
	{
		REQUIRE(Company::GetNumItems() == 0);
		REQUIRE(Station::GetNumItems() == 0);
		ResetVehicleHash();
		AllocateMap(64, 64);
		_current_company = Company::Create()->index;
		for (uint x = 9; x <= 12; x++) MakeRailNormal(TileXY(x, 10), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		station = Station::Create(TileXY(12, 10));
		station->owner = _current_company;
		station->facilities = {StationFacility::Train};
		station->train_station = TileArea(station->xy, 1, 1);
		station->rect.BeforeAddTile(station->xy, StationRect::ADD_FORCE);
		MakeRailStation(station->xy, _current_company, station->index, Axis::X, 0, RAILTYPE_RAIL);
		moving = MakeTrain();
		waiting = MakeTrain();
		Place(moving, 10);
		Place(waiting, 12);
		for (Train *train : {moving, waiting}) {
			train->SetFrontEngine();
			train->owner = _current_company;
			train->track = TRACK_BIT_X;
			train->direction = Direction::SW;
			train->compatible_railtypes.Set(RAILTYPE_RAIL);
			train->railtypes.Set(RAILTYPE_RAIL);
			train->vcache.cached_max_speed = 120;
			train->gcache.cached_total_length = VEHICLE_LENGTH;
		}
		moving->current_order.MakeGoToCouple();
		moving->current_order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
		waiting->current_order.MakeWaitCouple();
		waiting->current_order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
		waiting->last_station_visited = station->index;
		/* Both consists have their current tile reserved. The waiting train
		 * occupies the only exit line, exactly where the approach starts. */
		REQUIRE(TryReserveRailTrack(moving->tile, TRACK_X));
		REQUIRE(TryReserveRailTrack(waiting->tile, TRACK_X));
	}

	~CoupleBlockingPartnerFixture()
	{
		_vehicle_pool.CleanPool();
		ResetVehicleHash();
		_orderlist_pool.CleanPool();
		_station_pool.CleanPool();
		_company_pool.CleanPool();
		ClearOrderDestinationRefcountMap();
		DeallocateMap();
	}

	void Place(Train *train, uint x)
	{
		train->tile = TileXY(x, 10);
		train->x_pos = x * TILE_SIZE + 8;
		train->y_pos = 10 * TILE_SIZE + 8;
		train->z_pos = 0;
		UpdateVehicleTileHash(train, false);
	}
};

TEST_CASE_METHOD(CoupleBlockingPartnerFixture, "Couple - goto couple claims a waiting partner blocking the line")
{
	GameRandomSeedChecker random_checker;
	ClearCoupleSearchLastFail();
	/* The first extension stops at the waiting train before the target search
	 * has run. The approach must claim that partner and reserve up to it. */
	REQUIRE(TryPathReserve(moving));
	CHECK(moving->couple_target == waiting->index);
	CHECK(GetCoupleClaimant(waiting) == moving);
	CHECK(GetRailReservationTrackBits(TileXY(11, 10)) != TRACK_BIT_NONE);
	CHECK(random_checker.Check());
	for (Train *train : {moving, waiting}) CHECK(ValidateVehicleTileHash(train));
}
