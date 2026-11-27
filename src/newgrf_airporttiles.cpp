/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file newgrf_airporttiles.cpp NewGRF handling of airport tiles. */

#include "stdafx.h"
#include "debug.h"
#include "core/backup_type.hpp"
#include "zoom_func.h"
#include "newgrf_airport.h"
#include "newgrf_airporttiles.h"
#include "newgrf_badge.h"
#include "newgrf_extension.h"
#include "newgrf_spritegroup.h"
#include "newgrf_sound.h"
#include "station_base.h"
#include "tile_cmd.h"
#include "water.h"
#include "landscape.h"
#include "air_map.h"
#include "company_base.h"
#include "company_func.h"
#include "direction_func.h"
#include "gfx_func.h"
#include "sprite.h"
#include "town.h"
#include "newgrf_animation_base.h"

#include "table/strings.h"
#include "table/airporttiles.h"

#include "safeguards.h"

extern uint32_t GetRelativePosition(TileIndex tile, TileIndex ind_tile);

AirportTileSpec AirportTileSpec::tiles[NUM_AIRPORTTILES];

AirportTileOverrideManager _airporttile_mngr(NEW_AIRPORTTILE_OFFSET, NUM_AIRPORTTILES, INVALID_AIRPORTTILE);

/**
 * Retrieve airport tile spec for the given airport tile
 * @param gfx index of airport tile
 * @return A pointer to the corresponding AirportTileSpec
 */
/* static */ const AirportTileSpec *AirportTileSpec::Get(StationGfx gfx)
{
	/* should be assert(gfx < lengthof(tiles)), but that gives compiler warnings
	 * since it's always true if the following holds: */
	static_assert(std::numeric_limits<StationGfx>::max() + 1 == lengthof(tiles));
	return &AirportTileSpec::tiles[gfx];
}

/* A source identity must not resolve through a substitute or a different GRF's override. */
static const AirportSpec *GetSourceAirportSpec(GrfID grfid, uint16_t id)
{
	if (grfid.Empty()) return id < NEW_AIRPORT_OFFSET ? AirportSpec::GetWithoutOverride(id) : nullptr;
	return AirportSpec::GetByGrf(grfid, id);
}

static const AirportTileLayout *GetSourceLayout(const AirportSpec *spec, uint8_t layout, uint16_t pos, DiagDirection rotation)
{
	if (spec == nullptr || layout >= spec->layouts.size() || !IsValidDiagDirection(rotation)) return nullptr;
	const AirportTileLayout &source = spec->layouts[layout];
	if (source.size_x == 0 || source.tiles.size() != static_cast<size_t>(source.size_x) * source.size_y || pos >= source.tiles.size()) return nullptr;
	return source.tiles[pos].type != ATT_INVALID ? &source : nullptr;
}

static bool IsNewGRFAirportTileGraphics(StationGfx gfx)
{
	if (gfx < NEW_AIRPORTTILE_OFFSET || gfx == INVALID_AIRPORTTILE) return false;
	const AirportTileSpec *spec = AirportTileSpec::Get(gfx);
	return spec->enabled && spec->grf_prop.HasGrfFile() && !spec->grf_prop.grfid.Empty() && spec->grf_prop.HasSpriteGroups();
}

static StationGfx GetSourceTileGfx(const AirportTileTable &entry, DiagDirection rotation)
{
	const StationGfx gfx = entry.gfx[to_underlying(rotation)];
	return gfx == INVALID_AIRPORTTILE ? gfx : GetTranslatedAirportTileID(gfx);
}

static std::optional<AirportTileGraphics> MakeAirportTileGraphics(const AirportSpec *spec, uint8_t layout, uint16_t pos, DiagDirection rotation)
{
	const AirportTileLayout *source = GetSourceLayout(spec, layout, pos, rotation);
	if (source == nullptr) return std::nullopt;

	const StationGfx gfx = GetSourceTileGfx(source->tiles[pos], rotation);
	if (!IsNewGRFAirportTileGraphics(gfx)) return std::nullopt;

	const auto &tile_prop = AirportTileSpec::Get(gfx)->grf_prop;
	const auto &airport_prop = spec->grf_prop;
	const uint16_t source_id = airport_prop.HasGrfFile() ? airport_prop.local_id : spec->GetIndex();
	return AirportTileGraphics{tile_prop.grfid, tile_prop.local_id, airport_prop.grfid, source_id, layout, pos, rotation};
}

