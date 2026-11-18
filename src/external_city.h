/* SPDX-License-Identifier: GPL-2.0-only */
/** @file external_city.h Map-independent external destination registry. */
#ifndef EXTERNAL_CITY_H
#define EXTERNAL_CITY_H
#include "external_city_type.h"
#include "core/geometry_type.hpp"
#include "core/pool_type.hpp"
#include "core/tinystring_type.hpp"
#include "company_type.h"
#include "depot_type.h"
#include "direction_type.h"
#include "tile_type.h"
#include "rail_type.h"
#include "road_type.h"
#include "cargo_type.h"
#include "station_type.h"
#include "industry_type.h"
struct ExternalCity;
using ExternalCityPool = Pool<ExternalCity, ExternalCityID, 16>;
extern ExternalCityPool _external_city_pool;
static constexpr size_t EXTERNAL_CITY_INDUSTRIES = 3;
static constexpr size_t ExternalIndustryCargoIndex(size_t industry, CargoType cargo) { return industry * NUM_CARGO + uint(cargo); }
struct ExternalCity : ExternalCityPool::PoolItem<&_external_city_pool> {
	TinyString name{};
	Owner owner = OWNER_NONE;
	uint16_t trip_minutes = 30;
	uint16_t distance_tiles = 128; ///< One-way distance from a map entrance to the city.
	uint16_t stop_minutes = 5;
	uint32_t economy_seed = 0; ///< Saved city profile seed; zero uses the deterministic legacy fallback.
	StationID market_station = StationID::Invalid();
	bool exchange_market = false; ///< Opt-in reciprocal trade; older cities keep industry mode.
	std::array<uint8_t, NUM_CARGO> trade_cargo_roles{}; ///< Fixed new-city selection: 0 unused, 1 supply, 2 demand.
	uint64_t trade_credit = 0; ///< Shared collection allowance earned by actual deliveries; loading consumes it.
	uint16_t trade_credit_fraction = 0; ///< Fractional credit numerator; denominator is the number of demanded industrial cargoes.
	std::array<uint8_t, NUM_CARGO> trade_received{}, trade_previous_supply{}; ///< Demand types delivered this/last native economy month.
	CargoArray stock{}, demand{}; ///< demand is retained only for loading older saves; never limits acceptance.
	CargoArray month_supply{}, month_demand{}, month_loaded{}, month_delivered{}; ///< month_demand is a legacy quota, no longer used.
	bool statistics_initialized = false;
	bool market_initialized = false;
	uint64_t delivered_units = 0, loaded_units = 0;
	bool industries_initialized = false;
	std::array<IndustryType, EXTERNAL_CITY_INDUSTRIES> industry_types{IT_INVALID, IT_INVALID, IT_INVALID};
	std::array<uint64_t, EXTERNAL_CITY_INDUSTRIES * NUM_CARGO> industry_inputs{}; ///< Accepted raw materials waiting for processing, without a gameplay capacity limit.
	std::array<uint32_t, EXTERNAL_CITY_INDUSTRIES * NUM_CARGO> industry_month_consumed{}, industry_month_produced{};
	std::array<uint16_t, EXTERNAL_CITY_INDUSTRIES * NUM_CARGO> industry_remainders{}; ///< Fractional output in units of 1/256.
	uint16_t industry_counter = 0; ///< Native industry-tick phase, preserved across saves and client joins.
	std::array<uint8_t, EXTERNAL_CITY_INDUSTRIES * NUM_CARGO> industry_recent_supply{}; ///< Remaining AXIS supply cycles, 0..28.
	std::array<uint8_t, EXTERNAL_CITY_INDUSTRIES> industry_intermediate{}; ///< Undistributed AXIS intermediate output, 0..7.
	std::array<uint64_t, EXTERNAL_CITY_INDUSTRIES * NUM_CARGO> industry_pending_output{}; ///< Finished goods awaiting room in the shared market.
	ExternalCity(ExternalCityID id) : PoolItemBase(id) {}
	~ExternalCity();
};
struct ExternalBoundary;
using ExternalBoundaryPool = Pool<ExternalBoundary, ExternalBoundaryID, 16>;
extern ExternalBoundaryPool _external_boundary_pool;
struct ExternalBoundary : ExternalBoundaryPool::PoolItem<&_external_boundary_pool> {
	ExternalCityID city = ExternalCityID::Invalid();
	Owner owner = OWNER_NONE;
	DiagDirection edge = DiagDirection::NE;
	uint16_t first = 0, last = 0; ///< Inclusive coordinate along this map edge.
	ExternalBoundary(ExternalBoundaryID id) : PoolItemBase(id) {}
	static void PreCleanPool();
};
void GenerateExternalCities();
void InvalidateExternalBoundaries();
uint GetExternalBoundaryLength(DiagDirection edge);
bool GetExternalBoundaryCoordinate(TileIndex tile, DiagDirection edge, uint &coordinate, bool strict = true);
ExternalCityID GetExternalCityAtBoundary(DiagDirection edge, uint coordinate);
ExternalCityID GetExternalCityForEntrance(TileIndex tile);
std::string GetExternalConnectionCityName(TileIndex tile, DiagDirection inward);
void MigrateExternalCityBoundaries();
bool ValidateExternalBoundaries();
void ShowExternalCityWindow(DepotID portal = DepotID::Invalid());
void ShowExternalEntranceWindow(TileIndex tile);
struct ExternalCityEconomy {
	std::vector<IndustryType> industries;
	CargoTypes town_produced{}, town_accepted{}, produced{}, accepted{};
	bool operator==(const ExternalCityEconomy &) const = default;
};
/** Read-only planning profile. Does not create cargo, industries or income. */
ExternalCityEconomy GetExternalCityEconomy(ExternalCityID city);
bool IsExternalCityPassengerOrMail(CargoType cargo);
std::pair<uint, uint> GetExternalCityTradeCoverage(const ExternalCity *city);
void ShowExternalCityEconomyWindow(ExternalCityID city);
void InitializeExternalCityMarket(ExternalCity *city);
void RefillExternalCityMarkets();
bool ExternalIndustryUsesRecipe(IndustryType type);
bool ExternalIndustryUsesAxisCokeRecipe(IndustryType type);
bool ExternalIndustryUsesAxisBlastRecipe(IndustryType type);
void TickExternalCityIndustries(uint production_ticks);
size_t GetExternalIndustrySlot(const ExternalCity *city, IndustryType type);
void StoreExternalCityInputs(ExternalCity *city, CargoType cargo, uint amount);
void StoreExternalCityInputs(ExternalCity *city, const CargoArray &amounts);
void EnsureExternalCityMarkets();
struct Station;
void UpdateExternalMarketAcceptance(Station *station);
struct Order;
StationID GetExternalOrderStation(const Order *order);
ExternalCityID GetExternalOrderCity(const Order &order);
TileIndex GetExternalFlightBoundary(ExternalCityID city, TileIndex from);
TileIndex GetExternalFlightGateway(const ExternalBoundary &region);
struct Viewport;
void DrawExternalFlightBeacons();
bool ClickExternalFlightBeacon(const Viewport *viewport, Point point);
struct Vehicle;
void ServiceExternalCityVehicle(Vehicle *train, bool arrival);
bool ExternalCityVehicleNeedsFullLoad(const Vehicle *train);
struct Vehicle;
std::string GetExternalTripStatus(const Vehicle *vehicle);
/** Client-local map picker; no route or reservation state is changed. */
void SetExternalReturnHighlight(TileIndex origin, Owner owner, RailTypes compatible, RoadTypes roads = RoadTypes{UINT64_MAX});
void ClearExternalReturnHighlight();
bool IsExternalReturnHighlighted(TileIndex tile);
struct Viewport;
struct DrawPixelInfo;
/** Local display overlay only; never changes city, vehicle or reservation state. */
void DrawExternalCityBoundaries(const Viewport *viewport, DrawPixelInfo *dpi);

std::string GetExternalCityTooltipAt(const Viewport *viewport, Point point);
void ChangeExternalCityOwner(Owner old_owner, Owner new_owner);
#endif
