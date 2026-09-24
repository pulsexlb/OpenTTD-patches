/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file roadveh_transport.cpp Road vehicles carried by other vehicles (RoRo): core transactions. */

#include "stdafx.h"

#include "roadveh_transport.h"

#include "cargotype.h"
#include "console_func.h"
#include "economy_base.h"   // CargoPayment must be complete: the station stop of a carried vehicle deletes its payment
#include "order_func.h"
#include "strings_func.h"
#include "town.h"
#include "date_func.h"
#include "debug.h"
#include "direction_func.h"
#include "direction_type.h"
#include "map_func.h"
#include "news_func.h"
#include "order_base.h"
#include "road_map.h"
#include "roadstop_base.h"
#include "tracerestrict.h"
#include "roadveh.h"
#include "settings_type.h"
#include "station_base.h"
#include "station_map.h"
#include "tilearea_type.h"
#include "vehicle_base.h"

#include "safeguards.h"

/** Weight of a road vehicle in tonnes (including its current cargo), from the consist weight cache. */
uint32_t RVTransportGetVehicleWeightTonnes(const Vehicle *rv)
{
	if (rv == nullptr || rv->type != VehicleType::Road) return 0;
	if (!rv->IsFrontEngine()) return 0;
	return RoadVehicle::From(rv)->gcache.cached_weight;
}

/**
 * Cargo label of the dedicated "Vehicles" cargo which vehicle transporting NewGRFs use for the cargo
 * of their car ferries / car carriers (see RVTransportCarrierParts::BulkOversizedOrVehicles).
 * This is the built-in cargo's own label, see CT_VEHICLES.
 */
static constexpr CargoLabel RV_TRANSPORT_VEHICLES_CARGO_LABEL = CT_VEHICLES;

/** Is this carrier part able to carry road vehicles? */
bool RVTransportPartCanCarry(const Vehicle *part)
{
	if (part == nullptr) return false;
	/* Master switch (vehicle.rv_transport_enabled): with it off nothing new is loaded, while unloading
	 * keeps working so that no vehicle stays on board forever. */
	if (!_settings_game.vehicle.rv_transport_enabled) return false;
	if (part->cargo_cap == 0) return false;
	if (!IsValidCargoType(part->cargo_type)) return false;

	switch (static_cast<RVTransportCarrierParts>(_settings_game.vehicle.rv_transport_carrier_parts)) {
		case RVTransportCarrierParts::AnyPart:
			return true;

		case RVTransportCarrierParts::OversizedOnly:
			return IsCargoInClass(part->cargo_type, CargoClass::Oversized);

		case RVTransportCarrierParts::BulkOversizedOrVehicles: {
			/* A part may take a road vehicle when its cargo is one of the cargoes which can really
			 * hold one:
			 *  - bulk cargo (open/hopper wagons: coal, ore, grain, ...),
			 *  - a cargo the NewGRF marked as 'oversized' (stake/flatbed wagons, car ferries),
			 *  - the dedicated "Vehicles" cargo (label 'VEHC') which vehicle transporting NewGRFs
			 *    (car ferries, car carriers) use for exactly this purpose.
			 * Everything else is refused, so a passenger carriage, a mail van, a wood/steel/goods
			 * van and a tank car never take a vehicle. The default content has no 'oversized' and
			 * no 'VEHC' cargo, so with it only bulk wagons qualify. */
			if (IsCargoInClass(part->cargo_type, CargoClass::Bulk)) return true;
			if (IsCargoInClass(part->cargo_type, CargoClass::Oversized)) return true;
			return CargoSpec::Get(part->cargo_type)->label == RV_TRANSPORT_VEHICLES_CARGO_LABEL;
		}
	}

	return false;
}

/** Transport capacity of a carrier part in tonnes, derived from its cargo capacity. */
uint32_t RVTransportGetPartCapacityTonnes(const Vehicle *part)
{
	if (!RVTransportPartCanCarry(part)) return 0;
	const CargoSpec *cs = CargoSpec::Get(part->cargo_type);
	/* CargoSpec::weight is in 1/16 tonne units per cargo unit. */
	return static_cast<uint32_t>(part->cargo_cap) * cs->weight / 16;
}

/** Tonnes already used on this carrier part by carried road vehicles. */
uint32_t RVTransportGetPartUsedTonnes(const Vehicle *part)
{
	if (part == nullptr) return 0;
	uint32_t used = 0;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & RVTF_TRANSPORTED) == 0) continue;
		if (v->transported_host_part != part->index) continue;
		used += v->transported_weight;
	}
	return used;
}

/** Number of road vehicles currently carried by this carrier. */
uint32_t RVTransportCountOnCarrier(const Vehicle *carrier)
{
	if (carrier == nullptr) return 0;
	uint32_t count = 0;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & RVTF_TRANSPORTED) == 0) continue;
		if (v->transported_by != carrier->index) continue;
		if (!v->IsFrontEngine()) continue;      // an articulated vehicle counts once
		count++;
	}
	return count;
}

/** First carried road vehicle of this carrier, or nullptr. */
Vehicle *RVTransportFindFirstOnCarrier(const Vehicle *carrier)
{
	if (carrier == nullptr) return nullptr;
	for (Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & RVTF_TRANSPORTED) == 0) continue;
		if (v->transported_by != carrier->index) continue;
		if (!v->IsFrontEngine()) continue;
		return v;
	}
	return nullptr;
}

/** Collect the road vehicles this carrier currently holds (front vehicles only, in vehicle id order). */
void RVTransportGetCarriedVehicles(const Vehicle *carrier, std::vector<const Vehicle *> &out)
{
	out.clear();
	if (carrier == nullptr) return;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) continue;
		if (v->transported_by != carrier->index) continue;
		if (!v->IsFrontEngine()) continue;
		out.push_back(v);
	}
}

/** Weight in tonnes of the road vehicles this carrier holds. */
uint32_t RVTransportGetCarriedWeightTonnes(const Vehicle *carrier)
{
	if (carrier == nullptr) return 0;
	uint32_t weight = 0;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) continue;
		if (v->transported_by != carrier->index) continue;
		if (!v->IsFrontEngine()) continue;
		weight += v->transported_weight;
	}
	return weight;
}

/** Does this carrier part hold any road vehicle? */
bool RVTransportPartHoldsRoadVehicles(const Vehicle *part)
{
	if (part == nullptr) return false;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) continue;
		if (v->transported_host_part != part->index) continue;
		if (!v->IsFrontEngine()) continue;
		return true;
	}
	return false;
}

/**
 * Cargo units a carrier part should report on top of what it really carries, so that NewGRF sets
 * which derive their sprites from the cargo amount (variables 0x3C/0x3D) also show the "loaded"
 * appearance while the part carries road vehicles: the part is reported as full.
 * Master switch off: returns 0 without scanning the vehicle pool (0x3C/0x3D are read on the sprite path).
 * @param part The carrier part (a road vehicle is never a carrier).
 * @return The number of units needed to fill the part, or 0 when it holds no road vehicles.
 */
