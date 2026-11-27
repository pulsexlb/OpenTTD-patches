/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file refresh.cpp Definition of link refreshing utility. */

#include "../stdafx.h"
#include "../external_city.h"
#include "../core/bitmath_func.hpp"
#include "../station_func.h"
#include "../engine_base.h"
#include "../vehicle_func.h"
#include "refresh.h"
#include "linkgraph.h"

#include "../safeguards.h"

/**
 * Make an identifier of the order lists of a hop's endpoints, so that hops
 * crossing into executed schedules are distinguished from same-index hops in
 * the vehicle's own list.
 */
static uint32_t ScheduleHopLists(const OrderList *from, const OrderList *to)
{
	return static_cast<uint32_t>((from->index.base() << 16) | to->index.base());
}

/** Resolve a schedule reference the link predictor can follow. */
static OrderList *GetLinkRefreshSchedule(OrderListID id, const OrderList *from, Owner owner)
{
	OrderList *target = OrderList::GetIfValid(id);
	return target != nullptr && target != from && target->GetNumOrders() > 0 &&
			target->IsPlayerCreated() && target->IsVisibleToCompany(owner) ? target : nullptr;
}

static OrderList *GetLinkRefreshHome(const Vehicle *v)
{
	OrderList *home = v->IsExecutingSchedule() ? v->GetPrimaryOrderList() : nullptr;
	return home != nullptr && home != v->orders && home->GetNumOrders() > 0 ? home : nullptr;
}

/** Collect cargo policies from all lists the predictor may walk. */
static std::vector<const Order *> GetReachableCargoPolicies(const Vehicle *v)
{
	std::vector<const Order *> policies;
	btree::btree_set<OrderListID> seen;
	std::vector<const OrderList *> lists;
	auto add_list = [&](const OrderList *list) {
		if (list != nullptr && seen.insert(list->index).second) lists.push_back(list);
	};
	add_list(v->orders);
	add_list(GetLinkRefreshHome(v));

	/* Scan in list/order order, with the set used only to break cycles. This
	 * conservatively includes both conditional paths and both split parts. */
	for (size_t i = 0; i < lists.size(); ++i) {
		const OrderList *list = lists[i];
		for (const Order *order : list->Orders()) {
			if (order->IsExecuteScheduleOrder()) {
				add_list(GetLinkRefreshSchedule(order->GetDestination().ToOrderListID(), list, v->owner));
			} else if (order->IsType(OT_DECOUPLE)) {
				add_list(GetLinkRefreshSchedule(order->GetDecoupleFirstScheduleID(), list, v->owner));
				add_list(GetLinkRefreshSchedule(order->GetDecoupleSecondScheduleID(), list, v->owner));
			} else if ((order->IsType(OT_GOTO_STATION) || order->IsType(OT_IMPLICIT) || order->IsExternalTripOrder()) &&
					(order->GetUnloadType() == OrderUnloadType::CargoTypeUnload || order->GetLoadType() == OrderLoadType::CargoTypeLoad)) {
				policies.push_back(order);
			}
		}
	}
	return policies;
}

/**
 * Refresh all links the given vehicle will visit.
 * @param v Vehicle to refresh links for.
 * @param allow_merge If the refresher is allowed to merge or extend link graphs.
 * @param is_full_loading If the vehicle is full loading.
 * @param cargo_mask Mask of cargoes to refresh
 */
