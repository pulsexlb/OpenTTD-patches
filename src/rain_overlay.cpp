/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file rain_overlay.cpp Screen-space scrolling rain overlay. */

#include "stdafx.h"
#include "rain_overlay.h"
#include "weather.h"
#include "blitter/factory.hpp"
#include "gfx_type.h"
#include "palette_func.h"
#include "spritecache.h"
#include "zoom_type.h"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <vector>

#include "safeguards.h"

/** Side length (in screen pixels at normal zoom) of one rain texture tile. */
static constexpr int RAIN_TILE = 128;
/**
 * Number of animation frames per (layer, variant). The frames form a seamless
 * falling loop: each frame is the previous one shifted down the fall direction
 * by RAIN_TILE / RAIN_FRAMES pixels (times the layer's fall step count).
 */
static constexpr int RAIN_FRAMES = 12;
/** Number of parallax layers; far to near. */
static constexpr int RAIN_LAYERS = 3;
/** Number of layout variants per (layer, frame); picked per grid cell to break up tiling regularity. */
static constexpr int RAIN_VARIANTS = 4;
/** Zoom-out steps (from ZoomLevel::Normal) after which rain is not drawn at all. */
static constexpr int RAIN_MAX_ZOOM_OUT = 3;
/** Zoom-in steps that still enlarge the rain; further zoom-in reuses the largest size. */
static constexpr int RAIN_MAX_ZOOM_IN = 1;
/** Number of supported zoom deltas, from zoomed in to zoomed out. */
static constexpr int RAIN_ZOOM_COUNT = RAIN_MAX_ZOOM_IN + 1 + RAIN_MAX_ZOOM_OUT;

/** Parameters of one rain layer. */
struct RainLayerParams {
	int speed_x;          ///< Horizontal scroll speed, screen pixels per second (wind drift).
	int speed_y;          ///< Vertical speed; only used as the streak slope, the fall itself is the frame animation.
	int streak_count;     ///< Streaks per texture tile.
	int len_min;          ///< Shortest streak length in pixels.
	int len_extra;        ///< Maximum extra length.
	uint8_t base_alpha;   ///< Base opacity of the streaks (before the rain fade is applied).
	int fall_interval;    ///< Milliseconds between animation frames.
	int fall_steps;       ///< Fall animation steps (of RAIN_TILE / RAIN_FRAMES pixels) per frame.
};

static constexpr RainLayerParams RAIN_LAYER_PARAMS[RAIN_LAYERS] = {
	/* Far layer: many thin, short, faint streaks, moving slowly. */
	{  12,  34, 26,  5,  5, 35, 240, 1 },
	/* Middle layer. */
	{  17,  50, 16,  9,  8, 55, 120, 1 },
	/* Near layer: fewer, longer and more opaque streaks, moving fast. */
	{  24,  70,  9, 14, 10, 75, 120, 2 },
};

/** One encoded frame of one layer. */
struct RainFrame {
	const Sprite *sprite = nullptr;
	UniquePtrSpriteAllocator allocator;
};

/** All encoded frames per zoom delta (indexed [delta][layer][variant][frame]), plus what they were built for. */
struct RainOverlayCache {
	std::vector<RainFrame> frames;
	std::bitset<RAIN_ZOOM_COUNT> built; ///< Which zoom deltas have their frames generated.
	const Blitter *blitter = nullptr;
	uint8_t shade_level = 0xFF; ///< Shade level the current alpha scaling was made for.

	void Invalidate()
	{
		this->frames.clear();
		this->frames.resize(RAIN_ZOOM_COUNT * RAIN_LAYERS * RAIN_VARIANTS * RAIN_FRAMES);
		this->built.reset();
		this->blitter = nullptr;
		this->shade_level = 0xFF;
	}

	bool Matches(const Blitter *blitter, uint8_t shade_level) const
	{
		return this->blitter == blitter && this->shade_level == shade_level;
	}
};

static RainOverlayCache _rain_cache;

/** Map a viewport zoom level to a delta (in steps) from ZoomLevel::Normal, clamped to the supported range. */
static int RainZoomDelta(ZoomLevel zoom)
{
	return Clamp(to_underlying(zoom) - to_underlying(ZoomLevel::Normal), -RAIN_MAX_ZOOM_IN, RAIN_MAX_ZOOM_OUT);
}

/** Texture tile side length (in screen pixels) for a zoom delta. */
static int RainTileSize(int delta)
{
	return delta < 0 ? RAIN_TILE << -delta : RAIN_TILE >> delta;
}

