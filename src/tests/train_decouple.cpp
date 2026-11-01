/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file train_decouple.cpp Regression tests for read-only coupling checks and decoupling cuts. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../train.h"
#include "../clear_map.h"
#include "../debug.h"
#include "../direction_func.h"
#include "../cargotype.h"
#include "../tracerestrict_overtaking.h"
#include "../tracerestrict_cmd.h"
#include "../date_func.h"
#include "../train_cmd.h"
#include "../company_base.h"
#include "../company_func.h"
#include "../core/pool_func.hpp"
#include "../station_base.h"
#include "../waypoint_base.h"
#include "../pathfinder/yapf/yapf.h"
#include "../pbs.h"
#include "../pathfinder/follow_track.hpp"
#include "../aircraft.h"
#include "../engine_base.h"
#include "../newgrf.h"
#include "../newgrf_newsignals.h"
#include "../newgrf_spritegroup.h"
#include "../settings_type.h"
#include "../core/random_func.hpp"
#include "../core/backup_type.hpp"
#include "../map_func.h"
#include "../station_map.h"
#include "../rail_map.h"
#include "../rail_cmd.h"
#include "../signal_func.h"
#include "../bridge_map.h"
#include "../depot_base.h"
#include "../roadveh_transport.h"
#include "../scope.h"
#include "../gfx_type.h"
#include "../sl/saveload.h"
#include "../sl/saveload_func.h"
#include "../sl/saveload_filter.h"
#include "../sl/saveload_func.h"
#include "../strings_func.h"

extern std::string _savegame_format;
extern bool ValidateVehicleTileHash(const Vehicle *v);
extern void UpdateVehicleTileHash(Vehicle *v, bool remove);
extern const Order *_choose_train_track_saved_current_order;

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

TEST_CASE_METHOD(TrainDecoupleFixture, "Decouple - a pending partner is excluded only when its whole body is behind the departing front")
{
	const Direction moving = GENERATE(Direction::NE, Direction::SE, Direction::SW, Direction::NW);
	const bool backwards = GENERATE(false, true);
	AllocateMap(64, 64);
	auto cleanup = scope_guard([] { DeallocateMap(); });
	Train *front = MakeTrain(), *partner = MakeTrain();
	const TileIndex tile = TileXY(20, 20);
	const TileIndexDiffC delta = TileIndexDiffCByDir(moving);
	const Axis axis = DiagDirToAxis(DirToDiagDir(moving));
	const TrackBits track = axis == Axis::X ? TRACK_BIT_X : TRACK_BIT_Y;
	MakeRailStation(tile, OWNER_NONE, StationID{0}, axis, 0, RAILTYPE_RAIL);
	front->SetFrontEngine(); partner->SetFrontEngine();
	front->tile = partner->tile = tile; front->track = partner->track = track;
	front->direction = backwards ? ReverseDir(moving) : moving;
	front->vehicle_flags.Set(VehicleFlag::DrivingBackwards, backwards);
	partner->direction = ReverseDir(moving);
	front->flags.Set(VehicleRailFlag::JustDecoupled);
	partner->flags.Set(VehicleRailFlag::SameDirDecouplePending);
	front->gcache.cached_veh_length = partner->gcache.cached_veh_length = 2;
	front->x_pos = 20 * TILE_SIZE + (delta.x < 0 ? 4 : delta.x > 0 ? 12 : 8);
	front->y_pos = 20 * TILE_SIZE + (delta.y < 0 ? 4 : delta.y > 0 ? 12 : 8);
	front->z_pos = partner->z_pos = 0;
	partner->x_pos = front->x_pos - 11 * delta.x;
	partner->y_pos = front->y_pos - 11 * delta.y;
	bool behind = true;
	SECTION("the asymmetric split flags and a physically separated rear") {}
	SECTION("a foreign part ahead still blocks") {
		front->x_pos = 20 * TILE_SIZE + (delta.x < 0 ? 12 : delta.x > 0 ? 4 : 8);
		front->y_pos = 20 * TILE_SIZE + (delta.y < 0 ? 12 : delta.y > 0 ? 4 : 8);
		partner->x_pos = front->x_pos + 11 * delta.x;
		partner->y_pos = front->y_pos + 11 * delta.y;
		behind = false;
	}
	SECTION("an overlapping part remains protected") {
		partner->x_pos = front->x_pos - delta.x;
		partner->y_pos = front->y_pos - delta.y;
		behind = false;
	}
	SECTION("an odd length rounds the required separation upwards") {
		partner->gcache.cached_veh_length = 3;
		partner->x_pos = front->x_pos - 2 * delta.x;
		partner->y_pos = front->y_pos - 2 * delta.y;
		behind = false;
	}
	SECTION("another rail axis cannot use the exception") { partner->track = track == TRACK_BIT_X ? TRACK_BIT_Y : TRACK_BIT_X; behind = false; }
	SECTION("an offset parallel vehicle is not on the same rail line") { partner->x_pos += delta.y; partner->y_pos -= delta.x; behind = false; }
	SECTION("another height cannot use the exception") { partner->z_pos++; behind = false; }
	SECTION("a different tile cannot use the origin exception") { partner->tile = TileXY(21, 20); behind = false; }
	SECTION("an unrelated ordinary stopped train remains protected") { partner->flags.Reset(VehicleRailFlag::SameDirDecouplePending); behind = false; }
	SECTION("a departed train cannot keep using the exception") { front->flags.Reset(VehicleRailFlag::JustDecoupled); behind = false; }
	SECTION("the exception is unavailable on ordinary rail") { MakeRailNormal(tile, OWNER_NONE, track, RAILTYPE_RAIL); behind = false; }
	SECTION("a curve is left to the native blocker") { front->direction = backwards ? Direction::W : Direction::E; behind = false; }
	const auto before_front = front->flags, before_partner = partner->flags;
	const auto before_direction = front->direction;
	GameRandomSeedChecker random;
	CHECK(IsDecoupledPartBehindDeparture(front, partner) == behind);
	CHECK(front->flags == before_front); CHECK(partner->flags == before_partner);
	CHECK(front->direction == before_direction); CHECK(random.Check());
}

#include "coupling_trial_orientation.inc"

TEST_CASE_METHOD(TrainDecoupleFixture, "Train relocation - crossing a tile during spacing preserves rail and physical facing")
{
	AllocateMap(64, 64);
	ResetVehicleHash();
	auto cleanup = scope_guard([]() { _vehicle_pool.CleanPool(); ResetVehicleHash(); DeallocateMap(); });
	Train *train = MakeTrain();
	train->SetFrontEngine();
	train->owner = CompanyID{0};
	train->direction = GENERATE(Direction::NE, Direction::SE, Direction::SW, Direction::NW);
	train->track = (train->direction == Direction::NE || train->direction == Direction::SW) ? TRACK_BIT_X : TRACK_BIT_Y;
	train->vehicle_flags.Set(VehicleFlag::DrivingBackwards, GENERATE(false, true));
	train->flags.Set(VehicleRailFlag::Flipped);
	const bool forward_adjustment = GENERATE(false, true);
	const TileIndexDiffC motion = TileIndexDiffCByDir(train->GetMovingDirection());
	const int step_x = motion.x * (forward_adjustment ? 1 : -1);
	const int step_y = motion.y * (forward_adjustment ? 1 : -1);
	train->x_pos = step_x == 0 ? 10 * TILE_SIZE + 8 : 11 * TILE_SIZE - (step_x > 0 ? 1 : 0);
	train->y_pos = step_y == 0 ? 10 * TILE_SIZE + 8 : 11 * TILE_SIZE - (step_y > 0 ? 1 : 0);
	train->tile = TileVirtXY(train->x_pos, train->y_pos);
	const TileIndex destination = TileVirtXY(train->x_pos + step_x, train->y_pos + step_y);
	for (TileIndex tile : {train->tile, destination}) MakeRailNormal(tile, train->owner, TRACK_BIT_X | TRACK_BIT_Y, RAILTYPE_RAIL);
	train->UpdatePosition();
	const Direction facing = train->direction;
	const TrackBits track = train->track;
	const Trackdir td = train->GetVehicleTrackdir();
	const auto flags = train->flags;
	GameRandomSeedChecker random;
	TrainMoveToPosition(train, train->x_pos + step_x, train->y_pos + step_y);
	CHECK(train->tile == destination);
	CHECK(train->direction == facing);
	CHECK(train->track == track);
	CHECK(train->GetVehicleTrackdir() == td);
	CHECK(train->flags == flags);
	CHECK(ValidateVehicleTileHash(train));
	CHECK(random.Check());
}

TEST_CASE_METHOD(TrainDecoupleFixture, "Transport - batch fill preserves per-part rounding, cargo filters and live transport changes")
{
	AutoRestoreBackup weight0(CargoSpec::Get(0)->weight, uint8_t{32});
	AutoRestoreBackup weight1(CargoSpec::Get(1)->weight, uint8_t{16});
	Train *head = MakeTrain(), *tail = MakeTrain(), *other = MakeTrain();
	head->SetNext(tail);
	head->cargo_type = CargoType{0}; head->cargo_cap = 10;
	tail->cargo_type = CargoType{1}; tail->cargo_cap = 20;
	head->cargo.Append(CargoPacket::Create(uint16_t{3},uint16_t{0},StationID::Invalid(),TileIndex{0},Money{0}));
	tail->cargo.Append(CargoPacket::Create(uint16_t{1},uint16_t{0},StationID::Invalid(),TileIndex{0},Money{0}));
	Train *a = MakeTrain(), *b = MakeTrain(), *c = MakeTrain(), *unrelated = MakeTrain();
	const uint16_t tonnes = GENERATE(0,1,3,17,200);
	for (Train *v : {a,b,c,unrelated}) v->rv_transport_flags = RVTF_TRANSPORTED;
	a->transported_host_part = b->transported_host_part = head->index;
	a->transported_weight = tonnes; b->transported_weight = 1;
	c->transported_host_part = tail->index; c->transported_weight = 3;
	unrelated->transported_host_part = other->index; unrelated->transported_weight = 1000;
	const uint32_t expected_head = std::min<uint32_t>(3 + (tonnes + 1) / 2, 10);
	GameRandomSeedChecker random_checker;
	CHECK(RVTransportGetConsistCargoAmount(nullptr) == 0);
	CHECK(RVTransportGetConsistCargoAmount(head) == expected_head + 4);
	CHECK(RVTransportGetConsistCargoAmount(tail) == expected_head + 4);
	CHECK(RVTransportGetConsistCargoAmount(tail,CargoType{0}) == expected_head);
	CHECK(RVTransportGetConsistCargoAmount(tail,CargoType{1}) == 4);
	CHECK(RVTransportGetConsistCargoAmount(head,CargoType{2}) == 0);
	CHECK(RVTransportGetConsistCargoAmount(head) == RVTransportGetPartCargoAmount(head) + RVTransportGetPartCargoAmount(tail));
	CHECK(CalcPercentVehicleFilled(head,nullptr) == CeilDiv((expected_head + 4) * 100, 30));
	CHECK(CalcPercentVehicleFilledOfCargo(tail,CargoType{0}) == expected_head * 10);
	a->rv_transport_flags = b->rv_transport_flags = 0;
	CHECK(RVTransportGetConsistCargoAmount(head) == 7);
	CHECK(CalcPercentVehicleFilledOfCargo(head,CargoType{0}) == 30);
	c->transported_host_part = other->index;
	CHECK(RVTransportGetConsistCargoAmount(head) == 4);
	CargoSpec::Get(0)->weight = 0;
	a->rv_transport_flags = RVTF_TRANSPORTED;
	CHECK(RVTransportGetConsistCargoAmount(head) == 4);
	head->cargo_type = INVALID_CARGO;
	CHECK(RVTransportGetConsistCargoAmount(head,INVALID_CARGO) == 3);
	CHECK(random_checker.Check());
	for (Train *v : {a,b,c,unrelated}) v->rv_transport_flags = 0;
}

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
	AutoRestoreBackup<bool> turns_backup{_settings_game.pf.forbid_90_deg, AutoRestoreBackupNoNewValueTag{}};
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

// Existing regressions deliberately exercise their saved 25% configuration.
static constexpr uint32_t TEST_YIELD_CONFIG = 25u << 16;
class AutomaticYieldFixture : public TrainDecoupleFixture {
protected:
	AutoRestoreBackup<bool> automatic_yield_setting{_settings_game.vehicle.automatic_yield, true};
	AutoRestoreBackup<int8_t> diagnostic_level{_debug_levels[size_t(DebugLevelID::yapf)], std::getenv("PXP_YIELD_TEST_DEBUG") ? int8_t{7} : _debug_levels[size_t(DebugLevelID::yapf)]};
	AutoRestoreBackup<CompanyID> company_backup{_current_company, AutoRestoreBackupNoNewValueTag{}};
	AutoRestoreBackup<uint8_t> length_limit{_settings_game.vehicle.max_train_length, uint8_t{8}};
	AutoRestoreBackup<RailTypes> rail_direct{const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->compatible_railtypes, RailTypes{RAILTYPE_RAIL}};
	AutoRestoreBackup<RailTypes> rail_power{const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->powered_railtypes, RailTypes{RAILTYPE_RAIL}};
	AutoRestoreBackup<RailTypes> rail_indirect{const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->indirect_compatible_railtypes, RailTypes{RAILTYPE_RAIL}};
	Train *slow, *medium, *fast;
	TraceRestrictSlot *membership;
	AutomaticYieldFixture(uint map_width = 64)
	{
		REQUIRE(Company::GetNumItems() == 0);
		REQUIRE(TraceRestrictSlot::GetNumItems() == 0);
		REQUIRE(_tracerestrictprogram_mapping.empty());
		ResetVehicleHash();
		AllocateMap(map_width, 64);
		_current_company = Company::Create()->index;
		membership = TraceRestrictSlot::Create(_current_company);
		membership->name = "Test yielding area";
		membership->max_occupancy = 16;
		for (uint x = 1; x <= 40; ++x) MakeRailNormal(TileXY(x, 10), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		/* Model the next native PBS block beyond the area Clear at 27.
		 * A released occupant must clear that block, not just the PXP marker. */
		SB(_m[TileXY(33,10)].m5,6,2,to_underlying(RailTileType::Signals));
		SetPresentSignals(TileXY(33,10),SignalAlongTrackdir(TRACKDIR_X_SW));
		SetSignalType(TileXY(33,10),TRACK_X,SignalType::Path);
		for (uint x = 8; x <= 20; ++x) MakeRailNormal(TileXY(x, 12), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(8, 10), _current_company, TRACK_BIT_X | TRACK_BIT_RIGHT, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(8, 11), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(8, 12), _current_company, TRACK_BIT_LEFT, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20, 12), _current_company, TRACK_BIT_UPPER, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20, 11), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20, 10), _current_company, TRACK_BIT_X | TRACK_BIT_LOWER, RAILTYPE_RAIL);
		slow = MakeActor(7, 10, 100);
		medium = MakeActor(5, 10, 160);
		fast = MakeActor(4, 10, 200);
		Marker(3, 10, AutoYieldRole::Approach);
		Marker(10, 10, AutoYieldRole::Main);
		Marker(10, 12, AutoYieldRole::Siding);
		Marker(18, 12, AutoYieldRole::Exit);
		Marker(27, 10, AutoYieldRole::Clear);
	}
	~AutomaticYieldFixture()
	{
		ClearTraceRestrictMapping();
		_tracerestrictprogram_pool.CleanPool();
		_tracerestrictslot_pool.CleanPool();
		_vehicle_pool.CleanPool();
		ResetVehicleHash();
		_station_pool.CleanPool();
		_company_pool.CleanPool();
		DeallocateMap();
	}
	Train *MakeActor(uint x, uint y, uint16_t speed)
	{
		Train *v = MakeTrain();
		v->SetFrontEngine(); v->owner = _current_company;
		v->track = TRACK_BIT_X; v->direction = Direction::SW;
		v->compatible_railtypes.Set(RAILTYPE_RAIL); v->railtypes.Set(RAILTYPE_RAIL);
		v->gcache.cached_total_length = VEHICLE_LENGTH; v->vcache.cached_max_speed = speed;
		Place(v, x, y);
		return v;
	}
	void Place(Train *v, uint x, uint y)
	{
		v->tile = TileXY(x,y); v->x_pos = x * TILE_SIZE + 8; v->y_pos = y * TILE_SIZE + 8; v->z_pos = 0;
		UpdateVehicleTileHash(v, false);
	}
	TraceRestrictProgram *Marker(uint x, uint y, AutoYieldRole role, uint16_t area = INVALID_TRACE_RESTRICT_SLOT_ID.base(), uint32_t config = TEST_YIELD_CONFIG)
	{
		TileIndex tile = TileXY(x,y);
		MakeRailNormal(tile, _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		SB(_m[tile].m5, 6, 2, to_underlying(RailTileType::Signals));
		SetPresentSignals(tile, SignalAlongTrackdir(TRACKDIR_X_SW));
		SetSignalType(tile, TRACK_X, SignalType::Path);
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(tile, TRACK_X), true);
		TraceRestrictInstructionItem item{0}; item.SetType(role == AutoYieldRole::Clear ? TRIT_AUTO_YIELD_CLEAR : TRIT_AUTO_YIELD_AREA);
		item.SetValue(area == INVALID_TRACE_RESTRICT_SLOT_ID.base() ? membership->index.base() : area);
		if (role != AutoYieldRole::Clear) item.SetAuxField(to_underlying(role));
		program->items = {item.AsProgramItem()};
		if (role != AutoYieldRole::Clear) program->items.emplace_back(config);
		REQUIRE(program->Validate().Succeeded());
		InvalidateAutoYieldAreas();
		UpdateAutoYieldAreas();
		return program;
	}
	TraceRestrictSlot *Resource(uint32_t ref)
	{
		if (ref == AUTO_YIELD_DETECTION_REF) return membership;
		for (TraceRestrictSlot *slot : TraceRestrictSlot::Iterate()) if (slot->automatic_yield_area == membership->index.base() + 1 && slot->automatic_yield_signal == ref && !slot->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets)) return slot;
		return nullptr;
	}
	TraceRestrictProgramResult Execute(Train *v, uint x, uint y, TraceRestrictProgramInputSlotPermissions permissions = TRPISP_NONE)
	{
		TraceRestrictProgramInput input(TileXY(x,y), TRACKDIR_X_SW, nullptr, nullptr);
		input.permitted_slot_operations = permissions;
		TraceRestrictProgramResult out;
		GetExistingTraceRestrictProgram(input.tile, TRACK_X)->Execute(v, input, out);
		return out;
	}
	void Register(Train *v) { Execute(v, 3, 10, TRPISP_PBS_RES_END_ACQUIRE); Execute(v, 3, 10, TRPISP_RELEASE_FRONT); }
	/** Selection tests need a genuinely distant moving express: the old four
	 * tile gap now correctly fails the independent turnover-risk check. */
	void DistantExpress(Train *v, uint x = 2, uint entry_row = 10)
	{
		_settings_game.pf.forbid_90_deg = true;
		for (uint i = 2; i < 60; i++) {
			MakeRailNormal(TileXY(i,22),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
			MakeRailNormal(TileXY(i,4),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		}
		for (uint y = 5; y < 22; y++) MakeRailNormal(TileXY(60,y),_current_company,y==entry_row?TRACK_BIT_X|TRACK_BIT_Y:TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(60,22),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(60,4),_current_company,TRACK_BIT_RIGHT,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(1,4),_current_company,TRACK_BIT_LOWER,RAILTYPE_RAIL);
		for (uint y = 5; y < entry_row; y++) MakeRailNormal(TileXY(1,y),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(1,entry_row),_current_company,TRACK_BIT_LEFT,RAILTYPE_RAIL);
		Place(v,x+23,22);
	}
	void BackSignal(uint x, SignalType type)
	{
		TileIndex tile = TileXY(x, 12);
		MakeRailNormal(tile, _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		SB(_m[tile].m5, 6, 2, to_underlying(RailTileType::Signals));
		SetPresentSignals(tile, SignalAlongTrackdir(TRACKDIR_X_NE));
		SetSignalType(tile, TRACK_X, type);
	}
	void UpperSiding(bool exit_only)
	{
		MakeRailNormal(TileXY(8,10), _current_company, TRACK_BIT_X | TRACK_BIT_RIGHT | TRACK_BIT_UPPER, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(8,9), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(8,8), _current_company, TRACK_BIT_LOWER, RAILTYPE_RAIL);
		for (uint x = 9; x < 20; x++) MakeRailNormal(TileXY(x,8), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20,8), _current_company, TRACK_BIT_RIGHT, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20,9), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20,10), _current_company, TRACK_BIT_X | TRACK_BIT_LOWER | TRACK_BIT_LEFT, RAILTYPE_RAIL);
		if (!exit_only) Marker(10,8,AutoYieldRole::Siding);
		Marker(18,8,AutoYieldRole::Exit);
	}
	void UseExitOnly()
	{
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(10,12), TRACK_X), false);
		program->items.clear();
		MakeRailNormal(TileXY(10,12), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		InvalidateAutoYieldAreas();
		UpdateAutoYieldAreas();
	}
};

