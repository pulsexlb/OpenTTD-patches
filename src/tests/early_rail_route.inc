/* Included after AutomaticYieldFixture in train_decouple.cpp. These tests
 * exercise the native route query, not the automatic-yield decision policy. */
#include "../pathfinder/yapf/yapf_cache.h"
#include "../tracerestrict_early_route.h"

class EarlyRailRouteFixture : public AutomaticYieldFixture {
protected:
	AutoRestoreBackup<uint32_t> native_lookahead{_settings_game.pf.yapf.rail_look_ahead_max_signals, 10};
	Station *goal;
	EarlyRailRouteFixture()
	{
		/* Keep the real track forks and PBS signals, but isolate native route
		 * selection from unrelated automatic-yield decisions in this fixture. */
		ClearTraceRestrictMapping();
		_tracerestrictprogram_pool.CleanPool();
		InvalidateAutoYieldAreas(); UpdateAutoYieldAreas();
		for (uint y : {10u, 12u}) for (uint x = 1; x <= 40; x++) {
			const TileIndex tile = TileXY(x, y);
			if (IsPlainRailTile(tile) && HasSignalOnTrack(tile, TRACK_X)) SetSignalStateByTrackdir(tile, TRACKDIR_X_SW, SignalState::Green);
		}
		goal = StationAt(35, 10);
		slow->current_order.MakeGoToStation(goal->index); slow->dest_tile = goal->xy;
		YapfNotifyTrackLayoutChange(INVALID_TILE, INVALID_TRACK);
	}
	Station *StationAt(uint x, uint y, uint length = 1)
	{
		Station *station = Station::Create(TileXY(x, y));
		station->owner = _current_company; station->facilities = {StationFacility::Train};
		station->train_station = TileArea(station->xy, length, 1);
		for (uint i = 0; i < length; i++) {
			const TileIndex tile = TileXY(x + i, y);
			station->rect.BeforeAddTile(tile, StationRect::ADD_FORCE);
			MakeRailStation(tile, _current_company, station->index, Axis::X, 0, RAILTYPE_RAIL);
		}
		return station;
	}
	static bool Contains(const YapfRailRoute &route, TileIndex tile)
	{
		return std::any_of(route.path.begin(), route.path.end(), [tile](const YapfRailRouteStep &step) { return step.tile == tile; });
	}
	static void CheckSameRoute(const YapfRailRoute &a, const YapfRailRoute &b)
	{
		CHECK(a.complete == b.complete); CHECK(a.reason == b.reason); CHECK(a.nodes == b.nodes); CHECK(a.cost == b.cost);
		REQUIRE(a.path.size() == b.path.size());
		for (size_t i = 0; i < a.path.size(); i++) {
			CHECK(a.path[i].tile == b.path[i].tile); CHECK(a.path[i].trackdir == b.path[i].trackdir);
			CHECK(a.path[i].wormhole_from == b.path[i].wormhole_from);
			CHECK(a.path[i].wormhole_from_trackdir == b.path[i].wormhole_from_trackdir);
			CHECK(a.path[i].wormhole_tiles == b.path[i].wormhole_tiles);
		}
	}
};

