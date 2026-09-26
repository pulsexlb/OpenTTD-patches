/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file roadveh_transport.h Road vehicles carried by other vehicles (RoRo). */

#ifndef ROADVEH_TRANSPORT_H
#define ROADVEH_TRANSPORT_H

#include "core/enum_type.hpp"
#include "station_type.h"
#include "vehicle_type.h"

class Vehicle;
struct Station;

/** Bits stored in Vehicle::rv_transport_flags. */
static const uint8_t RVTF_WAITING     = 1 << 0; ///< Road vehicle waits at a station to be loaded onto a carrier.
static const uint8_t RVTF_TRANSPORTED = 1 << 1; ///< Road vehicle is currently carried by another vehicle (off the road network).
static const uint8_t RVTF_UNLOAD_WARNED = 1 << 2; ///< The "carried for too long" warning was already shown for this trip.

/** Bits stored in OrderExtraInfo::rv_transport_flags. */
static const uint8_t ORVTF_LOAD   = 1 << 0; ///< This station order loads road vehicles onto the carrier.
static const uint8_t ORVTF_UNLOAD = 1 << 1; ///< This station order unloads road vehicles from the carrier.
static const uint8_t ORVTF_MATCH_DEST = 1 << 2; ///< Only load road vehicles whose declared unload station equals the carrier's next stop.
static const uint8_t ORVTF_WAIT = 1 << 3;       ///< Keep waiting at this station until road vehicles have been loaded.
static const uint8_t ORVTF_UNLOAD_ALL = 1 << 4; ///< Unload every road vehicle here, ignoring the station each of them declares itself.
/* The two bits below are the road vehicle's own view on the same feature and are deliberately
 * separate from the carrier bits above: a standalone/shared order list is not bound to a vehicle
 * type, so one and the same order can have to say both "wait to be transported" (for a road
 * vehicle running it) and "load road vehicles" (for a carrier running it). */
static const uint8_t ORVTF_OWN_WAIT   = 1 << 5; ///< Road vehicle only: this station order makes the road vehicle wait to be transported here.
static const uint8_t ORVTF_OWN_UNLOAD = 1 << 6; ///< Road vehicle only: this station order makes the road vehicle get off a carrier here.

/**
 * Is this vehicle a dedicated road vehicle carrier: every cargo-carrying part of its chain is
 * refitted to the dedicated "Vehicles" cargo? Such a vehicle never transports normal cargo, so its
 * station orders default to (and its order buttons advertise) road vehicle transport. A vehicle
 * with no cargo capacity at all, or with any normal-cargo part, is not dedicated, and its orders
 * default to normal cargo.
 */
bool RVTransportVehicleCarriesOnlyVehicles(const Vehicle *v);

/**
 * Which parts of a carrier (train wagons, ship holds, aircraft compartments) may carry road vehicles.
 * This is the "vehicle.rv_transport_carrier_parts" setting; a part always has to have room for the
 * vehicle's weight on top of this.
 */
enum class RVTransportCarrierParts : uint8_t {
	AnyPart = 0,                ///< Any part with cargo capacity.
	OversizedOnly = 1,          ///< Only parts whose current cargo belongs to CargoClass::Oversized.
	BulkOversizedOrVehicles = 2, ///< Only parts whose cargo is bulk, oversized, or the NewGRF "Vehicles" cargo (label 'VEHC').
};

/**
 * Selection criteria of a carrier's "load road vehicles" order: the loader only takes road vehicles
 * which satisfy every criterion which is in use ("no match, skip this vehicle"), in the same spirit
 * as the coupling parameters of the px-patch train coupling feature.
 */
enum RVTransportLoadState : uint8_t {
	RVTLS_ANY   = 0, ///< Any road vehicle.
	RVTLS_EMPTY = 1, ///< Only an empty road vehicle.
	RVTLS_FULL  = 2, ///< Only a fully loaded road vehicle.
};

/** Cargo criterion of a carrier's "load road vehicles" order. */
enum RVTransportCargoMode : uint8_t {
	RVTC_ANY        = 0, ///< Any cargo.
	RVTC_CAN_CARRY  = 1, ///< The road vehicle must be able to carry the given cargo.
	RVTC_IS_CARRYING = 2, ///< The road vehicle must currently carry the given cargo.
};

/** Does this station order select road vehicles by any criterion? */
bool RVTransportOrderHasCriteria(const struct Order &order);

/**
 * Would this station order take the given road vehicle as a candidate? Applies the destination
 * match, the load state, the cargo and the minimum waiting time criteria of the order.
 */
bool RVTransportOrderAllowsCandidate(const Vehicle *carrier, const Vehicle *rv);

/** Station a road vehicle wants to be unloaded at (from its own "unload road vehicles" order), or invalid. */
StationID RVTransportGetDeclaredDestination(const Vehicle *rv);

/** Next station the carrier stops at after its current order, or invalid. */
StationID RVTransportGetNextCarrierStop(const Vehicle *carrier);

/** Weight in tonnes a road vehicle occupies on a carrier (rounded up). */
uint32_t RVTransportGetVehicleWeightTonnes(const Vehicle *rv);

/** Transport capacity in tonnes of a carrier part (based on its current cargo and capacity). */
uint32_t RVTransportGetPartCapacityTonnes(const Vehicle *part);

/** Tonnes already used on this carrier part by carried road vehicles. */
uint32_t RVTransportGetPartUsedTonnes(const Vehicle *part);

/** Can this carrier part carry road vehicles at all (cargo class oversized)? */
bool RVTransportPartCanCarry(const Vehicle *part);

/**
 * Set/clear the "waiting to be transported" state of a road vehicle.
 * @param waiting Whether the vehicle waits for a carrier.
 */
void RVTransportSetWaiting(Vehicle *rv, bool waiting);

