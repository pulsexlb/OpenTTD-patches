/* Native query/commit boundary regressions. Full train movement remains covered
 * by regression/automatic_yield; these do not claim that identifying a platform
 * alone proves a train has completed a real yielding manoeuvre. */
class EarlyStopRouteFixture : public AutomaticYieldFixture {
protected:
	AutoRestoreBackup<uint32_t> native_lookahead{_settings_game.pf.yapf.rail_look_ahead_max_signals, 10};
	AutoRestoreBackup<bool> native_turns{_settings_game.pf.forbid_90_deg, true};
	Station *stop, *upper_goal, *lower_goal;

	Station *AddStation(uint x, uint y, uint length = 1, Station *station = nullptr)
	{
		if (station == nullptr) {
			station = Station::Create(TileXY(x, y));
			station->owner = _current_company;
			station->facilities = {StationFacility::Train};
			station->train_station = TileArea(station->xy, length, 1);
		}
		for (uint i = 0; i < length; i++) {
			const TileIndex tile = TileXY(x + i, y);
			station->rect.BeforeAddTile(tile, StationRect::ADD_FORCE);
			MakeRailStation(tile, _current_company, station->index, Axis::X, 0, RAILTYPE_RAIL);
		}
		return station;
	}
	EarlyStopRouteFixture()
	{
		REQUIRE(OrderList::GetNumItems() == 0);
		/* The two platforms share a StationID, but have truly independent
		 * directed departures. The lower platform alone reaches lower_goal. */
		for (uint x = 20; x <= 40; x++) MakeRailNormal(TileXY(x, 12), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		MakeClear(TileXY(20, 11), ClearGround::Grass, 0);
		MakeRailNormal(TileXY(20, 10), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
		stop = AddStation(12, 10, 4);
		AddStation(12, 12, 4, stop);
		stop->train_station = TileArea(TileXY(12, 10), 4, 3);
		upper_goal = AddStation(35, 10);
		lower_goal = AddStation(35, 12);
		Marker(27, 12, AutoYieldRole::Clear);
		Marker(18, 12, AutoYieldRole::Exit, membership->index.base(), TEST_YIELD_CONFIG | AUTO_YIELD_PLATFORM);
		/* This fixture asks the actual PBS transaction to reach its platform,
		 * so use native reserve-through on the preceding control signals. A
		 * nearer safe PBS boundary would legitimately reserve only that prefix. */
		for (const auto [x, y] : {std::pair{3u, 10u}, std::pair{10u, 10u}, std::pair{10u, 12u}}) {
			auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(x, y), TRACK_X), false);
			TraceRestrictInstructionItem through{}; through.SetType(TRIT_RESERVE_THROUGH);
			program->items.push_back(through.AsProgramItem()); REQUIRE(program->Validate().Succeeded());
			UpdateSignalReserveThroughBit(TileXY(x, y), TRACK_X, false);
			UpdateSignalSpecialPropagationFlag(TileXY(x, y), TRACK_X, program, false);
		}
		for (uint y : {10u, 12u}) for (uint x = 1; x <= 40; x++) {
			const TileIndex tile = TileXY(x, y);
			if (IsPlainRailTile(tile) && HasSignalOnTrack(tile, TRACK_X)) SetSignalStateByTrackdir(tile, TRACKDIR_X_SW, SignalState::Green);
		}
		Place(slow, 2, 10);
		for (auto *other : {medium, fast}) {
			MakeRailNormal(TileXY(40, 20 + other->index.base()), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
			Place(other, 40, 20 + other->index.base());
		}
		Order first; first.MakeGoToStation(stop->index);
		slow->orders = OrderList::Create(Order(first), slow); slow->primary_order = slow->orders->index;
		Order next; next.MakeGoToStation(lower_goal->index); slow->orders->InsertOrderAt(std::move(next), 1);
		slow->current_order = first; slow->dest_tile = stop->xy;
		YapfNotifyTrackLayoutChange(INVALID_TILE, INVALID_TRACK);
	}
	~EarlyStopRouteFixture()
	{
		slow->orders = nullptr; slow->primary_order = OrderListID::Invalid();
		_orderlist_pool.CleanPool();
	}
	void Stage()
	{
		AutoYieldStageNativeRoute(slow, TileXY(3, 10), TRACKDIR_X_SW, membership->index.base(), true);
	}
	YapfRailRoute CurrentRoute()
	{
		return YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256,
				[this](TileIndex tile, Trackdir td) { return AutoYieldNativeRouteAllows(slow, tile, td); });
	}
	static bool Has(const YapfRailRoute &route, TileIndex tile)
	{
		return std::any_of(route.path.begin(), route.path.end(), [tile](const auto &step) { return step.tile == tile; });
	}
};

