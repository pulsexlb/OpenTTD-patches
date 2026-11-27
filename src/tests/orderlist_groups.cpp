/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file orderlist_groups.cpp Tests for schedule group ownership and hierarchy. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../company_base.h"
#include "../company_func.h"
#include "../order_base.h"
#include "../order_cmd.h"
#include "../order_func.h"
#include "../core/pool_func.hpp"
#include "../core/backup_type.hpp"
#include "../train.h"
#include "../schedule_group_gui.h"
#include "../depot_base.h"
#include "../linkgraph/linkgraph.h"
#include "../linkgraph/linkgraphschedule.h"
#include "../linkgraph/refresh.h"
#include "../map_func.h"
#include "../rail_map.h"
#include "../scope.h"
#include "../station_base.h"
#include "../tracerestrict.h"

class ScheduleGroupFixture {
protected:
	AutoRestoreBackup<CompanyID> company_backup{_current_company, AutoRestoreBackupNoNewValueTag{}};
	CompanyID owner;
	ScheduleGroupFixture()
	{
		REQUIRE(Company::GetNumItems() == 0);
		REQUIRE(OrderList::GetNumItems() == 0);
		REQUIRE(Company::CanAllocateItem());
		owner = Company::Create()->index;
		_current_company = owner;
	}
	~ScheduleGroupFixture()
	{
		_vehicle_pool.CleanPool();
		_orderlist_pool.CleanPool();
		_company_pool.CleanPool();
		ClearOrderDestinationRefcountMap();
	}
	Train *AttachTrain(OrderList *list)
	{
		Train *v = Train::Create();
		v->SetEngine();
		v->SetFrontEngine();
		v->owner = owner;
		v->orders = list;
		list->AssignVehicle(v);
		return v;
	}
	OrderList *Create(const std::string &name, OrderListID parent = OrderListID::Invalid(), bool group = true)
	{
		CommandCost result = CmdCreateOrderList(DoCommandFlag::Execute, name, parent, group);
		REQUIRE(result.Succeeded());
		return OrderList::Get(*result.GetResultData<OrderListID>());
	}
};

TEST_CASE_METHOD(ScheduleGroupFixture, "Schedule groups - hierarchy and command preflight")
{
	OrderList *root = Create("Root");
	OrderList *child = Create("Child", root->index);
	OrderList *schedule = Create("Schedule", child->index, false);
	CHECK(AreScheduleGroupsValid());
	CHECK_FALSE(root->IsVisibleToCompany(owner)); // Folders must never appear as executable schedules.
	CHECK(schedule->IsVisibleToCompany(owner));
	CHECK(CmdMoveOrderListToGroup({}, root->index, child->index).Failed());
	CHECK(CmdMoveOrderListToGroup({}, root->index, root->index).Failed());
	CHECK(CmdMoveOrderListToGroup({}, schedule->index, root->index).Succeeded());
	CHECK(schedule->GetScheduleGroup() == child->index); // Preflight must not mutate state.
	CHECK(CmdMoveOrderListToGroup(DoCommandFlag::Execute, schedule->index, root->index).Succeeded());
	CHECK(schedule->GetScheduleGroup() == root->index);
	CHECK(CmdSetOrderListPublic(DoCommandFlag::Execute, root->index, true).Failed());
	CHECK(CmdCreateOrderList({}, "Bad parent", schedule->index, true).Failed());
}

TEST_CASE_METHOD(ScheduleGroupFixture, "Schedule groups - destination cache rebuilds after public schedule changes")
{
	OrderList *schedule = Create("Shared public schedule", OrderListID::Invalid(), false);
	schedule->SetPublic(true);
	Order station_order;
	station_order.MakeGoToStation(StationID{123});
	schedule->GetOrderVector().push_back(station_order);
	schedule->RecalculateDerivedCounters();
	auto attach = [&](CompanyID company) {
		REQUIRE(Train::CanAllocateItem());
		Train *v = Train::Create();
		v->SetEngine();
		v->SetFrontEngine();
		v->owner = company;
		v->orders = schedule;
		schedule->AssignVehicle(v);
		return v;
	};
	Train *a = attach(owner);
	attach(owner);
	REQUIRE(Company::CanAllocateItem());
	CompanyID foreign = Company::Create()->index;
	Train *b = attach(foreign);
	std::map<CompanyID, uint32_t> counts;
	IterateOrderRefcountMapForDestinationID(StationID{123}, [&](CompanyID company, OrderType, VehicleType, uint32_t count) { counts[company] += count; return true; });
	CHECK(_order_destination_refcount_map_valid);
	CHECK(counts[owner] == 1); // Count the shared schedule once per company/type.
	CHECK(counts[foreign] == 1);
	b->RemoveFromShared();
	b->orders = nullptr;
	counts.clear();
	IterateOrderRefcountMapForDestinationID(StationID{123}, [&](CompanyID company, OrderType, VehicleType, uint32_t count) { counts[company] += count; return true; });
	CHECK(counts[owner] == 1);
	CHECK(counts[foreign] == 0);
	schedule->DeleteOrderAt(0);
	CHECK_FALSE(_order_destination_refcount_map_valid);
	bool found = false;
	IterateOrderRefcountMapForDestinationID(StationID{123}, [&](CompanyID, OrderType, VehicleType, uint32_t) { found = true; return false; });
	CHECK_FALSE(found);
	CHECK(a->orders == schedule);
}

