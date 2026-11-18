/* SPDX-License-Identifier: GPL-2.0-only */
/** @file external_city_type.h Stable IDs shared by external destinations and transport modes. */
#ifndef EXTERNAL_CITY_TYPE_H
#define EXTERNAL_CITY_TYPE_H
#include "core/pool_id_type.hpp"
struct ExternalCityIDTag : public PoolIDTraits<uint16_t, 64, 0xFFFF> {};
using ExternalCityID = PoolID<ExternalCityIDTag>;
struct ExternalBoundaryIDTag : public PoolIDTraits<uint16_t, 64000, 0xFFFF> {};
using ExternalBoundaryID = PoolID<ExternalBoundaryIDTag>;
enum class ExternalCityOperation : uint8_t { Create, Link, Rename, SetTime, SetBoundary, RemoveBoundary, SetDistance, SetStopTime };
enum class ExternalTripPhase : uint8_t { None, Outbound, Stopped, Returning, Queued, Legacy };
#endif
