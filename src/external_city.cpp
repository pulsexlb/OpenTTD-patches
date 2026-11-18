/* SPDX-License-Identifier: GPL-2.0-only */
/** @file external_city.cpp Synchronized external cities and map boundary regions. */
#include "stdafx.h"
#include "external_city.h"
#include "aircraft.h"
#include "external_city_cmd.h"
#include "depot_base.h"
#include "company_base.h"
#include "company_func.h"
#include "core/pool_func.hpp"
#include "settings_type.h"
#include "string_func.h"
#include "window_func.h"
#include "gfx_func.h"
#include "cargotype.h"
#include "industrytype.h"
#include "newgrf.h"
#include "newgrf_config.h"
#include "core/random_func.hpp"
#include "core/backup_type.hpp"
#include "train.h"
#include "roadveh.h"
#include "ship.h"
#include "station_base.h"
#include "station_func.h"
#include "town.h"
#include "townname_func.h"
#include "townname_type.h"
#include "genworld.h"
#include "linkgraph/linkgraph.h"
#include "linkgraph/linkgraphschedule.h"
#include "economy_func.h"
#include "table/strings.h"
#include "safeguards.h"

ExternalCityPool _external_city_pool("ExternalCity");
ExternalBoundaryPool _external_boundary_pool("ExternalBoundary");
INSTANTIATE_POOL_METHODS(ExternalCity)
INSTANTIATE_POOL_METHODS(ExternalBoundary)

ExternalCity::~ExternalCity()
{
 if (!Station::CleaningPool()) {
  if (Station *station = Station::GetIfValid(this->market_station); station != nullptr && station->external_city == this->index.base()) delete station;
 }
}

static uint32_t MixExternalEconomySeed(uint32_t value)
{
 value ^= value >> 16; value *= 0x7FEB352DU;
 value ^= value >> 15; value *= 0x846CA68BU;
 return value ^ (value >> 16);
}
bool IsExternalCityPassengerOrMail(CargoType cargo)
{
 if (cargo >= NUM_CARGO || !CargoSpec::Get(cargo)->IsValid()) return false;
 const auto *spec = CargoSpec::Get(cargo);
 return spec->classes.Test(CargoClass::Passengers) || spec->classes.Test(CargoClass::Mail) ||
   spec->town_production_effect == TownProductionEffect::Passengers || spec->town_production_effect == TownProductionEffect::Mail;
}
ExternalCityEconomy GetExternalCityEconomy(ExternalCityID id)
{
 ExternalCityEconomy result;
 const auto *city = ExternalCity::GetIfValid(id);
 if (city == nullptr) return result;
 if (city->exchange_market) {
  for (const CargoSpec *cargo : CargoSpec::Iterate()) {
   const CargoType type = cargo->Index();
   if (IsExternalCityPassengerOrMail(type)) { result.town_produced.Set(type); result.town_accepted.Set(type); continue; }
   if (city->trade_cargo_roles[type] == 1) result.produced.Set(type);
   if (city->trade_cargo_roles[type] == 2) result.accepted.Set(type);
  }
  result.produced |= result.town_produced; result.accepted |= result.town_accepted;
  return result;
 }
 const uint32_t seed = city->economy_seed == 0 ? MixExternalEconomySeed(0xC17EC17EU + id.base()) : city->economy_seed;
 auto cargoes = [](const auto &list) {
  CargoTypes mask;
  for (CargoType cargo : list) if (cargo < NUM_CARGO && CargoSpec::Get(cargo)->IsValid()) mask.Set(cargo);
  return mask;
 };
 /* Only finalised, active specifications are used. No position-dependent GRF
  * callbacks run here: this is a city planning profile, not a map industry. */
 std::vector<std::pair<uint32_t, IndustryType>> candidates;
 if (!city->industries_initialized) for (IndustryType type = 0; type < NUM_INDUSTRYTYPES; ++type) {
  const auto *spec = GetIndustrySpec(type);
  if (!spec->enabled || (cargoes(spec->produced_cargo).None() && cargoes(spec->accepts_cargo).None())) continue;
  candidates.emplace_back(MixExternalEconomySeed(seed ^ (uint32_t(type) + 1) * 0x9E3779B9U), type);
 }
 std::sort(candidates.begin(), candidates.end());
 if (city->industries_initialized) {
  candidates.clear();
  for (size_t i = 0; i < city->industry_types.size(); ++i) {
   const IndustryType type = city->industry_types[i];
   if (type < NUM_INDUSTRYTYPES && GetIndustrySpec(type)->enabled) candidates.emplace_back(uint32_t(i), type);
  }
 }
 for (size_t i = 0; i < std::min<size_t>(EXTERNAL_CITY_INDUSTRIES, candidates.size()); ++i) {
  const auto type = candidates[i].second;
  result.industries.push_back(type);
  const auto *spec = GetIndustrySpec(type);
  result.produced |= cargoes(spec->produced_cargo);
  result.accepted |= cargoes(spec->accepts_cargo);
 }
 for (const CargoSpec *cargo : CargoSpec::Iterate()) {
  if (cargo->town_production_effect == TownProductionEffect::Passengers || cargo->town_production_effect == TownProductionEffect::Mail) result.town_produced.Set(cargo->Index());
  if (cargo->town_acceptance_effect != TownAcceptanceEffect::None) result.town_accepted.Set(cargo->Index());
 }
 result.produced |= result.town_produced;
 result.accepted |= result.town_accepted;
 return result;
}

ExternalCityID GetExternalOrderCity(const Order &order)
{
 if (order.IsExternalFlightOrder()) {
  const auto *station = Station::GetIfValid(order.GetDestination().ToStationID());
  return station == nullptr ? ExternalCityID::Invalid() : ExternalCityID{station->external_city};
 }
 const Depot *portal = order.IsExternalTripOrder() ? Depot::GetIfValid(order.GetDestination().ToDepotID()) : nullptr;
 return portal == nullptr ? ExternalCityID::Invalid() : GetExternalCityForEntrance(portal->xy);
}

/** A stable aviation gateway per boundary segment, independent of ground entrances. */
TileIndex GetExternalFlightGateway(const ExternalBoundary &region)
{
 if (region.edge >= DiagDirection::End) return INVALID_TILE;
 const uint low = std::max<uint>(1, region.first);
 const uint high = std::min<uint>(GetExternalBoundaryLength(region.edge) - 2, region.last);
 if (low > high) return INVALID_TILE;
 const uint coordinate = (low + high) / 2;
 switch (region.edge) {
  case DiagDirection::NE: return TileXY(1, coordinate);
  case DiagDirection::SW: return TileXY(Map::SizeX() - 2, coordinate);
  case DiagDirection::NW: return TileXY(coordinate, 1);
  case DiagDirection::SE: return TileXY(coordinate, Map::SizeY() - 2);
  default: return INVALID_TILE;
 }
}