TEST_CASE_METHOD(ScheduleGroupFixture, "Decouple marker and coupling wait do not block timetable completion")
{
	OrderList *schedule = Create("Timetabled split", OrderListID::Invalid(), false);
	Order station;
	station.MakeGoToStation(StationID{123});
	station.SetTravelTime(50);
	station.SetWaitTime(10);
	station.SetTravelTimetabled(true);
	station.SetWaitTimetabled(true);
	schedule->GetOrderVector().push_back(station);
	Order decouple;
	decouple.MakeDecouple();
	schedule->GetOrderVector().push_back(decouple);
	Order wait;
	wait.MakeWaitCouple();
	schedule->GetOrderVector().push_back(wait);
	schedule->RecalculateDerivedCounters();
	CHECK(schedule->IsCompleteTimetable());
}

TEST_CASE_METHOD(ScheduleGroupFixture, "Schedule groups - deletion preserves schedules and subgroups")
{
	OrderList *root = Create("Root");
	OrderList *child = Create("Child", root->index);
	OrderList *grandchild = Create("Grandchild", child->index);
	OrderList *schedule = Create("Schedule", child->index, false);
	OrderListID deleted = child->index;
	REQUIRE(CmdDeleteOrderList(DoCommandFlag::Execute, deleted).Succeeded());
	CHECK_FALSE(OrderList::IsValidID(deleted));
	CHECK(grandchild->GetScheduleGroup() == root->index);
	CHECK(schedule->GetScheduleGroup() == root->index);
	CHECK(AreScheduleGroupsValid());
	REQUIRE(CmdMoveOrderListToGroup(DoCommandFlag::Execute, schedule->index, OrderListID::Invalid()).Succeeded());
	CHECK(schedule->GetScheduleGroup() == OrderListID::Invalid());
}

TEST_CASE_METHOD(ScheduleGroupFixture, "Schedule groups - ownership and malformed loaded hierarchies")
{
	OrderList *root = Create("Root");
	OrderList *child = Create("Child", root->index);
	REQUIRE(Company::CanAllocateItem());
	_current_company = Company::Create()->index;
	OrderList *foreign = Create("Foreign");
	CHECK(CmdCreateOrderList({}, "Cross-company", root->index, true).Failed());
	CHECK(CmdRenameOrderList(DoCommandFlag::Execute, root->index, "Stolen").Failed());
	CHECK(CmdMoveOrderListToGroup(DoCommandFlag::Execute, foreign->index, root->index).Failed());
	CHECK(CmdDeleteOrderList(DoCommandFlag::Execute, root->index).Failed());
	root->SetScheduleGroup(child->index);
	CHECK_FALSE(AreScheduleGroupsValid());
	root->SetScheduleGroup(OrderListID::Invalid());
	child->SetScheduleGroup(foreign->index);
	CHECK_FALSE(AreScheduleGroupsValid());
	child->SetScheduleGroup(OrderListID(63999));
	CHECK_FALSE(AreScheduleGroupsValid());
}

TEST_CASE_METHOD(ScheduleGroupFixture, "Schedule groups - public schedules keep foreign ancestors")
{
	OrderList *root = Create("Root");
	OrderList *child = Create("Child", root->index);
	OrderList *public_schedule = Create("Public", child->index, false);
	OrderList *private_schedule = Create("Private", child->index, false);
	OrderList *private_group = Create("Private group");
	public_schedule->SetPublic(true);
	CompanyID other = Company::Create()->index;
	const auto tree = GetVisibleScheduleTree(other);
	REQUIRE(tree.size() == 3);
	CHECK(tree[0].order_list == root);
	CHECK(tree[0].depth == 0);
	CHECK(tree[1].order_list == child);
	CHECK(tree[1].depth == 1);
	CHECK(tree[2].order_list == public_schedule);
	CHECK(tree[2].depth == 2);
	const auto own = GetVisibleScheduleEntries(owner);
	CHECK(own.contains(private_schedule->index));
	CHECK(own.contains(private_group->index));
	CHECK_FALSE(root->IsVisibleToCompany(other));
	CHECK_FALSE(private_schedule->IsVisibleToCompany(other));
	CHECK(CmdMoveOrderListToGroup({}, public_schedule->index, root->index).Succeeded());
	std::set<OrderListID> collapsed{root->index};
	CHECK(GetVisibleScheduleTree(other, nullptr, &collapsed).size() == 1);
	std::set<OrderListID> matches{root->index, child->index, public_schedule->index};
	CHECK(GetVisibleScheduleTree(other, &matches).size() == 3);
	public_schedule->SetPublic(false);
	CHECK(GetVisibleScheduleTree(other).empty());
}