TEST_CASE_METHOD(EarlyStopRouteFixture, "Native early stop route - known continuation chooses a compatible platform using native search")
{
	const Order before = slow->current_order;
	const auto before_destination = slow->dest_tile;
	GameRandomSeedChecker random;
	const auto ordinary = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256);
	REQUIRE(ordinary.complete); REQUIRE(Has(ordinary, TileXY(13, 10)));
	AutoYieldNativeRouteScope scope(slow, slow->tile, TRACKDIR_X_SW);
	Stage();
	REQUIRE(AutoYieldHasNativeRoute(slow));
	CHECK(AutoYieldNativeStopNeedsApproach(slow, membership->index.base(), TileXY(3, 10), TRACKDIR_X_SW));
	CHECK_FALSE(AutoYieldNativeStopNeedsApproach(slow, membership->index.base(), TileXY(18, 12), TRACKDIR_X_SW));
	CHECK_FALSE(AutoYieldNativeStopNeedsApproach(slow, membership->index.base(), TileXY(18, 10), TRACKDIR_X_SW));
	CHECK_FALSE(AutoYieldNativeRouteAllows(slow, TileXY(13, 10), TRACKDIR_X_SW));
	CHECK(AutoYieldNativeRouteAllows(slow, TileXY(13, 12), TRACKDIR_X_SW));
	const auto selected = CurrentRoute();
	REQUIRE(selected.complete); CHECK(Has(selected, TileXY(13, 12))); CHECK_FALSE(Has(selected, TileXY(13, 10)));
	CHECK(selected.path.back().tile == TileXY(15, 12));
	YapfRailRoute continuation;
	REQUIRE(AutoYieldGetNativeRoute(slow, membership->index.base(), TileXY(3, 10), TRACKDIR_X_SW, continuation));
	CHECK(continuation.path.back().tile == TileXY(27, 12));
	CHECK_FALSE(AutoYieldNativeRouteAllows(slow, TileXY(27, 10), TRACKDIR_X_SW));
	CHECK(slow->early_route.empty()); // A query is not a successful reservation.
	CHECK(slow->current_order.Equals(before)); CHECK(slow->dest_tile == before_destination);
	CHECK(slow->cur_real_order_index == 0); CHECK(slow->cur_implicit_order_index == 0);
	for (const auto &step : selected.path) CHECK(GetReservedTrackbits(step.tile) == TRACK_BIT_NONE);
	CHECK(random.Check()); CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(EarlyStopRouteFixture, "Native split alternative - exclusions only affect one fresh decision and never owned PBS")
{
	const uint16_t slot=membership->index.base();
	const std::vector<std::pair<TileIndex,Trackdir>> blocked{{TileXY(6,10),TRACKDIR_X_SW}};
	CHECK_FALSE(AutoYieldMayRequestSplitAlternative(slow,slot));
	CHECK_FALSE(AutoYieldRequestSplitAlternative(slow,slot,blocked));
	GameRandomSeedChecker random;
	AutoYieldNativeRouteScope scope(slow,slow->tile,TRACKDIR_X_SW);
	Stage(); REQUIRE(AutoYieldHasNativeRoute(slow));
	REQUIRE(AutoYieldMayRequestSplitAlternative(slow,slot));
	YapfRailRoute chosen;
	REQUIRE(AutoYieldGetNativeRoute(slow,slot,TileXY(3,10),TRACKDIR_X_SW,chosen));
	REQUIRE(std::any_of(chosen.path.begin(),chosen.path.end(),[&](const auto &step){return std::pair{step.tile,step.trackdir}==blocked.front();}));
	SECTION("recording evidence does not change the active search graph") {
		REQUIRE(AutoYieldNativeRouteAllows(slow,TileXY(6,10),TRACKDIR_X_SW));
		REQUIRE(AutoYieldRequestSplitAlternative(slow,slot,blocked));
		CHECK(AutoYieldNativeRouteAllows(slow,TileXY(6,10),TRACKDIR_X_SW));
		CHECK_FALSE(AutoYieldMayRequestSplitAlternative(slow,slot));
		CHECK_FALSE(AutoYieldRequestSplitAlternative(slow,slot,blocked));
		REQUIRE(scope.ReplanForSplit());
		CHECK_FALSE(AutoYieldNativeRouteAllows(slow,TileXY(6,10),TRACKDIR_X_SW));
		CHECK(AutoYieldNativeRouteAllows(slow,TileXY(7,10),TRACKDIR_X_SW));
		AutoYieldNativeRouteDenied(slow,TileXY(7,10),TRACKDIR_X_SW);
		CHECK(AutoYieldNativeRouteAllows(slow,TileXY(7,10),TRACKDIR_X_SW)); // The retry's filter stays frozen.
		CHECK_FALSE(scope.ReplanForSplit());
		scope.Finish(false);
		CHECK(slow->early_route.empty());
	}
	SECTION("a legal committed prefix cannot be excluded") {
		for(uint x=2;x<=6;x++)REQUIRE(TryReserveRailTrack(TileXY(x,10),TRACK_X));
		REQUIRE(TrainReservationPassesThroughTrackdir(slow,TileXY(6,10),TRACKDIR_X_SW));
		CHECK_FALSE(AutoYieldRequestSplitAlternative(slow,slot,blocked));
		CHECK_FALSE(scope.ReplanForSplit());
		for(uint x=2;x<=6;x++)CHECK(GetReservedTrackbits(TileXY(x,10))==TRACK_BIT_X);
	}
	SECTION("foreign area, absent path and over-budget evidence are rejected") {
		CHECK_FALSE(AutoYieldRequestSplitAlternative(slow,slot+1,blocked));
		CHECK_FALSE(AutoYieldRequestSplitAlternative(slow,slot,{{TileXY(30,30),TRACKDIR_X_SW}}));
		CHECK_FALSE(AutoYieldRequestSplitAlternative(slow,slot,std::vector<std::pair<TileIndex,Trackdir>>(65,blocked.front())));
		CHECK_FALSE(scope.ReplanForSplit());
	}
	CHECK(random.Check()); CHECK(slow->early_route.empty());
}