TEST_CASE_METHOD(EarlyRailRouteFixture, "Native early rail route - actual cost selects the same fork without reserving or changing the train")
{
	AutoRestoreBackup turns(_settings_game.pf.forbid_90_deg, GENERATE(false, true));
	const bool costly_main = GENERATE(false, true);
	if (costly_main) {
		/* A normal native restriction cost makes the longer branch preferable.
		 * The route API must use that cost, not choose an exit geometrically. */
		auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(10, 10), TRACK_X), true);
		TraceRestrictInstructionItem penalty{}; penalty.SetType(TRIT_PF_PENALTY); penalty.SetAuxField(TRPPAF_VALUE); penalty.SetValue(5000);
		program->items = {penalty.AsProgramItem()}; REQUIRE(program->Validate().Succeeded());
		SetRestrictedSignal(TileXY(10, 10), true);
		YapfNotifyTrackLayoutChange(INVALID_TILE, INVALID_TRACK);
	}
	const Order order = slow->current_order;
	const auto destination = slow->dest_tile, position = slow->tile;
	const auto real_index = slow->cur_real_order_index, implicit_index = slow->cur_implicit_order_index;
	const auto flags = slow->flags;
	const auto vehicle_flags = slow->vehicle_flags;
	const auto direction = slow->direction;
	std::vector<std::pair<TileIndex, TrackBits>> reservations;
	for (uint y : {10u, 11u, 12u}) for (uint x = 1; x <= 40; x++) {
		const TileIndex tile = TileXY(x, y);
		if (IsPlainRailTile(tile) || HasStationTileRail(tile)) reservations.emplace_back(tile, GetReservedTrackbits(tile));
	}
	std::vector<std::pair<const TraceRestrictSlot *, decltype(membership->occupants)>> occupants;
	for (const auto *slot : TraceRestrictSlot::Iterate()) occupants.emplace_back(slot, slot->occupants);
	GameRandomSeedChecker random;
	const auto plan = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256);
	REQUIRE(plan.complete); REQUIRE(plan.reason == YapfRailRouteReason::Complete); REQUIRE(plan.path.size() > 2);
	CHECK(plan.cost > 0); CHECK(plan.nodes > 0); CHECK(plan.nodes <= 256);
	CHECK(plan.path.front().tile == slow->tile); CHECK(plan.path.front().trackdir == TRACKDIR_X_SW);
	CHECK(plan.path.back().tile == goal->xy); CHECK(plan.path.back().trackdir == TRACKDIR_X_SW);
	CHECK(Contains(plan, TileXY(14, 12)) == costly_main);
	CHECK(Contains(plan, TileXY(14, 10)) == !costly_main);
	if (costly_main) {
		for (uint x = 9; x <= 19; x++) CHECK(Contains(plan, TileXY(x, 12)));
	} else {
		for (uint x = 7; x <= 35; x++) CHECK(Contains(plan, TileXY(x, 10)));
	}
	const auto alternative = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256,
			[costly_main](TileIndex tile, Trackdir) { return tile != TileXY(14, costly_main ? 12 : 10); });
	REQUIRE(alternative.complete); CHECK(plan.cost < alternative.cost);
	bool found = false;
	const Track chosen = YapfTrainChooseTrack(slow, TileXY(8, 10), DiagDirection::SW,
			GetTrackBits(TileXY(8, 10)), found, false, nullptr, nullptr);
	REQUIRE(found);
	CHECK(chosen == TrackdirToTrack(plan.path[1].trackdir));
	CHECK((chosen == TRACK_X) == !costly_main);
	CHECK(slow->current_order.Equals(order)); CHECK(slow->dest_tile == destination); CHECK(slow->tile == position);
	CHECK(slow->cur_real_order_index == real_index); CHECK(slow->cur_implicit_order_index == implicit_index);
	CHECK(slow->flags == flags); CHECK(slow->vehicle_flags == vehicle_flags); CHECK(slow->direction == direction);
	for (const auto &[tile, tracks] : reservations) CHECK(GetReservedTrackbits(tile) == tracks);
	REQUIRE(TraceRestrictSlot::GetNumItems() == occupants.size());
	for (const auto &[slot, saved] : occupants) CHECK(slot->occupants == saved);
	CHECK(TraceRestrictSlot::ValidateVehicleIndex()); CHECK(random.Check());
}

TEST_CASE_METHOD(EarlyRailRouteFixture, "Native early rail route - a directed plain-track target closes the actual segment")
{
	const TileIndex target = TileXY(11, 10);
	GameRandomSeedChecker random;
	const auto plan = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256, {}, target, TRACKDIR_X_SW);
	REQUIRE(plan.complete); REQUIRE_FALSE(plan.path.empty());
	CHECK(plan.path.back().tile == target); CHECK(plan.path.back().trackdir == TRACKDIR_X_SW);
	CHECK_FALSE(Contains(plan, goal->xy)); CHECK(plan.cost > 0);
	for (const auto &step : plan.path) CHECK(GetReservedTrackbits(step.tile) == TRACK_BIT_NONE);
	const auto rejected = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256,
			[target](TileIndex tile, Trackdir) { return tile != target; }, target, TRACKDIR_X_SW);
	CHECK_FALSE(rejected.complete); CHECK(rejected.reason == YapfRailRouteReason::NoPath);
	CHECK(rejected.path.empty()); CHECK(rejected.cost == -1); CHECK(random.Check());
}

