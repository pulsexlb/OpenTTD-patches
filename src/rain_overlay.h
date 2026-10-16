/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file rain_overlay.h Screen-space scrolling rain overlay. */

#ifndef RAIN_OVERLAY_H
#define RAIN_OVERLAY_H

struct DrawPixelInfo;
enum class ZoomLevel : uint8_t;

/**
 * Draw the scrolling rain overlay over the given (screen-space) rectangle.
 * Uses deterministic hashing for all randomness, so it is stateless and
 * purely cosmetic; nothing here touches synchronised game state.
 * @param zoom Viewport zoom level; the rain scales with it.
 * @param dpi Screen-space rectangle (as returned by ViewportDrawerDynamic::MakeDPIForText()).
 */
void DrawRainOverlay(ZoomLevel zoom, const DrawPixelInfo *dpi);

#endif /* RAIN_OVERLAY_H */