#include "early_rail_route.inc"

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - ordinary slot associates signals and only lane claims are hidden")
{
	REQUIRE(TraceRestrictSlot::GetNumItems() == 4);
	CHECK(Resource(AUTO_YIELD_DETECTION_REF) != nullptr);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X)) != nullptr);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->max_occupancy == 1);
	GameRandomSeedChecker random_checker;
	const auto seed_order = slow->current_order.GetType();
	Execute(slow, 3, 10);
	CHECK(Resource(AUTO_YIELD_DETECTION_REF)->occupants.empty());
	CHECK(slow->current_order.GetType() == seed_order);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - faster following members deny main line without penalties")
{
	Register(slow); Register(medium);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::Faster));
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	const auto result = Execute(slow, 10, 10);
	CHECK((result.flags & TRPRF_DENY) != 0);
	CHECK(result.penalty == 0);
	CHECK((Execute(medium, 10, 10).flags & TRPRF_DENY) == 0);
	CHECK(GetRailReservationTrackBits(TileXY(10,12)) == TRACK_BIT_NONE);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - unavailable or unreachable sidings preserve ordinary passage")
{
	Register(slow); Register(medium);
	SECTION("siding occupied") { Place(fast, 14, 12); fast->vehstatus.Set(VehState::Stopped); }
	SECTION("train too long") { slow->gcache.cached_total_length = 20 * TILE_SIZE; }
	SECTION("branch already passed") { Place(slow, 12, 10); }
	SECTION("siding disconnected") { MakeRailNormal(TileXY(12,12), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL); }
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	const auto result = Execute(slow, 10, 10);
	CHECK((result.flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - priority ratio boundary and inactive candidates")
{
	Register(slow); Register(medium);
	bool expected = false;
	SECTION("exact 25 percent") { medium->vcache.cached_max_speed = 125; expected = true; }
	SECTION("below threshold") { medium->vcache.cached_max_speed = 124; }
	SECTION("instant speed does not matter") { medium->cur_speed = 0; expected = true; }
	SECTION("manually stopped") { medium->vehstatus.Set(VehState::Stopped); }
	SECTION("crashed") { medium->vehstatus.Set(VehState::Crashed); }
	SECTION("opposite direction") { medium->direction = Direction::NE; }
	SECTION("not registered") { Resource(AUTO_YIELD_DETECTION_REF)->Vacate(medium); }
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster) == expected);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - multiple faster members release after each tail clears")
{
	Register(slow); Register(medium); Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK_FALSE(EvaluateAutoYield(fast,membership->index.base(),AutoYieldCondition::MustWait));
	Place(fast,30,10);
	Train *tail = MakeActor(27,10,200); tail->SetWagon(); fast->SetNext(tail);
	Execute(fast,27,10,TRPISP_RELEASE_FRONT);
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::MustWait));
	Execute(fast,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	Place(medium,30,10); Execute(medium,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - admission stays open with transactional lane claims")
{
	Register(slow); Register(medium);
	TraceRestrictSlotTemporaryState temporary;
	temporary.PushToChangeStack();
	Execute(slow,10,12,TRPISP_ACQUIRE_TEMP_STATE);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->IsOccupant(slow->index));
	CHECK((Execute(fast,3,10,TRPISP_ACQUIRE).flags & TRPRF_WAIT_AT_PBS) == 0);
	CHECK_FALSE(Resource(AUTO_YIELD_DETECTION_REF)->IsOccupant(fast->index));
	temporary.PopFromChangeStackRevertTemporaryChanges(slow->index);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->occupants.empty());
	Execute(slow,10,12,TRPISP_ACQUIRE);
	CHECK((Execute(medium,10,12).flags & TRPRF_DENY) != 0);
	CHECK((Execute(medium,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - full siding can safely vacate for a slower arrival")
{
	Register(slow); Register(medium); Register(fast);
	Place(medium,14,12); Execute(medium,10,12,TRPISP_ACQUIRE);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(medium,18,12).flags & TRPRF_WAIT_AT_PBS) == 0);
	CHECK((Execute(slow,10,10).flags & TRPRF_WAIT_AT_PBS) != 0);
	Place(medium,30,10); Execute(medium,27,10,TRPISP_RELEASE_BACK);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield free lanes - future capacity demand cannot evict an occupied lane while normal parking is free")
{
	UpperSiding(false);
	Register(slow);Register(medium);Register(fast);
	Place(medium,14,12);Execute(medium,10,12,TRPISP_ACQUIRE);
	DistantExpress(fast);fast->cur_speed=200;
	REQUIRE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	CHECK((Execute(medium,18,12).flags&TRPRF_WAIT_AT_PBS)!=0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - manual cancellation disables all conditions and frees the lane immediately")
{
	Register(slow); Register(medium);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	auto *lane = Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X));
	REQUIRE(lane->IsOccupant(slow->index));
	GameRandomSeedChecker random_checker;
	SECTION("current registry") {}
	SECTION("configuration edit pending rebuild") { InvalidateAutoYieldAreas(); }
	REQUIRE(CmdRemoveVehicleTraceRestrictSlot(DoCommandFlag::Execute, membership->index, slow->index).Succeeded());
	CHECK_FALSE(membership->IsOccupant(slow->index));
	CHECK(lane->occupants.empty());
	UpdateAutoYieldAreas();
	for (uint8_t c = 0; c <= to_underlying(AutoYieldCondition::MayLeave); c++) {
		CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), static_cast<AutoYieldCondition>(c)));
		CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), static_cast<AutoYieldCondition>(c), true));
	}
	for (const auto &[x, y] : {std::pair{10,10}, {10,12}, {18,12}}) {
		CHECK((Execute(slow, x, y, TRPISP_ACQUIRE).flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) == 0);
	}
	CHECK(lane->occupants.empty());
	CHECK(membership->IsOccupant(medium->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(GetRailReservationTrackBits(TileXY(10,12)) == TRACK_BIT_NONE);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - slot zero and unconfigured role defaults are valid")
{
	REQUIRE(membership->index.base() == 0);
	for (uint8_t role = 0; role <= to_underlying(AutoYieldRole::Exit); role++) {
		TraceRestrictInstructionItem storage{};
		TraceRestrictInstructionItemRef item(storage);
		SetTraceRestrictTypeAndNormalise(item, TRIT_AUTO_YIELD_AREA, role);
		CHECK(item.GetValueAsSlot() == INVALID_TRACE_RESTRICT_SLOT_ID);
		CHECK(item.GetAuxField() == role);
		CHECK(GetTraceRestrictTypeProperties(item).value_type == TRVT_SLOT_INDEX);
	}
	Register(slow); Register(medium);
	CHECK(EvaluateAutoYield(slow, 0, AutoYieldCondition::Faster));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - reservation registration commits or rolls back and respects capacity")
{
	membership->max_occupancy = 1;
	TraceRestrictSlotTemporaryState temporary;
	temporary.PushToChangeStack();
	Execute(slow,3,10,TRPISP_ACQUIRE_TEMP_STATE);
	CHECK(membership->IsOccupant(slow->index));
	temporary.PopFromChangeStackRevertTemporaryChanges(slow->index);
	CHECK(membership->occupants.empty());
	temporary.PushToChangeStack();
	Execute(slow,3,10,TRPISP_ACQUIRE_TEMP_STATE);
	temporary.PopFromChangeStackApplyTemporaryChanges(slow);
	CHECK(membership->IsOccupant(slow->index));
	Execute(slow,3,10,TRPISP_ACQUIRE);
	Execute(slow,3,10,TRPISP_PBS_RES_END_ACQUIRE);
	CHECK(membership->occupants.size()==1);
	Register(slow);
	CHECK((Execute(medium,3,10,TRPISP_ACQUIRE).flags & TRPRF_WAIT_AT_PBS) != 0);
	CHECK_FALSE(membership->IsOccupant(medium->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - committing a transactional release clears its lane while rollback preserves it")
{
	Register(slow);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	auto *lane = Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X));
	TraceRestrictSlotTemporaryState temporary;
	temporary.PushToChangeStack();
	membership->VacateUsingTemporaryState(slow->index, &temporary);
	temporary.PopFromChangeStackRevertTemporaryChanges(slow->index);
	CHECK(membership->IsOccupant(slow->index));
	CHECK(lane->IsOccupant(slow->index));
	temporary.PushToChangeStack();
	membership->VacateUsingTemporaryState(slow->index, &temporary);
	temporary.PopFromChangeStackApplyTemporaryChanges(slow);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	CHECK_FALSE(lane->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - global switch disables conditions and claims while tail cleanup remains available")
{
	Register(slow); Register(medium);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	_settings_game.vehicle.automatic_yield = false;
	AutoYieldSettingsChanged();
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->occupants.empty());
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::Faster));
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::Faster, true));
	CHECK((Execute(slow,10,10).flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) == 0);
	Register(fast);
	CHECK_FALSE(membership->IsOccupant(fast->index));
	Execute(slow,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	_settings_game.vehicle.automatic_yield = true;
	AutoYieldSettingsChanged();
	CHECK(membership->IsOccupant(medium->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - deleting the user slot clears every reference before its ID is reused")
{
	Register(slow); Register(medium);
	const auto id = membership->index;
	TraceRestrictRemoveSlotID(id);
	delete membership;
	membership = nullptr;
	for (TraceRestrictProgram *program : TraceRestrictProgram::Iterate()) {
		CHECK(program->Validate().Succeeded());
		for (auto item : program->IterateInstructions()) CHECK(item.Instruction().GetValueAsSlot() == INVALID_TRACE_RESTRICT_SLOT_ID);
	}
	UpdateAutoYieldAreas();
	CHECK(TraceRestrictSlot::GetNumItems() == 0);
	CHECK_FALSE(EvaluateAutoYield(slow, id.base(), AutoYieldCondition::Faster));
	membership = TraceRestrictSlot::Create(_current_company);
	CHECK(membership->index == id);
	CHECK_FALSE(EvaluateAutoYield(slow, id.base(), AutoYieldCondition::MayLeave, true));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - version one conversion keeps source membership and lane claims")
{
	Register(slow); Register(medium);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	TraceRestrictSlot *lane = Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X));
	const auto source_id = membership->index;
	TraceRestrictSlot *pending=Resource(AUTO_YIELD_PENDING_REF);
	REQUIRE(pending != nullptr); TraceRestrictRemoveSlotID(pending->index); delete pending;
	/* Version one had no saved wait-target resources. */
	std::vector<TraceRestrictSlot *> target_resources;
	for (auto *slot : TraceRestrictSlot::Iterate()) if (slot->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets)) target_resources.push_back(slot);
	for (auto *slot : target_resources) { TraceRestrictRemoveSlotID(slot->index); delete slot; }
	/* Simulate v1's numeric area, deliberately different from the slot ID. */
	for (TraceRestrictProgram *program : TraceRestrictProgram::Iterate()) {
		for (auto iter : program->IterateInstructionsMutable()) iter.InstructionRef().SetValue(42);
	}
	membership->automatic_yield_area = 42;
	lane->automatic_yield_area = 42;
	REQUIRE(MigrateAutoYieldAreasToSlots());
	UpdateAutoYieldAreas();
	CHECK(membership->index == source_id);
	CHECK(membership->automatic_yield_area == 0);
	CHECK(membership->IsOccupant(slow->index));
	CHECK(membership->IsOccupant(medium->index));
	CHECK(lane->IsOccupant(slow->index));
	CHECK(lane->automatic_yield_area == source_id.base() + 1);
	for (const TraceRestrictProgram *program : TraceRestrictProgram::Iterate()) {
		for (auto iter : program->IterateInstructions()) CHECK(iter.Instruction().GetValueAsSlot() == source_id);
	}
	CHECK(EvaluateAutoYield(slow, source_id.base(), AutoYieldCondition::MustWait));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - cancelled membership cannot trigger a negated program condition")
{
	Register(slow); Register(medium);
	REQUIRE(CmdRemoveVehicleTraceRestrictSlot(DoCommandFlag::Execute, membership->index, slow->index).Succeeded());
	TraceRestrictProgram *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(10,10), TRACK_X), false);
	TraceRestrictInstructionItem condition{};
	condition.SetType(TRIT_COND_AUTO_YIELD_FREE);
	condition.SetValue(membership->index);
	condition.SetCondOp(TRCO_ISNOT);
	TraceRestrictInstructionItem deny{};
	deny.SetType(TRIT_PF_DENY);
	TraceRestrictInstructionItem end{};
	end.SetType(TRIT_COND_ENDIF);
	program->items.insert(program->items.begin(), {condition.AsProgramItem(), deny.AsProgramItem(), end.AsProgramItem()});
	REQUIRE(program->Validate().Succeeded());
	CHECK((Execute(slow,10,10).flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - snapshot and registry rebuild retain membership and opt out")
{
	Register(slow); Register(medium); Register(fast);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	REQUIRE(CmdRemoveVehicleTraceRestrictSlot(DoCommandFlag::Execute, membership->index, fast->index).Succeeded());
	AutoRestoreBackup format(_savegame_format, std::string{"none"});
	AutoRestoreBackup cursor(_cursor.sprites, std::vector<CursorSprite>{{0, 0, 0, 0}});
	auto snapshot = std::make_shared<CoupleSnapshotCapture>();
	REQUIRE(SaveWithFilter(snapshot, false, SMF_NET_SERVER) == SaveLoadResult::Ok);
	for (const std::string &name : std::array<std::string, 5>{"pxp_automatic_yield", "vehicle.automatic_yield", "automatic_yield_area", "automatic_yield_signal", membership->name}) {
		CHECK(std::search(snapshot->bytes.begin(), snapshot->bytes.end(), name.begin(), name.end()) != snapshot->bytes.end());
	}
	const auto count = TraceRestrictSlot::GetNumItems();
	ResetAutoYieldAreas();
	UpdateAutoYieldAreas();
	CHECK(TraceRestrictSlot::GetNumItems() == count);
	CHECK(membership->IsOccupant(slow->index));
	CHECK(membership->IsOccupant(medium->index));
	CHECK_FALSE(membership->IsOccupant(fast->index));
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->IsOccupant(slow->index));
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::MustWait));
	CHECK_FALSE(EvaluateAutoYield(fast, membership->index.base(), AutoYieldCondition::MayLeave));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - tail cleanup survives participant overflow and incomplete configuration")
{
	Register(slow); Register(medium); Register(fast);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	SECTION("participant overflow") {
		membership->max_occupancy = 32;
		for (uint i = 0; i < 14; i++) REQUIRE(membership->Occupy(MakeActor(2,10,100)));
		REQUIRE(membership->occupants.size() == 17);
	}
	SECTION("missing main marker") {
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(10,10), TRACK_X), false);
		program->items.clear();
		InvalidateAutoYieldAreas();
		UpdateAutoYieldAreas();
	}
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::MustWait));
	Execute(slow,27,10,TRPISP_RELEASE_FRONT);
	CHECK(membership->IsOccupant(slow->index));
	Execute(slow,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	std::vector<TraceRestrictSlotID> held;
	TraceRestrictGetVehicleSlots(slow->index, held);
	CHECK(held.empty());
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - an exit alone identifies a siding and claims only after PBS reservation")
{
	UseExitOnly();
	Register(slow); Register(medium);
	const auto exit_ref = MakeTraceRestrictRefId(TileXY(18,12),TRACK_X);
	TraceRestrictSlot *lane = Resource(exit_ref);
	REQUIRE(lane != nullptr);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X)) == nullptr);
	CHECK(TraceRestrictSlot::GetNumItems() == 4);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	CHECK((Execute(slow,18,12,TRPISP_PBS_RES_END_ACQ_DRY).flags & TRPRF_PBS_RES_END_WAIT) == 0);
	CHECK(lane->occupants.empty());
	Execute(slow,18,12,TRPISP_PBS_RES_END_ACQUIRE);
	CHECK(lane->IsOccupant(slow->index));
	const auto competing = Execute(medium,18,12,TRPISP_PBS_RES_END_ACQ_DRY);
	CHECK((competing.flags & TRPRF_PBS_RES_END_WAIT) != 0);
	CHECK((competing.flags & TRPRF_DENY) != 0);
	CHECK_FALSE(lane->IsOccupant(medium->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(GetRailReservationTrackBits(TileXY(18,12)) == TRACK_BIT_NONE);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - exit only temporary claims roll back and manual cancellation still releases")
{
	UseExitOnly();
	Register(slow); Register(medium);
	auto *lane = Resource(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X));
	TraceRestrictSlotTemporaryState temporary;
	temporary.PushToChangeStack();
	Execute(slow,18,12,TRPISP_ACQUIRE_TEMP_STATE);
	CHECK(lane->IsOccupant(slow->index));
	temporary.PopFromChangeStackRevertTemporaryChanges(slow->index);
	CHECK(lane->occupants.empty());
	Execute(slow,18,12,TRPISP_PBS_RES_END_ACQUIRE);
	REQUIRE(lane->IsOccupant(slow->index));
	REQUIRE(CmdRemoveVehicleTraceRestrictSlot(DoCommandFlag::Execute, membership->index, slow->index).Succeeded());
	CHECK(lane->occupants.empty());
	CHECK((Execute(slow,18,12).flags & TRPRF_WAIT_AT_PBS) == 0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - exit only rejects a marked main boundary and unsuitable parking segments")
{
	UseExitOnly();
	Register(slow); Register(medium);
	SECTION("train longer than exclusive branch") { slow->gcache.cached_total_length = 20 * TILE_SIZE; }
	SECTION("exit on main line before merge") {
		GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X),false)->items.clear();
		Marker(16,10,AutoYieldRole::Exit);
	}
	SECTION("exit after a merge marked as main") {
		GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X),false)->items.clear();
		Marker(24,10,AutoYieldRole::Exit);
		Marker(20,10,AutoYieldRole::Main);
		SetTrackBits(TileXY(20,10), TRACK_BIT_X | TRACK_BIT_LOWER);
	}
	SECTION("unreachable branch") { MakeRailNormal(TileXY(12,12),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL); }
	SECTION("occupied branch") { Place(fast,14,12); fast->vehstatus.Set(VehState::Stopped); }
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - an additional feeder can merge before an exclusive siding parking segment")
{
	UseExitOnly();
	MakeRailNormal(TileXY(14, 12), _current_company, TRACK_BIT_X | TRACK_BIT_LOWER, RAILTYPE_RAIL);
	MakeRailNormal(TileXY(14, 13), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
	MakeRailNormal(TileXY(14, 14), _current_company, TRACK_BIT_UPPER, RAILTYPE_RAIL);
	for (uint x = 10; x < 14; x++) MakeRailNormal(TileXY(x, 14), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
	bool available = true;
	SECTION("the last merge leaves enough independent parking length") { }
	SECTION("a declared exit after an unmarked merge remains a siding") {
		GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X),false)->items.clear();
		Marker(25,10,AutoYieldRole::Exit);
	}
	SECTION("a train must fit after the last merge") { slow->gcache.cached_total_length = 4 * TILE_SIZE; available = false; }
	SECTION("a compound junction is excluded from otherwise exclusive parking") { MakeRailNormal(TileXY(14, 12), _current_company, TRACK_BIT_X | TRACK_BIT_LOWER | TRACK_BIT_RIGHT, RAILTYPE_RAIL); }
	SECTION("a foreign reservation still blocks entry") { REQUIRE(TryReserveRailTrack(TileXY(17, 12), TRACK_X)); available = false; }
	SECTION("a train already past the branch cannot enter") { Place(slow, 16, 10); available = false; }
	Register(slow); Register(medium);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding) == available);
	CHECK(((Execute(slow, 10, 10).flags & TRPRF_DENY) != 0) == available);
	CHECK(GetReservedTrackbits(TileXY(18, 12)) == TRACK_BIT_NONE);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - one empty siding is not withheld for another approaching train")
{
	Register(slow); Register(medium); Register(fast);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::NeedsSiding));
	CHECK(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow, 10, 10).flags & TRPRF_DENY) != 0);
	SECTION("slow train already passed the branch") {
		Place(slow, 12, 10);
		CHECK(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::NeedsSiding));
	}
	SECTION("slow train cannot fit") {
		slow->gcache.cached_total_length = 20 * TILE_SIZE;
		CHECK(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::NeedsSiding));
	}
	SECTION("slow train ahead is no longer registered") {
		membership->Vacate(slow);
		CHECK(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::NeedsSiding));
	}
	CHECK(random_checker.Check());
	CHECK(GetRailReservationTrackBits(TileXY(10,12)) == TRACK_BIT_NONE);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - parallel curves on one tile do not hide a siding")
{
	UseExitOnly();
	MakeRailNormal(TileXY(8,12), _current_company, TRACK_BIT_LEFT | TRACK_BIT_RIGHT, RAILTYPE_RAIL);
	Register(slow); Register(medium);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	CHECK(random_checker.Check());
	CHECK(GetRailReservationTrackBits(TileXY(18,12)) == TRACK_BIT_NONE);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - a free siding is not withheld for a slower train behind")
{
	Place(medium,7,10); Place(slow,5,10); Place(fast,4,10);
	Register(slow); Register(medium); Register(fast);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::NeedsSiding));
	CHECK((Execute(medium,10,10).flags & TRPRF_DENY) != 0);
	Place(medium,12,10);
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::NeedsSiding));
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - only the approach marker supplies the speed and distance parameters")
{
	Marker(10,10,AutoYieldRole::Main, membership->index.base(), (1000u << 16) | 1u);
	Marker(18,12,AutoYieldRole::Exit, membership->index.base(), (1000u << 16) | 1u);
	Register(slow); Register(medium);
	CHECK(membership->IsOccupant(slow->index));
	CHECK(membership->IsOccupant(medium->index));
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	Marker(3,10,AutoYieldRole::Approach, membership->index.base(), (1000u << 16) | 100u);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - reservation registers before crossing and crossing confirms without reclaiming")
{
	const auto *approach = GetExistingTraceRestrictProgram(TileXY(3,10),TRACK_X);
	REQUIRE(approach != nullptr);
	CHECK((approach->actions_used_flags & TRPAUF_SLOT_RELEASE_FRONT) != 0);
	CHECK((GetExistingTraceRestrictProgram(TileXY(10,10),TRACK_X)->actions_used_flags & TRPAUF_SLOT_RELEASE_FRONT) == 0);
	GameRandomSeedChecker random_checker;
	Execute(slow,3,10); // A route query must never register a train.
	CHECK_FALSE(membership->IsOccupant(slow->index));
	Execute(slow,3,10,TRPISP_RELEASE_FRONT);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	Execute(slow,3,10,TRPISP_PBS_RES_END_ACQUIRE);
	CHECK(membership->IsOccupant(slow->index));
	Execute(slow,3,10,TRPISP_RELEASE_FRONT);
	CHECK(membership->occupants.size() == 1);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->occupants.empty());
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - physical front crossing respects capacity direction and the switch")
{
	SECTION("ordinary capacity") {
		membership->max_occupancy = 1;
		Register(medium);
		Execute(slow,3,10,TRPISP_RELEASE_FRONT);
		CHECK_FALSE(membership->IsOccupant(slow->index));
		CHECK(membership->occupants.size() == 1);
	}
	SECTION("back of signal") {
		TraceRestrictProgramInput input(TileXY(3,10),TRACKDIR_X_NE,nullptr,nullptr);
		input.permitted_slot_operations = TRPISP_RELEASE_FRONT;
		TraceRestrictProgramResult out;
		GetExistingTraceRestrictProgram(input.tile,TRACK_X)->Execute(slow,input,out);
		CHECK_FALSE(membership->IsOccupant(slow->index));
	}
	SECTION("disabled") {
		_settings_game.vehicle.automatic_yield = false;
		Execute(slow,3,10,TRPISP_RELEASE_FRONT);
		CHECK_FALSE(membership->IsOccupant(slow->index));
	}
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield registration - physical front and tail modes do not acquire during reservation")
{
	const auto mode=GENERATE(AutoYieldRegistration::Front,AutoYieldRegistration::Tail);
	auto *program=Marker(3,10,AutoYieldRole::Approach,membership->index.base(),AUTO_YIELD_DEFAULT_CONFIG | (uint32_t(to_underlying(mode))<<26) | 120);
	CHECK(GetAutoYieldPercent(program->items[1].base())==5);
	CHECK((program->actions_used_flags & TRPAUF_SLOT_RELEASE_FRONT)!=0);
	CHECK(((program->actions_used_flags & TRPAUF_SLOT_RELEASE_BACK)!=0)==(mode==AutoYieldRegistration::Tail));
	Execute(slow,3,10,TRPISP_PBS_RES_END_ACQUIRE);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	{
		TraceRestrictSlotTemporaryState temporary;temporary.PushToChangeStack();
		Execute(slow,3,10,TRPISP_ACQUIRE_TEMP_STATE);
		CHECK_FALSE(membership->IsOccupant(slow->index));
		temporary.PopFromChangeStackRevertTemporaryChanges(slow->index);
	}
	Execute(slow,3,10,TRPISP_RELEASE_FRONT);
	CHECK(membership->IsOccupant(slow->index)==(mode==AutoYieldRegistration::Front));
	Execute(slow,3,10,TRPISP_RELEASE_BACK);
	CHECK(membership->IsOccupant(slow->index));
	CHECK_FALSE(Resource(AUTO_YIELD_PENDING_REF)->IsOccupant(slow->index));
	Execute(slow,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield registration - copied timing survives rebuild and role changes clear it")
{
	const uint32_t config=AUTO_YIELD_DEFAULT_CONFIG | (2u<<26) | 120;
	auto *program=Marker(3,10,AutoYieldRole::Approach,membership->index.base(),config);
	Marker(3,20,AutoYieldRole::Approach);
	REQUIRE(CmdProgramSignalTraceRestrictMgmt(DoCommandFlag::Execute,TileXY(3,20),TRACK_X,TRMDCT_PROG_COPY,TileXY(3,10),TRACK_X).Succeeded());
	const auto *copy=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(3,20),TRACK_X),false);
	REQUIRE(copy!=nullptr);CHECK(copy->items[1].base()==config);
	ResetAutoYieldAreas();UpdateAutoYieldAreas();
	Execute(slow,3,20,TRPISP_RELEASE_FRONT);CHECK_FALSE(membership->IsOccupant(slow->index));
	Execute(slow,3,20,TRPISP_RELEASE_BACK);CHECK(membership->IsOccupant(slow->index));
	TraceRestrictInstructionItem main{};main.SetType(TRIT_AUTO_YIELD_AREA);main.SetAuxField(to_underlying(AutoYieldRole::Main));main.SetValue(membership->index);
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,TileXY(3,10),TRACK_X,TRDCT_MODIFY_ITEM,0,main.base(),"").Succeeded());
	CHECK((program->items[1].base()&AUTO_YIELD_REGISTRATION_MASK)==0);
	CHECK(GetAutoYieldPercent(program->items[1].base())==5);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield registration - reserved bits and registration roles are validated")
{
	auto *program=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(3,10),TRACK_X),false);
	const uint32_t invalid=GENERATE(3u<<26,1u<<28,1001u<<16);
	program->items[1]=TraceRestrictProgramItem{AUTO_YIELD_DEFAULT_CONFIG|invalid};
	CHECK(program->Validate().Failed());
	program->items[1]=TraceRestrictProgramItem{AUTO_YIELD_DEFAULT_CONFIG|(1u<<26)};
	CHECK(program->Validate().Succeeded());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - detection registers a committed path without waiting for passage")
{
	Register(slow);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	CHECK((Execute(fast,3,10,TRPISP_ACQUIRE).flags & TRPRF_WAIT_AT_PBS) == 0);
	CHECK_FALSE(membership->IsOccupant(fast->index));
	Execute(fast,3,10,TRPISP_PBS_RES_END_ACQUIRE);
	CHECK(membership->IsOccupant(fast->index));
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - legacy distance bits never limit speed detection")
{
	const uint32_t old_distance = GENERATE(0u,1u,100u,4096u,65535u);
	auto *legacy=Marker(3,10,AutoYieldRole::Approach,membership->index.base(),(25u << 16) | old_distance);
	MigrateLegacyAutoYieldSpeedCaps(*legacy);
	InvalidateAutoYieldAreas();UpdateAutoYieldAreas();
	CHECK(legacy->items[1].base()==TEST_YIELD_CONFIG);
	Register(slow);Register(medium);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
}

class AutomaticYieldWideFixture : public AutomaticYieldFixture {
protected:
	AutomaticYieldWideFixture() : AutomaticYieldFixture(2048) {}
};

TEST_CASE_METHOD(AutomaticYieldWideFixture, "Automatic yield free lanes - an unreachable first siding cannot spend admission on downstream track")
{
	const bool exit_only=GENERATE(false,true);
	if(exit_only)UseExitOnly();
	UpperSiding(exit_only);
	MakeRailNormal(TileXY(8,9),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	for(uint x=41;x<1600;x++)MakeRailNormal(TileXY(x,10),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Register(slow);Register(fast);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield free lanes - full-lane incoming ranking cannot veto a free normal siding")
{
	medium->vcache.cached_max_speed=140;
	Register(slow);Register(medium);Register(fast);
	GameRandomSeedChecker random_checker;
	REQUIRE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldWideFixture, "Automatic yield free lanes - destination admission does not require exhausting unrelated downstream branches")
{
	const bool exit_only=GENERATE(false,true);
	if(exit_only)UseExitOnly();
	/* The siding and main have different first clearances but both serve the
	 * explicit depot. A long, irrelevant branch beyond the merge must not
	 * invalidate a positively reachable destination from the empty siding. */
	Marker(19,12,AutoYieldRole::Clear);
	MakeRailDepot(TileXY(40,10),_current_company,DepotID{0},DiagDirection::NE,RAILTYPE_RAIL);
	slow->current_order.MakeGoToDepot(DepotID{0},{});slow->dest_tile=TileXY(40,10);
	MakeRailNormal(TileXY(30,10),_current_company,TRACK_BIT_X|TRACK_BIT_RIGHT,RAILTYPE_RAIL);
	for(uint y=11;y<20;y++)MakeRailNormal(TileXY(30,y),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(30,20),_current_company,TRACK_BIT_LEFT,RAILTYPE_RAIL);
	for(uint x=31;x<1600;x++)MakeRailNormal(TileXY(x,20),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Register(slow);
	bool reachable=true;
	SECTION("a legal destination continuation admits the empty siding"){}
	SECTION("a different mandatory depot cannot be bypassed"){
		slow->current_order.MakeGoToDepot(DepotID{1},{});reachable=false;
	}
	SECTION("an ordinary restriction still rejects the only destination continuation"){
		auto*p=Marker(31,10,AutoYieldRole::Approach,INVALID_TRACE_RESTRICT_SLOT_ID.base());
		TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);p->items={deny.AsProgramItem()};
		REQUIRE(p->Validate().Succeeded());InvalidateAutoYieldAreas();UpdateAutoYieldAreas();reachable=false;
	}
	GameRandomSeedChecker random;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding)==reachable);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random.Check());
}

TEST_CASE_METHOD(AutomaticYieldWideFixture, "Automatic yield free lanes - a distant station remains reachable beyond the local forecast horizon")
{
	/* Unit settings are otherwise zero-initialised; use the game's normal
	 * restriction lookahead when exercising the native pathfinder itself. */
	AutoRestoreBackup<uint32_t> lookahead(_settings_game.pf.yapf.rail_look_ahead_max_signals,10);
	const bool exit_only=GENERATE(false,true);
	if(exit_only)UseExitOnly();
	Marker(19,12,AutoYieldRole::Clear);
	for(uint x=41;x<=400;x++)MakeRailNormal(TileXY(x,10),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Station*target=Station::Create(TileXY(380,10));target->owner=_current_company;target->facilities={StationFacility::Train};
	target->train_station=TileArea(target->xy,1,1);target->rect.BeforeAddTile(target->xy,StationRect::ADD_FORCE);
	MakeRailStation(target->xy,_current_company,target->index,Axis::X,0,RAILTYPE_RAIL);
	slow->current_order.MakeGoToStation(target->index);slow->dest_tile=target->xy;
	Register(slow);
	bool reachable=true;
	SECTION("the native non-reserving probe confirms the far destination"){}
	SECTION("local path cache history cannot change a bounded proof"){
		uint cold=0,warm=0;
		const bool before=YapfTrainCanReachStationBounded(slow,TileXY(27,10),TRACKDIR_X_SW,target->index,32,cold);
		REQUIRE(YapfTrainCanReachStation(slow,TileXY(27,10),TRACKDIR_X_SW,target->index));
		CHECK(YapfTrainCanReachStationBounded(slow,TileXY(27,10),TRACKDIR_X_SW,target->index,32,warm)==before);
		CHECK(before);CHECK(cold==warm);CHECK(cold>0);CHECK(cold<=32);
	}
	SECTION("a zero native budget cannot become an unlimited search"){
		uint used=123;
		CHECK_FALSE(YapfTrainCanReachStationBounded(slow,TileXY(27,10),TRACKDIR_X_SW,target->index,0,used));
		CHECK(used==0);
	}
	SECTION("a disconnected destination is not permission to divert"){
		MakeClear(TileXY(200,10),ClearGround::Grass,0);reachable=false;
	}
	SECTION("ordinary restrictions beyond the tile horizon still forbid admission"){
		auto*p=Marker(300,10,AutoYieldRole::Approach,INVALID_TRACE_RESTRICT_SLOT_ID.base());
		TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);p->items={deny.AsProgramItem()};
		REQUIRE(p->Validate().Succeeded());SetRestrictedSignal(TileXY(300,10),true);
		InvalidateAutoYieldAreas();UpdateAutoYieldAreas();reachable=false;
	}
	GameRandomSeedChecker random;
	const Order order=slow->current_order;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding)==reachable);
	CHECK(slow->current_order.GetDestination()==order.GetDestination());
	CHECK(slow->dest_tile==target->xy);
	for(uint x:{7u,10u,18u,24u,27u,200u,380u})CHECK(GetReservedTrackbits(TileXY(x,10))==TRACK_BIT_NONE);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random.Check());
}

class AutomaticYieldNativeLayoutFixture : public AutomaticYieldFixture {
protected:
	~AutomaticYieldNativeLayoutFixture()
	{
		/* Native commands also schedule updates while removing restriction
		 * mappings. Drain them before the fixture's map changes size. */
		ClearTraceRestrictMapping();
		UpdateSignalsInBuffer();
	}
};

TEST_CASE_METHOD(AutomaticYieldNativeLayoutFixture, "Automatic yield layout edits - native signal changes refresh marker direction without editing its program")
{
	AutoRestoreBackup braking(_settings_game.vehicle.train_braking_model, uint8_t{0});
	AutoRestoreBackup styles(_enabled_new_signal_styles_mask, uint16_t{1});
	Register(slow); Register(fast);
	REQUIRE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::NeedsSiding));
	const auto *program = GetExistingTraceRestrictProgram(TileXY(3,10), TRACK_X);
	const auto items = program->items;
	REQUIRE(CmdBuildSingleSignal(DoCommandFlag::Execute, TileXY(3,10), TRACK_X, SignalType::Block, SignalVariant::Electric,
			0, 1, BuildSignalFlags::None, {}, 0, SignalOnTrack(TRACK_X)).Succeeded());
	AutoYieldGameTick();
	CHECK(GetExistingTraceRestrictProgram(TileXY(3,10), TRACK_X)->items == items);
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::NeedsSiding));
	REQUIRE(CmdBuildSingleSignal(DoCommandFlag::Execute, TileXY(3,10), TRACK_X, SignalType::Path, SignalVariant::Electric,
			0, 1, BuildSignalFlags::None, {}, 0, SignalAlongTrackdir(TRACKDIR_X_SW)).Succeeded());
	AutoYieldGameTick();
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::NeedsSiding));
}