std::optional<AirportTileGraphics> MakeAirportTileGraphics(uint8_t airport_type, uint8_t layout, uint16_t pos, DiagDirection rotation)
{
	if (airport_type >= NUM_AIRPORTS) return std::nullopt;
	return MakeAirportTileGraphics(AirportSpec::Get(airport_type), layout, pos, rotation);
}

std::optional<AirportTileGraphics> MakeAirportTileGraphics(GrfID source_grfid, uint16_t source_airport, uint8_t layout, uint16_t pos, DiagDirection rotation)
{
	return MakeAirportTileGraphics(GetSourceAirportSpec(source_grfid, source_airport), layout, pos, rotation);
}

static const AirportTileTable *GetMatchingSourceTile(const AirportTileGraphics &graphics, const AirportTileLayout *layout)
{
	if (layout == nullptr) return nullptr;

	const AirportTileTable &source = layout->tiles[graphics.source_pos];
	const StationGfx gfx = GetSourceTileGfx(source, graphics.source_rotation);
	if (!IsNewGRFAirportTileGraphics(gfx)) return nullptr;
	const auto &prop = AirportTileSpec::Get(gfx)->grf_prop;
	return prop.grfid == graphics.tile_grfid && prop.local_id == graphics.tile_id ? &source : nullptr;
}

/** Find the unrotated source tile entry only if it still supplies the recorded variant. */
const AirportTileTable *GetAirportTileGraphicsSource(const AirportTileGraphics &graphics)
{
	const AirportSpec *spec = GetSourceAirportSpec(graphics.source_grfid, graphics.source_airport);
	return GetMatchingSourceTile(graphics, GetSourceLayout(spec, graphics.source_layout, graphics.source_pos, graphics.source_rotation));
}

/** Resolve an exact tile identity, even when its original airport layout is no longer available. */
StationGfx ResolveAirportTileGraphics(const AirportTileGraphics &graphics)
{
	if (graphics.tile_grfid.Empty() || graphics.tile_id >= NUM_AIRPORTTILES_PER_GRF) return INVALID_AIRPORTTILE;
	const uint16_t id = _airporttile_mngr.GetID(graphics.tile_id, graphics.tile_grfid);
	if (id >= NUM_AIRPORTTILES || !IsNewGRFAirportTileGraphics(id)) return INVALID_AIRPORTTILE;

	const auto &prop = AirportTileSpec::Get(id)->grf_prop;
	return prop.grfid == graphics.tile_grfid && prop.local_id == graphics.tile_id ? id : INVALID_AIRPORTTILE;
}

/** Look up the graphics record of an airport infrastructure tile. */
const AirportTileGraphics *GetAirportTileGraphics(TileIndex tile)
{
	if (!IsValidTile(tile) || !IsAirportTile(tile) || !IsInfrastructure(tile)) return nullptr;
	const Station *st = Station::GetIfValid(GetStationIndex(tile));
	if (st == nullptr) return nullptr;

	auto it = st->airport.tile_graphics.find(tile);
	return it != st->airport.tile_graphics.end() ? &it->second.graphics : nullptr;
}

/**
 * Retrieve the animation and graphics spec used on the map tile.
 * AirType radar and flag IDs overlap with unrelated original airport tiles.
 */
/* static */ const AirportTileSpec *AirportTileSpec::GetByTile(TileIndex tile)
{
	if (const AirportTileGraphics *graphics = GetAirportTileGraphics(tile); graphics != nullptr) {
		const StationGfx gfx = ResolveAirportTileGraphics(*graphics);
		if (gfx != INVALID_AIRPORTTILE) return AirportTileSpec::Get(gfx);
	}

	if (HasAirtypeGfx(tile)) {
		const AirportTiles gfx = GetTileAirportGfx(tile);
		if (IsInfrastructure(tile) && gfx < ATTG_END) return &_origin_airtype_specs[gfx];
		return &_origin_airtype_specs[ATTG_DEFAULT_GFX];
	}

	return AirportTileSpec::Get(GetAirportGfx(tile));
}

