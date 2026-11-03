/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file yapf.h Entry point for OpenTTD to YAPF. */

#ifndef YAPF_H
#define YAPF_H

#include "../../direction_type.h"
#include "../../track_type.h"
#include "../../vehicle_type.h"
#include "../../ship.h"
#include "../../roadveh.h"
#include "../../vehicle_func.h"
#include "../pathfinder_type.h"
#include <functional>
#include <optional>
#include <vector>

/**
 * Finds the best path for given ship using YAPF.
 * @param v        the ship that needs to find a path
 * @param tile     the tile to find the path from (should be next tile the ship is about to enter)
 * @param path_found [out] Whether a path has been found (true) or has been guessed (false)
 * @param path_cache Cache of a previously chosen path.
 * @return         the best trackdir for next turn or INVALID_TRACK if the path could not be found
 */
Track YapfShipChooseTrack(const Ship *v, TileIndex tile, bool &path_found, ShipPathCache &path_cache);

/**
 * Returns true if it is better to reverse the ship before leaving depot using YAPF.
 * @param v the ship leaving the depot
 * @param trackdir [out] the best of all possible reversed trackdirs
 * @return true if reversing is better
 */
bool YapfShipCheckReverse(const Ship *v, Trackdir *trackdir);

/**
 * Finds the best path for given road vehicle using YAPF.
 * @param v         the RV that needs to find a path
 * @param tile      the tile to find the path from (should be next tile the RV is about to enter)
 * @param enterdir  diagonal direction which the RV will enter this new tile from
 * @param trackdirs available trackdirs on the new tile (to choose from)
 * @param path_found [out] Whether a path has been found (true) or has been guessed (false)
 * @param path_cache Cache of a previously chosen path.
 * @return          the best trackdir for next turn or INVALID_TRACKDIR if the path could not be found
 */
Trackdir YapfRoadVehicleChooseTrack(const RoadVehicle *v, TileIndex tile, DiagDirection enterdir, TrackdirBits trackdirs, bool &path_found, RoadVehPathCache &path_cache);

/**
 * Cost of the cheapest path a road vehicle could drive from a road tile to a station, or UINT32_MAX
 * when it cannot get there within the node budget. The vehicle is not touched at all (no path cache,
 * no "pathfinder lost" handling), so this can be called repeatedly to compare candidate tiles.
 * @param v                the RV the path is for
 * @param tile             the road tile to start from
 * @param enterdir         diagonal direction which the RV enters this tile from
 * @param station          the station to drive to
 * @param travel_direction direction the RV must be travelling in on arrival, or DiagDirection::Invalid
 * @param max_nodes        node budget for this search
 * @param cost            [out] cost of the path, only valid when this returns true
 * @return whether the station was reached; false means the node budget ran out or there is no path
 *         at all, and  cost then does not describe the distance to the station
 */
bool YapfRoadVehicleProbeToStation(const RoadVehicle *v, TileIndex tile, DiagDirection enterdir, StationID station,
		DiagDirection travel_direction, int max_nodes, int &cost);

/**
 * Finds the best path for given train using YAPF.
 * @param v        the train that needs to find a path
 * @param tile     the tile to find the path from (should be next tile the train is about to enter)
 * @param enterdir diagonal direction which the RV will enter this new tile from
 * @param tracks   available trackdirs on the new tile (to choose from)
 * @param path_found [out] Whether a path has been found (true) or has been guessed (false)
 * @param reserve_track indicates whether YAPF should try to reserve the found path
 * @param target   [out] the target tile of the reservation, free is set to true if path was reserved
 * @param dest     [out] the final tile of the best path found
 * @return         the best track for next turn
 */
Track YapfTrainChooseTrack(const Train *v, TileIndex tile, DiagDirection enterdir, TrackBits tracks, bool &path_found, bool reserve_track, struct PBSTileInfo *target, TileIndex *dest);
struct Train;
struct Order;
Track YapfTrainCoupleTrack(const Train *v, bool dont_reserve, Train **couple_target = nullptr, uint32_t *couple_cost = nullptr, const Order *criteria = nullptr, const Train *only_target = nullptr, uint32_t max_nodes = 0);

/**
 * Used when user sends road vehicle to the nearest depot or if road vehicle needs servicing using YAPF.
 * @param v            vehicle that needs to go to some depot
 * @param max_penalty  max distance (in pathfinder penalty) from the current vehicle position
 *                     (used also as optimization - the pathfinder can stop path finding if max_penalty
 *                     was reached and no depot was seen)
 * @return             the data about the depot
 */
