/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file train_couple.h Interface of the train coupling/decoupling module.
 *
 *  Only the entry points that are new to this module live here; functions that
 *  were already declared in train.h / train_cmd.h keep their original
 *  declaration and are not duplicated. */

#ifndef TRAIN_COUPLE_H
#define TRAIN_COUPLE_H

#include "train.h"
#include "order_base.h"

static constexpr uint8_t DECOUPLE_NO_LOAD     = 0; ///< Neither part loads.
static constexpr uint8_t DECOUPLE_LOAD_FIRST  = 1; ///< Load the first part.
static constexpr uint8_t DECOUPLE_LOAD_SECOND = 2; ///< Load the second part.

bool ClaimOrWaitForCoupleOrder(Train *moving, const Order &couple);
void Couple(Train *v, Train *u);
int CoupleJointOffset(uint8_t front_len, uint8_t back_len, bool front_driving_backwards);
Train *DecoupleTrain(Train *v, bool &consist_in_rear, StringID &failure_reason);
Track DoTrainCouplePathfind(const Train *v, bool do_track_reservation, Train **couple_target, uint32_t *couple_cost, const Train *only_target = nullptr);
const Order *GetClaimCoupleOrder(const Train *moving);
Train *GetClaimedCoupleTarget(const Train *moving);
Train *GetValidCoupleClaimant(const Train *carrier);
const Train *GetWaitingCoupleCandidateOnTile(const Train *v, TileIndex tile);
void HoldWaitingTrainBody(Train *consist);
bool IsCouplePartnerOf(const Train *consist, const Train *other);
bool IsDecouplePartnerOnCutTile(const Train *consist, const Train *other, TileIndex tile);
bool IsHoldingForCouple(const Train *train);
StringID PrepareDecoupleWaitOrders(const Train *v, std::unique_ptr<OrderList> (&copies)[2]);
bool RepairStoppedTrainSpacing(Train *consist);
void RespaceVehicleTo(Train *v, const Train *target, int offset);
void ReverseTrainForCouple(Train *v, bool joint_at_tail);
bool ShouldCheckCoupleCheckpointDirection(const Train *train);
uint8_t SplitOrders(Train *v, Train *u, uint8_t &load_trains, std::unique_ptr<OrderList> (&wait_orders)[2], uint32_t inbound_time);

#endif /* TRAIN_COUPLE_H */