uint16_t RVTransportExtraCargoAmount(const Vehicle *part)
{
	if (part == nullptr || part->type == VehicleType::Road) return 0;
	if (!_settings_game.vehicle.rv_transport_enabled) return 0;
	if (!RVTransportPartHoldsRoadVehicles(part)) return 0;
	const int stored = static_cast<int>(part->cargo.StoredCount());
	const int capacity = static_cast<int>(part->cargo_cap);
	return (capacity > stored) ? static_cast<uint16_t>(capacity - stored) : 0;
}

/**
 * Is this vehicle a dedicated road vehicle carrier: every cargo-carrying part of its chain is
 * refitted to the dedicated "Vehicles" cargo? Such a vehicle never transports normal cargo, so its
 * station orders default to (and its order buttons advertise) road vehicle transport. A vehicle
 * with no cargo capacity at all, or with any normal-cargo part, is not dedicated, and its orders
 * default to normal cargo.
 */
bool RVTransportVehicleCarriesOnlyVehicles(const Vehicle *v)
{
	if (v == nullptr) return false;
	const CargoType vehicles_cargo = RV_TRANSPORT_CARGO_SLOT;
	if (!IsValidCargoType(vehicles_cargo)) return false;

	bool has_cargo_part = false;
	for (const Vehicle *u = v; u != nullptr; u = u->Next()) {
		if (u->cargo_cap == 0) continue;
		has_cargo_part = true;
		if (u->cargo_type != vehicles_cargo) return false;
	}
	return has_cargo_part;
}

/** Set or clear the "waiting to be transported" state; a waiting vehicle is stopped. */
void RVTransportSetWaiting(Vehicle *rv, bool waiting)
{
	if (rv == nullptr || rv->type != VehicleType::Road) return;
	if ((rv->rv_transport_flags & RVTF_TRANSPORTED) != 0) return; // carried vehicles are not waiting

	if (waiting) {
		rv->rv_transport_flags |= RVTF_WAITING;
		rv->transport_wait_tick = static_cast<uint32_t>(_tick_counter);
		rv->rv_transport_flags &= ~RVTF_UNLOAD_WARNED;   // a new trip, so the warning may be shown again
		rv->vehstatus.Set(VehState::Stopped);
		rv->cur_speed = 0;
	} else {
		rv->rv_transport_flags &= ~RVTF_WAITING;
		rv->transport_wait_tick = 0;
		rv->vehstatus.Reset(VehState::Stopped);
	}
	SetWindowDirty(WindowClass::VehicleView, rv->index);
	SetWindowDirty(WindowClass::VehicleDetails, rv->index);
}

/**
 * End the "waiting to be transported" state when the vehicle was told to do something else in the
 * meantime: the player may skip the order or send the vehicle to a depot, in which case it must not
 * keep waiting (and, because a waiting vehicle is stopped, it would not even carry the order out).
 */
void RVTransportTickWaiting(Vehicle *rv)
{
	if (rv == nullptr || rv->type != VehicleType::Road) return;
	if ((rv->rv_transport_flags & RVTF_WAITING) == 0) return;

	/* With the master switch off a vehicle must not wait for a carrier which will never take it: it
	 * carries on with its own schedule instead. */
	if (!_settings_game.vehicle.rv_transport_enabled) {
		RVTransportSetWaiting(rv, false);
		return;
	}

	/* The vehicle keeps waiting while the order it is executing asks for it. While it stands at the
	 * station its current order is the *loading* order which Vehicle::BeginLoading() derived from the
	 * station order (the transport flags survive that conversion), so look at the station order in the
	 * order list instead: that is also what makes a change made in the order window take effect. */
	const Order *order = &rv->current_order;
	if (order->IsType(OT_LOADING)) {
		const Order *listed = rv->GetOrder(rv->cur_real_order_index);
		if (listed != nullptr) order = listed;
	}
	if ((order->IsType(OT_GOTO_STATION) || order->IsType(OT_LOADING)) && (order->GetRVTransportFlags() & ORVTF_LOAD) != 0) return;

	RVTransportSetWaiting(rv, false);
}

/**
 * Warn once per trip when a carried road vehicle has been on board for longer than the configured
 * number of days (vehicle.rv_transport_unload_warn_days, 0 = no warning).
 *
 * Road vehicles are only put down at a station their own schedule asks for, so a carrier which never
 * reaches such a station (or a vehicle whose drop-off station was removed) keeps them on board
 * indefinitely. Carriers are not "stuck" in any engine sense in that case, so this is what tells the
 * player about it. The warning repeats after the vehicle was loaded again (the flag is cleared when a
 * trip starts).
 * @param v The carried road vehicle.
 */