/**
 * This function initializes the tile array of AirportTileSpec
 */
void AirportTileSpec::ResetAirportTiles()
{
	auto insert = std::copy(std::begin(_origin_airporttile_specs), std::end(_origin_airporttile_specs), std::begin(AirportTileSpec::tiles));
	std::fill(insert, std::end(AirportTileSpec::tiles), AirportTileSpec{});

	/* Reset any overrides that have been set. */
	_airporttile_mngr.ResetOverride();
}

void AirportTileOverrideManager::SetEntitySpec(AirportTileSpec &&airpts)
{
	StationGfx airpt_id = this->AddEntityID(airpts.grf_prop.local_id, airpts.grf_prop.grfid, airpts.grf_prop.subst_id);

	if (airpt_id == this->invalid_id) {
		GrfMsg(1, "AirportTile.SetEntitySpec: Too many airport tiles allocated. Ignoring.");
		return;
	}

	AirportTileSpec::tiles[airpt_id] = std::move(airpts);

	/* Now add the overrides. */
	for (int i = 0; i < this->max_offset; i++) {
		AirportTileSpec *overridden_airpts = &AirportTileSpec::tiles[i];

		if (this->entity_overrides[i] != AirportTileSpec::tiles[airpt_id].grf_prop.local_id || this->grfid_overrides[i] != AirportTileSpec::tiles[airpt_id].grf_prop.grfid) continue;

		overridden_airpts->grf_prop.override_id = airpt_id;
		overridden_airpts->enabled = false;
		this->entity_overrides[i] = this->invalid_id;
		this->grfid_overrides[i] = {};
	}
}

/**
 * Do airporttile gfx ID translation for NewGRFs.
 * @param gfx the type to get the override for.
 * @return the gfx to actually work with.
 */
StationGfx GetTranslatedAirportTileID(StationGfx gfx)
{
	const AirportTileSpec *it = AirportTileSpec::Get(gfx);
	return it->grf_prop.override_id == INVALID_AIRPORTTILE ? gfx : it->grf_prop.override_id;
}

/**
 * Based on newhouses/newindustries equivalent, but adapted for airports.
 * @param parameter from callback. It's in fact a pair of coordinates
 * @param tile TileIndex from which the callback was initiated
 * @param index of the industry been queried for
 * @param grf_version8 True, if we are dealing with a new NewGRF which uses GRF version >= 8.
 * @return a construction of bits obeying the newgrf format
 */
static uint32_t GetNearbyAirportTileInformation(uint8_t parameter, TileIndex tile, StationID index, bool grf_version8, uint32_t mask)
{
	if (parameter != 0) tile = GetNearbyTile(parameter, tile); // only perform if it is required
	bool is_same_airport = (IsTileType(tile, TileType::Station) && IsAirport(tile) && GetStationIndex(tile) == index);

	uint32_t result = (is_same_airport ? 1 : 0) << 8;
	if (mask & ~0x100) result |= GetNearbyTileInformation(tile, grf_version8, mask);
	return result;
}


/** Encode one tile's graphics ID as reported by variable 0x62. */
static uint32_t GetAirportTileIDForGraphics(StationGfx gfx, GrfID cur_grfid)
{
	if (gfx == INVALID_AIRPORTTILE) return 0xFFFF;
	const AirportTileSpec *ats = AirportTileSpec::Get(gfx);

	if (gfx < NEW_AIRPORTTILE_OFFSET) { // Does it belongs to an old type?
		/* It is an old tile.  We have to see if it's been overridden */
		if (ats->grf_prop.override_id == INVALID_AIRPORTTILE) { // has it been overridden?
			return 0xFF << 8 | gfx; // no. Tag FF + the gfx id of that tile
		}
		/* Overridden */
		const AirportTileSpec *tile_ovr = AirportTileSpec::Get(ats->grf_prop.override_id);

		if (tile_ovr->grf_prop.grfid == cur_grfid) {
			return tile_ovr->grf_prop.local_id; // same grf file
		} else {
			return 0xFFFE; // not the same grf file
		}
	}
	/* Not an 'old type' tile */
	if (ats->grf_prop.HasSpriteGroups()) {
		if (ats->grf_prop.grfid == cur_grfid) { // same airport, same grf ?
			return ats->grf_prop.local_id;
		} else {
			return 0xFFFE; // Defined in another grf file
		}
	}
	/* The tile has no spritegroup */
	return 0xFF << 8 | ats->grf_prop.subst_id; // so just give it the substitute
}