TEST_CASE_METHOD(EarlyRailRouteFixture, "Native early rail route - skipped middle platform resources are filtered and exported")
{
	const bool target_platform = GENERATE(false, true);
	Station *platform = StationAt(12, 10, 4);
	for (uint x = 12; x <= 15; x++) {
		platform->rect.BeforeAddTile(TileXY(x, 12), StationRect::ADD_FORCE);
		MakeRailStation(TileXY(x, 12), _current_company, platform->index, Axis::X, 0, RAILTYPE_RAIL);
	}
	if (target_platform) { slow->current_order.MakeGoToStation(platform->index); slow->dest_tile = platform->xy; }
	YapfNotifyTrackLayoutChange(INVALID_TILE, INVALID_TRACK);
	GameRandomSeedChecker random;
	/* Reject only a compressed interior tile, not either platform endpoint.
	 * This must also work when the station is merely an intermediate stop. */
	const auto plan = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256,
			[](TileIndex tile, Trackdir) { return tile != TileXY(13, 10); });
	REQUIRE(plan.complete);
	for (uint x = 12; x <= 15; x++) {
		CHECK_FALSE(Contains(plan, TileXY(x, 10)));
		CHECK(Contains(plan, TileXY(x, 12)));
		CHECK(GetReservedTrackbits(TileXY(x, 10)) == TRACK_BIT_NONE);
		CHECK(GetReservedTrackbits(TileXY(x, 12)) == TRACK_BIT_NONE);
	}
	CHECK(plan.path.back().tile == (target_platform ? TileXY(15, 12) : goal->xy));
	const auto interior = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256, {}, TileXY(13, 12), TRACKDIR_X_SW);
	CHECK_FALSE(interior.complete); CHECK(interior.reason == YapfRailRouteReason::UnsupportedTarget); CHECK(interior.path.empty());
	const auto far_end = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256, {}, TileXY(15, 12), TRACKDIR_X_SW);
	REQUIRE(far_end.complete); CHECK(far_end.path.back().tile == TileXY(15, 12));
	CHECK(random.Check());
}

TEST_CASE_METHOD(EarlyRailRouteFixture, "Native early rail route - incomplete searches never return a guessed usable corridor")
{
	REQUIRE(YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256).complete);
	SECTION("the native open list distinguishes exhausted budget from no path") {
		/* One native expansion can reach the original goal: the rail follower
		 * compresses the straight track after the first fork into one segment.
		 * Add a second physical fork so a one-node search genuinely runs out
		 * before it can examine the destination segment. */
		MakeRailNormal(TileXY(26, 10), _current_company, TRACK_BIT_X | TRACK_BIT_RIGHT, RAILTYPE_RAIL);
		MakeRailNormal(TileXY(26, 11), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
		const auto capped = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 1);
		CHECK_FALSE(capped.complete); CHECK(capped.reason == YapfRailRouteReason::NodeBudget);
		CHECK(capped.nodes == 1); CHECK(capped.path.empty()); CHECK(capped.cost == -1);
	}
	SECTION("an exhausted graph is no path rather than a partial best node") {
		MakeClear(TileXY(25, 10), ClearGround::Grass, 0);
		const auto disconnected = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256);
		CHECK_FALSE(disconnected.complete); CHECK(disconnected.reason == YapfRailRouteReason::NoPath);
		CHECK(disconnected.path.empty()); CHECK(disconnected.cost == -1);
	}
	SECTION("a bounded trace cannot silently truncate the selected route") {
		const auto limited = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256, {}, INVALID_TILE, INVALID_TRACKDIR, 2);
		CHECK_FALSE(limited.complete); CHECK(limited.reason == YapfRailRouteReason::RouteLimit);
		CHECK(limited.path.empty()); CHECK(limited.cost == -1);
	}
	SECTION("invalid inputs cannot turn zero into an unlimited search") {
		for (const auto &invalid : {
				YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 0),
				YapfTrainPlanRoute(nullptr, slow->tile, TRACKDIR_X_SW, 256),
				YapfTrainPlanRoute(slow, INVALID_TILE, TRACKDIR_X_SW, 256),
				YapfTrainPlanRoute(slow, slow->tile, INVALID_TRACKDIR, 256),
				YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256, {}, INVALID_TILE, INVALID_TRACKDIR, 0)}) {
			CHECK_FALSE(invalid.complete); CHECK(invalid.reason == YapfRailRouteReason::InvalidInput);
			CHECK(invalid.nodes == 0); CHECK(invalid.path.empty()); CHECK(invalid.cost == -1);
		}
	}
}

