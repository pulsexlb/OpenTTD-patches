/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file train_cmd.h Command definitions related to trains. */

#ifndef TRAIN_CMD_H
#define TRAIN_CMD_H

#include "3rdparty/cpp-btree/btree_map.h"
#include "command_type.h"
#include "vehicle_type.h"
#include "train.h"

enum class MoveRailVehicleFlags : uint8_t {
	None                  = 0,         ///< No flag set.
	MoveChain             = (1U << 0), ///< Move all vehicles following the source vehicle
	Virtual               = (1U << 1), ///< This is a virtual vehicle (for creating TemplateVehicles)
	NewHead               = (1U << 2), ///< When moving a head vehicle, always reset the head state
};
DECLARE_ENUM_AS_BIT_SET(MoveRailVehicleFlags)

struct Order;

/** Bind a consist identity without copying orders, numbers, slots or statistics. */
void BindTrainPrimary(Train *head, Train *primary);
/** Move an identity between distinct same-owner vehicles; callers account group statistics. */
void MoveTrainIdentity(Train *old_primary, Train *new_primary);
/** Purchased unit bases in physical first-encounter order, including rear-first dual heads. */
std::vector<Train *> CollectTrainPurchasedUnits(Train *head);
/** Atomically validate/relink complete physical members, safely detaching omitted complete units.
 * No resource accounting/world movement; rebuilds caches but leaves capacity/cargo for caller commit. */
CommandCost RebuildTrainDepotLayout(const std::vector<Train *> &members, Train *primary, ConsistChangeFlags allowed_changes = CCF_ARRANGE);

DEF_CMD_TUPLE_LT (Commands::MoveRailVehicle,          CmdMoveRailVehicle,           {}, CommandType::VehicleConstruction, CmdDataT<VehicleID, VehicleID, MoveRailVehicleFlags>)
DEF_CMD_TUPLE_LT (Commands::ForceTrainProceed,        CmdForceTrainProceed,         {}, CommandType::VehicleManagement,   CmdDataT<VehicleID>)
DEF_CMD_TUPLE_LT (Commands::ReverseTrainDirection,    CmdReverseTrainDirection,     {}, CommandType::VehicleManagement,   CmdDataT<VehicleID, bool>)
DEF_CMD_TUPLE_LT (Commands::SetTrainSpeedRestriction, CmdSetTrainSpeedRestriction,  {}, CommandType::VehicleManagement,   CmdDataT<VehicleID, uint16_t>)

/** Vehicle-tick stamp (_scaled_tick_counter) of the last failed couple target
 *  search, per train (see DoTrainCouplePathfind).
 *  Persisted in the savegame (chunk CSFC, feature XSLFI_COUPLE_SEARCH_FAIL_CACHE):
 *  it gates couple target searches, so it must not differ between a long-running
 *  server and a freshly loading client. Saves written before the switch to
 *  vehicle ticks hold date ticks; those are simply treated as long expired. */
extern btree::btree_map<VehicleID, uint64_t> _couple_search_last_fail;

void ClearCoupleSearchLastFail();
void CancelTrainCoupleCheckpoint(Train *train);
void UpdateWaitCoupleSlot(Train *train);
bool TrainShouldWaitForCoupleAtCheckpoint(Train *train, bool *at_checkpoint = nullptr, StationID arrived = StationID::Invalid());
bool IsCoupleCheckpointEngaged(const Train *moving, const Order *order);

#endif /* TRAIN_CMD_H */
