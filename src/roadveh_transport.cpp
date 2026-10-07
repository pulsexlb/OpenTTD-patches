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
#include "company_base.h"
#include "console_func.h"
#include "economy_base.h"   // CargoPayment must be complete: the station stop of a carried vehicle deletes its payment
#include "engine_base.h"
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
#include "infrastructure_func.h"
#include "road_map.h"
#include "roadstop_base.h"
#include "pathfinder/yapf/yapf.h"
#include "tracerestrict.h"
#include "ground_vehicle.hpp"
#include "pbs.h"
#include "rail.h"
#include "roadveh.h"
#include "settings_type.h"
#include "station_base.h"
#include "station_map.h"
#include "tilearea_type.h"
#include "tile_map.h"
#include "train.h"
#include "tunnelbridge_map.h"
#include "vehicle_base.h"

#include <cstdarg>
#include <set>

#include "safeguards.h"

/**
 * Is this vehicle the unit which holds the vehicle transport state of its consist?
 *
 * For a road vehicle that is its front engine. For a train it is the primary vehicle: the engine
 * routes station arrival through TrainEnterStation(), which hands the consist information to
 * Primary(), and Vehicle::BeginLoading() - and with it RVTransportSetWaiting() - therefore runs on
 * the primary. After a no-swap couple/decouple the primary may sit mid-chain, so it is not enough
 * to test IsFrontEngine() (the physical chain head) when looking for a waiting or carried train.
 */
static bool RVTransportIsStateHolder(const Vehicle *v)
{
	if (v == nullptr) return false;
	if (v->type == VehicleType::Train) return v == Train::From(v)->Primary();
	return v->IsFrontEngine();
}

/** Weight of a road vehicle or train in tonnes (including its current cargo), from the consist weight cache. */
uint32_t RVTransportGetVehicleWeightTonnes(const Vehicle *rv)
{
	if (rv == nullptr) return 0;
	if (rv->type != VehicleType::Road && rv->type != VehicleType::Train) return 0;
	if (!RVTransportIsStateHolder(rv)) return 0;
	return rv->GetGroundVehicleCache()->cached_weight;
}

/** Is this the dedicated cargo of vehicle transport ("Vehicles (Road)" or "Vehicles (Train)")? */
bool RVTransportIsSpecialCargo(CargoType ct)
{
	return ct == RV_TRANSPORT_CARGO_SLOT || ct == RAIL_TRANSPORT_CARGO_SLOT;
}

/**
 * Cargo label of the dedicated "Vehicles" cargo which vehicle transporting NewGRFs use for the cargo
 * of their car ferries / car carriers (see RVTransportCarrierParts::BulkOversizedOrVehicles).
 * This is the built-in cargo's own label, see CT_VEHICLES.
 */
static constexpr CargoLabel RV_TRANSPORT_VEHICLES_CARGO_LABEL = CT_VEHICLES;

/** Cargo label of the dedicated "Vehicles (Train)" cargo (see CT_RAILVEHICLES). */
static constexpr CargoLabel RV_TRANSPORT_RAIL_CARGO_LABEL = CT_RAILVEHICLES;

/** Is this carrier part able to carry road vehicles? */
bool RVTransportPartCanCarry(const Vehicle *part)
{
	if (part == nullptr) return false;
	/* Master switch (vehicle.rv_transport_enabled): with it off nothing new is loaded, while unloading
	 * keeps working so that no vehicle stays on board forever. */
	if (!_settings_game.vehicle.rv_transport_enabled) return false;
	if (part->cargo_cap == 0) return false;
	if (!IsValidCargoType(part->cargo_type)) return false;

	/* An aircraft never carries trains: the dedicated "Vehicles (Train)" cargo is a ship hold's
	 * property, and no aircraft gets it through its refit mask. This guard also keeps a NewGRF
	 * which puts the rail cargo (or a cargo with its label) on an aircraft from loading trains. */
	if (part->type == VehicleType::Aircraft &&
			(part->cargo_type == RAIL_TRANSPORT_CARGO_SLOT ||
			 CargoSpec::Get(part->cargo_type)->label == RV_TRANSPORT_RAIL_CARGO_LABEL)) {
		return false;
	}

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
			const CargoLabel label = CargoSpec::Get(part->cargo_type)->label;
			return label == RV_TRANSPORT_VEHICLES_CARGO_LABEL || label == RV_TRANSPORT_RAIL_CARGO_LABEL;
		}
	}

	return false;
}

bool RVTransportEngineMayBeRefitToVehicles(const Engine *e)
{
	if (e == nullptr) return false;
	/* Same master switch as RVTransportPartCanCarry(): with it off nothing becomes a carrier. */
	if (!_settings_game.vehicle.rv_transport_enabled) return false;

	/* The default-cargo criterion below is a wagon rule: it keeps a coach or a mail van from
	 * gaining the dedicated cargo. Ships and aircraft have no such distinction - turning a cargo
	 * ship or a passenger plane into a vehicle carrier is the point of the refit - so they are
	 * always allowed (the master switch is the only gate). */
	if (e->type != VehicleType::Train) return true;

	/* Judge by the engine's default cargo: that is what the wagon natively carries, the same
	 * criterion the loading-time check applies to a part's cargo. A wagon already refitted to the
	 * dedicated transport cargo qualifies through it as well, since that cargo is 'Oversized'. */
	const CargoSpec *cs = (IsValidCargoType(e->info.cargo_type)) ? CargoSpec::Get(e->info.cargo_type) : nullptr;
	if (cs == nullptr || !cs->IsValid()) return false;

	switch (static_cast<RVTransportCarrierParts>(_settings_game.vehicle.rv_transport_carrier_parts)) {
		case RVTransportCarrierParts::AnyPart:
			return true;

		case RVTransportCarrierParts::OversizedOnly:
			return cs->classes.Test(CargoClass::Oversized);

		case RVTransportCarrierParts::BulkOversizedOrVehicles:
			if (cs->classes.Test(CargoClass::Bulk)) return true;
			if (cs->classes.Test(CargoClass::Oversized)) return true;
			return cs->label == RV_TRANSPORT_VEHICLES_CARGO_LABEL || cs->label == RV_TRANSPORT_RAIL_CARGO_LABEL;
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

/** Tonnes of road vehicles this carrier part holds. */
uint32_t RVTransportGetPartCarriedTonnes(const Vehicle *part)
{
	if (part == nullptr) return 0;
	uint32_t carried = 0;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & RVTF_TRANSPORTED) == 0) continue;
		if (v->transported_host_part != part->index) continue;
		carried += v->transported_weight;
	}
	return carried;
}

/** Tonnes already used on this carrier part: the road vehicles on it plus the cargo it carries. */
uint32_t RVTransportGetPartUsedTonnes(const Vehicle *part)
{
	if (part == nullptr) return 0;
	uint32_t used = RVTransportGetPartCarriedTonnes(part);
	if (IsValidCargoType(part->cargo_type)) {
		used += static_cast<uint32_t>(CargoSpec::Get(part->cargo_type)->WeightOfNUnits(part->cargo.StoredCount()));
	}
	return used;
}

/** Cargo units to display for this carrier part (see RVTransportGetPartCargoAmount()). */
uint16_t RVTransportGetPartCargoAmount(const Vehicle *part)
{
	if (part == nullptr) return 0;

	const uint32_t stored = part->cargo.StoredCount();
	const uint32_t carried = RVTransportGetPartCarriedTonnes(part);
	if (carried == 0 || !IsValidCargoType(part->cargo_type)) return static_cast<uint16_t>(stored);

	const CargoSpec *cs = CargoSpec::Get(part->cargo_type);
	if (cs->weight == 0) return static_cast<uint16_t>(stored);

	/* One unit weighs CargoSpec::weight / 16 tonnes, the inverse of RVTransportGetPartCapacityTonnes():
	 * a part's road vehicle capacity and the amount it reports are expressed in the same tonnes. */
	const uint32_t amount = stored + carried * 16 / cs->weight;
	return static_cast<uint16_t>(std::min<uint32_t>(amount, part->cargo_cap));
}

/** Collect the road vehicles this carrier part holds (front vehicles only, in vehicle id order). */
void RVTransportGetPartCarriedVehicles(const Vehicle *part, std::vector<const Vehicle *> &out)
{
	out.clear();
	if (part == nullptr) return;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) continue;
		if (v->transported_host_part != part->index) continue;
		if (!RVTransportIsStateHolder(v)) continue;
		out.push_back(v);
	}
}

/** Carrier this road vehicle is on, from the part it occupies (see the header). */
Vehicle *RVTransportGetCarrier(const Vehicle *rv)
{
	if (rv == nullptr || (rv->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) return nullptr;
	const Vehicle *part = Vehicle::GetIfValid(rv->transported_host_part);
	return (part != nullptr) ? part->First() : nullptr;
}

/**
 * Is this road vehicle on this carrier? Both sides are reduced to the front of their chain, so a
 * caller may pass the chain head or any vehicle of the carrier (the details window can be opened on
 * a wagon as well) and gets the same answer.
 */
static bool RVTransportIsOnCarrier(const Vehicle *carrier, const Vehicle *rv)
{
	if (carrier == nullptr) return false;
	const Vehicle *rv_carrier = RVTransportGetCarrier(rv);
	return rv_carrier != nullptr && rv_carrier == carrier->First();
}

/** Number of road vehicles currently carried by this carrier. */
uint32_t RVTransportCountOnCarrier(const Vehicle *carrier)
{
	if (carrier == nullptr) return 0;
	uint32_t count = 0;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & RVTF_TRANSPORTED) == 0) continue;
		if (!RVTransportIsStateHolder(v)) continue;   // one count per vehicle
		if (!RVTransportIsOnCarrier(carrier, v)) continue;
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
		if (!RVTransportIsStateHolder(v)) continue;
		if (!RVTransportIsOnCarrier(carrier, v)) continue;
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
		if (!RVTransportIsStateHolder(v)) continue;
		if (!RVTransportIsOnCarrier(carrier, v)) continue;
		out.push_back(v);
	}
}