TEST_CASE_METHOD(EarlyRailRouteFixture, "Native early rail route - bounded plans are independent of native cache history")
{
	const auto cold = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256);
	REQUIRE(cold.complete);
	const auto avoid_main = [](TileIndex tile, Trackdir) { return tile != TileXY(10, 10); };
	const auto filtered_cold = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256, avoid_main);
	REQUIRE(filtered_cold.complete); REQUIRE(Contains(filtered_cold, TileXY(14, 12)));
	const auto capped_cold = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 1);
	for (uint iteration = 0; iteration < 3; iteration++) {
		bool found = false;
		YapfTrainChooseTrack(slow, TileXY(8, 10), DiagDirection::SW, GetTrackBits(TileXY(8, 10)), found, false, nullptr, nullptr);
		REQUIRE(found);
	}
	CheckSameRoute(cold, YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256));
	CheckSameRoute(filtered_cold, YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256, avoid_main));
	CheckSameRoute(capped_cold, YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 1));
	CHECK(GetReservedTrackbits(TileXY(10, 10)) == TRACK_BIT_NONE);
	CHECK(GetReservedTrackbits(TileXY(14, 12)) == TRACK_BIT_NONE);
}

TEST_CASE_METHOD(EarlyRailRouteFixture, "Native split route - a completed distant goal can export only its selected local corridor")
{
	const auto whole = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256);
	REQUIRE(whole.complete);
	const auto capped = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256, {}, INVALID_TILE, INVALID_TRACKDIR, 10);
	REQUIRE_FALSE(capped.complete);
	REQUIRE(capped.reason == YapfRailRouteReason::RouteLimit);
	GameRandomSeedChecker random;
	const Order order = slow->current_order;
	const auto prefix = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256, {}, INVALID_TILE, INVALID_TRACKDIR, 10,
			std::nullopt, [](TileIndex tile, Trackdir td) { return tile == TileXY(8, 10) && td == TRACKDIR_X_SW; });
	REQUIRE(prefix.complete);
	REQUIRE_FALSE(prefix.path.empty());
	CHECK(prefix.path.back().tile == TileXY(8, 10));
	CHECK(prefix.cost == whole.cost); // The real destination was searched, not replaced with a nearby marker.
	CHECK(prefix.nodes == whole.nodes);
	CHECK(slow->current_order.Equals(order)); CHECK(random.Check());
	for (const auto &step : prefix.path) CHECK(GetReservedTrackbits(step.tile) == TRACK_BIT_NONE);
	SECTION("a boundary on the wrong direction is not evidence") {
		const auto wrong = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256, {}, INVALID_TILE, INVALID_TRACKDIR, 10,
				std::nullopt, [](TileIndex tile, Trackdir td) { return tile == TileXY(8, 10) && td == TRACKDIR_X_NE; });
		CHECK_FALSE(wrong.complete); CHECK(wrong.path.empty());
	}
	SECTION("a reachable prefix cannot substitute for an unreachable real target") {
		MakeClear(TileXY(33, 10), ClearGround::Grass, 0);
		YapfNotifyTrackLayoutChange(INVALID_TILE, INVALID_TRACK);
		const auto blocked = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256, {}, INVALID_TILE, INVALID_TRACKDIR, 10,
				std::nullopt, [](TileIndex tile, Trackdir td) { return tile == TileXY(8, 10) && td == TRACKDIR_X_SW; });
		CHECK_FALSE(blocked.complete); CHECK(blocked.path.empty());
	}
}

TEST_CASE_METHOD(EarlyRailRouteFixture, "Native early rail route - bridge arcs cannot be confused with the ground below")
{
	MakeRailBridgeRamp(TileXY(12, 12), _current_company, 0, DiagDirection::SW, RAILTYPE_RAIL, false);
	MakeRailBridgeRamp(TileXY(16, 12), _current_company, 0, DiagDirection::NE, RAILTYPE_RAIL, false);
	for (uint x = 13; x <= 15; x++) SetBridgeMiddle(TileXY(x, 12), Axis::X);
	YapfNotifyTrackLayoutChange(INVALID_TILE, INVALID_TRACK);
	GameRandomSeedChecker random;
	const auto plan = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256,
			[](TileIndex tile, Trackdir) { return tile != TileXY(10, 10); });
	REQUIRE(plan.complete);
	CHECK(Contains(plan, TileXY(12, 12))); CHECK(Contains(plan, TileXY(16, 12)));
	for (uint x = 13; x <= 15; x++) CHECK_FALSE(Contains(plan, TileXY(x, 12)));
	const auto arc = std::find_if(plan.path.begin(), plan.path.end(), [](const YapfRailRouteStep &step) { return step.tile == TileXY(16, 12); });
	REQUIRE(arc != plan.path.end()); CHECK(arc->wormhole_from == TileXY(12, 12));
	CHECK(arc->wormhole_from_trackdir == TRACKDIR_X_SW); CHECK(arc->wormhole_tiles == 3);
	CHECK(GetReservedTrackbits(TileXY(12, 12)) == TRACK_BIT_NONE);
	CHECK(GetReservedTrackbits(TileXY(16, 12)) == TRACK_BIT_NONE); CHECK(random.Check());
}

