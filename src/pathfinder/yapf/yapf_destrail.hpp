/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file yapf_destrail.hpp Determining the destination for rail vehicles. */

#ifndef YAPF_DESTRAIL_HPP
#define YAPF_DESTRAIL_HPP

#include "../../train.h"
#include "../../pbs.h"
#include "../../tracerestrict.h"
#include "../../vehicle_func.h"
#include "../../infrastructure_func.h"
#include "../pathfinder_func.h"
#include "../pathfinder_type.h"

class CYapfDestinationRailBase {
protected:
	RailTypes compatible_railtypes;

public:
	void SetDestination(const Train *v, bool override_rail_type = false)
	{
		this->compatible_railtypes = v->compatible_railtypes;
		if (override_rail_type) this->compatible_railtypes.Set(GetAllCompatibleRailTypes(v->railtypes));
	}

	bool IsCompatibleRailType(RailType rt)
	{
		return this->compatible_railtypes.Test(rt);
	}

	RailTypes GetCompatibleRailTypes() const
	{
		return this->compatible_railtypes;
	}
};

template <class Types>
class CYapfDestinationAnyDepotRailT : public CYapfDestinationRailBase {
public:
	typedef typename Types::Tpf Tpf; ///< the pathfinder class (derived from THIS class)
	typedef typename Types::NodeList::Item Node; ///< this will be our node type
	typedef typename Node::Key Key; ///< key to hash tables

	/** @copydoc CYapfBaseT::Yapf */
	Tpf &Yapf()
	{
		return *static_cast<Tpf *>(this);
	}

	/** @copydoc CYapfBaseT::PfDetectDestinationFunc */
	inline bool PfDetectDestination(Node &n)
	{
		return this->PfDetectDestination(n.GetLastTile(), n.GetLastTrackdir());
	}

	/** @copydoc CYapfBaseT::PfDetectDestinationTileFunc */
	inline bool PfDetectDestination(TileIndex tile, [[maybe_unused]] Trackdir td)
	{
		return IsRailDepotTile(tile);
	}

	/** @copydoc CYapfBaseT::PfCalcEstimateFunc */
	inline bool PfCalcEstimate(Node &n)
	{
		n.estimate = n.cost;
		return true;
	}

	inline int TeleportCost(TileIndex cur_tile, TileIndex prev_tile)
	{
		return 0;
	}
};

template <class Types>
class CYapfDestinationAnySafeTileRailT : public CYapfDestinationRailBase {
public:
	typedef typename Types::Tpf Tpf; ///< the pathfinder class (derived from THIS class)
	typedef typename Types::NodeList::Item Node; ///< this will be our node type
	typedef typename Node::Key Key; ///< key to hash tables
	typedef typename Types::TrackFollower TrackFollower; ///< TrackFollower. Need to typedef for gcc 2.95

	/** @copydoc CYapfBaseT::Yapf */
	Tpf &Yapf()
	{
		return *static_cast<Tpf *>(this);
	}

	/** @copydoc CYapfBaseT::PfDetectDestinationFunc */
	inline bool PfDetectDestination(Node &n)
	{
		return this->PfDetectDestination(n.GetLastTile(), n.GetLastTrackdir());
	}

	/** @copydoc CYapfBaseT::PfDetectDestinationTileFunc */
	inline bool PfDetectDestination(TileIndex tile, Trackdir td)
	{
		return IsSafeWaitingPosition(Yapf().GetVehicle(), tile, td, true, !TrackFollower::Allow90degTurns()) &&
				IsWaitingPositionFree(Yapf().GetVehicle(), tile, td, !TrackFollower::Allow90degTurns());
	}

	/** @copydoc CYapfBaseT::PfCalcEstimateFunc */
	inline bool PfCalcEstimate(Node &n)
	{
		n.estimate = n.cost;
		return true;
	}

	inline int TeleportCost(TileIndex cur_tile, TileIndex prev_tile)
	{
		return 0;
	}
};

template <class Types>
class CYapfDestinationTileOrStationRailT : public CYapfDestinationRailBase {
public:
	typedef typename Types::Tpf Tpf; ///< the pathfinder class (derived from THIS class)
	typedef typename Types::NodeList::Item Node; ///< this will be our node type
	typedef typename Node::Key Key; ///< key to hash tables

protected:
	TileIndex dest_tile;
	TrackdirBits dest_trackdirs;
	StationID dest_station_id;
	bool any_depot;
	bool couple_dest = false; ///< goto-couple order with a plain-track partner: detect the partner anywhere in the segment
	bool couple_station_dest = false; ///< goto-couple order with a station partner: only the partner's own platform counts as the destination