TileIndex GetExternalFlightBoundary(ExternalCityID city, TileIndex from)
{
 TileIndex best = INVALID_TILE; uint distance = UINT_MAX;
 for (const auto *region : ExternalBoundary::Iterate()) {
  if (region->city != city) continue;
  const TileIndex tile = GetExternalFlightGateway(*region);
  if (tile == INVALID_TILE) continue;
  const uint score = DistanceSquare(from, tile);
  if (score < distance || (score == distance && tile < best)) { best = tile; distance = score; }
 }
 return best;
}

StationID GetExternalOrderStation(const Order *order)
{
 if (order == nullptr || !order->IsExternalTripOrder()) return StationID::Invalid();
 const ExternalCity *city = ExternalCity::GetIfValid(GetExternalOrderCity(*order));
 const Station *station = city == nullptr ? nullptr : Station::GetIfValid(city->market_station);
 return station != nullptr && station->external_city == city->index.base() ? station->index : StationID::Invalid();
}
void UpdateExternalMarketAcceptance(Station *station)
{
 const auto *city = ExternalCity::GetIfValid(ExternalCityID{station->external_city});
 const CargoTypes accepted = city == nullptr ? CargoTypes{} : GetExternalCityEconomy(city->index).accepted;
 for (CargoType cargo : EnumRange(NUM_CARGO)) {
  auto &ge = station->goods[cargo];
  const bool accepts = accepted.Test(cargo);
  ge.status.Set(GoodsEntry::State::Acceptance, accepts);
  if (LinkGraph::IsValidID(ge.link_graph)) (*LinkGraph::Get(ge.link_graph))[ge.node].SetDemand(accepts ? 1 : 0);
 }
}
static uint AddExternalSupply(ExternalCity *city, Station *station, CargoType cargo, uint amount)
{
 if (amount == 0 || !CargoPacket::CanAllocateItem()) return 0;
 auto &ge = station->goods[cargo];
 auto *packet = CargoPacket::Create(amount, uint16_t(0), station->index, INVALID_TILE, Money(0));
 packet->external_source_city = city->index.base(); packet->external_distance = city->distance_tiles;
 ge.CreateData().cargo.Append(packet, ge.GetVia(station->index));
 if (LinkGraph::IsValidID(ge.link_graph)) (*LinkGraph::Get(ge.link_graph))[ge.node].UpdateSupply(amount);
 return amount;
}
void EnsureExternalCityMarkets()
{
 for (auto *city : ExternalCity::Iterate()) {
  if (Station *station = Station::GetIfValid(city->market_station); station != nullptr && station->external_city == city->index.base()) {
   station->name = city->name; station->owner = city->owner; UpdateExternalMarketAcceptance(station); continue;
  }
  TileIndex anchor = INVALID_TILE;
  for (const auto *region : ExternalBoundary::Iterate()) if (region->city == city->index) {
   const uint low = _settings_game.construction.freeform_edges ? 1 : 0;
   const uint middle = (uint(region->first) + region->last) / 2;
   switch (region->edge) {
    case DiagDirection::NE: anchor = TileXY(low, middle); break;
    case DiagDirection::SW: anchor = TileXY(Map::SizeX() - 2, middle); break;
    case DiagDirection::NW: anchor = TileXY(middle, low); break;
    case DiagDirection::SE: anchor = TileXY(middle, Map::SizeY() - 2); break;
    default: break;
   }
   break;
  }
  if (!IsValidTile(anchor) || !Station::CanAllocateItem()) continue;
  Station *station = Station::Create(anchor);
  station->external_city = city->index.base(); station->owner = city->owner; station->name = city->name;
  station->town = ClosestTownFromTile(anchor, UINT_MAX);
  city->market_station = station->index;
  const auto profile = GetExternalCityEconomy(city->index);
  UpdateExternalMarketAcceptance(station);
  for (CargoType cargo : profile.produced | profile.accepted) {
   auto &ge = station->goods[cargo];
   if (LinkGraph::CanAllocateItem()) {
    auto *graph = LinkGraph::Create(cargo); ge.link_graph = graph->index; ge.node = graph->AddNode(station);
    LinkGraphSchedule::instance.Queue(graph);
   }
   if (profile.produced.Test(cargo)) city->stock[cargo] = AddExternalSupply(city, station, cargo, city->stock[cargo]);
  }
 }
}

/** Verified AXIS 2.3.1 coke-oven ratio adapter for the three exposed economies, not a GRF callback emulator.
 * Fail closed for other binaries, economies and cargo layouts. No GUI-local cache:
 * loading/joining clients derive the same result from the active GRF configuration. */
static const GRFConfig *VerifiedAxisIndustry(IndustryType type)
{
 if (type >= NUM_INDUSTRYTYPES) return nullptr;
 const auto *spec = GetIndustrySpec(type);
 if (!spec->enabled || spec->grf_prop.grffile == nullptr) return nullptr;
 const auto *config = GetGRFConfig(spec->grf_prop.grffile->grfid);
 static constexpr std::array<uint8_t, 16> checksum{0xB5, 0x80, 0xAE, 0x20, 0xD1, 0x22, 0x47, 0xF3, 0xA7, 0xED, 0xD2, 0x8F, 0xAD, 0xC6, 0xF2, 0xE5};
 if (config == nullptr || config->ident.grfid != GrfID{"\x4A\x4B\x88\x08"} || config->status != GRFStatus::Activated ||
   config->flags.Test(GRFConfigFlag::Compatible) || !std::ranges::equal(config->ident.md5sum, checksum) ||
   (!config->param.empty() && config->param[0] > 2)) return nullptr;
 return config;
}
static bool ExternalCargoesMatch(const auto &cargoes, std::initializer_list<CargoLabel> labels)
{
  CargoTypes expected, actual;
  for (auto label : labels) { const auto cargo = GetCargoTypeByLabel(label); if (cargo >= NUM_CARGO) return false; expected.Set(cargo); }
  for (CargoType cargo : cargoes) if (cargo < NUM_CARGO && CargoSpec::Get(cargo)->IsValid()) actual.Set(cargo);
  return actual == expected;
}
bool ExternalIndustryUsesAxisCokeRecipe(IndustryType type)
{
 const auto *config = VerifiedAxisIndustry(type);
 if (config == nullptr) return false;
 const auto *spec = GetIndustrySpec(type);
 if (spec->grf_prop.local_id != 78) return false;
 const uint32_t economy = config->param.empty() ? 0 : config->param[0];
 return ExternalCargoesMatch(spec->accepts_cargo, {CT_COAL}) && (economy == 1 ?
   ExternalCargoesMatch(spec->produced_cargo, {CargoLabel{"COKE"}, CargoLabel{"CTAR"}, CargoLabel{"SULP"}}) :
   ExternalCargoesMatch(spec->produced_cargo, {CargoLabel{"COKE"}, CargoLabel{"SULP"}}));
}
bool ExternalIndustryUsesAxisBlastRecipe(IndustryType type)
{
 if (VerifiedAxisIndustry(type) == nullptr) return false;
 const auto *spec = GetIndustrySpec(type);
 return ExternalCargoesMatch(spec->accepts_cargo, {CT_IRON_ORE, CargoLabel{"COKE"}, CargoLabel{"LIME"}}) &&
   ExternalCargoesMatch(spec->produced_cargo, {CargoLabel{"IRON"}, CargoLabel{"SLAG"}});
}