/* Convert coordinates in the unrotated layout to the layout's map orientation. */
static TileIndexDiffC RotateSourceOffset(int x, int y, int width, int height, DiagDirection rotation)
{
	switch (rotation) {
		case DiagDirection::NE: return {static_cast<int16_t>(x), static_cast<int16_t>(y)};
		case DiagDirection::SE: return {static_cast<int16_t>(y), static_cast<int16_t>(width - 1 - x)};
		case DiagDirection::SW: return {static_cast<int16_t>(width - 1 - x), static_cast<int16_t>(height - 1 - y)};
		case DiagDirection::NW: return {static_cast<int16_t>(height - 1 - y), static_cast<int16_t>(x)};
		default: NOT_REACHED();
	}
}

AirportTileScopeResolver::AirportTileScopeResolver(ResolverObject &ro, const AirportTileSpec *ats, TileIndex tile, Station *st, const AirportTileGraphics *graphics)
	: ScopeResolver(ro), st(st), tile(tile), ats(ats), graphics(graphics)
{
	if (this->graphics == nullptr && this->st != nullptr && this->tile != INVALID_TILE) {
		this->graphics = GetAirportTileGraphics(this->tile);
	}
	if (this->graphics == nullptr) return;

	/* When drawing the built-in fallback, the map record may still exist. */
	if (!ats->grf_prop.HasGrfFile() || !ats->grf_prop.HasSpriteGroups() ||
			ats->grf_prop.grfid != this->graphics->tile_grfid || ats->grf_prop.local_id != this->graphics->tile_id) {
		this->graphics = nullptr;
		return;
	}

	const AirportSpec *spec = GetSourceAirportSpec(this->graphics->source_grfid, this->graphics->source_airport);
	const AirportTileLayout *layout = GetSourceLayout(spec, this->graphics->source_layout, this->graphics->source_pos, this->graphics->source_rotation);
	if (GetMatchingSourceTile(*this->graphics, layout) == nullptr) return;
	this->source_spec = spec;
	this->source_layout = layout;
	const int width = this->source_layout->size_x;
	const int height = this->source_layout->size_y;
	const uint16_t pos = this->graphics->source_pos;
	this->source_offset = RotateSourceOffset(pos % width, pos / width, width, height, this->graphics->source_rotation);
}

const AirportSpec *AirportTileScopeResolver::GetAirportSpec() const
{
	if (this->graphics != nullptr) return this->source_spec;
	return this->st != nullptr ? this->st->airport.GetSpec() : nullptr;
}

uint8_t AirportTileScopeResolver::GetAirportLayout() const
{
	if (this->graphics != nullptr) return this->source_layout != nullptr ? this->graphics->source_layout : 0;
	return this->st != nullptr ? this->st->airport.layout : 0;
}

const AirportTileTable *AirportTileScopeResolver::GetSourceTileAtOffset(uint8_t parameter) const
{
	const AirportTileLayout &layout = *this->source_layout;
	const uint rotation = to_underlying(this->graphics->source_rotation);
	const int dx = static_cast<int>(GB(parameter, 0, 4) ^ 8) - 8;
	const int dy = static_cast<int>(GB(parameter, 4, 4) ^ 8) - 8;
	int width = layout.size_x;
	int height = layout.size_y;
	if ((rotation & 1) != 0) std::swap(width, height);

	const int x = this->source_offset.x + dx;
	const int y = this->source_offset.y + dy;
	if (x < 0 || x >= width || y < 0 || y >= height) return nullptr;

	const auto unrotated = RotateSourceOffset(x, y, width, height, static_cast<DiagDirection>((4 - rotation) % 4));
	const AirportTileTable &entry = layout.tiles[unrotated.y * layout.size_x + unrotated.x];
	return entry.type != ATT_INVALID ? &entry : nullptr;
}