TEST_CASE_METHOD(AutomaticYieldNativeLayoutFixture, "Automatic yield layout edits - disconnecting a chosen inlet releases only a proven orphan claim")
{
	const bool exit_only = GENERATE(false, true);
	if (exit_only) UseExitOnly();
	Register(slow); Register(fast);
	const uint x = exit_only ? 18 : 10;
	Execute(slow, x, 12, TRPISP_PBS_RES_END_ACQUIRE);
	auto *old_lane = Resource(MakeTraceRestrictRefId(TileXY(x,12),TRACK_X));
	REQUIRE(old_lane->IsOccupant(slow->index));
	UpperSiding(exit_only);
	bool retain = false;
	SECTION("empty disconnected lane") {}
	SECTION("tail still occupies the disconnected lane") {
		Place(slow, 25,10);
		const uint tail_x = GENERATE(9, 14); // Include parking behind a legacy inlet marker.
		Train *tail = MakeActor(tail_x,12,100); tail->ClearFrontEngine(); slow->SetNext(tail);
		retain = true;
	}
	SECTION("an unresolved exit reservation remains protected") {
		SetTrackReservation(TileXY(18,12), TRACK_BIT_X); retain = true;
	}
	REQUIRE(CmdRemoveSingleRail(DoCommandFlag::Execute, TileXY(8,11), TRACK_Y).Succeeded());
	AutoRestoreBackup tick(_state_ticks, StateTicks{64});
	AutoYieldGameTick();
	CHECK(old_lane->IsOccupant(slow->index) == retain);
	CHECK(membership->IsOccupant(slow->index));
	CHECK(membership->IsOccupant(fast->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldWideFixture, "Automatic yield - unknown distant routes retain membership without inventing a conflict")
{
	GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(10,10),TRACK_X),false)->items.clear();
	for(uint x=41;x<=1801;x++) MakeRailNormal(TileXY(x,10),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Marker(1800,10,AutoYieldRole::Main);
	Place(slow,1790,10);Place(fast,2,10);
	MakeRailNormal(TileXY(900,50),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Place(medium,900,50);
	Register(slow);Register(fast);Register(medium);
	GameRandomSeedChecker random_checker;
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	AutoRestoreBackup tick_backup(_state_ticks, StateTicks{64});
	AutoYieldGameTick();
	CHECK(membership->IsOccupant(slow->index));
	CHECK(membership->IsOccupant(fast->index));
	CHECK(membership->IsOccupant(medium->index));
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldWideFixture, "Automatic yield - distant members cannot spend a nearby train's siding admission budget")
{
	const bool focus_first = GENERATE(false, true);
	for (uint x = 41; x <= 1800; x++) MakeRailNormal(TileXY(x,10),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	if (focus_first) Register(slow);
	for (uint x = 100; x <= 600; x += 100) Register(MakeActor(x,10,80));
	if (!focus_first) Register(slow);
	Register(fast);
	bool can_divert = true;
	SECTION("free siding") { }
	SECTION("reserved siding remains protected") {
		REQUIRE(TryReserveRailTrack(TileXY(14,12),TRACK_X));
		can_divert = false;
	}
	SECTION("train too long") { slow->gcache.cached_total_length = 1024; can_divert = false; }
	SECTION("manually stopped follower has no priority") { fast->vehstatus.Set(VehState::Stopped); can_divert = false; }
	GameRandomSeedChecker random_checker;
	CHECK(((Execute(slow,10,10).flags & TRPRF_DENY) != 0) == can_divert);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - two approaches merging before a common main share following priority")
{
	for (uint x = 1; x < 6; x++) MakeRailNormal(TileXY(x,8),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(6,8),_current_company,TRACK_BIT_RIGHT,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(6,9),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(6,10),_current_company,TRACK_BIT_X | TRACK_BIT_LEFT,RAILTYPE_RAIL);
	Marker(3,8,AutoYieldRole::Approach);
	Place(medium,4,8);
	Register(slow);
	Execute(medium,3,8,TRPISP_PBS_RES_END_ACQUIRE);
	Execute(medium,3,8,TRPISP_RELEASE_FRONT);
	bool yield = true;
	SECTION("faster train approaches through the other branch") { }
	SECTION("equal speed empty trains need no diversion") { medium->vcache.cached_max_speed = 100; yield = false; }
	SECTION("stopped train on the other branch has no priority") { medium->vehstatus.Set(VehState::Stopped); yield = false; }
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding) == yield);
	CHECK(((Execute(slow,10,10).flags & TRPRF_DENY) != 0) == yield);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - clearing all source members releases hidden lane claims")
{
	Register(slow);Register(medium);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	auto *lane=Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X));
	REQUIRE(lane->IsOccupant(slow->index));
	GameRandomSeedChecker random_checker;
	membership->Clear();
	CHECK(membership->occupants.empty());
	CHECK(lane->occupants.empty());
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - crossing occupancy never proves an empty siding")
{
	Register(slow);Register(medium);
	MakeRailNormal(TileXY(14,12),_current_company,TRACK_BIT_X | TRACK_BIT_Y,RAILTYPE_RAIL);
	SECTION("physical crossing occupant") {
		fast->track=TRACK_BIT_Y;Place(fast,14,12);
		fast->vehstatus.Set(VehState::Stopped);
	}
	SECTION("crossing reservation") {
		REQUIRE(TryReserveRailTrack(TileXY(14,12),TRACK_Y));
	}
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - station platforms are usable but occupied platform tiles remain protected")
{
	UseExitOnly();
	const uint platform_length = GENERATE(1u, 5u);
	Station *station = Station::Create(TileXY(11,12));
	station->owner = _current_company;
	station->facilities = {StationFacility::Train};
	station->train_station = TileArea(station->xy, platform_length, 1);
	station->rect.BeforeAddTile(station->xy, StationRect::ADD_FORCE);
	station->rect.BeforeAddTile(TileXY(10 + platform_length,12), StationRect::ADD_FORCE);
	for (uint x = 11; x < 11 + platform_length; x++) MakeRailStation(TileXY(x,12), _current_company, station->index, Axis::X, 0, RAILTYPE_RAIL);
	Register(slow); Register(medium);
	GameRandomSeedChecker random_checker;
	SECTION("empty platform") {
		CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
		Execute(slow,18,12,TRPISP_PBS_RES_END_ACQUIRE);
		CHECK(Resource(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X))->IsOccupant(slow->index));
		CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::MustWait));
	}
	SECTION("another train on a skipped middle platform tile") {
		Place(fast,10 + (platform_length + 1) / 2,12);
		CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
	}
	SECTION("unattributed reservation on a skipped middle platform tile") {
		SetRailStationReservation(TileXY(10 + (platform_length + 1) / 2,12),true);
		CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
	}
	CHECK(random_checker.Check());
	_station_pool.CleanPool();
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - ordinary bridges do not read simulated signal fields")
{
	if (GENERATE(false, true)) UseExitOnly();
	Register(slow); Register(medium);
	MakeRailBridgeRamp(TileXY(12,12), _current_company, 0, DiagDirection::SW, RAILTYPE_RAIL, false);
	MakeRailBridgeRamp(TileXY(16,12), _current_company, 0, DiagDirection::NE, RAILTYPE_RAIL, false);
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	SECTION("a foreign train in the bridge prevents proof") {
		Train *occupant=MakeActor(12,12,100);occupant->track=TRACK_BIT_WORMHOLE;
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	}
	SECTION("an unattributed bridge reservation prevents proof") {
		REQUIRE(TryReserveRailTrack(TileXY(12,12),TRACK_X));
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	}
	SECTION("signal simulation requires an empty unreserved span") {
		SetTunnelBridgeSignalSimulationEntrance(TileXY(12,12));
		SetTunnelBridgeSignalSimulationExit(TileXY(16,12));
		CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
		REQUIRE(TryReserveRailTrack(TileXY(12,12),TRACK_X));
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	}
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - reverse one way signals cannot prove an available siding")
{
	UseExitOnly();
	Register(slow); Register(medium);
	const uint x = GENERATE(17u, 19u);
	const SignalType type = GENERATE(SignalType::Block, SignalType::PathOneWay);
	BackSignal(x, type);
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - reverse passable PBS preserves existing back face restrictions")
{
	UseExitOnly();
	Register(slow); Register(medium);
	const uint x = GENERATE(17u, 19u);
	BackSignal(x, SignalType::Path);
	bool denied = false;
	SECTION("plain reverse PBS") {}
	SECTION("reverse PBS denies its facing direction") {
		denied = true;
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(x,12), TRACK_X), true);
		TraceRestrictInstructionItem deny{}; deny.SetType(TRIT_PF_DENY);
		program->items = {deny.AsProgramItem()};
		REQUIRE(program->Validate().Succeeded());
		InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	}
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding) == !denied);
	CHECK(((Execute(slow,10,10).flags & TRPRF_DENY) != 0) == !denied);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - exit markers require one signal face in the departure direction")
{
	UseExitOnly();
	Register(slow); Register(medium);
	SECTION("exit faces the wrong way") { SetPresentSignals(TileXY(18,12), SignalAlongTrackdir(TRACKDIR_X_NE)); }
	SECTION("exit has both faces") { SetPresentSignals(TileXY(18,12), SignalAlongTrackdir(TRACKDIR_X_NE) | SignalAlongTrackdir(TRACKDIR_X_SW)); }
	InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
	CHECK((Execute(slow,18,12).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - an empty alternative siding avoids unnecessary evacuation")
{
	const bool exit_only = GENERATE(false, true);
	if (exit_only) UseExitOnly();
	UpperSiding(exit_only);
	Register(slow); Register(medium); Register(fast);
	Place(medium,14,12);
	Execute(medium,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	REQUIRE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(medium,18,12).flags & TRPRF_WAIT_AT_PBS) != 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - a train past the fork must not evict a siding occupant")
{
	const bool exit_only = GENERATE(false, true);
	if (exit_only) UseExitOnly();
	Register(slow); Register(medium); Register(fast);
	Place(medium,14,12); Execute(medium,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	Place(slow,9,10);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(medium,18,12).flags & TRPRF_WAIT_AT_PBS) != 0);
	CHECK((Execute(slow,10,10).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - evacuation cannot cut ahead of a faster train already passing")
{
	const uint fast_x = GENERATE(9u, 12u);
	Register(slow); Register(medium); Register(fast);
	Train *rear = MakeActor(4,10,220); Register(rear);
	Place(fast,fast_x,10);
	Place(medium,14,12); Execute(medium,10,12,TRPISP_ACQUIRE);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(medium,18,12).flags & TRPRF_WAIT_AT_PBS) != 0);
	Execute(fast,27,10,TRPISP_RELEASE_BACK);
	Place(fast,30,10);
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - eviction requires an eligible unblocked incoming route")
{
	const bool exit_only = GENERATE(false, true);
	if (exit_only) UseExitOnly();
	Register(slow); Register(medium); Register(fast);
	bool common_route = true;
	Place(medium,14,12); Execute(medium,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	SECTION("another train blocks the incoming branch") {
		Train *blocker = MakeActor(12,12,30); blocker->vehstatus.Set(VehState::Stopped);
	}
	SECTION("ordinary restriction denies the incoming branch") {
		BackSignal(11,SignalType::Path);
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(11,12),TRACK_X),true);
		TraceRestrictInstructionItem deny{}; deny.SetType(TRIT_PF_DENY);
		program->items = {deny.AsProgramItem()};
		REQUIRE(program->Validate().Succeeded());
		InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	}
	SECTION("incoming train cannot fit") { slow->gcache.cached_total_length = 20 * TILE_SIZE; }
	SECTION("only the departing train can use the rail type after the merge") {
		common_route = false;
		medium->railtypes.Set(RAILTYPE_ELECTRIC);
		medium->compatible_railtypes.Set(RAILTYPE_ELECTRIC);
		MakeRailNormal(TileXY(23,10),_current_company,TRACK_BIT_X,RAILTYPE_ELECTRIC);
	}
	SECTION("outgoing route is occupied") {
		Train *blocker = MakeActor(22,10,30); blocker->vehstatus.Set(VehState::Stopped);
	}
	GameRandomSeedChecker random_checker;
	const auto members = membership->occupants;
	auto *lane = Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,12),TRACK_X));
	REQUIRE(lane != nullptr);
	const auto claims = lane->occupants;
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK(((Execute(medium,18,12).flags & TRPRF_WAIT_AT_PBS) != 0) == common_route);
	CHECK(membership->occupants == members);
	CHECK(lane->occupants == claims);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - a departure proposal never releases live reservations or lane claims")
{
	const bool exit_only = GENERATE(false, true);
	if (exit_only) UseExitOnly();
	Register(slow); Register(medium); Register(fast);
	Place(medium,14,12); Execute(medium,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	REQUIRE(TryReserveRailTrack(TileXY(14,12),TRACK_X));
	REQUIRE(GetTrainForReservation(TileXY(14,12),TRACK_X) == medium);
	const auto members = membership->occupants;
	auto *lane = Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,12),TRACK_X));
	REQUIRE(lane != nullptr);
	const auto claims = lane->occupants;
	GameRandomSeedChecker random_checker;
	for (uint i = 0; i < 8; i++) {
		CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
		CHECK((Execute(slow,10,10).flags & TRPRF_WAIT_AT_PBS) != 0);
		CHECK(GetReservedTrackbits(TileXY(14,12)) == TRACK_BIT_X);
		CHECK(membership->occupants == members);
		CHECK(lane->occupants == claims);
	}
	CHECK(random_checker.Check());
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - only the selected incoming train waits for the departure proposal")
{
	Register(slow); Register(medium); Register(fast);
	Place(slow,9,10);
	Train *incoming = MakeActor(7,10,80); Register(incoming);
	Place(medium,14,12); Execute(medium,10,12,TRPISP_ACQUIRE);
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(incoming,10,10).flags & TRPRF_WAIT_AT_PBS) != 0);
	CHECK((Execute(slow,10,10).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - existing membership survives incomplete downstream roles")
{
	UseExitOnly(); Register(slow); Register(medium);
	SECTION("only exit becomes double faced") {
		SetPresentSignals(TileXY(18,12),SignalAlongTrackdir(TRACKDIR_X_NE) | SignalAlongTrackdir(TRACKDIR_X_SW));
	}
	SECTION("main marker is removed") {
		GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(10,10),TRACK_X),false)->items.clear();
	}
	SECTION("exit marker is removed") {
		GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X),false)->items.clear();
	}
	InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	GameRandomSeedChecker random_checker;
	Register(slow); Register(medium);
	CHECK(membership->IsOccupant(slow->index));
	CHECK(membership->IsOccupant(medium->index));
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(slow,18,12).flags & TRPRF_WAIT_AT_PBS) == 0);
	Execute(slow,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	CHECK(membership->IsOccupant(medium->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - a dirty registry defers new reservation registration until rebuilt")
{
	InvalidateAutoYieldAreas();
	Execute(slow,3,10,TRPISP_ACQUIRE_TEMP_STATE);
	CHECK(membership->occupants.empty());
	Register(slow);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	UpdateAutoYieldAreas();
	Register(slow);
	CHECK(membership->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - multiple approach markers keep their shared area active")
{
	UseExitOnly();
	Marker(5,10,AutoYieldRole::Approach);
	Register(slow); Register(medium);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	const auto members = membership->occupants;
	Execute(slow,5,10,TRPISP_RELEASE_FRONT);
	Execute(medium,5,10,TRPISP_RELEASE_FRONT);
	CHECK(membership->occupants == members);
	Execute(slow,18,12,TRPISP_PBS_RES_END_ACQUIRE);
	CHECK((Execute(medium,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
	CHECK((Execute(medium,5,10).flags & TRPRF_WAIT_AT_PBS) == 0);
	CHECK((Execute(fast,5,10).flags & TRPRF_WAIT_AT_PBS) == 0);
	Execute(slow,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - multiple approach thresholds use the strictest value independent of tile order")
{
	const bool strict_first = GENERATE(false, true);
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),(strict_first ? 50u : 5u) << 16);
	Marker(5,10,AutoYieldRole::Approach,membership->index.base(),(strict_first ? 5u : 50u) << 16);
	medium->vcache.cached_max_speed = 140;
	Register(slow); Register(medium);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
	Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - repeated approach crossings never consume another capacity slot")
{
	Marker(5,10,AutoYieldRole::Approach);
	membership->max_occupancy = 1;
	Register(slow);
	Execute(slow,5,10,TRPISP_RELEASE_FRONT);
	CHECK(membership->occupants.size() == 1);
	CHECK((Execute(medium,5,10).flags & TRPRF_WAIT_AT_PBS) != 0);
	Execute(medium,5,10,TRPISP_RELEASE_FRONT);
	CHECK_FALSE(membership->IsOccupant(medium->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

class AutomaticYieldPlatformFixture : public AutomaticYieldFixture {
protected:
	Station *platform_station = nullptr;
	Station *passing_target = nullptr;
	~AutomaticYieldPlatformFixture() { _station_pool.CleanPool(); }
	void SetupPlatform(bool exit_only)
	{
		if (exit_only) UseExitOnly();
		UpperSiding(exit_only);
		platform_station = Station::Create(TileXY(11,12));
		platform_station->owner = _current_company;
		platform_station->facilities = {StationFacility::Train};
		platform_station->train_station = TileArea(platform_station->xy, 5, 1);
		platform_station->rect.BeforeAddTile(platform_station->xy, StationRect::ADD_FORCE);
		platform_station->rect.BeforeAddTile(TileXY(15,12), StationRect::ADD_FORCE);
		for (uint x = 11; x <= 15; x++) MakeRailStation(TileXY(x,12),_current_company,platform_station->index,Axis::X,0,RAILTYPE_RAIL);
		Marker(18,12,AutoYieldRole::Exit,membership->index.base(),TEST_YIELD_CONFIG | AUTO_YIELD_PLATFORM);
	}
	void PassStation(Train *v)
	{
		/* A train passing this platform heads to a real station beyond Clear;
		 * naming the siding's own station would require it to use that siding. */
		if (passing_target == nullptr) {
			passing_target = Station::Create(TileXY(35,10)); passing_target->owner=_current_company;
			passing_target->facilities={StationFacility::Train}; passing_target->train_station=TileArea(passing_target->xy,4,1);
			for(uint x=35;x<=38;x++)MakeRailStation(TileXY(x,10),_current_company,passing_target->index,Axis::X,0,RAILTYPE_RAIL);
		}
		v->current_order.MakeGoToStation(passing_target->index);
		v->current_order.SetNonStopType(ONSF_NO_STOP_AT_ANY_STATION);
	}
};

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - passing trains prefer a usable ordinary siding")
{
	const bool exit_only = GENERATE(false,true);
	SetupPlatform(exit_only);
	PassStation(slow); Register(slow); Register(medium);
	GameRandomSeedChecker random_checker;
	const auto platform = Execute(slow,exit_only ? 18 : 10,12);
	CHECK((platform.flags & TRPRF_DENY) != 0);
	CHECK((Execute(slow,exit_only ? 18 : 10,8).flags & TRPRF_DENY) == 0);
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - platform fallback does not wait for an unusable ordinary siding")
{
	const bool exit_only = GENERATE(false,true);
	SetupPlatform(exit_only);
	PassStation(slow); Register(slow); Register(medium);
	SECTION("ordinary siding occupied") { Place(fast,14,8); fast->vehstatus.Set(VehState::Stopped); }
	SECTION("ordinary siding claimed") {
		auto *lane = Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,8),TRACK_X));
		REQUIRE(lane != nullptr); REQUIRE(lane->Occupy(fast));
	}
	SECTION("ordinary siding blocked by reverse one way signal") {
		Marker(14,8,AutoYieldRole::Approach,INVALID_TRACE_RESTRICT_SLOT_ID.base());
		GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(14,8),TRACK_X),false)->items.clear();
		SetPresentSignals(TileXY(14,8),SignalAlongTrackdir(TRACKDIR_X_NE));
		SetSignalType(TileXY(14,8),TRACK_X,SignalType::PathOneWay);
		InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	}
	CHECK((Execute(slow,exit_only ? 18 : 10,12).flags & TRPRF_DENY) == 0);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - scheduled stops and coupling destinations remain eligible")
{
	const bool exit_only = GENERATE(false,true);
	SetupPlatform(exit_only);
	Register(slow); Register(medium);
	SECTION("scheduled station stop") { slow->current_order.MakeGoToStation(platform_station->index); }
	SECTION("intermediate stop is allowed") { slow->current_order.MakeGoToStation(StationID{123}); slow->current_order.SetNonStopType(ONSF_STOP_EVERYWHERE); }
	SECTION("coupling target") { slow->current_order.MakeGoToCouple(); }
	SECTION("waiting for coupling") { slow->current_order.MakeWaitCouple(); }
	CHECK((Execute(slow,exit_only ? 18 : 10,12).flags & TRPRF_DENY) == 0);
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - a committed platform route is not rejected after ordinary capacity frees")
{
	const bool exit_only = GENERATE(false,true);
	SetupPlatform(exit_only);
	PassStation(slow); Register(slow); Register(medium);
	Place(fast,14,8); fast->vehstatus.Set(VehState::Stopped);
	Execute(slow,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	REQUIRE(Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,12),TRACK_X))->IsOccupant(slow->index));
	Place(fast,30,10);
	CHECK((Execute(slow,18,12).flags & TRPRF_DENY) == 0);
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - station work cannot freeze ordinary traffic or incoming coupling")
{
	const bool exit_only = GENERATE(false,true);
	SetupPlatform(exit_only);
	Register(slow); Register(medium);
	Place(medium,14,12);
	Execute(medium,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	medium->last_station_visited = platform_station->index;
	SECTION("full load") { medium->current_order.MakeLoading(true); medium->current_order.SetLoadType(OrderLoadType::FullLoad); }
	SECTION("waiting coupling") { medium->current_order.MakeWaitCouple(); }
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(fast,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
	Register(fast);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK(membership->IsOccupant(medium->index));
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,12),TRACK_X))->IsOccupant(medium->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - an unclaimed loading train is identified on its actual platform")
{
	SetupPlatform(true);
	Register(slow); Register(medium);
	Place(medium,14,12);
	medium->last_station_visited = platform_station->index;
	medium->current_order.MakeLoading(true);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(fast,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - marking one platform does not change unmarked tracks at the same station")
{
	SetupPlatform(true);
	platform_station->train_station = TileArea(TileXY(11,8),5,5);
	platform_station->rect.BeforeAddTile(TileXY(14,8),StationRect::ADD_FORCE);
	MakeRailStation(TileXY(14,8),_current_company,platform_station->index,Axis::X,0,RAILTYPE_RAIL);
	Register(slow); Register(medium);
	Place(medium,14,8);
	Execute(medium,18,8,TRPISP_ACQUIRE);
	medium->last_station_visited = platform_station->index;
	medium->current_order.MakeLoading(true);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(fast,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - authoritative commands preserve slot IDs and normalise lane flags")
{
	SetupPlatform(true);
	const TileIndex tile = TileXY(18,12);
	const auto ref = MakeTraceRestrictRefId(tile,TRACK_X);
	const uint32_t config = TEST_YIELD_CONFIG | AUTO_YIELD_PLATFORM;
	REQUIRE(CmdProgramSignalTraceRestrict({},tile,TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,TEST_YIELD_CONFIG,"").Succeeded());
	CHECK(GetTraceRestrictProgram(ref,false)->items[1].base() == config);
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,tile,TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,TEST_YIELD_CONFIG,"").Succeeded());
	CHECK(GetTraceRestrictProgram(ref,false)->items[1].base() == TEST_YIELD_CONFIG);
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,tile,TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,config,"").Succeeded());
	CHECK(GetTraceRestrictProgram(ref,false)->items[1].base() == config);
	CHECK(TraceRestrictInstructionItem(GetTraceRestrictProgram(ref,false)->items[0].base()).GetValueAsSlot() == membership->index);
	TraceRestrictInstructionItem approach(GetTraceRestrictProgram(ref,false)->items[0].base());
	approach.SetAuxField(to_underlying(AutoYieldRole::Approach));
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,tile,TRACK_X,TRDCT_MODIFY_ITEM,0,approach.base(),"").Succeeded());
	CHECK((GetTraceRestrictProgram(ref,false)->items[1].base() & AUTO_YIELD_PLATFORM) == 0);
	CHECK(CmdProgramSignalTraceRestrict({},tile,TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,config,"").Failed());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - a long train with its front beyond the platform can finish station work")
{
	SetupPlatform(true);
	Register(slow); Register(medium);
	Place(medium,19,12);
	Train *rear = MakeTrain(); medium->SetNext(rear);
	rear->owner = _current_company; rear->track = TRACK_BIT_X; rear->direction = Direction::SW;
	Place(rear,14,12);
	medium->last_station_visited = platform_station->index;
	medium->current_order.MakeLoading(true);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(fast,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - program copying retains the platform option")
{
	SetupPlatform(true);
	REQUIRE(CmdProgramSignalTraceRestrictMgmt(DoCommandFlag::Execute,TileXY(18,8),TRACK_X,TRMDCT_PROG_COPY,TileXY(18,12),TRACK_X).Succeeded());
	auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,8),TRACK_X),false);
	REQUIRE(program != nullptr);
	CHECK((program->items[1].base() & AUTO_YIELD_PLATFORM) != 0);
	CHECK(program->Validate().Succeeded());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - complex alternatives cannot consume the budget needed for safe waiting")
{
	SetupPlatform(true);
	PassStation(slow); Register(slow); Register(medium);
	Place(fast,14,8); fast->vehstatus.Set(VehState::Stopped);
	for (uint y = 20; y <= 56; y += 4) {
		for (uint x = 1; x <= 60; x++) {
			MakeRailNormal(TileXY(x,y),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
			MakeRailNormal(TileXY(x,y-2),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		}
		MakeRailNormal(TileXY(1,y),_current_company,TRACK_BIT_LEFT,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(1,y-1),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(1,y-2),_current_company,TRACK_BIT_LOWER,RAILTYPE_RAIL);
		Marker(60,y,AutoYieldRole::Exit);
	}
	const auto result = Execute(slow,18,12,TRPISP_PBS_RES_END_ACQUIRE);
	CHECK((result.flags & TRPRF_WAIT_AT_PBS) != 0);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X))->IsOccupant(slow->index));
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - unknown legacy platform geometry cannot force station work to vacate")
{
	SetupPlatform(false);
	GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X),false)->items.clear();
	Marker(24,10,AutoYieldRole::Exit,membership->index.base(),TEST_YIELD_CONFIG | AUTO_YIELD_PLATFORM);
	Register(slow); Register(medium); Register(fast);
	Train *blocker = MakeActor(14,8,30); blocker->vehstatus.Set(VehState::Stopped);
	Place(medium,14,12); Execute(medium,10,12,TRPISP_ACQUIRE);
	medium->last_station_visited = platform_station->index;
	medium->current_order.MakeLoading(true);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(slow,10,10).flags & TRPRF_WAIT_AT_PBS) == 0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - Wangjia detector does not close behind an occupied siding")
{
	const bool exit_only = GENERATE(false, true);
	if (exit_only) UseExitOnly();
	Register(slow); Register(medium);
	Place(slow,14,12);
	Execute(slow,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	auto *lane = Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,12),TRACK_X));
	REQUIRE(lane->IsOccupant(slow->index));
	Place(fast,2,10);
	GameRandomSeedChecker random_checker;
	SECTION("capacity available: a new fast train may enter") {
		CHECK((Execute(fast,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
		TraceRestrictSlotTemporaryState temporary; temporary.PushToChangeStack();
		CHECK((Execute(fast,3,10,TRPISP_ACQUIRE_TEMP_STATE).flags & TRPRF_WAIT_AT_PBS) == 0);
		CHECK(membership->IsOccupant(fast->index));
		temporary.PopFromChangeStackApplyTemporaryChanges(fast);
		Execute(fast,3,10,TRPISP_RELEASE_FRONT);
		CHECK(membership->IsOccupant(fast->index));
	}
	SECTION("capacity available: admission does not depend on speed") {
		fast->vcache.cached_max_speed = 80;
		CHECK((Execute(fast,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
	}
	SECTION("full source still stops new trains") {
		membership->max_occupancy = 2;
		CHECK((Execute(fast,3,10).flags & TRPRF_WAIT_AT_PBS) != 0);
		Execute(fast,3,10,TRPISP_RELEASE_FRONT);
		CHECK_FALSE(membership->IsOccupant(fast->index));
		CHECK((Execute(medium,3,10).flags & TRPRF_WAIT_AT_PBS) == 0);
	}
	CHECK(lane->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - a needed fallback ignores numeric penalties without bypassing safety")
{
	const bool exit_only = GENERATE(false,true);
	SetupPlatform(exit_only);
	PassStation(slow); Register(slow); Register(medium);
	Place(fast,14,8); fast->vehstatus.Set(VehState::Stopped);
	const uint x = exit_only ? 18 : 10;
	auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(x,12),TRACK_X),false);
	TraceRestrictInstructionItem penalty{0}; penalty.SetType(TRIT_PF_PENALTY); penalty.SetValue(60000);
	program->items.insert(program->items.begin(),penalty.AsProgramItem());
	program->items.push_back(penalty.AsProgramItem());
	REQUIRE(program->Validate().Succeeded());
	GameRandomSeedChecker random_checker;
	SECTION("slow train must use the safe empty platform despite penalties on both sides of the marker") {
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
		const auto out = Execute(slow,x,12);
		CHECK(out.penalty == 0);
		CHECK((out.flags & TRPRF_DENY) == 0);
	}
	SECTION("unnecessary or unregistered diversions retain penalties") {
		medium->vcache.cached_max_speed = 100;
		CHECK(Execute(slow,x,12).penalty == 120000);
		membership->Vacate(slow);
		CHECK(Execute(slow,x,12).penalty == 120000);
	}
	SECTION("ordinary space retains ordinary preference") {
		Place(fast,30,10);
		const auto out = Execute(slow,x,12);
		CHECK((out.flags & TRPRF_DENY) != 0);
		CHECK(out.penalty == 120000);
	}
	SECTION("short platform cannot gain preference") {
		slow->gcache.cached_total_length = 40 * TILE_SIZE;
		CHECK(Execute(slow,x,12).penalty == 120000);
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
	}
	SECTION("occupied platform is protected") {
		Train *blocker = MakeActor(13,12,80); blocker->vehstatus.Set(VehState::Stopped);
		CHECK(Execute(slow,x,12).penalty == 120000);
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
	}
	SECTION("claimed platform is protected") {
		auto *lane = Resource(MakeTraceRestrictRefId(TileXY(x,12),TRACK_X));
		REQUIRE(lane->Occupy(fast));
		CHECK(Execute(slow,x,12).penalty == 120000);
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
	}
	SECTION("an occupied shared departure still allows safe parking") {
		Train *blocker = MakeActor(25,10,80); blocker->vehstatus.Set(VehState::Stopped);
		CHECK(Execute(slow,x,12).penalty == 120000);
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	}
	SECTION("physical parking occupancy cannot justify forcing the platform") {
		Train *blocker = MakeActor(14,12,80); blocker->vehstatus.Set(VehState::Stopped);
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) == 0);
	}
	SECTION("mandatory station and coupling tasks keep their own rules") {
		slow->current_order.MakeGoToStation(platform_station->index);
		CHECK(Execute(slow,x,12).penalty == 120000);
		slow->current_order.MakeGoToCouple();
		CHECK(Execute(slow,x,12).penalty == 120000);
	}
	SECTION("global switch disables the cost override") {
		_settings_game.vehicle.automatic_yield = false;
		CHECK(Execute(slow,x,12).penalty == 120000);
	}
	SECTION("explicit deny remains authoritative") {
		TraceRestrictInstructionItem deny{0}; deny.SetType(TRIT_PF_DENY);
		program->items.push_back(deny.AsProgramItem());
		REQUIRE(program->Validate().Succeeded());
		CHECK((Execute(slow,x,12).flags & TRPRF_DENY) != 0);
	}
	SECTION("explicit PBS waiting is not cancelled") {
		TraceRestrictInstructionItem wait{0}; wait.SetType(TRIT_WAIT_AT_PBS);
		program->items.push_back(wait.AsProgramItem());
		REQUIRE(program->Validate().Succeeded());
		CHECK((Execute(slow,x,12).flags & TRPRF_WAIT_AT_PBS) != 0);
	}
	CHECK(random_checker.Check());
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldWideFixture, "Automatic yield - Dezhou main admission and evacuation share their route proofs")
{
	const bool exit_only = GENERATE(false,true);
	for (const auto &[x,y] : {std::pair{10,10},{10,12},{18,12},{27,10}}) {
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(x,y),TRACK_X),false);
		REQUIRE(program != nullptr); program->items.clear(); REQUIRE(program->Validate().Succeeded());
	}
	for (uint x=1; x<=240; x++) MakeRailNormal(TileXY(x,10),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	for (uint x=208; x<=220; x++) MakeRailNormal(TileXY(x,12),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(208,10),_current_company,TRACK_BIT_X|TRACK_BIT_RIGHT,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(208,11),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(208,12),_current_company,TRACK_BIT_LEFT,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(220,12),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(220,11),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(220,10),_current_company,TRACK_BIT_X|TRACK_BIT_LOWER,RAILTYPE_RAIL);
	Marker(3,10,AutoYieldRole::Approach);
	Marker(210,10,AutoYieldRole::Main);
	if (!exit_only) Marker(210,12,AutoYieldRole::Siding);
	Marker(218,12,AutoYieldRole::Exit);
	Marker(227,10,AutoYieldRole::Clear);
	SB(_m[TileXY(233,10)].m5,6,2,to_underlying(RailTileType::Signals));
	SetPresentSignals(TileXY(233,10),SignalAlongTrackdir(TRACKDIR_X_SW));
	SetSignalType(TileXY(233,10),TRACK_X,SignalType::Path);
	Place(slow,30,10); Place(medium,214,12); Place(fast,1,10);
	Register(slow); Register(medium); Register(fast);
	Execute(medium,exit_only ? 218 : 210,12,TRPISP_ACQUIRE);
	REQUIRE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	GameRandomSeedChecker random_checker;
	const auto main=Execute(slow,210,10);
	CHECK((main.flags&TRPRF_WAIT_AT_PBS)!=0);
	CHECK((main.flags&TRPRF_PBS_RES_END_WAIT)!=0);
	CHECK_FALSE(IsWaitingPositionFreeTraceRestrictExecute(GetExistingTraceRestrictProgram(TileXY(210,10),TRACK_X),slow,TileXY(210,10),TRACKDIR_X_SW));
	CHECK((main.flags&TRPRF_DENY)==0); // Wait for the selected departure; do not force entry into an occupied siding.
	CHECK((Execute(medium,218,12).flags&TRPRF_WAIT_AT_PBS)==0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldWideFixture, "Automatic yield turnover - equal speed distance boundary is thirty-two route tiles")
{
	const bool exit_only = GENERATE(false,true);
	for (const auto &[x,y] : {std::pair{10,10},{10,12},{18,12},{27,10}}) {
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(x,y),TRACK_X),false);
		REQUIRE(program != nullptr); program->items.clear(); REQUIRE(program->Validate().Succeeded());
	}
	for (uint x=1; x<=240; x++) MakeRailNormal(TileXY(x,10),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	for (uint x=208; x<=220; x++) MakeRailNormal(TileXY(x,12),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(208,10),_current_company,TRACK_BIT_X|TRACK_BIT_RIGHT,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(208,11),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(208,12),_current_company,TRACK_BIT_LEFT,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(220,12),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(220,11),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(220,10),_current_company,TRACK_BIT_X|TRACK_BIT_LOWER,RAILTYPE_RAIL);
	Marker(3,10,AutoYieldRole::Approach);
	Marker(210,10,AutoYieldRole::Main);
	if (!exit_only) Marker(210,12,AutoYieldRole::Siding);
	Marker(218,12,AutoYieldRole::Exit);
	Marker(227,10,AutoYieldRole::Clear);
	const bool near=GENERATE(false,true);
	SB(_m[TileXY(233,10)].m5,6,2,to_underlying(RailTileType::Signals));
	SetPresentSignals(TileXY(233,10),SignalAlongTrackdir(TRACKDIR_X_SW));
	SetSignalType(TileXY(233,10),TRACK_X,SignalType::Path);
	slow->vcache.cached_max_speed=medium->vcache.cached_max_speed=100;
	Place(slow,near?180:179,10);Place(medium,214,12);Place(fast,80,10);fast->cur_speed=20;
	Register(slow);Register(medium);Register(fast);
	Execute(medium,exit_only?218:210,12,TRPISP_ACQUIRE);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate)==!near);
	/* Turnover has the 32-tile boundary, but this remote express cannot
	 * justify withholding an ordinary safe departure. */
	CHECK((Execute(medium,218,12).flags&TRPRF_WAIT_AT_PBS)==0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield turnover - equal nearby trains move only to break a confirmed stopped queue")
{
	slow->vcache.cached_max_speed=medium->vcache.cached_max_speed=100;
	Register(slow);Register(medium);Register(fast);
	Place(medium,14,12);Execute(medium,10,12,TRPISP_ACQUIRE);
	bool may_turn=true;
	SECTION("stopped follower physically blocked behind the stopped arrival") { }
	SECTION("priority follower is still moving") { fast->cur_speed=20;may_turn=false; }
	SECTION("incoming train is still moving") { slow->cur_speed=20;may_turn=false; }
	SECTION("siding train is slower than incoming") { medium->vcache.cached_max_speed=80;may_turn=false; }
	SECTION("siding train is still carrying out a coupling wait") { medium->current_order.MakeWaitCouple();may_turn=false; }
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate)==may_turn);
	CHECK(((Execute(medium,18,12).flags&TRPRF_WAIT_AT_PBS)==0)==may_turn);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield turnover - highest design speed precedes passenger preference")
{
	UpperSiding(false);
	Train *passenger=MakeActor(14,8,120);
	AutoRestoreBackup passenger_class(CargoSpec::Get(0)->classes,CargoClasses{CargoClass::Passengers});
	AutoRestoreBackup freight_class(CargoSpec::Get(1)->classes,CargoClasses{CargoClass::Bulk});
	passenger->cargo_type=CargoType{0};passenger->cargo_cap=10;
	medium->cargo_type=CargoType{1};medium->cargo_cap=10;
	Train *expected=medium;
	SECTION("a slower passenger cannot precede a faster safe freight departure") { }
	SECTION("fastest passenger among passenger candidates leaves") { medium->cargo_type=CargoType{0};expected=medium; }
	SECTION("passenger slower than incoming cannot be selected") { slow->vcache.cached_max_speed=140;expected=medium; }
	Register(slow);Register(medium);Register(fast);Register(passenger);
	Place(medium,14,12);Execute(medium,10,12,TRPISP_ACQUIRE);Execute(passenger,10,8,TRPISP_ACQUIRE);
	fast->cur_speed=20;
	DistantExpress(fast);

	CAPTURE(expected->index, slow->vcache.cached_max_speed, medium->vcache.cached_max_speed, passenger->vcache.cached_max_speed);
	CHECK(EvaluateAutoYield(expected,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK_FALSE(EvaluateAutoYield(expected==medium?passenger:medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield full lanes - the closest conflicting arrival precedes a slower train farther ahead")
{
	AutoRestoreBackup<int8_t> debug(_debug_levels[size_t(DebugLevelID::yapf)], std::getenv("PXP_YIELD_TEST_DEBUG") ? 7 : _debug_levels[size_t(DebugLevelID::yapf)]);
	UpperSiding(false);
	Train *occupant=MakeActor(14,8,180),*nearest=MakeActor(6,10,140);
	Register(slow);Register(medium);Register(fast);Register(occupant);Register(nearest);
	Place(medium,14,12);Execute(medium,10,12,TRPISP_ACQUIRE);Execute(occupant,10,8,TRPISP_ACQUIRE);
	DistantExpress(fast);fast->cur_speed=40;
	const auto farther=Execute(slow,10,10),closer=Execute(nearest,10,10);
	CHECK((farther.flags&TRPRF_WAIT_AT_PBS)==0);
	CHECK((closer.flags&TRPRF_WAIT_AT_PBS)==0); // Its actual ingress is still occupied by the front train.
	Place(slow,30,10);Execute(slow,27,10,TRPISP_RELEASE_BACK);
	CHECK((Execute(nearest,10,10).flags&TRPRF_WAIT_AT_PBS)!=0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield full lanes - an arrival as fast as the fastest occupant retains the main exception")
{
	UpperSiding(false);
	Train *other=MakeActor(14,8,100);
	slow->vcache.cached_max_speed=160;
	Register(slow);Register(medium);Register(fast);Register(other);
	Place(medium,14,12);Execute(medium,10,12,TRPISP_ACQUIRE);Execute(other,10,8,TRPISP_ACQUIRE);
	DistantExpress(fast);fast->cur_speed=40;
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK_FALSE(EvaluateAutoYield(other,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(slow,10,10).flags&TRPRF_WAIT_AT_PBS)==0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield full lanes - equal design speeds release the farthest safe occupant")
{
	UpperSiding(false);
	Train *ahead=MakeActor(15,8,100);
	medium->vcache.cached_max_speed=100;
	Register(slow);Register(medium);Register(fast);Register(ahead);
	Place(medium,14,12);Execute(medium,10,12,TRPISP_ACQUIRE);Execute(ahead,10,8,TRPISP_ACQUIRE);
	medium->cur_speed=80;DistantExpress(fast);fast->cur_speed=40;
	CHECK(EvaluateAutoYield(ahead,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield split exits - only proven independent corridors relax priority")
{
	for (uint x=1; x<=40; x++) MakeRailNormal(TileXY(x,20),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Marker(3,20,AutoYieldRole::Approach); Marker(10,20,AutoYieldRole::Main); Marker(27,20,AutoYieldRole::Clear);
	Place(slow,9,10); Place(fast,7,20);
	Register(slow); Register(fast);
	[[maybe_unused]] bool independent = true;
	bool confirmed_conflict = false;
	SECTION("separate directed exits do not create a speed wait") { }
	SECTION("a waiting siding can leave toward its separate exit") { Place(slow,14,12); }
	SECTION("an unresolved split preserves the original waiting rule") {
		MakeRailNormal(TileXY(15,20),_current_company,TRACK_BIT_X|TRACK_BIT_RIGHT,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(15,21),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(15,22),_current_company,TRACK_BIT_LEFT,RAILTYPE_RAIL);
		independent=false;
	}
	SECTION("an owned PBS branch resolves the split without running pathfinding") {
		MakeRailNormal(TileXY(15,20),_current_company,TRACK_BIT_X|TRACK_BIT_RIGHT,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(15,21),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(15,22),_current_company,TRACK_BIT_LEFT,RAILTYPE_RAIL);
		for(uint x=7;x<=27;x++)REQUIRE(TryReserveRailTrack(TileXY(x,20),TRACK_X));
	}
	SECTION("an unowned reserved branch is not a route decision") {
		MakeRailNormal(TileXY(15,20),_current_company,TRACK_BIT_X|TRACK_BIT_RIGHT,RAILTYPE_RAIL);
		REQUIRE(TryReserveRailTrack(TileXY(15,20),TRACK_X)); independent=false;
	}
	SECTION("a gap cannot use a rear reservation to choose a future branch") {
		MakeRailNormal(TileXY(15,20),_current_company,TRACK_BIT_X|TRACK_BIT_RIGHT,RAILTYPE_RAIL);
		for(uint x=14;x<=27;x++)REQUIRE(TryReserveRailTrack(TileXY(x,20),TRACK_X)); independent=false;
	}
	SECTION("a future level crossing is still a conflict between different exits") {
		confirmed_conflict = true; // Native crossing regression also proves the actual wait and tail release.
		MakeRailNormal(TileXY(20,20),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);
		for(uint y=1;y<20;y++)MakeRailNormal(TileXY(20,y),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20,10),_current_company,TRACK_BIT_X|TRACK_BIT_Y,RAILTYPE_RAIL);
		const TileIndex clear=TileXY(20,3);
		SB(_m[clear].m5,6,2,to_underlying(RailTileType::Signals));SetPresentSignals(clear,SignalAlongTrackdir(TRACKDIR_Y_NW));SetSignalType(clear,TRACK_Y,SignalType::Path);
		auto *program=GetTraceRestrictProgram(MakeTraceRestrictRefId(clear,TRACK_Y),true);
		TraceRestrictInstructionItem item{};item.SetType(TRIT_AUTO_YIELD_CLEAR);item.SetValue(membership->index);
		program->items={item.AsProgramItem()};REQUIRE(program->Validate().Succeeded());InvalidateAutoYieldAreas();UpdateAutoYieldAreas();independent=false;
	}
	SECTION("a long unresolved body exhausts optional proof without granting priority") {
		Train *part=fast;
		for(uint i=0;i<260;i++){Train *tail=MakeActor(7,20,200);tail->SetMultiheaded();part->SetNext(tail);part=tail;}independent=false;
	}
	SECTION("a shared exit retains the priority wait") {
		confirmed_conflict = true;
		MakeRailNormal(TileXY(20,20),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);
		for(uint y=11;y<20;y++)MakeRailNormal(TileXY(20,y),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		independent=false;
	}
	SECTION("the fast train tail still on the slow route retains the wait") {
		confirmed_conflict = true;
		Train *tail=MakeActor(12,10,200);tail->SetMultiheaded();fast->SetNext(tail);independent=false;
	}
	SECTION("an unfinished route is unknown") {
		MakeRailNormal(TileXY(25,20),_current_company,TRACK_BIT_RIGHT,RAILTYPE_RAIL);independent=false;
	}
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait)==confirmed_conflict);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield target prediction - only a uniquely reachable mandatory target resolves a fork")
{
	for (uint x=1; x<=40; x++) MakeRailNormal(TileXY(x,20),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Marker(3,20,AutoYieldRole::Approach); Marker(10,20,AutoYieldRole::Main); Marker(27,20,AutoYieldRole::Clear);
	MakeRailNormal(TileXY(15,20),_current_company,TRACK_BIT_X|TRACK_BIT_RIGHT,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(15,21),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(15,22),_current_company,TRACK_BIT_LEFT,RAILTYPE_RAIL);
	Station *target=Station::Create(TileXY(40,20));
	target->owner=_current_company; target->facilities={StationFacility::Train};
	target->train_station=TileArea(target->xy,1,1);target->rect.BeforeAddTile(target->xy,StationRect::ADD_FORCE);
	MakeRailStation(target->xy,_current_company,target->index,Axis::X,0,RAILTYPE_RAIL);
	Place(slow,9,10); Place(fast,7,20);
	fast->current_order.MakeGoToStation(target->index);
	Register(slow); Register(fast);
	bool independent=true;
	bool confirmed_conflict=false;
	SECTION("a single reachable station target identifies the exit before reservation") { }
	SECTION("a waypoint is also a mandatory target") {
		Waypoint *waypoint=Waypoint::Create(TileXY(39,20));waypoint->owner=_current_company;
		MakeRailWaypoint(waypoint->xy,_current_company,waypoint->index,Axis::X,0,RAILTYPE_RAIL);
		fast->current_order.MakeGoToWaypoint(waypoint->index);
	}
	SECTION("an explicit depot is a physical target") {
		MakeRailDepot(TileXY(40,20),_current_company,DepotID{0},DiagDirection::NE,RAILTYPE_RAIL);
		fast->current_order.MakeGoToDepot(DepotID{0},{});
	}
	SECTION("nearest depot does not bind to a stale explicit destination") {
		fast->current_order.MakeGoToDepot(DepotID{0},{},ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS,ODATFB_NEAREST_DEPOT);independent=false;
	}
	SECTION("an unspecified target preserves the old waiting rule") { fast->current_order.MakeDummy();independent=false; }
	SECTION("multiple branches reaching the target remain ambiguous") {
		for(uint x=16;x<20;x++)MakeRailNormal(TileXY(x,22),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20,22),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20,21),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20,20),_current_company,TRACK_BIT_X|TRACK_BIT_LOWER,RAILTYPE_RAIL);independent=false;
	}
	SECTION("different exits rejoining after clearance do not prove a unique target route") {
		for(uint x=16;x<30;x++)MakeRailNormal(TileXY(x,22),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		Marker(27,22,AutoYieldRole::Clear);
		MakeRailNormal(TileXY(30,22),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(30,21),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(30,20),_current_company,TRACK_BIT_X|TRACK_BIT_LOWER,RAILTYPE_RAIL);independent=false;
	}
	SECTION("a loop in an alternative cannot be mistaken for a dead end") {
		MakeRailNormal(TileXY(15,22),_current_company,TRACK_BIT_LEFT|TRACK_BIT_LOWER,RAILTYPE_RAIL);
		for(uint x=16;x<30;x++)MakeRailNormal(TileXY(x,22),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(30,22),_current_company,TRACK_BIT_RIGHT,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(30,23),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(30,24),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);
		for(uint x=16;x<30;x++)MakeRailNormal(TileXY(x,24),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(15,24),_current_company,TRACK_BIT_LEFT,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(15,23),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);independent=false;
	}
	SECTION("a non-target depot may reverse and is never a dead-end proof") {
		MakeRailDepot(TileXY(15,22),_current_company,DepotID{1},DiagDirection::NW,RAILTYPE_RAIL);independent=false;
	}
	SECTION("a long unresolved alternative is unknown, not unreachable") {
		for(uint x=16;x<60;x++)MakeRailNormal(TileXY(x,22),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(60,22),_current_company,TRACK_BIT_RIGHT,RAILTYPE_RAIL);
		for(uint y=23;y<50;y++)MakeRailNormal(TileXY(60,y),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(60,50),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);
		for(uint x=1;x<60;x++)MakeRailNormal(TileXY(x,50),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);independent=false;
	}
	SECTION("a shared tail still prevents early independence") {
		confirmed_conflict=true;
		Train *tail=MakeActor(12,10,200);tail->SetMultiheaded();fast->SetNext(tail);independent=false;
	}
	SECTION("a mandatory target inside the area may change the next route") {
		MakeRailStation(TileXY(22,20),_current_company,target->index,Axis::X,0,RAILTYPE_RAIL);independent=false;
	}
	GameRandomSeedChecker random_checker;
	const auto order_index=fast->cur_real_order_index;
	const auto destination=fast->dest_tile;
	const auto flags=fast->flags;
	const auto occupants=membership->occupants;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait)==confirmed_conflict);
	if(independent){
		/* Predictions cannot survive an active target change, even within the
		 * same tick. No order advancement or cross-evaluation cache is allowed. */
		Station *other=Station::Create(TileXY(40,10));other->owner=_current_company;
		MakeRailStation(other->xy,_current_company,other->index,Axis::X,0,RAILTYPE_RAIL);
		fast->current_order.MakeGoToStation(other->index);
		/* The changed target is unreachable on this independent line: unknown
		 * must not manufacture a shared resource. */
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	}
	CHECK(fast->cur_real_order_index==order_index);
	CHECK(fast->dest_tile==destination);CHECK(fast->flags==flags);
	CHECK(membership->occupants==occupants);
	CHECK(ValidateVehicleTileHash(slow));CHECK(ValidateVehicleTileHash(fast));
	for(uint x=1;x<=40;x++)CHECK(GetReservedTrackbits(TileXY(x,20))==TRACK_BIT_NONE);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield independent feeders - a free last lane precedes future capacity pressure")
{
	UpperSiding(false);
	slow->vcache.cached_max_speed=120; medium->vcache.cached_max_speed=120;
	Train *following=MakeActor(5,10,140);
	Place(medium,14,12);
	slow->cur_speed=20; medium->cur_speed=20; following->cur_speed=20; fast->cur_speed=20;
	Register(slow); Register(medium); Register(following); Register(fast);
	Execute(medium,10,12,TRPISP_ACQUIRE);
	SECTION("two independent approaches require three lane turns") { }
	SECTION("a second exit does not disable capacity turnover on a shared exit") {
		for(uint x=1;x<=40;x++)MakeRailNormal(TileXY(x,20),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		Marker(27,20,AutoYieldRole::Clear);
	}
	SECTION("without the third demand the normal equal speed rule remains") { membership->Vacate(following); }
	SECTION("manual cancellation excludes the candidate") { membership->Vacate(medium); }
	SECTION("loading cannot be interrupted") { medium->current_order.MakeLoading(true); }
	SECTION("an occupied alternative still leaves the upper lane free") { MakeActor(9,12,120); }
	DistantExpress(fast); DistantExpress(following,3);

	GameRandomSeedChecker random_checker;
	REQUIRE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield non-members - normal diversion uses the remaining physically free lane")
{
	UpperSiding(false);medium->vcache.cached_max_speed=80;
	Train *outside=MakeActor(14,8,120);
	Register(slow);Register(medium);Register(fast);
	bool parking_free=true;
	SECTION("a non-member standing in the siding consumes its place") { }
	SECTION("a manually cancelled occupant still consumes its physical place") {
		Register(outside);Execute(outside,10,8,TRPISP_ACQUIRE);membership->Vacate(outside);
	}
	SECTION("a reversing non-member still occupies the siding") { outside->flags.Set(VehicleRailFlag::Reversing); }
	SECTION("a backwards moving non-member still occupies the siding") { outside->direction=Direction::NE;outside->cur_speed=32; }
	SECTION("a reversed train may reserve a place before physically entering it") {
		MakeRailNormal(TileXY(19,8),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		Place(outside,19,8);outside->direction=Direction::NE;
		for(uint x=12;x<=19;x++)REQUIRE(TryReserveRailTrack(TileXY(x,8),TRACK_X));
	}
	SECTION("another company's parked train is protected") { outside->owner=CompanyID{2};outside->vehstatus.Set(VehState::Stopped); }
	SECTION("an unknown reservation still blocks a physically empty siding") {
		Place(outside,30,20);MakeRailNormal(outside->tile,_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		REQUIRE(TryReserveRailTrack(TileXY(14,8),TRACK_X));
	}
	SECTION("the tail of a non-member also consumes the place") {
		Place(outside,30,20);MakeRailNormal(outside->tile,_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		Train *tail=MakeActor(14,8,120);tail->SetMultiheaded();outside->SetNext(tail);
	}
	SECTION("two genuinely free places preserve the original allocation") {
		Place(outside,30,20);MakeRailNormal(outside->tile,_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	}
	SECTION("two physical occupants leave no free parking despite empty claims") { MakeActor(14,12,50);parking_free=false; }
	const auto members=membership->occupants;GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding)==parking_free);
	CHECK(((Execute(slow,10,10).flags&TRPRF_DENY)!=0)==parking_free);
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK(membership->occupants==members);CHECK_FALSE(membership->IsOccupant(outside->index));
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,8),TRACK_X))->occupants.empty());
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield non-members - full-lane turnover requires every legal parking alternative unavailable")
{
	UpperSiding(false);
	MakeRailNormal(TileXY(6,10),_current_company,TRACK_BIT_X|TRACK_BIT_UPPER,RAILTYPE_RAIL);
	for(uint y=7;y<=9;y++)MakeRailNormal(TileXY(6,y),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(6,6),_current_company,TRACK_BIT_LOWER,RAILTYPE_RAIL);
	for(uint x=7;x<22;x++)MakeRailNormal(TileXY(x,6),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(22,6),_current_company,TRACK_BIT_RIGHT,RAILTYPE_RAIL);
	for(uint y=7;y<=9;y++)MakeRailNormal(TileXY(22,y),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(22,10),_current_company,TRACK_BIT_X|TRACK_BIT_LEFT,RAILTYPE_RAIL);
	Marker(10,6,AutoYieldRole::Siding);Marker(18,6,AutoYieldRole::Exit);
	medium->vcache.cached_max_speed=120;Place(medium,14,12);
	Place(slow,5,10);Place(fast,3,10);
	Train *arrival=MakeActor(4,10,140);Train *outside=MakeActor(14,6,120);
	Register(slow);Register(medium);Register(arrival);Register(fast);Execute(medium,10,12,TRPISP_ACQUIRE);
	slow->cur_speed=medium->cur_speed=fast->cur_speed=arrival->cur_speed=20;
	bool evacuate=false;
	SECTION("a non-member plus three demands cannot consume a still empty upper lane") { }
	SECTION("a reversing outside occupant consumes the same capacity") { outside->flags.Set(VehicleRailFlag::Reversing); }
	SECTION("an opposite-direction departure also consumes the place") { outside->direction=Direction::NE;outside->cur_speed=32; }
	SECTION("foreign occupancy is equally physical") { outside->owner=CompanyID{2}; }
	SECTION("manual cancellation does not make the lane physically empty") {
		Register(outside);Execute(outside,10,6,TRPISP_ACQUIRE);membership->Vacate(outside);
	}
	SECTION("without the outside occupant no extra turnover is needed") {
		Place(outside,30,20);MakeRailNormal(outside->tile,_current_company,TRACK_BIT_X,RAILTYPE_RAIL);evacuate=false;
	}
	SECTION("all remaining parking physically occupied requires actual turnover") { MakeActor(14,8,120);evacuate=true; }
	const auto members=membership->occupants;GameRandomSeedChecker random_checker;
	DistantExpress(fast); DistantExpress(arrival,3);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding)==!evacuate);
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate)==evacuate);
	CHECK_FALSE(EvaluateAutoYield(outside,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK(membership->occupants==members);CHECK_FALSE(membership->IsOccupant(outside->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield selected departures - prefer a free main before the siding fork")
{
	UseExitOnly(); Place(medium,7,10); Place(slow,5,10);
	medium->current_order.MakeGoToDepot(DepotID{0}, {});
	Register(slow); Register(medium); Register(fast);
	Execute(medium,18,12,TRPISP_ACQUIRE);
	bool prefer_main = true;
	SECTION("a selected depot bound departure can use the main") { }
	SECTION("a blocked main retains the siding route") { MakeRailNormal(TileXY(11,10),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL); prefer_main=false; }
	SECTION("a mandatory waypoint retains its route") { medium->current_order.MakeGoToWaypoint(StationID{0}); prefer_main=false; }
	SECTION("an unresolved station destination retains its route") { medium->current_order.MakeGoToStation(StationID{0}); prefer_main=false; }
	SECTION("an equal speed train still required to wait does not bypass") { medium->vcache.cached_max_speed=100; medium->cur_speed=20; slow->cur_speed=20; fast->cur_speed=20; prefer_main=false; }
	GameRandomSeedChecker random_checker;
	CHECK(((Execute(medium,10,10).flags&TRPRF_DENY)==0)==prefer_main);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X))->IsOccupant(medium->index));
	CHECK(GetReservedTrackbits(TileXY(10,10))==TRACK_BIT_NONE);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield capacity pressure - clear the eligible slow train farthest from its follower")
{
	UpperSiding(false);
	Train *upper = MakeActor(16,8,160);
	Place(medium,14,12);
	Register(slow); Register(medium); Register(upper); Register(fast);
	Execute(medium,10,12,TRPISP_ACQUIRE); Execute(upper,10,8,TRPISP_ACQUIRE);
	Train *expected = upper;
	SECTION("the farther train clears before the nearer equal speed train") { }
	SECTION("capacity pressure can clear an equal speed train before a queue stops") {
		slow->vcache.cached_max_speed=160; medium->cur_speed=20; upper->cur_speed=20; slow->cur_speed=20;
		DistantExpress(fast);
	}
	SECTION("blocked exits retain their protection") { MakeRailNormal(TileXY(19,8),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL); expected=medium; }
	SECTION("station work cannot be interrupted") { upper->current_order.MakeLoading(true); expected=medium; }
	SECTION("manual membership cancellation remains authoritative") { membership->Vacate(upper); expected=medium; }
	SECTION("a siding train slower than the arrival is ineligible") { upper->vcache.cached_max_speed=80; expected=medium; }
	SECTION("an already passing fast train still prevents both departures") { Place(fast,12,10); expected=nullptr; }
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(upper,membership->index.base(),AutoYieldCondition::Evacuate)==(expected==upper));
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate)==(expected==medium));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield turnover - equal speed clears platforms before ordinary sidings")
{
	const bool exit_only = GENERATE(false, true);
	SetupPlatform(exit_only); PassStation(slow);
	Train *upper = MakeActor(14, 8, 160);
	AutoRestoreBackup passenger_class(CargoSpec::Get(0)->classes, CargoClasses{CargoClass::Passengers});
	AutoRestoreBackup freight_class(CargoSpec::Get(1)->classes, CargoClasses{CargoClass::Bulk});
	medium->cargo_type = CargoType{1}; medium->cargo_cap = 10;
	upper->cargo_type = CargoType{0}; upper->cargo_cap = 10;
	Train *expected = medium;
	SECTION("platform freight before ordinary passenger at equal speed") { }
	SECTION("two platforms prefer the passenger") {
		Marker(18, 8, AutoYieldRole::Exit, membership->index.base(), TEST_YIELD_CONFIG | AUTO_YIELD_PLATFORM);
		expected = upper;
	}
	SECTION("ordinary sidings retain passenger preference") {
		Marker(18, 12, AutoYieldRole::Exit); expected = upper;
	}
	SECTION("a platform doing station work is never cleared") {
		medium->current_order.MakeLoading(true); medium->last_station_visited = platform_station->index;
		expected = upper;
	}
	SECTION("a platform with no safe exit is never cleared") {
		MakeRailNormal(TileXY(19, 12), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		expected = upper;
	}
	Register(slow); Register(medium); Register(fast); Register(upper);
	Place(medium, 14, 12);
	Execute(medium, exit_only ? 18 : 10, 12, TRPISP_ACQUIRE);
	Execute(upper, exit_only ? 18 : 10, 8, TRPISP_ACQUIRE);
	fast->cur_speed = 20;
	DistantExpress(fast);

	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(expected, membership->index.base(), AutoYieldCondition::Evacuate));
	CHECK_FALSE(EvaluateAutoYield(expected == medium ? upper : medium, membership->index.base(), AutoYieldCondition::Evacuate));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield turnover - following stopping passengers clear the fastest suitable platform")
{
	const bool exit_only = GENERATE(false, true);
	SetupPlatform(exit_only); PassStation(slow);
	Train *upper = MakeActor(14, 8, 120);
	AutoRestoreBackup passenger_class(CargoSpec::Get(0)->classes, CargoClasses{CargoClass::Passengers});
	AutoRestoreBackup freight_class(CargoSpec::Get(1)->classes, CargoClasses{CargoClass::Bulk});
	medium->cargo_type = CargoType{1}; medium->cargo_cap = 10;
	upper->cargo_type = CargoType{0}; upper->cargo_cap = 10;
	fast->cargo_type = CargoType{0}; fast->cargo_cap = 10;
	fast->current_order.MakeGoToStation(platform_station->index);
	for (uint x = 11; x <= 15; x++) MakeRailStation(TileXY(x, 8), _current_company, platform_station->index, Axis::X, 0, RAILTYPE_RAIL);
	platform_station->train_station = TileArea(TileXY(11, 8), 5, 5);
	platform_station->rect.BeforeAddTile(TileXY(11, 8), StationRect::ADD_FORCE);
	Marker(18, 8, AutoYieldRole::Exit, membership->index.base(), TEST_YIELD_CONFIG | AUTO_YIELD_PLATFORM);
	Train *expected = medium;
	const char *scenario = "fastest";
	SECTION("fastest platform freight before a slower platform passenger") { }
	SECTION("equal speed platforms clear the passenger first") { scenario = "equal"; upper->vcache.cached_max_speed = 160; expected = upper; }
	SECTION("ordinary freight cannot take the requested platform turn") { scenario = "ordinary"; Marker(18, 12, AutoYieldRole::Exit); expected = upper; }
	SECTION("loading platform freight cannot be forced out") {
		scenario = "loading"; medium->current_order.MakeLoading(true); medium->last_station_visited = platform_station->index; expected = nullptr;
	}
	SECTION("a non-stopping passenger uses the highest safe design speed policy") { scenario = "nonstop"; PassStation(fast); expected = medium; }
	Register(slow); Register(medium); Register(fast); Register(upper);
	Place(medium, 14, 12);
	Execute(medium, exit_only ? 18 : 10, 12, TRPISP_ACQUIRE);
	Execute(upper, exit_only ? 18 : 10, 8, TRPISP_ACQUIRE);
	fast->cur_speed = 20;
	if (fast->current_order.GetNonStopType() == ONSF_NO_STOP_AT_ANY_STATION) DistantExpress(fast);

	GameRandomSeedChecker random_checker;
	CAPTURE(exit_only, scenario);
	CHECK(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::Evacuate) == (expected == medium));
	CHECK(EvaluateAutoYield(upper, membership->index.base(), AutoYieldCondition::Evacuate) == (expected == upper));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield turnover - a slower stopping passenger cannot release a platform before a passing express")
{
	const bool exit_only = GENERATE(false, true);
	SetupPlatform(exit_only);
	Train *upper = MakeActor(14, 8, 120);
	AutoRestoreBackup passenger_class(CargoSpec::Get(0)->classes, CargoClasses{CargoClass::Passengers});
	slow->cargo_type = CargoType{0}; slow->cargo_cap = 10;
	slow->current_order.MakeGoToStation(platform_station->index);
	Train *rear = MakeActor(2, 10, 220);
	Register(slow); Register(medium); Register(fast); Register(upper); Register(rear);
	Place(medium, 14, 12);
	Execute(medium, exit_only ? 18 : 10, 12, TRPISP_ACQUIRE);
	Execute(upper, exit_only ? 18 : 10, 8, TRPISP_ACQUIRE);
	Place(fast, 12, 10);
	GameRandomSeedChecker random_checker;
	CHECK_FALSE(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::Evacuate));
	CHECK_FALSE(EvaluateAutoYield(upper, membership->index.base(), AutoYieldCondition::Evacuate));
	CHECK((Execute(medium, 18, 12).flags & TRPRF_WAIT_AT_PBS) != 0);
	Place(fast, 30, 10); Execute(fast, 27, 10, TRPISP_RELEASE_BACK);
	CHECK(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::Evacuate));
	CHECK_FALSE(EvaluateAutoYield(upper, membership->index.base(), AutoYieldCondition::Evacuate));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield station access - fastest scheduled arrival can free its occupied platform")
{
	const bool exit_only = GENERATE(false, true);
	SetupPlatform(exit_only);
	slow->vcache.cached_max_speed = 160;
	medium->vcache.cached_max_speed = 120;
	fast->vcache.cached_max_speed = 100;
	slow->current_order.MakeGoToStation(platform_station->index);
	bool release = true;
	const char *scenario = "";
	SECTION("a free ordinary siding cannot replace the required platform") { scenario = "a free ordinary siding cannot replace the required platform"; }
	SECTION("completed empty locomotive must also free a required platform") { scenario = "completed empty locomotive must also free a required platform"; medium->cargo_cap = 0; slow->cargo_cap = 0; }
	SECTION("loading must finish normally") { scenario = "loading must finish normally"; medium->current_order.MakeLoading(true); medium->last_station_visited = platform_station->index; release = false; }
	SECTION("an occupant must first complete its own reserved station stop") { medium->current_order.MakeGoToStation(platform_station->index); release = false; }
	SECTION("coupling must finish normally") { scenario = "coupling must finish normally"; medium->current_order.MakeWaitCouple(); release = false; }
	SECTION("a blocked platform exit cannot release") { scenario = "a blocked platform exit cannot release"; MakeRailNormal(TileXY(19, 12), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL); release = false; }
	SECTION("a physically blocked arrival cannot evict another train") { scenario = "a physically blocked arrival cannot evict another train"; Train *blocker = MakeActor(8, 11, 50); blocker->track = TRACK_BIT_Y; release = false; }
	SECTION("a non-stopping train does not demand the platform") { scenario = "a non-stopping train does not demand the platform"; PassStation(slow); release = false; }
	SECTION("an unrelated station does not demand the platform") { scenario = "an unrelated station does not demand the platform"; slow->current_order.MakeGoToStation(StationID{123}); release = false; }
	SECTION("a free suitable platform needs no turnover") { scenario = "a free suitable platform needs no turnover";
		for (uint x = 11; x <= 15; x++) MakeRailStation(TileXY(x, 8), _current_company, platform_station->index, Axis::X, 0, RAILTYPE_RAIL);
		platform_station->train_station = TileArea(TileXY(11, 8), 5, 5);
		platform_station->rect.BeforeAddTile(TileXY(11, 8), StationRect::ADD_FORCE);
		Marker(18, 8, AutoYieldRole::Exit, membership->index.base(), TEST_YIELD_CONFIG | AUTO_YIELD_PLATFORM);
		release = false;
	}
	Register(slow); Register(medium); Register(fast);
	Place(medium, 14, 12); Execute(medium, exit_only ? 18 : 10, 12, TRPISP_ACQUIRE);
	GameRandomSeedChecker random_checker;
	CAPTURE(exit_only, scenario);
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::NeedsSiding));
	CHECK(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::Evacuate) == release);
	const auto result = Execute(medium, 18, 12);
	if (release) CHECK((result.flags & TRPRF_WAIT_AT_PBS) == 0);
	CHECK((result.flags & TRPRF_DENY) == 0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform turnover - an unconfirmed neighbouring route cannot veto station access")
{
	const bool exit_only = GENERATE(false, true);
	SetupPlatform(exit_only);
	slow->vcache.cached_max_speed=160; medium->vcache.cached_max_speed=120;
	slow->current_order.MakeGoToStation(platform_station->index);
	Register(slow); Register(medium); Place(medium,14,12);
	Execute(medium,exit_only?18:10,12,TRPISP_ACQUIRE);
	for(uint x=1;x<=40;x++)MakeRailNormal(TileXY(x,30),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	auto *neighbour=TraceRestrictSlot::Create(_current_company);neighbour->max_occupancy=16;
	Marker(3,30,AutoYieldRole::Approach,neighbour->index.base());
	Marker(10,30,AutoYieldRole::Main,neighbour->index.base());
	Marker(18,32,AutoYieldRole::Exit,neighbour->index.base());
	Marker(27,30,AutoYieldRole::Clear,neighbour->index.base());
	auto *uncertain=Marker(6,30,AutoYieldRole::Approach,neighbour->index.base());
	TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);uncertain->items={deny.AsProgramItem()};
	REQUIRE(uncertain->Validate().Succeeded());InvalidateAutoYieldAreas();UpdateAutoYieldAreas();
	Place(fast,4,30);REQUIRE(neighbour->Occupy(fast));
	bool release=true;
	SECTION("no confirmed conflict leaves the native platform departure available") {}
	SECTION("a saved valid dependency survives an incomplete forecast") {
		/* A real possible merge is hidden by the temporary restriction.
		 * A permanently disconnected corridor is a positive cancellation. */
		auto *block=Marker(5,30,AutoYieldRole::Approach,neighbour->index.base());block->items={deny.AsProgramItem()};REQUIRE(block->Validate().Succeeded());
		MakeRailNormal(TileXY(6,30),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);
		for(uint y=11;y<30;y++)MakeRailNormal(TileXY(6,y),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(6,10),_current_company,TRACK_BIT_X|TRACK_BIT_LOWER,RAILTYPE_RAIL);
		for(auto *slot:TraceRestrictSlot::Iterate())if(slot->automatic_yield_area==membership->index.base()+1&&slot->automatic_yield_signal==MakeTraceRestrictRefId(TileXY(exit_only?18:10,12),TRACK_X)&&slot->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets))REQUIRE(slot->Occupy(fast));
		release=false;
	}
	GameRandomSeedChecker random;
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate)==release);
	CHECK_FALSE(membership->IsOccupant(fast->index));
	CHECK(neighbour->IsOccupant(fast->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield station access - another passing express still blocks the platform turnover")
{
	const bool exit_only = GENERATE(false, true);
	SetupPlatform(exit_only);
	slow->vcache.cached_max_speed = 160; medium->vcache.cached_max_speed = 120;
	slow->current_order.MakeGoToStation(platform_station->index);
	Register(slow); Register(medium); Register(fast);
	Place(medium, 14, 12); Execute(medium, exit_only ? 18 : 10, 12, TRPISP_ACQUIRE);
	Place(fast, 12, 10);
	CHECK_FALSE(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::Evacuate));
	CHECK((Execute(medium, 18, 12).flags & TRPRF_WAIT_AT_PBS) != 0);
	Place(fast, 30, 10); Execute(fast, 27, 10, TRPISP_RELEASE_BACK);
	CHECK(EvaluateAutoYield(medium, membership->index.base(), AutoYieldCondition::Evacuate));
	const auto main = Execute(slow, 10, 10);
	CHECK((main.flags & (TRPRF_WAIT_AT_PBS | TRPRF_PBS_RES_END_WAIT)) == (TRPRF_WAIT_AT_PBS | TRPRF_PBS_RES_END_WAIT));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield station access - occupied platforms retain fastest then passenger selection")
{
	const bool exit_only = GENERATE(false, true);
	SetupPlatform(exit_only);
	for (uint x = 11; x <= 15; x++) MakeRailStation(TileXY(x, 8), _current_company, platform_station->index, Axis::X, 0, RAILTYPE_RAIL);
	platform_station->train_station = TileArea(TileXY(11, 8), 5, 5);
	platform_station->rect.BeforeAddTile(TileXY(11, 8), StationRect::ADD_FORCE);
	Marker(18, 8, AutoYieldRole::Exit, membership->index.base(), TEST_YIELD_CONFIG | AUTO_YIELD_PLATFORM);
	slow->vcache.cached_max_speed = 180; slow->current_order.MakeGoToStation(platform_station->index);
	medium->vcache.cached_max_speed = 140; fast->vcache.cached_max_speed = 100;
	Train *upper = MakeActor(14, 8, 120);
	AutoRestoreBackup passenger_class(CargoSpec::Get(0)->classes, CargoClasses{CargoClass::Passengers});
	AutoRestoreBackup freight_class(CargoSpec::Get(1)->classes, CargoClasses{CargoClass::Bulk});
	upper->cargo_type = CargoType{0}; upper->cargo_cap = 10;
	medium->cargo_type = CargoType{1}; medium->cargo_cap = 10;
	Train *expected = medium;
	SECTION("fastest ready platform leaves even when the requester is faster") { }
	SECTION("passenger wins only at equal speed") { upper->vcache.cached_max_speed = 140; expected = upper; }
	SECTION("fastest platform still loading cannot be forced out") {
		medium->current_order.MakeLoading(true); medium->last_station_visited = platform_station->index;
		medium->current_order.SetLoadType(OrderLoadType::FullLoadAny); expected = upper;
	}
	Register(slow); Register(medium); Register(fast); Register(upper);
	Place(medium, 14, 12); Execute(medium, exit_only ? 18 : 10, 12, TRPISP_ACQUIRE);
	Execute(upper, exit_only ? 18 : 10, 8, TRPISP_ACQUIRE);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(expected, membership->index.base(), AutoYieldCondition::Evacuate));
	CHECK_FALSE(EvaluateAutoYield(expected == medium ? upper : medium, membership->index.base(), AutoYieldCondition::Evacuate));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield allocation - catching ranks do not cancel available normal parking")
{
	medium->vcache.cached_max_speed=slow->vcache.cached_max_speed;
	Place(medium,6,10);Register(slow);Register(medium);Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::NeedsSiding));
	medium->vcache.cached_max_speed=120;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::NeedsSiding));
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield turnover - equal stopped queues recover with platform only and mixed sidings")
{
	const uint layout=GENERATE(0u,1u,2u);
	SetupPlatform(true);PassStation(slow);
	if(layout==0)Marker(18,12,AutoYieldRole::Exit);
	if(layout==1)Marker(18,8,AutoYieldRole::Exit,membership->index.base(),TEST_YIELD_CONFIG|AUTO_YIELD_PLATFORM);
	slow->vcache.cached_max_speed=medium->vcache.cached_max_speed=100;
	Train *blocker=MakeActor(14,8,50);blocker->vehstatus.Set(VehState::Stopped);
	Register(slow);Register(medium);Register(fast);Place(medium,14,12);Execute(medium,18,12,TRPISP_ACQUIRE);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(medium,18,12).flags&TRPRF_WAIT_AT_PBS)==0);
	medium->current_order.MakeLoading(true);medium->last_station_visited=platform_station->index;
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - a departing tail retains the incoming train's turn until clearance")
{
	const bool exit_only = GENERATE(false,true);
	if (exit_only) UseExitOnly();
	Register(slow); Register(medium); Register(fast);
	Place(medium,14,12); Execute(medium,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	Train *rear = MakeTrain(); medium->SetNext(rear);
	rear->owner = _current_company; rear->track = TRACK_BIT_X; rear->direction = Direction::SW;
	Train *middle = MakeTrain(); medium->SetNext(middle); middle->SetNext(rear);
	middle->owner = _current_company; middle->track = TRACK_BIT_X; middle->direction = Direction::SW;
	Place(medium,30,10); Place(middle,27,10); Place(rear,25,10);
	medium->gcache.cached_total_length = 6 * TILE_SIZE;
	REQUIRE(membership->IsOccupant(medium->index));
	CHECK((Execute(slow,10,10).flags & TRPRF_WAIT_AT_PBS) != 0);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,12),TRACK_X))->IsOccupant(medium->index));
	Execute(medium,27,10,TRPISP_RELEASE_BACK);
	Place(middle,29,10); Place(rear,29,10);
	CHECK_FALSE(membership->IsOccupant(medium->index));
	CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - route proofs distinguish own junction reservations from foreign ones")
{
	medium->vcache.cached_max_speed = fast->vcache.cached_max_speed; // Isolate PBS ownership from scarcity ranking.
	Register(slow); Register(medium); Register(fast);
	REQUIRE(TryReserveRailTrack(TileXY(8,10),TRACK_X));
	REQUIRE(TryReserveRailTrack(TileXY(9,10),TRACK_X));
	SECTION("own main reservation does not conceal the diverging siding") {
		REQUIRE(TryReserveRailTrack(TileXY(7,10),TRACK_X));
		REQUIRE(GetTrainForReservation(TileXY(8,10),TRACK_X) == slow);
		CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
		CHECK((Execute(slow,10,10).flags & TRPRF_DENY) != 0);
	}
	SECTION("another train's main reservation is still protected") {
		Place(medium,9,10);
		REQUIRE(GetTrainForReservation(TileXY(8,10),TRACK_X) == medium);
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	}
	SECTION("unattributed crossing reservation is still protected") {
		CHECK(GetTrainForReservation(TileXY(8,10),TRACK_X) == nullptr);
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	}
	CHECK(GetReservedTrackbits(TileXY(8,10)) == TRACK_BIT_X);
	CHECK(GetReservedTrackbits(TileXY(9,10)) == TRACK_BIT_X);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield - ordinary platform and mixed layouts share safe waiting and turnover")
{
	const bool exit_only = GENERATE(false,true);
	const uint layout = GENERATE(0u,1u,2u); // Ordinary only, platform only, mixed.
	SetupPlatform(exit_only); PassStation(slow);
	if (layout == 0) Marker(18,12,AutoYieldRole::Exit);
	if (layout == 1) {
		for (uint x=11; x<=15; x++) MakeRailStation(TileXY(x,8),_current_company,platform_station->index,Axis::X,0,RAILTYPE_RAIL);
		platform_station->train_station = TileArea(TileXY(11,8),5,5);
		platform_station->rect.BeforeAddTile(TileXY(11,8),StationRect::ADD_FORCE);
		Marker(18,8,AutoYieldRole::Exit,membership->index.base(),TEST_YIELD_CONFIG|AUTO_YIELD_PLATFORM);
	}
	Register(slow); Register(medium); Register(fast);
	Place(medium,14,12); Execute(medium,exit_only ? 18 : 10,12,TRPISP_ACQUIRE);
	GameRandomSeedChecker random_checker;
	REQUIRE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(medium,18,12).flags&TRPRF_WAIT_AT_PBS)!=0);
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	Train *blocker = MakeActor(14,8,30); blocker->vehstatus.Set(VehState::Stopped);
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(slow,10,10).flags&TRPRF_WAIT_AT_PBS)!=0);
	Place(medium,30,10); Execute(medium,27,10,TRPISP_RELEASE_BACK);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield - a proven opposing train does not keep a siding waiting")
{
	Register(slow); Register(fast);
	medium->vehstatus.Set(VehState::Stopped);
	GameRandomSeedChecker random_checker;
	SECTION("opposing faster train remains in the same source slot") {
		fast->direction = Direction::NE;
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
		CHECK((Execute(slow,18,12).flags&TRPRF_WAIT_AT_PBS)==0);
		fast->direction = Direction::SW;
		CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
		fast->direction = Direction::NE;
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	}
	SECTION("a following fast train still has priority") {
		CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	}
	SECTION("a faster train between Main and Clear is still passing") {
		Place(fast,15,10);
		CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	}
	SECTION("a passing fast train remains protected until its tail clears") {
		Place(fast,30,10);
	Train *tail = MakeActor(27,10,200); tail->SetWagon(); fast->SetNext(tail);
		CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
		Execute(fast,27,10,TRPISP_RELEASE_BACK);
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	}
	CHECK(membership->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}


class AutomaticYieldMultipleMainFixture : public AutomaticYieldFixture {
protected:
	void ParallelBranch()
	{
		UpperSiding(true);
		GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,8),TRACK_X),false)->items.clear();
		MakeRailNormal(TileXY(18,8), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		Marker(10,8,AutoYieldRole::Main);
	}
	void IndependentApproach(uint row = 16)
	{
		for (uint x=1;x<24;x++) MakeRailNormal(TileXY(x,row),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(24,row),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);
		for (uint y=11;y<row;y++) MakeRailNormal(TileXY(24,y),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(24,10),_current_company,TRACK_BIT_X|TRACK_BIT_LOWER,RAILTYPE_RAIL);
		Marker(3,row,AutoYieldRole::Approach);
		Marker(10,row,AutoYieldRole::Main);
	}};

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - an unrelated main does not disable the area")
{
	Marker(10,16,AutoYieldRole::Main);
	Register(slow);Register(fast);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - separate approaches merging at a common exit share priority")
{
	IndependentApproach();
	Place(fast,2,16);
	Register(slow);Register(fast);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	// Once waiting in a shared siding, the merging fast train still has priority.
	Place(slow,14,12);Execute(slow,10,12,TRPISP_ACQUIRE);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Evacuate));
	Execute(fast,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - usable parallel main avoids a diversion but blocked tracks do not")
{
	ParallelBranch();Register(slow);Register(fast);
	bool parallel_free=false;
	SECTION("a free parallel track can carry normal traffic") { parallel_free=true; }
	SECTION("an occupant beyond the parallel main prevents bypass") { Place(medium,14,8);medium->vehstatus.Set(VehState::Stopped); }
	SECTION("an unknown PBS owner prevents bypass") { SetTrackReservation(TileXY(14,8),TRACK_BIT_X); }
	SECTION("a disconnected parallel track prevents bypass") { MakeRailNormal(TileXY(14,8),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL); }
	SECTION("ordinary path restrictions still apply") {
		auto *p=Marker(14,8,AutoYieldRole::Approach,INVALID_TRACE_RESTRICT_SLOT_ID.base());
		TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);p->items={deny.AsProgramItem()};REQUIRE(p->Validate().Succeeded());
		InvalidateAutoYieldAreas();UpdateAutoYieldAreas();
	}
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding)==!parallel_free);
	CHECK(((Execute(slow,10,10).flags&TRPRF_DENY)!=0)==!parallel_free);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - serial markers are not a parallel escape route")
{
	Marker(14,10,AutoYieldRole::Main);Register(slow);Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	Place(slow,12,10);
	/* The follower remains a conflict through the second serial control,
	 * but a train already beyond the siding fork must not be held there. */
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,14,10).flags&(TRPRF_DENY|TRPRF_WAIT_AT_PBS))==0);
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - a proven merging follower allows safe siding turnover")
{
	IndependentApproach();Place(fast,2,16);Register(slow);Register(fast);Register(medium);
	Place(medium,14,12);Execute(medium,10,12,TRPISP_ACQUIRE);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate)); // Independent close approach has priority.
	DistantExpress(fast,2,16);fast->cur_speed=20;
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(slow,10,10).flags&TRPRF_WAIT_AT_PBS)!=0);
	// A real faster follower on the same main can still trigger a safe turnover.
	Place(fast,4,10);fast->cur_speed=0;slow->cur_speed=0;
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(slow,10,10).flags&TRPRF_WAIT_AT_PBS)!=0);
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield merging mains - common-exit distances and positive topology proofs control admission")
{
	IndependentApproach(); Place(fast,2,16); Place(medium,1,30);medium->vehstatus.Set(VehState::Stopped);
	bool should_yield = true;
	SECTION("another feeder has a faster following train") { }
	SECTION("distance to its own Main is shorter but distance to the common exit is longer") { Place(slow,1,10);Place(fast,5,16); }
	SECTION("fast train passed its own Main but has not caught up at the merge") { Place(fast,11,16); }
	SECTION("fast train is already ahead at the common exit") { Place(fast,20,16);should_yield=false; }
	SECTION("disconnected feeder cannot create a shared bottleneck") { MakeRailNormal(TileXY(24,12),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);should_yield=false; }
	SECTION("downstream route choice is not a proven common bottleneck") { MakeRailNormal(TileXY(14,16),_current_company,TRACK_BIT_X | TRACK_BIT_RIGHT,RAILTYPE_RAIL);MakeRailNormal(TileXY(14,17),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);should_yield=false; }
	SECTION("an ordinary restriction forbids the other feeder's continuation") {
		auto *program=Marker(14,16,AutoYieldRole::Approach,INVALID_TRACE_RESTRICT_SLOT_ID.base());
		TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);program->items={deny.AsProgramItem()};REQUIRE(program->Validate().Succeeded());
		InvalidateAutoYieldAreas();UpdateAutoYieldAreas();should_yield=false;
	}
	SECTION("opposing train on the other feeder has no priority") { fast->direction=Direction::NE;should_yield=false; }
	SECTION("stopped train on the other feeder has no priority") { fast->vehstatus.Set(VehState::Stopped);should_yield=false; }
	SECTION("equal speed empty train on another feeder has no priority") { fast->vcache.cached_max_speed=100;should_yield=false; }
	Register(slow);Register(fast);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding)==should_yield);
	CHECK(((Execute(slow,10,10).flags & TRPRF_DENY)!=0)==should_yield);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield merging mains - equal speed passenger priority still requires proximity")
{
	const bool nearby = GENERATE(false,true);
	const uint row = nearby ? 16 : 26;
	IndependentApproach(row);Place(fast,2,row);
	fast->vcache.cached_max_speed=100;
	AutoRestoreBackup passenger_class(CargoSpec::Get(0)->classes,CargoClasses{CargoClass::Passengers});
	fast->cargo_type=CargoType{0};fast->cargo_cap=10;
	fast->cargo.Append(CargoPacket::Create(uint16_t{1},uint16_t{0},StationID::Invalid(),TileIndex{0},Money{0}));
	Register(slow);Register(fast);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding)==nearby);
	CHECK(((Execute(slow,10,10).flags & TRPRF_DENY)!=0)==nearby);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield merging mains - rail edits and disconnected feeders cannot retain merge proofs")
{
	IndependentApproach();DistantExpress(fast,2,16);fast->cur_speed=20;Register(slow);Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	MakeRailNormal(TileXY(24,12),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	Register(medium);Place(medium,14,12);Execute(medium,10,12,TRPISP_ACQUIRE);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(slow,10,10).flags & TRPRF_WAIT_AT_PBS)==0);
	MakeRailNormal(TileXY(24,12),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - marker cap keeps rebuild and evaluation bounded")
{
	const uint mains=GENERATE(16u,17u);
	for(uint i=1;i<mains;i++)Marker(2+i,20,AutoYieldRole::Main);
	Register(slow);Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding)==(mains==16));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}


TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - a third free branch is checked after a blocked alternative")
{
	ParallelBranch();
	// A Main on the former siding supplies a third route, with a separate
	// exit-only lower siding so that all three roles remain valid.
	GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X),false)->items.clear();
	GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X),false)->items.clear();
	MakeRailNormal(TileXY(18,12),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Marker(10,12,AutoYieldRole::Main);
	// Keep an unreachable but valid siding exit: it must not affect the bypass proof.
	Marker(18,20,AutoYieldRole::Exit);
	Register(slow);Register(fast);
	Place(medium,14,8);medium->vehstatus.Set(VehState::Stopped);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags&(TRPRF_DENY|TRPRF_WAIT_AT_PBS))==0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - rebuilding preserves exclusive claims and manual opt out")
{
	ParallelBranch();Register(slow);Register(fast);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	const auto lane_id=Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->index;
	const auto slot_count=TraceRestrictSlot::GetNumItems();
	ResetAutoYieldAreas();UpdateAutoYieldAreas();
	CHECK(TraceRestrictSlot::GetNumItems()==slot_count);
	CHECK(TraceRestrictSlot::Get(lane_id)->IsOccupant(slow->index));
	CHECK((Execute(fast,10,12,TRPISP_ACQUIRE).flags&TRPRF_WAIT_AT_PBS)!=0);
	REQUIRE(CmdRemoveVehicleTraceRestrictSlot(DoCommandFlag::Execute,membership->index,slow->index).Succeeded());
	CHECK_FALSE(TraceRestrictSlot::Get(lane_id)->IsOccupant(slow->index));
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}


TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield multi-main - stable choices do not depend on slot registration order")
{
	ParallelBranch();
	Place(medium,14,8);medium->vehstatus.Set(VehState::Stopped);
	const bool reverse=GENERATE(false,true);
	if(reverse){Register(fast);Register(slow);}else{Register(slow);Register(fast);}
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	CHECK(GetRailReservationTrackBits(TileXY(14,8))==TRACK_BIT_NONE);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}


TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - both trains are clamped before threshold and waiting comparisons")
{
	const uint cap=GENERATE(1u,100u,124u,125u,160u,65535u);
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|cap);
	Register(slow);Register(medium);
	const bool faster=cap>=125;
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster)==faster);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait)==faster);
	CHECK(((Execute(slow,10,10).flags&TRPRF_DENY)!=0)==faster);
	CHECK(slow->GetDisplayMaxSpeed()==100);CHECK(medium->GetDisplayMaxSpeed()==160);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - default rail limits are read live and zero is unlimited")
{
	const uint16_t cap=GENERATE(0,100,124,125,160,65535);
	AutoRestoreBackup<uint16_t> rail_limit(const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->max_speed,cap);
	Register(slow);Register(medium);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster)==(cap==0||cap>=125));
	// Rail conversion/NewGRF changes must not require re-saving the signal program.
	const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->max_speed=100;
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->max_speed=0;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - manual mode overrides the rail default")
{
	AutoRestoreBackup<uint16_t> rail_limit(const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->max_speed,80);
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|160);
	Register(slow);Register(medium);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
	Marker(3,10,AutoYieldRole::Approach);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - multiple detectors use the lowest cap independent of marker order")
{
	const bool reverse=GENERATE(false,true);
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|(reverse?160:100));
	Marker(5,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|(reverse?100:160));
	Register(slow);Register(medium);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)==0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - equal clamped speeds cannot trigger eviction")
{
	Register(slow);Register(medium);Register(fast);
	Place(medium,14,12);Execute(medium,10,12,TRPISP_ACQUIRE);
	REQUIRE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|100);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(slow,10,10).flags&TRPRF_WAIT_AT_PBS)==0);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->IsOccupant(medium->index));
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - commands preserve cap on edits and copies and reset it on role changes")
{
	const TileIndex tile=TileXY(3,10);
	const auto *program=GetExistingTraceRestrictProgram(tile,TRACK_X);
	const uint32_t config=(30u<<16)|160u;
	REQUIRE(CmdProgramSignalTraceRestrict({},tile,TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,config,"").Succeeded());
	CHECK(program->items[1].base()==TEST_YIELD_CONFIG);
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,tile,TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,config,"").Succeeded());
	CHECK(program->items[1].base()==config);
	REQUIRE(CmdProgramSignalTraceRestrictMgmt(DoCommandFlag::Execute,TileXY(10,10),TRACK_X,TRMDCT_PROG_COPY,tile,TRACK_X).Succeeded());
	CHECK(GetExistingTraceRestrictProgram(TileXY(10,10),TRACK_X)->items[1].base()==config);
	TraceRestrictInstructionItem item(program->items[0].base());
	item.SetAuxField(to_underlying(AutoYieldRole::Main));
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,tile,TRACK_X,TRDCT_MODIFY_ITEM,0,item.base(),"").Succeeded());
	item.SetAuxField(to_underlying(AutoYieldRole::Approach));
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,tile,TRACK_X,TRDCT_MODIFY_ITEM,0,item.base(),"").Succeeded());
	CHECK(program->items[1].base()==AUTO_YIELD_DEFAULT_CONFIG);
	CHECK(CmdProgramSignalTraceRestrict({},tile,TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,(1001u<<16)|160,"").Failed());
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield speed cap - clamped equality applies to multi-main shared exits")
{
	IndependentApproach();Place(fast,2,16);Register(slow);Register(fast);
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|100);
	Place(slow,14,12);Execute(slow,10,12,TRPISP_ACQUIRE);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(slow,18,12).flags&TRPRF_WAIT_AT_PBS)==0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - mph and kmh input conversion does not leak into game state")
{
	const uint internal=160;
	const uint8_t unit=GENERATE(0,1,2,3);
	AutoRestoreBackup<uint8_t> units(_settings_game.locale.units_velocity,unit);
	AutoRestoreBackup new_units(_settings_newgame.locale.units_velocity,unit);
	const uint displayed=ConvertKmhishSpeedToDisplaySpeed(internal,VehicleType::Train);
	const uint round_trip=ConvertDisplaySpeedToKmhishSpeed(displayed,VehicleType::Train);
	CHECK(round_trip>0);CHECK(round_trip<=UINT16_MAX);
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|internal);
	Register(slow);Register(medium);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
	CHECK(GetExistingTraceRestrictProgram(TileXY(3,10),TRACK_X)->items[1].base()==(TEST_YIELD_CONFIG|160));
}