	/** @copydoc CYapfBaseT::Yapf */
	Tpf &Yapf()
	{
		return *static_cast<Tpf *>(this);
	}

public:
	typedef typename Types::TrackFollower TrackFollower; ///< track follower, needed for node tile iteration

	void SetDestination(const Train *v)
	{
		this->any_depot = false;
		this->couple_station_dest = false;
		switch (v->current_order.GetType()) {
			case OT_GOTO_WAYPOINT:
				if (!Waypoint::Get(v->current_order.GetDestination().ToStationID())->IsSingleTile()) {
					/* In case of 'complex' waypoints we need to do a look
					 * ahead. This look ahead messes a bit about, which
					 * means that it 'corrupts' the cache. To prevent this
					 * we disable caching when we're looking for a complex
					 * waypoint. */
					Yapf().DisableCache(true);
				}
				[[fallthrough]];

			case OT_GOTO_STATION:
				this->dest_tile = CalcClosestStationTile(v->current_order.GetDestination().ToStationID(), v->GetMovingFront()->tile, v->current_order.IsType(OT_GOTO_STATION) ? StationType::Rail : StationType::RailWaypoint);
				this->dest_station_id = v->current_order.GetDestination().ToStationID();
				this->dest_trackdirs = INVALID_TRACKDIR_BIT;
				break;

			case OT_GOTO_COUPLE: {
				/* The destination is the claimed partner's position. When the
				 * partner stands on a station, only its own continuous
				 * platform strip counts as the destination — another platform
				 * of the same station (compatible but a separate strip) must
				 * not be accepted; this is checked in PfDetectDestination via
				 * IsCouplePartnerTile. On plain track the partner is detected
				 * anywhere in the segment. */
				this->dest_tile = (v->dest_tile == INVALID_TILE) ? TileIndex{} : v->dest_tile;
				const Train *tgt = Train::GetIfValid(v->Primary()->couple_target);
				if (tgt != nullptr && IsRailStationTile(tgt->tile)) {
					this->dest_station_id = GetStationIndex(tgt->tile);
					this->dest_trackdirs = INVALID_TRACKDIR_BIT;
					this->couple_dest = false;
					this->couple_station_dest = true;
				} else {
					this->dest_station_id = StationID::Invalid();
					this->dest_trackdirs = GetTileTrackdirBits(this->dest_tile, TRANSPORT_RAIL, 0);
					this->couple_dest = true;
				}
				break;
			}

			case OT_GOTO_DEPOT:
				if (v->current_order.GetDepotActionType() & ODATFB_NEAREST_DEPOT) {
					this->any_depot = true;
				}
				[[fallthrough]];

			default:
				this->dest_tile = (v->dest_tile == INVALID_TILE) ? TileIndex{} : v->dest_tile;
				this->dest_station_id = StationID::Invalid();
				this->dest_trackdirs = GetTileTrackdirBits(this->dest_tile, TRANSPORT_RAIL, 0);
				this->couple_dest = false;
				break;
		}
		this->CYapfDestinationRailBase::SetDestination(v);
	}

	/** @copydoc CYapfBaseT::PfDetectDestinationFunc */
	inline bool PfDetectDestination(Node &n)
	{
		if (this->couple_dest) {
			/* Plain-track couple partner: it stands mid-block, so the
			 * destination must be detected on any tile of the segment. */
			const Train *v = Yapf().GetVehicle();
			bool found = false;
			n.template IterateTiles<CYapfDestinationTileOrStationRailT<Types>>(v, Yapf(), [&](TileIndex tile, Trackdir) {
				if (IsCouplePartnerVehicleTile(v, tile)) {
					found = true;
					return false;
				}
				return true;
			});
			return found;
		}
		return this->PfDetectDestination(n.GetLastTile(), n.GetLastTrackdir());
	}

	/** @copydoc CYapfBaseT::PfDetectDestinationTileFunc */
	inline bool PfDetectDestination(TileIndex tile, Trackdir td)
	{
		if (this->dest_station_id != StationID::Invalid()) {
			if (!HasStationTileRail(tile) || GetStationIndex(tile) != this->dest_station_id) return false;
			if (GetRailStationTrack(tile) != TrackdirToTrack(td)) return false;
			/* For a goto-couple order only the partner's own continuous
			 * platform strip is the destination; another platform of the same
			 * station passes the checks above but is a separate strip. */
			if (this->couple_station_dest) return IsCouplePartnerTile(Yapf().GetVehicle(), tile);
			return true;
		}

		if (this->any_depot) {
			return IsRailDepotTile(tile);
		}

		return (tile == this->dest_tile) && HasTrackdir(this->dest_trackdirs, td);
	}

