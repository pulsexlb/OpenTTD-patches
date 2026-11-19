/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file depot_sl.cpp Code handling saving and loading of depots. */

#include "../stdafx.h"

#include "saveload.h"
#include "compat/depot_sl_compat.h"

#include "../depot_base.h"
#include "../external_city.h"
#include "../sl/extended_ver_sl.h"
#include "../town.h"

#include "../safeguards.h"

namespace upstream_sl {

static TownID _town_index;

static const SaveLoad _depot_desc[] = {
	 SLE_CONDVAR(Depot, xy, VarFileType::U16 | VarMemType::U32, SaveLoadVersion::MinVersion, SaveLoadVersion::MultipleRoadStops),
	 SLE_CONDVAR(Depot, xy, VarTypes::U32, SaveLoadVersion::MultipleRoadStops, SaveLoadVersion::MaxVersion),
	SLEG_CONDVAR("town_index", _town_index, VarTypes::U16, SaveLoadVersion::MinVersion, SaveLoadVersion::UniqueDepotNames),
	 SLE_CONDREF(Depot, town, SLRefType::Town, SaveLoadVersion::UniqueDepotNames, SaveLoadVersion::MaxVersion),
	 SLE_CONDVAR(Depot, town_cn, VarTypes::U16, SaveLoadVersion::UniqueDepotNames, SaveLoadVersion::MaxVersion),
	SLE_CONDSTR(Depot, name, VarTypes::STR, 0, SaveLoadVersion::UniqueDepotNames, SaveLoadVersion::MaxVersion),
	 SLE_CONDVAR(Depot, build_date, VarTypes::I32, SaveLoadVersion::NewGRFDepotBuildDate, SaveLoadVersion::MaxVersion),
	 SLE_VAR(Depot, is_external_connection, VarTypes::BOOL),
	 SLE_VAR(Depot, external_city, VarTypes::U16),
};

struct DEPTChunkHandler : ChunkHandler {
	DEPTChunkHandler() : ChunkHandler("DEPT", ChunkType::Table) {}

	void Save() const override
	{
		SlTableHeader(_depot_desc);

		for (Depot *depot : Depot::Iterate()) {
			SlSetArrayIndex(depot->index);
			SlObject(depot, _depot_desc);
		}
	}

	void Load() const override
	{
		const std::vector<SaveLoad> slt = SlCompatTableHeader(_depot_desc, _depot_sl_compat);

		int index;

		while ((index = SlIterateArray()) != -1) {
			Depot *depot = Depot::CreateAtIndex(DepotID(index));
			SlObject(depot, slt);

			/* Set the town 'pointer' so we can restore it later. */
			if (IsSavegameVersionBefore(SaveLoadVersion::UniqueDepotNames)) depot->town = reinterpret_cast<Town *>(static_cast<size_t>(_town_index.base()));
		}
	}