bool ExternalIndustryUsesRecipe(IndustryType type)
{
 if (ExternalIndustryUsesAxisCokeRecipe(type) || ExternalIndustryUsesAxisBlastRecipe(type)) return true;
 if (type >= NUM_INDUSTRYTYPES) return false;
 const auto *spec = GetIndustrySpec(type);
 /* Use the active definition rather than a GRF name or original industry ID.
  * Presentation callbacks do not change fixed cargo/production properties.
  * Everything else requires an actual industry context (including persistent
  * storage and map position), which a city market cannot safely impersonate. */
 constexpr IndustryCallbackMasks presentation{
  IndustryCallbackMask::CargoSuffix, IndustryCallbackMask::FundMoreText,
  IndustryCallbackMask::WindowMoreText, IndustryCallbackMask::SpecialEffect,
  IndustryCallbackMask::DecideColour};
 return spec->enabled && (spec->callback_mask & ~presentation).None();
}
size_t GetExternalIndustrySlot(const ExternalCity *city, IndustryType type)
{
 const auto it = std::ranges::find(city->industry_types, type);
 return size_t(it - city->industry_types.begin());
}
static void DeliverExternalBlastInput(ExternalCity *city, size_t slot, const CargoArray &amounts);
void StoreExternalCityInputs(ExternalCity *city, CargoType cargo, uint amount)
{
 CargoArray amounts{}; amounts[cargo] = amount;
 StoreExternalCityInputs(city, amounts);
}
std::pair<uint, uint> GetExternalCityTradeCoverage(const ExternalCity *city)
{
 uint supplied = 0, required = 0;
 for (CargoType cargo : GetExternalCityEconomy(city->index).accepted) {
  if (IsExternalCityPassengerOrMail(cargo)) continue;
  ++required; supplied += city->trade_previous_supply[cargo] != 0;
 }
 return {supplied, required};
}
void StoreExternalCityInputs(ExternalCity *city, const CargoArray &amounts)
{
 if (city->exchange_market) {
  const auto [supplied, required] = GetExternalCityTradeCoverage(city);
  if (required == 0) return;
  uint64_t delivered = 0;
  for (CargoType cargo : GetExternalCityEconomy(city->index).accepted) {
   if (IsExternalCityPassengerOrMail(cargo)) continue;
   if (amounts[cargo] != 0) city->trade_received[cargo] = 1;
   delivered += amounts[cargo];
  }
  /* Last completed economy month fixes this month's rate. No cargo pairing:
   * any demanded freight earns the same shared allowance for every supply.
   * A minimum rate keeps first deliveries useful; retain fractional units. */
  const uint64_t numerator = delivered * std::max<uint>(1, supplied) + city->trade_credit_fraction;
  city->trade_credit += std::min<uint64_t>(numerator / required, UINT64_MAX - city->trade_credit);
  city->trade_credit_fraction = numerator % required;
  return;
 }
 /* Receipt is independent of production speed or finished-goods capacity.
  * Share each delivery between eligible recipes in stable slot order; retain
  * all raw materials until processing. Town/abstract-market cargo is consumed. */
 std::array<CargoArray, EXTERNAL_CITY_INDUSTRIES> deliveries{};
 for (CargoType cargo : EnumRange(NUM_CARGO)) {
 const uint amount = amounts[cargo];
 if (amount == 0) continue;
 std::array<size_t, EXTERNAL_CITY_INDUSTRIES> receivers{};
 size_t count = 0;
 for (size_t slot = 0; slot < EXTERNAL_CITY_INDUSTRIES; ++slot) {
  const IndustryType type = city->industry_types[slot];
  if (!ExternalIndustryUsesRecipe(type)) continue;
  const auto *spec = GetIndustrySpec(type);
  if (std::ranges::find(spec->accepts_cargo, cargo) == spec->accepts_cargo.end()) continue;
  receivers[count++] = slot;
 }
 for (size_t i = 0; i < count; ++i) {
  const size_t slot = receivers[i];
  const uint share = amount / count + (i < amount % count);
  if (ExternalIndustryUsesAxisBlastRecipe(city->industry_types[slot])) deliveries[slot][cargo] = share;
  else city->industry_inputs[ExternalIndustryCargoIndex(slot, cargo)] += share;
 }
 }
 for (size_t slot = 0; slot < EXTERNAL_CITY_INDUSTRIES; ++slot) {
  if (ExternalIndustryUsesAxisBlastRecipe(city->industry_types[slot])) DeliverExternalBlastInput(city, slot, deliveries[slot]);
 }
}
static CargoTypes ExternalAbstractSupply(const ExternalCityEconomy &profile)
{
 CargoTypes result = profile.town_produced;
 for (IndustryType type : profile.industries) if (!ExternalIndustryUsesRecipe(type)) {
  for (CargoType cargo : GetIndustrySpec(type)->produced_cargo) if (cargo < NUM_CARGO && CargoSpec::Get(cargo)->IsValid()) result.Set(cargo);
 }
 return result;
}
static void ConfigureExternalCityEconomy(ExternalCity *city)
{
 if (_settings_game.vehicle.external_economy_mode != 1) return;
 city->exchange_market = true;
 /* Private, seed-derived ordering: neither map RNG nor local GUI state is
  * consumed. Persist cargo roles, never reroll on load or a client join. */
 std::vector<std::pair<uint32_t, CargoType>> cargoes;
 for (const CargoSpec *cargo : CargoSpec::Iterate()) {
  if (IsExternalCityPassengerOrMail(cargo->Index())) continue;
  cargoes.emplace_back(MixExternalEconomySeed(city->economy_seed ^ (uint(cargo->Index()) + 1) * 0x9E3779B9U), cargo->Index());
 }
 std::sort(cargoes.begin(), cargoes.end());
 if (cargoes.size() < 2) return;
 const size_t supplies = std::min<size_t>(3, cargoes.size() / 2);
 const size_t demands = std::min<size_t>(3, cargoes.size() - supplies);
 for (size_t n = 0; n < supplies; ++n) city->trade_cargo_roles[cargoes[n].second] = 1;
 for (size_t n = supplies; n < supplies + demands; ++n) city->trade_cargo_roles[cargoes[n].second] = 2;
}
void InitializeExternalCityMarket(ExternalCity *city)
{
 if (!city->industries_initialized) {
  const auto profile = GetExternalCityEconomy(city->index);
  city->industry_types.fill(IT_INVALID);
  std::copy(profile.industries.begin(), profile.industries.end(), city->industry_types.begin());
  city->industries_initialized = true;
 }
 if (city->market_initialized) {
  if (!city->statistics_initialized) { city->month_supply = city->stock; city->month_demand = city->demand; city->statistics_initialized = true; }
  return; // Existing saves retain every unit; never invent past inputs or production.
 }
 const auto profile = GetExternalCityEconomy(city->index);
 for (CargoType cargo : city->exchange_market ? profile.produced : ExternalAbstractSupply(profile)) city->stock[cargo] = 512;
 city->market_initialized = true;
 city->month_supply = city->stock; city->month_demand = city->demand; city->statistics_initialized = true;
}
/** New-game destinations share the selected native/GRF town naming scheme.
 * A private RNG keeps map generation unchanged. Names and profiles are saved,
 * so joining clients never regenerate them from local settings. */