TEST_CASE_METHOD(EarlyStopRouteFixture, "Native early stop route - legacy detector membership restores missing planning context without publishing a query")
{
	const bool registered = GENERATE(false, true);
	Place(slow, 7, 10); // Already beyond Approach, before the legal platform fork.
	if (registered) REQUIRE(membership->Occupy(slow));
	REQUIRE(slow->early_route.empty()); REQUIRE(slow->early_route_area == UINT16_MAX);
	const auto occupants = membership->occupants;
	GameRandomSeedChecker random;
	{
		AutoYieldNativeRouteScope scope(slow, slow->tile, TRACKDIR_X_SW);
		scope.PrepareFromContext(slow->tile, TRACKDIR_X_SW);
		CHECK(AutoYieldHasNativeRoute(slow) == registered);
		if (registered) {
			YapfRailRoute route;
			REQUIRE(AutoYieldGetNativeRoute(slow, membership->index.base(), slow->tile, TRACKDIR_X_SW, route));
			CHECK(route.path.back().tile == TileXY(27, 12));
		}
		scope.Finish(false);
	}
	CHECK(slow->early_route.empty()); CHECK(slow->early_route_area == UINT16_MAX);
	CHECK(membership->occupants == occupants); CHECK(GetReservedTrackbits(slow->tile) == TRACK_BIT_NONE);
	CHECK(random.Check()); CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(EarlyStopRouteFixture, "Native early route area - single-word clearance never reads the next instruction as registration")
{
	for (uint row : {10u, 12u}) {
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(27, row), TRACK_X), false);
		REQUIRE(program != nullptr);
		TraceRestrictInstructionItem penalty{}; penalty.SetType(TRIT_PF_PENALTY); penalty.SetValue(0);
		program->items.push_back(penalty.AsProgramItem()); REQUIRE(program->Validate().Succeeded());
	}
	InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	AutoYieldRouteArea original; REQUIRE(GetAutoYieldRouteArea(slow, membership->index.base(), original));
	const uint registration_bits = GENERATE(1u, 2u, 3u);
	auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(27, 12), TRACK_X), false);
	TraceRestrictInstructionItem penalty{program->items.back().base()}; penalty.SetValue(registration_bits << 10);
	program->items.back() = penalty.AsProgramItem(); REQUIRE(program->Validate().Succeeded());
	InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	AutoYieldRouteArea rebuilt; REQUIRE(GetAutoYieldRouteArea(slow, membership->index.base(), rebuilt));
	CHECK(rebuilt.clears == original.clears);
	CHECK(rebuilt.signature == original.signature);
}

