/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file tbtr_template_vehicle_func.cpp Template-based train replacement: template vehicle functions. */

#include "stdafx.h"
#include "articulated_vehicles.h"
#include "autoreplace_func.h"
#include "cargoaction.h"
#include "command_func.h"
#include "company_func.h"
#include "core/backup_type.hpp"
#include "core/geometry_type.hpp"
#include "core/random_func.hpp"
#include "debug.h"
#include "depot_base.h"
#include "gfx_func.h"
#include "group_cmd.h"
#include "order_cmd.h"
#include "newgrf.h"
#include "spritecache.h"
#include "strings_func.h"
#include "table/sprites.h"
#include "table/strings.h"
#include "tbtr_template_vehicle_cmd.h"
#include "tbtr_template_vehicle_func.h"
#include "tbtr_template_vehicle.h"
#include "train.h"
#include "train_cmd.h"
#include "vehicle_func.h"
#include "vehicle_gui.h"
#include "window_func.h"
#include "window_gui.h"
#include "zoom_func.h"

#include "safeguards.h"

bool _template_vehicle_images_valid = false;

void BuildTemplateGuiList(GUITemplateList *list, Scrollbar *vscroll, Owner oid, RailType railtype)
{
	list->clear();
	for (const TemplateVehicle *tv : TemplateVehicle::Iterate()) {
		if (tv->owner == oid && tv->Prev() == nullptr && TemplateVehicleContainsEngineOfRailtype(tv, railtype)) {
			list->push_back(tv);
		}
	}

	list->RebuildDone();
	if (vscroll) vscroll->SetCount((uint)list->size());
}

Money CalculateOverallTemplateCost(const TemplateVehicle *tv)
{
	Money val = 0;

	for (tv = tv != nullptr ? tv->First()->GetUnitMain() : nullptr; tv != nullptr; tv = tv->GetNextUnit()) {
		val += (Engine::Get(tv->engine_type))->GetCost();
	}
	return val;
}

Money CalculateOverallTemplateDisplayRunningCost(const TemplateVehicle *tv)
{
	Money val = 0;

	for (tv = tv != nullptr ? tv->First()->GetUnitMain() : nullptr; tv != nullptr; tv = tv->GetNextUnit()) {
		val += (Engine::Get(tv->engine_type))->GetDisplayRunningCost();
	}
	return val;
}

void DrawTemplate(const TemplateVehicle *tv, int left, int right, int y, int height)
{
	if (tv == nullptr) return;

	bool rtl = _current_text_dir == TD_RTL;

	DrawPixelInfo tmp_dpi;
	int max_width = right - left + 1;
	int veh_height = ScaleSpriteTrad(14);
	int padding = height - veh_height;
	if (!FillDrawPixelInfo(&tmp_dpi, left, y + (padding / 2), max_width, height)) return;

	AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);

	const TemplateVehicle *t = tv;
	int offset = rtl ? max_width : 0;

	while (t != nullptr) {
		t->sprite_seq.Draw(offset + ((rtl ? -1 : 1) * t->image_dimensions.GetOffsetX()), t->image_dimensions.GetOffsetY() + ScaleSpriteTrad(10), t->colourmap, false);

		offset += (rtl ? -1 : 1) * t->image_dimensions.GetDisplayImageWidth();
		t = t->Next();
	}
}

/* Copy important stuff from the virtual vehicle to the template */
void SetupTemplateVehicleFromVirtual(TemplateVehicle *tmp, TemplateVehicle *prev, Train *virt)
{
	if (prev != nullptr) {
		prev->SetNext(tmp);
		tmp->SetPrev(prev);
		tmp->SetFirst(prev->First());
	}
	tmp->railtypes = virt->railtypes;
	tmp->owner = virt->owner;

	/* Set the subtype but also clear the virtual flag while doing it */
	tmp->subtype = virt->subtype & ~(1 << GVSF_VIRTUAL);
	/* Set the cargo type and capacity */
	tmp->cargo_type = virt->cargo_type;
	tmp->cargo_subtype = virt->cargo_subtype;
	tmp->cargo_cap = virt->cargo_cap;

	AssignBit(tmp->ctrl_flags, TVCF_REVERSED, virt->flags.Test(VehicleRailFlag::Flipped));

	if (virt->Previous() == nullptr) {
		uint cargo_weight = 0;
		uint full_cargo_weight = 0;
		for (const Train *u = virt; u != nullptr; u = u->Next()) {
			cargo_weight += u->GetCargoWeight(u->cargo.StoredCount());
			full_cargo_weight += u->GetCargoWeight(u->cargo_cap);
		}
		const GroundVehicleCache *gcache = virt->GetGroundVehicleCache();
		tmp->max_speed = virt->GetDisplayMaxSpeed();
		tmp->power = gcache->cached_power;
		tmp->empty_weight = std::max<uint32_t>(gcache->cached_weight - cargo_weight, 1);
		tmp->full_weight = std::max<uint32_t>(gcache->cached_weight + full_cargo_weight - cargo_weight, 1);
		tmp->max_te = gcache->cached_max_te;
		tmp->air_drag = gcache->cached_air_drag;
	}

	virt->GetImage(_current_text_dir == TD_RTL ? Direction::E : Direction::W, EngineImageType::InDepot, &tmp->sprite_seq);
	tmp->image_dimensions.SetFromTrain(virt);
	tmp->colourmap = GetUncachedTrainPaletteIgnoringGroup(virt);
}

