/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file train_internal.h Internals shared between the train command and train coupling modules. */

#ifndef TRAIN_INTERNAL_H
#define TRAIN_INTERNAL_H

#include <vector>
#include "train.h"

struct TrainList : std::vector<Train *> {
	struct State {
		Train *vehicle;
		Vehicle *unit_main;
		Train *primary;
		uint8_t subtype;
		Direction direction;
		bool driving_backwards;
	};
	std::vector<State> states;
	void RestoreState() const
	{
		for (const State &s : this->states) {
			s.vehicle->SetUnitMain(s.unit_main);
			s.vehicle->SetPrimary(s.primary);
			s.vehicle->subtype = s.subtype;
			s.vehicle->direction = s.direction;
			s.vehicle->vehicle_flags.Set(VehicleFlag::DrivingBackwards, s.driving_backwards);
		}
	}
};

void MakeTrainBackup(TrainList &list, Train *t);
void RestoreTrainBackup(TrainList &list);
void RestoreChainFromList(const TrainList &list);
void ArrangeTrains(Train **dst_head, Train *dst, Train **src_head, Train *src, bool move_chain, bool pull_in_free_cars = true);
CommandCost ValidateTrains(Train *original_dst, Train *dst, Train *original_src, Train *src, bool check_limit);
void NormaliseTrainHead(Train *head, ConsistChangeFlags allowed_changes);
bool CheckReverseTrain(const Train *consist);
void RecheckSignalsAndTryReserve(Train *consist);
void RefreshConsistBreakdownState(Train *part);
void ReverseTrainNoSwapVehicles(Train *v);
void UpdateStatusAfterSwap(Train *v, bool reverse = true);
CommandCost CheckTrainAttachment(Train *t);

bool TrainController(Train *v, Vehicle *nomove, bool reverse = true); // Also used in vehicle_sl.cpp.

#endif /* TRAIN_INTERNAL_H */