/* virtual */ uint32_t AirportTileScopeResolver::GetVariable(uint16_t variable, uint32_t parameter, GetVariableExtra &extra) const
{
	const bool has_world_tile = this->tile != INVALID_TILE && this->st != nullptr;

	/* Variables which can be resolved using the source layout even in a GUI preview. */
	switch (variable) {
		case 0x7A: return GetBadgeVariableResult(*this->ro.grffile, this->ats->badges, parameter);

		case A2VRI_AIRPORTTILES_AIRPORT_LAYOUT:
			return this->GetAirportLayout();

		case A2VRI_AIRPORTTILES_AIRPORT_ID: {
			if (this->source_spec != nullptr) return this->source_spec->grf_prop.local_id;
			const AirportSpec *spec = this->GetAirportSpec();
			return spec != nullptr ? spec->grf_prop.local_id : 0;
		}
	}

	if (this->source_layout != nullptr) {
		switch (variable) {
			case 0x43: {
				const uint32_t x = this->source_offset.x;
				const uint32_t y = this->source_offset.y;
				return ((y & 0xF) << 20) | ((x & 0xF) << 16) | (y << 8) | x;
			}

			case 0x60: {
				/* Airport membership comes from the source; terrain comes from the map. */
				uint32_t result = this->GetSourceTileAtOffset(parameter) != nullptr ? 0x100 : 0;
				if (has_world_tile && (extra.mask & ~0x100) != 0) {
					result |= GetNearbyTileInformation(GetNearbyTile(parameter, this->tile), this->ro.grffile->grf_version >= 8, extra.mask);
				}
				return result;
			}

			case 0x61: {
				if (this->GetSourceTileAtOffset(parameter) == nullptr) return UINT_MAX;
				if (!has_world_tile) return 0;
				const TileIndex neighbour = GetNearbyTile(parameter, this->tile);
				/* The structural neighbour exists even if no live animation occupies its destination. */
				return this->st->TileBelongsToAirport(neighbour) ? GetAnimationFrame(neighbour) : 0;
			}

			case 0x62: {
				const AirportTileTable *entry = this->GetSourceTileAtOffset(parameter);
				if (entry == nullptr) return 0xFFFF;
				const StationGfx gfx = GetSourceTileGfx(*entry, this->graphics->source_rotation);
				if (gfx == INVALID_AIRPORTTILE) {
					const bool is_infrastructure = entry->type == ATT_INFRASTRUCTURE_WITH_CATCH || entry->type == ATT_INFRASTRUCTURE_NO_CATCH;
					return 0xFF00 | (is_infrastructure ? entry->at_gfx : ATTG_DEFAULT_GFX);
				}
				return GetAirportTileIDForGraphics(gfx, this->ro.grffile->grfid);
			}
		}
	}

	if (!has_world_tile) {
		if (this->graphics != nullptr && variable == 0x44) return 0;
		extra.available = false;
		return UINT_MAX;
	}

	switch (variable) {
		/* Terrain type */
		case 0x41: return GetTerrainType(this->tile);

		/* Current town zone of the tile in the nearest town */
		case 0x42: {
			const Town *town = ClosestTownFromTile(this->tile, UINT_MAX);
			return to_underlying(town != nullptr ? GetTownRadiusGroup(town, this->tile) : HouseZone::TownEdge);
		}

		/* Position relative to most northern airport tile. */
		case 0x43: return GetRelativePosition(this->tile, this->st->airport.tile);

		/* Animation frame of tile */
		case 0x44: return GetAnimationFrame(this->tile);

		/* Land info of nearby tiles */
		case 0x60: return GetNearbyAirportTileInformation(parameter, this->tile, this->st->index, this->ro.grffile->grf_version >= 8, extra.mask);

		/* Animation stage of nearby tiles */
		case 0x61: {
			TileIndex tile = GetNearbyTile(parameter, this->tile);
			if (this->st->TileBelongsToAirport(tile)) {
				return GetAnimationFrame(tile);
			}
			return UINT_MAX;
		}

		/* Get airport tile ID at offset */
		case 0x62: {
			const TileIndex neighbour = GetNearbyTile(parameter, this->tile);
			return this->st->TileBelongsToAirport(neighbour) ? GetAirportTileIDForGraphics(GetAirportGfx(neighbour), this->ro.grffile->grfid) : 0xFFFF;
		}
	}

	Debug(grf, 1, "Unhandled airport tile variable 0x{:X}", variable);

	extra.available = false;
	return UINT_MAX;
}

