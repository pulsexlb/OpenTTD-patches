/* This file is part of OpenTTD, licensed under the GNU GPL, version 2. */
/** @file tracerestrict_early_route.h Native, non-reserving route decisions for multi-outlet yielding areas. */
#ifndef TRACERESTRICT_EARLY_ROUTE_H
#define TRACERESTRICT_EARLY_ROUTE_H
#include "tracerestrict.h"
#include <memory>

struct YapfRailRoute;
struct AutoYieldRouteArea {
	uint16_t slot = UINT16_MAX;
	uint32_t signature = 0;
	bool registered = false;
	std::vector<std::pair<TileIndex, Trackdir>> clears;
};

/** Only an authoritative native ChooseRailTrack search may stage a decision. */
class AutoYieldNativeRouteScope {
public:
	struct State;
	AutoYieldNativeRouteScope(const Train *train, TileIndex origin, Trackdir td);
	~AutoYieldNativeRouteScope();
	void Commit(const YapfRailRoute &actual);
	bool NeedsInspection() const;
	bool NeedsSelection() const;
	bool Select(const YapfRailRoute &actual);
	/** Export only the winning detector prefix or the staged area's boundary. */
	bool ExportEnd(TileIndex tile, Trackdir td) const;
	bool RelaxForReplan();
	/** Retry one failed PBS attempt with split-only directed exclusions. */
	bool ReplanForSplit();
	void PrepareFromContext(TileIndex origin, Trackdir td);
	void Finish(bool reservation_made);
private:
	std::unique_ptr<State> state;
	State *previous;
	State *active = nullptr;
};

bool GetAutoYieldRouteArea(const Train *train, uint16_t slot, AutoYieldRouteArea &area);
bool AutoYieldRouteAreasAvailable();
void AutoYieldStageNativeRoute(const Train *train, TileIndex detector, Trackdir td, uint16_t slot, bool authoritative = false);
bool AutoYieldHasNativeRoute(const Train *train);
bool AutoYieldNativeRouteAllows(const Train *train, TileIndex tile, Trackdir td);
/** Read-only native search evidence, used only by a subsequent fresh replan. */
void AutoYieldNativeRouteDenied(const Train *train, TileIndex tile, Trackdir td);
std::optional<std::pair<TileIndex, Trackdir>> AutoYieldNativeRouteRequiredPoint(const Train *train, TileIndex origin, Trackdir td);
bool AutoYieldGetNativeRoute(const Train *train, uint16_t slot, TileIndex start, Trackdir td, YapfRailRoute &route);
/** A selected local platform may lie beyond an intermediate Main marker. */
bool AutoYieldNativeStopNeedsApproach(const Train *train, uint16_t slot, TileIndex tile, Trackdir td);
/** Record evidence for a later fresh search; never change an active A* filter. */
bool AutoYieldMayRequestSplitAlternative(const Train *train, uint16_t slot);
bool AutoYieldRequestSplitAlternative(const Train *train, uint16_t slot, const std::vector<std::pair<TileIndex, Trackdir>> &blocked);
bool AutoYieldRouteQuerySuppresses(uint16_t slot);
/** Actual track edits invalidate plans, never native PBS reservations. */
void AutoYieldInvalidateNativeRoutes(TileIndex tile);
#endif