/* Create a full TemplateVehicle based train according to a virtual train */
TemplateVehicle *TemplateVehicleFromVirtualTrain(Train *virt)
{
	assert(virt != nullptr);

	virt = virt->First();
	Train *init_virt = virt;
	const Train *primary = virt->Primary()->IsPrimaryVehicle() ? virt->Primary() : nullptr;
	std::vector<std::pair<const Train *, TemplateVehicle *>> mapping;

	TemplateVehicle *tmp = nullptr;
	TemplateVehicle *prev = nullptr;
	for (; virt != nullptr; virt = virt->Next()) {
		tmp = TemplateVehicle::Create(virt->engine_type);
		SetupTemplateVehicleFromVirtual(tmp, prev, virt);
		mapping.emplace_back(virt, tmp);
		prev = tmp;
	}
	TemplateVehicle *head = tmp->First();
	auto mapped = [&mapping](const Train *v) -> TemplateVehicle * {
		for (const auto &[source, target] : mapping) if (source == v) return target;
		return nullptr;
	};
	TemplateVehicle *template_primary = mapped(primary);
	for (const auto &[source, target] : mapping) {
		const Train *main = source->IsRearDualheaded() && source->other_multiheaded_part != nullptr ? source->other_multiheaded_part : Train::From(source->GetUnitMain());
		target->SetUnitMain(mapped(main));
		target->SetPrimary(template_primary);
		ClrBit(target->subtype, GVSF_FRONT);
		ClrBit(target->subtype, GVSF_FRONT_WAGON);
		ClrBit(target->subtype, GVSF_FREE_WAGON);
	}
	if (template_primary != nullptr) {
		if (template_primary->IsEngine()) template_primary->SetFrontEngine();
		else SetBit(template_primary->subtype, GVSF_FRONT_WAGON);
	} else {
		head->SetFreeWagon();
	}
	head->SetRealLength(CeilDiv(init_virt->gcache.cached_total_length * 10, TILE_SIZE));
	return head;
}

CommandCost CmdSellRailWagon(DoCommandFlags flags, Vehicle *t, bool sell_chain, bool backup_order, ClientID user);

static const Train *TemplateTrainUnitMain(const Train *part);

Train *DeleteVirtualTrain(Train *chain, Train *to_del)
{
	Train *head = chain->First();
	VehicleID survivor = VehicleID::Invalid();
	for (Train *part = head; part != nullptr; part = part->Next()) {
		if (TemplateTrainUnitMain(part) != TemplateTrainUnitMain(to_del)) { survivor = part->index; break; }
	}
	CommandCost result = CmdSellRailWagon(DoCommandFlag::Execute, to_del, false, false, ClientID::Invalid);
	if (result.Failed()) return head;
	Train *remaining = Train::GetIfValid(survivor);
	return remaining != nullptr ? remaining->First() : nullptr;
}

/* Retrieve template vehicle from template replacement that belongs to the given group */
TemplateVehicle *GetTemplateVehicleByGroupID(GroupID gid)
{
	if (gid >= NEW_GROUP) return nullptr;
	const TemplateID tid = GetTemplateIDByGroupID(gid);
	return tid != INVALID_TEMPLATE ? TemplateVehicle::GetIfValid(tid) : nullptr;
}

TemplateVehicle *GetTemplateVehicleByGroupIDRecursive(GroupID gid)
{
	if (gid >= NEW_GROUP) return nullptr;
	const TemplateID tid = GetTemplateIDByGroupIDRecursive(gid);
	return tid != INVALID_TEMPLATE ? TemplateVehicle::GetIfValid(tid) : nullptr;
}

/**
 * Check a template consist whether it contains any engine of the given railtype
 */