FindDepotData YapfRoadVehicleFindNearestDepot(const RoadVehicle *v, int max_penalty);

/**
 * Used when user sends train to the nearest depot or if train needs servicing using YAPF.
 * @param v            train that needs to go to some depot
 * @param max_distance max distance (int pathfinder penalty) from the current train position
 *                     (used also as optimization - the pathfinder can stop path finding if max_penalty
 *                     was reached and no depot was seen)
 * @return             the data about the depot
 */
FindDepotData YapfTrainFindNearestDepot(const Train *v, int max_distance);

/**
 * Returns true if it is better to reverse the train before leaving station using YAPF.
 * @param v the train leaving the station
 * @return true if reversing is better
 */
bool YapfTrainCheckReverse(const Train *v);

bool YapfTrainCheckDepotReverse(const Train *v, TileIndex forward_depot, TileIndex reverse_depot);

/**
 * Try to extend the reserved path of a train to the nearest safe tile using YAPF.
 *
 * @param v    The train that needs to find a safe tile.
 * @param tile Last tile of the current reserved path.
 * @param td   Last trackdir of the current reserved path.
 * @param override_railtype Should all physically compatible railtypes be searched, even if the vehicle can't run on them on its own?
 * @return True if the path could be extended to a safe tile.
 */
bool YapfTrainFindNearestSafeTile(const Train *v, TileIndex tile, Trackdir td, bool override_railtype);

/** Vehicle transport: can a train leaving the given tile/trackdir reach the given rail station? */
bool YapfTrainCanReachStation(const Train *v, TileIndex tile, Trackdir td, StationID station);

/** Read-only, cache-independent station reachability with a strict node cap.
 * Does not reserve track, update the train, or return a guessed route. */
bool YapfTrainCanReachStationBounded(const Train *v, TileIndex tile, Trackdir td, StationID station, uint max_nodes, uint &nodes_used);

/** A selected native rail route, without a reservation transaction. */
enum class YapfRailRouteReason : uint8_t {
	Complete,
	InvalidInput,
	NoPath, ///< No route under the current native signal/restriction conditions.
	NodeBudget, ///< Search was incomplete, not proof that a route cannot exist.
	UnsupportedTarget, ///< An exact target inside a compressed platform is not an endpoint.
	UnsupportedReverse, ///< A native depot/signal turnaround cannot be a simple directed corridor.
	RouteLimit,
	InvalidTrace,
};

struct YapfRailRouteStep {
	TileIndex tile;
	Trackdir trackdir;
	/** A bridge/tunnel is one native track arc, not the ground tiles beneath it. */
	TileIndex wormhole_from = INVALID_TILE;
	Trackdir wormhole_from_trackdir = INVALID_TRACKDIR;
	uint wormhole_tiles = 0;
};

struct YapfRailRoute {
	bool complete = false;
	YapfRailRouteReason reason = YapfRailRouteReason::InvalidInput;
	uint nodes = 0;
	int cost = -1;
	std::vector<YapfRailRouteStep> path;
};

using YapfRailRouteFilter = std::function<bool(TileIndex, Trackdir)>;

/**
 * Select and export the actual native best route from an explicit origin.
 * A filter may reject directed track resources, never add path penalties.
 * An optional exact target replaces the current-order destination, without
 * modifying the order. Exact platform targets must be their directed far end.
 * An optional required point must be traversed in its directed track state
 * before reaching the target; its search uses a separate two-phase node list.
 * Does not reserve, advance orders, acquire slots, or return partial paths.
 * Callers providing a future order context must preserve that context and any
 * callback caches/RNG state around this read-only query.
 * With export_end, a successful full destination search may export just its
 * selected prefix ending at the first matching directed resource. This does
 * not replace the destination, lower its cost, or accept an incomplete search.
 */
YapfRailRoute YapfTrainPlanRoute(const Train *v, TileIndex origin, Trackdir td, uint max_nodes,
		const YapfRailRouteFilter &filter = {}, TileIndex exact_target = INVALID_TILE,
		Trackdir exact_td = INVALID_TRACKDIR, uint max_path_steps = 16384,
		std::optional<std::pair<TileIndex, Trackdir>> required_point = std::nullopt,
		const YapfRailRouteFilter &export_end = {});

#endif /* YAPF_H */

struct Aircraft;
struct PBSTileInfo;
struct AircraftPathChoice;
enum AircraftState : uint8_t;
Trackdir YapfAircraftFindPath(const Aircraft *v, PBSTileInfo &best_dest, bool &path_found, AircraftState dest_state, AircraftPathChoice &path_cache);