void RVTransportCheckCarriedTooLong(Vehicle *v)
{
	if (v == nullptr || v->type != VehicleType::Road || !v->IsFrontEngine()) return;
	if ((v->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) return;
	if ((v->rv_transport_flags & RVTF_UNLOAD_WARNED) != 0) return;

	const uint16_t warn_days = _settings_game.vehicle.rv_transport_unload_warn_days;
	if (warn_days == 0 || v->transport_wait_tick == 0) return;

	/* The same day length the "minimum waiting time" criterion uses. */
	const uint32_t carried_ticks = _tick_counter - v->transport_wait_tick;
	if (carried_ticks < static_cast<uint32_t>(warn_days) * DAY_TICKS) return;

	v->rv_transport_flags |= RVTF_UNLOAD_WARNED;
	AddNewsItem(GetEncodedString(STR_NEWS_RV_TRANSPORT_UNLOAD_OVERDUE, v->index, carried_ticks / DAY_TICKS),
			NewsType::Advice, NewsStyle::Small, {NewsFlag::InColour, NewsFlag::VehicleParam0}, v->index);
}

/**
 * Toggle one road vehicle transport flag of a station order, keeping the combination meaningful:
 * the destination match and waiting only make sense while road vehicles are loaded, and a road
 * vehicle's own order never uses the destination match.
 */
uint8_t RVTransportToggleOrderFlag(uint8_t flags, uint8_t bit, bool is_road_vehicle)
{
	const bool enabling = (flags & bit) == 0;

	switch (bit) {
		case ORVTF_LOAD:
			if (enabling) {
				flags |= ORVTF_LOAD;
				/* Default to taking only road vehicles heading for the carrier's next stop; the player
				 * can switch that off again with the entry of its own. */
				if (is_road_vehicle) {
					flags &= ~(ORVTF_MATCH_DEST | ORVTF_WAIT);
				} else {
					flags |= ORVTF_MATCH_DEST;
				}
			} else {
				flags &= ~(ORVTF_LOAD | ORVTF_MATCH_DEST | ORVTF_WAIT);
			}
			break;

		case ORVTF_MATCH_DEST:
		case ORVTF_WAIT:
			/* Both are only meaningful while road vehicles are loaded here. */
			flags = enabling ? (flags | bit | ORVTF_LOAD) : (flags & ~bit);
			break;

		case ORVTF_UNLOAD:
			flags = enabling ? (flags | ORVTF_UNLOAD) : (flags & ~(ORVTF_UNLOAD | ORVTF_UNLOAD_ALL));
			break;

		case ORVTF_UNLOAD_ALL:
			/* Only a carrier unloads: for a road vehicle the flag would be meaningless. It implies
			 * "unload road vehicles here", just with the vehicle's own declared destination ignored. */
			if (is_road_vehicle) break;
			flags = enabling ? (flags | ORVTF_UNLOAD_ALL | ORVTF_UNLOAD) : (flags & ~ORVTF_UNLOAD_ALL);
			break;

		default:
			NOT_REACHED();
	}

	return flags;
}

/**
 * Recompute the entry occupancy caches of the drive-through road stop at this tile from the vehicles
 * which are really on its tiles. A savegame load does the same for the whole map, which is why a game
 * which has drifted out of step can be "fixed" by loading it again; doing it after vehicles have been
 * put down keeps the caches exact instead.
 */
static void RVTransportRebuildRoadStop(TileIndex tile)
{
	if (!IsAnyRoadStopTile(tile) || IsBayRoadStopTile(tile)) return;

	const RoadStopType rst = GetRoadStopType(tile);
	const TileIndexDiff offset = TileOffsByAxis(GetDriveThroughStopAxis(tile));
	TileIndex base_tile = tile;
	for (TileIndex t = base_tile - offset; RoadStop::IsDriveThroughRoadStopContinuation(base_tile, t); t -= offset) base_tile = t;

	RoadStop *base = RoadStop::GetByTile(base_tile, rst);
	if (base == nullptr || !base->status.Test(RoadStop::RoadStopStatusFlag::BaseEntry)) return;
	base->GetEntry(DiagDirection::NE).Rebuild(base);
	base->GetEntry(DiagDirection::NW).Rebuild(base);
}

/**
 * A road vehicle which is loaded onto a carrier leaves the road stop it was waiting in.
 * A parking bay is just a flag, but a drive-through stop caches how much of its entry is occupied;
 * that cache can be out of step with reality (it is rebuilt from the vehicles on the tiles, and a
 * parked vehicle whose facing does not match the entry it used is not attributed to either entry),
 * so the subtraction used by RoadStop::Leave() cannot be used there. The occupancy is recomputed
 * instead, from the vehicles which are really on the stop - this must happen after the road vehicle
 * has been taken off the road network, so that it is not counted any more.
 */
static void RVTransportReleaseRoadStop(Vehicle *rv)
{
	if (rv == nullptr || !IsAnyRoadStopTile(rv->tile)) return;

	if (IsBayRoadStopTile(rv->tile)) {
		/* Bay stop: free the bay flag (the bay number is stored in the vehicle state). */
		RoadStop *rs = RoadStop::GetByTile(rv->tile, GetRoadStopType(rv->tile));
		if (rs != nullptr) rs->Leave(RoadVehicle::From(rv));
		return;
	}

	RVTransportRebuildRoadStop(rv->tile);
}

/**
 * Refresh a carrier after the road vehicles it holds changed: it is heavier now (MarkDirty()
 * recomputes the cached weight through CargoChanged(), which adds the carried vehicles, and the
 * acceleration), the affected part is drawn with its "loaded" appearance, and the details window
 * lists the vehicles which are on board now.
 * @param carrier Carrier front vehicle.
 * @param part The part which gained or lost a road vehicle.
 */
static void RVTransportRefreshCarrier(Vehicle *carrier, Vehicle *part)
{
	if (carrier == nullptr) return;

	/* Recomputes the weight, the consist image caches and the acceleration. */
	carrier->MarkDirty();

	if (part != nullptr) {
		/* A part which is not the front of a ship or aircraft is not covered by MarkDirty(). Only the
		 * image cache and the viewport may be touched here: UpdateViewportDeferred() would leave a
		 * pointer to this vehicle in the deferred viewport hash, which a later vehicle deletion (a
		 * destroyed carrier, for instance) turns into a dangling one. */
		part->InvalidateImageCache();
		part->UpdateViewport(true);
	}

	/* The details window lists the carried road vehicles: ships and aircraft grow/shrink with it. */
	InvalidateWindowData(WindowClass::VehicleDetails, carrier->index);
}

/**
 * Finish the station stop of a road vehicle which is about to be carried away by a carrier.
 *
 * A road vehicle which arrives at a station runs Vehicle::BeginLoading(), which registers it in the
 * station's list of loading vehicles (PrepareUnload()) and creates a CargoPayment for it. The engine
 * undoes both in Vehicle::LeaveStation() and Vehicle::PreDestructor() - and a vehicle which is loaded
 * onto a carrier never gets to run either of them, because it never leaves the station normally.
 * Left behind, the station keeps processing the vehicle as a loading one (which asserts as soon as its
 * current order has moved on) and the next station it arrives at asserts that it has no cargo payment
 * yet. So do the same bookkeeping here.
 *
 * @param rv The road vehicle (front vehicle) being carried away.
 */
static void RVTransportLeaveBoardingStation(Vehicle *rv)
{
	if (rv == nullptr) return;

	if (Station::IsValidID(rv->last_station_visited)) {
		Station *st = Station::Get(rv->last_station_visited);
		st->loading_vehicles.erase(std::remove(st->loading_vehicles.begin(), st->loading_vehicles.end(), rv), st->loading_vehicles.end());
		HideFillingPercent(&rv->fill_percent_te_id);
		rv->CancelReservation(StationID::Invalid(), st);
	}

	/* Settles the route profit earned so far and clears the pointer (~CargoPayment). */
	delete rv->cargo_payment;
	dbg_assert(rv->cargo_payment == nullptr);

	/* The vehicle is not loading anywhere at the moment. */
	rv->load_unload_ticks = 0;
	rv->vehicle_flags.Reset(VehicleFlag::LoadingFinished);
}

/**
 * Move a carried road vehicle on to its next order.
 *
 * A road vehicle which is loaded onto a carrier has done its "wait to be transported" order, so it is
 * moved on to the next one: that is the order which says where it wants to get off (see
 * RVTransportGetDeclaredDestination()), and it also means the vehicle continues its own schedule from
 * the station it is dropped at instead of driving back to the station it was picked up at. The
 * vehicle's own controller does not run while it is carried, so this is the only place which does it.
 */
static void RVTransportAdvanceCarriedVehicleOrder(Vehicle *rv)
{
	if (rv == nullptr || rv->GetNumOrders() == 0) return;
	/* The same sequence the engine uses when a vehicle finishes an order (Vehicle::HandleWaiting()):
	 * move the order index on, drop the current order and let the game work out the new one (which also
	 * evaluates conditional orders). */
	rv->IncrementImplicitOrderIndex();
	rv->current_order.Free();
	ProcessOrders(rv);
}

/**
 * Load one road vehicle onto a carrier part: the road vehicle leaves the road network
 * and is remembered by the carrier (single tick commit, no intermediate state).
 * @param force skip the cargo class / capacity checks (used by the debug self test).
 */
bool RVTransportAttach(Vehicle *carrier, Vehicle *part, Vehicle *rv, bool force)
{
	extern void UpdateVehicleTileHash(Vehicle *v, bool remove);

	if (carrier == nullptr || part == nullptr || rv == nullptr) return false;
	if (carrier->type == VehicleType::Road) return false; // carriers are trains/ships/aircraft, not road vehicles
	if (rv->type != VehicleType::Road) return false;
	if (!rv->IsFrontEngine()) return false;              // carriers take a whole road vehicle, never a lone part
	if ((rv->rv_transport_flags & RVTF_TRANSPORTED) != 0) return false;
	if (part->First() != carrier) return false;          // part must belong to this carrier
	if (!force && !RVTransportPartCanCarry(part)) return false;

	/* Articulated road vehicles are carried as a whole: cached_weight covers every part. */
	uint32_t weight = RVTransportGetVehicleWeightTonnes(rv);
	if (weight == 0) weight = 1;
	if (!force) {
		const uint32_t capacity = RVTransportGetPartCapacityTonnes(part);
		const uint32_t used = RVTransportGetPartUsedTonnes(part);
		if (used + weight > capacity) return false;      // refused: no room on this part
	}

	/* The road vehicle leaves the road stop it was loaded at: free a parking bay while the vehicle
	 * state still holds the bay number (a drive-through stop is handled after the road network
	 * removal below, when the vehicle is no longer part of the stop's occupancy). */
	if (IsBayRoadStopTile(rv->tile)) RVTransportReleaseRoadStop(rv);

	/* Hide the whole road vehicle: every part of an articulated vehicle is drawn and hashed on its
	 * own, and in a bend the parts are not even on the same tile. */
	rv->rv_transport_flags &= ~RVTF_WAITING;
	for (Vehicle *u = rv; u != nullptr; u = u->Next()) {
		u->rv_transport_flags |= RVTF_TRANSPORTED;
		u->transported_by = carrier->index;
		u->transported_host_part = part->index;
		/* Only the front records the weight of the whole (articulated) vehicle. */
		u->transported_weight = (u == rv) ? static_cast<uint16_t>(std::min<uint32_t>(weight, UINT16_MAX)) : 0;

		u->vehstatus.Set(VehState::Stopped);
		u->vehstatus.Set(VehState::Hidden);
		u->cur_speed = 0;
		RoadVehicle::From(u)->state = DiagDirToDiagTrackdir(DirToDiagDir(u->direction));
		UpdateVehicleTileHash(u, true);   // off the road network (like virtual vehicles)
		InvalidateVehicleTickCaches();
		u->UpdateIsDrawn();
		u->Vehicle::UpdateViewport(true); // appears/disappears: mark the area dirty
	}

	/* Drive-through stop: the vehicle is off the road network now, so the cached occupancy of the
	 * stop is recomputed without it. */
	if (!IsBayRoadStopTile(rv->tile)) RVTransportReleaseRoadStop(rv);

	/* The vehicle is no longer at the station it was picked up at: finish that station stop for it, so
	 * that nothing is left behind there (the engine only does this in Vehicle::LeaveStation(), which
	 * the vehicle never got to run - it was loaded while it was waiting). */
	RVTransportLeaveBoardingStation(rv);

	/* The road vehicle has done its "wait to be transported" order: move it on to its next one, which
	 * is where it wants to get off. */
	RVTransportAdvanceCarriedVehicleOrder(rv);

	/* Remember when this trip started: the "carried for too long" warning counts from here (the field
	 * is otherwise only read while the vehicle is *waiting*, where RVTransportSetWaiting() sets it
	 * again), and a new trip may warn again. */
	rv->transport_wait_tick = static_cast<uint32_t>(_tick_counter);
	rv->rv_transport_flags &= ~RVTF_UNLOAD_WARNED;

	carrier->MarkDirty();              // the carrier is heavier now and the part looks loaded
	RVTransportRefreshCarrier(carrier, part);
	return true;
}

/** Try to load a road vehicle onto any suitable part of the carrier. */
bool RVTransportAttachAuto(Vehicle *carrier, Vehicle *rv, bool force)
{
	if (carrier == nullptr) return false;

	/* The "load road vehicles" order of this carrier may cap how many it carries at once (0 = no cap);
	 * this is the one place through which every load goes, so the cap is checked here. */
	const uint8_t max_load = carrier->current_order.GetRVTransportMax();
	if (!force && max_load != 0 && RVTransportCountOnCarrier(carrier) >= max_load) return false;

	for (Vehicle *part = carrier; part != nullptr; part = part->Next()) {
		if (RVTransportAttach(carrier, part, rv, force)) return true;
	}
	return false;
}

/**
 * Find a road stop tile of this station where the given road vehicle can be put back on the road,
 * plus an exit direction which has road.
 *
 * The tile does not have to be empty: a drive-through stop has room per entry direction, so a tile
 * which is occupied on one side can still take a vehicle on the other side. Whole-tile emptiness is
 * only required for bay stops (whose bays are counted by the stop itself, but where a second vehicle
 * on the same tile would overlap).
 */
bool FindFreeRoadStopTile(const Station *st, Vehicle *rv, TileIndex &out_tile, DiagDirection &out_dd)
{
	for (int pass = 0; pass < 2; pass++) {
		const TileArea &area = (pass == 0) ? st->bus_station : st->truck_station;
		for (TileIndex t : area) {
			if (!IsAnyRoadStopTile(t)) continue;
			RoadStop *rs = RoadStop::GetByTile(t, GetRoadStopType(t));
			if (rs == nullptr) continue;

			for (DiagDirection dd = DiagDirection::Begin; dd < DiagDirection::End; dd++) {
				TileIndex next = TileAddByDiagDir(t, dd);
				if (!IsValidTile(next)) continue;
				if (!IsNormalRoadTile(next) && !IsAnyRoadStopTile(next)) continue;

				if (IsBayRoadStopTile(t)) {
					/* Bay stops cannot hold articulated road vehicles, and each tile holds at most one
					 * (the stop counts the bays, the tile itself must be free). */
					if (rv->HasArticulatedPart()) continue;
					if (!rs->HasFreeBay()) continue;
					if (rs->IsEntranceBusy()) continue;
					if (GetFirstVehicleOnTile(t, VehicleType::Road) != nullptr) continue;
				} else {
					/* Drive-through stop: the room of the entry this vehicle would use decides. */
					const RoadStop::Entry &entry = rs->GetEntry(dd);
					if (entry.GetLength() == 0) continue;
					if (entry.GetOccupied() + static_cast<int>(RoadVehicle::From(rv)->gcache.cached_total_length) > entry.GetLength()) continue;
				}

				out_tile = t;
				out_dd = dd;
				return true;
			}
		}
	}
	return false;
}

/* Debug helpers: only compiled in for a test build (RORO_DEBUG_COMMANDS), see console_cmds.cpp. */
#ifdef RORO_DEBUG_COMMANDS

/**
 * Debug (RoRo): print one station's road stops and whether the given road vehicle could be put down
 * there, using exactly the lookup the unload transaction uses.
 */
void RVTransportDebugStation(const Vehicle *rv, const Station *st)
{
	if (st == nullptr) { IConsolePrint(CC_ERROR, "station not found"); return; }

	IConsolePrint(CC_DEFAULT, "station #{} '{}' at 0x{:X} (town {}): bus area {}x{}, truck area {}x{}",
			st->index.base(), GetString(STR_STATION_NAME, st->index), st->xy.base(),
			(st->town != nullptr) ? st->town->index.base() : (uint16_t)UINT16_MAX,
			st->bus_station.w, st->bus_station.h, st->truck_station.w, st->truck_station.h);

	int stops = 0;
	for (int pass = 0; pass < 2; pass++) {
		const TileArea &area = (pass == 0) ? st->bus_station : st->truck_station;
		for (TileIndex t : area) {
			if (!IsAnyRoadStopTile(t)) continue;
			RoadStop *rs = RoadStop::GetByTile(t, GetRoadStopType(t));
			if (rs == nullptr) continue;
			stops++;
			if (IsBayRoadStopTile(t)) {
				IConsolePrint(CC_DEFAULT, "  stop 0x{:X} bay: free_bay={} entrance_busy={} vehicle_on_tile={}",
						t.base(), rs->HasFreeBay(), rs->IsEntranceBusy(),
						(GetFirstVehicleOnTile(t, VehicleType::Road) != nullptr));
			} else {
				const RoadStop::Entry &ne = rs->GetEntry(DiagDirection::NE);
				const RoadStop::Entry &nw = rs->GetEntry(DiagDirection::NW);
				IConsolePrint(CC_DEFAULT, "  stop 0x{:X} drive-through: NE {}/{} NW {}/{}",
						t.base(), ne.GetOccupied(), ne.GetLength(), nw.GetOccupied(), nw.GetLength());
			}
		}
	}
	IConsolePrint(CC_DEFAULT, "  road stop tiles found: {}", stops);

	if (rv != nullptr) {
		TileIndex tile = INVALID_TILE;
		DiagDirection dd = DiagDirection::NE;
		const bool ok = FindFreeRoadStopTile(st, const_cast<Vehicle *>(rv), tile, dd);
		if (ok) {
			IConsolePrint(CC_DEFAULT, "  rv #{} could be put down on tile 0x{:X} (exit dir {})", rv->index.base(), tile.base(), (int)dd);
		} else {
			IConsolePrint(CC_DEFAULT, "  rv #{} could NOT be put down here (no free road stop)", rv->index.base());
		}
	}
}

/**
 * Debug (RoRo): one-shot dump of everything the road vehicle transport needs to make sense: every
 * road vehicle with its state and declared destination, every carrier with its parts, its current
 * order and the road vehicles it holds, and for each of those the station it is heading for.
 */
void RVTransportDebugDump()
{
	extern void UpdateVehicleTileHash(Vehicle *v, bool remove);

	IConsolePrint(CC_DEFAULT, "-- road vehicles --");
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->type != VehicleType::Road || !v->IsFrontEngine()) continue;
		const Order *cur = (v->cur_real_order_index < v->GetNumOrders()) ? v->GetOrder(v->cur_real_order_index) : nullptr;
		const StationID cur_station = (cur != nullptr && cur->IsType(OT_GOTO_STATION)) ? cur->GetDestination().ToStationID() : StationID::Invalid();
		IConsolePrint(CC_DEFAULT, "rv #{} flags={} (wait={} carried={}) vehstatus=0x{:X} stopped={} hidden={} drawn={} state={} progress={} speed={} subspeed={} load_unload_ticks={} in_loading_list={} payment={} tile=0x{:X} is_station_tile={} cargo={}/{} order#{} type={} dest={} '{}' declared_dest={} carried_by={} host_part={} weight={}",
				v->index.base(), v->rv_transport_flags,
				((v->rv_transport_flags & RVTF_WAITING) != 0), ((v->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) != 0),
				v->vehstatus.base(), v->vehstatus.Test(VehState::Stopped), v->vehstatus.Test(VehState::Hidden),
				v->IsDrawn(), (int)RoadVehicle::From(v)->state, v->progress, v->cur_speed, v->subspeed,
				v->load_unload_ticks, RVTransportDebugStationLists(v), (v->cargo_payment != nullptr),
				v->tile.base(), IsAnyRoadStopTile(v->tile),
				v->cargo.StoredCount(), v->cargo_cap, v->cur_real_order_index,
				(cur != nullptr) ? (int)cur->GetType() : -1,
				(cur_station != StationID::Invalid()) ? (int)cur_station.base() : -1,
				(cur_station != StationID::Invalid()) ? GetString(STR_STATION_NAME, cur_station) : std::string("<none>"),
				RVTransportGetDeclaredDestination(v).base(),
				v->transported_by.base(), v->transported_host_part.base(), v->transported_weight);
	}

	IConsolePrint(CC_DEFAULT, "-- carriers --");
	for (const Vehicle *carrier : Vehicle::Iterate()) {
		if (carrier->type == VehicleType::Road) continue;
		if (!carrier->IsPrimaryVehicle()) continue;

		const Station *st = Station::GetIfValid(carrier->current_order.GetDestination().ToStationID());
		IConsolePrint(CC_DEFAULT, "carrier #{} type={} tile=0x{:X} order: type={} station={} rvflags={} (load={} unload={} unload_all={} match={} wait={}) -> station {} '{}'",
				carrier->index.base(), (int)carrier->type, carrier->tile.base(), (int)carrier->current_order.GetType(),
				carrier->current_order.IsType(OT_GOTO_STATION), carrier->current_order.GetRVTransportFlags(),
				((carrier->current_order.GetRVTransportFlags() & ORVTF_LOAD) != 0),
				((carrier->current_order.GetRVTransportFlags() & ORVTF_UNLOAD) != 0),
				((carrier->current_order.GetRVTransportFlags() & ORVTF_UNLOAD_ALL) != 0),
				((carrier->current_order.GetRVTransportFlags() & ORVTF_MATCH_DEST) != 0),
				((carrier->current_order.GetRVTransportFlags() & ORVTF_WAIT) != 0),
				st != nullptr ? (int)st->index.base() : -1,
				st != nullptr ? GetString(STR_STATION_NAME, st->index) : std::string("<none>"));

		int n = 0;
		for (const Vehicle *part = carrier; part != nullptr; part = part->Next(), n++) {
			IConsolePrint(CC_DEFAULT, "  part {}: #{} cargo={} cap={} stored={} rv_capacity={}t rv_used={}t holds_rv={}",
					n, part->index.base(), (int)part->cargo_type, part->cargo_cap, part->cargo.StoredCount(),
					RVTransportGetPartCapacityTonnes(part), RVTransportGetPartUsedTonnes(part), RVTransportPartHoldsRoadVehicles(part));
		}

		std::vector<const Vehicle *> carried;
		RVTransportGetCarriedVehicles(carrier, carried);
		for (const Vehicle *rv : carried) {
			const StationID declared = RVTransportGetDeclaredDestination(rv);
			IConsolePrint(CC_DEFAULT, "  holds rv #{} weight={}t declared_dest={} '{}' -> unloads here: {}",
					rv->index.base(), rv->transported_weight,
					(declared != StationID::Invalid()) ? (int)declared.base() : -1,
					(declared != StationID::Invalid()) ? GetString(STR_STATION_NAME, declared) : std::string("<none>"),
					(declared == StationID::Invalid() || (st != nullptr && declared == st->index)));
		}

		if (st != nullptr) {
			const Vehicle *first = RVTransportFindFirstOnCarrier(carrier);
			RVTransportDebugStation(first, st);
		}
	}
}