void GenerateExternalCities()
{
 if (_game_mode != GameMode::Normal || !_generating_world || !_settings_game.vehicle.external_connections ||
     !ExternalCity::Iterate().empty() || !ExternalBoundary::Iterate().empty()) return;
 Randomizer random;
 random.SetSeed(MixExternalEconomySeed(_settings_game.game_creation.generation_seed ^ 0x45585443U));
 TownNameParams params(_settings_game.game_creation.town_name);
 TownNames used;
 for (const Town *town : Town::Iterate()) used.insert(GetTownName(town));
 for (uint edge = 0; edge < 4; ++edge) {
  if (!ExternalCity::CanAllocateItem() || !ExternalBoundary::CanAllocateItem()) break;
  uint32_t parts;
  std::string name;
  if (GenerateTownName(random, &parts, &used)) {
   name = GetTownName(&params, parts);
  } else {
   /* Small finite NewGRF name lists can be exhausted by on-map towns.
    * Keep their naming scheme, disambiguating rather than omitting an edge. */
   name = GetTownName(&params, random.Next());
   const std::string base = name;
   for (uint suffix = 2; used.contains(name); ++suffix) name = fmt::format("{} {}", base, suffix);
   used.insert(name);
  }
  ExternalCity *city = ExternalCity::Create();
  city->name = name; city->owner = OWNER_NONE;
  city->trip_minutes = _settings_game.vehicle.external_trip_minutes;
  city->distance_tiles = std::max<uint>(1, _settings_game.vehicle.external_trip_distance);
  city->stop_minutes = _settings_game.vehicle.external_stop_minutes;
  city->economy_seed = std::max<uint32_t>(1, random.Next());
  ConfigureExternalCityEconomy(city);
  InitializeExternalCityMarket(city);
  ExternalBoundary *region = ExternalBoundary::Create();
  region->city = city->index; region->owner = OWNER_NONE; region->edge = DiagDirection(edge);
  region->first = _settings_game.construction.freeform_edges ? 1 : 0;
  region->last = GetExternalBoundaryLength(region->edge) - 2;
 }
 InvalidateExternalBoundaries();
 EnsureExternalCityMarkets();
}

static uint PublishExternalProduction(ExternalCity *city, Station *station, CargoType cargo, uint amount)
{
 amount = std::min<uint>(amount, 4096 - std::min<uint>(4096, city->stock[cargo]));
 const uint added = station == nullptr ? amount : AddExternalSupply(city, station, cargo, amount);
 city->stock[cargo] += added; city->month_supply[cargo] += added;
 return added;
}
static void FlushExternalBlastOutput(ExternalCity *city, Station *station, size_t slot)
{
 for (CargoType cargo : GetIndustrySpec(city->industry_types[slot])->produced_cargo) {
  if (cargo >= NUM_CARGO) continue;
  auto &pending = city->industry_pending_output[ExternalIndustryCargoIndex(slot, cargo)];
  if (pending == 0) continue;
  const uint added = PublishExternalProduction(city, station, cargo, uint(std::min<uint64_t>(4096, pending)));
  pending -= added;
 }
}
/** AXIS 2.3.1 blast-furnace arrival arithmetic and recent-supply history.
 * This is a bounded adapter, not a synthetic Industry or a GRF interpreter.
 * Preserve native per-delivery rounding and the >=8 intermediate threshold.
 * Retain finished goods if the market is full; accepting a delivery is never
 * conditional on available space or CargoDist's current routing decision. */