TEST_CASE_METHOD(EarlyStopRouteFixture, "Native early route legacy context - success alone publishes the recovered route")
{
	Place(slow, 7, 10); REQUIRE(membership->Occupy(slow)); REQUIRE(TryReserveRailTrack(slow->tile, TRACK_X));
	bool found = false; PBSTileInfo target;
	{
		AutoYieldNativeRouteScope scope(slow, slow->tile, TRACKDIR_X_SW);
		scope.PrepareFromContext(slow->tile, TRACKDIR_X_SW);
		YapfTrainChooseTrack(slow, TileXY(8, 10), DiagDirection::SW, GetTrackBits(TileXY(8, 10)), found, true, &target, nullptr);
		REQUIRE(found); REQUIRE(target.okay); scope.Finish(true);
	}
	CHECK(slow->early_route_area == membership->index.base()); CHECK_FALSE(slow->early_route.empty());
	CHECK(TrainReservationPassesThroughTrackdir(slow, TileXY(13, 12), TRACKDIR_X_SW));
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(EarlyStopRouteFixture, "Native early route legacy context - multiple registered areas never choose an arbitrary one")
{
	Place(slow, 7, 10); REQUIRE(membership->Occupy(slow));
	auto *other = TraceRestrictSlot::Create(_current_company); other->max_occupancy = 16;
	for (uint row : {20u, 22u}) for (uint x = 1; x <= 40; x++) MakeRailNormal(TileXY(x, row), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
	for (auto [x, row, role] : {std::tuple{3u, 20u, AutoYieldRole::Approach}, {10u, 20u, AutoYieldRole::Main},
			{18u, 22u, AutoYieldRole::Exit}, {27u, 20u, AutoYieldRole::Clear}, {27u, 22u, AutoYieldRole::Clear}}) Marker(x, row, role, other->index.base());
	REQUIRE(other->Occupy(slow));
	AutoYieldRouteArea first, second;
	REQUIRE(GetAutoYieldRouteArea(slow, membership->index.base(), first)); REQUIRE(first.registered);
	REQUIRE(GetAutoYieldRouteArea(slow, other->index.base(), second)); REQUIRE(second.registered);
	AutoYieldNativeRouteScope scope(slow, slow->tile, TRACKDIR_X_SW);
	scope.PrepareFromContext(slow->tile, TRACKDIR_X_SW);
	CHECK_FALSE(AutoYieldHasNativeRoute(slow)); CHECK(slow->early_route_area == UINT16_MAX); CHECK(slow->early_route.empty());
	CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(EarlyStopRouteFixture, "Native early stop route - unresolved side effects preserve a valid current stop without inventing an exit")
{
	SECTION("a conditional departure is not evaluated in advance") {
		Order conditional; conditional.MakeConditional(0); conditional.SetConditionVariable(OrderConditionVariable::LoadPercentage);
		slow->orders->InsertOrderAt(std::move(conditional), 1);
	}
	SECTION("a slot action is not executed or skipped as if its effects were known") {
		Order release; release.MakeReleaseSlot(); release.SetDestination(membership->index);
		slow->orders->InsertOrderAt(std::move(release), 1);
		REQUIRE(membership->Occupy(slow));
	}
	const auto occupants = membership->occupants;
	const Order before = slow->current_order;
	GameRandomSeedChecker random;
	AutoYieldNativeRouteScope scope(slow, slow->tile, TRACKDIR_X_SW); Stage();
	CHECK_FALSE(AutoYieldNativeStopNeedsApproach(slow, membership->index.base(), TileXY(3, 10), TRACKDIR_X_SW));
	const auto selected = CurrentRoute();
	REQUIRE(selected.complete); CHECK(Has(selected, TileXY(13, 10)));
	CHECK(AutoYieldNativeRouteAllows(slow, TileXY(27, 10), TRACKDIR_X_SW));
	CHECK(AutoYieldNativeRouteAllows(slow, TileXY(27, 12), TRACKDIR_X_SW));
	YapfRailRoute fake;
	CHECK_FALSE(AutoYieldGetNativeRoute(slow, membership->index.base(), TileXY(3, 10), TRACKDIR_X_SW, fake));
	CHECK(slow->current_order.Equals(before)); CHECK(slow->cur_real_order_index == 0);
	CHECK(membership->occupants == occupants); CHECK(slow->early_route.empty());
	CHECK(random.Check()); CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(EarlyStopRouteFixture, "Native early stop route - successful deferred reservation retains context without inventing an exit")
{
	const bool slot_action = GENERATE(false, true);
	Order unresolved;
	if (slot_action) {
		unresolved.MakeReleaseSlot(); unresolved.SetDestination(membership->index);
	} else {
		unresolved.MakeConditional(0); unresolved.SetConditionVariable(OrderConditionVariable::LoadPercentage);
	}
	slow->orders->InsertOrderAt(std::move(unresolved), 1);
	REQUIRE(slow->early_route.empty()); REQUIRE(slow->early_route_area == UINT16_MAX);
	REQUIRE(TryReserveRailTrack(slow->tile, TRACK_X));
	const bool publish_success = GENERATE(false, true);
	GameRandomSeedChecker random;
	const Order before = slow->current_order;
	const auto before_index = slow->cur_real_order_index;
	bool found = false;
	PBSTileInfo target;
	{
		AutoYieldNativeRouteScope transaction(slow, slow->tile, TRACKDIR_X_SW);
		YapfTrainChooseTrack(slow, TileXY(8, 10), DiagDirection::SW,
				GetTrackBits(TileXY(8, 10)), found, true, &target, nullptr);
		REQUIRE(found); REQUIRE(target.okay);
		transaction.Finish(publish_success);
	}
	REQUIRE(membership->IsOccupant(slow->index));
	CHECK(TrainReservationPassesThroughTrackdir(slow, TileXY(3, 10), TRACKDIR_X_SW));
	CHECK(slow->early_route.empty()); // Neither conditional nor slot work chooses a future exit.
	CHECK(slow->early_route_area == (publish_success ? membership->index.base() : UINT16_MAX));
	CHECK(slow->current_order.Equals(before)); CHECK(slow->cur_real_order_index == before_index);
	const auto reservation = GetReservedTrackbits(TileXY(13, 10));
	REQUIRE(CmdRemoveVehicleTraceRestrictSlot(DoCommandFlag::Execute, membership->index, slow->index).Succeeded());
	CHECK(slow->early_route.empty()); CHECK(slow->early_route_area == UINT16_MAX);
	CHECK(GetReservedTrackbits(TileXY(13, 10)) == reservation);
	CHECK(random.Check()); CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(EarlyStopRouteFixture, "Native early stop route - unrelated native lookahead cannot commit a source corridor")
{
	Order unrelated; unrelated.MakeGoToStation(upper_goal->index);
	slow->orders->InsertOrderAt(Order(unrelated), 2);
	REQUIRE(TryReserveRailTrack(slow->tile, TRACK_X));
	GameRandomSeedChecker random;
	const Order source = slow->current_order;
	const auto source_index = slow->cur_real_order_index;
	AutoYieldNativeRouteScope transaction(slow, slow->tile, TRACKDIR_X_SW);
	Stage(); REQUIRE(AutoYieldHasNativeRoute(slow));
	CHECK_FALSE(AutoYieldNativeRouteAllows(slow, TileXY(13, 10), TRACKDIR_X_SW));
	bool found = false;
	PBSTileInfo target;
	/* Reproduce an actual native lookahead overlay, while the underlying
	 * source/next orders remain unchanged and therefore individually valid. */
	slow->current_order = unrelated; slow->cur_real_order_index = slow->cur_implicit_order_index = 2;
	slow->dest_tile = upper_goal->xy;
	{
		AutoYieldNativeRouteScope lookahead(slow, slow->tile, TRACKDIR_X_SW);
		REQUIRE_FALSE(AutoYieldHasNativeRoute(slow));
		YapfTrainChooseTrack(slow, TileXY(8, 10), DiagDirection::SW,
				GetTrackBits(TileXY(8, 10)), found, true, &target, nullptr);
		REQUIRE(found); REQUIRE(target.okay);
	}
	slow->current_order = source; slow->cur_real_order_index = slow->cur_implicit_order_index = source_index;
	slow->dest_tile = stop->xy;
	transaction.Finish(true);
	CHECK(slow->early_route.empty()); // Upper-goal PBS cannot become the stop-to-lower-goal plan.
	CHECK((GetReservedTrackbits(TileXY(13, 10)) & TRACK_BIT_X) != TRACK_BIT_NONE);
	CHECK(GetReservedTrackbits(TileXY(13, 12)) == TRACK_BIT_NONE);
	CHECK(slow->current_order.Equals(source)); CHECK(random.Check());
}

TEST_CASE_METHOD(EarlyStopRouteFixture, "Native early stop route - actual native reservation commits the selected platform and saves its next leg")
{
	AutoRestoreBackup<int8_t> native_cache_check(_debug_levels[size_t(DebugLevelID::desync)], GENERATE(int8_t{0}, int8_t{2}));
	GameRandomSeedChecker random;
	REQUIRE(TryReserveRailTrack(slow->tile, TRACK_X));
	bool found = false;
	PBSTileInfo target;
	{
		AutoYieldNativeRouteScope actual(slow, slow->tile, TRACKDIR_X_SW);
		const Track chosen = YapfTrainChooseTrack(slow, TileXY(8, 10), DiagDirection::SW,
				GetTrackBits(TileXY(8, 10)), found, true, &target, nullptr);
		/* The public native query returns its first segment after the actual
		 * reservation origin (2), not after the advisory tile argument (8). */
		REQUIRE(found); CHECK(chosen == TRACK_X);
		REQUIRE(target.okay);
		actual.Finish(target.okay);
	}
	REQUIRE_FALSE(slow->early_route.empty());
	CHECK(GetReservedTrackbits(TileXY(13, 10)) == TRACK_BIT_NONE);
	CHECK((GetReservedTrackbits(TileXY(8, 10)) & TRACK_BIT_RIGHT) != TRACK_BIT_NONE);
	CHECK((GetReservedTrackbits(TileXY(13, 12)) & TRACK_BIT_X) != TRACK_BIT_NONE);
	const auto saved = slow->early_route;
	SECTION("a new leg after Main retains its proved continuous platform approach") {
		slow->early_route.clear();
		const auto reservation = GetReservedTrackbits(TileXY(8, 10));
		AutoYieldNativeRouteScope scope(slow, TileXY(8, 11), TRACKDIR_Y_SE);
		AutoYieldStageNativeRoute(slow, TileXY(8, 11), TRACKDIR_Y_SE, membership->index.base(), true);
		REQUIRE(AutoYieldHasNativeRoute(slow));
		YapfRailRoute full;
		REQUIRE(AutoYieldGetNativeRoute(slow, membership->index.base(), slow->tile, TRACKDIR_X_SW, full));
		CHECK(full.path.front().tile == slow->tile); CHECK(full.path.back().tile == TileXY(27, 12));
		CHECK(AutoYieldNativeStopNeedsApproach(slow, membership->index.base(), TileXY(3, 10), TRACKDIR_X_SW));
		CHECK_FALSE(AutoYieldNativeStopNeedsApproach(slow, membership->index.base(), TileXY(18, 10), TRACKDIR_X_SW));
		CHECK(GetReservedTrackbits(TileXY(8, 10)) == reservation);
	}
	SECTION("a registry rebuild keeps the same decision") {
		ResetAutoYieldAreas(); UpdateAutoYieldAreas();
		AutoYieldNativeRouteScope scope(slow, slow->tile, TRACKDIR_X_SW);
		CHECK(AutoYieldHasNativeRoute(slow));
		CHECK_FALSE(AutoYieldNativeRouteAllows(slow, TileXY(8, 10), TRACKDIR_X_SW));
		const auto restored = CurrentRoute();
		REQUIRE(restored.complete); CHECK(Has(restored, TileXY(13, 12))); CHECK_FALSE(Has(restored, TileXY(13, 10)));
		CHECK(slow->early_route == saved);
	}
	SECTION("a completed stop uses the saved onward route") {
		slow->current_order = *slow->GetOrder(1); slow->cur_real_order_index = slow->cur_implicit_order_index = 1;
		slow->dest_tile = lower_goal->xy;
		AutoYieldNativeRouteScope scope(slow, TileXY(15, 12), TRACKDIR_X_SW);
		CHECK(AutoYieldHasNativeRoute(slow));
		YapfRailRoute onward;
		REQUIRE(AutoYieldGetNativeRoute(slow, membership->index.base(), TileXY(15, 12), TRACKDIR_X_SW, onward));
		CHECK(onward.path.back().tile == TileXY(27, 12));
		CHECK_FALSE(AutoYieldNativeRouteAllows(slow, TileXY(27, 10), TRACKDIR_X_SW));
	}
	SECTION("a changed next destination invalidates the old internal decision") {
		slow->GetOrder(1)->SetDestination(upper_goal->index);
		const auto before = GetReservedTrackbits(TileXY(13, 12));
		AutoYieldNativeRouteScope scope(slow, slow->tile, TRACKDIR_X_SW);
		CHECK_FALSE(AutoYieldHasNativeRoute(slow));
		CHECK_FALSE(AutoYieldNativeStopNeedsApproach(slow, membership->index.base(), TileXY(3, 10), TRACKDIR_X_SW));
		CHECK(GetReservedTrackbits(TileXY(13, 12)) == before);
	}
	SECTION("native lookahead at an unrelated order cannot reuse the old conflict proof") {
		AutoYieldNativeRouteScope source(slow, slow->tile, TRACKDIR_X_SW);
		REQUIRE(AutoYieldHasNativeRoute(slow));
		const Order original = slow->current_order;
		slow->current_order.MakeGoToStation(upper_goal->index);
		{
			AutoYieldNativeRouteScope unrelated(slow, slow->tile, TRACKDIR_X_SW);
			REQUIRE_FALSE(AutoYieldHasNativeRoute(slow));
			YapfRailRoute stale;
			CHECK_FALSE(AutoYieldGetNativeRoute(slow, membership->index.base(), TileXY(3, 10), TRACKDIR_X_SW, stale));
		}
		slow->current_order = original;
		AutoYieldNativeRouteScope restored(slow, slow->tile, TRACKDIR_X_SW);
		CHECK(AutoYieldHasNativeRoute(slow));
	}
	SECTION("a changed stop location invalidates a departure planned for the old stop") {
		const auto old = slow->GetOrder(0)->GetStopLocation();
		slow->GetOrder(0)->SetStopLocation(old == OrderStopLocation::FarEnd ? OrderStopLocation::NearEnd : OrderStopLocation::FarEnd);
		AutoYieldNativeRouteScope scope(slow, slow->tile, TRACKDIR_X_SW);
		CHECK_FALSE(AutoYieldHasNativeRoute(slow));
	}
	SECTION("new decoupling work invalidates the old continuation") {
		slow->GetOrder(0)->SetDecouple(ODF_DECOUPLE);
		slow->GetOrder(0)->SetNumDecouple(1);
		AutoYieldNativeRouteScope scope(slow, slow->tile, TRACKDIR_X_SW);
		CHECK_FALSE(AutoYieldHasNativeRoute(slow));
	}
	SECTION("a changed order speed invalidates the old native cost decision") {
		slow->GetOrder(1)->SetMaxSpeed(80);
		AutoYieldNativeRouteScope scope(slow, slow->tile, TRACKDIR_X_SW);
		CHECK_FALSE(AutoYieldHasNativeRoute(slow));
	}
	SECTION("reversing the live front invalidates the old directed corridor") {
		slow->direction = Direction::NE;
		AutoYieldNativeRouteScope scope(slow, slow->tile, TRACKDIR_X_NE);
		CHECK_FALSE(AutoYieldHasNativeRoute(slow));
	}
	SECTION("manual detector cancellation discards only the internal plan") {
		REQUIRE(membership->IsOccupant(slow->index));
		const auto before = GetReservedTrackbits(TileXY(13, 12));
		REQUIRE(CmdRemoveVehicleTraceRestrictSlot(DoCommandFlag::Execute, membership->index, slow->index).Succeeded());
		CHECK(slow->early_route.empty()); CHECK(GetReservedTrackbits(TileXY(13, 12)) == before);
	}
	SECTION("a network snapshot includes the route feature and per-train state") {
		AutoRestoreBackup format(_savegame_format, std::string{"none"});
		AutoRestoreBackup cursor(_cursor.sprites, std::vector<CursorSprite>{{0, 0, 0, 0}});
		auto snapshot = std::make_shared<CoupleSnapshotCapture>();
		REQUIRE(SaveWithFilter(snapshot, false, SMF_NET_SERVER) == SaveLoadResult::Ok);
		for (const std::string &name : {std::string{"pxp_auto_yield_route_plan"}, std::string{"early_route"}}) {
			CHECK(std::search(snapshot->bytes.begin(), snapshot->bytes.end(), name.begin(), name.end()) != snapshot->bytes.end());
		}
		CHECK(slow->early_route == saved);
	}
	SECTION("nearby layout edits invalidate only the plan") {
		const auto before = GetReservedTrackbits(TileXY(13, 12));
		AutoYieldInvalidateNativeRoutes(TileXY(14, 13));
		CHECK(slow->early_route.empty()); CHECK(GetReservedTrackbits(TileXY(13, 12)) == before);
		const auto owned = FollowTrainReservation(slow, nullptr, FollowTrainReservationFlag::OkayUnused);
		const auto order = slow->current_order;
		REQUIRE(slow->early_route_area == membership->index.base());
		AutoYieldNativeRouteScope restore(slow, slow->tile, TRACKDIR_X_SW);
		restore.PrepareFromContext(owned.tile, owned.trackdir);
		/* Restoring proof before the PBS quick check is not a committed route. */
		CHECK(slow->early_route.empty());
		CHECK(slow->current_order.Equals(order));
		const auto unchanged = FollowTrainReservation(slow, nullptr, FollowTrainReservationFlag::OkayUnused);
		CHECK(unchanged.tile == owned.tile); CHECK(unchanged.trackdir == owned.trackdir);
		CHECK(GetReservedTrackbits(TileXY(13, 12)) == before);
	}
	SECTION("unrelated layout edits preserve the plan") {
		AutoYieldInvalidateNativeRoutes(TileXY(58, 55));
		CHECK(slow->early_route == saved);
	}
	CHECK(random.Check()); CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(EarlyStopRouteFixture, "Native early stop route - edited intermediate control flow invalidates a saved departure")
{
	Order jump; jump.MakeConditional(2); jump.SetConditionVariable(OrderConditionVariable::Unconditionally);
	slow->orders->InsertOrderAt(std::move(jump), 1);
	Order alternative; alternative.MakeGoToStation(upper_goal->index);
	slow->orders->InsertOrderAt(std::move(alternative), 3);
	REQUIRE(TryReserveRailTrack(slow->tile, TRACK_X));
	bool found = false;
	PBSTileInfo target;
	{
		AutoYieldNativeRouteScope actual(slow, slow->tile, TRACKDIR_X_SW);
		YapfTrainChooseTrack(slow, TileXY(8, 10), DiagDirection::SW,
				GetTrackBits(TileXY(8, 10)), found, true, &target, nullptr);
		REQUIRE(found); REQUIRE(target.okay); actual.Finish(target.okay);
	}
	REQUIRE_FALSE(slow->early_route.empty());
	{
		AutoYieldNativeRouteScope before(slow, slow->tile, TRACKDIR_X_SW);
		REQUIRE(AutoYieldHasNativeRoute(slow));
		CHECK_FALSE(AutoYieldNativeRouteAllows(slow, TileXY(27, 10), TRACKDIR_X_SW));
	}
	const auto reservation = GetReservedTrackbits(TileXY(13, 12));
	const auto original_order = slow->current_order;
	GameRandomSeedChecker random;
	SECTION("an unconditional jump now reaches a different target") {
		slow->GetOrder(1)->SetConditionSkipToOrder(3);
	}
	SECTION("a formerly unconditional jump now depends on live state") {
		slow->GetOrder(1)->SetConditionVariable(OrderConditionVariable::LoadPercentage);
	}
	AutoYieldNativeRouteScope after(slow, slow->tile, TRACKDIR_X_SW);
	CHECK_FALSE(AutoYieldHasNativeRoute(slow));
	CHECK(GetReservedTrackbits(TileXY(13, 12)) == reservation);
	CHECK(slow->current_order.Equals(original_order)); CHECK(random.Check());
}

TEST_CASE_METHOD(EarlyStopRouteFixture, "Native early stop route - future queries restore an outer loading context and per-part callback caches")
{
	/* The outer caller can be doing reservation lookahead while loading. The
	 * future GOTO query must not inherit that outer TR Loading value. */
	Order loading; loading.MakeLoading(true);
	AutoRestoreBackup outer(_choose_train_track_saved_current_order, static_cast<const Order *>(&loading));
	TileIndex signal = TileXY(23, 12);
	MakeRailNormal(signal, _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
	SB(_m[signal].m5, 6, 2, to_underlying(RailTileType::Signals));
	SetPresentSignals(signal, SignalAlongTrackdir(TRACKDIR_X_SW));
	SetSignalType(signal, TRACK_X, SignalType::Path); SetSignalStateByTrackdir(signal, TRACKDIR_X_SW, SignalState::Green);
	auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(signal, TRACK_X), true);
	TraceRestrictInstructionItem condition{}; condition.SetType(TRIT_COND_TRAIN_STATUS); condition.SetValue(TRTSVF_LOADING); condition.SetCondOp(TRCO_IS);
	TraceRestrictInstructionItem deny{}; deny.SetType(TRIT_PF_DENY);
	TraceRestrictInstructionItem end{}; end.SetType(TRIT_COND_ENDIF);
	program->items = {condition.AsProgramItem(), deny.AsProgramItem(), end.AsProgramItem()}; REQUIRE(program->Validate().Succeeded());
	SetRestrictedSignal(signal, true);
	InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	Train *tail = MakeTrain(); slow->SetNext(tail);
	slow->gcache.cached_total_length = 2 * VEHICLE_LENGTH;
	tail->owner = slow->owner; tail->track = TRACK_BIT_X; tail->direction = Direction::SW;
	Place(tail, 1, 10);
	slow->grf_cache.position_consist_length = 0x12345678u; slow->grf_cache.cache_valid = 7;
	tail->grf_cache.position_consist_length = 0x87654321u; tail->grf_cache.cache_valid = 3;
	slow->tcache.user_def_data = 21; tail->tcache.user_def_data = 43;
	slow->vcache.cached_veh_flags = 3; tail->vcache.cached_veh_flags = 5;
	const auto head_cache = slow->grf_cache, tail_cache = tail->grf_cache;
	const auto head_flags = slow->vcache.cached_veh_flags, tail_flags = tail->vcache.cached_veh_flags;
	const auto old_order = slow->current_order;
	const auto old_destination = slow->dest_tile;
	const auto old_last_station = slow->last_station_visited;
	const auto real = slow->cur_real_order_index, implicit = slow->cur_implicit_order_index, timetable = slow->cur_timetable_order_index;
	GameRandomSeedChecker random;
	AutoYieldNativeRouteScope scope(slow, slow->tile, TRACKDIR_X_SW); Stage();
	REQUIRE(AutoYieldHasNativeRoute(slow));
	CHECK_FALSE(AutoYieldNativeRouteAllows(slow, TileXY(13, 10), TRACKDIR_X_SW));
	CHECK(AutoYieldNativeRouteAllows(slow, TileXY(13, 12), TRACKDIR_X_SW));
	CHECK(_choose_train_track_saved_current_order == &loading);
	CHECK(slow->current_order.Equals(old_order)); CHECK(slow->dest_tile == old_destination); CHECK(slow->last_station_visited == old_last_station);
	CHECK(slow->cur_real_order_index == real); CHECK(slow->cur_implicit_order_index == implicit); CHECK(slow->cur_timetable_order_index == timetable);
	CHECK(slow->grf_cache == head_cache); CHECK(tail->grf_cache == tail_cache);
	CHECK(slow->tcache.user_def_data == 21); CHECK(tail->tcache.user_def_data == 43);
	CHECK(slow->vcache.cached_veh_flags == head_flags); CHECK(tail->vcache.cached_veh_flags == tail_flags);
	CHECK(random.Check()); CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}

TEST_CASE_METHOD(EarlyStopRouteFixture, "Native early stop route - a failed PBS transaction cannot publish its planned corridor")
{
	/* The only forward-compatible platform is presently reserved. It remains
	 * a candidate for a later retry, but this train cannot acquire it now or
	 * silently enter the incompatible platform merely to make progress. */
	for (uint x = 12; x <= 15; x++) SetRailStationReservation(TileXY(x, 12), true);
	REQUIRE(TryReserveRailTrack(slow->tile, TRACK_X));
	bool found = false;
	PBSTileInfo target;
	GameRandomSeedChecker random;
	{
		AutoYieldNativeRouteScope actual(slow, slow->tile, TRACKDIR_X_SW);
		YapfTrainChooseTrack(slow, TileXY(8, 10), DiagDirection::SW,
				GetTrackBits(TileXY(8, 10)), found, true, &target, nullptr);
		actual.Finish(target.okay);
	}
	CHECK_FALSE(target.okay);
	CHECK(slow->early_route.empty());
	for (uint x = 12; x <= 15; x++) {
		CHECK(HasStationReservation(TileXY(x, 12)));
		CHECK_FALSE(HasStationReservation(TileXY(x, 10)));
	}
	CHECK((GetReservedTrackbits(slow->tile) & TRACK_BIT_X) != TRACK_BIT_NONE);
	CHECK(random.Check()); CHECK(TraceRestrictSlot::ValidateVehicleIndex());
}