/**
 * Debug (RoRo): is this vehicle still in any station's list of vehicles which are loading there?
 */
bool RVTransportDebugStationLists(const Vehicle *v)
{
	if (v == nullptr) return false;
	for (const Station *st : Station::Iterate()) {
		if (std::find(st->loading_vehicles.begin(), st->loading_vehicles.end(), v) != st->loading_vehicles.end()) return true;
	}
	return false;
}

#endif /* RORO_DEBUG_COMMANDS */

/**
 * Unload road vehicles carried by this carrier at the given station.
 * @return true if at least one road vehicle reached the road network.
 */
bool RVTransportDetachAtStation(Vehicle *carrier, Station *st, bool force)
{
	extern void UpdateVehicleTileHash(Vehicle *v, bool remove);

	if (carrier == nullptr || st == nullptr) return false;

	bool any = false;
	for (Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & RVTF_TRANSPORTED) == 0) continue;
		if (v->transported_by != carrier->index) continue;
		if (!v->IsFrontEngine()) continue;          // the parts are handled together with the front

		/* Only unload a road vehicle which wants to be dropped here: the station of its own "be
		 * unloaded here" order. A road vehicle which declares no destination at all is dropped at the
		 * carrier's unload order, as it would otherwise never leave the carrier - and a carrier whose
		 * order says "unload all road vehicles" drops everything, whatever the vehicles declare. */
		if (!force && (carrier->current_order.GetRVTransportFlags() & ORVTF_UNLOAD_ALL) == 0) {
			const StationID declared = RVTransportGetDeclaredDestination(v);
			if (declared != StationID::Invalid() && declared != st->index) continue;
		}

		TileIndex tile = INVALID_TILE;
		DiagDirection dd = DiagDirection::NE;
		if (!FindFreeRoadStopTile(st, v, tile, dd)) continue; // no room: stay on the carrier, retry later

		/* Remember how the vehicle was carried, in case the stop refuses it below. */
		const VehicleID host_part = v->transported_host_part;
		const uint16_t carried_weight = v->transported_weight;

		/* A bay is a dead end: the vehicle drives into it and reverses out again, so it has to be put
		 * down travelling towards the station, exactly like a vehicle which just entered the tile.
		 * A drive-through stop is left through the road end, so the vehicle keeps travelling towards
		 * it. Getting this wrong leaves the vehicle facing the wall of the bay, and it then drives
		 * into that wall when it tries to leave. */
		const DiagDirection travel_dd = IsBayRoadStopTile(tile) ? ReverseDiagDir(dd) : dd;

		/* Put the whole road vehicle on that tile, in the same way a vehicle leaves a depot: every
		 * part starts on the tile and spreads out while the vehicle drives off. */
		for (Vehicle *u = v; u != nullptr; u = u->Next()) {
			u->rv_transport_flags &= ~RVTF_TRANSPORTED;
			u->transported_by = VehicleID::Invalid();
			u->transported_host_part = VehicleID::Invalid();
			u->transported_weight = 0;
			u->transport_wait_tick = 0;

			RoadVehicle *rv = RoadVehicle::From(u);
			u->tile = tile;
			u->x_pos = TileX(tile) * TILE_SIZE + TILE_SIZE / 2;
			u->y_pos = TileY(tile) * TILE_SIZE + TILE_SIZE / 2;
			u->z_pos = GetSlopePixelZ(u->x_pos, u->y_pos);
			u->direction = DiagDirToDir(travel_dd);
			rv->state = DiagDirToDiagTrackdir(travel_dd);
			rv->frame = 0;
			u->progress = 0;
			u->cur_speed = 0;
			u->vehstatus.Reset(VehState::Hidden);
			u->vehstatus.Reset(VehState::Stopped);
			UpdateVehicleTileHash(u, true);    // make sure it is not listed where it came from
			UpdateVehicleTileHash(u, false);   // back on the road network
			InvalidateVehicleTickCaches();
			u->UpdateIsDrawn();
			u->Vehicle::UpdateViewport(true); // appears/disappears: mark the area dirty
		}

		/* Let the road stop itself account for the vehicle: a parking bay is allocated, or the
		 * occupancy of the drive-through entry it uses is increased. The engine releases it again
		 * (RoadStop::Leave()) when the road vehicle drives off. */
		RoadStop *rs = RoadStop::GetByTile(tile, GetRoadStopType(tile));
		if (rs == nullptr || !rs->Enter(RoadVehicle::From(v))) {
			/* The stop refused the vehicle after all: put it back on the carrier rather than leaving
			 * it half placed (its room was checked above, so this should not happen). */
			for (Vehicle *u = v; u != nullptr; u = u->Next()) {
				u->rv_transport_flags |= RVTF_TRANSPORTED;
				u->transported_by = carrier->index;
				u->transported_host_part = host_part;
				u->transported_weight = carried_weight;
				u->vehstatus.Set(VehState::Stopped);
				u->vehstatus.Set(VehState::Hidden);
				u->cur_speed = 0;
				UpdateVehicleTileHash(u, true);   // off the road network (like virtual vehicles)
				InvalidateVehicleTickCaches();
				u->UpdateIsDrawn();
				u->Vehicle::UpdateViewport(true); // appears/disappears: mark the area dirty
			}
			continue;
		}

		/* The stop's own bookkeeping adds the vehicle up entry by entry; recompute the entry caches from
		 * the vehicles which are really on the tiles (exactly what a savegame load does), so that a
		 * vehicle which was put down can always drive out again. */
		RVTransportRebuildRoadStop(tile);

		/* RoadStop::Enter() only set the road stop state on the front. A drive-through stop also takes
		 * articulated vehicles, and every part drives on the stop with that state, so give them all
		 * the state the front got. */
		if (IsDriveThroughStopTile(tile)) {
			for (Vehicle *u = v->Next(); u != nullptr; u = u->Next()) {
				SetBit(RoadVehicle::From(u)->state, RVS_IN_DT_ROAD_STOP);
			}
		}

		carrier->MarkDirty();
		RVTransportRefreshCarrier(carrier, Vehicle::GetIfValid(host_part));
		any = true;
	}
	return any;
}

