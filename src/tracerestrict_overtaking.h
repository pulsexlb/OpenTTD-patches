/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */
/** @file tracerestrict_overtaking.h Automatic yielding associated through ordinary train slots. */
#ifndef TRACERESTRICT_OVERTAKING_H
#define TRACERESTRICT_OVERTAKING_H
#include "tracerestrict.h"
enum class AutoYieldRole : uint8_t { Approach, Main, Siding, Exit, Clear };
enum class AutoYieldCondition : uint8_t { Faster, NeedsSiding, FreeSiding, MustWait, Full, Evacuate, MayLeave };
constexpr uint32_t AUTO_YIELD_DEFAULT_CONFIG = 5u << 16;
enum class AutoYieldRegistration : uint8_t { Reservation, Front, Tail };
constexpr uint32_t AUTO_YIELD_PERCENT_MASK = 0x03FF0000u;
constexpr uint32_t AUTO_YIELD_REGISTRATION_MASK = 3u << 26;
constexpr uint GetAutoYieldPercent(uint32_t config) { return (config & AUTO_YIELD_PERCENT_MASK) >> 16; }
constexpr AutoYieldRegistration GetAutoYieldRegistration(uint32_t config) { return static_cast<AutoYieldRegistration>((config & AUTO_YIELD_REGISTRATION_MASK) >> 26); }
constexpr uint32_t AUTO_YIELD_PLATFORM = 1u << 31; ///< Never overlaps valid legacy percentage/distance values.
constexpr bool IsAutoYieldLaneRole(AutoYieldRole role) { return role == AutoYieldRole::Siding || role == AutoYieldRole::Exit; }
constexpr uint32_t AUTO_YIELD_DETECTION_REF = UINT32_MAX;
constexpr uint32_t AUTO_YIELD_PENDING_REF = UINT32_MAX - 1; ///< Hidden pre-entry reservation claims, saved like siding slots.
bool IsAutoYieldInstruction(TraceRestrictItemType type);
void MigrateLegacyAutoYieldSpeedCaps(TraceRestrictProgram &program);
void InvalidateAutoYieldAreas();
void ResetAutoYieldAreas();
void UpdateAutoYieldAreas();
void AutoYieldSignalChanged(TileIndex tile, Track track);
bool MigrateAutoYieldAreasToSlots();
void AutoYieldSettingsChanged();
void AutoYieldSlotVacated(TraceRestrictSlotID slot, VehicleID vehicle);
void UpdateAutoYieldSlotSignals(const TraceRestrictSlot *slot);
void AutoYieldGameTick();
bool EvaluateAutoYield(const Train *train, uint16_t slot, AutoYieldCondition condition, bool invert = false);
void ExecuteAutoYield(const Train *train, const TraceRestrictProgramInput &input, TraceRestrictProgramResult &out, uint16_t area, AutoYieldRole role, bool &ignore_pathfinder_penalty);
#endif