bool TemplateVehicleContainsEngineOfRailtype(const TemplateVehicle *tv, RailType type)
{
	if (tv != nullptr) tv = tv->First()->GetUnitMain();
	if (type == INVALID_RAILTYPE) return true;

	/* For standard rail engines, allow only those */
	if (type == RAILTYPE_BEGIN || type == RAILTYPE_RAIL) {
		while (tv != nullptr) {
			if (!tv->railtypes.Test(type)) {
				return false;
			}
			tv = tv->GetNextUnit();
		}
		return true;
	}
	/* For electrified rail engines, standard wagons or engines are allowed to be included */
	while (tv != nullptr) {
		if (tv->railtypes.Test(type)) {
			return true;
		}
		tv = tv->GetNextUnit();
	}
	return false;
}

Train *ChainContainsEngine(EngineID eid, Train *chain)
{
	for (Train *main : CollectTrainPurchasedUnits(chain)) {
		if (main->engine_type == eid) return main;
	}
	return nullptr;
}

static bool IsTrainUsableAsTemplateReplacementSource(const Train *t)
{
	if (t->First()->IsFreeWagon()) return true;

	if (t->Primary() == t && t->IsStoppedInDepot() && CollectTrainPurchasedUnits(const_cast<Train *>(t)).size() == 1) {
		if (t->GetNumOrders() != 0) return false;
		if (t->IsOrderListShared()) return false;
		if (t->group_id != DEFAULT_GROUP) return false;
		return true;
	}

	return false;
}

void TemplateDepotVehicles::Init(TileIndex tile)
{
	for (const Train *v : VehiclesOnTile<VehicleType::Train>(tile)) {
		if (v->IsArticulatedPart() || v->IsRearDualheaded() || v->GetUnitMain() != v) continue;
		this->vehicles.insert(v->index);
	}
}

void TemplateDepotVehicles::RemoveVehicle(VehicleID id)
{
	this->vehicles.erase(id);
}

Train *TemplateDepotVehicles::ContainsEngine(EngineID eid, Train *not_in)
{
	for (VehicleID id : this->vehicles) {
		Train *t = Train::GetIfValid(id);
		if (t == nullptr) continue;
		/* Conditions: v is stopped in the given depot, has the right engine and if 'not_in' is given v must not be contained within 'not_in'.
		 * If 'not_in' is nullptr, no check is needed. */
		if (t->owner == _current_company
				/* If the veh belongs to a chain, wagons will not return true on IsStoppedInDepot(), only primary vehicles will.
				 * In case of t not a primary veh, we demand it to be a free wagon to consider it for replacement. */
				&& IsTrainUsableAsTemplateReplacementSource(t)
				&& t->engine_type == eid
				&& (not_in == nullptr || not_in->First() != t->First())) {
			return t;
		}
	}
	return nullptr;
}

void NeutralizeStatus(Train *t)
{
	Command<Commands::AddVehicleToGroup>::Do(DoCommandFlag::Execute, DEFAULT_GROUP, t->index, false);
	Command<Commands::CloneOrder>::Do(DoCommandFlag::Execute, CO_SHARE, t->index, VehicleID::Invalid());
	Command<Commands::RenameVehicle>::Do(DoCommandFlag::Execute, t->index, {});
}

static const Train *TemplateTrainUnitMain(const Train *part)
{
	return part->IsRearDualheaded() && part->other_multiheaded_part != nullptr ? part->other_multiheaded_part : Train::From(part->GetUnitMain());
}

/** Per-purchase canonical member mapping; no zip across physical chains. */
static TemplateTrainMapping MapTemplateTrainMembers(Train *train, const TemplateVehicle *tv)
{
	TemplateTrainMapping result;
	if (train == nullptr || tv == nullptr) return result;
	const TemplateVehicle *unit = tv->First()->GetUnitMain();
	for (Train *main : CollectTrainPurchasedUnits(train)) {
		if (unit == nullptr) break;
		std::vector<const TemplateVehicle *> old_members;
		std::vector<Train *> new_members;
		const TemplateVehicle *old_rear = nullptr;
		for (const TemplateVehicle *p = tv->First(); p != nullptr; p = p->Next()) {
			if (p->GetUnitMain() != unit) continue;
			if (p->IsRearDualheaded()) old_rear = p;
			else old_members.push_back(p);
		}
		for (Train *p = train->First(); p != nullptr; p = p->Next()) {
			if (TemplateTrainUnitMain(p) == main && !p->IsRearDualheaded()) new_members.push_back(p);
		}
		if (!old_members.empty() && old_members.front() != unit) std::reverse(old_members.begin(), old_members.end());
		if (!new_members.empty() && new_members.front() != main) std::reverse(new_members.begin(), new_members.end());
		if (!old_members.empty()) {
			for (size_t ordinal = 0; ordinal < new_members.size(); ordinal++) result.emplace_back(old_members[std::min(ordinal, old_members.size() - 1)], new_members[ordinal]);
		}
		if (old_rear != nullptr && main->other_multiheaded_part != nullptr) result.emplace_back(old_rear, main->other_multiheaded_part);
		unit = unit->GetNextUnit();
	}
	return result;
}