/** Cheap deterministic hash; all overlay randomness flows through this. */
static uint32_t RainHash(uint32_t a, uint32_t b, uint32_t c)
{
	uint32_t h = RainSeed();
	h ^= a * 0x9E3779B9u;
	h ^= b * 0x85EBCA6Bu;
	h ^= c * 0xC2B2AE35u;
	h ^= h >> 15;
	h *= 0x2545F491u;
	h ^= h >> 13;
	return h;
}

/** Milliseconds elapsed on the monotonic clock. */
static uint64_t RainNowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
}

/**
 * Generate one texture tile: a pattern of slanted rain streaks that wraps
 * around the tile edges, so it can be tiled seamlessly.
 */
static void GenerateRainTile(SpriteLoader::Sprite &sprite, uint layer, uint variant, uint frame, int tile_size, uint8_t alpha_scale)
{
	const RainLayerParams &params = RAIN_LAYER_PARAMS[layer];

	sprite.width = tile_size;
	sprite.height = tile_size;
	sprite.x_offs = 0;
	sprite.y_offs = 0;
	sprite.AllocateData(ZoomLevel::Min, tile_size * tile_size);

	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	bool use_rgba = blitter->GetScreenDepth() >= 32;
	if (use_rgba) {
		sprite.colours.Set(SpriteComponent::RGB);
		sprite.colours.Set(SpriteComponent::Alpha);
	} else {
		sprite.colours = SpriteComponent::Palette;
	}

	/* Texture transparency is index 0 (8bpp) / alpha 0 (32bpp). */
	for (int i = 0; i < tile_size * tile_size; i++) {
		sprite.data[i].r = 0;
		sprite.data[i].g = 0;
		sprite.data[i].b = 0;
		sprite.data[i].a = 0;
		sprite.data[i].m = 0;
	}

	/* Slope of the streaks, matching the layer's fall direction. */
	const int slope_x_num = params.speed_x;
	const int slope_x_den = params.speed_y;

	/* All frames of a (layer, variant) share one streak layout; each frame is
	 * shifted further along the fall direction, forming a seamless loop. The
	 * shift follows the streak slope, so the drops move along their own
	 * orientation instead of sliding sideways. */
	uint32_t shift_rnd = RainHash(0x5A17, layer * RAIN_VARIANTS + variant, 0);
	int shift = (frame * params.fall_steps % RAIN_FRAMES) * tile_size / RAIN_FRAMES;
	int shift_x = shift * params.speed_x / params.speed_y;

	for (int s = 0; s < params.streak_count; s++) {
		uint32_t rnd = RainHash(shift_rnd, s, 0);
		int x = rnd % tile_size;
		int y = (rnd >> 8) % tile_size;
		/* Streak lengths scale with the zoom level the tile is for. */
		int len = std::max(1, (params.len_min + (int)((rnd >> 16) % params.len_extra)) * tile_size / RAIN_TILE);
		/* Opacity flickers per streak; some streaks are hidden in some frames,
		 * so individual drops appear and disappear while falling. */
		if (RainHash(0x717, layer * RAIN_VARIANTS + variant, s * RAIN_FRAMES + frame) % 10 == 0) continue;
		uint8_t alpha = params.base_alpha * (60 + (rnd >> 4) % 81) / 160;
		uint8_t shade = 185 + (rnd >> 8) % 40; /* Pale blue-grey variation. */

		for (int i = 0; i < len; i++) {
			int px = (x + i * slope_x_num / slope_x_den + shift_x + tile_size) % tile_size;
			int py = (y + i + shift) % tile_size;
			auto &p = sprite.data[py * tile_size + px];
			if (use_rgba) {
				p.r = shade * 9 / 10;
				p.g = shade;
				p.b = std::min(shade + 25, 255);
				p.a = alpha * alpha_scale / 255;
			} else {
				p.m = PC_LIGHT_BLUE.p;
				p.a = 0xFF;
			}
		}
	}
}

/** (Re)build the encoded rain frames of one zoom delta for the current blitter and fade level. */
static void BuildRainFrames(int delta)
{
	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	int tile_size = RainTileSize(delta);
	uint8_t alpha_scale = RainShadeProgress();

	const uint base = (delta + RAIN_MAX_ZOOM_IN) * RAIN_LAYERS * RAIN_VARIANTS * RAIN_FRAMES;
	for (uint layer = 0; layer < RAIN_LAYERS; layer++) {
		for (uint variant = 0; variant < RAIN_VARIANTS; variant++) {
			for (uint frame = 0; frame < RAIN_FRAMES; frame++) {
				SpriteLoader::SpriteCollection collection;
				SpriteLoader::Sprite &sprite = collection.Root();
				GenerateRainTile(sprite, layer, variant, frame, tile_size, alpha_scale);

				RainFrame f;
				UniquePtrSpriteAllocator allocator;
				f.sprite = blitter->Encode(SpriteType::Normal, collection, allocator);
				f.allocator.data = std::move(allocator.data);
				_rain_cache.frames[base + (layer * RAIN_VARIANTS + variant) * RAIN_FRAMES + frame] = std::move(f);
			}
		}
	}
	_rain_cache.built.set(delta + RAIN_MAX_ZOOM_IN);
}