TEST_CASE_METHOD(ScheduleGroupFixture, "Couple - speed filter commands validate and update shared current orders")
{
	OrderList *schedule = Create("Speed filtered coupling", OrderListID::Invalid(), false);
	Order couple;
	couple.MakeGoToCouple();
	schedule->GetOrderVector().push_back(couple);
	schedule->RecalculateDerivedCounters();
	std::array<Train *, 2> trains;
	for (Train *&train : trains) {
		REQUIRE(Train::CanAllocateItem());
		train = Train::Create();
		train->SetEngine();
		train->SetFrontEngine();
		train->owner = owner;
		train->orders = schedule;
		schedule->AssignVehicle(train);
		train->current_order = couple;
	}
	for (ModifyOrderFlags flag : {MOF_COUPLE_REJECT_SLOWER, MOF_COUPLE_REJECT_FASTER}) {
		CHECK(CmdModifyOrder({}, OrderTargetType::OrderList, schedule->index.base(), 0, flag, 1, INVALID_CARGO, {}).Succeeded());
		CHECK_FALSE(schedule->GetOrderAt(0)->GetCoupleRejectSlower());
		CHECK_FALSE(schedule->GetOrderAt(0)->GetCoupleRejectFaster());
		CHECK(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::OrderList, schedule->index.base(), 0, flag, 2, INVALID_CARGO, {}).Failed());
	}
	CHECK(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::OrderList, schedule->index.base(), 0, MOF_COUPLE_REJECT_SLOWER, 1, INVALID_CARGO, {}).Succeeded());
	for (Train *train : trains) {
		CHECK(train->current_order.GetCoupleRejectSlower());
		CHECK_FALSE(train->current_order.GetCoupleRejectFaster());
	}
	CHECK(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::Vehicle, trains[0]->index.base(), 0, MOF_COUPLE_REJECT_FASTER, 1, INVALID_CARGO, {}).Succeeded());
	CHECK(schedule->GetOrderAt(0)->GetCoupleRejectSlower());
	CHECK(schedule->GetOrderAt(0)->GetCoupleRejectFaster());
	for (Train *train : trains) {
		CHECK(train->current_order.GetCoupleRejectSlower());
		CHECK(train->current_order.GetCoupleRejectFaster());
	}
	CHECK(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::OrderList, schedule->index.base(), 0, MOF_COUPLE_REJECT_SLOWER, 0, INVALID_CARGO, {}).Succeeded());
	for (Train *train : trains) {
		CHECK_FALSE(train->current_order.GetCoupleRejectSlower());
		CHECK(train->current_order.GetCoupleRejectFaster());
	}
	for (ModifyOrderFlags flag : {MOF_COUPLE_SPEED_COMPARATOR1, MOF_COUPLE_SPEED_COMPARATOR2}) {
		CHECK(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::OrderList, schedule->index.base(), 0, flag, 7, INVALID_CARGO, {}).Failed());
	}
	CHECK(CmdModifyOrder({}, OrderTargetType::OrderList, schedule->index.base(), 0, MOF_COUPLE_SPEED_VALUE1, 80, static_cast<CargoType>(1), {}).Succeeded());
	CHECK(schedule->GetOrderAt(0)->GetCoupleSpeedValue(0) == 0);
	for (auto [flag, value] : {std::pair{MOF_COUPLE_SPEED_COMPARATOR1, 6}, std::pair{MOF_COUPLE_SPEED_VALUE1, 80}, std::pair{MOF_COUPLE_SPEED_COMPARATOR2, 4}, std::pair{MOF_COUPLE_SPEED_VALUE2, 120}}) {
		CHECK(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::OrderList, schedule->index.base(), 0, flag, value, (flag == MOF_COUPLE_SPEED_VALUE1 || flag == MOF_COUPLE_SPEED_VALUE2) ? static_cast<CargoType>(1) : INVALID_CARGO, {}).Succeeded());
	}
	for (Train *train : trains) {
		CHECK(train->current_order.GetCoupleSpeedComparator(0) == 6);
		CHECK(train->current_order.GetCoupleSpeedValue(0) == 80);
		CHECK(train->current_order.GetCoupleSpeedComparator(1) == 4);
		CHECK(train->current_order.GetCoupleSpeedValue(1) == 120);
		CHECK(train->current_order.GetCoupleSpeedUnit(0) == 1);
		CHECK(train->current_order.GetCoupleSpeedUnit(1) == 1);
	}
	CHECK(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::Vehicle, trains[0]->index.base(), 0, MOF_COUPLE_SPEED_VALUE2, 65535, static_cast<CargoType>(1), {}).Succeeded());
	for (Train *train : trains) CHECK(train->current_order.GetCoupleSpeedValue(1) == 65535);
	CHECK(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::OrderList, schedule->index.base(), 0, MOF_COUPLE_SPEED_VALUE1, 75, static_cast<CargoType>(6), {}).Failed());
	CHECK(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::OrderList, schedule->index.base(), 0, MOF_COUPLE_SPEED_VALUE1, 75, static_cast<CargoType>(0), {}).Succeeded());
	for (Train *train : trains) {
		CHECK(train->current_order.GetCoupleSpeedValue(0) == 75);
		CHECK(train->current_order.GetCoupleSpeedUnit(0) == 0);
	}
	CHECK(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::OrderList, schedule->index.base(), 0, MOF_WAIT_COUPLE_POWER_MODE, 1, INVALID_CARGO, {}).Failed());
	schedule->GetOrderAt(0)->MakeWaitCouple();
	for (Train *train : trains) train->current_order = *schedule->GetOrderAt(0);
	CHECK(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::OrderList, schedule->index.base(), 0, MOF_WAIT_COUPLE_POWER_MODE, 4, INVALID_CARGO, {}).Failed());
	REQUIRE(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::OrderList, schedule->index.base(), 0, MOF_WAIT_COUPLE_POWER_MODE, 2, INVALID_CARGO, {}).Succeeded());
	REQUIRE(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::Vehicle, trains[0]->index.base(), 0, MOF_WAIT_COUPLE_POWER_VALUE, 4200, INVALID_CARGO, {}).Succeeded());
	for (Train *train : trains) {
		CHECK(train->current_order.GetWaitCouplePowerMode() == WaitCouplePowerMode::Less);
		CHECK(train->current_order.GetWaitCouplePowerValue() == 4200);
	}
	CHECK(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::OrderList, schedule->index.base(), 0, MOF_COUPLE_REJECT_FASTER, 1, INVALID_CARGO, {}).Failed());
}