static void DeliverExternalBlastInput(ExternalCity *city, size_t slot, const CargoArray &amounts)
{
 bool delivered = false;
 for (CargoType cargo : EnumRange(NUM_CARGO)) if (amounts[cargo] != 0) {
  city->industry_recent_supply[ExternalIndustryCargoIndex(slot, cargo)] = 28;
  city->industry_month_consumed[ExternalIndustryCargoIndex(slot, cargo)] += amounts[cargo];
  delivered = true;
 }
 if (!delivered) return;
 const auto *config = VerifiedAxisIndustry(city->industry_types[slot]);
 assert(config != nullptr);
 const bool steeltown = !config->param.empty() && config->param[0] == 1;
 uint ratio = 0;
 for (auto [label, weight] : {std::pair{CT_IRON_ORE, steeltown ? 3U : 4U},
     std::pair{CargoLabel{"COKE"}, 3U}, std::pair{CargoLabel{"LIME"}, 2U}}) {
  if (city->industry_recent_supply[ExternalIndustryCargoIndex(slot, GetCargoTypeByLabel(label))] != 0) ratio += weight;
 }
 /* Native station unloading collects all delivered cargoes before invoking
  * production. A mixed consist must establish every supplied input first. */
 uint64_t intermediate = city->industry_intermediate[slot];
 for (CargoType cargo : EnumRange(NUM_CARGO)) intermediate += uint64_t(amounts[cargo]) * ratio / 8;
 city->industry_intermediate[slot] = intermediate < 8 ? uint8_t(intermediate) : 0;
 if (intermediate >= 8) {
  for (auto [label, weight] : {std::pair{CargoLabel{"IRON"}, 6U}, std::pair{CargoLabel{"SLAG"}, 2U}}) {
   const size_t index = ExternalIndustryCargoIndex(slot, GetCargoTypeByLabel(label));
   const uint64_t produced = intermediate * weight / 8;
   city->industry_pending_output[index] += produced;
   city->industry_month_produced[index] += produced;
  }
 }
 FlushExternalBlastOutput(city, Station::GetIfValid(city->market_station), slot);
}
void TickExternalCityIndustries(uint production_ticks)
{
 assert(production_ticks != 0);
 for (auto *city : ExternalCity::Iterate()) {
  --city->industry_counter;
  if (city->industry_counter % production_ticks != 0) continue;
  for (size_t slot = 0; slot < EXTERNAL_CITY_INDUSTRIES; ++slot) {
   if (!ExternalIndustryUsesAxisBlastRecipe(city->industry_types[slot])) continue;
   for (CargoType cargo : GetIndustrySpec(city->industry_types[slot])->accepts_cargo) {
    if (cargo >= NUM_CARGO) continue;
    auto &remaining = city->industry_recent_supply[ExternalIndustryCargoIndex(slot, cargo)];
    if (remaining != 0) --remaining;
   }
   FlushExternalBlastOutput(city, Station::GetIfValid(city->market_station), slot);
  }
 }
}
static void ProduceExternalIndustry(ExternalCity *city, Station *station, size_t slot)
{
 const IndustryType type = city->industry_types[slot];
 if (!ExternalIndustryUsesRecipe(type)) return;
 if (ExternalIndustryUsesAxisBlastRecipe(type)) {
  FlushExternalBlastOutput(city, station, slot);
  return; // Arrival-driven; the monthly refresh must not invent a delivery.
 }
 if (ExternalIndustryUsesAxisCokeRecipe(type)) {
  /* Steeltown: 8 coal -> 6 coke + 1 coal tar + 1 sulphur; other verified
   * economies: 8 coal -> 6 coke + 2 sulphur. Process complete batches,
   * retaining small deliveries and blocked inputs rather than losing them.
   * The off-map monthly cadence/cap is deliberate; no on-map callback timing,
   * closure or persistent-storage side effects are impersonated here. */
  const CargoType coal = GetCargoTypeByLabel(CT_COAL);
  const auto *spec = GetIndustrySpec(type);
  const CargoType tar = GetCargoTypeByLabel(CargoLabel{"CTAR"});
  const bool has_tar = tar < NUM_CARGO && std::ranges::find(spec->produced_cargo, tar) != spec->produced_cargo.end();
  std::vector<CargoType> products{GetCargoTypeByLabel(CargoLabel{"COKE"}), GetCargoTypeByLabel(CargoLabel{"SULP"})};
  std::vector<uint> ratios{6, has_tar ? 1U : 2U};
  if (has_tar) { products.push_back(tar); ratios.push_back(1); }
  auto &stored = city->industry_inputs[ExternalIndustryCargoIndex(slot, coal)];
  uint batches = uint(std::min<uint64_t>(32, stored / 8));
  for (size_t i = 0; i < products.size(); ++i) batches = std::min(batches, (4096 - std::min<uint>(4096, city->stock[products[i]])) / ratios[i]);
  if (batches == 0 || (station != nullptr && !CargoPacket::CanAllocateItem(products.size()))) return;
  for (size_t i = 0; i < products.size(); ++i) {
   const uint amount = batches * ratios[i];
   const uint added = PublishExternalProduction(city, station, products[i], amount);
   assert(added == amount);
   city->industry_month_produced[ExternalIndustryCargoIndex(slot, products[i])] += added;
  }
  stored -= batches * 8;
  city->industry_month_consumed[ExternalIndustryCargoIndex(slot, coal)] += batches * 8;
  return;
 }
 const auto *spec = GetIndustrySpec(type);
 for (size_t output = 0; output < spec->produced_cargo.size(); ++output) {
  const CargoType cargo = spec->produced_cargo[output];
  if (cargo >= NUM_CARGO || !CargoSpec::Get(cargo)->IsValid()) continue;
  /* Off-map monthly cadence: eight native production batches. Terrain-only
   * lumbermills use their original 45-unit tree-cut batch, without map edits. */
  uint amount = spec->production_rate[output] * 8U;
  if (output == 0 && spec->behaviour.Test(IndustryBehaviour::CutTrees)) amount += 45U * 8;
  city->industry_month_produced[ExternalIndustryCargoIndex(slot, cargo)] += PublishExternalProduction(city, station, cargo, amount);
 }
 for (size_t input = 0; input < spec->accepts_cargo.size(); ++input) {
  const CargoType cargo = spec->accepts_cargo[input];
  if (cargo >= NUM_CARGO || !CargoSpec::Get(cargo)->IsValid()) continue;
  auto &stored = city->industry_inputs[ExternalIndustryCargoIndex(slot, cargo)];
  if (stored == 0) continue;
  CargoArray multipliers{};
  for (size_t output = 0; output < spec->produced_cargo.size(); ++output) {
   const CargoType product = spec->produced_cargo[output];
   if (product < NUM_CARGO && CargoSpec::Get(product)->IsValid()) multipliers[product] += spec->input_cargo_multiplier[input][output];
  }
  auto fits = [&](uint amount) {
   uint packets = 0;
   for (CargoType product : EnumRange(NUM_CARGO)) if (multipliers[product] != 0) {
    const uint count = (uint64_t(amount) * multipliers[product] + city->industry_remainders[ExternalIndustryCargoIndex(slot, product)]) / 256;
    if (count > 4096 - std::min<uint>(4096, city->stock[product])) return false;
    packets += count != 0;
   }
   return station == nullptr || CargoPacket::CanAllocateItem(packets);
  };
  uint low = 0, high = uint(std::min<uint64_t>(256, stored));
  while (low < high) {
   const uint middle = (low + high + 1) / 2;
   if (fits(middle)) low = middle; else high = middle - 1;
  }
  if (low == 0) continue; // Finished-goods capacity unavailable: retain the raw material.
  for (CargoType product : EnumRange(NUM_CARGO)) if (multipliers[product] != 0) {
   const size_t index = ExternalIndustryCargoIndex(slot, product);
   const uint64_t scaled = uint64_t(low) * multipliers[product] + city->industry_remainders[index];
   const uint count = uint(scaled / 256);
   const uint added = PublishExternalProduction(city, station, product, count);
   assert(added == count); // All output space and packet slots were checked before consuming inputs.
   city->industry_month_produced[index] += added;
   city->industry_remainders[index] = uint16_t(scaled % 256);
  }
  stored -= low;
  city->industry_month_consumed[ExternalIndustryCargoIndex(slot, cargo)] += low;
 }
}
void RefillExternalCityMarkets()
{
 for (auto *city : ExternalCity::Iterate()) {
  InitializeExternalCityMarket(city);
  if (city->exchange_market) {
   city->trade_previous_supply = city->trade_received;
   city->trade_received.fill(0);
  }
  const auto profile = GetExternalCityEconomy(city->index);
  Station *station = Station::GetIfValid(city->market_station);
  for (CargoType cargo : EnumRange(NUM_CARGO)) {
   if (station != nullptr) {
    const auto &ge = station->goods[cargo];
    city->stock[cargo] = ge.data == nullptr ? 0 : ge.data->cargo.AvailableCount();
   } else if (!profile.produced.Test(cargo)) {
    city->stock[cargo] = 0;
   }
  }
  city->month_supply = city->stock;
  city->month_loaded = {}; city->month_delivered = {};
  city->industry_month_consumed.fill(0); city->industry_month_produced.fill(0);
  for (CargoType cargo : city->exchange_market ? profile.produced : ExternalAbstractSupply(profile)) PublishExternalProduction(city, station, cargo, 256);
  for (size_t slot = 0; slot < EXTERNAL_CITY_INDUSTRIES; ++slot) ProduceExternalIndustry(city, station, slot);
 }
 EnsureExternalCityMarkets();
 InvalidateWindowClassesData(WindowClass::ExternalCityEconomy);
}
void ServiceExternalCityVehicle(Vehicle *train, bool arrival)
{
 auto *city = ExternalCity::GetIfValid(train->external_trip_city);
 if (city == nullptr || train->cargo_payment != nullptr) return;
 InitializeExternalCityMarket(city);
 Station *station = Station::GetIfValid(city->market_station);
 if (station == nullptr) return;
 CargoStationIDVectorSet next_stations;
 bool routing_ready = false;
 if (!arrival && train->current_order.IsRefit()) {
  AutoRestoreBackup last_station(train->last_station_visited, station->index);
  next_stations = train->GetNextStoppingStation(); routing_ready = true;
  RefitExternalCityVehicle(train, station, next_stations);
 }
 const auto profile = GetExternalCityEconomy(city->index);
 Money income = 0;
 CargoArray delivered{};
 bool changed = false;
 for (Vehicle *part = train->First(); part != nullptr; part = part->Next()) {
  const CargoType cargo = part->cargo_type;
  if (cargo >= NUM_CARGO || !CargoSpec::Get(cargo)->IsValid() || part->cargo.ReservedCount() != 0) continue;
  if (arrival) {
   if (!profile.accepted.Test(cargo) || train->current_order.GetCargoUnloadType(cargo) == OrderUnloadType::NoUnload) continue;
   const uint count = part->cargo.DeliverToExternal(part->cargo.StoredCount(), cargo, train->external_entry_tile, city->index.base(), train->external_distance_tiles, income, station->index, &station->goods[cargo]);
   changed |= count != 0;
   city->delivered_units += count; city->month_delivered[cargo] += count;
   delivered[cargo] += count;
   if (Company *company = Company::GetIfValid(train->owner)) company->cur_economy.delivered_cargo[cargo] += count;
  } else if (profile.produced.Test(cargo) && train->current_order.GetCargoLoadType(cargo) != OrderLoadType::NoLoad && part->cargo.StoredCount() < part->cargo_cap) {
   auto &ge = station->goods[cargo];
   if (ge.data == nullptr || ge.data->cargo.AvailableCount() == 0) continue;
   if (!routing_ready) {
    /* This native query skips the current cargo stop. While outside the map,
     * its context is the city terminal, not the last physical station visited.
     * Restore the physical history immediately after the cargo-only query. */
    AutoRestoreBackup last_station(train->last_station_visited, station->index);
    next_stations = train->GetNextStoppingStation(); routing_ready = true;
   }
   /* The virtual terminal's map anchor is not the shipment origin. Native
    * loading records the actual portal; the off-map leg uses this trip's
    * distance snapshot, even when the city setting changed after entry. */
   for (StationCargoList::ConstIterator it = ge.data->cargo.Packets()->begin(); it != ge.data->cargo.Packets()->end(); ++it) {
    CargoPacket *packet = *it;
    if (packet->external_source_city == city->index.base() && packet->GetFirstStation() == station->index) packet->external_distance = train->external_distance_tiles;
   }
   uint capacity = part->cargo_cap - part->cargo.StoredCount();
   const bool uses_trade_credit = city->exchange_market && !IsExternalCityPassengerOrMail(cargo);
   if (uses_trade_credit) capacity = uint(std::min<uint64_t>(capacity, city->trade_credit));
   if (capacity == 0) continue;
   const uint count = ge.data->cargo.Load(capacity, &part->cargo,
     next_stations.Get(cargo), train->external_entry_tile);
   if (count == 0) continue;
   changed = true;
   city->stock[cargo] -= count; city->loaded_units += count; city->month_loaded[cargo] += count;
   if (uses_trade_credit) { assert(count <= city->trade_credit); city->trade_credit -= count; }
  }
 }
 if (arrival) StoreExternalCityInputs(city, delivered);
 if (income != 0) {
  SubtractMoneyFromCompany(train->owner, CommandCost(train->GetExpenseType(true), -income));
  train->profit_this_year += income << 8;
 }
 if (!changed) return;
 UpdateExternalMarketAcceptance(station);
 if (train->type == VehicleType::Train) Train::From(train)->First()->CargoChanged();
 else if (train->type == VehicleType::Road) RoadVehicle::From(train)->First()->CargoChanged();
 else if (train->type == VehicleType::Aircraft) { train->InvalidateNewGRFCacheOfChain(); UpdateAircraftCache(Aircraft::From(train)); Aircraft::From(train)->MarkDirty(); }
 else if (train->type == VehicleType::Ship) {
  /* NewGRF ships may change speed and appearance with their cargo load.
   * Refresh before either same-entrance departure or a save/load rebuild. */
  train->InvalidateNewGRFCacheOfChain();
  Ship::From(train)->MarkDirty();
 }
 InvalidateWindowClassesData(WindowClass::ExternalCityEconomy);
 InvalidateWindowData(WindowClass::VehicleDetails, train->index);
}

