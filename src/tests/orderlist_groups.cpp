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
#include "../core/pool_func.hpp"
#include "../core/backup_type.hpp"
#include "../train.h"

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
