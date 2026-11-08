/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file autoreplace_cmd.cpp Deals with autoreplace execution but not the setup. */

#include "stdafx.h"
#include "company_func.h"
#include "train.h"
#include "command_func.h"
#include "engine_func.h"
#include "vehicle_func.h"
#include "autoreplace_cmd.h"
#include "autoreplace_func.h"
#include "autoreplace_gui.h"
#include "articulated_vehicles.h"
#include "tracerestrict.h"
#include "core/bitmath_func.hpp"
#include "core/random_func.hpp"
#include "vehiclelist.h"
#include "road.h"
#include "ai/ai.hpp"
#include "news_func.h"
#include "strings_func.h"
#include "group_cmd.h"
#include "order_cmd.h"
#include "train_cmd.h"
#include "vehicle_cmd.h"
#include "script/api/script_event_types.hpp"

#include "table/strings.h"

#include "safeguards.h"

extern void ChangeVehicleViewports(VehicleID from_index, VehicleID to_index);
extern void ChangeVehicleNews(VehicleID from_index, VehicleID to_index);

/** Resolve a multi-part unit member to its base/main part for autoreplace. */
static Train *GetAutoreplaceUnitStart(Vehicle *v)
{
	if (v == nullptr || v->type != VehicleType::Train) return nullptr;
	Train *t = Train::From(v);
	/* Keep dual-headed halves separate here: the rear half is handled/skipped
	 * by GetNewEngineType(), and resolving it back to the front would loop. */
	if (t->IsMultiheaded()) return t;
	return Train::From(t->GetUnitMain());
}

/**
 * Figure out if two engines got at least one type of cargo in common (refitting if needed)
 * @param engine_a one of the EngineIDs
 * @param engine_b the other EngineID
 * @return true if they can both carry the same type of cargo (or at least one of them got no capacity at all)
 */
static bool EnginesHaveCargoInCommon(EngineID engine_a, EngineID engine_b)
{
	CargoTypes available_cargoes_a = GetUnionOfArticulatedRefitMasks(engine_a, true);
	CargoTypes available_cargoes_b = GetUnionOfArticulatedRefitMasks(engine_b, true);
	return available_cargoes_a.None() || available_cargoes_b.None() || available_cargoes_a.Any(available_cargoes_b);
}

/**
 * Checks some basic properties whether autoreplace is allowed
 * @param from Origin engine
 * @param to Destination engine
 * @param company Company to check for
 * @return true if autoreplace is allowed
 */
bool CheckAutoreplaceValidity(EngineID from, EngineID to, CompanyID company)
{
	assert(Engine::IsValidID(from) && Engine::IsValidID(to));

	const Engine *e_from = Engine::Get(from);
	const Engine *e_to = Engine::Get(to);
	VehicleType type = e_from->type;

	/* check that the new vehicle type is available to the company and its type is the same as the original one */
	if (!IsEngineBuildable(to, type, company)) return false;

	switch (type) {
		case VehicleType::Train: {
			/* make sure the railtypes are compatible */
			if (!GetAllCompatibleRailTypes(e_from->VehInfo<RailVehicleInfo>().railtypes).Any(GetAllCompatibleRailTypes(e_to->VehInfo<RailVehicleInfo>().railtypes))) return false;

			/* make sure we do not replace wagons with engines or vice versa */
			if ((e_from->VehInfo<RailVehicleInfo>().railveh_type == RailVehicleType::Wagon) != (e_to->VehInfo<RailVehicleInfo>().railveh_type == RailVehicleType::Wagon)) return false;
			break;
		}

		case VehicleType::Road:
			/* make sure the roadtypes are compatible */
			if (!GetRoadTypeInfo(e_from->VehInfo<RoadVehicleInfo>().roadtype)->powered_roadtypes.Any(GetRoadTypeInfo(e_to->VehInfo<RoadVehicleInfo>().roadtype)->powered_roadtypes)) return false;

			/* make sure that we do not replace a tram with a normal road vehicles or vice versa */
			if (e_from->info.misc_flags.Test(EngineMiscFlag::RoadIsTram) != e_to->info.misc_flags.Test(EngineMiscFlag::RoadIsTram)) return false;
			break;

		case VehicleType::Aircraft:
			/* make sure that we do not replace a plane with a helicopter or vice versa */
			if ((e_from->VehInfo<AircraftVehicleInfo>().subtype & AIR_CTOL) != (e_to->VehInfo<AircraftVehicleInfo>().subtype & AIR_CTOL)) return false;
			break;

		default: break;
	}

	/* the engines needs to be able to carry the same cargo */
	return EnginesHaveCargoInCommon(from, to);
}

/**
 * Check the capacity of all vehicles in a chain and spread cargo if needed.
 * @param v The vehicle to check.
 * @pre You can only do this if the consist is not loading or unloading. It
 *      must not carry reserved cargo, nor cargo to be unloaded or transferred.
 */
void CheckCargoCapacity(Vehicle *v)
{
	assert(v == nullptr || v->First() == v);

	for (Vehicle *src = v; src != nullptr; src = src->Next()) {
		assert(src->cargo.TotalCount() == src->cargo.ActionCount(VehicleCargoList::MoveToAction::Keep));

		/* Do we need to more cargo away? */
		if (src->cargo.TotalCount() <= src->cargo_cap) continue;

		/* We need to move a particular amount. Try that on the other vehicles. */
		uint to_spread = src->cargo.TotalCount() - src->cargo_cap;
		for (Vehicle *dest = v; dest != nullptr && to_spread != 0; dest = dest->Next()) {
			assert(dest->cargo.TotalCount() == dest->cargo.ActionCount(VehicleCargoList::MoveToAction::Keep));
			if (dest->cargo.TotalCount() >= dest->cargo_cap || dest->cargo_type != src->cargo_type) continue;

			uint amount = std::min(to_spread, dest->cargo_cap - dest->cargo.TotalCount());
			src->cargo.Shift(amount, &dest->cargo);
			to_spread -= amount;
		}

		/* Any left-overs will be thrown away, but not their feeder share. */
		if (src->cargo_cap < src->cargo.TotalCount()) src->cargo.Truncate(src->cargo.TotalCount() - src->cargo_cap);
	}
}

/**
 * Transfer cargo from a single (articulated )old vehicle to the new vehicle chain
 * @param old_veh Old vehicle that will be sold
 * @param new_head Head of the completely constructed new vehicle chain
 * @param part_of_chain The vehicle is part of a train
 * @pre You can only do this if both consists are not loading or unloading.
 *      They must not carry reserved cargo, nor cargo to be unloaded or
 *      transferred.
 */