/** Weight in tonnes of the vehicles this carrier holds. */
uint32_t RVTransportGetCarriedWeightTonnes(const Vehicle *carrier)
{
	if (carrier == nullptr) return 0;
	uint32_t weight = 0;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) continue;
		if (!RVTransportIsOnCarrier(carrier, v)) continue;
		/* Every member carries the weight it occupies on its host part: a road vehicle the whole
		 * weight on its front, a train the weight of its carriage on each carriage's first
		 * vehicle (the rest of the chain records none), so summing every member gives the total. */
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
		if (!RVTransportIsStateHolder(v)) continue;
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
	if (!IsValidCargoType(RV_TRANSPORT_CARGO_SLOT)) return false;

	bool has_cargo_part = false;
	for (const Vehicle *u = v->First(); u != nullptr; u = u->Next()) {
		if (u->cargo_cap == 0) continue;
		has_cargo_part = true;
		if (!RVTransportIsSpecialCargo(u->cargo_type)) return false;
	}
	return has_cargo_part;
}

/**
 * Set or clear the "stopped" state of the whole consist a waiting vehicle belongs to. A waiting
 * train is halted as a whole, the way the engine stops a consist (TrainEnterStation() and the
 * coupling code do the same), so that no part of it keeps rolling while it waits to be loaded.
 */
static void RVTransportSetChainStopped(Vehicle *v, bool stopped)
{
	for (Vehicle *u = v->First(); u != nullptr; u = u->Next()) {
		if (stopped) {
			u->vehstatus.Set(VehState::Stopped);
			u->cur_speed = 0;
		} else {
			u->vehstatus.Reset(VehState::Stopped);
		}
	}
}

/**
 * Clear the reservations of the platform tiles around a train's head which no vehicle stands on.
 * The engine frees the look-ahead ahead of a train only up to the first tile, and platform tiles
 * are never freed when a vehicle leaves them, so a train stopped mid-platform keeps reservations
 * over the rest of the platform unless they are cleared here.
 */
static void RVTransportClearNearbyPlatformReservations(Train *tr)
{
	const TileIndex head_tile = tr->First()->tile;
	if (!IsRailStationTile(head_tile)) return;
	const TileIndexDiff delta = TileOffsByAxis(GetRailStationAxis(head_tile));
	for (int pass = 0; pass < 2; pass++) {
		const TileIndexDiff step = (pass == 0) ? delta : -delta;
		TileIndex pt = head_tile;
		while (IsValidTile(pt + step) && IsCompatibleTrainStationTile(pt + step, pt)) {
			pt += step;
			TrackBits reserved = GetReservedTrackbits(pt);
			if (reserved == TRACK_BIT_NONE) continue;
			if (GetFirstVehicleOnTile(pt, VehicleType::Train) != nullptr) continue; // a train stands there
			for (Track t = TRACK_BEGIN; t < TRACK_END; t++) {
				if ((reserved & TrackToTrackBits(t)) == TRACK_BIT_NONE) continue;
				UnreserveRailTrack(pt, t);
			}
		}
	}
}