/* static */ void LinkRefresher::Run(Vehicle *v, bool allow_merge, bool is_full_loading, CargoTypes cargo_mask)
{
	if (v->orders == nullptr || cargo_mask.None()) return;

	const CargoTypes have_cargo_mask = v->GetLastLoadingStationValidCargoMask();
	const auto policies = GetReachableCargoPolicies(v);

	/* Repeatedly extract the equivalence class of the first remaining cargo. */
	while (cargo_mask.Any()) {
		const CargoType first_cargo = cargo_mask.FindFirstBit();
		const bool has_cargo = have_cargo_mask.Test(first_cargo);
		CargoTypes iter_cargo_mask = cargo_mask & (has_cargo ? have_cargo_mask : ~have_cargo_mask);
		for (const Order *order : policies) {
			if (order->GetUnloadType() == OrderUnloadType::CargoTypeUnload) {
				CargoMaskValueFilter<OrderUnloadType>(iter_cargo_mask, [&](CargoType cargo) { return order->GetCargoUnloadType(cargo); });
			}
			if (order->GetLoadType() == OrderLoadType::CargoTypeLoad) {
				CargoMaskValueFilter<bool>(iter_cargo_mask, [&](CargoType cargo) { return order->GetCargoLoadType(cargo) == OrderLoadType::NoLoad; });
			}
		}
		assert(iter_cargo_mask.Test(first_cargo));

		/* Make sure the first order is a useful order. */
		CargoTypes first_mask = iter_cargo_mask;
		const Order *first = v->orders->GetNextDecisionNode(v->GetOrder(v->cur_implicit_order_index), 0, first_mask, true);
		assert(first_mask == iter_cargo_mask);
		if (first != nullptr) {
			HopSet seen_hops;
			LinkRefresher refresher(v, &seen_hops, allow_merge, is_full_loading, iter_cargo_mask);

			RefreshFlags flags = {};
			if (has_cargo) flags.Set(RefreshFlag::HasCargo);
			if (v->type == VehicleType::Aircraft) flags.Set(RefreshFlag::Aircraft);
			refresher.RefreshLinks(first, first, { 0, TimetableTravelTimeFlag::NoWaitTime }, flags);
		}

		cargo_mask &= ~iter_cargo_mask;
	}
}

/**
 * Constructor for link refreshing algorithm.
 * @param vehicle Vehicle to refresh links for.
 * @param seen_hops Set of hops already seen. This is shared between this
 *                  refresher and all its children.
 * @param allow_merge If the refresher is allowed to merge or extend link graphs.
 * @param is_full_loading If the vehicle is full loading.
 */
LinkRefresher::LinkRefresher(Vehicle *vehicle, HopSet *seen_hops, bool allow_merge, bool is_full_loading, CargoTypes cargo_mask) :
	vehicle(vehicle), seen_hops(seen_hops), cargo(INVALID_CARGO), allow_merge(allow_merge),
	is_full_loading(is_full_loading), cargo_mask(cargo_mask), walk_orders(vehicle->orders)
{
	/* While the vehicle is executing a schedule its own list is suspended:
	 * walking the executed list must return to the primary list at the
	 * remembered resume order when the executed list wraps, so seed the
	 * bottom return frame. Without it the junction links (last stop of the
	 * schedule to the resumed stop of the primary list) would never be
	 * refreshed for as long as the vehicle stays on the schedule. */
	OrderList *home = GetLinkRefreshHome(vehicle);
	if (home != nullptr) {
		this->home_list = home;
		const Order *resume = home->GetOrderAt(vehicle->primary_order_index);
		if (resume == nullptr) resume = home->GetFirstOrder();
		this->resume_stack.emplace_back(home, resume);
	} else {
		this->home_list = vehicle->orders;
	}

	/* Assemble list of capacities and set last loading stations to 0.
	 * The primary may sit mid-chain or at the physical tail (decoupled parts
	 * driving away reversed), so start at the physical head to cover the
	 * whole consist; otherwise the capacities stay empty and RefreshStats
	 * would skip every cargo. */
	for (Vehicle *v = this->vehicle->First(); v != nullptr; v = v->Next()) {
		this->refit_capacities.push_back(RefitDesc(v->cargo_type, v->cargo_cap, v->refit_cap));
		if (v->refit_cap > 0) {
			assert(v->cargo_type < NUM_CARGO);
			this->capacities[v->cargo_type] += v->refit_cap;
		}
	}
}

/**
 * Handle refit orders by updating capacities and refit_capacities.
 * @param refit_cargo Cargo to refit to.
 * @return True if any vehicle was refit; false if none was.
 */