static void TransferCargo(Vehicle *old_veh, Vehicle *new_head, bool part_of_chain)
{
	assert(!part_of_chain || new_head->IsPrimaryVehicle());
	/* Cargo belongs to the purchased unit, whose base may follow its parts.
	 * Dual halves are separate physical ranges, possibly separated by wagons. */
	auto unit_members = [](Vehicle *v) {
		std::vector<Vehicle *> members;
		if (v->type == VehicleType::Train) {
			Train *unit = Train::From(v);
			auto append = [&members](Train *half) {
				Train *last = half->GetUnitChainLast();
				for (Train *part = half->GetUnitChainFront(); part != nullptr; part = part->Next()) {
					if (std::ranges::find(members, part) == members.end()) members.push_back(part);
					if (part == last) break;
				}
			};
			append(unit);
			if (unit->IsMultiheaded() && unit->other_multiheaded_part != nullptr) append(unit->other_multiheaded_part);
		} else {
			for (; v != nullptr; v = v->Next()) members.push_back(v);
		}
		return members;
	};

	std::vector<Vehicle *> targets;
	if (part_of_chain) {
		for (Vehicle *dest = new_head->First(); dest != nullptr; dest = dest->Next()) targets.push_back(dest);
	} else {
		targets = unit_members(new_head);
	}
	for (Vehicle *src : unit_members(old_veh)) {
		assert(src->cargo.TotalCount() == src->cargo.ActionCount(VehicleCargoList::MoveToAction::Keep));
		if (src->cargo_type >= NUM_CARGO || src->cargo.TotalCount() == 0) continue;

		for (Vehicle *dest : targets) {
			if (src->cargo.TotalCount() == 0) break;
			assert(dest->cargo.TotalCount() == dest->cargo.ActionCount(VehicleCargoList::MoveToAction::Keep));
			if (dest == src || dest->cargo_type != src->cargo_type || dest->cargo.TotalCount() >= dest->cargo_cap) continue;

			uint amount = std::min(src->cargo.TotalCount(), dest->cargo_cap - dest->cargo.TotalCount());
			if (amount == 0) continue;
			src->cargo.Shift(amount, &dest->cargo);
		}
	}

	/* Update train weight etc., the old vehicle will be sold anyway */
	if (part_of_chain && new_head->type == VehicleType::Train) Train::From(new_head)->First()->ConsistChanged(CCF_LOADUNLOAD);
}

/**
 * Tests whether refit orders that applied to v will also apply to the new vehicle type
 * @param v The vehicle to be replaced
 * @param engine_type The type we want to replace with
 * @return true iff all refit orders stay valid
 */
static bool VerifyAutoreplaceRefitForOrders(const Vehicle *v, EngineID engine_type)
{
	CargoTypes union_refit_mask_a = GetUnionOfArticulatedRefitMasks(v->engine_type, false);
	CargoTypes union_refit_mask_b = GetUnionOfArticulatedRefitMasks(engine_type, false);

	const Vehicle *u = (v->type == VehicleType::Train) ? v->Primary() : v;
	for (const Order *o : u->Orders()) {
		if (!o->IsRefit() || o->IsAutoRefit()) continue;
		CargoType cargo_type = o->GetRefitCargo();

		if (!union_refit_mask_a.Test(cargo_type)) continue;
		if (!union_refit_mask_b.Test(cargo_type)) return false;
	}

	return true;
}

/**
 * Gets the index of the first refit order that is incompatible with the requested engine type
 * @param v The vehicle to be replaced
 * @param engine_type The type we want to replace with
 * @return index of the incompatible order or -1 if none were found
 */
static int GetIncompatibleRefitOrderIdForAutoreplace(const Vehicle *v, EngineID engine_type)
{
	CargoTypes union_refit_mask = GetUnionOfArticulatedRefitMasks(engine_type, false);

	const Vehicle *u = (v->type == VehicleType::Train) ? v->Primary() : v;

	const OrderList *orders = u->orders;
	if (orders == nullptr) return -1;
	for (VehicleOrderID i = 0; i < orders->GetNumOrders(); i++) {
		const Order *o = orders->GetOrderAt(i);
		if (!o->IsRefit()) continue;
		if (!union_refit_mask.Test(o->GetRefitCargo())) return i;
	}

	return -1;
}

/**
 * Function to find what type of cargo to refit to when autoreplacing
 * @param *v Original vehicle that is being replaced.
 * @param engine_type The EngineID of the vehicle that is being replaced to
 * @param part_of_chain The vehicle is part of a train
 * @return The cargo type to replace to
 *    CARGO_NO_REFIT is returned if no refit is needed
 *    INVALID_CARGO is returned when both old and new vehicle got cargo capacity and refitting the new one to the old one's cargo type isn't possible
 */
static CargoType GetNewCargoTypeForReplace(const Vehicle *v, EngineID engine_type, bool part_of_chain)
{
	CargoTypes available_cargo_types, union_mask;
	GetArticulatedRefitMasks(engine_type, true, &union_mask, &available_cargo_types);

	if (union_mask.None()) return CARGO_NO_REFIT; // Don't try to refit an engine with no cargo capacity

	CargoType cargo_type;
	CargoTypes cargo_mask = GetCargoTypesOfArticulatedVehicle(v, &cargo_type);
	if (!HasAtMostOneBit(cargo_mask.base())) {
		CargoTypes new_engine_default_cargoes = GetCargoTypesOfArticulatedParts(engine_type);
		if ((cargo_mask & new_engine_default_cargoes) == cargo_mask) {
			return CARGO_NO_REFIT; // engine_type is already a mixed cargo type which matches the incoming vehicle by default, no refit required
		}

		return INVALID_CARGO; // We cannot refit to mixed cargoes in an automated way
	}

	if (cargo_type == INVALID_CARGO) {
		if (v->type != VehicleType::Train) return CARGO_NO_REFIT; // If the vehicle does not carry anything at all, every replacement is fine.

		if (!part_of_chain) return CARGO_NO_REFIT;

		/* the old engine didn't have cargo capacity, but the new one does
		 * now we will figure out what cargo the train is carrying and refit to fit this */

		for (v = v->First(); v != nullptr; v = v->Next()) {
			if (!v->GetEngine()->CanCarryCargo()) continue;
			/* Now we found a cargo type being carried on the train and we will see if it is possible to carry to this one */
			if (available_cargo_types.Test(v->cargo_type)) return v->cargo_type;
		}

		return CARGO_NO_REFIT; // We failed to find a cargo type on the old vehicle and we will not refit the new one
	} else {
		if (!available_cargo_types.Test(cargo_type)) return INVALID_CARGO; // We can't refit the vehicle to carry the cargo we want

		if (part_of_chain && !VerifyAutoreplaceRefitForOrders(v, engine_type)) return INVALID_CARGO; // Some refit orders lose their effect

		return cargo_type;
	}
}

/**
 * Get the EngineID of the replacement for a vehicle
 * @param v The vehicle to find a replacement for
 * @param c The vehicle's owner (it's faster to forward the pointer than refinding it)
 * @param always_replace Always replace, even if not old.
 * @param same_type_only Only replace with same engine type.
 * @param[out] e the EngineID of the replacement. EngineID::Invalid() if no replacement is found
 * @return Error if the engine to build is not available
 */
static CommandCost GetNewEngineType(const Vehicle *v, const Company *c, bool always_replace, bool same_type_only, EngineID &e)
{
	if (v->type == VehicleType::Train && v->IsArticulatedPart()) {
		/* Callers must resolve to the unit main. Do not abort the game if a
		 * malformed/older save still reaches this point. */
		return CMD_ERROR;
	}

	e = EngineID::Invalid();

	if (v->type == VehicleType::Train && Train::From(v)->IsRearDualheaded()) {
		/* we build the rear ends of multiheaded trains with the front ones */
		return CommandCost();
	}

	if (!same_type_only) {
		bool replace_when_old;
		e = EngineReplacementForCompany(c, v->engine_type, v->group_id, &replace_when_old);
		if (!always_replace && replace_when_old && !v->NeedsAutorenewing(c, false)) e = EngineID::Invalid();
	}

	/* Autoreplace, if engine is available */
	if (e != EngineID::Invalid() && IsEngineBuildable(e, v->type, _current_company)) {
		return CommandCost();
	}

	/* Autorenew if needed */
	if (v->NeedsAutorenewing(c)) e = v->engine_type;

	/* Nothing to do or all is fine? */
	if (e == EngineID::Invalid() || IsEngineBuildable(e, v->type, _current_company)) return CommandCost();

	/* The engine we need is not available. Report error to user */
	return CommandCost(STR_ERROR_RAIL_VEHICLE_NOT_AVAILABLE + to_underlying(v->type));
}