TEST_CASE_METHOD(EarlyRailRouteFixture, "Native early required point - converging paths retain separate before and after point states")
{
	AutoRestoreBackup turns(_settings_game.pf.forbid_90_deg, GENERATE(false, true));
	/* Both branches merge before this intermediate platform. The station
	 * closes the native segment, forcing both histories to compete at the
	 * identical next (tile, trackdir), not merely at different branch keys.
	 * The current order explicitly passes intermediate stations. */
	StationAt(24, 10);
	slow->current_order.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
	const auto ordinary = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256);
	REQUIRE(ordinary.complete); REQUIRE(Contains(ordinary, TileXY(14, 10)));
	REQUIRE_FALSE(Contains(ordinary, TileXY(14, 12)));
	const Order order = slow->current_order;
	GameRandomSeedChecker random;
	const std::pair point{TileXY(14, 12), TRACKDIR_X_SW};
	const auto required = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256,
			{}, INVALID_TILE, INVALID_TRACKDIR, 16384, point);
	REQUIRE(required.complete); REQUIRE(required.reason == YapfRailRouteReason::Complete);
	CHECK(Contains(required, point.first)); CHECK_FALSE(Contains(required, TileXY(14, 10)));
	CHECK(Contains(required, TileXY(25, 10))); CHECK(required.path.back().tile == goal->xy);
	CHECK(required.cost > ordinary.cost);
	/* Warming either specialization must not alter the original no-point
	 * choice, nor let its cheaper closed state prune the required detour. */
	CheckSameRoute(ordinary, YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256));
	CheckSameRoute(required, YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256,
			{}, INVALID_TILE, INVALID_TRACKDIR, 16384, point));
	CHECK(slow->current_order.Equals(order)); CHECK(random.Check());
	for (const auto &step : required.path) CHECK(GetReservedTrackbits(step.tile) == TRACK_BIT_NONE);
}

TEST_CASE_METHOD(EarlyRailRouteFixture, "Native early required point - a mandatory stop cannot be passed before satisfying the point")
{
	AutoRestoreBackup turns(_settings_game.pf.forbid_90_deg, GENERATE(false, true));
	Station *stop = StationAt(14, 10);
	slow->current_order.MakeGoToStation(stop->index); slow->dest_tile = stop->xy;
	/* Remove the pre-stop bypass. There is a real directed circuit from the
	 * later point back to the station, so rejecting this query specifically
	 * requires respecting the first mandatory stop, not graph disconnection. */
	MakeRailNormal(TileXY(8, 10), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
	MakeClear(TileXY(8, 11), ClearGround::Grass, 0);
	MakeRailNormal(TileXY(28, 10), _current_company, TRACK_BIT_X | TRACK_BIT_RIGHT, RAILTYPE_RAIL);
	for (uint y = 11; y < 16; y++) MakeRailNormal(TileXY(28, y), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
	MakeRailNormal(TileXY(28, 16), _current_company, TRACK_BIT_UPPER, RAILTYPE_RAIL);
	for (uint x = 5; x < 28; x++) MakeRailNormal(TileXY(x, 16), _current_company, TRACK_BIT_X, RAILTYPE_RAIL);
	MakeRailNormal(TileXY(4, 16), _current_company, TRACK_BIT_LEFT, RAILTYPE_RAIL);
	for (uint y = 11; y < 16; y++) MakeRailNormal(TileXY(4, y), _current_company, TRACK_BIT_Y, RAILTYPE_RAIL);
	MakeRailNormal(TileXY(4, 10), _current_company, TRACK_BIT_X | TRACK_BIT_LOWER, RAILTYPE_RAIL);
	const std::pair point{TileXY(24, 10), TRACKDIR_X_SW};
	const auto before = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256);
	REQUIRE(before.complete); REQUIRE(before.path.back().tile == stop->xy);
	const auto after = YapfTrainPlanRoute(slow, point.first, point.second, 256);
	REQUIRE(after.complete); REQUIRE(after.path.back().tile == stop->xy);
	REQUIRE(Contains(after, TileXY(28, 16))); REQUIRE(Contains(after, TileXY(4, 16)));
	GameRandomSeedChecker random;
	const auto invalid = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256,
			{}, INVALID_TILE, INVALID_TRACKDIR, 16384, point);
	CHECK_FALSE(invalid.complete); CHECK(invalid.reason == YapfRailRouteReason::NoPath);
	CHECK(invalid.path.empty()); CHECK(invalid.cost == -1); CHECK(random.Check());
	CheckSameRoute(before, YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256));
}

