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

enum class MoveRailVehicleFlags : uint8_t {
	None                  = 0,         ///< No flag set.
	MoveChain             = (1U << 0), ///< Move all vehicles following the source vehicle
	Virtual               = (1U << 1), ///< This is a virtual vehicle (for creating TemplateVehicles)
	NewHead               = (1U << 2), ///< When moving a head vehicle, always reset the head state
};
DECLARE_ENUM_AS_BIT_SET(MoveRailVehicleFlags)

DEF_CMD_TUPLE_LT (Commands::MoveRailVehicle,          CmdMoveRailVehicle,           {}, CommandType::VehicleConstruction, CmdDataT<VehicleID, VehicleID, MoveRailVehicleFlags>)
DEF_CMD_TUPLE_LT (Commands::ForceTrainProceed,        CmdForceTrainProceed,         {}, CommandType::VehicleManagement,   CmdDataT<VehicleID>)
DEF_CMD_TUPLE_LT (Commands::ReverseTrainDirection,    CmdReverseTrainDirection,     {}, CommandType::VehicleManagement,   CmdDataT<VehicleID, bool>)
DEF_CMD_TUPLE_LT (Commands::SetTrainSpeedRestriction, CmdSetTrainSpeedRestriction,  {}, CommandType::VehicleManagement,   CmdDataT<VehicleID, uint16_t>)

/** Tick of the last failed couple target search, per train (see DoTrainCouplePathfind).
 *  Persisted in the savegame (chunk CSFC, feature XSLFI_COUPLE_SEARCH_FAIL_CACHE):
 *  it gates couple target searches, so it must not differ between a long-running
 *  server and a freshly loading client. */
extern btree::btree_map<VehicleID, uint64_t> _couple_search_last_fail;

void ClearCoupleSearchLastFail();
void CancelTrainCoupleCheckpoint(Train *train);
bool TrainShouldWaitForCoupleAtCheckpoint(Train *train, bool *at_checkpoint = nullptr, StationID arrived = StationID::Invalid());

#endif /* TRAIN_CMD_H */