/**
 * Station at which a road vehicle wants to be unloaded on the leg it is on: the first of its own
 * station orders from the order it is executing on (that one included) which asks for unloading road
 * vehicles ("declared destination").
 *
 * Both order positions matter: while the vehicle waits for a carrier, its current order is the "wait
 * to be transported" one, so the answer is the *next* station it wants to get off at; once it has been
 * loaded, RVTransportAdvanceCarriedVehicleOrder() has moved it on, so the answer is its current order.
 * Reading only the first order of the whole list would keep a vehicle which is carried more than once
 * (train from A to B, drive to C, ship from C to D) on board of every carrier after the first leg.
 *
 * @return The station id, or an invalid id when the vehicle declares no destination.
 */
StationID RVTransportGetDeclaredDestination(const Vehicle *rv)
{
	if (rv == nullptr) return StationID::Invalid();
	const VehicleOrderID num_orders = rv->GetNumOrders();
	if (num_orders == 0) return StationID::Invalid();

	const VehicleOrderID current = (rv->cur_real_order_index < num_orders) ? rv->cur_real_order_index : 0;
	for (VehicleOrderID i = 0; i < num_orders; i++) {
		const Order *o = rv->GetOrder(static_cast<VehicleOrderID>((current + i) % num_orders));
		if (o == nullptr || !o->IsType(OT_GOTO_STATION)) continue;
		if ((o->GetRVTransportFlags() & ORVTF_UNLOAD) != 0) return o->GetDestination().ToStationID();
	}
	return StationID::Invalid();
}