bool LinkRefresher::HandleRefit(CargoType refit_cargo)
{
	this->cargo = refit_cargo;
	RefitList::iterator refit_it = this->refit_capacities.begin();
	bool any_refit = false;
	/* See the constructor: cover the whole physical consist. */
	for (Vehicle *v = this->vehicle->First(); v != nullptr; v = v->Next()) {
		const Engine *e = Engine::Get(v->engine_type);
		if (!e->info.refit_mask.Test(this->cargo)) {
			++refit_it;
			continue;
		}
		any_refit = true;

		/* Back up the vehicle's cargo type */
		CargoType temp_cargo_type = v->cargo_type;
		uint8_t temp_subtype = v->cargo_subtype;
		v->cargo_type = this->cargo;
		if (e->refit_capacity_values == nullptr || !(e->callbacks_used & SGCU_REFIT_CB_ALL_CARGOES) || this->cargo == e->GetDefaultCargoType() || (e->type == VehicleType::Aircraft && IsCargoInClass(this->cargo, CargoClass::Passengers))) {
			/* This can be omitted when the refit capacity values are already determined, and the capacity is definitely from the refit callback */
			v->cargo_subtype = GetBestFittingSubType(v, v, this->cargo);
		}

		uint16_t mail_capacity = 0;
		uint amount = e->DetermineCapacity(v, &mail_capacity);

		/* Restore the original cargo type */
		v->cargo_type = temp_cargo_type;
		v->cargo_subtype = temp_subtype;

		/* Skip on next refit. */
		if (this->cargo != refit_it->cargo && refit_it->remaining > 0) {
			this->capacities[refit_it->cargo] -= refit_it->remaining;
			refit_it->remaining = 0;
		} else if (amount < refit_it->remaining) {
			this->capacities[refit_it->cargo] -= refit_it->remaining - amount;
			refit_it->remaining = amount;
		}
		refit_it->capacity = amount;
		refit_it->cargo = this->cargo;

		++refit_it;

		/* Special case for aircraft with mail. */
		if (v->type == VehicleType::Aircraft) {
			if (mail_capacity < refit_it->remaining) {
				this->capacities[refit_it->cargo] -= refit_it->remaining - mail_capacity;
				refit_it->remaining = mail_capacity;
			}
			refit_it->capacity = mail_capacity;
			break; // aircraft have only one vehicle
		}
	}
	return any_refit;
}

/**
 * Restore capacities and refit_capacities as vehicle might have been able to load now.
 */
void LinkRefresher::ResetRefit()
{
	for (auto &it : this->refit_capacities) {
		if (it.remaining == it.capacity) continue;
		this->capacities[it.cargo] += it.capacity - it.remaining;
		it.remaining = it.capacity;
	}
}

/**
 * Update the linear timetable travel time with the times between two orders.
 * The caller is responsible for ensuring that these orders are in a linear sequence.
 * @param from Start order.
 * @param to End order.
 * @param travel Travel time so far.
 * @return Updated travel time.
 */
LinkRefresher::TimetableTravelTime LinkRefresher::UpdateTimetableTravelSoFar(const Order *from, const Order *to, LinkRefresher::TimetableTravelTime travel)
{
	if (from == to || from == nullptr || to == nullptr || travel.flags.Test(TimetableTravelTimeFlag::Invalid)) return travel;

	do {
		if (from->IsType(OT_CONDITIONAL)) {
			if (from->GetConditionVariable() == OrderConditionVariable::Unconditionally) {
				/* Taken branch travel time */
				travel.time_so_far += from->GetWaitTime();
				from = this->walk_orders->GetOrderAt(from->GetConditionSkipToOrder());
				travel.flags.Set(TimetableTravelTimeFlag::NoTravelTime);
			} else if (!travel.flags.Test(TimetableTravelTimeFlag::AllowCondition)) {
				/* Unexpected conditional branch, give up */
				travel.flags.Set(TimetableTravelTimeFlag::Invalid);
				return travel;
			} else {
				/* Non-taken branch, ignore travel time field */
				from = this->walk_orders->GetNext(from);
				travel.flags.Reset(TimetableTravelTimeFlag::NoTravelTime);
			}
		} else {
			if (!travel.flags.Test(TimetableTravelTimeFlag::NoWaitTime)) {
				if (from->IsScheduledDispatchOrder(true)) {
					travel.flags.Set(TimetableTravelTimeFlag::Invalid);
					return travel;
				}
				travel.time_so_far += from->GetWaitTime();
			}
			from = this->walk_orders->GetNext(from);
			travel.flags.Reset(TimetableTravelTimeFlag::NoTravelTime);
		}

		travel.flags.Reset(TimetableTravelTimeFlag::NoWaitTime);
		travel.flags.Reset(TimetableTravelTimeFlag::AllowCondition);

		if (!from->IsType(OT_CONDITIONAL) && !travel.flags.Test(TimetableTravelTimeFlag::NoTravelTime)) {
			if (from->GetTravelTime() == 0 && !from->IsTravelTimetabled() && !from->IsType(OT_IMPLICIT)) {
				travel.flags.Set(TimetableTravelTimeFlag::Invalid);
				return travel;
			}
			travel.time_so_far += from->GetTravelTime();
		}

		travel.flags.Reset(TimetableTravelTimeFlag::NoTravelTime);
	} while (from != to);

	return travel;
}