TEST_CASE_METHOD(EarlyRailRouteFixture, "Native early required point - origin and directed point semantics are exact")
{
	const std::pair point{TileXY(14, 12), TRACKDIR_X_SW};
	const auto ordinary = YapfTrainPlanRoute(slow, point.first, point.second, 256);
	REQUIRE(ordinary.complete);
	const auto at_origin = YapfTrainPlanRoute(slow, point.first, point.second, 256,
			{}, INVALID_TILE, INVALID_TRACKDIR, 16384, point);
	REQUIRE(at_origin.complete); CHECK(at_origin.path.front().tile == point.first);
	CHECK(at_origin.path.back().tile == goal->xy); CHECK(at_origin.cost == ordinary.cost);
	const auto wrong_direction = YapfTrainPlanRoute(slow, slow->tile, TRACKDIR_X_SW, 256,
			{}, INVALID_TILE, INVALID_TRACKDIR, 16384, std::pair{point.first, TRACKDIR_X_NE});
	CHECK_FALSE(wrong_direction.complete); CHECK(wrong_direction.reason == YapfRailRouteReason::NoPath);
	CHECK(wrong_direction.path.empty());
}

TEST_CASE_METHOD(EarlyRailRouteFixture, "Native early directed reservation - only a continuous correctly directed prefix satisfies a point")
{
	const bool reverse = GENERATE(false, true);
	if (reverse) {
		Place(slow, 14, 10);
		slow->direction = Direction::NE;
	}
	const Trackdir direction = reverse ? TRACKDIR_X_NE : TRACKDIR_X_SW;
	const TileIndex point = TileXY(reverse ? 8 : 13, 10);
	for (uint x = 7; x <= 14; x++) REQUIRE(TryReserveRailTrack(TileXY(x, 10), TRACK_X));
	const auto vehicle_flags = slow->flags;
	GameRandomSeedChecker random;
	/* The existing native helper intentionally accepts either direction of
	 * the physical track. The new query must not change that old contract. */
	CHECK(TrainReservationPassesThroughTile(slow, point, TRACK_X));
	CHECK(TrainReservationPassesThroughTrackdir(slow, point, direction));
	CHECK_FALSE(TrainReservationPassesThroughTrackdir(slow, point, ReverseTrackdir(direction)));
	CHECK(TrainReservationPassesThroughTrackdir(slow, slow->tile, direction));
	CHECK_FALSE(TrainReservationPassesThroughTrackdir(slow, slow->tile, ReverseTrackdir(direction)));
	CHECK_FALSE(TrainReservationPassesThroughTrackdir(slow, point, TRACKDIR_Y_SE));
	CHECK_FALSE(TrainReservationPassesThroughTrackdir(nullptr, point, direction));
	CHECK_FALSE(TrainReservationPassesThroughTrackdir(slow, INVALID_TILE, direction));
	CHECK_FALSE(TrainReservationPassesThroughTrackdir(slow, point, INVALID_TRACKDIR));
	for (uint x = 7; x <= 14; x++) CHECK(GetReservedTrackbits(TileXY(x, 10)) == TRACK_BIT_X);
	CHECK(slow->flags == vehicle_flags); CHECK(random.Check());
	/* A reserved tile beyond a gap is not evidence that this train's prefix
	 * has already passed the point; no reservation is changed by the query. */
	UnreserveRailTrack(TileXY(10, 10), TRACK_X);
	CHECK_FALSE(TrainReservationPassesThroughTile(slow, point, TRACK_X));
	CHECK_FALSE(TrainReservationPassesThroughTrackdir(slow, point, direction));
	CHECK(GetReservedTrackbits(point) == TRACK_BIT_X);
	CHECK(GetReservedTrackbits(TileXY(10, 10)) == TRACK_BIT_NONE);
	CHECK(slow->flags == vehicle_flags); CHECK(random.Check());
}

#include "early_stop_route.inc"