TEST_CASE_METHOD(ScheduleGroupFixture, "Decouple - moving shared trains survive order vector reallocation")
{
	const bool edit_list = GENERATE(false, true);
	OrderList *schedule = Create("Moving split", OrderListID::Invalid(), false);
	Order station;
	station.MakeGoToStation(StationID{123});
	schedule->GetOrderVector() = std::vector<Order>{station};
	schedule->RecalculateDerivedCounters();
	std::array<Train *, 2> trains;
	for (Train *&train : trains) {
		train = Train::Create();
		train->SetEngine();
		train->SetFrontEngine();
		train->owner = owner;
		train->orders = schedule;
		schedule->AssignVehicle(train);
		train->current_order = station;
		train->cur_real_order_index = train->cur_implicit_order_index = 0;
		train->cur_speed = 80;
	}
	const auto target = edit_list ? OrderTargetType::OrderList : OrderTargetType::Vehicle;
	const uint32_t id = edit_list ? schedule->index.base() : trains[0]->index.base();
	const Order *old_address = schedule->GetOrderAt(0);
	REQUIRE(schedule->GetOrderVector().capacity() == 1);
	CHECK(CmdModifyOrder({}, target, id, 0, MOF_DECOUPLE, 1, static_cast<CargoType>(0), {}).Succeeded());
	CHECK(schedule->GetNumOrders() == 1);
	CHECK(schedule->GetOrderAt(0)->GetDecouple() == ODF_NOTHING);
	REQUIRE(CmdModifyOrder(DoCommandFlag::Execute, target, id, 0, MOF_DECOUPLE, 1, static_cast<CargoType>(0), {}).Succeeded());
	REQUIRE(schedule->GetNumOrders() == 2);
	CHECK(schedule->GetOrderAt(0) != old_address);
	CHECK(schedule->GetOrderAt(0)->GetDecouple() == ODF_DECOUPLE);
	CHECK(schedule->GetOrderAt(1)->IsType(OT_DECOUPLE));
	for (Train *train : trains) {
		CHECK(train->current_order.IsType(OT_GOTO_STATION));
		CHECK(train->current_order.GetDecouple() == ODF_DECOUPLE);
		CHECK(train->cur_real_order_index == 0);
		CHECK(train->cur_implicit_order_index == 0);
		CHECK(train->cur_speed == 80);
	}
	REQUIRE(CmdModifyOrder(DoCommandFlag::Execute, target, id, 0, MOF_DECOUPLE, 0, static_cast<CargoType>(0), {}).Succeeded());
	CHECK(schedule->GetNumOrders() == 1);
	for (Train *train : trains) CHECK(train->current_order.GetDecouple() == ODF_NOTHING);
}