/**
 * Called for every ticked road vehicle: ends the "waiting to be transported" state when the vehicle
 * was told to do something else in the meantime (the player skipped the order, sent it to a depot,
 * ...). Without this the vehicle would keep sitting at the station with the waiting flag set, and a
 * "go to depot" order would never be carried out because the vehicle stays stopped.
 */
void RVTransportTickWaiting(Vehicle *rv);

/**
 * Warn once when a road vehicle has been carried for longer than the configured number of days
 * (vehicle.rv_transport_unload_warn_days, 0 = no warning). Called from the daily vehicle loop for
 * carried vehicles, which are otherwise skipped there.
 * @param v The carried road vehicle.
 */
void RVTransportCheckCarriedTooLong(Vehicle *v);

/**
 * Toggle one road vehicle transport flag of a station order, keeping the combination meaningful:
 * the destination match and waiting only make sense while road vehicles are loaded, and a road
 * vehicle's own order never uses the destination match.
 * @param flags Current order flags (ORVTF_* bits).
 * @param bit The single flag to toggle.
 * @param is_road_vehicle Whether the order belongs to a road vehicle (rather than to a carrier).
 * @return The new flags.
 */
uint8_t RVTransportToggleOrderFlag(uint8_t flags, uint8_t bit, bool is_road_vehicle);

/** Try to load one road vehicle onto a carrier part. Returns true when carried. */
bool RVTransportAttach(Vehicle *carrier, Vehicle *part, Vehicle *rv, bool force = false);

/** Try to attach a road vehicle to any suitable part of the carrier. */
bool RVTransportAttachAuto(Vehicle *carrier, Vehicle *rv, bool force = false);

/**
 * Unload road vehicles carried by this carrier at the given station. Returns true if anything was unloaded.
 * @param force unload every carried road vehicle, even the ones which want to get off somewhere else
 *        (debug only).
 */
bool RVTransportDetachAtStation(Vehicle *carrier, Station *st, bool force = false);

/**
 * How many road vehicles this carrier still carries which want to be dropped at this station (their
 * own "be unloaded here" order, or no declared destination at all). Used to keep a carrier which was
 * told to wait for road vehicles waiting until the station has room for them.
 */
uint32_t RVTransportCountWantingUnloadHere(const Vehicle *carrier, const Station *st);

/**
 * Find the first road vehicle waiting to be transported at this station which satisfies the
 * selection criteria of the carrier's current order (see RVTransportOrderAllowsCandidate()).
 * @param st Station to look at.
 * @param carrier Carrier which wants to load; when given, its order criteria are applied.
 */
Vehicle *RVTransportFindWaitingAtStation(const Station *st, const Vehicle *carrier = nullptr);

/** Number of road vehicles currently carried by this carrier (front vehicle). */
uint32_t RVTransportCountOnCarrier(const Vehicle *carrier);

/** First road vehicle currently carried by this carrier (front vehicle), or nullptr. */
Vehicle *RVTransportFindFirstOnCarrier(const Vehicle *carrier);

/**
 * Collect the road vehicles this carrier currently holds (front vehicles only, in vehicle id order).
 * @param carrier Carrier (front vehicle) to look at.
 * @param out Receives the carried road vehicles.
 */
void RVTransportGetCarriedVehicles(const Vehicle *carrier, std::vector<const Vehicle *> &out);

/**
 * Weight in tonnes of the road vehicles this carrier holds. Added to the carrier's own weight by
 * GroundVehicle::CargoChanged(), so that accelerating, climbing and braking account for them.
 */
uint32_t RVTransportGetCarriedWeightTonnes(const Vehicle *carrier);

/**
 * Does this carrier part hold any road vehicle? Used for the weight above and for drawing the part
 * with its "loaded" appearance while it carries road vehicles.
 */
bool RVTransportPartHoldsRoadVehicles(const Vehicle *part);

/**
 * Cargo units a carrier part should report on top of what it really carries, so that NewGRF sets
 * which derive their sprites from the cargo amount also show the "loaded" appearance while the part
 * carries road vehicles (the part is reported as full).
 */
uint16_t RVTransportExtraCargoAmount(const Vehicle *part);

#ifdef RORO_DEBUG_COMMANDS
/** Debug (RoRo): print one station's road stops and whether this road vehicle could be put down. */
void RVTransportDebugStation(const Vehicle *rv, const Station *st);

/** Debug (RoRo): one-shot dump of every road vehicle, carrier, part and station involved. */
void RVTransportDebugDump();

/**
 * Debug (RoRo): is this vehicle still in any station's list of vehicles which are loading there?
 * (It must not be, once it has been carried away from that station.)
 */
bool RVTransportDebugStationLists(const Vehicle *v);
#endif /* RORO_DEBUG_COMMANDS */

/**
 * Vehicle whose position represents this vehicle on the map: a carried road vehicle is where its
 * carrier is. Used by the camera/viewport follow ("centre on vehicle") and other actions which
 * need the position of a vehicle.
 */
const Vehicle *RVTransportGetFollowVehicle(const Vehicle *v);

/**
 * Destroy the road vehicles carried by this carrier: they are lost together with it (as when a
 * train crashes), instead of being left behind on the map.
 */
void RVTransportDestroyCarriedVehicles(Vehicle *carrier);

/**
 * Fix up the carried state of all road vehicles after a savegame was loaded (a road vehicle whose
 * carrier does not exist any more is put back on the road).
 */
void RVTransportValidateAfterLoad();

/**
 * Emergency release of a carried road vehicle, used when its carrier is destroyed: the road
 * vehicle is put back on the road network at its remembered tile and continues on its own.
 */
void RVTransportForceRelease(Vehicle *rv);

#endif /* ROADVEH_TRANSPORT_H */
