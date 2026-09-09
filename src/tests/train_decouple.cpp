/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */

/** @file train_decouple.cpp Regression tests for read-only coupling checks and decoupling cuts. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../train.h"
#include "../engine_base.h"
#include "../newgrf.h"
#include "../newgrf_spritegroup.h"
#include "../settings_type.h"
#include "../core/random_func.hpp"
#include "../core/backup_type.hpp"

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

TEST_CASE_METHOD(TrainDecoupleFixture, "Decouple - single unit has no cut")
{
	Train *v = MakeTrain();
	for (uint count : {0, 1, 2, 127}) CHECK(GetDecoupleVehicleForCount(v, count) == nullptr);
	CHECK(GetDecoupleCuts(v).empty());
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