TEST_CASE_METHOD(ScheduleGroupFixture, "Decouple - insertion failure leaves station and active order unchanged")
{
	const bool edit_list = GENERATE(false, true);
	OrderList *schedule = Create("Full split", OrderListID::Invalid(), false);
	Order station;
	station.MakeGoToStation(StationID{123});
	schedule->GetOrderVector().assign(MAX_VEH_ORDER_ID, station);
	schedule->RecalculateDerivedCounters();
	Train *train = Train::Create();
	train->SetEngine();
	train->SetFrontEngine();
	train->owner = owner;
	train->orders = schedule;
	schedule->AssignVehicle(train);
	train->current_order = station;
	const auto target = edit_list ? OrderTargetType::OrderList : OrderTargetType::Vehicle;
	const uint32_t id = edit_list ? schedule->index.base() : train->index.base();
	for (DoCommandFlags flags : {DoCommandFlags{}, DoCommandFlags{DoCommandFlag::Execute}}) {
		CHECK(CmdModifyOrder(flags, target, id, 0, MOF_DECOUPLE, 1, static_cast<CargoType>(0), {}).Failed());
		CHECK(schedule->GetNumOrders() == MAX_VEH_ORDER_ID);
		CHECK(schedule->GetOrderAt(0)->GetDecouple() == ODF_NOTHING);
		CHECK(train->current_order.GetDecouple() == ODF_NOTHING);
	}
}

TEST_CASE_METHOD(ScheduleGroupFixture, "Decouple - disabling a missing marker preserves other orders and sharing")
{
	const bool has_successor = GENERATE(false, true);
	OrderList *schedule = Create("Legacy split", OrderListID::Invalid(), false);
	Order station;
	station.MakeGoToStation(StationID{123});
	station.SetDecouple(ODF_DECOUPLE);
	schedule->GetOrderVector().push_back(station);
	if (has_successor) schedule->GetOrderVector().push_back(station);
	schedule->RecalculateDerivedCounters();
	Train *train = Train::Create();
	train->SetEngine();
	train->SetFrontEngine();
	train->owner = owner;
	train->orders = schedule;
	schedule->AssignVehicle(train);
	train->current_order = station;
	CHECK(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::Vehicle, train->index.base(), 0, MOF_DECOUPLE, 2, static_cast<CargoType>(0), {}).Failed());
	REQUIRE(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::Vehicle, train->index.base(), 0, MOF_DECOUPLE, 0, static_cast<CargoType>(0), {}).Succeeded());
	CHECK(train->orders == schedule);
	CHECK(schedule->GetNumOrders() == (has_successor ? 2 : 1));
	CHECK(schedule->GetOrderAt(0)->GetDecouple() == ODF_NOTHING);
	if (has_successor) CHECK(schedule->GetOrderAt(1)->GetDecouple() == ODF_DECOUPLE);
}


TEST_CASE_METHOD(ScheduleGroupFixture, "Couple - speed bound sources switch atomically and synchronise active orders")
{
	const bool edit_list = GENERATE(false, true);
	OrderList *schedule = Create("Speed bounds", OrderListID::Invalid(), false);
	Order couple;
	couple.MakeGoToCouple();
	couple.SetCoupleSpeedValue(0, 80);
	couple.SetCoupleSpeedValue(1, 120);
	couple.SetCoupleSpeedUnit(0, 0);
	schedule->GetOrderVector().push_back(couple);
	schedule->RecalculateDerivedCounters();
	std::array<Train *, 2> trains;
	for (Train *&train : trains) {
		train = Train::Create();
		train->SetEngine();
		train->SetFrontEngine();
		train->owner = owner;
		train->orders = schedule;
		schedule->AssignVehicle(train);
		train->current_order = couple;
	}
	const auto target = edit_list ? OrderTargetType::OrderList : OrderTargetType::Vehicle;
	const uint32_t id = edit_list ? schedule->index.base() : trains[0]->index.base();
	for (uint row = 0; row < 2; row++) {
		const ModifyOrderFlags mof = row == 0 ? MOF_COUPLE_SPEED_MIN_MODE : MOF_COUPLE_SPEED_MAX_MODE;
		for (CoupleSpeedBoundMode mode : {CoupleSpeedBoundMode::OwnMax, CoupleSpeedBoundMode::Manual, CoupleSpeedBoundMode::Disabled}) {
			const auto before = schedule->GetOrderAt(0)->GetCoupleSpeedBoundMode(row);
			CHECK(CmdModifyOrder({}, target, id, 0, mof, to_underlying(mode), INVALID_CARGO, {}).Succeeded());
			CHECK(schedule->GetOrderAt(0)->GetCoupleSpeedBoundMode(row) == before);
			REQUIRE(CmdModifyOrder(DoCommandFlag::Execute, target, id, 0, mof, to_underlying(mode), INVALID_CARGO, {}).Succeeded());
			for (const Order *order : {schedule->GetOrderAt(0), &trains[0]->current_order, &trains[1]->current_order}) {
				CHECK(order->GetCoupleSpeedBoundMode(row) == mode);
				CHECK(order->GetCoupleSpeedComparator(row) == (mode == CoupleSpeedBoundMode::Manual ? to_underlying(row == 0 ? OrderConditionComparator::MoreThanOrEqual : OrderConditionComparator::LessThanOrEqual) + 1 : 0));
				CHECK((row == 0 ? order->GetCoupleRejectFaster() : order->GetCoupleRejectSlower()) == (mode == CoupleSpeedBoundMode::OwnMax));
				CHECK(order->GetCoupleSpeedBoundMode(1 - row) == CoupleSpeedBoundMode::Disabled);
				CHECK(order->GetCoupleSpeedValue(0) == 80);
				CHECK(order->GetCoupleSpeedValue(1) == 120);
				CHECK(order->GetCoupleSpeedUnit(0) == 0);
			}
		}
		CHECK(CmdModifyOrder(DoCommandFlag::Execute, target, id, 0, mof, 3, INVALID_CARGO, {}).Failed());
	}
	Order legacy;
	legacy.MakeGoToCouple();
	legacy.SetCoupleSpeedComparator(0, 2); // Existing not-equal criteria remain unchanged until edited.
	CHECK(legacy.GetCoupleSpeedBoundMode(0) == CoupleSpeedBoundMode::Manual);
	CHECK(legacy.GetCoupleSpeedComparator(0) == 2);
}