/**
 * Predict the next order the vehicle will execute and resolve conditionals by
 * recursion and return next non-conditional order in list.
 * @param cur Current order being evaluated.
 * @param next Next order to be evaluated.
 * @param travel Travel time so far.
 * @param flags RefreshFlags to give hints about the previous link and state carried over from that.
 * @param num_hops Number of hops already taken by recursive calls to this method.
 * @return new next Order, and travel time so far.
 */
std::pair<const Order *, LinkRefresher::TimetableTravelTime> LinkRefresher::PredictNextOrder(const Order *cur, const Order *next, LinkRefresher::TimetableTravelTime travel, RefreshFlags flags, uint num_hops)
{
	/* next is good if it's either nullptr (then the caller will stop the
	 * evaluation) or if it's not conditional and the caller allows it to be
	 * chosen (by setting RefreshFlag::UseNext). */
	while (next != nullptr && (!flags.Test(RefreshFlag::UseNext) || next->IsType(OT_CONDITIONAL))) {

		/* After the first step any further non-conditional order is good,
		 * regardless of previous RefreshFlag::UseNext settings. The case of cur and next or
		 * their respective stations being equal is handled elsewhere. */
		flags.Set(RefreshFlag::UseNext);

		if (next->IsType(OT_CONDITIONAL)) {
			if (next->GetConditionVariable() == OrderConditionVariable::Unconditionally) {
				const Order *current = next;
				CargoTypes this_cargo_mask = this->cargo_mask;
				next = this->walk_orders->GetNextDecisionNode(
						this->walk_orders->GetOrderAt(next->GetConditionSkipToOrder()),
						num_hops++, this_cargo_mask, true);
				assert(this_cargo_mask == this->cargo_mask);
				travel = this->UpdateTimetableTravelSoFar(current, next, travel);
				continue;
			}
			CargoTypes this_cargo_mask = this->cargo_mask;
			const Order *target = this->walk_orders->GetOrderAt(next->GetConditionSkipToOrder());
			const Order *skip_to = this->walk_orders->GetNextDecisionNode(target, num_hops, this_cargo_mask, true);
			assert(this_cargo_mask == this->cargo_mask);
			if (skip_to != nullptr && num_hops < std::min<uint>(64, this->walk_orders->GetNumOrders()) && skip_to != next) {
				/* Make copies of capacity tracking lists. There is potential
				 * for optimization here: If the vehicle never refits we don't
				 * need to copy anything. */

				/* Record the branch before executing it,
				 * to avoid recursively executing it again. */
				Hop hop(this->walk_orders->GetIndexOfOrder(cur), this->walk_orders->GetIndexOfOrder(skip_to), this->cargo, flags, ScheduleHopLists(this->walk_orders, this->walk_orders));
				auto iter = this->seen_hops->lower_bound(hop);
				if (iter == this->seen_hops->end() || *iter != hop) {
					this->seen_hops->insert(iter, hop);
					TimetableTravelTime branch_travel = travel;
					branch_travel.time_so_far += next->GetWaitTime();
					branch_travel.flags.Set(TimetableTravelTimeFlag::NoTravelTime);
					LinkRefresher branch(*this);
					branch.RefreshLinks(cur, skip_to, this->UpdateTimetableTravelSoFar(target, skip_to, branch_travel), flags, num_hops + 1);
				}
			}

			travel.time_so_far += next->GetWaitTime();
		}

		/* Reassign next with the following stop. This can be a station or a
		 * depot.*/
		CargoTypes this_cargo_mask = this->cargo_mask;
		const Order *current = next;
		const Order *following = this->walk_orders->GetNext(current);
		bool list_changed = false;
		if (!this->resume_stack.empty() && this->walk_orders != this->home_list
				&& this->walk_orders->GetIndexOfOrder(following) == 0) {
			/* One full pass of the executed schedule is finished: the vehicle
			 * returns to its primary order list (ReturnFromExecuteSchedule uses
			 * the outermost home, not the immediately suspended list), so
			 * unwind the whole stack and resume at the bottom entry. This also
			 * applies when the run started in the middle of an executed
			 * schedule, whose return frame was seeded in the constructor. */
			this->walk_orders = this->resume_stack.front().first;
			following = this->resume_stack.front().second;
			this->resume_stack.clear();
			list_changed = true;
		}
		if (following->IsType(OT_CONDITIONAL) && following->GetConditionVariable() == OrderConditionVariable::Unconditionally) {
			/* An unconditional jump straight back to an execute-schedule order
			 * (the repeat loop of a decouple schedule wrapper) restarts the
			 * executed schedule. Follow it to the execute order so the jump
			 * below is taken; GetNextDecisionNode cannot cross lists. */
			const Order *skip_to = this->walk_orders->GetOrderAt(following->GetConditionSkipToOrder());
			if (skip_to != nullptr && skip_to->IsExecuteScheduleOrder() && skip_to != following) following = skip_to;
		}
		if (following->IsExecuteScheduleOrder() && this->resume_stack.size() < 8) {
			/* The vehicle will jump into the target schedule: continue the
			 * prediction there, suspending the current list. The depth limit
			 * only guards against runaway recursion; cycles in the executed
			 * schedules are broken by the seen-hop check. */
			OrderList *target = GetLinkRefreshSchedule(following->GetDestination().ToOrderListID(), this->walk_orders, this->vehicle->owner);
			if (target != nullptr) {
				this->resume_stack.emplace_back(this->walk_orders, this->walk_orders->GetNext(following));
				this->walk_orders = target;
				following = target->GetFirstOrder();
				list_changed = true;
			}
		}
		if (following->IsType(OT_DECOUPLE)) {
			/* The consist splits here: each part may adopt a player-created
			 * schedule afterwards. Refresh cargo links into both referenced
			 * schedules while the whole consist is still standing at this
			 * station, so the schedules' own stations are part of the link
			 * graph even before the split happens. The front part keeps
			 * walking this list below (GetNextDecisionNode skips the decouple
			 * order), so the stations after the decouple are still visited. */
			for (OrderListID schedule_id : { following->GetDecoupleFirstScheduleID(), following->GetDecoupleSecondScheduleID() }) {
				OrderList *target = GetLinkRefreshSchedule(schedule_id, this->walk_orders, this->vehicle->owner);
				if (target == nullptr) continue;

				/* Skip a leading stop at the station the consist currently stands at. */
				const Order *start = target->GetFirstOrder();
				if (cur->IsType(OT_GOTO_STATION) || cur->IsType(OT_IMPLICIT)) {
					const StationID cur_station = cur->GetDestination().ToStationID();
					for (uint skipped = 0; skipped < target->GetNumOrders(); ++skipped) {
						if (!(start->IsType(OT_GOTO_STATION) || start->IsType(OT_IMPLICIT))) break;
						if (start->GetDestination() != cur_station) break;
						start = target->GetNext(start);
					}
				}

				CargoTypes sched_cargo_mask = this->cargo_mask;
				const Order *first_target = target->GetNextDecisionNode(start, num_hops, sched_cargo_mask, true);
				assert(sched_cargo_mask == this->cargo_mask);
				if (first_target == nullptr) continue;

				LinkRefresher branch(*this);
				branch.walk_orders = target;
				branch.RefreshLinks(cur, first_target, { 0, TimetableTravelTimeFlag::NoWaitTime }, flags, num_hops + 1);
			}
		}
		next = this->walk_orders->GetNextDecisionNode(following, num_hops++, this_cargo_mask, true);
		assert(this_cargo_mask == this->cargo_mask);

		if (list_changed) {
			/* "current" and "next" live in different order lists now, so there
			 * is no meaningful travel time between them. */
			travel.flags.Set(TimetableTravelTimeFlag::Invalid);
		} else {
			travel.flags.Set(TimetableTravelTimeFlag::AllowCondition);
			travel = this->UpdateTimetableTravelSoFar(current, next, travel);
		}
	}
	return std::make_pair(next, travel);
}