/** How many carried road vehicles of this carrier want to be dropped at this station. */
uint32_t RVTransportCountWantingUnloadHere(const Vehicle *carrier, const Station *st)
{
	if (carrier == nullptr || st == nullptr) return 0;
	uint32_t count = 0;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) continue;
		if (v->transported_by != carrier->index) continue;
		if (!v->IsFrontEngine()) continue;
		const StationID declared = RVTransportGetDeclaredDestination(v);
		if (declared != StationID::Invalid() && declared != st->index) continue; // wants another station
		count++;
	}
	return count;
}

/**
 * Next station the carrier stops at after its current order.
 * @return The station id, or an invalid id when there is none.
 */
StationID RVTransportGetNextCarrierStop(const Vehicle *carrier)
{
	if (carrier == nullptr || carrier->orders == nullptr) return StationID::Invalid();
	const OrderList *ol = carrier->orders;
	const VehicleOrderID num = ol->GetNumOrders();
	for (VehicleOrderID i = static_cast<VehicleOrderID>(carrier->cur_real_order_index + 1); i < num; i++) {
		const Order *o = ol->GetOrderAt(i);
		if (o != nullptr && o->IsType(OT_GOTO_STATION)) return o->GetDestination().ToStationID();
	}
	return StationID::Invalid();
}

