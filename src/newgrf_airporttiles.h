/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file newgrf_airporttiles.h NewGRF handling of airport tiles. */

#ifndef NEWGRF_AIRPORTTILES_H
#define NEWGRF_AIRPORTTILES_H

#include "airport.h"
#include "air_type.h"
#include "newgrf_airport.h"
#include "table/airporttile_ids.h"
#include "station_map.h"
#include "newgrf_animation_type.h"
#include "newgrf_badge_type.h"
#include "newgrf_callbacks.h"
#include "newgrf_commons.h"
#include "newgrf_spritegroup.h"
#include "station_base.h"

#include <span>

/** Scope resolver for handling the tiles of an airport. */
struct AirportTileScopeResolver : public ScopeResolver {
	Station *st;                ///< Destination station, or nullptr in the build GUI.
	TileIndex tile;             ///< Destination tile, or INVALID_TILE in the build GUI.
	const AirportTileSpec *ats; ///< Graphics specification for this tile.
	const AirportTileGraphics *graphics; ///< Per-tile graphics record, if active.
	const AirportSpec *source_spec = nullptr; ///< Source airport, if still available.
	const AirportTileLayout *source_layout = nullptr; ///< Source layout, if still available.
	TileIndexDiffC source_offset{}; ///< Position of the source tile in the rotated layout.

	AirportTileScopeResolver(ResolverObject &ro, const AirportTileSpec *ats, TileIndex tile, Station *st, const AirportTileGraphics *graphics);
	const AirportSpec *GetAirportSpec() const;
	uint8_t GetAirportLayout() const;
	const AirportTileTable *GetSourceTileAtOffset(uint8_t parameter) const;

	uint32_t GetRandomBits() const override;
	uint32_t GetVariable(uint16_t variable, uint32_t parameter, GetVariableExtra &extra) const override;
};

/** Resolver for tiles of an airport. */
struct AirportTileResolverObject : public ResolverObject {
	AirportTileScopeResolver tiles_scope; ///< Scope resolver for the tiles.
	AirportScopeResolver airport_scope;   ///< Scope resolver for the airport owning the tile.

	AirportTileResolverObject(const AirportTileSpec *ats, TileIndex tile, Station *st,
			CallbackID callback = CBID_NO_CALLBACK, uint32_t callback_param1 = 0, uint32_t callback_param2 = 0,
			const AirportTileGraphics *graphics = nullptr);

	ScopeResolver *GetScope(VarSpriteGroupScope scope = VarSpriteGroupScope::Self, VarSpriteGroupScopeOffset relative = 0) override
	{
		switch (scope) {
			case VarSpriteGroupScope::Self: return &tiles_scope;
			case VarSpriteGroupScope::Parent: return &airport_scope;
			default: return ResolverObject::GetScope(scope, relative);
		}
	}

	GrfSpecFeature GetFeature() const override;
	uint32_t GetDebugID() const override;
};

/**
 * Defines the data structure of each individual tile of an airport.
 */
struct AirportTileSpec {
	AnimationInfo<AirportAnimationTriggers> animation; ///< Information about the animation.
	StringID name;                        ///< Tile Subname string, land information on this tile will give you "AirportName (TileSubname)"
	AirportTileCallbackMasks callback_mask; ///< Bitmask telling which grf callback is set
	uint8_t animation_special_flags;      ///< Extra flags to influence the animation
	bool enabled;                         ///< entity still available (by default true). newgrf can disable it, though
	SubstituteGRFFileProps grf_prop;      ///< properties related the the grf file
	std::vector<BadgeID> badges;

	static const AirportTileSpec *Get(StationGfx gfx);
	static const AirportTileSpec *GetByTile(TileIndex tile);

	static void ResetAirportTiles();

private:
	static AirportTileSpec tiles[NUM_AIRPORTTILES];

	friend void AirportTileOverrideManager::SetEntitySpec(AirportTileSpec &&airpts);
};

/** One source tile, with the NewGRF graphics it defines for each rotation. */
struct AirportTileGfxSet {
	std::array<std::optional<AirportTileGraphics>, static_cast<size_t>(DiagDirection::End)> variants;
};

std::optional<AirportTileGraphics> MakeAirportTileGraphics(uint8_t airport_type, uint8_t layout, uint16_t pos, DiagDirection rotation);
std::optional<AirportTileGraphics> MakeAirportTileGraphics(GrfID source_grfid, uint16_t source_airport, uint8_t layout, uint16_t pos, DiagDirection rotation);
const AirportTileGraphics *GetAirportTileGraphics(TileIndex tile);
const AirportTileTable *GetAirportTileGraphicsSource(const AirportTileGraphics &graphics);
StationGfx ResolveAirportTileGraphics(const AirportTileGraphics &graphics);
void InstallAirportTileGraphics(Station *st, TileIndex tile, const AirportTileGraphics *graphics);
void ResetAirportTileAnimation(TileIndex tile, bool start_one_shot = false);
void ReconcileAirportTileGraphicsAnimation(TileIndex tile, AirportTileAppearance &appearance);
void InitializeBuiltAirportTiles(std::span<const TileIndex> tiles);
bool DrawAirportTileGraphicsInGUI(int x, int y, const AirportTileGraphics &graphics, PaletteID palette);

void AnimateAirportTile(TileIndex tile);
bool TriggerAirportTileAnimation(Station *st, TileIndex tile, AirportAnimationTrigger trigger);
bool TriggerAirportAnimation(Station *st, AirportAnimationTrigger trigger, CargoType cargo_type = INVALID_CARGO);
uint8_t GetAirportTileAnimationSpeed(TileIndex tile);
bool DrawNewAirportTile(TileInfo *ti, Station *st, const AirportTileSpec *airts);

void GetGRFAirportTileCandidates(AirportTileType type, std::vector<AirportTileGfxSet> &candidates);

#endif /* NEWGRF_AIRPORTTILES_H */