/* virtual */ uint32_t AirportTileScopeResolver::GetRandomBits() const
{
	return (this->st == nullptr ? 0 : this->st->random_bits) | (this->tile == INVALID_TILE ? 0 : GetStationTileRandomBits(this->tile) << 16);
}

/**
 * Constructor of the resolver for airport tiles.
 * @param ats Specification of the airport tiles.
 * @param tile %Tile for the callback, only valid for airporttile callbacks.
 * @param st Station of the airport for which the callback is run, or \c nullptr for build gui.
 * @param callback Callback ID.
 * @param callback_param1 First parameter (var 10) of the callback.
 * @param callback_param2 Second parameter (var 18) of the callback.
 * @param graphics Optional source record for a GUI preview. Map draws look it up by tile.
 */
AirportTileResolverObject::AirportTileResolverObject(const AirportTileSpec *ats, TileIndex tile, Station *st,
		CallbackID callback, uint32_t callback_param1, uint32_t callback_param2, const AirportTileGraphics *graphics)
	: ResolverObject(ats->grf_prop.grffile, callback, callback_param1, callback_param2),
			tiles_scope(*this, ats, tile, st, graphics),
			airport_scope(*this, tile, st, this->tiles_scope.GetAirportSpec(), this->tiles_scope.GetAirportLayout(), this->tiles_scope.graphics == nullptr)
{
	this->root_spritegroup = ats->grf_prop.GetSpriteGroup(st != nullptr || this->tiles_scope.graphics != nullptr);
}

GrfSpecFeature AirportTileResolverObject::GetFeature() const
{
	return GrfSpecFeature::AirportTiles;
}

uint32_t AirportTileResolverObject::GetDebugID() const
{
	return this->tiles_scope.ats->grf_prop.local_id;
}

uint16_t GetAirportTileCallback(CallbackID callback, uint32_t param1, uint32_t param2, const AirportTileSpec *ats, Station *st, TileIndex tile, int extra_data = 0)
{
	AirportTileResolverObject object(ats, tile, st, callback, param1, param2);
	return object.ResolveCallback();
}

static void AirportDrawTileLayout(const TileInfo *ti, const TileLayoutSpriteGroup *group, PaletteID palette)
{
	auto processor = group->ProcessRegisters(nullptr);
	auto dts = processor.GetLayout();

	SpriteID image = dts.ground.sprite;
	SpriteID pal = dts.ground.pal;

	if (GB(image, 0, SPRITE_WIDTH) != 0) {
		if (image == SPR_FLAT_WATER_TILE && IsTileOnWater(ti->tile)) {
			DrawWaterClassGround(ti);
		} else {
			DrawGroundSprite(image, GroundSpritePaletteTransform(image, pal, palette));
		}
	}

	DrawNewGRFTileSeq(ti, &dts, TransparencyOption::Buildings, 0, palette);
}

bool DrawNewAirportTile(TileInfo *ti, Station *st, const AirportTileSpec *airts)
{
	bool draw_foundation = ti->tileh != SLOPE_FLAT;
	if (draw_foundation && airts->callback_mask.Test(AirportTileCallbackMask::DrawFoundations)) {
		uint32_t result = GetAirportTileCallback(CBID_AIRPTILE_DRAW_FOUNDATIONS, 0, 0, airts, st, ti->tile);
		if (result != CALLBACK_FAILED) draw_foundation = ConvertBooleanCallback(airts->grf_prop.grffile, CBID_AIRPTILE_DRAW_FOUNDATIONS, result);
	}

	AirportTileResolverObject object(airts, ti->tile, st);
	const TileLayoutSpriteGroup *group = object.Resolve<TileLayoutSpriteGroup>();
	if (group == nullptr) return false;

	/* Leave TileInfo untouched if the caller needs to draw the fallback instead. */
	if (draw_foundation) DrawFoundation(ti, Foundation::Leveled);

	const PaletteID palette = Company::IsValidID(st->owner) ? GetCompanyPalette(st->owner) : PALETTE_TO_GREY;
	AirportDrawTileLayout(ti, group, palette);
	return true;
}