TBTRDiffFlags TrainTemplateDifference(const Train *t, const TemplateVehicle *tv)
{
	if (t == nullptr || tv == nullptr) return t == nullptr && tv == nullptr ? TBTRDF_NONE : TBTRDF_ALL;
	t = t->First();
	tv = tv->First();
	TBTRDiffFlags diff = TBTRDF_NONE;
	const bool check_refit = tv->refit_as_template;
	auto units = CollectTrainPurchasedUnits(const_cast<Train *>(t));
	const TemplateVehicle *unit = tv->GetUnitMain();
	for (Train *main : units) {
		if (unit == nullptr || main->engine_type != unit->engine_type) return TBTRDF_ALL;
		if ((main == t->Primary()) != (unit == tv->Primary())) diff |= TBTRDF_CONSIST;
		unit = unit->GetNextUnit();
	}
	if (unit != nullptr) return TBTRDF_ALL;
	const auto mapping = MapTemplateTrainMembers(const_cast<Train *>(t), tv);
	/* Physical token ordering detects separated dual halves, reversed parts,
	 * and callback topology changes without charging repeated part engines. */
	const TemplateVehicle *physical = tv;
	for (const Train *p = t; p != nullptr; p = p->Next()) {
		auto mapped = std::find_if(mapping.begin(), mapping.end(), [p](const auto &m) { return m.second == p; });
		if (mapped == mapping.end()) { diff |= TBTRDF_CONSIST; continue; }
		const TemplateVehicle *old = mapped->first;
		if (physical != old) diff |= TBTRDF_CONSIST;
		if (physical != nullptr) physical = physical->Next();
		if (check_refit && (p->cargo_type != old->cargo_type || p->cargo_subtype != old->cargo_subtype)) diff |= TBTRDF_REFIT;
		if (p->flags.Test(VehicleRailFlag::Flipped) != HasBit(old->ctrl_flags, TVCF_REVERSED)) diff |= TBTRDF_DIR;
	}
	if (physical != nullptr) diff |= TBTRDF_CONSIST;
	return diff;
}

void BreakUpRemainders(Train *t)
{
	while (t != nullptr) {
		Train *next = t->GetNextVehicle();
		Train *main = Train::From(t->GetUnitMain());
		if (main->IsEngine() && !t->IsRearDualheaded()) {
			Command<Commands::MoveRailVehicle>::Do(DoCommandFlag::Execute, main->index, VehicleID::Invalid(), MoveRailVehicleFlags::NewHead);
			NeutralizeStatus(main->Primary());
		}
		t = next;
	}
}

uint CountTrainsNeedingTemplateReplacement(GroupID g_id, const TemplateVehicle *tv)
{
	uint count = 0;
	if (tv == nullptr) return count;

	for (const Train *t : Train::IterateFrontOnly()) {
		if (t->IsPrimaryVehicle() && t->group_id == g_id && TrainTemplateDifference(t, tv) != TBTRDF_NONE) {
			count++;
		}
	}
	return count;
}

/* Refit each vehicle in t as is in tv, assume t and tv contain the same types of vehicles */
CommandCost CmdRefitTrainFromTemplate(Train *t, const TemplateVehicle *tv, DoCommandFlags flags)
{
	CommandCost cost(t->GetExpenseType(false));

	for (const auto &[source, member] : MapTemplateTrainMembers(t, tv)) {
		cost.AddCost(Command<Commands::RefitVehicle>::Do(flags, member->index, source->cargo_type, source->cargo_subtype, false, true, 0));
	}
	return cost;
}

/* Set unit direction of each vehicle in t as is in tv, assume t and tv contain the same types of vehicles */
void CmdSetTrainUnitDirectionFromTemplate(Train *t, const TemplateVehicle *tv, DoCommandFlags flags)
{
	if (t == nullptr || tv == nullptr) return;
	tv = tv->First()->GetUnitMain();
	for (Train *main : CollectTrainPurchasedUnits(t)) {
		if (tv == nullptr) break;
		if (main->flags.Test(VehicleRailFlag::Flipped) != HasBit(tv->ctrl_flags, TVCF_REVERSED)) Command<Commands::ReverseTrainDirection>::Do(flags, main->index, true);
		tv = tv->GetNextUnit();
	}
}

