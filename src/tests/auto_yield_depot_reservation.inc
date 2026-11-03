TEST_CASE_METHOD(AutomaticYieldFixture, "PBS reservation - sheltered depot parts do not block the entrance but exposed parts do")
{
	REQUIRE(Depot::GetNumItems() == 0);
	auto cleanup = scope_guard([] { _depot_pool.CleanPool(); });
	const auto tile = TileXY(30,10);
	Place(fast,30,10);
	bool sheltered = false;
	SECTION("a foreign consist on ordinary rail remains a blocker") {}
	SECTION("a fully sheltered depot consist is not a track occupant") {
		Depot *depot = Depot::Create(tile);
		MakeRailDepot(tile,_current_company,depot->index,DiagDirection::NE,RAILTYPE_RAIL);
		fast->track = TRACK_BIT_DEPOT; fast->vehstatus.Set(VehState::Hidden);
		SetDepotReservation(tile,true); sheltered = true;
	}
	SECTION("an exposed part at the depot entrance remains a blocker") {
		Depot *depot = Depot::Create(tile);
		MakeRailDepot(tile,_current_company,depot->index,DiagDirection::NE,RAILTYPE_RAIL);
		SetDepotReservation(tile,true);
	}
	for (uint x=7;x<=30;x++) if (IsPlainRailTile(TileXY(x,10))) REQUIRE(TryReserveRailTrack(TileXY(x,10),TRACK_X));
	REQUIRE(FollowTrainReservation(slow).tile == tile);
	GameRandomSeedChecker random;
	CHECK(FindForeignTrainOnReservation(slow) == (sheltered ? nullptr : fast));
	CHECK(random.Check()); CHECK(ValidateVehicleTileHash(fast));
}
