/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file schedule_group_gui.h Hierarchical schedule entries shared by vehicle order pickers. */
#ifndef SCHEDULE_GROUP_GUI_H
#define SCHEDULE_GROUP_GUI_H

#include "order_base.h"
#include "string_func.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

struct ScheduleTreeEntry {
	const OrderList *order_list; ///< Schedule or group to display.
	uint depth; ///< Indentation depth in the schedule hierarchy.
};

/**
 * Build the visible schedule tree, grouping siblings before sorting them by name.
 * @param owner Company viewing the schedules.
 * @param matches Optional set of matching entries, including their ancestors.
 * @param collapsed Optional set of groups whose children should be hidden.
 * @return Visible groups and schedules in display order.
 */
inline std::vector<ScheduleTreeEntry> GetVisibleScheduleTree(Owner owner, const std::set<OrderListID> *matches = nullptr, const std::set<OrderListID> *collapsed = nullptr)
{
	std::map<OrderListID, std::vector<const OrderList *>> children;
	for (const OrderList *ol : OrderList::Iterate()) {
		if (ol->IsScheduleGroup() ? ol->GetCompany() != owner : !ol->IsPlayerCreated() || !ol->IsVisibleToCompany(owner)) continue;
		if (matches != nullptr && !matches->contains(ol->index)) continue;
		OrderListID parent = ol->GetCompany() == owner ? ol->GetScheduleGroup() : OrderListID::Invalid();
		children[parent].push_back(ol);
	}
	std::vector<ScheduleTreeEntry> result;
	std::vector<ScheduleTreeEntry> pending;
	auto append = [&](OrderListID parent, uint depth) {
		auto &items = children[parent];
		std::stable_sort(items.begin(), items.end(), [](const OrderList *a, const OrderList *b) {
			if (a->IsScheduleGroup() != b->IsScheduleGroup()) return a->IsScheduleGroup();
			int order = StrNaturalCompare(a->GetName(), b->GetName());
			return order != 0 ? order < 0 : a->index < b->index;
		});
		for (auto it = items.rbegin(); it != items.rend(); ++it) pending.push_back({*it, depth});
	};
	append(OrderListID::Invalid(), 0);
	while (!pending.empty()) {
		auto item = pending.back();
		pending.pop_back();
		result.push_back(item);
		if (item.order_list->IsScheduleGroup() && (collapsed == nullptr || !collapsed->contains(item.order_list->index))) append(item.order_list->index, item.depth + 1);
	}
	return result;
}

#endif /* SCHEDULE_GROUP_GUI_H */