static CommandCost BuildReplacementVehicleRefitFailure(EngineID e, const Vehicle *old_veh, DoCommandFlags flags)
{
	if (!IsLocalCompany() || !flags.Test(DoCommandFlag::Execute)) return CommandCost();

	VehicleID old_veh_id = (old_veh->type == VehicleType::Train) ? Train::From(old_veh)->Primary()->index : old_veh->index;
	EncodedString headline;

	int order_id = GetIncompatibleRefitOrderIdForAutoreplace(old_veh, e);
	if (order_id != -1) {
		/* Orders contained a refit order that is incompatible with the new vehicle. */
		headline = GetEncodedString(STR_NEWS_VEHICLE_AUTORENEW_FAILED,
			old_veh_id,
			STR_ERROR_AUTOREPLACE_INCOMPATIBLE_REFIT,
			order_id + 1); // 1-based indexing for display
	} else {
		/* Current cargo is incompatible with the new vehicle. */
		headline = GetEncodedString(STR_NEWS_VEHICLE_AUTORENEW_FAILED,
			old_veh_id,
			STR_ERROR_AUTOREPLACE_INCOMPATIBLE_CARGO,
			CargoSpec::Get(old_veh->cargo_type)->name);
	}

	AddVehicleAdviceNewsItem(AdviceType::AutorenewFailed, std::move(headline), old_veh_id);
	return CommandCost();
}

static CommandCost BuildReplacementMultiPartShipSimple(EngineID e, const Vehicle *old_veh, Vehicle **new_vehicle)
{
	/* Build the new vehicle */
	CommandCost cost = Command<Commands::BuildVehicle>::Do({DoCommandFlag::Execute, DoCommandFlag::AutoReplace}, old_veh->tile, e, false, INVALID_CARGO, ClientID::Invalid);
	if (cost.Failed()) return cost;

	auto veh_id = cost.GetResultData<VehicleID>();
	if (!veh_id.has_value()) return cost;

	Vehicle *new_veh = Vehicle::Get(*veh_id);
	*new_vehicle = new_veh;

	Vehicle *v = new_veh;
	const Vehicle *old = old_veh;
	for (; v != nullptr && old != nullptr; v = v->Next(), old = old->Next()) {
		if (old->cargo_type == INVALID_CARGO) continue;

		uint8_t subtype = GetBestFittingSubType(old, v, old->cargo_type);
		CommandCost refit_cost = Command<Commands::RefitVehicle>::Do(DoCommandFlag::Execute, v->index, old->cargo_type, subtype, false, false, 1);
		if (refit_cost.Succeeded()) cost.AddCost(refit_cost.GetCost());
	}

	return cost;
}

/**
 * Builds and refits a replacement multi-part ship
 * @param old_veh A ship that shall be replaced.
 * @param new_vehicle Returns the newly build and refitted ship, if this is nullptr the function operates in dry-run mode
 * @param all_cargoes Mask of all cargoes in old_veh
 * @return cost or error
 */
static CommandCost BuildReplacementMultiPartShip(EngineID e, const Vehicle *old_veh, Vehicle **new_vehicle, CargoTypes all_cargoes, DoCommandFlags flags)
{
	if (old_veh->engine_type == e) {
		/* Easy mode, autoreplacing with same engine */
		if (new_vehicle == nullptr) return CommandCost(); // dry-run: success
		return BuildReplacementMultiPartShipSimple(e, old_veh, new_vehicle);
	}

	std::vector<CargoTypes> refit_mask_list = GetArticulatedRefitMaskVector(e, true);

	std::array<const Vehicle *, NUM_CARGO> old_cargo_vehs = {};
	bool easy_mode = true;
	size_t refit_idx = 0;
	for (const Vehicle *old = old_veh; old != nullptr; old = old->Next(), refit_idx++) {
		if (refit_idx == refit_mask_list.size()) {
			easy_mode = false;
		}
		if (old->cargo_type == INVALID_CARGO) continue;

		old_cargo_vehs[old->cargo_type] = old;

		if (easy_mode && !refit_mask_list[refit_idx].Test(old->cargo_type)) {
			easy_mode = false;
		}
	}
	if (easy_mode) {
		if (new_vehicle == nullptr) return CommandCost(); // dry-run: success

		CommandCost cost = BuildReplacementMultiPartShipSimple(e, old_veh, new_vehicle);
		if (*new_vehicle != nullptr && refit_idx < refit_mask_list.size()) {
			for (Vehicle *v = (*new_vehicle)->Move((int)refit_idx); v != nullptr; v = v->Next(), refit_idx++) {
				if (refit_idx == refit_mask_list.size()) break;

				CargoTypes available = all_cargoes & refit_mask_list[refit_idx];
				if (available.None()) continue;
				CargoType c = available.FindFirstBit();
				assert(old_cargo_vehs[c] != nullptr);

				uint8_t subtype = GetBestFittingSubType(old_cargo_vehs[c], v, c);
				CommandCost refit_cost = Command<Commands::RefitVehicle>::Do(DoCommandFlag::Execute, v->index, c, subtype, false, false, 1);
				if (refit_cost.Succeeded()) cost.AddCost(refit_cost.GetCost());
			}
		}
		return cost;
	}

	if (!VerifyAutoreplaceRefitForOrders(old_veh, e)) {
		if (new_vehicle == nullptr) return CMD_ERROR; // dry-run: failure
		return BuildReplacementVehicleRefitFailure(e, old_veh, flags);
	}

	std::vector <CargoType> output_cargoes;
	CargoTypes remaining = all_cargoes;
	CargoTypes todo = all_cargoes;
	for (size_t i = 0; i < refit_mask_list.size(); i++) {
		CargoTypes available = todo & refit_mask_list[i];
		if (available.None()) available = all_cargoes & refit_mask_list[i];
		if (available.None()) {
			output_cargoes.push_back(INVALID_CARGO);
			continue;
		}

		CargoType c = available.FindFirstBit();
		output_cargoes.push_back(c);
		remaining.Reset(c);
		todo.Reset(c);
		if (todo.None()) todo = all_cargoes;
	}

	if (remaining.Any()) {
		if (new_vehicle == nullptr) return CMD_ERROR; // dry-run: failure
		if (IsLocalCompany()) {
			EncodedString msg = GetEncodedString(STR_NEWS_VEHICLE_AUTORENEW_FAILED, old_veh->index, STR_ERROR_AUTOREPLACE_INCOMPATIBLE_CARGO, CargoSpec::Get(remaining.FindFirstBit())->name);
			AddVehicleAdviceNewsItem(AdviceType::AutorenewFailed, std::move(msg), old_veh->index);
		}
		return CommandCost();
	}

	if (new_vehicle == nullptr) return CommandCost(); // dry-run: success

	/* Build the new vehicle */
	CommandCost cost = Command<Commands::BuildVehicle>::Do({DoCommandFlag::Execute, DoCommandFlag::AutoReplace}, old_veh->tile, e, false, INVALID_CARGO, ClientID::Invalid);
	if (cost.Failed()) return cost;

	auto veh_id = cost.GetResultData<VehicleID>();
	if (!veh_id.has_value()) return cost;

	Vehicle *new_veh = Vehicle::Get(*veh_id);
	*new_vehicle = new_veh;

	size_t i = 0;
	for (Vehicle *v = new_veh; v != nullptr && i < output_cargoes.size(); v = v->Next(), i++) {
		CargoType c = output_cargoes[i];
		if (c == INVALID_CARGO) continue;

		assert(old_cargo_vehs[c] != nullptr);
		uint8_t subtype = GetBestFittingSubType(old_cargo_vehs[c], v, c);
		CommandCost refit_cost = Command<Commands::RefitVehicle>::Do(DoCommandFlag::Execute, v->index, c, subtype, false, false, 1);
		if (refit_cost.Succeeded()) cost.AddCost(refit_cost.GetCost());
	}
	return cost;
}