/** Full load uses cargo classes, not a single full wagon in an otherwise empty train. */
bool ExternalCityVehicleNeedsFullLoad(const Vehicle *train)
{
 CargoArray capacity{}, stored{};
 for (const Vehicle *part = train; part != nullptr; part = part->Next()) {
  if (part->cargo_type >= NUM_CARGO || part->cargo_cap == 0) continue;
  if (train->current_order.GetCargoLoadType(part->cargo_type) == OrderLoadType::NoLoad) continue;
  capacity[part->cargo_type] += part->cargo_cap; stored[part->cargo_type] += part->cargo.StoredCount();
 }
 bool any = false, any_full = false, missing = false;
 for (CargoType cargo : EnumRange(NUM_CARGO)) {
  if (capacity[cargo] == 0) continue;
  any = true; any_full |= stored[cargo] >= capacity[cargo];
  if (IsFullLoadOrderLoadType(train->current_order.GetCargoLoadType(cargo)) && stored[cargo] < capacity[cargo]) missing = true;
 }
 if (train->current_order.GetLoadType() == OrderLoadType::FullLoadAny) return any && !any_full;
 return missing;
}

static bool boundary_cache_dirty = true;
static std::array<std::vector<ExternalBoundaryID>, 4> boundary_cache;
void InvalidateExternalBoundaries() { boundary_cache_dirty = true; }
void ExternalBoundary::PreCleanPool()
{
 for (auto &edge : boundary_cache) edge.clear();
 InvalidateExternalBoundaries();
}
uint GetExternalBoundaryLength(DiagDirection edge)
{
 return edge == DiagDirection::NE || edge == DiagDirection::SW ? Map::SizeY() : Map::SizeX();
}
bool GetExternalBoundaryCoordinate(TileIndex tile, DiagDirection edge, uint &coordinate, bool strict)
{
 if (!IsValidTile(tile) || edge >= DiagDirection::End) return false;
 const uint x = TileX(tile), y = TileY(tile), low = _settings_game.construction.freeform_edges ? 1 : 0;
 const uint normal = edge == DiagDirection::NE || edge == DiagDirection::SW ? x : y;
 coordinate = edge == DiagDirection::NE || edge == DiagDirection::SW ? y : x;
 const uint end = edge == DiagDirection::NE || edge == DiagDirection::SW ? Map::SizeX() - 2 : Map::SizeY() - 2;
 return !strict || normal == (edge == DiagDirection::NE || edge == DiagDirection::NW ? low : end);
}
static void RebuildBoundaryCache()
{
 if (!boundary_cache_dirty) return;
 for (auto &edge : boundary_cache) edge.clear();
 for (const ExternalBoundary *region : ExternalBoundary::Iterate()) {
  if (region->edge < DiagDirection::End) boundary_cache[uint(region->edge)].push_back(region->index);
 }
 for (auto &edge : boundary_cache) std::sort(edge.begin(), edge.end(), [](auto a, auto b) {
  return std::make_pair(ExternalBoundary::Get(a)->first, a) < std::make_pair(ExternalBoundary::Get(b)->first, b);
 });
 boundary_cache_dirty = false;
}
ExternalCityID GetExternalCityAtBoundary(DiagDirection edge, uint coordinate)
{
 if (edge >= DiagDirection::End || coordinate >= GetExternalBoundaryLength(edge)) return ExternalCityID::Invalid();
 RebuildBoundaryCache();
 const auto &regions = boundary_cache[uint(edge)];
 auto next = std::upper_bound(regions.begin(), regions.end(), coordinate, [](uint value, ExternalBoundaryID id) {
  return value < ExternalBoundary::Get(id)->first;
 });
 if (next == regions.begin()) return ExternalCityID::Invalid();
 const ExternalBoundary *region = ExternalBoundary::Get(*std::prev(next));
 return coordinate <= region->last && ExternalCity::IsValidID(region->city) ? region->city : ExternalCityID::Invalid();
}
ExternalCityID GetExternalCityForEntrance(TileIndex tile)
{
 if (!Depot::IsExternalConnection(tile)) return ExternalCityID::Invalid();
 const DiagDirection edge = ReverseDiagDir(Depot::GetExternalDirection(tile));
 uint coordinate;
 /* Legacy portals can be a few tiles inland. Their orientation projects onto
  * the appropriate boundary; new construction still requires the actual edge. */
 if (!GetExternalBoundaryCoordinate(tile, edge, coordinate, false)) return ExternalCityID::Invalid();
 return GetExternalCityAtBoundary(edge, coordinate);
}
void ChangeExternalCityOwner(Owner old_owner, Owner new_owner)
{
 for (ExternalCity *city : ExternalCity::Iterate()) if (city->owner == old_owner) city->owner = new_owner;
 for (ExternalBoundary *region : ExternalBoundary::Iterate()) if (region->owner == old_owner) region->owner = new_owner;
}
bool ValidateExternalBoundaries()
{
 InvalidateExternalBoundaries();
 RebuildBoundaryCache();
 for (const auto &edge : boundary_cache) {
  uint end = 0; bool previous = false;
  for (auto id : edge) {
   const ExternalBoundary *region = ExternalBoundary::Get(id);
   if (!ExternalCity::IsValidID(region->city) || region->first > region->last || region->last >= GetExternalBoundaryLength(region->edge) || (previous && region->first <= end)) return false;
   previous = true; end = region->last;
  }
 }
 for (const auto *region : ExternalBoundary::Iterate()) if (region->edge >= DiagDirection::End) return false;
 return true;
}
void MigrateExternalCityBoundaries()
{
 /* v3 linked cities to depots. Preserve those destinations by partitioning
  * each populated edge at deterministic midpoints between legacy entrances. */
 struct Anchor { uint coordinate; ExternalCityID city; Owner owner; DepotID depot; };
 std::array<std::vector<Anchor>, 4> anchors;
 for (Depot *portal : Depot::Iterate()) {
  if (portal->is_external_connection && IsRailDepotTile(portal->xy) && ExternalCity::IsValidID(portal->external_city)) {
   auto edge = ReverseDiagDir(Depot::GetExternalDirection(portal->xy)); uint coordinate;
   if (GetExternalBoundaryCoordinate(portal->xy, edge, coordinate, false)) anchors[uint(edge)].push_back({coordinate, portal->external_city, GetTileOwner(portal->xy), portal->index});
  }
  portal->external_city = ExternalCityID::Invalid();
 }
 for (uint e = 0; e < 4; ++e) {
  auto &list = anchors[e];
  std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return std::make_pair(a.coordinate, a.depot) < std::make_pair(b.coordinate, b.depot); });
  list.erase(std::unique(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.coordinate == b.coordinate; }), list.end());
  ExternalBoundary *previous = nullptr;
  for (size_t i = 0; i < list.size(); ++i) {
   uint first = i == 0 ? 0 : (list[i-1].coordinate + list[i].coordinate) / 2 + 1;
   uint last = i+1 == list.size() ? GetExternalBoundaryLength(DiagDirection(e))-1 : (list[i].coordinate + list[i+1].coordinate) / 2;
   if (previous != nullptr && previous->city == list[i].city && previous->owner == list[i].owner) { previous->last = uint16_t(last); continue; }
   previous = ExternalBoundary::Create(); previous->city = list[i].city; previous->owner = list[i].owner;
   previous->edge = DiagDirection(e); previous->first = uint16_t(first); previous->last = uint16_t(last);
  }
 }
 InvalidateExternalBoundaries();
}