bool DrawAirportTileGraphicsInGUI(int x, int y, const AirportTileGraphics &graphics, PaletteID palette)
{
	const StationGfx gfx = ResolveAirportTileGraphics(graphics);
	if (gfx == INVALID_AIRPORTTILE) return false;

	AirportTileResolverObject object(AirportTileSpec::Get(gfx), INVALID_TILE, nullptr, CBID_NO_CALLBACK, 0, 0, &graphics);
	const TileLayoutSpriteGroup *group = object.Resolve<TileLayoutSpriteGroup>();
	if (group == nullptr) return false;

	auto processor = group->ProcessRegisters(nullptr);
	auto dts = processor.GetLayout();
	const SpriteID image = dts.ground.sprite;
	if (GB(image, 0, SPRITE_WIDTH) != 0) {
		DrawSprite(image, GroundSpritePaletteTransform(image, dts.ground.pal, palette), x, y);
	}
	DrawNewGRFTileSeqInGUI(x, y, &dts, 0, palette);
	return true;
}

/** Helper class for animation control. */
struct AirportTileAnimationBase : public AnimationBase<AirportTileAnimationBase, AirportTileSpec, Station, int, GetAirportTileCallback, TileAnimationFrameAnimationHelper<Station> > {
	static constexpr CallbackID cb_animation_speed      = CBID_AIRPTILE_ANIMATION_SPEED;
	static constexpr CallbackID cb_animation_next_frame = CBID_AIRPTILE_ANIMATION_NEXT_FRAME;

	static constexpr AirportTileCallbackMask cbm_animation_speed      = AirportTileCallbackMask::AnimationSpeed;
	static constexpr AirportTileCallbackMask cbm_animation_next_frame = AirportTileCallbackMask::AnimationNextFrame;
};

/** Install an optional material without touching the tile's fallback, topology or animation. */
void InstallAirportTileGraphics(Station *st, TileIndex tile, const AirportTileGraphics *graphics)
{
	if (graphics == nullptr) {
		st->airport.tile_graphics.erase(tile);
	} else {
		assert(IsInfrastructure(tile));
		st->airport.tile_graphics.insert_or_assign(tile, AirportTileAppearance{*graphics, ResolveAirportTileGraphics(*graphics) == INVALID_AIRPORTTILE});
	}
}

/** Start from frame zero without running any callback or consuming game RNG. */
void ResetAirportTileAnimation(TileIndex tile, bool start_one_shot)
{
	SetAnimationFrame(tile, 0);
	const AnimationStatus status = AirportTileSpec::GetByTile(tile)->animation.status;
	if (status == AnimationStatus::Looping || (start_one_shot && status == AnimationStatus::NonLooping)) {
		AddAnimatedTile(tile, false);
	} else {
		DeleteAnimatedTile(tile);
	}
}

/** The loaded frame and ANIT entry already belong to this material unless its availability changed. */
void ReconcileAirportTileGraphicsAnimation(TileIndex tile, AirportTileAppearance &appearance)
{
	const bool uses_fallback = ResolveAirportTileGraphics(appearance.graphics) == INVALID_AIRPORTTILE;
	if (appearance.uses_fallback == uses_fallback) return;
	appearance.uses_fallback = uses_fallback;
	/* Do not fire Built or restart one-shot animations while loading. */
	ResetAirportTileAnimation(tile);
}

void AnimateAirportTile(TileIndex tile)
{
	const AirportTileSpec *ats = AirportTileSpec::GetByTile(tile);
	if (ats == nullptr) return;

	AirportTileAnimationBase::AnimateTile(ats, Station::GetByTile(tile), tile, HasBit(ats->animation_special_flags, 0));
}