bool AutoreplaceMultiPartShipWouldSucceed(EngineID e, const Vehicle *old_veh, CargoTypes all_cargoes)
{
	return BuildReplacementMultiPartShip(e, old_veh, nullptr, all_cargoes, {}).Succeeded(); // dry-run mode
}

/**
 * Builds and refits a replacement vehicle
 * Important: The old vehicle is still in the original vehicle chain (used for determining the cargo when the old vehicle did not carry anything, but the new one does)
 * @param old_veh A single (articulated/multiheaded) vehicle that shall be replaced.
 * @param new_vehicle Returns the newly build and refitted vehicle
 * @param part_of_chain The vehicle is part of a train
 * @param flags The calling command flags
 * @param same_type_only Only replace with same engine type.
 * @return cost or error
 */
static CommandCost BuildReplacementVehicle(const Vehicle *old_veh, Vehicle **new_vehicle, bool part_of_chain, DoCommandFlags flags, bool same_type_only)
{
	*new_vehicle = nullptr;

	/* Shall the vehicle be replaced? */
	const Company *c = Company::Get(_current_company);
	EngineID e;
	CommandCost cost = GetNewEngineType(old_veh, c, true, same_type_only, e);
	if (cost.Failed()) return cost;
	if (e == EngineID::Invalid()) return CommandCost(); // neither autoreplace is set, nor autorenew is triggered

	if (old_veh->type == VehicleType::Ship && old_veh->Next() != nullptr) {
		CargoTypes cargoes{};
		for (const Vehicle *u = old_veh; u != nullptr; u = u->Next()) {
			if (u->cargo_type != INVALID_CARGO && u->GetEngine()->CanCarryCargo()) {
				cargoes.Set(u->cargo_type);
			}
		}
		if (!HasAtMostOneBit(cargoes)) {
			/* Old ship has more than one cargo, special handling */
			return BuildReplacementMultiPartShip(e, old_veh, new_vehicle, cargoes, flags);
		}
	}

	/* Does it need to be refitted */
	CargoType refit_cargo = GetNewCargoTypeForReplace(old_veh, e, part_of_chain);
	if (!IsValidCargoType(refit_cargo)) {
		return BuildReplacementVehicleRefitFailure(e, old_veh, flags);
	}

	/* Build the new vehicle */
	cost = Command<Commands::BuildVehicle>::Do({DoCommandFlag::Execute, DoCommandFlag::AutoReplace}, old_veh->tile, e, false, INVALID_CARGO, ClientID::Invalid);
	if (cost.Failed()) return cost;

	auto veh_id = cost.GetResultData<VehicleID>();
	if (!veh_id.has_value()) return cost;

	Vehicle *new_veh = Vehicle::Get(*veh_id);
	*new_vehicle = new_veh;

	/* Refit the vehicle if needed */
	if (refit_cargo != CARGO_NO_REFIT) {
		uint8_t subtype = GetBestFittingSubType(old_veh, new_veh, refit_cargo);

		cost.AddCost(Command<Commands::RefitVehicle>::Do(DoCommandFlag::Execute, new_veh->index, refit_cargo, subtype, false, false, 0));
		assert(cost.Succeeded()); // This should be ensured by GetNewCargoTypeForReplace()
	}

	/* Try to reverse the vehicle, but do not care if it fails as the new type might not be reversible */
	if (new_veh->type == VehicleType::Train && Train::From(old_veh)->flags.Test(VehicleRailFlag::Flipped)) {
		/* Only copy the reverse state if neither old or new vehicle implements reverse-on-build probability callback. */
		if (!TestVehicleBuildProbability(old_veh, BuildProbabilityType::Reversed).has_value() &&
			!TestVehicleBuildProbability(new_veh, BuildProbabilityType::Reversed).has_value()) {
			Command<Commands::ReverseTrainDirection>::Do(DoCommandFlag::Execute, new_veh->index, true);
		}
	}

	return cost;
}

/**
 * Issue a start/stop command
 * @param v a vehicle
 * @param evaluate_callback shall the start/stop callback be evaluated?
 * @return success or error
 */
static inline CommandCost CmdStartStopVehicle(const Vehicle *v, bool evaluate_callback)
{
	return CmdStartStopVehicle({DoCommandFlag::Execute, DoCommandFlag::AutoReplace}, v->index, evaluate_callback);
}

/**
 * Issue a train vehicle move command
 * @param v The vehicle to move
 * @param after The vehicle to insert 'v' after, or nullptr to start new chain
 * @param flags the command flags to use
 * @param whole_chain move all vehicles following 'v' (true), or only 'v' (false)
 * @return success or error
 */
static inline CommandCost CmdMoveVehicle(const Vehicle *v, const Vehicle *after, DoCommandFlags flags, bool whole_chain)
{
	return Command<Commands::MoveRailVehicle>::Do(flags | DoCommandFlag::NoCargoCapacityCheck, v->index, after != nullptr ? after->index : VehicleID::Invalid(), whole_chain ? MoveRailVehicleFlags::MoveChain : MoveRailVehicleFlags::None);
}

/**
 * Copy head specific things to the new vehicle chain after it was successfully constructed
 * @param old_head The old front vehicle (no wagons attached anymore).
 * @param new_head The new head of the completely replaced vehicle chain.
 * @param flags the command flags to use.
 * @param start_stop_check whether to run the start-stop check.
 */
CommandCost CopyHeadSpecificThings(Vehicle *old_head, Vehicle *new_head, DoCommandFlags flags, bool start_stop_check)
{
	CommandCost cost = CommandCost();

	/* Train resources are transferred once, only after validation succeeds.
	 * The provisional dry-run must not join shared orders or consume numbers. */
	const bool train = old_head->type == VehicleType::Train;
	const DoCommandFlags copy_flags = train ? DoCommandFlags{flags}.Reset(DoCommandFlag::Execute) : DoCommandFlags{DoCommandFlag::Execute};
	if (cost.Succeeded() && old_head != new_head) cost.AddCost(Command<Commands::CloneOrder>::Do(copy_flags, CO_SHARE, new_head->index, old_head->index));
	if (cost.Succeeded() && old_head != new_head) cost.AddCost(Command<Commands::AddVehicleToGroup>::Do(copy_flags, old_head->group_id, new_head->index, false));

	/* Perform start/stop check whether the new vehicle suits newgrf restrictions etc. */
	if (start_stop_check && cost.Succeeded()) {
		/* Start the vehicle, might be denied by certain things */
		assert(new_head->vehstatus.Test(VehState::Stopped));
		if (train) {
			/* The read-only command path evaluates callbacks without resetting
			 * unbunching/separation state or any original identity resources. */
			cost.AddCost(CmdStartStopVehicle(DoCommandFlags{DoCommandFlag::AutoReplace}, new_head->index, true));
		} else {
			cost.AddCost(CmdStartStopVehicle(new_head, true));
			/* Stop again even if the GRF refuses an ordinary stop. */
			if (cost.Succeeded()) cost.AddCost(CmdStartStopVehicle(new_head, false));
		}
	}

	/* Last do those things which do never fail (resp. we do not care about), but which are not undo-able */
	if (cost.Succeeded() && old_head != new_head && flags.Test(DoCommandFlag::Execute)) {
		/* Train identity has a single destructive transfer path, including
		 * orders, number, slots, config and lifetime statistics. Never self-copy. */
		if (old_head->type == VehicleType::Train) {
			MoveTrainIdentity(Train::From(old_head), Train::From(new_head));
		} else {
			new_head->CopyVehicleConfigAndStatistics(old_head);
			new_head->unbunch_state = std::move(old_head->unbunch_state);
			GroupStatistics::AddProfitLastYear(new_head);
		}

		/* Switch vehicle windows/news to the new vehicle, so they are not closed/deleted when the old vehicle is sold */
		ChangeVehicleViewports(old_head->index, new_head->index);
		ChangeVehicleViewWindow(old_head->index, new_head->index);
		ChangeVehicleNews(old_head->index, new_head->index);

		if (old_head->type == VehicleType::Train) {
			Train::From(new_head)->speed_restriction = Train::From(old_head)->speed_restriction;
			Train::From(new_head)->flags.Set(VehicleRailFlag::SpeedAdaptationExempt, Train::From(old_head)->flags.Test(VehicleRailFlag::SpeedAdaptationExempt));
		}

		/* Transfer any acquired trace restrict slots to the new vehicle */
		if (!train && old_head->vehicle_flags.Test(VehicleFlag::HaveSlot)) {
			TraceRestrictTransferVehicleOccupantInAllSlots(old_head->index, new_head->index);
			old_head->vehicle_flags.Reset(VehicleFlag::HaveSlot);
			new_head->vehicle_flags.Set(VehicleFlag::HaveSlot);
		}
	}

	return cost;
}