class AutomaticYieldPassengerFixture : public AutomaticYieldFixture {
protected:
	AutoRestoreBackup<CargoClasses> passenger_class{CargoSpec::Get(0)->classes,CargoClasses{CargoClass::Passengers}};
	AutoRestoreBackup<CargoClasses> freight_class{CargoSpec::Get(1)->classes,CargoClasses{CargoClass::Bulk}};
	void Load(Train *v, CargoType cargo=CargoType{0}) {
		v->cargo_type=cargo;v->cargo_cap=10;
		v->cargo.Append(CargoPacket::Create(uint16_t{1},uint16_t{0},StationID::Invalid(),TileXY(1,1),Money{0}));
	}
};

TEST_CASE_METHOD(AutomaticYieldPassengerFixture, "Automatic yield passenger priority - only equal effective speeds prefer loaded passenger trains")
{
	bool expected=true;
	SECTION("identical actual maximum speeds") { fast->vcache.cached_max_speed=100;Load(fast); }
	SECTION("identical clamped speeds") { Marker(3,10,AutoYieldRole::Approach,membership->index.base(),TEST_YIELD_CONFIG|100);Load(fast); }
	SECTION("slower passenger train cannot outrank freight") { fast->vcache.cached_max_speed=80;Load(fast);expected=false; }
	SECTION("empty passenger train gets no special priority") { fast->vcache.cached_max_speed=100;fast->cargo_type=CargoType{0};fast->cargo_cap=10;expected=false; }
	SECTION("loaded freight gets no passenger priority") { fast->vcache.cached_max_speed=100;Load(fast,CargoType{1});expected=false; }
	SECTION("both trains carry passengers") { fast->vcache.cached_max_speed=100;Load(fast);Load(slow);expected=false; }
	Register(slow);Register(fast);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding)==expected);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait)==expected);
	CHECK(((Execute(slow,10,10).flags&TRPRF_DENY)!=0)==expected);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPassengerFixture, "Automatic yield passenger priority - unloading is reflected in the next evaluation")
{
	fast->vcache.cached_max_speed=100;Load(fast);Register(slow);Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	fast->cargo.Truncate();
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	Load(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
}

TEST_CASE_METHOD(AutomaticYieldPassengerFixture, "Automatic yield passenger priority - payload on following parts counts")
{
	fast->vcache.cached_max_speed=100;
	Train *coach=MakeTrain();coach->SetWagon();coach->owner=fast->owner;fast->SetNext(coach);Load(coach);
	Register(slow);Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK_FALSE(EvaluateAutoYield(fast,membership->index.base(),AutoYieldCondition::MustWait));
}


TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield defaults - new detectors use five percent and rail speed while existing values survive")
{
	CHECK(AUTO_YIELD_DEFAULT_CONFIG==(5u<<16));
	TraceRestrictInstructionItem item{};item.SetType(TRIT_AUTO_YIELD_AREA);item.SetAuxField(to_underlying(AutoYieldRole::Approach));item.SetValue(membership->index);
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,TileXY(10,10),TRACK_X,TRDCT_INSERT_ITEM,0,item.base(),"").Succeeded());
	CHECK(GetExistingTraceRestrictProgram(TileXY(10,10),TRACK_X)->items[1].base()==AUTO_YIELD_DEFAULT_CONFIG);
	CHECK(GetExistingTraceRestrictProgram(TileXY(3,10),TRACK_X)->items[1].base()==TEST_YIELD_CONFIG);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield defaults - five percent threshold is applied after the cap")
{
	const uint cap=GENERATE(100,104,105,160);
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),AUTO_YIELD_DEFAULT_CONFIG|cap);
	Register(slow);Register(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding)==(cap>=105));
}


TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - the detector uses its own track on mixed rail type tiles")
{
	AutoRestoreBackup<uint16_t> primary_limit(const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_RAIL))->max_speed,0);
	AutoRestoreBackup<uint16_t> secondary_limit(const_cast<RailTypeInfo *>(GetRailTypeInfo(RAILTYPE_ELECTRIC))->max_speed,100);
	const TileIndex tile=TileXY(3,20);
	MakeRailNormal(tile,_current_company,TRACK_BIT_HORZ,RAILTYPE_RAIL);SetSecondaryRailType(tile,RAILTYPE_ELECTRIC);
	SB(_m[tile].m5,6,2,to_underlying(RailTileType::Signals));
	SetPresentSignals(tile,SignalAlongTrackdir(TrackToTrackdir(TRACK_LOWER)));SetSignalType(tile,TRACK_LOWER,SignalType::Path);
	auto *p=GetTraceRestrictProgram(MakeTraceRestrictRefId(tile,TRACK_LOWER),true);
	TraceRestrictInstructionItem item{};item.SetType(TRIT_AUTO_YIELD_AREA);item.SetAuxField(to_underlying(AutoYieldRole::Approach));item.SetValue(membership->index);
	p->items={item.AsProgramItem(),TraceRestrictProgramItem{AUTO_YIELD_DEFAULT_CONFIG}};
	REQUIRE(p->Validate().Succeeded());InvalidateAutoYieldAreas();UpdateAutoYieldAreas();
	Register(slow);Register(fast);
	CHECK(GetRailTypeByTrack(tile,TRACK_LOWER)==RAILTYPE_ELECTRIC);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Faster));
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
}


TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield speed cap - converting a single word action safely creates the default secondary word")
{
	const TileIndex tile=TileXY(10,10);
	auto *p=GetTraceRestrictProgram(MakeTraceRestrictRefId(tile,TRACK_X),false);
	TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);p->items={deny.AsProgramItem()};p->items.shrink_to_fit();REQUIRE(p->Validate().Succeeded());
	TraceRestrictInstructionItem detector{};detector.SetType(TRIT_AUTO_YIELD_AREA);detector.SetAuxField(to_underlying(AutoYieldRole::Approach));detector.SetValue(membership->index);
	GameRandomSeedChecker random_checker;
	REQUIRE(CmdProgramSignalTraceRestrict({},tile,TRACK_X,TRDCT_MODIFY_ITEM,0,detector.base(),"").Succeeded());
	CHECK(p->items.size()==1);
	REQUIRE(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,tile,TRACK_X,TRDCT_MODIFY_ITEM,0,detector.base(),"").Succeeded());
	REQUIRE(p->items.size()==2);
	CHECK(p->items[1].base()==AUTO_YIELD_DEFAULT_CONFIG);CHECK(p->Validate().Succeeded());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPassengerFixture, "Automatic yield passenger proximity - only nearby same speed followers cause a diversion")
{
	const uint gap = GENERATE(10u,17u,18u,40u);
	for(uint x=28;x<=110;x++) MakeRailNormal(TileXY(x,10),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Marker(27,10,AutoYieldRole::Approach);
	Marker(80,10,AutoYieldRole::Main);Marker(100,10,AutoYieldRole::Clear);
	fast->vcache.cached_max_speed=100;Load(fast);
	Place(slow,60,10);Place(fast,60-gap,10);Register(slow);Register(fast);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding)==(gap<=17));
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait)==(gap<=17));
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPassengerFixture, "Automatic yield passenger proximity - opposite directions never gain a proximity priority")
{
	fast->vcache.cached_max_speed=100;Load(fast);fast->direction=Direction::NE;
	Register(slow);Register(fast);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
}

TEST_CASE_METHOD(AutomaticYieldPassengerFixture, "Automatic yield passenger priority - a short unload does not flicker the passenger classification")
{
	fast->vcache.cached_max_speed=100;Load(fast);Register(slow);Register(fast);
	fast->current_order.MakeLoading(true);fast->cargo.Truncate();
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	fast->current_order.MakeGoToStation(StationID{123});
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - a short priority stop does not release a waiting train")
{
	SetupPlatform(true);Register(slow);Register(medium);
	Place(slow,14,8);Execute(slow,18,8,TRPISP_ACQUIRE);
	Place(medium,14,12);Execute(medium,18,12,TRPISP_ACQUIRE);
	medium->last_station_visited=platform_station->index;medium->current_order.MakeLoading(true);
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(slow,18,8).flags&TRPRF_WAIT_AT_PBS)!=0);
	CHECK_FALSE(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate));
	SECTION("bounded short stop") {
		medium->current_order_time=255;
		CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
		medium->current_order_time=256;
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	}
	SECTION("full load cannot hold the area forever") {
		medium->current_order.SetLoadType(OrderLoadType::FullLoad);
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	}
	SECTION("a manual stop releases priority") {
		medium->vehstatus.Set(VehState::Stopped);
		CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	}
	CHECK(random_checker.Check());CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield platform - turnover cannot bypass a short higher priority station stop")
{
	SetupPlatform(true);Register(slow);Register(medium);Register(fast);
	Place(slow,14,8);Execute(slow,18,8,TRPISP_ACQUIRE);
	Place(medium,14,12);Execute(medium,18,12,TRPISP_ACQUIRE);
	medium->last_station_visited=platform_station->index;medium->current_order.MakeLoading(true);
	fast->vcache.cached_max_speed=80;
	Train *follower=MakeActor(1,10,200);Register(follower);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::Evacuate));
	CHECK((Execute(slow,18,8).flags&TRPRF_WAIT_AT_PBS)!=0);
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield reservation - pending transactions preserve cancellation and capacity")
{
	TraceRestrictSlot *pending=Resource(AUTO_YIELD_PENDING_REF);REQUIRE(pending!=nullptr);
	TraceRestrictSlotTemporaryState temporary;temporary.PushToChangeStack();
	Execute(fast,3,10,TRPISP_ACQUIRE_TEMP_STATE);
	Execute(fast,10,12,TRPISP_ACQUIRE_TEMP_STATE);
	CHECK(membership->IsOccupant(fast->index));CHECK(pending->IsOccupant(fast->index));
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->IsOccupant(fast->index));
	temporary.PopFromChangeStackRevertTemporaryChanges(fast->index);
	CHECK(membership->occupants.empty());CHECK(pending->occupants.empty());
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->occupants.empty());
	CHECK_FALSE(fast->vehicle_flags.Test(VehicleFlag::HaveSlot));
	temporary.PushToChangeStack();Execute(fast,3,10,TRPISP_ACQUIRE_TEMP_STATE);
	temporary.PopFromChangeStackApplyTemporaryChanges(fast);
	CHECK(membership->IsOccupant(fast->index));CHECK(pending->IsOccupant(fast->index));
	SECTION("manual source cancellation removes pending and crossing does not reclaim") {
		membership->Vacate(fast);Execute(fast,3,10,TRPISP_RELEASE_FRONT);
		CHECK(membership->occupants.empty());CHECK(pending->occupants.empty());
	}
	SECTION("an unreserved cancelled path is reconciled") {
		AutoRestoreBackup ticks(_state_ticks,StateTicks{0});AutoYieldGameTick();
		CHECK(membership->occupants.empty());CHECK(pending->occupants.empty());
	}
	SECTION("turning the feature off releases pending claims") {
		_settings_game.vehicle.automatic_yield=false;AutoYieldSettingsChanged();
		CHECK(membership->occupants.empty());CHECK(pending->occupants.empty());
	}
	SECTION("a full source cannot create an orphan pending claim") {
		membership->max_occupancy=1;
		TraceRestrictSlotTemporaryState blocked;blocked.PushToChangeStack();
		CHECK((Execute(medium,3,10,TRPISP_ACQUIRE_TEMP_STATE).flags&TRPRF_WAIT_AT_PBS)!=0);
		CHECK_FALSE(membership->IsOccupant(medium->index));CHECK_FALSE(pending->IsOccupant(medium->index));
		blocked.PopFromChangeStackApplyTemporaryChanges(medium);
		CHECK_FALSE(medium->vehicle_flags.Test(VehicleFlag::HaveSlot));
	}
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield reservation - an owned path registers before entry and survives until physical confirmation")
{
	Place(slow,2,10);
	for(uint x=2;x<=4;x++)REQUIRE(TryReserveRailTrack(TileXY(x,10),TRACK_X));
	REQUIRE(GetTrainForReservation(TileXY(3,10),TRACK_X)==slow);
	Execute(slow,3,10,TRPISP_ACQUIRE);
	TraceRestrictSlot *pending=Resource(AUTO_YIELD_PENDING_REF);REQUIRE(pending!=nullptr);
	CHECK(membership->IsOccupant(slow->index));CHECK(pending->IsOccupant(slow->index));
	AutoRestoreBackup ticks(_state_ticks,StateTicks{0});AutoYieldGameTick();
	CHECK(membership->IsOccupant(slow->index));
	Execute(slow,3,10,TRPISP_RELEASE_FRONT);
	CHECK(pending->occupants.empty());
	for(uint x=2;x<=4;x++)UnreserveRailTrack(TileXY(x,10),TRACK_X);
	AutoYieldGameTick();CHECK(membership->IsOccupant(slow->index));
	Execute(slow,27,10,TRPISP_RELEASE_BACK);
	CHECK(membership->occupants.empty());CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield reservation - an awaited express keeps its pending entry during a temporary PBS gap")
{
	Place(fast,2,10);
	Execute(fast,3,10,TRPISP_PBS_RES_END_ACQUIRE);
	Register(slow);
	Place(slow,14,12);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	TraceRestrictSlot *targets=nullptr;
	for (auto *slot : TraceRestrictSlot::Iterate()) if (slot->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets) && slot->IsOccupant(fast->index)) targets=slot;
	REQUIRE(targets!=nullptr);
	REQUIRE(Resource(AUTO_YIELD_PENDING_REF)->IsOccupant(fast->index));
	bool retained=true;
	SECTION("a train waiting for a new PBS route remains a contender") {}
	SECTION("an express that reverses away releases its old entry") { fast->direction=Direction::NE; retained=false; }
	SECTION("manual source cancellation is authoritative") { membership->Vacate(fast); retained=false; }
	SECTION("a stopped express no longer keeps the slow train waiting") { fast->vehstatus.Set(VehState::Stopped); retained=false; }
	GameRandomSeedChecker random_checker;
	AutoRestoreBackup ticks(_state_ticks,StateTicks{0});AutoYieldGameTick();
	CHECK(membership->IsOccupant(fast->index)==retained);
	CHECK(Resource(AUTO_YIELD_PENDING_REF)->IsOccupant(fast->index)==retained);
	CHECK(targets->IsOccupant(fast->index)==retained);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield pre-fork refresh - protected reservations remain untouched")
{
	const uint last = GENERATE(9u,14u);
	Register(slow); Register(medium);
	slow->current_order.MakeGoToStation(StationID{0});
	for (uint x = 7; x <= last; x++) REQUIRE(TryReserveRailTrack(TileXY(x,10), TRACK_X));
	SECTION("the feature is disabled") { _settings_game.vehicle.automatic_yield = false; }
	SECTION("manual slot cancellation is authoritative") { membership->Vacate(slow); }
	SECTION("a manually stopped train keeps its route") { slow->vehstatus.Set(VehState::Stopped); }
	SECTION("a reversing train keeps its route") { slow->flags.Set(VehicleRailFlag::Reversing); }
	SECTION("a player forcing a signal retains control") { slow->force_proceed = TFP_SIGNAL; }
	SECTION("a committed siding claim is not reconsidered") { REQUIRE(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->Occupy(slow)); }
	SECTION("coupling work is not reconsidered") { slow->current_order.MakeWaitCouple(); }
	GameRandomSeedChecker random_checker;
	AutoRestoreBackup ticks(_state_ticks, StateTicks{(8 - (slow->index.base() & 7)) & 7});
	AutoYieldGameTick();
	for (uint x = 7; x <= last; x++) CHECK(GetReservedTrackbits(TileXY(x,10)) == TRACK_BIT_X);
	CHECK(GetReservedTrackbits(TileXY(10,12)) == TRACK_BIT_NONE);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield pre-fork refresh - a reservation beyond Main no longer commits the straight branch")
{
	Station *target=Station::Create(TileXY(40,10));target->owner=_current_company;
	target->facilities={StationFacility::Train};target->train_station=TileArea(target->xy,1,1);
	target->rect.BeforeAddTile(target->xy,StationRect::ADD_FORCE);
	MakeRailStation(target->xy,_current_company,target->index,Axis::X,0,RAILTYPE_RAIL);
	slow->current_order.MakeGoToStation(target->index);slow->dest_tile=target->xy;
	Register(slow);Register(medium);
	for(uint x=7;x<=14;x++)REQUIRE(TryReserveRailTrack(TileXY(x,10),TRACK_X));
	REQUIRE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	REQUIRE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	REQUIRE(TrainReservationPassesThroughTile(slow,TileXY(10,10),TRACK_X));
	GameRandomSeedChecker random_checker;
	AutoRestoreBackup ticks(_state_ticks,StateTicks{(8-(slow->index.base()&7))&7});
	AutoYieldGameTick();
	CHECK_FALSE(TrainReservationPassesThroughTile(slow,TileXY(10,10),TRACK_X));
	/* This minimal actor may fail the normal PBS safe-end check. Either a
	 * committed siding or its normal failed-reservation stop is safe; it must
	 * never retain the now-invalid straight reservation past the fork. */
	const bool siding_reserved=TrainReservationPassesThroughTile(slow,TileXY(10,12),TRACK_X);
	const bool claimed=Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->IsOccupant(slow->index);
	CHECK(siding_reserved==claimed);
	CHECK((siding_reserved || slow->flags.Test(VehicleRailFlag::Stuck)));
	CHECK(slow->tile==TileXY(7,10));
	CHECK(slow->current_order.GetDestination()==target->index);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield forward proofs - depot turnarounds are not a siding still ahead")
{
	MakeRailDepot(TileXY(1,10),_current_company,DepotID{0},DiagDirection::SW,RAILTYPE_RAIL);
	MakeRailDepot(TileXY(40,10),_current_company,DepotID{1},DiagDirection::NE,RAILTYPE_RAIL);
	for(uint x=20;x<=21;x++)MakeRailNormal(TileXY(x,20),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Place(fast,20,20);Place(medium,21,20);Place(slow,25,10);
	slow->current_order.MakeGoToDepot(DepotID{1},{});slow->dest_tile=TileXY(40,10);
	Register(slow);
	GameRandomSeedChecker random_checker;
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield depot - departing reservations register and temporary failures roll back")
{
	slow->track = TRACK_BIT_DEPOT;
	bool allowed = true;
	SECTION("a moving depot departure can register") { }
	SECTION("a parked train does not register") { slow->vehstatus.Set(VehState::Stopped); allowed = false; }
	SECTION("a crashed train does not register") { slow->vehstatus.Set(VehState::Crashed); allowed = false; }
	GameRandomSeedChecker random_checker;
	TraceRestrictSlot *pending = Resource(AUTO_YIELD_PENDING_REF); REQUIRE(pending != nullptr);
	TraceRestrictSlotTemporaryState temporary; temporary.PushToChangeStack();
	Execute(slow, 3, 10, TRPISP_ACQUIRE_TEMP_STATE);
	CHECK(membership->IsOccupant(slow->index) == allowed);
	temporary.PopFromChangeStackRevertTemporaryChanges(slow->index);
	CHECK(membership->occupants.empty()); CHECK(pending->occupants.empty());
	Execute(slow, 3, 10, TRPISP_PBS_RES_END_ACQUIRE);
	CHECK(membership->IsOccupant(slow->index) == allowed);
	CHECK(pending->IsOccupant(slow->index) == allowed);
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::MustWait));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield depot - reconciliation preserves only a live reserved departure")
{
	Place(slow, 1, 10); slow->track = TRACK_BIT_DEPOT;
	Place(fast, 30, 20); Place(medium, 31, 20);
	MakeRailDepot(TileXY(1, 10), _current_company, DepotID{0}, DiagDirection::SW, RAILTYPE_RAIL);
	for (uint x = 1; x <= 4; x++) REQUIRE(TryReserveRailTrack(TileXY(x, 10), TRACK_X));
	REQUIRE(TrainReservationPassesThroughTile(slow, TileXY(3, 10), TRACK_X));
	CHECK_FALSE(TrainReservationPassesThroughTile(slow, TileXY(3, 10), TRACK_Y));
	CHECK(TrainReservationPassesThroughTile(slow, TileXY(3, 10)));
	CHECK(GetTrainForReservation(TileXY(3, 10), TRACK_X) == nullptr);
	Execute(slow, 3, 10, TRPISP_PBS_RES_END_ACQUIRE);
	TraceRestrictSlot *pending = Resource(AUTO_YIELD_PENDING_REF); REQUIRE(pending != nullptr);
	REQUIRE(membership->IsOccupant(slow->index)); REQUIRE(pending->IsOccupant(slow->index));
	bool retained = true;
	SECTION("a valid departure survives the cleanup tick") { }
	SECTION("a cancelled path releases its pending membership") { UnreserveRailTrack(TileXY(3, 10), TRACK_X); retained = false; }
	SECTION("a stopped depot train releases its pending membership") { slow->vehstatus.Set(VehState::Stopped); retained = false; }
	GameRandomSeedChecker random_checker;
	AutoRestoreBackup ticks(_state_ticks, StateTicks{0}); AutoYieldGameTick();
	CHECK(membership->IsOccupant(slow->index) == retained);
	CHECK(pending->IsOccupant(slow->index) == retained);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield reservation - private automatic slots accept foreign trains without changing ordinary permissions")
{
	AutoRestoreBackup sharing(_settings_game.economy.infrastructure_sharing[0],true);
	slow->owner=CompanyID{1};fast->owner=CompanyID{2};
	REQUIRE_FALSE(membership->flags.Test(TraceRestrictSlot::Flag::Public));
	CHECK_FALSE(membership->IsUsableByOwner(slow->owner));
	Register(slow);Register(fast);
	CHECK(membership->IsOccupant(slow->index));CHECK(membership->IsOccupant(fast->index));
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	Execute(slow,10,12,TRPISP_ACQUIRE);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->IsOccupant(slow->index));
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	Execute(slow,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->occupants.empty());
	CHECK_FALSE(membership->flags.Test(TraceRestrictSlot::Flag::Public));
	CHECK_FALSE(membership->IsUsableByOwner(fast->owner));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield reservation - downstream edits release pending without evicting entered members")
{
	Execute(fast,3,10,TRPISP_PBS_RES_END_ACQUIRE);Register(slow);
	CHECK(Resource(AUTO_YIELD_PENDING_REF)->IsOccupant(fast->index));
	GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X),false)->items.clear();
	InvalidateAutoYieldAreas();UpdateAutoYieldAreas();
	CHECK_FALSE(membership->IsOccupant(fast->index));CHECK(membership->IsOccupant(slow->index));
	CHECK(Resource(AUTO_YIELD_PENDING_REF)==nullptr);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldPassengerFixture, "Automatic yield parallel exit - a nearby ready passenger siding precedes equal speed main freight")
{
	UseExitOnly();fast->vcache.cached_max_speed=100;Load(fast);
	Register(slow);Register(fast);Place(slow,9,10);Place(fast,17,12);
	Execute(fast,18,12,TRPISP_PBS_RES_END_ACQUIRE);
	const bool blocked=GENERATE(false,true);
	if(blocked){
		auto*p=Marker(23,10,AutoYieldRole::Approach);
		TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);p->items={deny.AsProgramItem()};
		REQUIRE(p->Validate().Succeeded());InvalidateAutoYieldAreas();UpdateAutoYieldAreas();
	}
	GameRandomSeedChecker random_checker;
	const auto main=Execute(slow,10,10);
	CHECK(((main.flags&TRPRF_WAIT_AT_PBS)!=0)==!blocked);
	CHECK(((main.flags&TRPRF_PBS_RES_END_WAIT)!=0)==!blocked);
	CHECK((main.flags&TRPRF_DENY)==0);
	CHECK((Execute(fast,18,12).flags&TRPRF_WAIT_AT_PBS)==0);
	CHECK(random_checker.Check());CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldPassengerFixture, "Automatic yield parallel exit - a waiting passenger cannot block diversion to a free siding")
{
 UseExitOnly();fast->vcache.cached_max_speed=100;Load(fast);
 UpperSiding(true);
 Register(slow);Register(fast);Register(medium);
 Place(slow,7,10);Place(fast,17,12);Place(medium,5,10);
 Execute(fast,18,12,TRPISP_PBS_RES_END_ACQUIRE);
 REQUIRE(EvaluateAutoYield(fast,membership->index.base(),AutoYieldCondition::MustWait));
 REQUIRE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
 GameRandomSeedChecker random_checker;
 /* Deny Main to select the empty siding. A plain wait here would commit
  * the freight to Main and hold the faster follower its passenger awaits. */
 CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
 CHECK((Execute(fast,18,12).flags&TRPRF_WAIT_AT_PBS)!=0);
 CHECK(random_checker.Check());CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield candidate Main - reject a slow detour onto a fast feeder before merging")
{
 UseExitOnly();ParallelBranch();
 for(uint x=1;x<8;x++)MakeRailNormal(TileXY(x,8),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
 MakeRailNormal(TileXY(8,8),_current_company,TRACK_BIT_X|TRACK_BIT_LOWER,RAILTYPE_RAIL);
 Place(slow,7,10);Place(fast,2,8);Place(medium,35,10);
 Register(slow);Register(fast);
 /* Ambiguous exits prevent the older unique-common-clearance heuristic.
  * The candidate Main itself still proves the attempted shared approach. */
 Marker(29,10,AutoYieldRole::Clear);
 bool full=false;
 SECTION("a free siding allows diversion before the merge") {}
 SECTION("a full siding cannot create an unreproducible turnover wait") {Place(medium,14,12);medium->vehstatus.Set(VehState::Stopped);full=true;}
 GameRandomSeedChecker random_checker;
 const auto main=Execute(slow,10,8);
 CHECK(((main.flags&TRPRF_DENY)!=0)==!full);
 if(full)CHECK((main.flags&TRPRF_PBS_RES_END_WAIT)==0);
 else CHECK((Execute(slow,18,12).flags&TRPRF_DENY)==0);
 CHECK(random_checker.Check());CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldMultipleMainFixture, "Automatic yield destination merge - a second main cannot hide a shared forward destination outlet")
{
 UseExitOnly();IndependentApproach();
 MakeRailNormal(TileXY(14,16),_current_company,TRACK_BIT_X|TRACK_BIT_RIGHT,RAILTYPE_RAIL);
 MakeRailNormal(TileXY(14,17),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
 MakeRailNormal(TileXY(14,18),_current_company,TRACK_BIT_LEFT,RAILTYPE_RAIL);
 for(uint x=15;x<20;x++)MakeRailNormal(TileXY(x,18),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
 MakeRailDepot(TileXY(20,18),_current_company,DepotID{1},DiagDirection::NE,RAILTYPE_RAIL);
 MakeRailDepot(TileXY(40,10),_current_company,DepotID{0},DiagDirection::NE,RAILTYPE_RAIL);
 slow->current_order.MakeGoToDepot(DepotID{0},{});fast->current_order.MakeGoToDepot(DepotID{0},{});
 Place(fast,2,16);Register(slow);Register(fast);
 bool expected=true,full=false;
 SECTION("the active forward destinations share a directed clearance") {}
 SECTION("nearest depot is not an explicit target") {fast->current_order.MakeGoToDepot(DepotID{0},{},ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS,ODATFB_NEAREST_DEPOT);expected=false;}
 SECTION("another destination does not establish the same outlet") {fast->current_order.MakeGoToDepot(DepotID{1},{});expected=false;}
 SECTION("different directed clearances do not establish a common outlet") {Marker(18,18,AutoYieldRole::Clear);fast->current_order.MakeGoToDepot(DepotID{1},{});expected=false;}
 SECTION("ordinary restrictions still block the destination forecast") {
  auto*p=Marker(18,16,AutoYieldRole::Approach,INVALID_TRACE_RESTRICT_SLOT_ID.base());TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);p->items={deny.AsProgramItem()};REQUIRE(p->Validate().Succeeded());InvalidateAutoYieldAreas();UpdateAutoYieldAreas();expected=false;
 }
 SECTION("opposing traffic has no forward destination forecast") {fast->direction=Direction::NE;expected=false;}
 SECTION("manual cancellation still opts out") {membership->Vacate(fast);expected=false;}
 SECTION("equal speed trains gain no new priority") {fast->vcache.cached_max_speed=100;expected=false;}
 SECTION("a disconnected merge cannot create a forecast") {MakeRailNormal(TileXY(24,12),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);expected=false;}
 SECTION("multiple forward outlets to the same target remain ambiguous") {UpperSiding(true);Marker(19,12,AutoYieldRole::Clear);expected=false;}
 SECTION("a siding ending at another clearance cannot justify the diversion") {Marker(19,12,AutoYieldRole::Clear);expected=false;}
 SECTION("a short siding cannot be forced to hold a long train") {slow->gcache.cached_total_length=20*TILE_SIZE;expected=false;}
 SECTION("a full siding cannot be forced open") {Place(medium,14,12);medium->vehstatus.Set(VehState::Stopped);full=true;expected=false;}
 GameRandomSeedChecker random_checker;
 CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding)==expected);
 const auto main=Execute(slow,10,10);
 CHECK(((main.flags&TRPRF_DENY)!=0)==(expected&&!full));
 if(full)CHECK((main.flags&TRPRF_PBS_RES_END_WAIT)==0);
 CHECK(random_checker.Check());CHECK(TraceRestrictSlot::ValidateVehicleIndex());
 if(expected&&!full){
  fast->current_order.MakeGoToDepot(DepotID{1},{});
  CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
  fast->current_order.MakeGoToDepot(DepotID{0},{});
  CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
 }
}


TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield registration - simulated bridge detectors retain timing and rollback")
{
	const auto mode = GENERATE(AutoYieldRegistration::Reservation, AutoYieldRegistration::Front, AutoYieldRegistration::Tail);
	const bool entrance = GENERATE(false, true);
	AutoRestoreBackup braking(_settings_game.vehicle.train_braking_model, uint8_t(GENERATE(TBM_ORIGINAL, TBM_REALISTIC)));
	Marker(3,10,AutoYieldRole::Approach,membership->index.base(),AUTO_YIELD_DEFAULT_CONFIG | (uint32_t(to_underlying(mode)) << 26));
	MakeRailBridgeRamp(TileXY(3,10), _current_company, 0, entrance ? DiagDirection::SW : DiagDirection::NE, RAILTYPE_RAIL, false);
	if (entrance) SetTunnelBridgeSignalSimulationEntrance(TileXY(3,10));
	else SetTunnelBridgeSignalSimulationExit(TileXY(3,10));
	MakeRailBridgeRamp(TileXY(entrance ? 5 : 1,10), _current_company, 0, entrance ? DiagDirection::NE : DiagDirection::SW, RAILTYPE_RAIL, false);
	SetBridgeMiddle(TileXY(entrance ? 4 : 2,10), Axis::X);
	if (entrance) SetTunnelBridgeSignalSimulationExit(TileXY(5,10));
	else SetTunnelBridgeSignalSimulationEntrance(TileXY(1,10));
	SetTunnelBridgeRestrictedSignal(TileXY(3,10), true);
	InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	REQUIRE(Resource(AUTO_YIELD_PENDING_REF) != nullptr);
	GameRandomSeedChecker random_checker;
	{
		TraceRestrictProgramInput input(TileXY(3,10), TRACKDIR_X_NE, nullptr, nullptr);
		input.permitted_slot_operations = TRPISP_RELEASE_FRONT | TRPISP_RELEASE_BACK;
		TraceRestrictProgramResult out;
		GetExistingTraceRestrictProgram(input.tile,TRACK_X)->Execute(slow,input,out);
		CHECK_FALSE(membership->IsOccupant(slow->index));
	}
	{
		TraceRestrictSlotTemporaryState temporary; temporary.PushToChangeStack();
		Execute(slow,3,10,TRPISP_ACQUIRE_TEMP_STATE);
		CHECK(membership->IsOccupant(slow->index) == (mode == AutoYieldRegistration::Reservation));
		temporary.PopFromChangeStackRevertTemporaryChanges(slow->index);
		CHECK_FALSE(membership->IsOccupant(slow->index));
	}
	Execute(slow,3,10,TRPISP_PBS_RES_END_ACQUIRE);
	CHECK(membership->IsOccupant(slow->index) == (mode == AutoYieldRegistration::Reservation));
	Execute(slow,3,10,TRPISP_RELEASE_FRONT);
	CHECK(membership->IsOccupant(slow->index) == (mode != AutoYieldRegistration::Tail));
	Execute(slow,3,10,TRPISP_RELEASE_BACK);
	REQUIRE(membership->IsOccupant(slow->index));
	CHECK_FALSE(Resource(AUTO_YIELD_PENDING_REF)->IsOccupant(slow->index));
	Register(medium);
	Execute(medium,3,10,TRPISP_RELEASE_BACK);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	Execute(slow,27,10,TRPISP_RELEASE_BACK);
	CHECK_FALSE(membership->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
	if (mode == AutoYieldRegistration::Reservation) {
		membership->Vacate(slow);
		Execute(slow,3,10,TRPISP_RELEASE_FRONT);
		CHECK(membership->IsOccupant(slow->index) == (_settings_game.vehicle.train_braking_model != TBM_REALISTIC));
		membership->Vacate(slow);
	}
	CHECK(random_checker.Check());
}


TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield independent exits - a forbidden depot return cannot block a separate departure")
{
	for (uint x = 1; x <= 40; x++) MakeRailNormal(TileXY(x,20),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Marker(3,20,AutoYieldRole::Approach); Marker(10,20,AutoYieldRole::Main); Marker(27,20,AutoYieldRole::Clear);
	MakeRailNormal(TileXY(15,20),_current_company,TRACK_BIT_X | TRACK_BIT_RIGHT,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(15,21),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailDepot(TileXY(15,22),_current_company,DepotID{1},DiagDirection::NW,RAILTYPE_RAIL);
	const TileIndex gate = TileXY(15,21);
	SB(_m[gate].m5,6,2,to_underlying(RailTileType::Signals));
	SetPresentSignals(gate,SignalAlongTrackdir(TRACKDIR_Y_SE));
	SetSignalType(gate,TRACK_Y,SignalType::PathOneWay);
	const bool station_target = GENERATE(false,true);
	if (station_target) {
		Station *target = Station::Create(TileXY(40,20)); target->owner = _current_company;
		target->facilities = {StationFacility::Train}; target->train_station = TileArea(target->xy,1,1);
		target->rect.BeforeAddTile(target->xy,StationRect::ADD_FORCE);
		MakeRailStation(target->xy,_current_company,target->index,Axis::X,0,RAILTYPE_RAIL);
		slow->current_order.MakeGoToStation(target->index);
	} else {
		MakeRailDepot(TileXY(40,20),_current_company,DepotID{0},DiagDirection::NE,RAILTYPE_RAIL);
		slow->current_order.MakeGoToDepot(DepotID{0},{});
	}
	Place(slow,7,20); Place(fast,9,10); Register(slow); Register(fast);
	[[maybe_unused]] bool independent = true;
	bool confirmed_conflict = false;
	SECTION("one-way return blocked: different exits can move together") { }
	SECTION("a passable reverse signal retains ambiguity") { SetSignalType(gate,TRACK_Y,SignalType::Path); independent = false; }
	SECTION("a missing return signal retains ambiguity") { MakeRailNormal(gate,_current_company,TRACK_BIT_Y,RAILTYPE_RAIL); independent = false; }
	SECTION("changing the target to this depot invalidates independence") { slow->current_order.MakeGoToDepot(DepotID{1},{}); independent = false; }
	SECTION("the passing train still has a conflicting tail") {
		confirmed_conflict = true;
		Train *tail = MakeActor(12,20,200); tail->SetMultiheaded(); fast->SetNext(tail); independent = false;
	}
	SECTION("a route merging before the clearance remains protected") {
		MakeRailNormal(TileXY(20,10),_current_company,TRACK_BIT_X | TRACK_BIT_RIGHT,RAILTYPE_RAIL);
		for (uint y = 11; y < 20; y++) MakeRailNormal(TileXY(20,y),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20,20),_current_company,TRACK_BIT_X | TRACK_BIT_LEFT,RAILTYPE_RAIL);
		independent = false;
	}
	GameRandomSeedChecker random_checker;
	const auto order = slow->current_order;
	const auto flags = slow->flags;
	const auto members = membership->occupants;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait) == confirmed_conflict);
	CHECK(slow->current_order.GetType() == order.GetType());
	CHECK(slow->current_order.GetDestination() == order.GetDestination());
	CHECK(slow->flags == flags); CHECK(membership->occupants == members);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}


TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield station access - a free platform does not require a free departure to avoid eviction")
{
	const bool exit_only = GENERATE(false,true);
	SetupPlatform(exit_only);
	for (uint x = 11; x <= 15; x++) MakeRailStation(TileXY(x,8),_current_company,platform_station->index,Axis::X,0,RAILTYPE_RAIL);
	platform_station->rect.BeforeAddTile(TileXY(11,8),StationRect::ADD_FORCE);
	platform_station->train_station = TileArea(TileXY(11,8),5,5);
	Marker(18,8,AutoYieldRole::Exit,membership->index.base(),TEST_YIELD_CONFIG | AUTO_YIELD_PLATFORM);
	slow->current_order.MakeGoToStation(platform_station->index);
	medium->current_order.MakeGoToDepot(DepotID{0},{});
	Place(medium,14,8); Register(slow); Register(medium); Register(fast);
	Execute(medium,exit_only ? 18 : 10,8,TRPISP_PBS_RES_END_ACQUIRE);
	for (uint x = 14; x < 20; x++) REQUIRE(TryReserveRailTrack(TileXY(x,8),TRACK_X));
	REQUIRE(TryReserveRailTrack(TileXY(20,8),TRACK_RIGHT));
	REQUIRE(TryReserveRailTrack(TileXY(20,9),TRACK_Y));
	REQUIRE(TryReserveRailTrack(TileXY(20,10),TRACK_LEFT));
	for (uint x = 21; x <= 27; x++) REQUIRE(TryReserveRailTrack(TileXY(x,10),TRACK_X));
	REQUIRE(GetTrainForReservation(TileXY(24,10),TRACK_X) == medium);
	bool must_evict = false;
	SECTION("safe free platform remains usable while its departure is reserved") { }
	SECTION("a nonmember physically occupies the otherwise free platform") {
		Train *blocker = MakeActor(14,12,80); blocker->vehstatus.Set(VehState::Stopped); must_evict = true;
	}
	SECTION("an unattributed reservation still occupies the platform") {
		SetRailStationReservation(TileXY(14,12),true); must_evict = true;
	}
	SECTION("a committed platform claim is not an empty place") {
		REQUIRE(Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,12),TRACK_X))->Occupy(fast)); must_evict = true;
	}
	SECTION("a permanently disconnected platform exit cannot count as available") {
		MakeRailNormal(TileXY(20,12),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL); must_evict = true;
	}
	SECTION("a forbidden onward route cannot masquerade as usable parking") {
		auto *program = Marker(19,12,AutoYieldRole::Approach,INVALID_TRACE_RESTRICT_SLOT_ID.base());
		TraceRestrictInstructionItem deny{}; deny.SetType(TRIT_PF_DENY);
		program->items = {deny.AsProgramItem()}; REQUIRE(program->Validate().Succeeded());
		InvalidateAutoYieldAreas(); UpdateAutoYieldAreas(); must_evict = true;
	}
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate) == must_evict);
	if (!must_evict) {
		const auto main = Execute(slow,10,10);
		/* Rejection of Main is a diversion, not a demand to evict the other
		 * platform: a free parking route is legal with a busy shared exit. */
		CHECK(((main.flags & (TRPRF_WAIT_AT_PBS | TRPRF_PBS_RES_END_WAIT)) == 0 || (main.flags & TRPRF_DENY) != 0));
		CHECK((Execute(slow,exit_only ? 18 : 10,12).flags & TRPRF_DENY) == 0);
		CHECK((Execute(medium,18,8).flags & TRPRF_WAIT_AT_PBS) != 0);
	}
	CHECK(GetTrainForReservation(TileXY(24,10),TRACK_X) == medium);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,8),TRACK_X))->IsOccupant(medium->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}


TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield compound inlets - bypass curves do not erase exclusive parking")
{
	UseExitOnly();
	MakeRailNormal(TileXY(14,10),_current_company,TRACK_BIT_X | TRACK_BIT_RIGHT,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(14,11),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(14,12),_current_company,TRACK_BIT_X | TRACK_BIT_UPPER | TRACK_BIT_LEFT,RAILTYPE_RAIL);
	Register(slow); Register(fast);
	bool free = true;
	SECTION("an empty exclusive segment beyond the compound merge is usable") { }
	SECTION("physical nonmember occupancy is still counted") { Place(medium,16,12); free = false; }
	SECTION("a reversing foreign train still occupies the lane") { Place(medium,16,12); medium->direction = Direction::NE; medium->owner = CompanyID{1}; free = false; }
	SECTION("an unattributed reservation is still counted") { REQUIRE(TryReserveRailTrack(TileXY(16,12),TRACK_X)); free = false; }
	SECTION("the excluded junction cannot supply missing parking length") { slow->gcache.cached_total_length = 48; free = false; }
	SECTION("another train's lane claim remains authoritative") {
		REQUIRE(Resource(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X))->Occupy(medium)); free = false;
	}
	SECTION("an excluded crossing cannot supply missing parking length") {
		MakeRailNormal(TileXY(14,12),_current_company,TRACK_BIT_X | TRACK_BIT_Y,RAILTYPE_RAIL); slow->gcache.cached_total_length = 48; free = false;
	}
	SECTION("an occupied excluded junction still blocks admission") { Place(medium,14,12); medium->track = TRACK_BIT_LEFT; free = false; }
	SECTION("a crossing junction reservation still blocks admission") { REQUIRE(TryReserveRailTrack(TileXY(14,12),TRACK_LEFT)); free = false; }
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding) == free);
	CHECK(((Execute(slow,10,10).flags & TRPRF_DENY) != 0) == free);
	CHECK(membership->IsOccupant(slow->index));
	CHECK_FALSE(Resource(MakeTraceRestrictRefId(TileXY(18,12),TRACK_X))->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield parking segments - all compound boundaries use exclusive space")
{
	const bool exit_only = GENERATE(false, true);
	const uint extra = GENERATE(Catch::Generators::range(1u,32u));
	if (exit_only) UseExitOnly();
	const TrackBits tracks = static_cast<TrackBits>(TRACK_BIT_X | (extra << 1));
	MakeRailNormal(TileXY(14,12),_current_company,tracks,RAILTYPE_RAIL);
	Register(slow); Register(fast);
	bool free = true;
	SECTION("an empty segment after any junction or crossing is usable") { }
	SECTION("the boundary cannot lengthen a short segment") { slow->gcache.cached_total_length=48; free=false; }
	SECTION("physical occupancy on the boundary still prevents admission") { Place(medium,14,12); free=false; }
	SECTION("another reservation on the boundary still prevents admission") { REQUIRE(TryReserveRailTrack(TileXY(14,12),TRACK_X)); free=false; }
	SECTION("a main marker at the boundary cannot become parking") { Marker(14,12,AutoYieldRole::Main); SetTrackBits(TileXY(14,12),tracks); free=false; }
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding)==free);
	CHECK(((Execute(slow,10,10).flags & TRPRF_DENY)!=0)==free);
	CHECK(Resource(MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10,12),TRACK_X))->occupants.empty());
	CHECK(membership->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield parking segments - bends use actual full and half tile lengths")
{
	const bool exit_only = GENERATE(false,true);
	if (exit_only) UseExitOnly();
	MakeRailNormal(TileXY(12,12),_current_company,TRACK_BIT_RIGHT,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(12,13),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(12,14),_current_company,TRACK_BIT_LEFT,RAILTYPE_RAIL);
	for (uint x=13;x<16;x++) MakeRailNormal(TileXY(x,14),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(16,14),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(16,13),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(16,12),_current_company,TRACK_BIT_LOWER,RAILTYPE_RAIL);
	Register(slow); Register(fast);
	const uint space = exit_only ? 200 : 160;
	bool free=true;
	SECTION("a train and its margin fit exactly") { slow->gcache.cached_total_length=space-TILE_SIZE; }
	SECTION("one pixel too long does not fit") { slow->gcache.cached_total_length=space-TILE_SIZE+1; free=false; }
	SECTION("a foreign train on a bend still blocks entry") { Place(medium,12,12);medium->track=TRACK_BIT_RIGHT;free=false; }
	SECTION("a bend reservation still blocks entry") { REQUIRE(TryReserveRailTrack(TileXY(12,12),TRACK_RIGHT));free=false; }
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding)==free);
	CHECK(((Execute(slow,10,10).flags & TRPRF_DENY)!=0)==free);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}


TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield lifecycle - missed clearance and reversed entry release only after the entire body leaves")
{
	Register(slow);
	const bool reversed = GENERATE(false, true);
	const bool tail_inside = GENERATE(false, true);
	Place(slow, reversed ? 1 : 30, 10);
	if (reversed) slow->direction = Direction::NE;
	if (tail_inside) {
		Train *tail = MakeActor(reversed ? 4 : 26, 10, 100);
		tail->SetWagon(); tail->direction = slow->direction; slow->SetNext(tail);
	}
	AutoRestoreBackup tick(_state_ticks, StateTicks{64});
	AutoYieldGameTick();
	CHECK(membership->IsOccupant(slow->index) == tail_inside);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldWideFixture, "Automatic yield conflict - a remote express on the same outlet does not hold a ready departure indefinitely")
{
	for (uint x=41; x<200; x++) MakeRailNormal(TileXY(x,10),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(27,10),TRACK_X),false)->items.clear();
	Marker(190,10,AutoYieldRole::Clear);
	Place(slow,180,10); Place(fast,30,10); Register(slow); Register(fast);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	Place(fast,177,10);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
}

class AutomaticYieldTransformFixture : public AutomaticYieldFixture {
protected:
	uint transform = 0;
	TileIndex Mapped(uint x, uint y) const
	{
		if (transform & 1) x = 63-x;
		if (transform & 2) y = 63-y;
		if (transform & 4) std::swap(x,y);
		return TileXY(x,y);
	}
	DiagDirection MapDir(DiagDirection dir) const
	{
		int x=0,y=0;
		switch(dir) { case DiagDirection::NE:x=-1;break;case DiagDirection::SW:x=1;break;case DiagDirection::SE:y=1;break;default:y=-1;break; }
		if(transform&1)x=-x;if(transform&2)y=-y;if(transform&4)std::swap(x,y);
		return x<0?DiagDirection::NE:x>0?DiagDirection::SW:y>0?DiagDirection::SE:DiagDirection::NW;
	}
	Trackdir MapTrackdir(Trackdir td) const
	{
		for (Trackdir out=TRACKDIR_BEGIN;out<TRACKDIR_END;out++) if(IsValidTrackdir(out) &&
			TrackdirToExitdir(out)==MapDir(TrackdirToExitdir(td)) &&
			TrackdirToExitdir(ReverseTrackdir(out))==MapDir(TrackdirToExitdir(ReverseTrackdir(td)))) return out;
		return INVALID_TRACKDIR;
	}
	void Transform(uint value)
	{
		transform=value;
		struct Rail { uint x,y; TrackBits bits; decltype(TraceRestrictProgram::items) program; };
		std::vector<Rail> rails;
		for(uint y=1;y<63;y++)for(uint x=1;x<63;x++) {
			const auto tile=TileXY(x,y);if(!IsPlainRailTile(tile))continue;
			Rail item{x,y,GetTrackBits(tile),{}};
			if(auto *p=GetExistingTraceRestrictProgram(tile,TRACK_X))item.program=p->items;
			rails.push_back(item);
		}
		ClearTraceRestrictMapping();
		for(const auto &rail:rails)MakeClear(TileXY(rail.x,rail.y),ClearGround::Grass,0);
		for(const auto &rail:rails) {
			TrackBits bits=TRACK_BIT_NONE;
			for(Track tr=TRACK_X;tr<TRACK_END;tr++)if(rail.bits&TrackToTrackBits(tr))bits|=TrackToTrackBits(TrackdirToTrack(MapTrackdir(TrackToTrackdir(tr))));
			const auto tile=Mapped(rail.x,rail.y);MakeRailNormal(tile,_current_company,bits,RAILTYPE_RAIL);
			if(rail.program.empty())continue;
			const auto td=MapTrackdir(TRACKDIR_X_SW);const auto track=TrackdirToTrack(td);
			SB(_m[tile].m5,6,2,to_underlying(RailTileType::Signals));SetPresentSignals(tile,SignalAlongTrackdir(td));SetSignalType(tile,track,SignalType::Path);
			auto*p=GetTraceRestrictProgram(MakeTraceRestrictRefId(tile,track),true);p->items=rail.program;REQUIRE(p->Validate().Succeeded());
		}
		for(Train*v:{slow,medium,fast}) {
			const auto tile=Mapped(TileX(v->tile),TileY(v->tile));
			const auto td=MapTrackdir(TRACKDIR_X_SW);v->track=TrackToTrackBits(TrackdirToTrack(td));v->direction=DiagDirToDir(TrackdirToExitdir(td));
			Place(v,TileX(tile),TileY(tile));
		}
		InvalidateAutoYieldAreas();UpdateAutoYieldAreas();
	}
};

TEST_CASE_METHOD(AutomaticYieldTransformFixture, "Automatic yield generated topology - mirrored and rotated parking preserves safety and deterministic decisions")
{
	const uint seed=GENERATE(range(1u,33u));
	const uint orientation=GENERATE(range(0u,8u));
	const bool exit_only=GENERATE(false,true);
	const bool second_lane=(seed&4)!=0;
	if(second_lane)UpperSiding(exit_only);
	if(seed&8){
		for(uint x=1;x<=40;x++)MakeRailNormal(TileXY(x,24),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		Marker(3,24,AutoYieldRole::Approach);Marker(10,24,AutoYieldRole::Main);Marker(27,24,AutoYieldRole::Clear);
	}
	slow->vcache.cached_max_speed=80+(seed%5)*20;fast->vcache.cached_max_speed=slow->vcache.cached_max_speed+100;
	slow->gcache.cached_total_length=8+(seed%4)*16;
	if(exit_only)UseExitOnly();
	Transform(orientation);
	// Local reproducible generation never consumes the simulation RNG.
	const uint scenario=(seed*1664525u+1013904223u)%4;
	CAPTURE(seed,orientation,scenario,exit_only);
	GameRandomSeedChecker random_checker;
	const auto blocked=Mapped(14,12);
	if(scenario==1)slow->gcache.cached_total_length=1024;
	if(scenario==2){Place(medium,TileX(blocked),TileY(blocked));medium->vehstatus.Set(VehState::Stopped);}
	if(scenario==3)REQUIRE(TryReserveRailTrack(blocked,TrackdirToTrack(MapTrackdir(TRACKDIR_X_SW))));
	membership->Occupy(slow);membership->Occupy(fast);
	const auto before=membership->occupants;
	for(uint repeat=0;repeat<3;repeat++) {
		CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding)==(scenario!=1 && (scenario==0 || second_lane)));
		CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	}
	CHECK(membership->occupants==before);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield topology - incomparable clearance distances cannot create a merging threat")
{
	for(uint x=1;x<=40;x++)MakeRailNormal(TileXY(x,20),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Marker(3,20,AutoYieldRole::Approach);Marker(10,20,AutoYieldRole::Main);Marker(27,20,AutoYieldRole::Clear);
	Station *station=Station::Create(TileXY(21,20));station->owner=_current_company;
	station->facilities={StationFacility::Train};station->train_station=TileArea(station->xy,4,1);
	for(uint x=21;x<=24;x++)MakeRailStation(TileXY(x,20),_current_company,station->index,Axis::X,0,RAILTYPE_RAIL);
	Place(fast,4,20);Register(slow);Register(fast);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(slow,10,10).flags&(TRPRF_DENY|TRPRF_WAIT_AT_PBS))==0);
}

TEST_CASE_METHOD(AutomaticYieldTransformFixture, "Automatic yield lifecycle - outward boundary cleanup is invariant under eight map transforms")
{
	const uint orientation=GENERATE(range(0u,8u));
	const bool reversing=GENERATE(false,true);
	Transform(orientation);
	membership->Occupy(slow);
	const auto tile=Mapped(reversing?1:30,10);
	Place(slow,TileX(tile),TileY(tile));
	const auto td=MapTrackdir(reversing?TRACKDIR_X_NE:TRACKDIR_X_SW);
	slow->direction=DiagDirToDir(TrackdirToExitdir(td));
	AutoRestoreBackup tick(_state_ticks,StateTicks{64});
	AutoYieldGameTick();
	CHECK_FALSE(membership->IsOccupant(slow->index));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield diagnostics - enabling logs cannot change admission or saved state")
{
	Register(slow); Register(fast);
	const auto members=membership->occupants;
	GameRandomSeedChecker random_checker;
	const auto before=Execute(slow,10,10);
	AutoRestoreBackup<int8_t> debug(_debug_levels[size_t(DebugLevelID::yapf)],int8_t{7});
	const auto after=Execute(slow,10,10);
	CHECK(before.flags==after.flags);CHECK(before.penalty==after.penalty);
	CHECK(membership->occupants==members);CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield conflict - an unfinished station target before Main cannot invent a following route")
{
	Station *station=Station::Create(TileXY(5,10));station->owner=_current_company;
	station->facilities={StationFacility::Train};station->train_station=TileArea(station->xy,2,1);
	for(uint x=5;x<=6;x++)MakeRailStation(TileXY(x,10),_current_company,station->index,Axis::X,0,RAILTYPE_RAIL);
	fast->current_order.MakeGoToStation(station->index);
	Register(slow);Register(fast);
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags&(TRPRF_DENY|TRPRF_WAIT_AT_PBS))==0);
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield conflict - alternative station platforms cannot invent a bypass Main approach")
{
	SetupPlatform(true);
	for(uint x=11;x<=15;x++)MakeRailStation(TileXY(x,8),_current_company,platform_station->index,Axis::X,0,RAILTYPE_RAIL);
	fast->current_order.MakeGoToStation(platform_station->index);
	Place(slow,9,10); // Its whole body is past the station's inbound split.
	Register(slow);Register(fast);
	GameRandomSeedChecker random_checker;
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	CHECK((Execute(slow,10,10).flags&(TRPRF_DENY|TRPRF_WAIT_AT_PBS))==0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield turnover - the next PBS stopping point must fit the whole departing tail beyond Clear")
{
	medium->gcache.cached_total_length=8*TILE_SIZE;
	Place(medium,14,12);DistantExpress(fast);fast->cur_speed=40;
	Register(slow);Register(medium);Register(fast);Execute(medium,10,12,TRPISP_ACQUIRE);
	bool safe=false;
	SECTION("a nearby signal does not fit the whole tail") {}
	SECTION("a proved additional block fits the whole tail") {
		SB(_m[TileXY(39,10)].m5,6,2,to_underlying(RailTileType::Signals));
		SetPresentSignals(TileXY(39,10),SignalAlongTrackdir(TRACKDIR_X_SW));
		SetSignalType(TileXY(39,10),TRACK_X,SignalType::Path);safe=true;
	}
	GameRandomSeedChecker random_checker;
	CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::Evacuate)==safe);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random_checker.Check());
}


TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield regression - committed targets survive temporary route uncertainty")
{
 const bool exit_only = GENERATE(false,true);
 if(exit_only) UseExitOnly();
 Register(slow);Register(fast);
 const uint x=exit_only?18:10;
 Place(slow,14,12);
 Execute(slow,x,12,TRPISP_PBS_RES_END_ACQUIRE);
 TraceRestrictSlot *targets=nullptr;
 for(auto *slot:TraceRestrictSlot::Iterate())if(slot->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets))targets=slot;
 REQUIRE(targets!=nullptr);REQUIRE(targets->IsOccupant(fast->index));
 CHECK_FALSE(targets->IsOccupant(slow->index));
 CHECK(TraceRestrictSlot::ValidateVehicleIndex());
 // A temporary restriction beyond the merge prevents proving either outlet.
 auto *program=Marker(23,10,AutoYieldRole::Main);
 TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);
 program->items={deny.AsProgramItem()};REQUIRE(program->Validate().Succeeded());
 InvalidateAutoYieldAreas();UpdateAutoYieldAreas();
 REQUIRE(targets->IsOccupant(fast->index));
 CHECK((Execute(slow,18,12).flags&TRPRF_WAIT_AT_PBS)!=0);
 SECTION("fast tail clears") { Execute(fast,27,10,TRPISP_RELEASE_BACK); }
 SECTION("fast membership cancelled") { membership->Vacate(fast); }
 SECTION("fast deleted") { TraceRestrictRemoveVehicleFromAllSlots(fast->index); }
 SECTION("slow membership cancelled") { membership->Vacate(slow); }
 CHECK(targets->occupants.empty());
 CHECK((Execute(slow,18,12).flags&TRPRF_WAIT_AT_PBS)==0);
 CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield regression - speculative lane search cannot record another vehicle")
{
 UseExitOnly();Register(slow);Register(fast);
 TraceRestrictSlot *targets=nullptr;
 for(auto *slot:TraceRestrictSlot::Iterate())if(slot->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets))targets=slot;
 REQUIRE(targets!=nullptr);
 TraceRestrictSlotTemporaryState temporary;
 temporary.PushToChangeStack();
 Execute(slow,18,12,TRPISP_ACQUIRE_TEMP_STATE);
 CHECK(targets->occupants.empty());
 temporary.PopFromChangeStackRevertTemporaryChanges(slow->index);
 CHECK(targets->occupants.empty());
 CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield conflict resources - a foreign detector can request the common route without joining our slot")
{
 for(uint x=1;x<6;x++)MakeRailNormal(TileXY(x,8),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
 MakeRailNormal(TileXY(6,8),_current_company,TRACK_BIT_RIGHT,RAILTYPE_RAIL);
 MakeRailNormal(TileXY(6,9),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
 MakeRailNormal(TileXY(6,10),_current_company,TRACK_BIT_X|TRACK_BIT_LEFT,RAILTYPE_RAIL);
 auto *foreign=TraceRestrictSlot::Create(_current_company);foreign->max_occupancy=16;
 Marker(3,8,AutoYieldRole::Approach,foreign->index.base());
 Marker(4,8,AutoYieldRole::Main,foreign->index.base());
 Marker(4,6,AutoYieldRole::Exit,foreign->index.base());
 auto *clear=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(27,10),TRACK_X),false);
 TraceRestrictInstructionItem extra{};extra.SetType(TRIT_AUTO_YIELD_CLEAR);extra.SetValue(foreign->index);
 clear->items.push_back(extra.AsProgramItem());REQUIRE(clear->Validate().Succeeded());
 InvalidateAutoYieldAreas();UpdateAutoYieldAreas();
 Place(fast,4,8);Register(slow);REQUIRE(foreign->Occupy(fast));
 CHECK_FALSE(membership->IsOccupant(fast->index));
 CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
 CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
 Place(slow,14,12);Execute(slow,10,12,TRPISP_PBS_RES_END_ACQUIRE);
 CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
 foreign->Vacate(fast);
 CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
 for(auto*s:TraceRestrictSlot::Iterate())if(s->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets))CHECK_FALSE(s->IsOccupant(fast->index));
 CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield parking - a reserved shared exit cannot turn an empty siding into an occupied lane")
{
 Register(slow);Register(fast);
 REQUIRE(TryReserveRailTrack(TileXY(24,10),TRACK_X));
 const auto reserved=GetReservedTrackbits(TileXY(24,10));
 CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
 CHECK(GetReservedTrackbits(TileXY(24,10))==reserved);
 CHECK(Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->occupants.empty());
 CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield saved dependency cycle - only a safe deterministic departure falls back to PBS")
{
 UpperSiding(false);
 AutoRestoreBackup<uint16_t> rail_cap(const_cast<RailTypeInfo*>(GetRailTypeInfo(RAILTYPE_RAIL))->max_speed,uint16_t{0});
 auto *second=TraceRestrictSlot::Create(_current_company);second->max_occupancy=16;
 auto *approach=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(3,10),TRACK_X),false);
 approach->items[1]=TraceRestrictProgramItem{TEST_YIELD_CONFIG|100};REQUIRE(approach->Validate().Succeeded());
 for(auto *program:TraceRestrictProgram::Iterate()) {
  auto copies=program->items;
  for(auto iter:program->IterateInstructions()) {
   auto item=iter.Instruction();item.SetValue(second->index);copies.push_back(item.AsProgramItem());
   if(item.GetType()!=TRIT_AUTO_YIELD_CLEAR)copies.emplace_back(iter.Secondary()&0xffff0000u);
  }
  program->items=std::move(copies);REQUIRE(program->Validate().Succeeded());
 }
 InvalidateAutoYieldAreas();UpdateAutoYieldAreas();
 AutoRestoreBackup passenger_class(CargoSpec::Get(0)->classes,CargoClasses{CargoClass::Passengers});
 medium->vcache.cached_max_speed=160;fast->vcache.cached_max_speed=120;
 fast->cargo_type=CargoType{0};fast->cargo_cap=10;
 fast->cargo.Append(CargoPacket::Create(uint16_t{1},uint16_t{0},StationID::Invalid(),TileIndex{0},Money{0}));
 Place(medium,14,12);Place(fast,14,8);
 REQUIRE(membership->Occupy(medium));REQUIRE(membership->Occupy(fast));REQUIRE(second->Occupy(medium));REQUIRE(second->Occupy(fast));
 auto lane_ref=MakeTraceRestrictRefId(TileXY(10,12),TRACK_X),upper_ref=MakeTraceRestrictRefId(TileXY(10,8),TRACK_X);
 for(auto*s:TraceRestrictSlot::Iterate()) {
  if(s->automatic_yield_area==membership->index.base()+1&&s->automatic_yield_signal==lane_ref)REQUIRE(s->Occupy(s->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets)?fast:medium));
  if(s->automatic_yield_area==second->index.base()+1&&s->automatic_yield_signal==upper_ref)REQUIRE(s->Occupy(s->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets)?medium:fast));
 }
 bool safe=true;
 SECTION("a proved free departure breaks the logical cycle"){}
 SECTION("a conflicting reservation must retain native protection"){REQUIRE(TryReserveRailTrack(TileXY(24,10),TRACK_X));safe=false;}
 CHECK(EvaluateAutoYield(medium,membership->index.base(),AutoYieldCondition::MustWait));
 CHECK(EvaluateAutoYield(fast,second->index.base(),AutoYieldCondition::MustWait)==!safe);
 CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldWideFixture, "Automatic yield long approach - a confirmed follower beyond 256 nodes still diverts into free parking")
{
	const uint gap = GENERATE(255u, 257u, 300u);
	for (auto *program : TraceRestrictProgram::Iterate()) {
		program->items.clear(); REQUIRE(program->Validate().Succeeded());
	}
	MakeRailNormal(TileXY(8,10),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(20,10),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	for(uint x=41;x<=380;x++)MakeRailNormal(TileXY(x,10),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	for(uint x=328;x<=340;x++)MakeRailNormal(TileXY(x,12),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(328,10),_current_company,TRACK_BIT_X|TRACK_BIT_RIGHT,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(328,11),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(328,12),_current_company,TRACK_BIT_LEFT,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(340,12),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(340,11),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(340,10),_current_company,TRACK_BIT_X|TRACK_BIT_LOWER,RAILTYPE_RAIL);
	Marker(3,10,AutoYieldRole::Approach);Marker(330,10,AutoYieldRole::Main);
	Marker(330,12,AutoYieldRole::Siding);Marker(338,12,AutoYieldRole::Exit);Marker(347,10,AutoYieldRole::Clear);
	Place(slow,326,10);Place(fast,330-gap,10);
	Register(slow);Register(fast);
	bool divert=true;
	SECTION("free parking is used"){}
	SECTION("another train's reservation still prevents admission") { REQUIRE(TryReserveRailTrack(TileXY(334,12),TRACK_X));divert=false; }
	SECTION("an actually disconnected follower does not impose a wait") { MakeRailNormal(TileXY(100,20),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);Place(fast,100,20);divert=false; }
	GameRandomSeedChecker random;
	CHECK(((Execute(slow,330,10).flags&TRPRF_DENY)!=0)==divert);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield pre-fork refresh - a reachable siding is not an already reserved siding")
{
	Station *target=Station::Create(TileXY(40,10));target->owner=_current_company;
	target->facilities={StationFacility::Train};target->train_station=TileArea(target->xy,1,1);
	target->rect.BeforeAddTile(target->xy,StationRect::ADD_FORCE);
	MakeRailStation(target->xy,_current_company,target->index,Axis::X,0,RAILTYPE_RAIL);
	Place(slow,5,10);Place(medium,2,10);
	slow->current_order.MakeGoToStation(target->index);slow->dest_tile=target->xy;
	Register(slow);Register(medium);
	for(uint x=5;x<=6;x++)REQUIRE(TryReserveRailTrack(TileXY(x,10),TRACK_X));
	REQUIRE(FollowTrainReservation(slow,nullptr,FollowTrainReservationFlag::OkayUnused).tile==TileXY(6,10));
	REQUIRE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	REQUIRE_FALSE(slow->flags.Test(VehicleRailFlag::Stuck));
	GameRandomSeedChecker random;
	AutoRestoreBackup ticks(_state_ticks,StateTicks{(8-(slow->index.base()&7))&7});
	AutoYieldGameTick();
	/* Either the native safe-end reservation succeeds into the siding, or
	 * native PBS reports its ordinary failed-reservation stop. It must not
	 * retain the old branch decision simply because both exits are reachable. */
	const bool siding=TrainReservationPassesThroughTile(slow,TileXY(10,12),TRACK_X);
	CHECK(siding==Resource(MakeTraceRestrictRefId(TileXY(10,12),TRACK_X))->IsOccupant(slow->index));
	CHECK((siding||slow->flags.Test(VehicleRailFlag::Stuck)));
	CHECK(slow->tile==TileXY(5,10));CHECK(slow->current_order.GetDestination()==target->index);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random.Check());
}

TEST_CASE_METHOD(AutomaticYieldFixture, "Automatic yield continuous waiting - a later express is retained after the first tail clears")
{
	Register(slow);Register(fast);
	Place(slow,14,12);slow->cur_speed=0;
	Execute(slow,10,12,TRPISP_PBS_RES_END_ACQUIRE);
	REQUIRE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	DistantExpress(medium);medium->cur_speed=20;Register(medium);
	AutoRestoreBackup tick(_state_ticks,StateTicks{(8-(slow->index.base()&7))&7});
	AutoYieldGameTick();
	TraceRestrictSlot*targets=nullptr;
	for(auto*s:TraceRestrictSlot::Iterate())if(s->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets))targets=s;
	REQUIRE(targets!=nullptr);
	CHECK(targets->IsOccupant(fast->index));
	CHECK(targets->IsOccupant(medium->index));
	membership->Vacate(fast);
	CHECK_FALSE(targets->IsOccupant(fast->index));
	CHECK(targets->IsOccupant(medium->index));
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	membership->Vacate(medium);
	CHECK(targets->occupants.empty());
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(AutomaticYieldWideFixture, "Automatic yield split admission - prove the candidate destination after bypassing an irrelevant head branch")
{
	const bool exit_only=GENERATE(false,true);
	if(exit_only)UseExitOnly();
	for(uint x=41;x<=280;x++)MakeRailNormal(TileXY(x,10),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(15,10),_current_company,TRACK_BIT_X|TRACK_BIT_UPPER,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(15,9),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(15,8),_current_company,TRACK_BIT_LOWER,RAILTYPE_RAIL);
	for(uint x=16;x<=280;x++)MakeRailNormal(TileXY(x,8),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
	Marker(27,8,AutoYieldRole::Clear);
	for(auto [train,row]:{std::pair{slow,10u},std::pair{fast,8u}}){
		Station*station=Station::Create(TileXY(260,row));station->owner=_current_company;
		station->facilities={StationFacility::Train};station->train_station=TileArea(station->xy,1,1);
		station->rect.BeforeAddTile(station->xy,StationRect::ADD_FORCE);
		MakeRailStation(station->xy,_current_company,station->index,Axis::X,0,RAILTYPE_RAIL);
		train->current_order.MakeGoToStation(station->index);train->dest_tile=station->xy;
	}
	Register(slow);Register(fast);
	GameRandomSeedChecker random;
	CHECK((Execute(slow,10,10).flags&TRPRF_DENY)!=0);
	/* After their actual paths split, the siding rejoins only the slow
	 * train's own continuation. The express's separate exit must not hold it. */
	Place(slow,14,12);Place(fast,16,8);
	CHECK((Execute(slow,18,12).flags&TRPRF_WAIT_AT_PBS)==0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random.Check());
}

class AutomaticYieldStationRouteFixture : public AutomaticYieldFixture {
protected:
	AutoRestoreBackup<bool> forbid_turns{_settings_game.pf.forbid_90_deg,true};
	Station *stop, *common_goal, *separate_goal;
	AutomaticYieldStationRouteFixture()
	{
		REQUIRE(OrderList::GetNumItems()==0);
		for (uint x=1;x<=40;x++) MakeRailNormal(TileXY(x,6),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20,6),_current_company,TRACK_BIT_X|TRACK_BIT_RIGHT,RAILTYPE_RAIL);
		for(uint y=7;y<10;y++)MakeRailNormal(TileXY(20,y),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
		MakeRailNormal(TileXY(20,10),_current_company,TRACK_BIT_X|TRACK_BIT_LEFT|TRACK_BIT_LOWER,RAILTYPE_RAIL);
		Marker(3,6,AutoYieldRole::Approach);Marker(10,6,AutoYieldRole::Main);Marker(27,6,AutoYieldRole::Clear);
		stop=StationAt(12,6);common_goal=StationAt(35,10);separate_goal=StationAt(35,6);
		slow->current_order.MakeGoToStation(common_goal->index);slow->dest_tile=common_goal->xy;
		Place(fast,4,6);
		Order first;first.MakeGoToStation(stop->index);
		fast->orders=OrderList::Create(Order(first),fast);fast->primary_order=fast->orders->index;
		Order next;next.MakeGoToStation(common_goal->index);fast->orders->InsertOrderAt(std::move(next),1);
		fast->current_order=first;fast->dest_tile=stop->xy;fast->cur_real_order_index=0;fast->cur_implicit_order_index=0;
		Register(slow);Register(fast);
	}
	~AutomaticYieldStationRouteFixture()
	{
		fast->orders=nullptr;fast->primary_order=OrderListID::Invalid();slow->orders=nullptr;slow->primary_order=OrderListID::Invalid();_orderlist_pool.CleanPool();
	}
	Station *StationAt(uint x,uint y)
	{
		Station*s=Station::Create(TileXY(x,y));s->owner=_current_company;s->facilities={StationFacility::Train};
		s->train_station=TileArea(s->xy,1,1);s->rect.BeforeAddTile(s->xy,StationRect::ADD_FORCE);
		MakeRailStation(s->xy,_current_company,s->index,Axis::X,0,RAILTYPE_RAIL);return s;
	}
};

TEST_CASE_METHOD(AutomaticYieldStationRouteFixture,"Automatic yield station continuation - next destination determines the shared exit without changing orders")
{
	bool yield=true;
	SECTION("station stop followed by the shared exit"){}
	SECTION("a different destination beyond the stop uses the independent exit"){
		fast->GetOrder(1)->MakeGoToStation(separate_goal->index);yield=false;
	}
	SECTION("an unresolved conditional after the stop is not a route prediction"){
		fast->GetOrder(1)->MakeConditional(0);fast->GetOrder(1)->SetConditionVariable(OrderConditionVariable::LoadPercentage);yield=false;
	}
	SECTION("coupling after the stop must not be skipped"){
		fast->GetOrder(1)->MakeWaitCouple();yield=false;
	}
	SECTION("the end of an executed schedule cannot loop to its first station"){
		Order home_order;home_order.MakeGoToStation(separate_goal->index);
		fast->primary_order=OrderList::Create(std::move(home_order),fast)->index;
		fast->GetOrder(0)->MakeGoToStation(common_goal->index);fast->GetOrder(1)->MakeGoToStation(stop->index);
		fast->cur_real_order_index=1;fast->cur_implicit_order_index=1;yield=false;
	}
	const auto old_current=fast->current_order;const auto old_tile=fast->dest_tile;
	const auto old_real=fast->cur_real_order_index,old_implicit=fast->cur_implicit_order_index;
	GameRandomSeedChecker random;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding)==yield);
	CHECK(((Execute(slow,10,10).flags&TRPRF_DENY)!=0)==yield);
	CHECK(fast->current_order.GetType()==old_current.GetType());CHECK(fast->current_order.GetDestination()==old_current.GetDestination());
	CHECK(fast->dest_tile==old_tile);CHECK(fast->cur_real_order_index==old_real);CHECK(fast->cur_implicit_order_index==old_implicit);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random.Check());
}

TEST_CASE_METHOD(AutomaticYieldStationRouteFixture,"Automatic yield station continuation - a candidate platform serves the stop before forecasting its exit")
{
	const bool exit_only=GENERATE(false,true);
	if(exit_only)UseExitOnly();
	Station*station=StationAt(14,12);
	Marker(18,12,AutoYieldRole::Exit,membership->index.base(),TEST_YIELD_CONFIG|AUTO_YIELD_PLATFORM);
	Order first;first.MakeGoToStation(station->index);
	slow->orders=OrderList::Create(Order(first),slow);slow->primary_order=slow->orders->index;
	Order next;next.MakeGoToStation(common_goal->index);slow->orders->InsertOrderAt(std::move(next),1);
	slow->current_order=first;slow->dest_tile=station->xy;
	GameRandomSeedChecker random;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::FreeSiding));
	CHECK(slow->current_order.GetDestination()==station->index);CHECK(slow->cur_real_order_index==0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random.Check());
}

TEST_CASE_METHOD(AutomaticYieldStationRouteFixture,"Automatic yield station continuation - a conditional departure during loading does not clear a saved wait")
{
	Marker(11,6,AutoYieldRole::Siding);
	Marker(18,6,AutoYieldRole::Exit,membership->index.base(),TEST_YIELD_CONFIG|AUTO_YIELD_PLATFORM);
	Place(fast,12,6);fast->last_station_visited=stop->index;fast->current_order.MakeLoading(true);
	Place(slow,14,12);slow->cur_speed=0;
	Execute(slow,10,12,TRPISP_ACQUIRE);
	REQUIRE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	fast->GetOrder(1)->MakeConditional(0);fast->GetOrder(1)->SetConditionVariable(OrderConditionVariable::LoadPercentage);
	GameRandomSeedChecker random;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK(fast->current_order.IsType(OT_LOADING));CHECK(fast->cur_real_order_index==0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random.Check());
}

class AutomaticYieldLongDestinationFixture : public AutomaticYieldWideFixture {
protected:
	AutoRestoreBackup<bool> forbid_turns{_settings_game.pf.forbid_90_deg,true};
	AutomaticYieldLongDestinationFixture()
	{
		REQUIRE(OrderList::GetNumItems()==0);
		for(uint x=41;x<=420;x++)MakeRailNormal(TileXY(x,10),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);
		std::array<StationID,2> stations{};
		for(uint i=0;i<stations.size();i++){
			Station*s=Station::Create(TileXY(380+i*20,10));s->owner=_current_company;s->facilities={StationFacility::Train};
			s->train_station=TileArea(s->xy,1,1);s->rect.BeforeAddTile(s->xy,StationRect::ADD_FORCE);
			MakeRailStation(s->xy,_current_company,s->index,Axis::X,0,RAILTYPE_RAIL);stations[i]=s->index;
		}
		Order first;first.MakeGoToStation(stations[0]);slow->orders=OrderList::Create(Order(first),slow);slow->primary_order=slow->orders->index;
		Order next;next.MakeGoToStation(stations[1]);slow->orders->InsertOrderAt(std::move(next),1);
		slow->current_order=first;slow->dest_tile=TileXY(380,10);fast->current_order=first;fast->dest_tile=slow->dest_tile;
	}
	~AutomaticYieldLongDestinationFixture(){slow->orders=nullptr;slow->primary_order=OrderListID::Invalid();_orderlist_pool.CleanPool();}
};

TEST_CASE_METHOD(AutomaticYieldLongDestinationFixture,"Automatic yield distant destinations - committing a diversion retains the confirmed follower")
{
	const bool exit_only=GENERATE(false,true);
	if(exit_only)UseExitOnly();
	DistantExpress(fast);fast->cur_speed=40;slow->cur_speed=40;
	Register(slow);Register(fast);
	REQUIRE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding));
	GameRandomSeedChecker random;
	Execute(slow,exit_only?18:10,12,TRPISP_PBS_RES_END_ACQUIRE);
	bool saved=false;
	for(const auto*s:TraceRestrictSlot::Iterate())if(s->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets)&&s->IsOccupant(fast->index))saved=true;
	CHECK(saved);
	Place(slow,14,12);slow->cur_speed=0;
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(slow,18,12).flags&TRPRF_WAIT_AT_PBS)!=0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random.Check());
}

TEST_CASE_METHOD(AutomaticYieldLongDestinationFixture,"Automatic yield distant destinations - late confirmed main arrivals survive the first express")
{
	const bool exit_only=GENERATE(false,true);if(exit_only)UseExitOnly();
	Register(slow);Register(fast);Execute(slow,exit_only?18:10,12,TRPISP_PBS_RES_END_ACQUIRE);
	Place(slow,14,12);slow->cur_speed=0;
	REQUIRE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	for(uint x=1;x<=200;x++){MakeRailNormal(TileXY(x,22),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);MakeRailNormal(TileXY(x,4),_current_company,TRACK_BIT_X,RAILTYPE_RAIL);}
	for(uint y=5;y<22;y++)MakeRailNormal(TileXY(200,y),_current_company,y==10?TRACK_BIT_X|TRACK_BIT_Y:TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(200,22),_current_company,TRACK_BIT_UPPER,RAILTYPE_RAIL);MakeRailNormal(TileXY(200,4),_current_company,TRACK_BIT_RIGHT,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(1,4),_current_company,TRACK_BIT_LOWER,RAILTYPE_RAIL);for(uint y=5;y<10;y++)MakeRailNormal(TileXY(1,y),_current_company,TRACK_BIT_Y,RAILTYPE_RAIL);
	MakeRailNormal(TileXY(1,10),_current_company,TRACK_BIT_X|TRACK_BIT_LEFT,RAILTYPE_RAIL);
	Place(medium,170,22);medium->cur_speed=40;medium->current_order=slow->current_order;medium->dest_tile=slow->dest_tile;
	bool conflict=true;
	SECTION("a positively reachable common main retains the later express"){}
	SECTION("a disconnected feeder creates no new waiting relation"){MakeClear(TileXY(200,21),ClearGround::Grass,0);conflict=false;}
	SECTION("a mandatory stop before Main remains unresolved"){
		Station*s=Station::Create(TileXY(190,22));s->owner=_current_company;s->facilities={StationFacility::Train};s->train_station=TileArea(s->xy,1,1);s->rect.BeforeAddTile(s->xy,StationRect::ADD_FORCE);MakeRailStation(s->xy,_current_company,s->index,Axis::X,0,RAILTYPE_RAIL);medium->current_order.MakeGoToStation(s->index);medium->dest_tile=s->xy;conflict=false;
	}
	Register(medium);GameRandomSeedChecker random;
	AutoRestoreBackup tick(_state_ticks,StateTicks{(8-(slow->index.base()&7))&7});AutoYieldGameTick();
	bool saved=false;for(const auto*s:TraceRestrictSlot::Iterate())if(s->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets)&&s->IsOccupant(medium->index))saved=true;
	CHECK(saved==conflict);
	membership->Vacate(fast);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait)==conflict);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random.Check());
}

TEST_CASE_METHOD(AutomaticYieldStationRouteFixture,"Automatic yield station continuation - short loading protects the matching exit but not an independent route")
{
	Marker(11,6,AutoYieldRole::Siding);
	Marker(18,6,AutoYieldRole::Exit,membership->index.base(),TEST_YIELD_CONFIG|AUTO_YIELD_PLATFORM);
	Place(fast,12,6);fast->last_station_visited=stop->index;fast->current_order.MakeLoading(true);fast->current_order_time=0;
	Place(slow,14,12);slow->cur_speed=0;
	bool wait=true;
	SECTION("normal short loading preserves priority at its next shared resource"){}
	SECTION("the next destination is independent"){
		fast->GetOrder(1)->MakeGoToStation(separate_goal->index);wait=false;
	}
	SECTION("full load does not hold other traffic indefinitely"){
		fast->current_order.SetLoadType(OrderLoadType::FullLoad);wait=false;
	}
	SECTION("long station work keeps its existing timeout protection"){
		fast->current_order_time=300;wait=false;
	}
	GameRandomSeedChecker random;
	Execute(slow,10,12,TRPISP_ACQUIRE);
	CAPTURE(wait,fast->current_order.GetLoadType(),fast->current_order_time);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait)==wait);
	CHECK(((Execute(slow,18,12).flags&TRPRF_WAIT_AT_PBS)!=0)==wait);
	CHECK(fast->current_order.IsType(OT_LOADING));CHECK(fast->cur_real_order_index==0);CHECK(fast->cur_implicit_order_index==0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());CHECK(random.Check());
}

#include "auto_yield_topology_proof.inc"
#include "auto_yield_outlet_candidates.inc"
#include "auto_yield_mixed_platform.inc"
#include "auto_yield_depot_reservation.inc"

TEST_CASE_METHOD(AutomaticYieldFixture, "PBS reservation - a split partner behind cannot block departure but a part ahead still does")
{
	const bool towards_ne = GENERATE(false, true);
	const bool ahead = GENERATE(false, true);
	AutoRestoreBackup braking(_settings_game.vehicle.train_braking_model, uint8_t{TBM_ORIGINAL});
	Place(slow, 23, 10); Place(fast, 23, 10); Place(medium, 45, 10);
	Station *station = Station::Create(slow->tile);
	station->owner = _current_company;
	station->facilities = {StationFacility::Train};
	station->train_station = TileArea(station->xy, 1, 1);
	station->rect.BeforeAddTile(station->xy, StationRect::ADD_FORCE);
	MakeRailStation(station->xy, _current_company, station->index, Axis::X, 0, RAILTYPE_RAIL);
	SB(_m[TileXY(12, 10)].m5, 6, 2, to_underlying(RailTileType::Signals));
	SetPresentSignals(TileXY(12, 10), SignalAlongTrackdir(TRACKDIR_X_NE));
	SetSignalType(TileXY(12, 10), TRACK_X, SignalType::Path);
	slow->current_order.MakeDummy(); fast->current_order.MakeDummy();
	slow->flags.Set(VehicleRailFlag::JustDecoupled);
	fast->flags.Set(VehicleRailFlag::SameDirDecouplePending);
	slow->direction = towards_ne ? Direction::NE : Direction::SW;
	fast->direction = ReverseDir(slow->direction);
	slow->gcache.cached_veh_length = fast->gcache.cached_veh_length = 2;
	const int delta = towards_ne ? -1 : 1;
	slow->x_pos = 23 * TILE_SIZE + ((ahead == towards_ne) ? 12 : 4);
	fast->x_pos = slow->x_pos + (ahead ? 11 : -11) * delta;
	for (uint x = 13; x <= 32; x++) REQUIRE(TryReserveRailTrack(TileXY(x, 10), TRACK_X));
	const auto origin = FollowTrainReservation(slow);
	REQUIRE(origin.okay);
	REQUIRE(origin.tile == TileXY(towards_ne ? 13 : 32, 10));
	GameRandomSeedChecker random;
	CHECK(FindForeignTrainOnReservation(slow) == (ahead ? fast : nullptr));
	CHECK(((TryPathReserveWithResultFlags(slow, true, false) & TPRRF_RESERVATION_OK) != 0) == !ahead);
	CHECK(slow->flags.Test(VehicleRailFlag::Stuck) == ahead);
	CHECK(random.Check());
	for (Train *train : {slow, fast, medium}) CHECK(ValidateVehicleTileHash(train));
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

TEST_CASE_METHOD(CoupleBlockingPartnerFixture, "Couple - reserved approach recognises a claimed rear vehicle")
{
	Train *rear = MakeTrain();
	rear->owner = waiting->owner;
	rear->track = TRACK_BIT_X;
	rear->direction = waiting->direction;
	waiting->SetNext(rear);
	Place(rear, 11);
	station->train_station = TileArea(TileXY(11, 10), 2, 1);
	station->rect.BeforeAddTile(rear->tile, StationRect::ADD_FORCE);
	MakeRailStation(rear->tile, _current_company, station->index, Axis::X, 0, RAILTYPE_RAIL);
	waiting->gcache.cached_total_length += VEHICLE_LENGTH;
	REQUIRE(rear->Primary() == waiting);
	REQUIRE(TryReserveRailTrack(rear->tile, TRACK_X));
	moving->couple_target = rear->index;
	ClaimCoupleTarget(moving, waiting, 0);
	REQUIRE(GetCoupleClaimant(waiting) == moving);
	REQUIRE(FindForeignTrainOnReservation(moving) == rear);
	GameRandomSeedChecker random_checker;
	SECTION("valid partner rear permits the reserved approach") {
		CHECK(TryPathReserve(moving, true));
		CHECK_FALSE(moving->flags.Test(VehicleRailFlag::Stuck));
	}
	SECTION("unrelated consist on the same approach still blocks") {
		Train *obstruction = MakeTrain();
		obstruction->SetFrontEngine();
		obstruction->owner = waiting->owner;
		obstruction->track = TRACK_BIT_X;
		Place(obstruction, 10);
		obstruction->x_pos += 6;
		CHECK_FALSE(TryPathReserve(moving, true));
		CHECK(moving->flags.Test(VehicleRailFlag::Stuck));
	}
	CHECK(GetCoupleClaimant(waiting) == moving);
	CHECK(random_checker.Check());
}

TEST_CASE_METHOD(CoupleBlockingPartnerFixture, "Couple - direction preflight observes live target conditions without caching")
{
	GameRandomSeedChecker random_checker;
	REQUIRE(HasPotentialCoupleTarget(moving));
	SECTION("manually stopped partner resumes in the same tick") {
		waiting->vehstatus.Set(VehState::Stopped);
		CHECK_FALSE(HasPotentialCoupleTarget(moving));
		waiting->vehstatus.Reset(VehState::Stopped);
	}
	SECTION("new waiting order is visible in the same tick") {
		waiting->current_order.Free();
		CHECK_FALSE(HasPotentialCoupleTarget(moving));
		waiting->current_order.MakeWaitCouple();
		waiting->current_order.SetCoupleSlot(TraceRestrictSlotID::Invalid());
	}
	SECTION("station restriction must match") {
		moving->current_order.SetCoupleStation(StationID{62});
		CHECK_FALSE(HasPotentialCoupleTarget(moving));
		moving->current_order.SetCoupleStation(station->index);
	}
	SECTION("another valid exclusive claim blocks the candidate") {
		Train *other = MakeTrain();
		other->SetFrontEngine();
		other->owner = moving->owner;
		other->current_order = moving->current_order;
		other->couple_target = waiting->index;
		waiting->couple_claimant = other->index;
		CHECK_FALSE(HasPotentialCoupleTarget(moving));
		waiting->couple_claimant = VehicleID::Invalid();
	}
	CHECK(HasPotentialCoupleTarget(moving));
	CHECK(random_checker.Check());
	CHECK(GetCoupleClaimant(waiting) == nullptr);
	CHECK(moving->couple_target == VehicleID::Invalid());
}