TEST_CASE_METHOD(ScheduleGroupFixture, "Independent timetable condition edits use their order list")
{
	OrderList *list = Create("Independent conditions", OrderListID::Invalid(), false);
	Order condition;
	condition.MakeConditional(1);
	Order station;
	station.MakeGoToStation(StationID{123});
	list->GetOrderVector() = {condition, station, station};
	list->RecalculateDerivedCounters();
	auto modify = [&](DoCommandFlags flags, ModifyOrderFlags change, uint16_t value) {
		return CmdModifyOrder(flags, OrderTargetType::OrderList, list->index.base(), 0, change, value, INVALID_CARGO, {});
	};
	CHECK(modify({}, MOF_COND_DESTINATION, 2).Succeeded());
	CHECK(list->GetOrderAt(0)->GetConditionSkipToOrder() == 1);
	CHECK(modify({}, MOF_COND_DESTINATION, 0).Failed());
	CHECK(modify(DoCommandFlag::Execute, MOF_COND_DESTINATION, 3).Failed());
	REQUIRE(modify(DoCommandFlag::Execute, MOF_COND_DESTINATION, 2).Succeeded());
	CHECK(list->GetOrderAt(0)->GetConditionSkipToOrder() == 2);

	auto cleanup = scope_guard([] { _tracerestrictslot_pool.CleanPool(); });
	TraceRestrictSlot *slot = TraceRestrictSlot::Create(owner, VehicleType::Train);
	REQUIRE(modify(DoCommandFlag::Execute, MOF_COND_VARIABLE, to_underlying(OrderConditionVariable::SlotOccupancy)).Succeeded());
	REQUIRE(modify(DoCommandFlag::Execute, MOF_COND_VALUE, slot->index.base()).Succeeded());
	REQUIRE(modify(DoCommandFlag::Execute, MOF_COND_VARIABLE, to_underlying(OrderConditionVariable::VehicleInSlot)).Succeeded());
	CHECK(list->GetOrderAt(0)->GetXData() == slot->index);
	REQUIRE(modify(DoCommandFlag::Execute, MOF_COND_VALUE, slot->index.base()).Succeeded());
	CHECK(list->GetOrderAt(0)->GetXData() == slot->index);
}