/**
 * Replace a single unit in a free wagon chain
 * @param single_unit vehicle to let autoreplace/renew operator on
 * @param flags command flags
 * @param nothing_to_do is set to 'false' when something was done (only valid when not failed)
 * @param same_type_only Only replace with same engine type.
 * @return cost or error
 */
static CommandCost ReplaceFreeUnit(Vehicle **single_unit, DoCommandFlags flags, bool *nothing_to_do, bool same_type_only)
{
	Train *old_v = Train::From(*single_unit);
	assert(!old_v->IsArticulatedPart() && !old_v->IsRearDualheaded());

	CommandCost cost = CommandCost(ExpensesType::NewVehicles, 0);

	/* Build and refit replacement vehicle */
	Vehicle *new_v = nullptr;
	cost.AddCost(BuildReplacementVehicle(old_v, &new_v, false, flags, same_type_only));

	/* Was a new vehicle constructed? */
	if (cost.Succeeded() && new_v != nullptr) {
		*nothing_to_do = false;

		if (flags.Test(DoCommandFlag::Execute)) {
			/* Move the new vehicle behind the old */
			CmdMoveVehicle(new_v, old_v, DoCommandFlag::Execute, false);

			/* Take over cargo
			 * Note: We do only transfer cargo from the old to the new vehicle.
			 *       I.e. we do not transfer remaining cargo to other vehicles.
			 *       Else you would also need to consider moving cargo to other free chains,
			 *       or doing the same in ReplaceChain(), which would be quite troublesome.
			 */
			TransferCargo(old_v, new_v, false);

			*single_unit = new_v;

			AI::NewEvent(old_v->owner, new ScriptEventVehicleAutoReplaced(old_v->index, new_v->index));
		}

		/* Sell the old vehicle */
		cost.AddCost(Command<Commands::SellVehicle>::Do(flags, old_v->tile, old_v->index, SellVehicleFlags::None, ClientID::Invalid));

		/* If we are not in DoCommandFlag::Execute undo everything */
		if (!flags.Test(DoCommandFlag::Execute)) {
			Command<Commands::SellVehicle>::Do(DoCommandFlag::Execute, new_v->tile, new_v->index, SellVehicleFlags::None, ClientID::Invalid);
		}
	}

	return cost;
}

/** Struct for recording vehicle chain replacement information. */
struct ReplaceChainItem {
	Vehicle *old_veh; ///< Old vehicle to replace.
	Vehicle *new_veh; ///< Replacement vehicle, or nullptr if no replacement.
	Money cost; ///< Cost of buying and refitting replacement.

	/**
	 * Create a new item.
	 * @param old_veh The vehicle to replace.
	 * @param new_veh The replacement vehicle, maybe be \c nullptr if there is no replacement.
	 * @param cost The cost for buying/refitting.
	 */
	ReplaceChainItem(Vehicle *old_veh, Vehicle *new_veh, Money cost) : old_veh(old_veh), new_veh(new_veh), cost(cost) { }

	/**
	 * Get vehicle to use for this position.
	 * @return Either the new vehicle, or the old vehicle if there is no replacement.
	 */
	Vehicle *GetVehicle() const { return new_veh == nullptr ? old_veh : new_veh; }
};