/** Total cargo of a (possibly articulated) road vehicle. */
static uint32_t RVTransportCandidateCargoCount(const Vehicle *rv)
{
	uint32_t count = 0;
	for (const Vehicle *u = rv; u != nullptr; u = u->Next()) count += u->cargo.StoredCount();
	return count;
}

/** Total cargo capacity of a (possibly articulated) road vehicle. */
static uint32_t RVTransportCandidateCargoCapacity(const Vehicle *rv)
{
	uint32_t cap = 0;
	for (const Vehicle *u = rv; u != nullptr; u = u->Next()) cap += u->cargo_cap;
	return cap;
}

/** Does this station order select road vehicles by any criterion? */
bool RVTransportOrderHasCriteria(const Order &order)
{
	if ((order.GetRVTransportFlags() & ORVTF_MATCH_DEST) != 0) return true;
	if (order.GetRVTransportLoadState() != RVTLS_ANY) return true;
	if (order.GetRVTransportCargoMode() != RVTC_ANY) return true;
	if (order.GetRVTransportMinWait() != 0) return true;
	if (order.GetRVTransportSlot() != 0) return true;
	if (order.GetRVTransportMax() != 0) return true;
	return false;
}

/**
 * Would this station order take the given road vehicle as a candidate? Every criterion which is in
 * use must be satisfied; a road vehicle which does not match is skipped (it keeps waiting for
 * another carrier), exactly like the destination match works on its own.
 */
bool RVTransportOrderAllowsCandidate(const Vehicle *carrier, const Vehicle *rv)
{
	if (carrier == nullptr || rv == nullptr) return false;
	const Order &order = carrier->current_order;

	/* Declared destination: the carrier's next stop (the road vehicle's own "be unloaded here" order). */
	if ((order.GetRVTransportFlags() & ORVTF_MATCH_DEST) != 0) {
		if (RVTransportGetDeclaredDestination(rv) != RVTransportGetNextCarrierStop(carrier)) return false;
	}

	/* Trace restrict slot ("鐠侯垳顒?): the candidate must be an occupant of that slot, which is how a
	 * specific road vehicle can be picked (the same mechanism the px-patch coupling feature uses). */
	if (const uint16_t slot_raw = order.GetRVTransportSlot(); slot_raw != 0) {
		const TraceRestrictSlot *slot = TraceRestrictSlot::GetIfValid(TraceRestrictSlotID{static_cast<uint16_t>(slot_raw - 1)});
		if (slot == nullptr || !slot->IsOccupant(rv->index)) return false;
	}

	/* Load state of the candidate. */
	switch (order.GetRVTransportLoadState()) {
		case RVTLS_EMPTY:
			if (RVTransportCandidateCargoCount(rv) != 0) return false;
			break;

		case RVTLS_FULL: {
			const uint32_t capacity = RVTransportCandidateCargoCapacity(rv);
			if (capacity == 0 || RVTransportCandidateCargoCount(rv) != capacity) return false;
			break;
		}

		default:
			break;
	}

	/* Cargo criterion. */
	const uint8_t cargo_mode = order.GetRVTransportCargoMode();
	if (cargo_mode != RVTC_ANY) {
		const CargoType cargo = static_cast<CargoType>(order.GetRVTransportCargo());
		if (!IsValidCargoType(cargo)) return false;

		bool ok = false;
		for (const Vehicle *u = rv; u != nullptr && !ok; u = u->Next()) {
			if (u->cargo_type != cargo) continue;
			ok = (cargo_mode == RVTC_CAN_CARRY) ? (u->cargo_cap > 0) : (u->cargo.StoredCount() > 0);
		}
		if (!ok) return false;
	}

	/* Minimum waiting time. */
	const uint16_t min_wait_days = order.GetRVTransportMinWait();
	if (min_wait_days != 0) {
		if (rv->transport_wait_tick == 0) return false;
		if (_tick_counter - rv->transport_wait_tick < static_cast<uint32_t>(min_wait_days) * DAY_TICKS) return false;
	}

	return true;
}

/**
 * First road vehicle waiting to be transported at this station which satisfies the selection
 * criteria of the carrier's current order.
 * @param st Station to look at.
 * @param carrier Carrier which wants to load; when given, its order criteria are applied.
 */
Vehicle *RVTransportFindWaitingAtStation(const Station *st, const Vehicle *carrier)
{
	if (st == nullptr) return nullptr;
	for (Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & RVTF_WAITING) == 0) continue;
		if (v->type != VehicleType::Road) continue;
		if (!v->IsFrontEngine()) continue;
		if (v->last_station_visited != st->index) continue;
		if (carrier != nullptr && !RVTransportOrderAllowsCandidate(carrier, v)) continue; // does not match: skip it
		return v;
	}
	return nullptr;
}

/**
 * Check the carried state of all road vehicles after a savegame was loaded. A road vehicle which
 * claims to be carried by a vehicle that does not exist any more (or by something which cannot be
 * a carrier) must not stay hidden and frozen on the map: it is put back on the road, or, if there
 * is no sane place for it, removed.
 */