/**
 * Blit one texture tile at the given screen position, clipped to the
 * destination rectangle.
 */
static void BlitRainTile(Blitter *blitter, const Sprite *sprite, const DrawPixelInfo *dpi, int tile_size, int px, int py)
{
	int left = std::max(px, dpi->left);
	int top = std::max(py, dpi->top);
	int right = std::min(px + tile_size, dpi->left + dpi->width);
	int bottom = std::min(py + tile_size, dpi->top + dpi->height);
	if (left >= right || top >= bottom) return;

	Blitter::BlitterParams bp;
	bp.sprite = sprite->data;
	bp.sprite_width = sprite->width;
	bp.sprite_height = sprite->height;
	bp.skip_left = left - px;
	bp.skip_top = top - py;
	bp.width = right - left;
	bp.height = bottom - top;
	bp.left = left - dpi->left;
	bp.top = top - dpi->top;
	bp.dst = dpi->dst_ptr;
	bp.pitch = dpi->pitch;
	bp.remap = nullptr;
	bp.brightness_adjust = 0;

	blitter->Draw(&bp, BlitterMode::Normal, ZoomLevel::Min);
}

void DrawRainOverlay(ZoomLevel zoom, const DrawPixelInfo *dpi)
{
	if (_rain_shade_level == 0) return;

	int delta = RainZoomDelta(zoom);
	if (to_underlying(zoom) - to_underlying(ZoomLevel::Normal) > RAIN_MAX_ZOOM_OUT) return;

	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	if (!_rain_cache.Matches(blitter, _rain_shade_level)) _rain_cache.Invalidate();
	if (!_rain_cache.built.test(delta + RAIN_MAX_ZOOM_IN)) {
		_rain_cache.blitter = blitter;
		_rain_cache.shade_level = _rain_shade_level;
		BuildRainFrames(delta);
	}

	int tile_size = RainTileSize(delta);

	uint64_t now = RainNowMs();

	for (uint layer = 0; layer < RAIN_LAYERS; layer++) {
		const RainLayerParams &params = RAIN_LAYER_PARAMS[layer];

		/* The only motion is the fall animation along the streak slope; the
		 * pattern itself does not drift or jitter, so nothing appears to move
		 * sideways. */
		uint frame = (uint)(now / params.fall_interval) % RAIN_FRAMES;
		const RainFrame *frame_base = &_rain_cache.frames[(delta + RAIN_MAX_ZOOM_IN) * RAIN_LAYERS * RAIN_VARIANTS * RAIN_FRAMES + layer * RAIN_VARIANTS * RAIN_FRAMES];

		/* Tile positions align to absolute screen coordinates, so the pattern
		 * is continuous across viewports and stable while panning. */
		int first_x = dpi->left - (dpi->left % tile_size + tile_size) % tile_size;
		int first_y = dpi->top - (dpi->top % tile_size + tile_size) % tile_size;

		for (int py = first_y; py < dpi->top + dpi->height; py += tile_size) {
			for (int px = first_x; px < dpi->left + dpi->width; px += tile_size) {
				/* Pattern-cell coordinates: constant per cell, so its random
				 * variant and jitter do not flicker. */
				int cx = px / tile_size;
				int cy = py / tile_size;
				uint32_t rnd = RainHash(0xCE11 + layer, (uint32_t)(cx * 4096 + cy), 0);
				/* Pick a fixed layout variant per cell and nudge the cell a
				 * little, so no grid of repeating rain is visible, especially
				 * when zoomed out. */
				uint variant = rnd % RAIN_VARIANTS;
				int jx = (int)(rnd >> 8) % (tile_size / 4 + 1) - tile_size / 8;
				int jy = (int)(rnd >> 16) % (tile_size / 4 + 1) - tile_size / 8;
				BlitRainTile(blitter, frame_base[variant * RAIN_FRAMES + frame].sprite, dpi, tile_size, px + jx, py + jy);
			}
		}
	}
}