/** Set or clear the "waiting to be transported" state; a waiting vehicle is stopped. */
void RVTransportSetWaiting(Vehicle *rv, bool waiting)
{
	if (rv == nullptr || (rv->type != VehicleType::Road && rv->type != VehicleType::Train)) return;
	/* A train's transport state lives on its primary vehicle (see RVTransportIsStateHolder). */
	if (rv->type == VehicleType::Train) rv = Train::From(rv)->Primary();
	if ((rv->rv_transport_flags & RVTF_TRANSPORTED) != 0) return; // carried vehicles are not waiting

	if (waiting) {
		rv->rv_transport_flags |= RVTF_WAITING;
		rv->transport_wait_tick = static_cast<uint32_t>(_tick_counter);
		rv->rv_transport_flags &= ~RVTF_UNLOAD_WARNED;   // a new trip, so the warning may be shown again
		if (rv->type == VehicleType::Train) {
			Train *tr = Train::From(rv);
			/* Drop the remaining path reservation: a waiting train never departs by itself, so its
			 * look-ahead would block the rest of the platform forever. The train keeps blocking the
			 * tiles it stands on by being there, like any stopped train. */
			if (tr->IsPrimaryVehicle()) FreeTrainTrackReservation(tr);
			RVTransportClearNearbyPlatformReservations(tr);
			RVTransportSetChainStopped(rv, true);
		} else {
			rv->vehstatus.Set(VehState::Stopped);
			rv->cur_speed = 0;
		}
	} else {
		rv->rv_transport_flags &= ~RVTF_WAITING;
		rv->transport_wait_tick = 0;
		if (rv->type == VehicleType::Train) {
			RVTransportSetChainStopped(rv, false);
		} else {
			rv->vehstatus.Reset(VehState::Stopped);
		}
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
	if (rv == nullptr || (rv->type != VehicleType::Road && rv->type != VehicleType::Train)) return;
	/* A train's transport state lives on its primary vehicle (see RVTransportIsStateHolder). */
	if (rv->type == VehicleType::Train) rv = Train::From(rv)->Primary();
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
	if ((order->IsType(OT_GOTO_STATION) || order->IsType(OT_LOADING)) && (order->GetRVTransportFlags() & ORVTF_OWN_WAIT) != 0) return;

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
	if (v == nullptr || (v->type != VehicleType::Road && v->type != VehicleType::Train) || !RVTransportIsStateHolder(v)) return;
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
	if (rv->type != VehicleType::Road && rv->type != VehicleType::Train) return false;
	if (!RVTransportIsStateHolder(rv)) return false;     // carriers take a whole vehicle, never a lone part
	if ((rv->rv_transport_flags & RVTF_TRANSPORTED) != 0) return false;
	if (part->First() != carrier) return false;          // part must belong to this carrier
	if (!force && !RVTransportPartCanCarry(part)) return false;

	/* Carrying a vehicle of another company is infrastructure sharing of a sort, and is off by
	 * default: a carrier only takes the vehicles of its own company. */
	if (!force && rv->owner != carrier->owner && !_settings_game.economy.infrastructure_sharing_rv) return false;

	/* A carried road vehicle is a whole vehicle: cached_weight covers every part. */
	uint32_t weight = RVTransportGetVehicleWeightTonnes(rv);
	if (weight == 0) weight = 1;

	/* A train is loaded carriage by carriage: every carriage only has to fit in one carrier part
	 * ("hold") on its own, the consist as a whole may span several holds. The carriages keep their
	 * order and are filled into the parts from `part` onwards. Each carriage remembers the part it
	 * went on and its weight, which its first vehicle records below so that the per-part weight
	 * accounting (RVTransportGetPartCarriedTonnes) sees exactly what is on each hold. */
	std::vector<Vehicle *> carriage_hosts;                 // host part per chain vehicle (train only)
	std::vector<uint32_t> carriage_weights;                // weight each member records (train only)
	if (rv->type == VehicleType::Train && !force) {
		/* Weight of each carriage: an articulated unit counts once, on its first part. */
		std::vector<std::pair<Vehicle *, uint32_t>> carriages;
		for (Vehicle *u = rv->First(); u != nullptr; u = u->Next()) {
			const uint32_t w = Train::From(u)->GetSelfWeight();
			if (u->IsArticulatedPart() && !carriages.empty()) {
				carriages.back().second += w;
				continue;
			}
			carriages.emplace_back(u, w);
		}

		carriage_hosts.clear();
		carriage_weights.clear();
		Vehicle *part_it = part;
		size_t ci = 0;
		Vehicle *current_host = part;
		for (Vehicle *u = rv->First(); u != nullptr; u = u->Next()) {
			uint32_t member_weight = 0;
			if (ci < carriages.size() && carriages[ci].first == u) {
				member_weight = carriages[ci].second;
				/* Fill the parts onwards, in order, with whole carriages: find the first hold with
				 * room for this carriage on its own. */
				while (part_it != nullptr && (!RVTransportPartCanCarry(part_it)
						|| RVTransportGetPartUsedTonnes(part_it) + carriages[ci].second > RVTransportGetPartCapacityTonnes(part_it))) {
					part_it = part_it->Next();
				}
				if (part_it == nullptr) {
					/* Some carriage found no hold with room for it: nothing is loaded. */
					return false;
				}
				current_host = part_it;
				ci++;
			}
			carriage_hosts.push_back(current_host);
			carriage_weights.push_back(member_weight);
		}
		{
			uint32_t total = 0;
			unsigned holds = 0;
			const Vehicle *prev = nullptr;
			for (size_t i = 0; i < carriage_hosts.size(); i++) {
				total += carriage_weights[i];
				if (carriage_hosts[i] != prev) holds++;
				prev = carriage_hosts[i];
			}
		}
	} else if (!force) {
		const uint32_t capacity = RVTransportGetPartCapacityTonnes(part);
		const uint32_t used = RVTransportGetPartUsedTonnes(part);
		if (used + weight > capacity) return false;      // refused: no room on this part
	}

	/* A waiting train holds PBS reservations: the tiles it stands on, and the look-ahead
	 * reservation chain which can reach several tiles ahead of its front. All of them must go, or
	 * the tiles stay reserved forever for a train which is no longer there. */
	if (rv->type == VehicleType::Train) {
		Train *tr = Train::From(rv);
		if (tr->IsPrimaryVehicle()) FreeTrainTrackReservation(tr);
		for (Vehicle *u = rv->First(); u != nullptr; u = u->Next()) {
			TrackBits reserved = GetReservedTrackbits(u->tile);
			for (Track t = TRACK_BEGIN; t < TRACK_END; t++) {
				if ((reserved & TrackToTrackBits(t)) == TRACK_BIT_NONE) continue;
				UnreserveRailTrack(u->tile, t);
			}
		}

		/* The arrival path reservation of a waiting train covers the whole platform it stopped on,
		 * and the chain walk above only reaches the tiles ahead of the front. Clear the remaining
		 * reservations of that platform as well: every reserved tile of a platform our train
		 * occupies is part of its own arrival path (PBS never grants a second reservation onto a
		 * platform a train is standing on), so nothing of another train can be lost here. Tiles
		 * with a vehicle on them are skipped - those are the train's own parts, handled above. */
		const TileIndex head_tile = Train::From(rv)->First()->tile;
		if (IsRailStationTile(head_tile)) {
			const TileIndexDiff delta = TileOffsByAxis(GetRailStationAxis(head_tile));
			for (int pass = 0; pass < 2; pass++) {
				const TileIndexDiff step = (pass == 0) ? delta : -delta;
				TileIndex pt = head_tile;
				while (IsValidTile(pt + step) && IsCompatibleTrainStationTile(pt + step, pt)) {
					pt += step;
					TrackBits reserved = GetReservedTrackbits(pt);
					if (reserved == TRACK_BIT_NONE) continue;
					if (GetFirstVehicleOnTile(pt, VehicleType::Train) != nullptr) continue; // a train (ours or another) stands there
					for (Track t = TRACK_BEGIN; t < TRACK_END; t++) {
						if ((reserved & TrackToTrackBits(t)) == TRACK_BIT_NONE) continue;
						UnreserveRailTrack(pt, t);
					}
				}
			}
		}
	}

	/* The road vehicle leaves the road stop it was loaded at: free a parking bay while the vehicle
	 * state still holds the bay number (a drive-through stop is handled after the road network
	 * removal below, when the vehicle is no longer part of the stop's occupancy). */
	if (rv->type == VehicleType::Road && IsBayRoadStopTile(rv->tile)) RVTransportReleaseRoadStop(rv);

	/* Hide the whole vehicle: every part of an articulated vehicle is drawn and hashed on its
	 * own, and in a bend the parts are not even on the same tile. */
	rv->rv_transport_flags &= ~RVTF_WAITING;
	{
		/* Weight each chain member records on its host part: a train non-force load puts the
		 * weight of every carriage on its first vehicle (the hold accounting sums these per
		 * part); everything else keeps the whole weight on the state holder. */
		size_t i = 0;
		for (Vehicle *u = rv->First(); u != nullptr; u = u->Next(), i++) {
			const bool distributed = !carriage_hosts.empty();
			const Vehicle *host = distributed ? carriage_hosts[i] : part;
			u->rv_transport_flags |= RVTF_TRANSPORTED;
			u->transported_by = carrier->index;
			u->transported_host_part = host->index;
			/* The station it is loaded at: the vehicle transport fee is charged on the direct distance
			 * from there to the station it is put down at again. A waiting vehicle is always at the
			 * station it waits at. */
			u->transported_from = rv->last_station_visited;
			/* Weight on the host part: a distributed train puts every carriage's weight on its first
			 * vehicle; everything else keeps the whole weight on the state holder. */
			u->transported_weight = 0;
			if (distributed && carriage_weights[i] != 0) {
				u->transported_weight = static_cast<uint16_t>(std::min<uint32_t>(carriage_weights[i], UINT16_MAX));
			} else if (u == rv) {
				u->transported_weight = static_cast<uint16_t>(std::min<uint32_t>(weight, UINT16_MAX));
			}

			u->vehstatus.Set(VehState::Stopped);
			u->vehstatus.Set(VehState::Hidden);
			u->cur_speed = 0;
			if (u->type == VehicleType::Road) RoadVehicle::From(u)->state = DiagDirToDiagTrackdir(DirToDiagDir(u->direction));
			UpdateVehicleTileHash(u, true);   // off the road network (like virtual vehicles)
			InvalidateVehicleTickCaches();
			u->UpdateIsDrawn();
			u->Vehicle::UpdateViewport(true); // appears/disappears: mark the area dirty
		}
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
	if (rv->type == VehicleType::Train) {
		const Train *tr = Train::From(rv);
		fprintf(stderr, "[taildbg] attach t#%u onto carrier#%u BEFORE advance: num=%u impl=%u real=%u cur_dest=%u\n",
			tr->index.base(), carrier->index.base(), tr->GetNumOrders(),
			tr->cur_implicit_order_index, tr->cur_real_order_index, tr->current_order.GetDestination().ToStationID().base());
		for (VehicleOrderID i = 0; i < tr->GetNumOrders(); i++) {
			const Order *o = tr->GetOrder(i);
			if (o == nullptr) { fprintf(stderr, "[taildbg]     order %u: null\n", i); continue; }
			fprintf(stderr, "[taildbg]     order %u: type=%d dest=%u rvflags=%u unload=%d\n",
				i, (int)o->GetType(), o->GetDestination().ToStationID().base(), (uint)o->GetRVTransportFlags(), (int)o->GetUnloadType());
		}
	}
	RVTransportAdvanceCarriedVehicleOrder(rv);
	if (rv->type == VehicleType::Train) {
		const Train *tr = Train::From(rv);
		fprintf(stderr, "[taildbg] attach t#%u AFTER advance: impl=%u real=%u cur_dest=%u cur_type=%d\n",
			tr->index.base(), tr->cur_implicit_order_index, tr->cur_real_order_index,
			tr->current_order.GetDestination().ToStationID().base(), (int)tr->current_order.GetType());
	}

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
 * A road stop of a station where a road vehicle could be put back on the road, plus the road tile it
 * would end up on and the direction it would be travelling in when it gets there.
 */
struct RVTransportRoadStopCandidate {
	TileIndex tile;             ///< road stop tile to put the vehicle on
	DiagDirection dd;           ///< direction from that tile to the road outside the station
	TileIndex exit_tile;        ///< the road tile the vehicle drives on to after leaving the station
};

/**
 * Collect every road stop of this station where the given road vehicle can be put back on the road.
 *
 * The tile does not have to be empty: a drive-through stop has room per entry direction, so a tile
 * which is occupied on one side can still take a vehicle on the other side. Whole-tile emptiness is
 * only required for bay stops (whose bays are counted by the stop itself, but where a second vehicle
 * on the same tile would overlap).
 *
 * The candidates come in a fixed order (area order, then NE/SE/SW/NW per tile), which is used as the
 * tie-break when several of them turn out to be equally good.
 * @param st         station to look at
 * @param rv         road vehicle to put down
 * @param candidates [out] the candidates, in the order they were found
 */
void CollectFreeRoadStopTiles(const Station *st, Vehicle *rv, std::vector<RVTransportRoadStopCandidate> &candidates)
{
	const RoadStopType wanted = RoadVehicle::From(rv)->IsBus() ? RoadStopType::Bus : RoadStopType::Truck;

	for (int pass = 0; pass < 2; pass++) {
		const TileArea &area = (pass == 0) ? st->bus_station : st->truck_station;
		for (TileIndex t : area) {
			if (!IsAnyRoadStopTile(t)) continue;
			/* Only the road stops of the vehicle's own type: a bus needs a bus stop and a truck
			 * needs a truck stop, like Station::GetPrimaryRoadStop() and CanVehicleUseStation()
			 * do. RoadStop::Enter() only refuses busy, full and articulated vehicles, so without
			 * this a bus would be put into the truck stop, where it can never drive out again. */
			if (GetRoadStopType(t) != wanted) continue;
			RoadStop *rs = RoadStop::GetByTile(t, wanted);
			if (rs == nullptr) continue;

			for (DiagDirection dd = DiagDirection::Begin; dd < DiagDirection::End; dd++) {
				TileIndex next = TileAddByDiagDir(t, dd);
				if (!IsValidTile(next)) continue;
				if (!IsNormalRoadTile(next) && !IsAnyRoadStopTile(next)) continue;
				/* The vehicle has to be able to drive on the road it leaves the station by, otherwise
				 * it would be put down on a stop it can never leave. */
				if (!HasTileAnyRoadType(next, RoadVehicle::From(rv)->compatible_roadtypes)) continue;

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

				candidates.push_back({ t, dd, next });
			}
		}
	}
}

/** Node budget for one road-stop probe; bounds the cost of choosing where to put a vehicle down. */
static const int RVTRANSPORT_PROBE_MAX_NODES = 10000;

/** The leg a road vehicle drives after being put down at a station, as far as its orders say. */
struct RVTransportNextLeg {
	StationID station = StationID::Invalid();      ///< station to drive to, invalid when there is none
	DiagDirection travel_direction = DiagDirection::Invalid; ///< direction to arrive with, if the order asks for one
};

/**
 * The station a road vehicle drives to once it has been put down at \a drop_st, and the direction it
 * has to arrive with.
 *
 * The vehicle is executing its "be unloaded here" order while it is being put down, so the leg that
 * matters starts *after* that order. As elsewhere in this file the order list indices cannot be
 * trusted (a carried vehicle never runs ProcessOrders(), so they drift away from the order it is
 * really on), so the order being executed is located by what it is rather than by index.
 * @param rv      the road vehicle
 * @param drop_st the station it is being put down at
 * @return the next leg; \a station is invalid when the vehicle has nowhere to go afterwards
 */
static RVTransportNextLeg RVTransportGetNextLeg(const Vehicle *rv, StationID drop_st)
{
	RVTransportNextLeg leg;
	if (rv == nullptr) return leg;

	/* Orders live on the primary vehicle of the chain, which is not necessarily the chain head
	 * (a train with its only locomotive at the rear, for instance). */
	const Vehicle *pv = rv->Primary();
	const VehicleOrderID num_orders = pv->GetNumOrders();
	if (num_orders == 0) return leg;

	/* Find the order the vehicle is on right now: the "be unloaded here" order for this station. */
	VehicleOrderID start = 0;
	bool found_current = false;
	for (VehicleOrderID i = 0; i < num_orders; i++) {
		const Order *o = pv->GetOrder(i);
		if (o == nullptr || !o->IsType(OT_GOTO_STATION)) continue;
		if (o->GetDestination().ToStationID() != drop_st) continue;
		if ((o->GetRVTransportFlags() & ORVTF_OWN_UNLOAD) == 0) continue;
		start = i + 1;
		found_current = true;
		break;
	}
	if (!found_current) {
		/* No matching order (an edited schedule, for instance): fall back to the index the vehicle
		 * is on, even though it can lag behind. */
		start = (pv->cur_implicit_order_index < num_orders) ? pv->cur_implicit_order_index + 1 : 0;
	}

	for (VehicleOrderID i = 0; i < num_orders; i++) {
		const Order *o = pv->GetOrder(static_cast<VehicleOrderID>((start + i) % num_orders));
		if (o == nullptr) continue;
		if (!o->IsType(OT_GOTO_STATION) && !o->IsType(OT_GOTO_WAYPOINT)) continue;
		leg.station = o->GetDestination().ToStationID();
		leg.travel_direction = o->GetRoadVehTravelDirection();
		return leg;
	}
	return leg;
}

/**
 * Find the road stop of this station where the given road vehicle should be put back on the road.
 *
 * When the vehicle has a station to drive to after this one, every candidate is scored by asking the
 * road pathfinder how expensive it would be to get to that station from the road the vehicle would
 * leave by, and the cheapest one wins. A candidate from which the next station cannot be reached at
 * all is never chosen over one that can. Candidates that score equally keep the order in which they
 * were found, so a station with a single stop behaves exactly as before.
 * @param st      station to put the vehicle down at
 * @param rv      road vehicle to put down
 * @param out_tile [out] road stop tile to use
 * @param out_dd   [out] direction from that tile to the road outside the station
 * @return whether a road stop was found
 */
bool FindFreeRoadStopTile(const Station *st, Vehicle *rv, TileIndex &out_tile, DiagDirection &out_dd)
{
	std::vector<RVTransportRoadStopCandidate> candidates;
	CollectFreeRoadStopTiles(st, rv, candidates);
	if (candidates.empty()) return false;

	const RVTransportNextLeg leg = RVTransportGetNextLeg(rv, st->index);

	size_t best = 0;
	if (leg.station != StationID::Invalid()) {
		const RoadVehicle *road_rv = RoadVehicle::From(rv);
		int best_cost = 0;
		bool found_reachable = false;
		for (size_t i = 0; i < candidates.size(); i++) {
			/* The pathfinder wants the direction the vehicle is *travelling* in on that road tile (this
			 * is what the road vehicle controller passes as 'enterdir'), which is the direction it
			 * leaves the station in - the very direction of the candidate. A bay reverses out of the
			 * station, but still travels away from it. */
			const DiagDirection enterdir = candidates[i].dd;
			int cost = 0;
			const bool reachable = YapfRoadVehicleProbeToStation(road_rv, candidates[i].exit_tile, enterdir,
					leg.station, leg.travel_direction, RVTRANSPORT_PROBE_MAX_NODES, cost);
			/* A candidate the next station can be reached from always beats one it cannot, and only
			 * among those does the cost decide. Ties keep the order the candidates were found in. */
			if (!reachable) continue;
			if (!found_reachable || cost < best_cost) {
				found_reachable = true;
				best_cost = cost;
				best = i;
			}
		}
		/* If no candidate could reach the next station, the first one is used anyway: this station is
		 * where the vehicle asked to be put down, and the pathfinder sorts out the route from there. */
	}

	out_tile = candidates[best].tile;
	out_dd = candidates[best].dd;
	return true;
}

/* ---- Train detach: the ship counterpart of the road stop machinery above. ---- */

/**
 * A platform of this station's rail station where a train could be put back on the rails, plus the
 * direction the train would face (and leave in) when it is put on the platform end.
 */
struct RVTransportRailCandidate {
	TileIndex exit_end;         ///< the last platform tile towards the exit
	DiagDirection dir;          ///< direction the train faces (its direction of travel when leaving)
	uint platform_tiles;        ///< length of the platform, in tiles
	uint platform_idx = 0;      ///< index of the platform this end belongs to (two ends share one)
	bool reachable = false;     ///< the train leaving here (forward, or reversed off the other end) can reach the next station
	bool fwd_reachable = false; ///< a train leaving *this* end forward can reach the next station
	int dist = INT_MAX;         ///< direct distance from the exit end to the next station
	int score = 0;              ///< ranking score: reachable, or facing the target when none is reachable
};

/** Is this rail type usable by the given train (one the train can run on)? */
static bool RVTransportRailTypeCompatible(const Train *tr, RailType rt)
{
	/* The train must not only be *allowed* on this rail type but also able to drive away from
	 * the platform by itself, so require power, the same criterion the engine uses for moving:
	 * a merely compatible rail type (e.g. declared compatible by a rail NewGRF) would leave the
	 * consist stranded on the platform. */
	return HasPowerOnRail(tr->railtypes, rt);
}

/**
 * Collect every platform of this station's rail station where the given train can be put back on the
 * rails: the rail type must be compatible with the train, the platform must be at least as long as
 * the train, the tiles the train would occupy must be free of vehicles and of other trains' PBS
 * reservations, and the platform end it would leave by must lead onto usable rail. The candidates
 * come in a fixed order (area order, then the platform ends), which is used as the tie-break when
 * several of them turn out to be equally good.
 * @param st         station to look at
 * @param tr         train to put down
 * @param candidates [out] the candidates, in the order they were found
 */
static void CollectFreeRailPlatformTiles(const Station *st, const Train *tr, std::vector<RVTransportRailCandidate> &candidates)
{
	const uint tiles_needed = CeilDiv(tr->gcache.cached_total_length, TILE_SIZE);
	std::set<TileIndex> seen;


	for (TileIndex t : st->train_station) {
		if (!st->TileBelongsToRailStation(t)) continue;

		const Axis axis = GetRailStationAxis(t);
		const TileIndexDiff delta = TileOffsByAxis(axis);

		/* Walk to the start of this platform (the far end against the axis direction). */
		TileIndex start = t;
		while (IsValidTile(start - delta) && IsCompatibleTrainStationTile(start - delta, start)) start -= delta;

		/* Walk to its end. */
		TileIndex end = start;
		while (IsValidTile(end + delta) && IsCompatibleTrainStationTile(end + delta, end)) end += delta;

		/* Every tile of one platform walks to the same start; process each platform once. */
		if (!seen.insert(start).second) continue;

		const uint platform_len = static_cast<uint>(std::abs(static_cast<int>(end.base()) - static_cast<int>(start.base())) / std::abs(delta)) + 1;
		if (platform_len < tiles_needed) {
			continue;
		}

		/* One candidate per platform end, described by the end the train leaves by. */
		for (DiagDirection dd = DiagDirection::Begin; dd < DiagDirection::End; dd++) {
			const TileIndexDiffC off = TileIndexDiffCByDiagDir(dd);
			const bool along_axis = (axis == Axis::X) ? (off.x != 0) : (off.y != 0);
			if (!along_axis) continue;
			const bool towards_end = (axis == Axis::X) ? (off.x > 0) : (off.y > 0);
			const TileIndex exit_end = towards_end ? end : start;

			/* The tiles the train would occupy, walking backwards from the exit end - backwards
			 * means against the exit direction, which for the far end of the platform is the axis
			 * direction and for the near end against it. */
			const TileIndexDiff back_step = -TileOffsByDiagDir(dd);
			bool free = true;
			TileIndex pt = exit_end;
			for (uint i = 0; i < tiles_needed && free; i++) {
				if (!st->TileBelongsToRailStation(pt)) {
					free = false;
					break;
				}
				if (!RVTransportRailTypeCompatible(tr, GetRailType(pt))) {
					free = false;
					break;
				}
				if (GetReservedTrackbits(pt) != TRACK_BIT_NONE) {
					/* A reservation nobody owns is an orphan: its chain was broken (e.g. another
					 * train was loaded onto a carrier from this platform earlier), so no engine will
					 * ever free it. Clear it and use the tile; a reservation owned by a train still
					 * blocks the platform as before. */
					bool orphan = true;
					Train *res_owner = nullptr;
					for (Track t = TRACK_BEGIN; t < TRACK_END; t++) {
						if ((GetReservedTrackbits(pt) & TrackToTrackBits(t)) == TRACK_BIT_NONE) continue;
						res_owner = GetTrainForReservation(pt, t);
						if (res_owner != nullptr) {
							orphan = false;
							break;
						}
					}
					if (orphan) {
						for (Track t = TRACK_BEGIN; t < TRACK_END; t++) {
							if ((GetReservedTrackbits(pt) & TrackToTrackBits(t)) == TRACK_BIT_NONE) continue;
							UnreserveRailTrack(pt, t);
						}
					} else {
						free = false;
						break;
					}
				}
				if (GetFirstVehicleOnTile(pt, VehicleType::Train) != nullptr) {
					free = false;
					break;
				}
				if (free && i + 1 < tiles_needed) {
					if (!IsValidTile(pt + back_step) || !IsCompatibleTrainStationTile(pt + back_step, pt)) {
						free = false;
						break;
					}
				}
				pt += back_step;
			}
			if (!free) continue;

			candidates.push_back({exit_end, dd, platform_len});
		}
	}
}

/**
 * Rank this station's platforms where the given train can be put back on the rails, best first.
 *
 * Candidates are ranked for placing a dropped train: the engine's own pathfinder decides, per exit
 * end, whether a train leaving there can actually drive on to the station it heads to next (this
 * follows the real track connectivity rules, including curve tiles). Reachable candidates outrank
 * unreachable ones, and the direct distance to the next station breaks ties. When no candidate can
 * reach the next station, the exit end facing towards it ranks first, so the train is at least laid
 * out looking the right way and can reverse or re-path from there. The caller tries the ranked
 * candidates in order, so a placement blocked by another train's reservation falls back to the
 * next-best platform instead of giving up.
 * @param st      station to put the train down at
 * @param tr      train to put down
 * @param ranked [out] the viable candidates, best first; empty when no platform fits
 */
static void FindFreeRailPlatformCandidates(const Station *st, const Train *tr, std::vector<RVTransportRailCandidate> &ranked)
{
	std::vector<RVTransportRailCandidate> candidates;
	CollectFreeRailPlatformTiles(st, tr, candidates);
	if (candidates.empty()) {
		return;
	}

	const RVTransportNextLeg leg = RVTransportGetNextLeg(tr, st->index);
	const Station *target = (leg.station != StationID::Invalid()) ? Station::GetIfValid(leg.station) : nullptr;
	const TileIndex target_xy = (target != nullptr) ? target->xy : INVALID_TILE;
	fprintf(stderr, "[taildbg] st#%u t#%u: front_can_lead=%d last_can_lead=%d len=%u leg_st=%u target=(%d,%d)\n",
		st->index.base(), tr->index.base(),
		tr->CanLeadTrain() ? 1 : 0, tr->Last()->CanLeadTrain() ? 1 : 0,
		tr->gcache.cached_total_length, (uint)leg.station.base(),
		(target_xy != INVALID_TILE) ? (int)TileX(target_xy) : -1,
		(target_xy != INVALID_TILE) ? (int)TileY(target_xy) : -1);
	fprintf(stderr, "[taildbg]   orders of st#%u t#%u (first v#%u, primary v#%u): num=%u impl=%u impl2=%u cur=%u\n",
		st->index.base(), tr->index.base(), tr->First()->index.base(), tr->Primary()->index.base(),
		tr->Primary()->GetNumOrders(), tr->Primary()->cur_implicit_order_index, tr->Primary()->cur_real_order_index,
		tr->Primary()->current_order.GetDestination().ToStationID().base());
	for (VehicleOrderID i = 0; i < tr->Primary()->GetNumOrders(); i++) {
		const Order *o = tr->Primary()->GetOrder(i);
		if (o == nullptr) { fprintf(stderr, "[taildbg]     order %u: null\n", i); continue; }
		fprintf(stderr, "[taildbg]     order %u: type=%d dest=%u rvflags=%u unload=%d\n",
			i, (int)o->GetType(), o->GetDestination().ToStationID().base(),
			(uint)o->GetRVTransportFlags(), (int)o->GetUnloadType());
	}
	for (size_t i = 0; i < candidates.size(); i++) {
		auto &c = candidates[i];
		c.platform_idx = i / 2; // candidates come in per-platform pairs (one per end)
		if (leg.station != StationID::Invalid()) {
			/* The trackdir of a train travelling in direction dir on this platform track
			 * (TrackEnterdirToTrackdir takes the direction of travel, not the entry edge). */
			const Trackdir start_td = TrackEnterdirToTrackdir(GetRailStationTrack(c.exit_end), c.dir);
			if (start_td != INVALID_TRACKDIR) {
				/* Ask the engine's own pathfinder whether a train leaving the platform here can
				 * actually drive on and reach the station it will head to next. */
				c.fwd_reachable = YapfTrainCanReachStation(tr, c.exit_end, start_td, leg.station);
			}
		} else {
			c.fwd_reachable = true; // no next leg to probe for; any exit will do
		}
		if (target_xy != INVALID_TILE) {
			const int dx = std::abs(static_cast<int>(TileX(c.exit_end)) - static_cast<int>(TileX(target_xy)));
			const int dy = std::abs(static_cast<int>(TileY(c.exit_end)) - static_cast<int>(TileY(target_xy)));
			c.dist = std::max(dx, dy);
		}
	}

	/* The placed train is centred on the platform, so a train facing one end and then reversing
	 * off departs exactly like a train facing the other end. A platform is therefore usable in
	 * either orientation when at least one of its two ends can reach the next station. */
	for (size_t i = 0; i < candidates.size(); i += 2) {
		const bool either = candidates[i].fwd_reachable || candidates[i + 1].fwd_reachable;
		candidates[i].reachable = candidates[i + 1].reachable = either;
	}
	for (auto &c : candidates) {
		c.score = c.reachable ? 1 : 0;
	}

	/* When nothing reaches the next station, rank exit ends facing towards it first. */
	if (target_xy != INVALID_TILE && std::none_of(candidates.begin(), candidates.end(), [](const RVTransportRailCandidate &c) { return c.reachable; })) {
		for (auto &c : candidates) {
			const int ddx = static_cast<int>(TileX(target_xy)) - static_cast<int>(TileX(c.exit_end));
			const int ddy = static_cast<int>(TileY(target_xy)) - static_cast<int>(TileY(c.exit_end));
			/* Dot product of the exit direction with the bearing to the target: positive means the
			 * train would leave heading towards the next station. */
			const int dot = ddx * TileIndexDiffCByDiagDir(c.dir).x + ddy * TileIndexDiffCByDiagDir(c.dir).y;
			c.score = (dot > 0) ? 1 : 0;
		}
	}

	for (size_t i = 0; i < candidates.size(); i++) {
		const auto &c = candidates[i];
		fprintf(stderr, "[taildbg]   cand %zu (plat %u) end=(%d,%d) dir=%d fwd=%d reachable=%d dist=%d score=%d\n",
			i, c.platform_idx, (int)TileX(c.exit_end), (int)TileY(c.exit_end), (int)c.dir,
			c.fwd_reachable ? 1 : 0, c.reachable ? 1 : 0, c.dist, c.score);
	}

	std::stable_sort(candidates.begin(), candidates.end(), [](const RVTransportRailCandidate &a, const RVTransportRailCandidate &b) {
		if (a.score != b.score) return a.score > b.score;
		if (a.fwd_reachable != b.fwd_reachable) return a.fwd_reachable; // prefer leaving forward over reversing
		return a.dist < b.dist;
	});

	ranked = std::move(candidates);
}

/**
 * Put a carried train back on the rails, centred on the platform it is dropped on.
 *
 * The platform tiles between the leading vehicle and the exit end are reserved for the train (the
 * reservation is cleared again by the engine when the train leaves), and the track beyond the exit
 * end is held for it as well when that track has
 * exactly one continuation - on a junction the engine's own pathfinder picks a branch when the
 * train departs. The consist is laid out with the exact centre-to-centre spacing the engine itself
 * uses (see Train::CalcNextVehicleOffset()), so the vehicles stand bumper to bumper like a train
 * which has just stopped at the station.
 *
 * @param tr             the train being put down
 * @param exit_end       the last platform tile towards the exit
 * @param dir            direction the train faces
 * @param platform_tiles length of the platform, in tiles
 * @return true when the train was placed (and the reservations taken)
 */
static bool RVTransportPlaceTrainOnPlatform(Train *tr, TileIndex exit_end, DiagDirection dir, uint platform_tiles, bool tail_leads = false)
{
	tr = Train::From(tr->First());
	extern void UpdateVehicleTileHash(Vehicle *v, bool remove);

	const uint platform_units = platform_tiles * TILE_SIZE;
	const uint train_units = tr->gcache.cached_total_length;
	if (train_units > platform_units) return false;

	/* Centre the consist on the platform: the gap between its front and the exit end equals the gap
	 * between its rear and the far end, exactly like the engine centres a stopping train. */
	const uint gap = (platform_units - train_units) / 2;

	/* Pixel step of one vehicle unit along the exit direction, and the position of the exit edge:
	 * the boundary between the exit end tile and the tile beyond it. */
	const TileIndexDiffC unit = TileIndexDiffCByDir(DiagDirToDir(dir));
	const int edge_x = TileX(exit_end) * TILE_SIZE + TILE_SIZE / 2 + unit.x * (TILE_SIZE / 2);
	const int edge_y = TileY(exit_end) * TILE_SIZE + TILE_SIZE / 2 + unit.y * (TILE_SIZE / 2);

	/* Lay the consist out from the leading end towards the rear. Normally the leading end is the
	 * chain front; when only the chain tail can lead the train (a locomotive at the rear), the
	 * consist is laid out mirrored and flagged as driving backwards, so the tail leads the
	 * departure instead of the train backing out of the platform. All vehicles face the exit
	 * direction and none drives forwards, so the rounding of Train::CalcNextVehicleOffset() is the
	 * forward one.
	 * Note the consist caches MUST be refreshed after this (ConsistChanged below): the no-driving-cab
	 * speed limit is part of the consist cache, and it is judged by the leading end, which the
	 * backwards flag changes to the chain tail. */
	int s = static_cast<int>(gap) + (tr->gcache.cached_veh_length + 1) / 2; // centre of the leading vehicle
	std::vector<TileIndex> span; // the tiles the consist stands on
	TileIndex leading_tile = INVALID_TILE; // the tile the leading vehicle (front or tail) stands on
	for (Vehicle *u = (tail_leads ? tr->Last() : tr); u != nullptr; u = (tail_leads ? u->Previous() : u->Next())) {
		Train *tu = Train::From(u);
		const int x = edge_x - unit.x * s;
		const int y = edge_y - unit.y * s;

		u->rv_transport_flags &= ~RVTF_TRANSPORTED;
		u->rv_transport_flags &= ~RVTF_WAITING;
		u->transported_by = VehicleID::Invalid();
		u->transported_host_part = VehicleID::Invalid();
		u->transported_weight = 0;
		u->transported_from = StationID::Invalid();
		u->transport_wait_tick = 0;

		u->direction = DiagDirToDir(tail_leads ? ReverseDiagDir(dir) : dir);
		u->vehicle_flags.Set(VehicleFlag::DrivingBackwards, tail_leads);
		tu->flags.Reset(VehicleRailFlag::Reversing);
		tu->flags.Reset(VehicleRailFlag::BeyondPlatformEnd);
		tu->cur_speed = 0;
		u->progress = 0;
		u->vehstatus.Reset(VehState::Hidden);
		u->vehstatus.Reset(VehState::Stopped);

		const TileIndex pt = TileVirtXY(x, y);
		if (IsRailStationTile(pt)) {
			tu->track = TrackToTrackBits(GetRailStationTrack(pt));
		} else if (IsPlainRailTile(pt)) {
			tu->track = GetTrackBits(pt);
		}

		TrainMoveToPosition(tu, x, y);
		UpdateVehicleTileHash(u, false);  // back on the rail network
		InvalidateVehicleTickCaches();

		/* Remember the tiles the consist stands on, and the tile of the leading vehicle: the
		 * platform tiles between it and the exit end (inclusive) are reserved below. */
		if (std::find(span.begin(), span.end(), tu->tile) == span.end()) span.push_back(tu->tile);
		if (leading_tile == INVALID_TILE) leading_tile = tu->tile;

		Vehicle *u_next = tail_leads ? u->Previous() : u->Next();
		if (u_next != nullptr) {
			s += tu->gcache.cached_veh_length / 2 + (Train::From(u_next)->gcache.cached_veh_length + 1) / 2;
		}
	}

	/* Refresh the consist caches: setting the backwards flag above changed the leading end, so the
	 * no-driving-cab speed limit and friends must be re-judged from the new tail-leading state. */
	tr->ConsistChanged(CCF_COUPLE);
	if (tail_leads) {
		fprintf(stderr, "[taildbg] t#%u: placed tail-leads at end=(%d,%d) dir=%d: loco(v#%u) tile=(%d,%d) faces %d, DrivingBackwards=%d, front v#%u tile=(%d,%d)\n",
			tr->index.base(), (int)TileX(exit_end), (int)TileY(exit_end), (int)dir,
			tr->Last()->index.base(), (int)TileX(tr->Last()->tile), (int)TileY(tr->Last()->tile),
			(int)DirToDiagDir(tr->Last()->direction), tr->Last()->vehicle_flags.Test(VehicleFlag::DrivingBackwards) ? 1 : 0,
			tr->index.base(), (int)TileX(tr->tile), (int)TileY(tr->tile));
	}

	/* Reserve a contiguous chain from the exit end back to and including the leading vehicle's
	 * tile. The chain must end on a tile the train actually stands on: PBS attributes a
	 * reservation to a train found on the reserved chain itself, and a chain which ends on an
	 * empty tile in front of the train can fail to be attributed (its owner would look like an
	 * orphan and be cleaned up). The tiles behind the leading vehicle must NOT be reserved:
	 * platform tiles are never unreserved when a vehicle leaves them (ClearPathReservation only
	 * updates the platform-occupancy flag there), and the engine never frees reservations behind
	 * a train, so those would linger forever. When the train departs it adopts this chain into
	 * its own path reservation, which is freed again when the train next stops. The chain must be
	 * contiguous, or the tiles behind a gap would look like another train's reservation and the
	 * train would wait at "red" for its own exit. */
	const TileIndexDiff back_step = -TileOffsByDiagDir(dir);
	std::vector<TileIndex> reserve_tiles;
	for (TileIndex pt = exit_end;; pt += back_step) {
		reserve_tiles.push_back(pt);
		if (pt == leading_tile) break;
		if (!IsValidTile(pt + back_step) || !IsCompatibleTrainStationTile(pt + back_step, pt) || reserve_tiles.size() > platform_tiles) break;
	}
	bool all_reserved = true;
	for (const TileIndex &pt : reserve_tiles) {
		Track track = (IsRailStationTile(pt)) ? GetRailStationTrack(pt) : FindFirstTrack(GetTrackBits(pt));
		if (!TryReserveRailTrack(pt, track)) {
			all_reserved = false;
		} else {
		}
	}

	/* Hold the track beyond the exit end for the train, so that it can actually leave. */
	bool hold_needed = false; // the exit has exactly one continuation, which we want to hold
	bool hold_taken = false;  // the exit track was actually reserved and must be released on failure
	Track held_track = TRACK_BEGIN;
	const TileIndex exit_tile = TileAddByDiagDir(exit_end, dir);
	if (IsValidTile(exit_tile)) {
		TrackBits bits = TRACK_BIT_NONE;
		if (IsPlainRailTile(exit_tile)) bits = GetTrackBits(exit_tile);
		else if (IsRailStationTile(exit_tile)) bits = GetRailStationTrackBits(exit_tile);
		const TrackBits connecting = bits & DiagdirReachesTracks(ReverseDiagDir(dir));
		if (CountBits(static_cast<uint8_t>(connecting)) == 1) {
			/* Exactly one continuation: hold it for the train. On a fork or a dead end the engine's
			 * own pathfinder reserves a branch (or waits) when the train departs. */
			hold_needed = true;
			held_track = FindFirstTrack(connecting);
			hold_taken = TryReserveRailTrack(exit_tile, held_track);
		}
	}

	if ((hold_needed && !hold_taken) || !all_reserved) {
		/* The way out is already reserved by another train: leave the train on the carrier and try
		 * again later, rather than parking it somewhere it cannot move from. */
		for (const TileIndex &pt : reserve_tiles) UnreserveRailTrack(pt, (IsRailStationTile(pt)) ? GetRailStationTrack(pt) : FindFirstTrack(GetTrackBits(pt)));
		if (hold_taken) UnreserveRailTrack(exit_tile, held_track);
		return false;
	}

	tr->MarkDirty();
	return true;
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

/** How many tiles the vehicle transport fee charges for. */
static const uint RVTRANSPORT_TILES_PER_FEE_UNIT = 8;

/**
 * Charge the vehicle transport fee for having carried a road vehicle of one company on a carrier of
 * another one.
 *
 * The road vehicle's company pays its carrier's company when the vehicle is put down again. The
 * charge is the direct distance between the station the vehicle was loaded at and this one, times the
 * weight of the whole carrier, per 1000 tonnes and per eight tiles - the same per-1000-tonnes basis
 * the fee for trains on foreign tracks uses, with the distance measured up front instead of over time.
 * @param carrier         the carrier the vehicle was put down from
 * @param rv              the road vehicle that was put down
 * @param from_station_id station the vehicle was loaded at; this is passed in rather than read from
 *                        the vehicle, which no longer carries it by the time this is called
 * @param to_station      the station it was put down at
 */
/**
 * Charge the vehicle transport fee for having carried a road vehicle of one company on a carrier of
 * another one.
 *
 * The road vehicle's company pays its carrier's company when the vehicle is put down again. The
 * charge is the direct distance between the station the vehicle was loaded at and this one, times the
 * weight of the whole carrier, per 1000 tonnes and per eight tiles - the same per-1000-tonnes basis
 * the fee for trains on foreign tracks uses, with the distance measured up front instead of over time.
 * @param carrier         the carrier the vehicle was put down from
 * @param rv              the road vehicle that was put down
 * @param from_station_id station the vehicle was loaded at; this is passed in rather than read from
 *                        the vehicle, which no longer carries it by the time this is called
 * @param to_station      the station it was put down at
 */
static void RVTransportPayTransportFee(const Vehicle *carrier, const Vehicle *rv, StationID from_station_id, const Station *to_station)
{
	if (carrier == nullptr || rv == nullptr || to_station == nullptr) return;
	if (carrier->owner == rv->owner) return;                         // own vehicle, nothing to pay for
	if (!_settings_game.economy.infrastructure_sharing_rv) return;  // no shared transport, so no shared fee
	if (_settings_game.economy.rv_sharing_fee == 0) return;

	const Station *from_station = Station::GetIfValid(from_station_id);
	if (from_station == nullptr || from_station == to_station) return;  // no known origin, or no distance covered

	/* Direct distance between the two stations, in tiles. */
	const int dx = std::abs(static_cast<int>(TileX(from_station->xy)) - static_cast<int>(TileX(to_station->xy)));
	const int dy = std::abs(static_cast<int>(TileY(from_station->xy)) - static_cast<int>(TileY(to_station->xy)));
	const uint distance = static_cast<uint>(std::max(dx, dy));

	/* The weight the carrier has right now, which includes the road vehicle still on board. Nothing
	 * on board changes while it is being carried: the road vehicle does not tick at all, and the
	 * carrier's own cargo neither, so the weight at this moment is the weight for the whole ride. */
	const Vehicle *front = carrier->First();
	uint64_t weight = 0;
	if (front->IsGroundVehicle()) {
		weight = front->GetGroundVehicleCache()->cached_weight;
	} else {
		/* An aircraft has no consist weight cache and all of its parts share one engine, so that
		 * engine is counted once. */
		if (front->GetEngine() != nullptr) weight = front->GetEngine()->GetDisplayWeight();
		weight += RVTransportGetCarriedWeightTonnes(front);
	}
	if (weight == 0) return;

	Money cost = static_cast<Money>(_settings_game.economy.rv_sharing_fee) << 8;
	cost = static_cast<Money>((static_cast<uint64_t>(cost) * distance * weight) / (1000 * RVTRANSPORT_TILES_PER_FEE_UNIT));
	if (cost <= 0) return;

	PaySharingFee(const_cast<Vehicle *>(rv), carrier->owner, cost);
}

/**
 * Clear every reservation on this station's rail platforms that no train owns. PBS attribution is
 * geometric (it follows the reserved chain and looks for a train on it), so a reservation with no
 * train anywhere on its chain is an orphan: its chain was broken earlier (e.g. a train was loaded
 * onto a carrier from this platform), and no engine will ever free it. Such reservations would
 * block placements forever, so they are removed here.
 */
static void RVTransportHealOrphanReservations(const Station *st)
{
	for (TileIndex t : st->train_station) {
		if (!st->TileBelongsToRailStation(t)) continue;
		TrackBits reserved = GetReservedTrackbits(t);
		if (reserved == TRACK_BIT_NONE) continue;
		bool orphan = true;
		for (Track tr = TRACK_BEGIN; tr < TRACK_END; tr++) {
			if ((reserved & TrackToTrackBits(tr)) == TRACK_BIT_NONE) continue;
			if (GetTrainForReservation(t, tr) != nullptr) {
				orphan = false;
				break;
			}
		}
		if (!orphan) continue;
		for (Track tr = TRACK_BEGIN; tr < TRACK_END; tr++) {
			if ((GetReservedTrackbits(t) & TrackToTrackBits(tr)) == TRACK_BIT_NONE) continue;
			UnreserveRailTrack(t, tr);
		}
	}
}

/**
 * Unload road vehicles carried by this carrier at the given station.
 * @return true if at least one road vehicle reached the road network.
 */
bool RVTransportDetachAtStation(Vehicle *carrier, Station *st, bool force)
{
	extern void UpdateVehicleTileHash(Vehicle *v, bool remove);

	if (carrier == nullptr || st == nullptr) return false;

	/* Remove phantom reservations before trying to place anything. */
	RVTransportHealOrphanReservations(st);

	bool any = false;
	for (Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & RVTF_TRANSPORTED) == 0) continue;
		if (!RVTransportIsStateHolder(v)) continue; // the parts are handled together with the whole vehicle
		if (!RVTransportIsOnCarrier(carrier, v)) continue;

		/* Only unload a road vehicle which wants to be dropped here: the station of its own "be
		 * unloaded here" order. A road vehicle which declares no destination at all is dropped at the
		 * carrier's unload order, as it would otherwise never leave the carrier - and a carrier whose
		 * order says "unload all road vehicles" drops everything, whatever the vehicles declare. */
		if (!force && (carrier->current_order.GetRVTransportFlags() & ORVTF_UNLOAD_ALL) == 0) {
			const StationID declared = RVTransportGetDeclaredDestination(v);
			if (declared != StationID::Invalid() && declared != st->index) {
				continue;
			}
		}

		/* Trains are put back on the rails of this station's platforms; road vehicles use the
		 * road stops below. */
		if (v->type == VehicleType::Train) {
			Train *consist = Train::From(v->First());
			std::vector<RVTransportRailCandidate> ranked;
			FindFreeRailPlatformCandidates(st, consist, ranked);
			if (ranked.empty()) continue; // no room: stay on the carrier, retry later

			/* Don't let the train back out of the platform: when only the chain tail can lead the
			 * train (a locomotive at the rear), place the consist mirrored and marked as driving
			 * backwards, so the tail leads the departure towards the next destination. The consist
			 * caches are refreshed inside the placement, so the no-driving-cab speed limit is not
			 * applied to the (locomotive-equipped) leading end. */
			const bool tail_leads = !consist->CanLeadTrain() && consist->Last()->CanLeadTrain();
			fprintf(stderr, "[taildbg] t#%u: tail_leads=%d\n", consist->index.base(), tail_leads ? 1 : 0);

			/* Remember how the train was carried, in case the platform refuses it below: a
			 * distributed train records a host part and weight per member, so snapshot every
			 * member rather than only the state holder's. */
			std::vector<VehicleID> snap_hosts;
			std::vector<uint16_t> snap_weights;
			for (Vehicle *u = v->First(); u != nullptr; u = u->Next()) {
				snap_hosts.push_back(u->transported_host_part);
				snap_weights.push_back(u->transported_weight);
			}
			const VehicleID host_part = v->transported_host_part;
			const StationID transported_from = v->transported_from;

			bool placed = false;
			for (size_t ci = 0; ci < ranked.size() && !placed; ci++) {
				const RVTransportRailCandidate &c = ranked[ci];
				fprintf(stderr, "[taildbg] t#%u: try placement %zu end=(%d,%d) dir=%d reachable=%d fwd=%d\n",
					consist->index.base(), ci, (int)TileX(c.exit_end), (int)TileY(c.exit_end), (int)c.dir,
					c.reachable ? 1 : 0, c.fwd_reachable ? 1 : 0);
				if (!RVTransportPlaceTrainOnPlatform(consist, c.exit_end, c.dir, c.platform_tiles, tail_leads)) {
					/* This platform is blocked (e.g. its track is reserved by another train):
					 * put the layout attempt back and try the next-best platform. */
					continue;
				}
				placed = true;
			}
			if (!placed) {
				/* Every candidate was blocked: put the train back on the carrier rather than parking
				 * it somewhere it cannot move from, and retry later. */
				size_t i = 0;
				for (Vehicle *u = v->First(); u != nullptr; u = u->Next(), i++) {
					u->rv_transport_flags |= RVTF_TRANSPORTED;
					u->transported_by = carrier->index;
					u->transported_host_part = snap_hosts[i];
					u->transported_weight = snap_weights[i];
					u->transported_from = transported_from;
					u->vehicle_flags.Reset(VehicleFlag::DrivingBackwards); // the layout attempt may have set it
					u->vehstatus.Set(VehState::Stopped);
					u->vehstatus.Set(VehState::Hidden);
					u->cur_speed = 0;
					UpdateVehicleTileHash(u, true);   // off the rail network again
					InvalidateVehicleTickCaches();
					u->UpdateIsDrawn();
					u->Vehicle::UpdateViewport(true);
				}
				continue;
			}

			RVTransportPayTransportFee(carrier, v, transported_from, st);

			/* The train stands centred on the platform now: let it enter the station's loading logic
			 * exactly like a train which has just arrived, so that the "be unloaded here" order it
			 * is on completes and the train departs through the normal engine flow. */
			Train *primary = consist->Primary();
			primary->last_station_visited = st->index;
			primary->BeginLoading();

			carrier->MarkDirty();
			RVTransportRefreshCarrier(carrier, Vehicle::GetIfValid(host_part));
			any = true;
			continue;
		}


		TileIndex tile = INVALID_TILE;
		DiagDirection dd = DiagDirection::NE;
		if (!FindFreeRoadStopTile(st, v, tile, dd)) continue; // no room: stay on the carrier, retry later

		/* Remember how the vehicle was carried, in case the stop refuses it below. */
		const VehicleID host_part = v->transported_host_part;
		const uint16_t carried_weight = v->transported_weight;
		const StationID transported_from = v->transported_from;

		/* A bay is a dead end: the vehicle drives into it and reverses out again, so it has to be put
		 * down travelling towards the station, exactly like a vehicle which just entered the tile.
		 * A drive-through stop is left through the road end, so the vehicle keeps travelling towards
		 * it. Getting this wrong leaves the vehicle facing the wall of the bay, and it then drives
		 * into that wall when it tries to leave. */
		const DiagDirection travel_dd = IsBayRoadStopTile(tile) ? ReverseDiagDir(dd) : dd;

		/* Put the whole road vehicle on that tile, in the same way a vehicle leaves a depot: every
		 * part starts on the tile and spreads out while the vehicle drives off. */
		for (Vehicle *u = v->First(); u != nullptr; u = u->Next()) {
			u->rv_transport_flags &= ~RVTF_TRANSPORTED;
			u->transported_by = VehicleID::Invalid();
			u->transported_host_part = VehicleID::Invalid();
			u->transported_weight = 0;
			u->transported_from = StationID::Invalid();
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
			for (Vehicle *u = v->First(); u != nullptr; u = u->Next()) {
				u->rv_transport_flags |= RVTF_TRANSPORTED;
				u->transported_by = carrier->index;
				u->transported_host_part = host_part;
				u->transported_weight = carried_weight;
				u->transported_from = transported_from;
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

		RVTransportPayTransportFee(carrier, v, transported_from, st);

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
 * The order the vehicle is executing right now (current_order) is the authoritative answer and is
 * used as such: the order list indices cannot be trusted for this. They are only brought in step
 * with the executed order by ProcessOrders(), which runs from the vehicle's own controller - and a
 * carried vehicle is stopped, so its controller returns before ever getting there. The indices can
 * therefore lag behind (or run ahead of) the order the vehicle is really on, and scanning the list
 * from them would skip the "be unloaded here" order and report some later station instead.
 *
 * @return The station id, or an invalid id when the vehicle declares no destination.
 */
StationID RVTransportGetDeclaredDestination(const Vehicle *rv)
{
	if (rv == nullptr) return StationID::Invalid();

	/* Being loaded onto a carrier has advanced the vehicle to its "be unloaded here" order, which is
	 * the order it is executing now: that is where it wants to get off. */
	const Order &current = rv->current_order;
	if ((current.IsType(OT_GOTO_STATION) || current.IsType(OT_GOTO_WAYPOINT)) &&
			(current.GetRVTransportFlags() & ORVTF_OWN_UNLOAD) != 0) {
		return current.GetDestination().ToStationID();
	}

	/* The vehicle has not been loaded yet, so its current order is the "wait to be transported" one
	 * (or something else): the station it wants to get off at is the first "be unloaded here" order
	 * from the order it is executing onwards. */
	const VehicleOrderID num_orders = rv->GetNumOrders();
	if (num_orders == 0) return StationID::Invalid();

	const VehicleOrderID current_index = (rv->cur_implicit_order_index < num_orders) ? rv->cur_implicit_order_index : 0;
	for (VehicleOrderID i = 0; i < num_orders; i++) {
		const Order *o = rv->GetOrder(static_cast<VehicleOrderID>((current_index + i) % num_orders));
		if (o == nullptr || !o->IsType(OT_GOTO_STATION)) continue;
		if ((o->GetRVTransportFlags() & ORVTF_OWN_UNLOAD) != 0) return o->GetDestination().ToStationID();
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
		if (!RVTransportIsStateHolder(v)) continue;
		if (!RVTransportIsOnCarrier(carrier, v)) continue;
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
		if (v->type != VehicleType::Road && v->type != VehicleType::Train) continue;
		if (!RVTransportIsStateHolder(v)) continue;
		if (v->last_station_visited != st->index) {
			continue;
		}
		if (carrier != nullptr && !RVTransportOrderAllowsCandidate(carrier, v)) {
			continue; // does not match: skip it
		}
		return v;
	}
	return nullptr;
}

/**
 * Can this road vehicle be put back on the road network at \a t?
 *
 * A carried road vehicle keeps the tile it had when it was loaded, and while a road vehicle is in
 * a tunnel or on a bridge that tile is frozen (the controller does not advance it there), so the
 * remembered tile can well be a tunnel tile. Asking "is this a road stop or a normal road tile" is
 * therefore not enough: tunnels and bridges carry road as well.
 * @param rv Road vehicle which would be placed there.
 * @param t Tile to check.
 * @return true if the vehicle can stand on that tile.
 */
static bool RVTransportCanPutDownHere(const RoadVehicle *rv, TileIndex t)
{
	if (!IsValidTile(t)) return false;
	if (IsAnyRoadStopTile(t)) return true;
	if (IsNormalRoadTile(t)) return true;
	return IsTileType(t, TileType::TunnelBridge) && HasRoadTypeRoad(t) && rv->compatible_roadtypes.Test(GetRoadTypeRoad(t));
}

/**
 * Train counterpart of RVTransportCanPutDownHere(): can the train stand on this tile (plain rail,
 * rail station or a rail tunnel/bridge)?
 */
static bool RVTransportTrainCanPutDownHere(const Train *tr, TileIndex t)
{
	if (!IsValidTile(t)) return false;
	if (IsPlainRailTile(t)) return RVTransportRailTypeCompatible(tr, GetRailType(t));
	if (IsRailStationTile(t) || IsRailWaypointTile(t)) return RVTransportRailTypeCompatible(tr, GetRailType(t));
	return IsTileType(t, TileType::TunnelBridge) && GetTunnelBridgeTransportType(t) == TransportType::Rail;
}

/**
 * Check the carried state of all carried vehicles after a savegame was loaded. A vehicle which
 * claims to be carried by a vehicle that does not exist any more (or by something which cannot be
 * a carrier) must not stay hidden and frozen on the map: it is put back on the road or the rails,
 * or, if there is no sane place for it, removed.
 */
void RVTransportValidateAfterLoad()
{
	std::vector<VehicleID> release;
	std::vector<VehicleID> remove;

	for (Vehicle *v : Vehicle::Iterate()) {
		if (v->type != VehicleType::Road && v->type != VehicleType::Train) continue;

		if (!RVTransportIsStateHolder(v)) {
			/* A following part - of an articulated road vehicle or of a train - follows the unit which
			 * holds the transport state: only clear stale state here. */
			if ((v->Primary()->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) {
				/* A part which was carried itself is hidden because it is being carried: nothing
				 * else can hide it, because it is off the road network and frozen. A part which
				 * was not carried is hidden for one of the engine's own reasons instead (a road
				 * vehicle in a tunnel or in a depot is hidden), and VehState::Hidden must not be
				 * touched for those - clearing it there leaves a vehicle which claims to be in a
				 * tunnel while being drawn, which trips the assertion in
				 * RoadVehicle::GetCurrentMaxSpeed(). */
				const bool was_carried = (v->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) != 0;

				v->rv_transport_flags &= ~Vehicle::RV_TRANSPORT_CARRIED;
				v->transported_by = VehicleID::Invalid();
				v->transported_host_part = VehicleID::Invalid();
				v->transported_weight = 0;
				if (was_carried) v->vehstatus.Reset(VehState::Hidden);
			}
			continue;
		}

		if ((v->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) {
			/* Not carried: drop any stale carrier reference left behind. */
			v->transported_by = VehicleID::Invalid();
			v->transported_host_part = VehicleID::Invalid();
			v->transported_weight = 0;
			v->transported_from = StationID::Invalid();
			continue;
		}

		/* The part the vehicle occupies decides whether it is really carried: it has to exist, and a
		 * road vehicle is never a carrier. The stored transported_by is deliberately not consulted:
		 * it names a vehicle of the consist the road vehicle was loaded onto, which splitting,
		 * joining or rearranging the consist may have moved into another part since. */
		const Vehicle *part = Vehicle::GetIfValid(v->transported_host_part);
		if (part != nullptr && part->type != v->type) {
			/* Carried as expected. A savegame written before the road vehicle's order was advanced when
			 * it was loaded still has the "wait to be transported" order as its current order: catch
			 * up. The station stop of the vehicle is finished here as well - a savegame can also have
			 * been written before that bookkeeping existed, in which case the station it was picked up
			 * at still lists it as a loading vehicle and it still holds a cargo payment. */
			RVTransportLeaveBoardingStation(v);
			if ((v->current_order.GetRVTransportFlags() & ORVTF_OWN_WAIT) != 0) RVTransportAdvanceCarriedVehicleOrder(v);
			continue; // carried as expected
		}

		Debug(misc, 0, "RoRo: road vehicle #{} was carried by missing part #{}", v->index.base(), v->transported_host_part.base());
		v->transported_by = VehicleID::Invalid();
		v->transported_host_part = VehicleID::Invalid();
		const bool can_put_down = (v->type == VehicleType::Train)
				? RVTransportTrainCanPutDownHere(Train::From(v), v->tile)
				: RVTransportCanPutDownHere(RoadVehicle::From(v), v->tile);
		if (can_put_down) {
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
		for (Vehicle *u = front->First(); u != nullptr; u = u->Next()) chain.push_back(u->index);
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
	for (Vehicle *u = rv->First(); u != nullptr; u = u->Next()) {
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
		} else if (u->type == VehicleType::Train && IsValidTile(tile)) {
			Train *tu = Train::From(u);
			u->tile = tile;
			u->x_pos = TileX(tile) * TILE_SIZE + TILE_SIZE / 2;
			u->y_pos = TileY(tile) * TILE_SIZE + TILE_SIZE / 2;
			u->z_pos = GetSlopePixelZ(u->x_pos, u->y_pos);
			u->direction = DiagDirToDir(DiagDirection::NE);
			if (IsRailStationTile(tile)) {
				tu->track = TrackToTrackBits(GetRailStationTrack(tile));
			} else if (IsPlainRailTile(tile)) {
				tu->track = GetTrackBits(tile);
			}
			UpdateVehicleTileHash(u, true);    // make sure it is not listed where it came from
			UpdateVehicleTileHash(u, false);   // back on the rail network
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

	const Vehicle *carrier = RVTransportGetCarrier(v);
	if (carrier == nullptr) return v;
	return carrier->GetMovingFront();
}

/**
 * Destroy the road vehicles held by this vehicle: they are lost together with it, exactly like the
 * wagons of a crashed train, instead of being left behind on the map. Covers both a whole carrier
 * which is going away (they are on one of its parts) and a single part which is removed on its own
 * (a wagon sold in a depot), which loses the road vehicles it holds.
 */
void RVTransportDestroyCarriedVehicles(Vehicle *carrier)
{
	if (carrier == nullptr) return;
	if (carrier->type == VehicleType::Road) return; // a road vehicle is never a carrier

	/* Collect first: deleting a vehicle modifies the pool, so it must not happen while iterating. */
	std::vector<VehicleID> fronts;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if ((v->rv_transport_flags & Vehicle::RV_TRANSPORT_CARRIED) == 0) continue;
		if (!v->IsFrontEngine()) continue;
		if (v->transported_host_part != carrier->index && !RVTransportIsOnCarrier(carrier, v)) continue;
		fronts.push_back(v->index);
	}

	for (const VehicleID id : fronts) {
		Vehicle *front = Vehicle::GetIfValid(id);
		if (front == nullptr) continue;

		/* Gather the whole (possibly articulated) vehicle, then delete it from the rear, as a part
		 * must never outlive the vehicle it is attached to. */
		std::vector<VehicleID> chain;
		for (Vehicle *u = front->First(); u != nullptr; u = u->Next()) chain.push_back(u->index);

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