/** Replace trains in physical order, keeping the logical identity separate. */
static CommandCost ReplaceTrainChain(Vehicle **chain, DoCommandFlags flags, bool wagon_removal, bool *nothing_to_do, bool same_type_only)
{
	Train *old_primary = Train::From(*chain);
	Train *old_root = old_primary->First();
	const std::vector<uint32_t> original_early_route = old_root->early_route;
	const uint old_total_length = CeilDiv(old_root->gcache.cached_total_length, TILE_SIZE) * TILE_SIZE;
	const bool execute = flags.Test(DoCommandFlag::Execute);
	const bool old_driving_backwards = old_primary->vehicle_flags.Test(VehicleFlag::DrivingBackwards);
	CommandCost cost(ExpensesType::NewVehicles, 0);

	/* No command may migrate the original identity during the construction
	 * trial. Back up physical links and every marker/cache changed by arrange. */
	struct Snapshot {
		Train *vehicle;
		Train *next;
		Train *previous;
		Train *first;
		Train *last;
		Train *primary;
		Vehicle *unit_main;
		uint8_t subtype;
		VehicleRailFlags flags;
		VehicleFlags vehicle_flags;
		NewGRFCache grf_cache;
		TrainCache tcache;
		decltype(old_root->gcache) gcache;
		decltype(old_root->vcache) vcache;
		decltype(old_root->vehstatus) vehstatus;
		CargoType cargo_type;
		uint8_t cargo_subtype;
		uint16_t cargo_cap;
		uint16_t refit_cap;
		GroupID group;
		Direction direction;
		RailTypes railtypes;
		RailTypes compatible_railtypes;
		decltype(old_root->acceleration) acceleration;
		decltype(old_root->gv_flags) gv_flags;
		decltype(old_root->colourmap) colourmap;
	};
	std::vector<Snapshot> snapshots;
	std::vector<Train *> counted_primaries;
	auto save_chain = [&snapshots, &counted_primaries](Train *root) {
		Train *primary = root->Primary();
		if (primary->IsPrimaryVehicle() && std::ranges::find(counted_primaries, primary) == counted_primaries.end()) counted_primaries.push_back(primary);
		for (Train *u = root->First(); u != nullptr; u = u->Next()) {
			snapshots.push_back({u, u->Next(), Train::From(u->Previous()), u->First(), u->Last(), u->Primary(),
				u->HasUnitMain() ? u->GetUnitMain() : nullptr, u->subtype, u->flags, u->vehicle_flags,
				u->grf_cache, u->tcache, u->gcache, u->vcache, u->vehstatus, u->cargo_type, u->cargo_subtype,
				u->cargo_cap, u->refit_cap, u->group_id, u->direction, u->railtypes,
				u->compatible_railtypes, u->acceleration, u->gv_flags, u->colourmap});
		}
	};
	save_chain(old_root);
	std::vector<Train *> old_members;
	for (const Snapshot &s : snapshots) old_members.push_back(s.vehicle);
	for (Train *u : old_members) {
		CommandCost owner = CheckOwnership(u->owner);
		if (owner.Failed()) return owner;
	}
	std::vector<ReplaceChainItem> replacements;
	for (Train *unit : CollectTrainPurchasedUnits(old_root)) replacements.emplace_back(unit, nullptr, 0);
	if (replacements.empty()) return CMD_ERROR;

	/* Build while all original cargo/orders still belong to the old consist. */
	for (ReplaceChainItem &item : replacements) {
		CommandCost build = BuildReplacementVehicle(item.old_veh, &item.new_veh, true, flags, same_type_only);
		item.cost = build.GetCost();
		if (item.new_veh != nullptr) {
			item.new_veh = GetAutoreplaceUnitStart(item.new_veh);
			save_chain(Train::From(item.new_veh));
			*nothing_to_do = false;
		}
		cost.AddCost(std::move(build));
		if (cost.Failed()) break;
	}

	auto restore = [&snapshots, old_root, &original_early_route]() {
		/* Restore all original and provisional chains together; otherwise a
		 * provisional half can retain a link into the restored original train. */
		for (const Snapshot &s : snapshots) {
			Train *u = s.vehicle;
			u->SetNextRaw(s.next);
			u->SetPreviousRaw(s.previous);
			u->SetFirst(s.first);
			u->SetLast(s.last);
			u->SetPrimary(s.primary);
			u->SetUnitMain(s.unit_main);
			u->subtype = s.subtype;
			u->flags = s.flags;
			u->vehicle_flags = s.vehicle_flags;
			u->grf_cache = s.grf_cache;
			u->tcache = s.tcache;
			u->gcache = s.gcache;
			u->vcache = s.vcache;
			u->vehstatus = s.vehstatus;
			u->cargo_type = s.cargo_type;
			u->cargo_subtype = s.cargo_subtype;
			u->cargo_cap = s.cargo_cap;
			u->refit_cap = s.refit_cap;
			u->group_id = s.group;
			u->direction = s.direction;
			u->railtypes = s.railtypes;
			u->compatible_railtypes = s.compatible_railtypes;
			u->acceleration = s.acceleration;
			u->gv_flags = s.gv_flags;
			u->colourmap = s.colourmap;
		}
		old_root->early_route = original_early_route;
		/* Pool front markers are separate from subtype masks. Raw relinking
		 * restores neither, so refresh each restored chain after all links exist. */
		std::vector<Train *> restored_heads;
		for (const Snapshot &s : snapshots) {
			if (std::ranges::find(restored_heads, s.first) != restored_heads.end()) continue;
			restored_heads.push_back(s.first);
			ResetChainNonFrontMarkers(s.first);
		}
		InvalidateVehicleTickCaches();
	};
	auto discard_new = [&replacements]() {
		for (ReplaceChainItem &item : replacements) {
			Train *unit = item.new_veh == nullptr ? nullptr : Train::From(item.new_veh);
			if (unit == nullptr) continue;
			item.new_veh = nullptr;
			if (unit->IsMultiheaded() && unit->First() != unit) {
				std::vector<Train *> members;
				for (Train *u = unit->First(); u != nullptr; u = u->Next()) members.push_back(u);
				auto front = std::ranges::find(members, unit);
				std::rotate(members.begin(), front, members.end());
				for (size_t i = 0; i < members.size(); ++i) {
					members[i]->SetNextRaw(i + 1 < members.size() ? members[i + 1] : nullptr);
					members[i]->SetPreviousRaw(i > 0 ? members[i - 1] : nullptr);
					members[i]->SetFirst(members.front());
					members[i]->SetLast(members.back());
				}
				BindTrainPrimary(members.front(), unit);
			}
			[[maybe_unused]] CommandCost sold = Command<Commands::SellVehicle>::Do({DoCommandFlag::Execute, DoCommandFlag::AutoReplace}, unit->tile, unit->index, SellVehicleFlags::SellChain, ClientID::Invalid);
			assert(sold.Succeeded());
		}
	};
	if (cost.Failed()) {
		restore();
		discard_new();
		return cost;
	}

	auto purchased_main = [](Train *u) {
		if (u->IsRearDualheaded()) return u->other_multiheaded_part;
		return Train::From(u->GetUnitMain());
	};
	auto find_item = [&replacements](Train *main) -> ReplaceChainItem * {
		for (ReplaceChainItem &item : replacements) if (item.old_veh == main) return &item;
		return nullptr;
	};
	ReplaceChainItem *primary_item = find_item(purchased_main(old_primary));
	if (primary_item == nullptr) {
		restore();
		discard_new();
		return CMD_ERROR;
	}
	Train *new_primary = Train::From(primary_item->GetVehicle());
	std::vector<Train *> final_members;
	std::vector<Train *> final_units;
	/* One entry per physical half/range. Replacing both dual halves together
	 * must not swallow independent wagons between them. */
	for (size_t i = 0; i < old_members.size();) {
		Train *front = old_members[i];
		Train *last = front->GetUnitChainLast();
		Train *main = purchased_main(front);
		ReplaceChainItem *item = find_item(main);
		if (item == nullptr) { cost = CMD_ERROR; break; }
		size_t end = i;
		while (end < old_members.size() && old_members[end] != last) ++end;
		if (end == old_members.size()) { cost = CMD_ERROR; break; }
		std::vector<Train *> range;
		if (item->new_veh == nullptr) {
			range.assign(old_members.begin() + i, old_members.begin() + end + 1);
		} else {
			Train *replacement = Train::From(item->new_veh);
			const bool both_dual = main->IsMultiheaded() && replacement->IsMultiheaded();
			if (both_dual) {
				Train *half = main == last || front == main ? replacement : replacement->other_multiheaded_part;
				if (half == nullptr) { cost = CMD_ERROR; break; }
				for (Train *u = half->GetUnitChainFront(); u != nullptr; u = u->Next()) {
					range.push_back(u);
					if (u == half->GetUnitChainLast()) break;
				}
			} else if (std::ranges::find(final_units, replacement) == final_units.end()) {
				/* A new dual head replacing a one-range unit includes both halves;
				 * a one-range replacement of a dual head is emitted just once. */
				for (Train *u = replacement->First(); u != nullptr; u = u->Next()) range.push_back(u);
				if (!main->IsMultiheaded() && !replacement->IsMultiheaded() &&
						(main->GetUnitChainFront() == main) != (replacement->GetUnitChainFront() == replacement)) {
					std::ranges::reverse(range);
				}
			}
		}
		Train *unit = Train::From(item->GetVehicle());
		if (std::ranges::find(final_units, unit) == final_units.end()) final_units.push_back(unit);
		for (Train *u : range) {
			if (std::ranges::find(final_members, u) != final_members.end()) { cost = CMD_ERROR; break; }
			final_members.push_back(u);
		}
		if (cost.Failed()) break;
		i = end + 1;
	}

	/* Let native attachment/capacity/length checks validate the actual physical
	 * order. Keep-length pruning always removes the rear-most complete wagon,
	 * never an engine or just one half of a purchased unit. */
	if (cost.Succeeded()) {
		for (;;) {
			CommandCost arranged = RebuildTrainDepotLayout(final_members, new_primary, CCF_ARRANGE | ConsistChangeFlag::CheckOnly);
			bool too_long = arranged.Failed() ? arranged.GetErrorMessage() == STR_ERROR_TRAIN_TOO_LONG :
				final_members.front()->gcache.cached_total_length > old_total_length;
			if (!wagon_removal || !too_long) { cost.AddCost(std::move(arranged)); break; }
			Train *remove = nullptr;
			for (auto it = final_members.rbegin(); it != final_members.rend(); ++it) {
				Train *unit = purchased_main(*it);
				if (unit != new_primary && RailVehInfo(unit->engine_type)->railveh_type == RailVehicleType::Wagon) { remove = unit; break; }
			}
			if (remove == nullptr) { cost = CommandCost(STR_ERROR_TRAIN_TOO_LONG_AFTER_REPLACEMENT); break; }
			std::erase_if(final_members, [remove, &purchased_main](Train *u) { return purchased_main(u) == remove; });
			std::erase(final_units, remove);
		}
	}
	/* Detach retiring purchased units from one another before sale. Native
	 * single-dual sales may otherwise widen across intervening wagons. Every
	 * plan owns exactly its bound articulated members and both dual halves. */
	struct Retirement {
		Train *unit;
		std::vector<Train *> members;
	};
	std::vector<Retirement> retiring;
	if (cost.Succeeded()) {
		for (ReplaceChainItem &item : replacements) {
			for (Vehicle *v : {item.old_veh, item.new_veh}) {
				if (v == nullptr || std::ranges::find(final_members, v) != final_members.end()) continue;
				Train *unit = Train::From(v);
				Retirement plan{unit, {}};
				for (const Snapshot &s : snapshots) {
					if (purchased_main(s.vehicle) == unit) plan.members.push_back(s.vehicle);
				}
				if (plan.members.empty()) { cost = CMD_ERROR; break; }
				if (unit->IsMultiheaded()) {
					/* Native SellChain rejects a rear-half ID and starts at the
					 * selected front half. Put that half first in an isolated
					 * retirement, even if its physical rear originally preceded it. */
					auto front = std::ranges::find(plan.members, unit);
					if (front == plan.members.end()) { cost = CMD_ERROR; break; }
					std::rotate(plan.members.begin(), front, plan.members.end());
				}
				retiring.push_back(std::move(plan));
			}
			if (cost.Failed()) break;
		}
		for (Retirement &plan : retiring) {
			for (size_t i = 0; i < plan.members.size(); ++i) {
				Train *u = plan.members[i];
				u->SetNextRaw(i + 1 < plan.members.size() ? plan.members[i + 1] : nullptr);
				u->SetPreviousRaw(i > 0 ? plan.members[i - 1] : nullptr);
				u->SetFirst(plan.members.front());
				u->SetLast(plan.members.back());
			}
			BindTrainPrimary(plan.members.front(), plan.unit);
			plan.unit->vehstatus.Set(VehState::Stopped);
		}
	}
	/* Validate every old sale and provisional discard before cargo, identity,
	 * group counts, numbers or deletions become irreversible. */
	if (cost.Succeeded()) {
		for (const Retirement &plan : retiring) {
			CommandCost sale = Command<Commands::SellVehicle>::Do(DoCommandFlags{flags}.Reset(DoCommandFlag::Execute) | DoCommandFlag::AutoReplace,
				plan.unit->tile, plan.unit->index, SellVehicleFlags::SellChain, ClientID::Invalid);
			if (sale.Failed()) { cost.AddCost(std::move(sale)); break; }
			for (const ReplaceChainItem &item : replacements) {
				if (item.old_veh == plan.unit) cost.AddCost(sale.GetCost());
				if (item.new_veh == plan.unit) cost.AddCost(-item.cost);
			}
		}
	}
	if (cost.Succeeded()) cost.AddCost(CopyHeadSpecificThings(old_primary, new_primary, DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), true));
	if (!execute || cost.Failed()) {
		restore();
		discard_new();
		return cost;
	}

	/* Commit starts here. A snapshot must never be restored after this point:
	 * native sales delete entire purchased units and invalidate their pointers. */
	for (Train *primary : counted_primaries) GroupStatistics::CountVehicle(primary, -1);
	CommandCost copied = CopyHeadSpecificThings(old_primary, new_primary, flags, false);
	assert(copied.Succeeded()); // The same commands were validated above.
	if (copied.Failed()) return copied;
	GroupStatistics::CountVehicle(new_primary, 1);
	for (Retirement &plan : retiring) {
		for (Train *u : plan.members) {
			u->ClearFrontEngine();
			u->ClearFrontWagon();
		}
	}
	Train *final_root = new_primary->First();
	/* Evaluate final capacities without truncation, then spread cargo before
	 * actual ConsistChanged can discard excess. This is execution-only. */
	final_root->InvalidateNewGRFCacheOfChain();
	for (Train *u = final_root; u != nullptr; u = u->Next()) {
		u->cargo_cap = u->GetEngine()->DetermineCapacity(u);
		u->refit_cap = std::min(u->refit_cap, u->cargo_cap);
	}
	CheckCargoCapacity(final_root);
	*chain = new_primary;
	if (old_primary != new_primary) AI::NewEvent(old_primary->owner, new ScriptEventVehicleAutoReplaced(old_primary->index, new_primary->index));
	/* The rollback journal is dead after commit. Drop its member pointers
	 * before any retirement so even later cleanup cannot accidentally use it. */
	snapshots.clear();
	old_members.clear();
	counted_primaries.clear();

	for (Retirement &plan : retiring) {
		for (ReplaceChainItem &item : replacements) {
			if (item.old_veh == plan.unit) {
				TransferCargo(plan.unit, new_primary, true);
				item.old_veh = nullptr;
			}
			if (item.new_veh == plan.unit) item.new_veh = nullptr;
		}
		Train *unit = plan.unit;
		plan.unit = nullptr;
		plan.members.clear(); // Do not retain any member pointer across deletion.
		CommandCost sold = Command<Commands::SellVehicle>::Do(flags | DoCommandFlag::AutoReplace,
			unit->tile, unit->index, SellVehicleFlags::SellChain, ClientID::Invalid);
		assert(sold.Succeeded());
		if (sold.Failed()) return sold; // Never restore snapshots of deleted units.
	}
	final_root->ConsistChanged(CCF_ARRANGE);
	CheckCargoCapacity(final_root);
	if (old_driving_backwards && !new_primary->vehicle_flags.Test(VehicleFlag::DrivingBackwards) &&
			(_settings_game.difficulty.train_flip_reverse_allowed == TrainFlipReversingAllowed::None || final_root->Last()->CanLeadTrain())) {
		new_primary->vehicle_flags.Set(VehicleFlag::DrivingBackwards);
		final_root->ConsistChanged(CCF_ARRANGE);
	}

	return cost;
}