void RVTransportValidateAfterLoad()
{
	std::vector<VehicleID> release;
	std::vector<VehicleID> remove;

	for (Vehicle *v : Vehicle::Iterate()) {
		if (v->type != VehicleType::Road) continue;

		if (!v->IsFrontEngine()) {
			/* A part of an articulated road vehicle follows its front: only clear stale state here. */
			if ((v->First()->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) {
				v->rv_transport_flags &= ~Vehicle::RV_TRANSPORT_CARRIED;
				v->transported_by = VehicleID::Invalid();
				v->transported_host_part = VehicleID::Invalid();
				v->transported_weight = 0;
				v->vehstatus.Reset(VehState::Hidden);
			}
			continue;
		}

		if ((v->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) {
			/* Not carried: drop any stale carrier reference left behind. */
			v->transported_by = VehicleID::Invalid();
			v->transported_host_part = VehicleID::Invalid();
			v->transported_weight = 0;
			continue;
		}

		const Vehicle *carrier = Vehicle::GetIfValid(v->transported_by);
		if (carrier != nullptr && carrier->type != VehicleType::Road && carrier->First() == carrier) {
			/* Carried as expected. A savegame written before the road vehicle's order was advanced when
			 * it was loaded still has the "wait to be transported" order as its current order: catch
			 * up. The station stop of the vehicle is finished here as well - a savegame can also have
			 * been written before that bookkeeping existed, in which case the station it was picked up
			 * at still lists it as a loading vehicle and it still holds a cargo payment. */
			RVTransportLeaveBoardingStation(v);
			if ((v->current_order.GetRVTransportFlags() & ORVTF_LOAD) != 0) RVTransportAdvanceCarriedVehicleOrder(v);
			continue; // carried as expected
		}

		Debug(misc, 0, "RoRo: road vehicle #{} was carried by missing vehicle #{}", v->index.base(), v->transported_by.base());
		v->transported_by = VehicleID::Invalid();
		v->transported_host_part = VehicleID::Invalid();
		if (IsValidTile(v->tile) && (IsAnyRoadStopTile(v->tile) || IsNormalRoadTile(v->tile))) {
			release.push_back(v->index);
		} else {
			remove.push_back(v->index);
		}
	}

	for (const VehicleID id : release) {
		Vehicle *v = Vehicle::GetIfValid(id);
		if (v != nullptr) RVTransportForceRelease(v);
	}
	for (const VehicleID id : remove) {
		Vehicle *front = Vehicle::GetIfValid(id);
		if (front == nullptr) continue;

		std::vector<VehicleID> chain;
		for (Vehicle *u = front; u != nullptr; u = u->Next()) chain.push_back(u->index);
		for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
			Vehicle *u = Vehicle::GetIfValid(*it);
			if (u == nullptr) continue;
			Debug(misc, 0, "RoRo: removing road vehicle #{} which was carried and has no valid tile", u->index.base());
			u->rv_transport_flags &= ~Vehicle::RV_TRANSPORT_CARRIED;
			u->transported_by = VehicleID::Invalid();
			u->transported_host_part = VehicleID::Invalid();
			u->transported_weight = 0;
			if (u->Previous() != nullptr) u->Previous()->SetNext(nullptr);
			delete u;
		}
	}
}

/**
 * Emergency release: put a carried road vehicle back on the road network at its remembered
 * tile. Kept for the debug console command and as a safety net (e.g. for a vehicle which is
 * carried by something that no longer exists after loading a savegame).
 */
void RVTransportForceRelease(Vehicle *rv)
{
	extern void UpdateVehicleTileHash(Vehicle *v, bool remove);

	if (rv == nullptr) return;
	if ((rv->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) return;

	/* Release the whole (possibly articulated) road vehicle at its remembered tile. */
	const TileIndex tile = rv->tile;
	for (Vehicle *u = rv; u != nullptr; u = u->Next()) {
		u->rv_transport_flags &= ~Vehicle::RV_TRANSPORT_CARRIED;
		u->transported_by = VehicleID::Invalid();
		u->transported_host_part = VehicleID::Invalid();
		u->transported_weight = 0;
		u->vehstatus.Reset(VehState::Hidden);
		u->vehstatus.Reset(VehState::Stopped);
		u->cur_speed = 0;

		if (u->type == VehicleType::Road && IsValidTile(tile)) {
			RoadVehicle *rvv = RoadVehicle::From(u);
			u->tile = tile;
			u->x_pos = TileX(tile) * TILE_SIZE + TILE_SIZE / 2;
			u->y_pos = TileY(tile) * TILE_SIZE + TILE_SIZE / 2;
			u->z_pos = GetSlopePixelZ(u->x_pos, u->y_pos);
			u->direction = DiagDirToDir(DiagDirection::NE);
			rvv->state = DiagDirToDiagTrackdir(DiagDirection::NE);
			rvv->frame = 0;
			UpdateVehicleTileHash(u, true);    // make sure it is not listed where it came from
			UpdateVehicleTileHash(u, false);   // back on the road network
		}
		InvalidateVehicleTickCaches();
		u->UpdateIsDrawn();
		u->Vehicle::UpdateViewport(true); // appears/disappears: mark the area dirty
	}
}

/**
 * Vehicle whose position represents this vehicle on the map: a carried road vehicle is where its
 * carrier is, so that "centre on vehicle" and the follow camera look at the carrier instead of at
 * the station the road vehicle was loaded at.
 */
const Vehicle *RVTransportGetFollowVehicle(const Vehicle *v)
{
	if (v == nullptr) return nullptr;
	if ((v->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) return v;

	const Vehicle *carrier = Vehicle::GetIfValid(v->transported_by);
	if (carrier == nullptr) return v;
	return carrier->GetMovingFront();
}

/**
 * Destroy the road vehicles carried by this carrier: they are lost together with it, exactly like
 * the wagons of a crashed train, instead of being left behind on the map.
 */
void RVTransportDestroyCarriedVehicles(Vehicle *carrier)
{
	if (carrier == nullptr) return;
	if (carrier->type == VehicleType::Road) return; // a road vehicle is never a carrier

	/* Collect first: deleting a vehicle modifies the pool, so it must not happen while iterating. */
	std::vector<VehicleID> fronts;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) continue;
		if (v->transported_by != carrier->index) continue;
		if (!v->IsFrontEngine()) continue;
		fronts.push_back(v->index);
	}

	for (const VehicleID id : fronts) {
		Vehicle *front = Vehicle::GetIfValid(id);
		if (front == nullptr) continue;

		/* Gather the whole (possibly articulated) vehicle, then delete it from the rear, as a part
		 * must never outlive the vehicle it is attached to. */
		std::vector<VehicleID> chain;
		for (Vehicle *u = front; u != nullptr; u = u->Next()) chain.push_back(u->index);

		for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
			Vehicle *u = Vehicle::GetIfValid(*it);
			if (u == nullptr) continue;
			/* Detach first, so that nothing refers to a vehicle which is about to disappear. */
			u->rv_transport_flags &= ~Vehicle::RV_TRANSPORT_CARRIED;
			u->transported_by = VehicleID::Invalid();
			u->transported_host_part = VehicleID::Invalid();
			u->transported_weight = 0;
			if (u->Previous() != nullptr) u->Previous()->SetNext(nullptr);
			delete u;
		}
	}
}