CommandCost CmdManageExternalCityRegion(DoCommandFlags flags, DepotID, ExternalCityID city_id, ExternalCityOperation operation, uint16_t minutes, const std::string &name, ExternalBoundaryID region_id, DiagDirection edge, uint16_t first, uint16_t last)
{
 if (!Company::IsValidID(_current_company) || operation > ExternalCityOperation::SetStopTime || operation == ExternalCityOperation::Link) return CMD_ERROR;
 ExternalCity *city = ExternalCity::GetIfValid(city_id);
 ExternalBoundary *region = ExternalBoundary::GetIfValid(region_id);
 auto foreign = [](Owner owner) { return Company::IsValidID(owner) && owner != _current_company; };
 if ((operation == ExternalCityOperation::Create || operation == ExternalCityOperation::SetBoundary || operation == ExternalCityOperation::RemoveBoundary) &&
     _game_mode != GameMode::Editor && !_settings_game.vehicle.external_boundary_editing) return CommandCost(STR_EXTERNAL_BOUNDARY_LOCKED);
 if (operation == ExternalCityOperation::SetBoundary || operation == ExternalCityOperation::RemoveBoundary) {
  if (region_id != ExternalBoundaryID::Invalid() && region == nullptr) return CMD_ERROR;
  if (region != nullptr && foreign(region->owner)) return CommandCost(STR_ERROR_AREA_IS_OWNED_BY_ANOTHER);
  if (operation == ExternalCityOperation::RemoveBoundary) {
   if (region == nullptr) return CMD_ERROR;
  } else {
   if (city == nullptr || edge >= DiagDirection::End || first > last || last >= GetExternalBoundaryLength(edge)) return CMD_ERROR;
   if (region == nullptr && !ExternalBoundary::CanAllocateItem()) return CMD_ERROR;
   for (const ExternalBoundary *other : ExternalBoundary::Iterate()) {
    if (other != region && other->edge == edge && first <= other->last && last >= other->first) return CommandCost(STR_EXTERNAL_BOUNDARY_OVERLAP);
   }
  }
 } else {
  if (operation != ExternalCityOperation::Create && city == nullptr) return CMD_ERROR;
  if (city != nullptr && foreign(city->owner)) return CommandCost(STR_ERROR_AREA_IS_OWNED_BY_ANOTHER);
  if (operation == ExternalCityOperation::Create && (city_id != ExternalCityID::Invalid() || !ExternalCity::CanAllocateItem())) return CommandCost(STR_EXTERNAL_CITY_LIMIT);
  if (operation == ExternalCityOperation::Create || operation == ExternalCityOperation::Rename) {
   if (name.find_first_not_of(" \t\r\n") == std::string::npos || Utf8StringLength(name) >= 64) return CMD_ERROR;
   for (const ExternalCity *other : ExternalCity::Iterate()) if (other != city && std::string_view(other->name) == name) return CommandCost(STR_ERROR_NAME_MUST_BE_UNIQUE);
  }
  if ((operation == ExternalCityOperation::Create || operation == ExternalCityOperation::SetTime) && (minutes < 1 || minutes > 1440)) return CMD_ERROR;
  if (operation == ExternalCityOperation::SetDistance && minutes == 0) return CMD_ERROR;
  if (operation == ExternalCityOperation::SetStopTime && minutes > 1440) return CMD_ERROR;
 }
 if (flags.Test(DoCommandFlag::Execute)) {
  switch (operation) {
   case ExternalCityOperation::Create:
    city = ExternalCity::Create(); city->name = name; city->owner = _current_company; city->trip_minutes = minutes;
    city->distance_tiles = std::max<uint>(1, _settings_game.vehicle.external_trip_distance);
    city->stop_minutes = _settings_game.vehicle.external_stop_minutes;
    /* Derive a private seed from synchronized state without consuming the
     * world's random stream: city management must not alter map industry RNG. */
    city->economy_seed = std::max<uint32_t>(1, MixExternalEconomySeed(_random.state[0] ^ (_random.state[1] * 0x85EBCA6BU) ^ (uint32_t(city->index.base()) + 1) * 0x9E3779B9U));
    ConfigureExternalCityEconomy(city);
    InitializeExternalCityMarket(city);
    break;
   case ExternalCityOperation::Rename: city->name = name; city->owner = _current_company; break;
   case ExternalCityOperation::SetTime: city->trip_minutes = minutes; city->owner = _current_company; break;
   case ExternalCityOperation::SetDistance: city->distance_tiles = minutes; city->owner = _current_company; break;
   case ExternalCityOperation::SetStopTime: city->stop_minutes = minutes; city->owner = _current_company; break;
   case ExternalCityOperation::SetBoundary:
    if (region == nullptr) region = ExternalBoundary::Create();
    region->city = city_id; region->owner = _current_company; region->edge = edge; region->first = first; region->last = last; InvalidateExternalBoundaries(); break;
   case ExternalCityOperation::RemoveBoundary: delete region; InvalidateExternalBoundaries(); break;
   default: break;
  }
  EnsureExternalCityMarkets();
  MarkWholeScreenDirty();
  InvalidateWindowClassesData(WindowClass::ExternalCity);
  InvalidateWindowClassesData(WindowClass::ExternalCityEconomy);
  SetWindowClassesDirty(WindowClass::ExternalEntrance);
  SetWindowClassesDirty(WindowClass::VehicleOrders);
  SetWindowClassesDirty(WindowClass::VehicleView);
  SetWindowClassesDirty(WindowClass::VehicleDepot);
 }
 return CommandCost();
}
