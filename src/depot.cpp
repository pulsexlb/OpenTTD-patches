/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file depot.cpp Handling of depots. */

#include "stdafx.h"
#include "depot_base.h"
#include "order_backup.h"
#include "order_func.h"
#include "order_base.h"
#include "vehicle_base.h"
#include "window_func.h"
#include "core/pool_func.hpp"
#include "vehicle_gui.h"
#include "vehiclelist.h"
#include "tracerestrict.h"
#include "departures_gui.h"

#include "safeguards.h"

/** All our depots tucked away in a pool. */
DepotPool _depot_pool("Depot");
INSTANTIATE_POOL_METHODS(Depot)

/**
 * Clean up a depot
 */
Depot::~Depot()
{
	if (CleaningPool()) return;

	if (!IsDepotTile(this->xy) || (!IsHangarTile(this->xy) && GetDepotIndex(this->xy) != this->index)) {
		/* It can happen there is no depot here anymore (TTO/TTD savegames) */
		return;
	}

	/* Clear the order backup. */
	OrderBackup::Reset(this->xy, false);

	/* Additional external-return references are not main order destinations.
	 * Clear them before the pool ID can be reused. Active trip snapshots keep
	 * their expected tile and safely fall back if this entrance disappears. */
	for (OrderList *list : OrderList::Iterate()) {
		for (Order &order : list->GetOrderVector()) {
			if (order.IsExternalTripOrder() && order.GetExternalReturnDepot() == this->index) order.SetExternalReturnDepot(DepotID::Invalid());
		}
	}
	for (Vehicle *vehicle : Vehicle::Iterate()) {
		if (vehicle->current_order.IsExternalTripOrder() && vehicle->current_order.GetExternalReturnDepot() == this->index) vehicle->current_order.SetExternalReturnDepot(DepotID::Invalid());
	}
	SetWindowClassesDirty(WindowClass::VehicleOrders);

	/* Clear the depot from all order-lists */
	RemoveOrderFromAllVehicles(OT_GOTO_DEPOT, this->index);

	TraceRestrictRemoveDestinationID(TROCAF_DEPOT, this->index);

	/* Delete the depot-window */
	CloseWindowById(WindowClass::VehicleDepot, this->xy.base());
	CloseWindowById(WindowClass::ExternalEntrance, this->xy.base());

	/* Delete the depot list */
	VehicleType vt = GetDepotVehicleType(this->xy);
	CloseWindowById(GetWindowClassForVehicleType(vt), VehicleListIdentifier(VehicleListType::Depot, vt, GetTileOwner(this->xy), this->index).ToWindowNumber());

	/* Delete any depot departure window */
	CloseDepotDeparturesWindow(this->xy);
}
