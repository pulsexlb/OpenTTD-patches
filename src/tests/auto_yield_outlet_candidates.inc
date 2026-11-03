/* Included after the automatic yield fixtures in train_decouple.cpp. */
TEST_CASE_METHOD(AutomaticYieldLongDestinationFixture, "Automatic yield outlet candidates - a nearer wrong first outlet cannot hide a valid distant destination")
{
	AutoRestoreBackup<uint32_t> lookahead(_settings_game.pf.yapf.rail_look_ahead_max_signals, 10);
	const bool exit_only = GENERATE(false, true);
	if (exit_only) UseExitOnly();
	/* Fork after the held exit. The shorter branch has its own first Clear
	 * but cannot serve the mandatory far station; the other first Clear can.
	 * Each native probe is read-only and the area must never be re-entered. */
	MakeRailNormal(TileXY(19, 12), _current_company, TRACK_BIT_X | TRACK_BIT_RIGHT, RAILTYPE_RAIL);
	MakeRailNormal(TileXY(19, 13), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
	MakeRailNormal(TileXY(19, 14), _current_company, TRACK_BIT_LEFT, RAILTYPE_RAIL);
	for (uint x = 20; x <= 30; x++) MakeRailNormal(TileXY(x, 14), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
	Marker(20, 14, AutoYieldRole::Clear);
	SetSignalType(TileXY(20, 14), TRACK_X, SignalType::PathOneWay);
	bool reachable = true;
	SECTION("the farther valid first outlet admits the free lane") {}
	SECTION("neither outlet reaches the mandatory target") {
		MakeClear(TileXY(200, 10), ClearGround::Grass, 0);
		reachable = false;
	}
	InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	Register(slow);
	GameRandomSeedChecker random;
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding) == reachable);
	CHECK(slow->cur_real_order_index == 0);
	CHECK(slow->current_order.GetDestination() == slow->GetOrder(0)->GetDestination());
	for (uint x : {18u, 19u, 27u, 200u, 380u}) CHECK(GetReservedTrackbits(TileXY(x, 10)) == TRACK_BIT_NONE);
	CHECK(GetReservedTrackbits(TileXY(20, 14)) == TRACK_BIT_NONE);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random.Check());
}