/**
 * Refresh link stats for the given pair of orders.
 * @param cur Last stop where the consist could interact with cargo.
 * @param next Next order to be processed.
 * @param travel_estimate Estimated travel time, only valid if non-zero.
 * @param flags RefreshFlags to give hints about the previous link and state carried over from that.
 */
void LinkRefresher::RefreshStats(const Order *cur, const Order *next, uint32_t travel_estimate, RefreshFlags flags)
{
	StationID next_station = next->IsExternalTripOrder() ? GetExternalOrderStation(next) : next->GetDestination().ToStationID();
	Station *st = Station::GetIfValid(cur->IsExternalTripOrder() ? GetExternalOrderStation(cur) : cur->GetDestination().ToStationID());
	Station *st_to = Station::GetIfValid(next_station);
	if (st != nullptr && st_to != nullptr && next_station != StationID::Invalid() && next_station != st->index) {
		for (CargoType cargo : EnumRange(NUM_CARGO)) {
			/* Refresh the link and give it a minimum capacity. */

			if (!this->cargo_mask.Test(cargo)) continue;

			uint cargo_quantity = this->capacities[cargo];
			if (cargo_quantity == 0) continue;

			if (this->vehicle->GetDisplayMaxSpeed() == 0) continue;

			/* If not allowed to merge link graphs, make sure the stations are
			 * already in the same link graph. */
			if (!this->allow_merge && st->goods[cargo].link_graph != st_to->goods[cargo].link_graph) {
				continue;
			}

			/* A link is at least partly restricted if a vehicle can't load at its source. */
			EdgeUpdateModes restricted_modes = (cur->GetCargoLoadType(cargo) != OrderLoadType::NoLoad) ?
						EdgeUpdateMode::Unrestricted : EdgeUpdateMode::Restricted;
			/* This estimates the travel time of the link as the time needed
			 * to travel between the stations at half the max speed of the consist.
			 * The result is in tiles/tick (= 2048 km-ish/h). */
			uint32_t time_estimate = DistanceManhattan(st->xy, st_to->xy) * 4096U / this->vehicle->GetDisplayMaxSpeed();

			if (travel_estimate > 0) {
				/* If a timetable-based time is available, use that, clamping it to be in the range (estimate / 3, estimate * 2)
				 * of the distance/speed based estimate.
				 * This is effectively clamping it to be within the estimated speed range: (max_speed / 4, max_speed * 1.5). */
				time_estimate = Clamp<uint32_t>(travel_estimate, time_estimate / 3, time_estimate * 2);
			}

			if (flags.Test(RefreshFlag::Aircraft)) restricted_modes.Set(EdgeUpdateMode::Aircraft);

			/* If the vehicle is currently full loading, increase the capacities at the station
			 * where it is loading by an estimate of what it would have transported if it wasn't
			 * loading. Don't do that if the vehicle has been waiting for longer than the entire
			 * order list is supposed to take, though. If that is the case the total duration is
			 * probably far off and we'd greatly overestimate the capacity by increasing.*/
			if (this->is_full_loading && this->vehicle->orders != nullptr &&
					st->index == vehicle->last_station_visited &&
					this->vehicle->orders->GetTotalDuration() >
					(Ticks)this->vehicle->current_order_time) {
				uint effective_capacity = cargo_quantity * this->vehicle->load_unload_ticks;
				if (effective_capacity > (uint)this->vehicle->orders->GetTotalDuration()) {
					IncreaseStats(st, cargo, next_station, effective_capacity /
							this->vehicle->orders->GetTotalDuration(), 0, 0,
							EdgeUpdateModes{EdgeUpdateMode::Increase} | restricted_modes);
				} else if (RandomRange(this->vehicle->orders->GetTotalDuration()) < effective_capacity) {
					IncreaseStats(st, cargo, next_station, 1, 0, 0, EdgeUpdateModes{EdgeUpdateMode::Increase} | restricted_modes);
				} else {
					IncreaseStats(st, cargo, next_station, cargo_quantity, 0, time_estimate, EdgeUpdateModes{EdgeUpdateMode::Refresh} | restricted_modes);
				}
			} else {
				IncreaseStats(st, cargo, next_station, cargo_quantity, 0, time_estimate, EdgeUpdateModes{EdgeUpdateMode::Refresh} | restricted_modes);
			}
		}
	}
}