TEST_CASE_METHOD(ScheduleGroupFixture, "Station removal fixes dormant lists, jumps and suspended resume positions")
{
	AllocateMap(64, 64);
	auto cleanup = scope_guard([] { _vehicle_pool.CleanPool(); _station_pool.CleanPool(); DeallocateMap(); });
	const StationID removed{123};
	Order station;
	station.MakeGoToStation(removed);
	station.SetWaitTime(10);
	station.SetWaitTimetabled(true);
	station.SetTravelTime(20);
	station.SetTravelTimetabled(true);
	Order survivor;
	survivor.MakeGoToStation(StationID{124});
	Order implicit;
	implicit.MakeImplicit(removed);
	Order condition;
	condition.MakeConditional(2);
	condition.SetConditionVariable(OrderConditionVariable::CargoWaitingAmount);
	condition.SetConditionStationID(removed);
	condition.SetConditionViaStationID(removed);
	Order jump_to_deleted;
	jump_to_deleted.MakeConditional(1);
	jump_to_deleted.SetConditionVariable(OrderConditionVariable::Unconditionally);

	OrderList *active = Create("Detour", OrderListID::Invalid(), false);
	active->GetOrderVector() = {survivor};
	active->RecalculateDerivedCounters();
	OrderList *home = OrderList::Create();
	home->GetOrderVector() = {condition, implicit, survivor, station, jump_to_deleted};
	home->RecalculateDerivedCounters();
	OrderList *unattached = Create("Unattached detour", OrderListID::Invalid(), false);
	unattached->GetOrderVector() = {condition, implicit, survivor, station, jump_to_deleted};
	unattached->RecalculateDerivedCounters();

	Train *v = AttachTrain(active);
	v->primary_order = home->index;
	v->primary_order_index = 2;
	v->execute_schedule_stack.push_back(home->index.base() | (uint32_t{3} << 16));
	v->execute_schedule_stack.push_back(unattached->index.base() | (uint32_t{1} << 16));
	v->current_order = condition;
	Train *other = AttachTrain(active);
	other->current_order = station;
	REQUIRE(v->IsExecutingSchedule());
	REQUIRE(home->GetFirstSharedVehicle() == nullptr);

	RemoveOrderFromAllVehicles(OT_GOTO_STATION, removed);
	REQUIRE(Station::CreateAtIndex(removed, TileXY(10, 10))->index == removed);
	for (const OrderList *list : {home, unattached}) {
		REQUIRE(list->GetNumOrders() == 4);
		CHECK(list->GetOrderAt(0)->GetConditionSkipToOrder() == 1);
		CHECK(list->GetOrderAt(0)->GetConditionStationID() == StationID::Invalid());
		CHECK(list->GetOrderAt(0)->HasConditionViaStation());
		CHECK(list->GetOrderAt(0)->GetConditionViaStationID() == StationID::Invalid());
		CHECK(list->GetOrderAt(1)->GetDestination() == survivor.GetDestination());
		CHECK(list->GetOrderAt(2)->IsType(OT_DUMMY));
		CHECK(list->GetOrderAt(2)->GetWaitTime() == 0);
		CHECK(list->GetOrderAt(2)->GetTimetabledTravel() == 20);
		CHECK(list->GetTotalDuration() == 20);
		CHECK(list->GetOrderAt(3)->GetConditionSkipToOrder() == 1);
	}
	CHECK(v->current_order.GetConditionStationID() == StationID::Invalid());
	CHECK(v->current_order.GetConditionViaStationID() == StationID::Invalid());
	/* Even when the tested station is valid, an invalid via must not match
	 * the empty/unrouted cargo bucket (the default comparison is equal to 0). */
	Order via_only = v->current_order;
	via_only.SetConditionStationID(removed);
	CHECK(ProcessConditionalOrder(&via_only, v, PCO_DEFERRED) == INVALID_VEH_ORDER_ID);
	v->cargo_type = static_cast<CargoType>(0);
	v->cargo_cap = 10;
	via_only.SetConditionVariable(OrderConditionVariable::CargoWaitingAmountPercentage);
	CHECK(ProcessConditionalOrder(&via_only, v, PCO_DEFERRED) == INVALID_VEH_ORDER_ID);
	CHECK(other->current_order.IsType(OT_DUMMY));
	CHECK(v->primary_order_index == 1);
	REQUIRE(v->execute_schedule_stack.size() == 2);
	CHECK((v->execute_schedule_stack[0] >> 16) == 2);
	CHECK((v->execute_schedule_stack[1] >> 16) == 1);
}

TEST_CASE_METHOD(ScheduleGroupFixture, "Depot removal sweeps unmounted schedules in both single and batch mode")
{
	const bool batch = GENERATE(false, true);
	OrderList *list = Create("Dormant depot orders", OrderListID::Invalid(), false);
	const DepotID depot{123};
	Order fixed;
	fixed.MakeGoToDepot(depot, OrderDepotTypeFlag::PartOfOrders);
	Order nearest;
	nearest.MakeGoToDepot(DepotID::Invalid(), OrderDepotTypeFlag::PartOfOrders, ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS, ODATFB_NEAREST_DEPOT);
	list->GetOrderVector() = {fixed, nearest};
	list->RecalculateDerivedCounters();

	if (batch) StartRemoveOrderFromAllVehiclesBatch();
	RemoveOrderFromAllVehicles(OT_GOTO_DEPOT, depot);
	if (batch) {
		CHECK(list->GetFirstOrder()->IsType(OT_GOTO_DEPOT));
		StopRemoveOrderFromAllVehiclesBatch();
	}
	CHECK(list->GetFirstOrder()->IsType(OT_DUMMY));
	CHECK(list->GetLastOrder()->IsType(OT_GOTO_DEPOT));
}