	/** @copydoc CYapfBaseT::PfCalcEstimateFunc */
	inline bool PfCalcEstimate(Node &n)
	{
		/* The cheap tile-based check only: the segment-scanning couple
		 * detection runs when the node is evaluated as the best node. */
		if (this->PfDetectDestination(n.GetLastTile(), n.GetLastTrackdir())) {
			n.estimate = n.cost;
			return true;
		}

		n.estimate = n.cost + OctileDistanceCost(n.GetLastTile(), n.GetLastTrackdir(), this->dest_tile);
		assert(n.estimate >= n.parent->estimate);
		return true;
	}

	inline int TeleportCost(TileIndex cur_tile, TileIndex prev_tile)
	{
		auto calculate_distance_cost = [&](TileIndex t, int d_adjust) -> int {
			int x1 = 2 * TileX(t);
			int y1 = 2 * TileY(t);
			int x2 = 2 * TileX(this->dest_tile);
			int y2 = 2 * TileY(this->dest_tile);
			int dx = abs(x1 - x2) + d_adjust;
			int dy = abs(y1 - y2);
			int dmin = std::min(dx, dy) + d_adjust; // up to 2x track exit dir tile offsets in opposite directions
			int dxy = abs(dx - dy) + d_adjust; // "
			return dmin * YAPF_TILE_CORNER_LENGTH + (dxy - 1) * (YAPF_TILE_LENGTH / 2);
		};
		return std::max<int>(0, calculate_distance_cost(prev_tile, 8) - calculate_distance_cost(cur_tile, 0));
	}
};

template <class Types>
class CYapfDestinationTrainRailT : public CYapfDestinationRailBase {
public:
	typedef typename Types::Tpf Tpf;              ///< the pathfinder class (derived from THIS class)
	typedef typename Types::NodeList::Item Node; ///< this will be our node type
	typedef typename Node::Key Key;               ///< key to hash tables

protected:
	Order dest_order;

public:
	/** @copydoc CYapfBaseT::Yapf */
	Tpf &Yapf()
	{
		return *static_cast<Tpf *>(this);
	}

	void SetDestination(const Train *v)
	{
		dest_order.AssignOrder(v->current_order);
		this->CYapfDestinationRailBase::SetDestination(v);
	}

	/** @copydoc CYapfBaseT::PfDetectDestinationFunc */
	inline bool PfDetectDestination(Node &n)
	{
		return this->PfDetectDestination(n.GetLastTile(), n.GetLastTrackdir(), n.GetCost());
	}

	/**
	 * Coarse segment-level check used by the cost calculation. The challenger's
	 * node cost is not final yet, so only a free (or self-claimed) waiting
	 * train counts here; the exact claim comparison happens when the node is
	 * evaluated as the destination.
	 */
	inline bool PfDetectDestination(TileIndex tile, Trackdir td)
	{
		return this->PfDetectDestination(tile, td, UINT32_MAX);
	}

	/** @copydoc CYapfBaseT::PfDetectDestinationTileFunc */
	inline bool PfDetectDestination(TileIndex tile, Trackdir td, uint32_t claim_cost)
	{
		if (IsRailStationTile(tile)) {
			/* Station tile: search the entire platform for a waiting train
			 * directly, without requiring a reservation. Cross-company trains
			 * that arrived via block signals do not leave reservations.
			 * All order/permission/validity checks live in the shared resolver.
			 * A waiting train already claimed by another approaching consist is
			 * not a destination unless this consist's path is cheaper. */
			return ResolveCoupleTargetStation(Yapf().GetVehicle(), tile, td, true, claim_cost) != nullptr;
		}

		/* Non-station tiles: require reservation as usual. */
		TrackdirBits tdb = TrackdirToTrackdirBits(td);
		if (!HasReservedTracks(tile, TrackdirBitsToTrackBits(tdb))) return false;
		Train *t = GetTrainForReservation(tile, TrackdirToTrack(td));
		if (t == nullptr) return false;
		return ValidateCoupleCandidate(Yapf().GetVehicle(), t->First(), tile, true, claim_cost) != nullptr;
	}

	/** @copydoc CYapfBaseT::PfCalcEstimateFunc */
	inline bool PfCalcEstimate(Node &n)
	{
		n.estimate = n.cost;
		return true;
	}

	inline int TeleportCost(TileIndex cur_tile, TileIndex prev_tile)
	{
		return 0;
	}
};

#endif /* YAPF_DESTRAIL_HPP */