static bool DoTriggerAirportTileAnimation(Station *st, TileIndex tile, AirportAnimationTrigger trigger, uint32_t random, uint32_t var18_extra = 0)
{
	const AirportTileSpec *ats = AirportTileSpec::GetByTile(tile);
	if (!ats->animation.triggers.Test(trigger)) return false;

	AirportTileAnimationBase::ChangeAnimationFrame(CBID_AIRPTILE_ANIMATION_TRIGGER, ats, st, tile, random, to_underlying(trigger) | var18_extra);
	return true;
}

bool TriggerAirportTileAnimation(Station *st, TileIndex tile, AirportAnimationTrigger trigger)
{
	return DoTriggerAirportTileAnimation(st, tile, trigger, Random());
}

/** Call after all of the airport topology and materials have been installed. Tiles must be in map order. */
void InitializeBuiltAirportTiles(std::span<const TileIndex> tiles)
{
	for (TileIndex tile : tiles) ResetAirportTileAnimation(tile, true);
	for (TileIndex tile : tiles) {
		if (AirportTileSpec::GetByTile(tile)->animation.triggers.Test(AirportAnimationTrigger::Built)) {
			TriggerAirportTileAnimation(Station::GetByTile(tile), tile, AirportAnimationTrigger::Built);
		}
	}
}

bool TriggerAirportAnimation(Station *st, AirportAnimationTrigger trigger, CargoType cargo_type)
{
	if (st->airport.IsEmpty()) return false;

	bool ret = true;
	uint32_t random = Random();
	for (TileIndex tile : st->airport) {
		if (!st->TileBelongsToAirport(tile)) continue;

		const AirportTileSpec *ats = AirportTileSpec::GetByTile(tile);
		if (ats->grf_prop.grffile == nullptr) continue;

		uint32_t var18_extra = 0;
		if (IsValidCargoType(cargo_type)) {
			var18_extra = uint32_t{ats->grf_prop.grffile->cargo_map[cargo_type]} << 8;
		}

		if (DoTriggerAirportTileAnimation(st, tile, trigger, random, var18_extra)) {
			SB(random, 0, 16, Random());
		} else {
			ret = false;
		}
	}
	return ret;
}

uint8_t GetAirportTileAnimationSpeed(TileIndex tile)
{
	const AirportTileSpec *ats = AirportTileSpec::GetByTile(tile);
	if (ats == nullptr) return 0;

	return AirportTileAnimationBase::GetAnimationSpeed(ats);
}

/** Collect individual source tiles which offer NewGRF graphics for this infrastructure type. */
void GetGRFAirportTileCandidates(AirportTileType type, std::vector<AirportTileGfxSet> &candidates)
{
	candidates.clear();
	if (type != ATT_INFRASTRUCTURE_WITH_CATCH && type != ATT_INFRASTRUCTURE_NO_CATCH) return;

	for (uint i = 0; i < NUM_AIRPORTS; i++) {
		const AirportSpec *spec = AirportSpec::GetWithoutOverride(i);
		if (!spec->enabled) continue;

		for (uint l = 0; l < spec->layouts.size() && l <= UINT8_MAX; l++) {
			const AirportTileLayout &layout = spec->layouts[l];
			if (layout.size_x == 0 || layout.tiles.size() != static_cast<size_t>(layout.size_x) * layout.size_y) continue;

			for (uint pos = 0; pos < layout.tiles.size(); pos++) {
				if (layout.tiles[pos].type != type) continue;

				/* Identical tile IDs can draw differently at different positions in the source layout. */
				AirportTileGfxSet set;
				bool has_variant = false;
				for (DiagDirection dir = DiagDirection::Begin; dir < DiagDirection::End; dir++) {
					auto &variant = set.variants[to_underlying(dir)];
					variant = MakeAirportTileGraphics(spec, l, pos, dir);
					has_variant |= variant.has_value();
				}
				if (has_variant) candidates.push_back(std::move(set));
			}
		}
	}
}