TEST_CASE_METHOD(ScheduleGroupFixture, "Depot cleanup preserves typed orders on station ID collisions")
{
	AllocateMap(64, 64);
	auto cleanup = scope_guard([] { _vehicle_pool.CleanPool(); _station_pool.CleanPool(); _depot_pool.CleanPool(); DeallocateMap(); });
	const DepotID depot{123};
	Station *station = Station::CreateAtIndex(StationID{depot.base()}, TileXY(10, 10));
	Depot *ground = Depot::CreateAtIndex(depot, TileXY(20, 20));
	MakeRailDepot(ground->xy, owner, depot, DiagDirection::NE, RAILTYPE_RAIL);
	Order order;
	order.MakeGoToDepot(depot, OrderDepotTypeFlag::PartOfOrders);
	OrderList *untagged = Create("Untyped depot destination", OrderListID::Invalid(), false);
	OrderList *typed = Create("Train depot destination", OrderListID::Invalid(), false);
	for (OrderList *list : {untagged, typed}) {
		list->GetOrderVector() = {order};
		list->RecalculateDerivedCounters();
	}
	AttachTrain(typed);

	RemoveOrderFromAllVehicles(OT_GOTO_STATION, station->index);
	CHECK(typed->GetFirstOrder()->IsType(OT_GOTO_DEPOT));
	CHECK(untagged->GetFirstOrder()->IsType(OT_GOTO_DEPOT));

	/* A real hangar and ground depot sharing the number cannot be distinguished
	 * without a vehicle type. Drop only the genuinely ambiguous order. */
	station->airport.hangar = Depot::Create(TileXY(10, 10));
	RemoveOrderFromAllVehicles(OT_GOTO_DEPOT, station->index, true);
	CHECK(typed->GetFirstOrder()->IsType(OT_GOTO_DEPOT));
	CHECK(untagged->GetFirstOrder()->IsType(OT_DUMMY));
}

TEST_CASE_METHOD(ScheduleGroupFixture, "LinkGraph separates cargo by policies across list boundaries")
{
	enum class Transition { Execute, Decouple, Resume };
	const auto transition = GENERATE(Transition::Execute, Transition::Decouple, Transition::Resume);
	AllocateMap(64, 64);
	auto cleanup = scope_guard([] {
		_vehicle_pool.CleanPool();
		_station_pool.CleanPool();
		LinkGraphSchedule::Clear();
		_link_graph_pool.CleanPool();
		DeallocateMap();
	});
	Station *a = Station::Create(TileXY(10, 10));
	Station *b = Station::Create(TileXY(15, 10));
	Station *c = Station::Create(TileXY(20, 10));
	Station *d = Station::Create(TileXY(25, 10));
	auto stop = [](const Station *st) {
		Order order;
		order.MakeGoToStation(st->index);
		return order;
	};
	const auto cargo0 = static_cast<CargoType>(0);
	const auto cargo1 = static_cast<CargoType>(1);
	Order filtered_stop = stop(b);
	filtered_stop.SetLoadType(OrderLoadType::CargoTypeLoad);
	filtered_stop.SetLoadType(OrderLoadType::NoLoad, cargo0);
	filtered_stop.SetUnloadType(OrderUnloadType::CargoTypeUnload);
	filtered_stop.SetUnloadType(OrderUnloadType::NoUnload, cargo0);

	OrderList *active = Create("Active cargo route", OrderListID::Invalid(), false);
	OrderList *target = Create("Other cargo route", OrderListID::Invalid(), false);
	if (transition == Transition::Resume) {
		active->GetOrderVector() = {stop(a)};
		target->GetOrderVector() = {stop(c), filtered_stop, stop(d)};
	} else {
		Order crossing;
		if (transition == Transition::Execute) {
			crossing.MakeExecuteSchedule();
			crossing.SetDestination(target->index);
		} else {
			crossing.MakeDecouple();
			crossing.SetDecoupleSecondScheduleID(target->index);
		}
		active->GetOrderVector() = {stop(a), crossing, stop(c)};
		target->GetOrderVector() = {filtered_stop, stop(d)};
	}
	active->RecalculateDerivedCounters();
	target->RecalculateDerivedCounters();

	Train *v = AttachTrain(active);
	v->cur_implicit_order_index = 0;
	if (transition == Transition::Resume) {
		v->primary_order = target->index;
		v->primary_order_index = 1;
	}
	v->vcache.cached_max_speed = 100;
	v->cargo_type = cargo0;
	v->cargo_cap = v->refit_cap = 20;
	Train *wagon = Train::Create();
	wagon->SetWagon();
	wagon->owner = owner;
	wagon->cargo_type = cargo1;
	wagon->cargo_cap = wagon->refit_cap = 30;
	v->SetNext(wagon);

	LinkRefresher::Run(v, true, false, CargoTypes{cargo0, cargo1});
	auto capacity = [](const Station *from, const Station *to, CargoType cargo) {
		const GoodsEntry &src = from->goods[cargo];
		const GoodsEntry &dst = to->goods[cargo];
		const LinkGraph *graph = LinkGraph::GetIfValid(src.link_graph);
		return graph != nullptr && src.link_graph == dst.link_graph ? graph->GetConstEdge(src.node, dst.node).Capacity() : 0u;
	};
	CHECK(capacity(a, b, cargo0) == 0);
	CHECK(capacity(a, d, cargo0) == 20);
	CHECK(capacity(a, b, cargo1) == 30);
	CHECK(capacity(b, d, cargo1) == 30);
	CHECK(capacity(a, d, cargo1) == 0);
}