	void FixPointers() const override
	{
		for (Depot *depot : Depot::Iterate()) {
			SlObject(depot, _depot_desc);
			if (IsSavegameVersionBefore(SaveLoadVersion::UniqueDepotNames)) depot->town = Town::Get((size_t)depot->town);
		}
	}
};

static const DEPTChunkHandler DEPT;

static const SaveLoad _external_city_desc[] = {
	SLE_STR(ExternalCity, name, VarTypes::STR, 0),
	SLE_VAR(ExternalCity, owner, VarTypes::U8),
	SLE_VAR(ExternalCity, trip_minutes, VarTypes::U16),
	SLE_VAR(ExternalCity, economy_seed, VarTypes::U32),
	SLE_VAR(ExternalCity, market_station, VarTypes::U16),
 SLE_VAR(ExternalCity, exchange_market, VarTypes::BOOL),
 SLE_ARR(ExternalCity, trade_cargo_roles, VarTypes::U8, NUM_CARGO),
 SLE_VAR(ExternalCity, trade_credit, VarTypes::U64),
 SLE_VAR(ExternalCity, trade_credit_fraction, VarTypes::U16),
 SLE_ARR(ExternalCity, trade_received, VarTypes::U8, NUM_CARGO),
 SLE_ARR(ExternalCity, trade_previous_supply, VarTypes::U8, NUM_CARGO),
	SLE_VAR(ExternalCity, distance_tiles, VarTypes::U16),
	SLE_VAR(ExternalCity, stop_minutes, VarTypes::U16),
	SLE_ARR(ExternalCity, stock, VarTypes::U32, NUM_CARGO),
	SLE_ARR(ExternalCity, demand, VarTypes::U32, NUM_CARGO),
	SLE_VAR(ExternalCity, market_initialized, VarTypes::BOOL),
	SLE_VAR(ExternalCity, delivered_units, VarTypes::U64),
	SLE_VAR(ExternalCity, loaded_units, VarTypes::U64),
	SLE_ARR(ExternalCity, month_supply, VarTypes::U32, NUM_CARGO),
	SLE_ARR(ExternalCity, month_demand, VarTypes::U32, NUM_CARGO),
	SLE_ARR(ExternalCity, month_loaded, VarTypes::U32, NUM_CARGO),
	SLE_ARR(ExternalCity, month_delivered, VarTypes::U32, NUM_CARGO),
	SLE_VAR(ExternalCity, statistics_initialized, VarTypes::BOOL),
 SLE_VAR(ExternalCity, industries_initialized, VarTypes::BOOL),
 SLE_ARR(ExternalCity, industry_types, VarTypes::U8, EXTERNAL_CITY_INDUSTRIES),
 SLE_ARR(ExternalCity, industry_inputs, VarTypes::U64, EXTERNAL_CITY_INDUSTRIES * NUM_CARGO),
 SLE_ARR(ExternalCity, industry_month_consumed, VarTypes::U32, EXTERNAL_CITY_INDUSTRIES * NUM_CARGO),
 SLE_ARR(ExternalCity, industry_month_produced, VarTypes::U32, EXTERNAL_CITY_INDUSTRIES * NUM_CARGO),
 SLE_ARR(ExternalCity, industry_remainders, VarTypes::U16, EXTERNAL_CITY_INDUSTRIES * NUM_CARGO),
 SLE_ARR(ExternalCity, industry_storage, VarTypes::I32, EXTERNAL_CITY_INDUSTRIES * 256),
 SLE_ARR(ExternalCity, industry_last_delivery, VarTypes::I32, EXTERNAL_CITY_INDUSTRIES * NUM_CARGO),
 SLE_ARR(ExternalCity, industry_previous_produced, VarTypes::U32, EXTERNAL_CITY_INDUSTRIES * NUM_CARGO),
 SLE_ARR(ExternalCity, industry_transported, VarTypes::U32, EXTERNAL_CITY_INDUSTRIES * NUM_CARGO),
 SLE_ARR(ExternalCity, industry_previous_transported, VarTypes::U32, EXTERNAL_CITY_INDUSTRIES * NUM_CARGO),
 SLE_ARR(ExternalCity, industry_rates, VarTypes::U8, EXTERNAL_CITY_INDUSTRIES * INDUSTRY_NUM_OUTPUTS),
 SLE_ARR(ExternalCity, industry_level, VarTypes::U8, EXTERNAL_CITY_INDUSTRIES),
 SLE_ARR(ExternalCity, industry_built, VarTypes::I32, EXTERNAL_CITY_INDUSTRIES),
 SLE_ARR(ExternalCity, industry_accepts, VarTypes::U8, EXTERNAL_CITY_INDUSTRIES * INDUSTRY_NUM_INPUTS),
 SLE_ARR(ExternalCity, industry_products, VarTypes::U8, EXTERNAL_CITY_INDUSTRIES * INDUSTRY_NUM_OUTPUTS),
 SLE_VAR(ExternalCity, industry_counter, VarTypes::U16),
 SLE_ARR(ExternalCity, industry_recent_supply, VarTypes::U8, EXTERNAL_CITY_INDUSTRIES * NUM_CARGO),
 SLE_ARR(ExternalCity, industry_intermediate, VarTypes::U8, EXTERNAL_CITY_INDUSTRIES),
 SLE_ARR(ExternalCity, industry_pending_output, VarTypes::U64, EXTERNAL_CITY_INDUSTRIES * NUM_CARGO),
};
struct EXCTChunkHandler : ChunkHandler {
	EXCTChunkHandler() : ChunkHandler("EXCT", ChunkType::Table) {}
	void Save() const override
	{
		SlTableHeader(_external_city_desc);
		for (ExternalCity *city : ExternalCity::Iterate()) {
			SlSetArrayIndex(city->index); SlObject(city, _external_city_desc);
		}
	}
	void Load() const override
	{
		std::vector<SaveLoad> description(std::begin(_external_city_desc), std::end(_external_city_desc));
		if (SlXvIsFeatureMissing(XSLFI_EXTERNAL_CONNECTIONS, 14)) {
			for (auto &field : description) if (field.name == "industry_inputs") field.conv = VarFileType::U32 | VarMemType::U64;
		}
		const auto table = SlCompatTableHeader(description, {});
		int index;
		while ((index = SlIterateArray()) != -1) {
			if (index >= 64) SlErrorCorrupt("Invalid external city index");
			ExternalCity *city = ExternalCity::CreateAtIndex(ExternalCityID{uint16_t(index)});
			SlObject(city, table);
   for (IndustryType type : city->industry_types) if (type != IT_INVALID && type >= NUM_INDUSTRYTYPES) SlErrorCorrupt("Invalid external industry");
   for (uint level : city->industry_level) if (level != 0 && (level < 4 || level > 128)) SlErrorCorrupt("Invalid external industry production level");
   for (const auto &cargoes : {std::span<const CargoType>(city->industry_accepts), std::span<const CargoType>(city->industry_products)}) {
    for (CargoType cargo : cargoes) if (cargo != INVALID_CARGO && cargo >= NUM_CARGO) SlErrorCorrupt("Invalid external industry cargo");
   }
   for (uint fraction : city->industry_remainders) if (fraction >= 256) SlErrorCorrupt("Invalid external industry fraction");
   for (uint cycles : city->industry_recent_supply) if (cycles > 28) SlErrorCorrupt("Invalid external industry supply history");
   for (uint role : city->trade_cargo_roles) if (role > 2) SlErrorCorrupt("Invalid external city trade cargo role");
   if (city->trade_credit_fraction >= NUM_CARGO) SlErrorCorrupt("Invalid external city trade credit fraction");
   for (uint delivered : city->trade_received) if (delivered > 1) SlErrorCorrupt("Invalid external city trade supply history");
   for (uint delivered : city->trade_previous_supply) if (delivered > 1) SlErrorCorrupt("Invalid external city trade supply history");
   for (uint remainder : city->industry_intermediate) if (remainder >= 8) SlErrorCorrupt("Invalid external industry intermediate stock");
			if (city->name.empty() || city->trip_minutes < 1 || city->trip_minutes > 1440) SlErrorCorrupt("Invalid external city");
			if (city->distance_tiles == 0 || city->stop_minutes > 1440) SlErrorCorrupt("Invalid external city itinerary");
			for (uint cargo = 0; cargo < NUM_CARGO; ++cargo) if (city->stock[CargoType(cargo)] > 4096 || city->demand[CargoType(cargo)] > 4096) SlErrorCorrupt("Invalid external city market");
		}
	}
};
static const EXCTChunkHandler EXCT;
static const SaveLoad _external_boundary_desc[] = {
 SLE_VAR(ExternalBoundary, city, VarTypes::U16),
 SLE_VAR(ExternalBoundary, owner, VarTypes::U8),
 SLE_VAR(ExternalBoundary, edge, VarTypes::U8),
 SLE_VAR(ExternalBoundary, first, VarTypes::U16),
 SLE_VAR(ExternalBoundary, last, VarTypes::U16),
};
struct EXBRChunkHandler : ChunkHandler {
 EXBRChunkHandler() : ChunkHandler("EXBR", ChunkType::Table) {}
 void Save() const override {
  SlTableHeader(_external_boundary_desc);
  for (ExternalBoundary *region : ExternalBoundary::Iterate()) { SlSetArrayIndex(region->index); SlObject(region, _external_boundary_desc); }
 }
 void Load() const override {
  const auto table = SlCompatTableHeader(_external_boundary_desc, {});
  int index;
  while ((index = SlIterateArray()) != -1) {
   if (index >= 64000) SlErrorCorrupt("Invalid external boundary index");
   auto *region = ExternalBoundary::CreateAtIndex(ExternalBoundaryID{uint16_t(index)});
   SlObject(region, table);
  }
  InvalidateExternalBoundaries();
 }
};
static const EXBRChunkHandler EXBR;
static const ChunkHandlerRef depot_chunk_handlers[] = {
	DEPT,
	EXCT,
	EXBR,
};

extern const ChunkHandlerTable _depot_chunk_handlers(depot_chunk_handlers);

}