/**
 * Iterate over orders starting at \a cur and \a next and refresh links
 * associated with them. \a cur and \a next can be equal. If they're not they
 * must be "neighbours" in their order list, which means \a next must be directly
 * reachable from \a cur without passing any further OT_GOTO_STATION or
 * OT_IMPLICIT orders in between.
 * @param cur Current order being evaluated.
 * @param next Next order to be checked.
 * @param travel Travel time so far.
 * @param flags RefreshFlags to give hints about the previous link and state carried over from that.
 * @param num_hops Number of hops already taken by recursive calls to this method.
 */
void LinkRefresher::RefreshLinks(const Order *cur, const Order *next, TimetableTravelTime travel, RefreshFlags flags, uint num_hops)
{
	while (next != nullptr) {

		if ((next->IsType(OT_GOTO_DEPOT) || next->IsType(OT_GOTO_STATION)) && next->IsRefit()) {
			flags.Set(RefreshFlag::WasRefit);
			if (!next->IsAutoRefit()) {
				this->HandleRefit(next->GetRefitCargo());
			} else if (!flags.Test(RefreshFlag::InAutorefit)) {
				flags.Set(RefreshFlag::InAutorefit);
				LinkRefresher backup(*this);
				for (CargoType cargo : EnumRange(NUM_CARGO)) {
					if (!CargoSpec::Get(cargo)->IsValid()) continue;
					if (next->GetCargoLoadType(cargo) == OrderLoadType::NoLoad) continue;
					if (this->HandleRefit(cargo)) {
						this->RefreshLinks(cur, next, travel, flags, num_hops);
						*this = backup;
					}
				}
			}
		}

		/* Only reset the refit capacities if the "previous" next is a station,
		 * meaning that either the vehicle was refit at the previous station or
		 * it wasn't at all refit during the current hop. */
		if (flags.Test(RefreshFlag::WasRefit) && (next->IsType(OT_GOTO_STATION) || next->IsType(OT_IMPLICIT))) {
			flags.Set(RefreshFlag::ResetRefit);
		} else {
			flags.Reset(RefreshFlag::ResetRefit);
		}

		std::tie(next, travel) = this->PredictNextOrder(cur, next, travel, flags, num_hops);
		if (next == nullptr && !this->resume_stack.empty()) {
			/* The executed schedule had no stops at all: the vehicle passes
			 * through it and resumes its primary order list. */
			this->walk_orders = this->resume_stack.front().first;
			next = this->resume_stack.front().second;
			this->resume_stack.clear();
		}
		if (next == nullptr) break;
		/* "cur" may still belong to the vehicle's own list right after jumping
		 * into an executed schedule, so look up its index in the list holding it. */
		OrderList *cur_orders = (this->walk_orders->GetIndexOfOrder(cur) != INVALID_VEH_ORDER_ID) ? this->walk_orders : this->vehicle->orders;
		Hop hop(cur_orders->GetIndexOfOrder(cur), this->walk_orders->GetIndexOfOrder(next), this->cargo, {},
				ScheduleHopLists(cur_orders, this->walk_orders));
		auto iter = this->seen_hops->lower_bound(hop);
		if (iter != this->seen_hops->end() && *iter == hop) {
			break;
		} else {
			this->seen_hops->insert(iter, hop);
		}

		/* Don't use the same order again, but choose a new one in the next round. */
		flags.Reset(RefreshFlag::UseNext);

		/* Skip resetting and link refreshing if next order won't do anything with cargo. */
		if (!next->IsType(OT_GOTO_STATION) && !next->IsType(OT_IMPLICIT) && !next->IsExternalTripOrder()) continue;

		if (flags.Test(RefreshFlag::ResetRefit)) {
			this->ResetRefit();
			flags.Reset({RefreshFlag::ResetRefit, RefreshFlag::WasRefit});
		}

		if (cur->IsType(OT_GOTO_STATION) || cur->IsType(OT_IMPLICIT) || cur->IsExternalTripOrder()) {
			if (cur->CanLeaveWithCargo(flags.Test(RefreshFlag::HasCargo), this->cargo_mask.FindFirstBit())) {
				flags.Set(RefreshFlag::HasCargo);
				this->RefreshStats(cur, next, (!travel.flags.Test(TimetableTravelTimeFlag::Invalid) && travel.time_so_far > 0) ? (uint32_t)travel.time_so_far : 0, flags);
			} else {
				flags.Reset(RefreshFlag::HasCargo);
			}
		}

		/* "cur" is only assigned here if the stop is a station so that
		 * whenever stats are to be increased two stations can be found. */
		cur = next;
		travel = { 0, TimetableTravelTimeFlag::NoWaitTime };
	}
}
