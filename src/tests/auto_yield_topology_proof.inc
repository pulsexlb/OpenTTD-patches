/* Focused route-recognition regressions. Included after the shared fixtures. */
TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield topology proof - a mandatory station branch is independent without a platform hint")
{
	const bool platform_hint = GENERATE(false, true);
	SetupPlatform(true);
	if (!platform_hint) Marker(18,12,AutoYieldRole::Exit,membership->index.base(),TEST_YIELD_CONFIG);
	for (uint x=11;x<=15;x++) MakeRailStation(TileXY(x,8),_current_company,platform_station->index,Axis::X,0,RAILTYPE_RAIL);
	fast->current_order.MakeGoToStation(platform_station->index);
	bool shared_prefix = false;
	SECTION("the slow train is beyond the mandatory station's inbound split") { Place(slow,9,10); }
	SECTION("the fast train still needs the shared approach before the split") { Place(slow,6,10); shared_prefix = true; }
	Register(slow); Register(fast);
	GameRandomSeedChecker random;
	CAPTURE(platform_hint,shared_prefix);
	CHECK(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::NeedsSiding) == shared_prefix);
	CHECK(((Execute(slow,10,10).flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) != 0) == shared_prefix);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random.Check());
}

TEST_CASE_METHOD(AutomaticYieldPlatformFixture, "Automatic yield topology proof - a late claimed siding cannot invent priority for an independent mandatory station")
{
	SetupPlatform(true);
	Marker(18,12,AutoYieldRole::Exit,membership->index.base(),TEST_YIELD_CONFIG);
	for(uint x=11;x<=15;x++) MakeRailStation(TileXY(x,8),_current_company,platform_station->index,Axis::X,0,RAILTYPE_RAIL);
	fast->current_order.MakeGoToStation(platform_station->index);
	Marker(12,10,AutoYieldRole::Main); Marker(15,10,AutoYieldRole::Main);
	Register(slow); Register(fast); Place(slow,14,12); slow->cur_speed=40;
	GameRandomSeedChecker random;
	Execute(slow,18,12,TRPISP_PBS_RES_END_ACQUIRE);
	for(const auto *slot:TraceRestrictSlot::Iterate()) if(slot->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets)) CHECK_FALSE(slot->IsOccupant(fast->index));
	CHECK_FALSE(EvaluateAutoYield(slow,membership->index.base(),AutoYieldCondition::MustWait));
	CHECK((Execute(slow,18,12).flags&TRPRF_WAIT_AT_PBS)==0);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random.Check());
}
