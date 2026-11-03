/* Included after AutomaticYieldStationRouteFixture in train_decouple.cpp.
 * A proved stop inside one candidate must not require a forecast of work
 * performed after that stop. The other candidate is an ordinary siding. */
TEST_CASE_METHOD(AutomaticYieldStationRouteFixture, "Automatic yield mixed platform - an available current stop does not require a predicted departure")
{
	const bool exit_only = GENERATE(false, true);
	const bool multiple_clearances = GENERATE(false, true);
	if (exit_only) UseExitOnly();
	UpperSiding(exit_only);
	Station *station = StationAt(14, 12);
	Marker(18, 12, AutoYieldRole::Exit, membership->index.base(), TEST_YIELD_CONFIG | AUTO_YIELD_PLATFORM);
	if (!multiple_clearances) {
		GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(27, 6), TRACK_X), false)->items.clear();
		InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	}
	/* Establish a genuine mixed layout: distinct platform and ordinary lane
	 * claims, an actual station only on the platform, and both candidates free. */
	const auto platform_ref = MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10, 12), TRACK_X);
	const auto ordinary_ref = MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10, 8), TRACK_X);
	REQUIRE(Resource(platform_ref) != nullptr);
	REQUIRE(Resource(ordinary_ref) != nullptr);
	REQUIRE(Resource(platform_ref) != Resource(ordinary_ref));
	REQUIRE(Resource(platform_ref)->occupants.empty());
	REQUIRE(Resource(ordinary_ref)->occupants.empty());
	REQUIRE(IsRailStationTile(TileXY(14, 12)));
	REQUIRE(IsPlainRailTile(TileXY(14, 8)));
	Order first; first.MakeGoToStation(station->index);
	slow->orders = OrderList::Create(Order(first), slow); slow->primary_order = slow->orders->index;
	slow->current_order = first; slow->dest_tile = station->xy;
	std::string_view continuation;
	SECTION("the current stop is the only order") { continuation = "single"; }
	SECTION("the next physical destination remains predictable") {
		continuation = "destination";
		Order next; next.MakeGoToStation(common_goal->index); slow->orders->InsertOrderAt(std::move(next), 1);
	}
	SECTION("a conditional after the stop cannot be forecast") {
		continuation = "conditional";
		Order next; next.MakeConditional(0); next.SetConditionVariable(OrderConditionVariable::LoadPercentage);
		slow->orders->InsertOrderAt(std::move(next), 1);
	}
	SECTION("coupling work after the stop cannot be forecast") {
		continuation = "waitcouple";
		Order next; next.MakeWaitCouple(); slow->orders->InsertOrderAt(std::move(next), 1);
	}
	const auto current = slow->current_order;
	const auto destination = slow->dest_tile;
	const auto real_index = slow->cur_real_order_index, implicit_index = slow->cur_implicit_order_index;
	const auto order_count = slow->GetNumOrders();
	std::vector<Order> saved_orders;
	for (VehicleOrderID i = 0; i < order_count; i++) saved_orders.push_back(*slow->GetOrder(i));
	std::vector<std::pair<const TraceRestrictSlot *, decltype(membership->occupants)>> saved_slots;
	for (const auto *slot : TraceRestrictSlot::Iterate()) saved_slots.emplace_back(slot, slot->occupants);
	std::vector<std::pair<TileIndex, TrackBits>> reservations;
	for (uint y : {6u, 8u, 10u, 12u}) for (uint x = 1; x <= 40; x++) {
		const auto tile = TileXY(x, y);
		if (IsPlainRailTile(tile) || IsRailStationTile(tile)) reservations.emplace_back(tile, GetReservedTrackbits(tile));
	}
	GameRandomSeedChecker random;
	CAPTURE(exit_only, multiple_clearances, continuation);
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	CHECK(slow->current_order.Equals(current));
	CHECK(slow->dest_tile == destination);
	CHECK(slow->cur_real_order_index == real_index); CHECK(slow->cur_implicit_order_index == implicit_index);
	CHECK(slow->GetNumOrders() == order_count);
	for (VehicleOrderID i = 0; i < order_count; i++) CHECK(slow->GetOrder(i)->Equals(saved_orders[i]));
	CHECK(TraceRestrictSlot::GetNumItems() == saved_slots.size());
	for (const auto &[slot, occupants] : saved_slots) CHECK(slot->occupants == occupants);
	for (const auto &[tile, tracks] : reservations) CHECK(GetReservedTrackbits(tile) == tracks);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random.Check());
}