/**
 * Replace a whole vehicle chain
 * @param chain vehicle chain to let autoreplace/renew operator on
 * @param flags command flags
 * @param wagon_removal remove wagons when the resulting chain occupies more tiles than the old did
 * @param nothing_to_do is set to 'false' when something was done (only valid when not failed)
 * @param same_type_only Only replace with same engine type.
 * @return cost or error
 */
static CommandCost ReplaceChain(Vehicle **chain, DoCommandFlags flags, bool wagon_removal, bool *nothing_to_do, bool same_type_only)
{
	Vehicle *old_head = *chain;
	assert(old_head->IsPrimaryVehicle());

	CommandCost cost = CommandCost(ExpensesType::NewVehicles, 0);

	if (old_head->type == VehicleType::Train) {
		return ReplaceTrainChain(chain, flags, wagon_removal, nothing_to_do, same_type_only);
	} else {
		/* Build and refit replacement vehicle */
		Vehicle *new_head = nullptr;
		cost.AddCost(BuildReplacementVehicle(old_head, &new_head, true, flags, same_type_only));

		/* Was a new vehicle constructed? */
		if (cost.Succeeded() && new_head != nullptr) {
			*nothing_to_do = false;

			/* The new vehicle is constructed, now take over orders and everything... */
			cost.AddCost(CopyHeadSpecificThings(old_head, new_head, flags, true));

			if (cost.Succeeded()) {
				/* The new vehicle is constructed, now take over cargo */
				if (flags.Test(DoCommandFlag::Execute)) {
					TransferCargo(old_head, new_head, true);
					*chain = new_head;

					AI::NewEvent(old_head->owner, new ScriptEventVehicleAutoReplaced(old_head->index, new_head->index));
				}

				/* Sell the old vehicle */
				cost.AddCost(Command<Commands::SellVehicle>::Do(flags, old_head->tile, old_head->index, SellVehicleFlags::None, ClientID::Invalid));
			}

			/* If we are not in DoCommandFlag::Execute undo everything */
			if (!flags.Test(DoCommandFlag::Execute)) {
				Command<Commands::SellVehicle>::Do(DoCommandFlag::Execute, new_head->tile, new_head->index, SellVehicleFlags::None, ClientID::Invalid);
			}
		}
	}

	return cost;
}