/** Using CmdTemplateReplaceVehicle as a test function (i.e. without DoCommandFlag::Execute) is not a good idea as that function relies on
 *  actually moving vehicles around to work properly.
 *  We do this worst-cast test instead.
 */
CommandCost TestBuyAllTemplateVehiclesInChain(const TemplateVehicle *tv, TileIndex tile)
{
	CommandCost cost(ExpensesType::NewVehicles);

	for (tv = tv != nullptr ? tv->First()->GetUnitMain() : nullptr; tv != nullptr; tv = tv->GetNextUnit()) {
		cost.AddCost(Command<Commands::BuildVehicle>::Do({}, tile, tv->engine_type, false, INVALID_CARGO, ClientID::Invalid));
	}

	return cost;
}


/** Transfer as much cargo from a given (single train) vehicle onto a chain of vehicles.
 *  I.e., iterate over the chain from head to tail and use all available cargo capacity (w.r.t. cargo type of course)
 *  to store the cargo from the given single vehicle.
 *  @param old_veh:     ptr to the single vehicle, which's cargo shall be moved
 *  @param new_head:    ptr to the head of the chain, which shall obtain old_veh's cargo
 *  @return:            amount of moved cargo, TODO
 */
void TransferCargoForTrain(Train *old_veh, Train *new_head)
{
	new_head = new_head->First();

	const CargoType cargo_type = old_veh->cargo_type;
	const uint8_t cargo_subtype = old_veh->cargo_subtype;

	/* How much cargo has to be moved (if possible) */
	uint remainingAmount = old_veh->cargo.TotalCount();
	/* Each vehicle in the new chain shall be given as much of the old cargo as possible, until none is left */
	for (Train *tmp = new_head; tmp != nullptr && remainingAmount > 0; tmp = tmp->Next()) {
		if (tmp->cargo_type == cargo_type && tmp->cargo_subtype == cargo_subtype) {
			/* Calculate the free space for new cargo on the current vehicle */
				uint stored = tmp->cargo.TotalCount();
				uint curCap = stored < tmp->cargo_cap ? tmp->cargo_cap - stored : 0;
			uint moveAmount = std::min(remainingAmount, curCap);
			/* Move (parts of) the old vehicle's cargo onto the current vehicle of the new chain */
			if (moveAmount > 0) {
				old_veh->cargo.Shift(moveAmount, &tmp->cargo);
				remainingAmount -= moveAmount;
			}
		}
	}

	/* Update train weight etc., the old vehicle will be sold anyway */
	new_head->ConsistChanged(CCF_LOADUNLOAD);
}

void UpdateAllTemplateVehicleImages()
{
	SavedRandomSeeds saved_seeds;
	SaveRandomSeeds(&saved_seeds);

	for (TemplateVehicle *tv : TemplateVehicle::Iterate()) {
		if (tv->Prev() == nullptr) {
			Backup<CompanyID> cur_company(_current_company, tv->owner, FILE_LINE);
			StringID err;
			TemplateTrainMapping mapping;
			Train *t = VirtualTrainFromTemplateVehicle(tv, err, (ClientID)0, &mapping);
			if (t != nullptr) {
				for (const auto &[source, member] : mapping) {
					TemplateVehicle *u = const_cast<TemplateVehicle *>(source);
					member->GetImage(_current_text_dir == TD_RTL ? Direction::E : Direction::W, EngineImageType::InDepot, &u->sprite_seq);
					u->image_dimensions.SetFromTrain(member);
					u->colourmap = GetVehiclePalette(member);
				}
				delete t->First();
			}
			cur_company.Restore();
		}
	}

	RestoreRandomSeeds(saved_seeds);

	_template_vehicle_images_valid = true;
}

int GetTemplateVehicleEstimatedMaxAchievableSpeed(const TemplateVehicle *tv, int mass, const int speed_cap)
{
	int max_speed = 0;
	int acceleration;

	if (mass < 1) mass = 1;

	do {
		max_speed++;
		acceleration = GetTrainRealisticAccelerationAtSpeed(max_speed, mass, tv->power, tv->max_te, tv->air_drag, tv->railtypes);
	} while (acceleration > 0 && max_speed < speed_cap);

	return max_speed;
}