TEST_CASE_METHOD(AutomaticYieldStationRouteFixture, "Automatic yield mixed platform - current stop never admits a dead exit or the wrong known destination")
{
	const bool exit_only = GENERATE(false, true);
	if (exit_only) UseExitOnly();
	UpperSiding(exit_only);
	Station *station = StationAt(14, 12);
	Marker(18, 12, AutoYieldRole::Exit, membership->index.base(), TEST_YIELD_CONFIG | AUTO_YIELD_PLATFORM);
	const auto platform_ref = MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10, 12), TRACK_X);
	const auto ordinary_ref = MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10, 8), TRACK_X);
	REQUIRE(Resource(platform_ref) != nullptr); REQUIRE(Resource(ordinary_ref) != nullptr);
	REQUIRE(Resource(platform_ref) != Resource(ordinary_ref));
	REQUIRE(IsRailStationTile(TileXY(14, 12))); REQUIRE(IsPlainRailTile(TileXY(14, 8)));
	Order first; first.MakeGoToStation(station->index);
	slow->orders = OrderList::Create(Order(first), slow); slow->primary_order = slow->orders->index;
	slow->current_order = first; slow->dest_tile = station->xy;
	std::string_view rejection;
	SECTION("the platform is reachable but disconnected from every forward Clear") {
		rejection = "disconnected";
		MakeRailNormal(TileXY(19, 12), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
	}
	SECTION("a one-way signal blocks the platform's actual departure") {
		rejection = "one-way";
		BackSignal(19, SignalType::PathOneWay);
	}
	SECTION("the known next station only belongs to another outlet") {
		rejection = "known-other-outlet";
		Order next; next.MakeGoToStation(separate_goal->index); slow->orders->InsertOrderAt(std::move(next), 1);
		/* The upper independent main serves this next stop; the candidate
		 * platform's first Clear does not. Neither test may switch orders. */
		uint used = 0;
		REQUIRE(YapfTrainCanReachStationBounded(slow, TileXY(27, 6), TRACKDIR_X_SW, separate_goal->index, 32, used));
		REQUIRE_FALSE(YapfTrainCanReachStationBounded(slow, TileXY(27, 10), TRACKDIR_X_SW, separate_goal->index, 32, used));
	}
	InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	const auto current = slow->current_order;
	std::vector<Order> orders;
	for (VehicleOrderID i = 0; i < slow->GetNumOrders(); i++) orders.push_back(*slow->GetOrder(i));
	GameRandomSeedChecker random;
	CAPTURE(exit_only, rejection);
	CHECK_FALSE(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding));
	CHECK(slow->current_order.Equals(current)); CHECK(slow->cur_real_order_index == 0); CHECK(slow->cur_implicit_order_index == 0);
	REQUIRE(slow->GetNumOrders() == orders.size());
	for (VehicleOrderID i = 0; i < slow->GetNumOrders(); i++) CHECK(slow->GetOrder(i)->Equals(orders[i]));
	CHECK(Resource(platform_ref)->occupants.empty()); CHECK(Resource(ordinary_ref)->occupants.empty());
	for (uint x : {14u, 18u, 19u}) CHECK(GetReservedTrackbits(TileXY(x, 12)) == TRACK_BIT_NONE);
	CHECK(GetReservedTrackbits(TileXY(27, 10)) == TRACK_BIT_NONE);
	CHECK(GetReservedTrackbits(TileXY(27, 6)) == TRACK_BIT_NONE);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random.Check());
}

TEST_CASE_METHOD(AutomaticYieldStationRouteFixture, "Automatic yield mixed platform - a single lane retains its existing admission behaviour")
{
	const bool exit_only = GENERATE(false, true);
	const bool multiple_clearances = GENERATE(false, true);
	if (exit_only) UseExitOnly();
	if (!multiple_clearances) {
		GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(27, 6), TRACK_X), false)->items.clear();
		InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
	}
	const auto lane_ref = MakeTraceRestrictRefId(TileXY(exit_only ? 18 : 10, 12), TRACK_X);
	REQUIRE(Resource(lane_ref) != nullptr);
	/* No UpperSiding: this area must contain exactly one claimable lane. */
	uint lanes = 0;
	for (const auto *slot : TraceRestrictSlot::Iterate()) {
		if (slot->automatic_yield_area == membership->index.base() + 1 &&
				!slot->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets) && slot->automatic_yield_signal != AUTO_YIELD_PENDING_REF) lanes++;
	}
	REQUIRE(lanes == 1);
	bool expected_free = true;
	std::string_view continuation;
	SECTION("an ordinary single siding retains its through destination") { continuation = "ordinary"; }
	SECTION("a single platform retains the previous forecast requirement") {
		Station *station = StationAt(14, 12);
		Marker(18, 12, AutoYieldRole::Exit, membership->index.base(), TEST_YIELD_CONFIG | AUTO_YIELD_PLATFORM);
		Order first; first.MakeGoToStation(station->index);
		slow->orders = OrderList::Create(Order(first), slow); slow->primary_order = slow->orders->index;
		slow->current_order = first; slow->dest_tile = station->xy;
		expected_free = !multiple_clearances;
		SECTION("only the current stop") { continuation = "single"; }
		SECTION("a known onward destination") {
			continuation = "destination"; expected_free = true;
			Order next; next.MakeGoToStation(common_goal->index); slow->orders->InsertOrderAt(std::move(next), 1);
		}
		SECTION("a conditional continuation") {
			continuation = "conditional";
			Order next; next.MakeConditional(0); next.SetConditionVariable(OrderConditionVariable::LoadPercentage);
			slow->orders->InsertOrderAt(std::move(next), 1);
		}
		SECTION("coupling work after the stop") {
			continuation = "waitcouple";
			Order next; next.MakeWaitCouple(); slow->orders->InsertOrderAt(std::move(next), 1);
		}
	}
	const auto current = slow->current_order;
	GameRandomSeedChecker random;
	CAPTURE(exit_only, multiple_clearances, continuation, expected_free);
	CHECK(EvaluateAutoYield(slow, membership->index.base(), AutoYieldCondition::FreeSiding) == expected_free);
	CHECK(slow->current_order.Equals(current));
	CHECK(Resource(lane_ref)->occupants.empty());
	CHECK(GetReservedTrackbits(TileXY(18, 12)) == TRACK_BIT_NONE);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random.Check());
}