/**
 * Autoreplaces a vehicle
 * Trains are replaced as a whole chain, free wagons in depot are replaced on their own
 * @param flags type of operation
 * @param veh_id Index of vehicle
 * @param same_type_only Autoreplace with same type only
 * @return the cost of this operation or an error
 */
CommandCost CmdAutoreplaceVehicle(DoCommandFlags flags, VehicleID veh_id, bool same_type_only)
{
	Vehicle *v = Vehicle::GetIfValid(veh_id);
	if (v == nullptr || !IsCompanyBuildableVehicleType(v)) return CMD_ERROR;

	CommandCost ret = CheckOwnership(v->owner);
	if (ret.Failed()) return ret;

	if (v->vehstatus.Test(VehState::Crashed)) return CMD_ERROR;

	bool free_wagon = false;
	if (v->type == VehicleType::Train) {
		Train *t = Train::From(v);
		Train *primary = t->Primary();
		if (primary == nullptr || primary->First() != t->First()) return CMD_ERROR;
		if (primary->IsArticulatedPart() || primary->IsRearDualheaded()) {
			primary = GetAutoreplaceUnitStart(primary);
			if (primary != nullptr && primary->IsRearDualheaded()) primary = primary->other_multiheaded_part;
			if (primary == nullptr || primary->First() != t->First() || primary->IsArticulatedPart() || primary->IsRearDualheaded()) return CMD_ERROR;
		}
		if (primary->IsFrontEngine() || primary->IsFrontWagon()) {
			/* A controlling engine may be behind a parts-first unit or prefix
			 * wagons. Resolve all valid member IDs to that identity owner. */
			v = primary;
		} else {
			/* Keep the free-wagon policy: renew only the requested purchased unit. */
			t = GetAutoreplaceUnitStart(t);
			if (t == nullptr || t->IsArticulatedPart() || t->IsRearDualheaded() || !t->IsWagon()) return CMD_ERROR;
			v = t;
			free_wagon = true;
		}
	} else {
		if (!v->IsPrimaryVehicle()) return CMD_ERROR;
	}
	if (!v->IsChainInDepot()) return CMD_ERROR;

	const Company *c = Company::Get(_current_company);
	bool wagon_removal = c->settings.renew_keep_length;

	const Group *g = Group::GetIfValid(v->group_id);
	if (g != nullptr) wagon_removal = g->flags.Test(GroupFlag::ReplaceWagonRemoval);

	/* Test whether any replacement is set, before issuing a whole lot of commands that would end in nothing changed */
	std::vector<Vehicle *> candidates;
	if (v->type == VehicleType::Train && !free_wagon) {
		for (Train *unit : CollectTrainPurchasedUnits(Train::From(v)->First())) candidates.push_back(unit);
	} else {
		candidates.push_back(v);
	}
	bool any_replacements = false;
	for (Vehicle *candidate : candidates) {
		EngineID e;
		CommandCost check = GetNewEngineType(candidate, c, false, same_type_only, e);
		if (check.Failed()) return check;
		any_replacements |= (e != EngineID::Invalid());
	}

	CommandCost cost = CommandCost(ExpensesType::NewVehicles, 0);
	bool nothing_to_do = true;

	if (any_replacements) {
		Vehicle *original_vehicle = v;
		const auto original_status = v->vehstatus;
		bool was_stopped = free_wagon || v->vehstatus.Test(VehState::Stopped);
		bool temporary_train_stop = !was_stopped && v->type == VehicleType::Train && !flags.Test(DoCommandFlag::Execute);

		/* A train query needs a stopped construction trial, not a real stop
		 * command that destroys unbunching/separation resources. */
		if (temporary_train_stop) v->vehstatus.Set(VehState::Stopped);
		else if (!was_stopped) cost.AddCost(CmdStartStopVehicle(v, true));
		if (cost.Failed()) return cost;

		assert(free_wagon || v->IsStoppedInDepot());

		/* We have to construct the new vehicle chain to test whether it is valid.
		 * Vehicle construction needs random bits, so we have to save the random seeds
		 * to prevent desyncs and to replay newgrf callbacks during DoCommandFlag::Execute */
		SavedRandomSeeds saved_seeds;
		SaveRandomSeeds(&saved_seeds);
		if (free_wagon) {
			cost.AddCost(ReplaceFreeUnit(&v, DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), &nothing_to_do, same_type_only));
		} else {
			cost.AddCost(ReplaceChain(&v, DoCommandFlags{flags}.Reset(DoCommandFlag::Execute), wagon_removal, &nothing_to_do, same_type_only));
		}
		RestoreRandomSeeds(saved_seeds);

		if (cost.Succeeded() && flags.Test(DoCommandFlag::Execute)) {
			CommandCost ret;
			if (free_wagon) {
				ret = ReplaceFreeUnit(&v, flags, &nothing_to_do, same_type_only);
			} else {
				ret = ReplaceChain(&v, flags, wagon_removal, &nothing_to_do, same_type_only);
			}
			assert(ret.Succeeded() && ret.GetCost() == cost.GetCost());
		}

		/* Restore the exact query status, or restart after execution. */
		if (temporary_train_stop) original_vehicle->vehstatus = original_status;
		else if (!was_stopped) cost.AddCost(CmdStartStopVehicle(v, false));
	}

	if (cost.Succeeded() && nothing_to_do) cost = CommandCost(STR_ERROR_AUTOREPLACE_NOTHING_TO_DO);
	cost.SetResultData(v->index);
	return cost;
}

/**
 * Change engine renewal parameters
 * @param flags operation to perform
 * @param id_g engine group
 * @param old_engine_type old engine type
 * @param new_engine_type new engine type
 * @param when_old replace when engine gets old?
 * @return the cost of this operation or an error
 */
CommandCost CmdSetAutoReplace(DoCommandFlags flags, GroupID id_g, EngineID old_engine_type, EngineID new_engine_type, bool when_old)
{
	Company *c = Company::GetIfValid(_current_company);
	if (c == nullptr) return CMD_ERROR;

	CommandCost cost;

	if (Group::IsValidID(id_g) ? Group::Get(id_g)->owner != _current_company : !IsAllGroupID(id_g) && !IsDefaultGroupID(id_g)) return CMD_ERROR;
	if (!Engine::IsValidID(old_engine_type)) return CMD_ERROR;
	if (Group::IsValidID(id_g) && Group::Get(id_g)->vehicle_type != Engine::Get(old_engine_type)->type) return CMD_ERROR;

	if (new_engine_type != EngineID::Invalid()) {
		if (!Engine::IsValidID(new_engine_type)) return CMD_ERROR;
		if (!CheckAutoreplaceValidity(old_engine_type, new_engine_type, _current_company)) return CMD_ERROR;

		cost = AddEngineReplacementForCompany(c, old_engine_type, new_engine_type, id_g, when_old, flags);
	} else {
		cost = RemoveEngineReplacementForCompany(c, old_engine_type, id_g, flags);
	}

	if (flags.Test(DoCommandFlag::Execute)) {
		GroupStatistics::UpdateAutoreplace(_current_company);
		if (IsLocalCompany()) SetWindowDirty(WindowClass::ReplaceVehicle, Engine::Get(old_engine_type)->type);

		const VehicleType vt = Engine::Get(old_engine_type)->type;
		SetWindowDirty(GetWindowClassForVehicleType(vt), VehicleListIdentifier(VehicleListType::Group, vt, _current_company).ToWindowNumber());
	}
	if (flags.Test(DoCommandFlag::Execute) && IsLocalCompany()) InvalidateAutoreplaceWindow(old_engine_type, id_g);

	return cost;
}

