/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint_gui.h GUI stuff related to the blueprint (copy and paste) feature. */

#ifndef BLUEPRINT_GUI_H
#define BLUEPRINT_GUI_H

#include "tile_type.h"
#include "track_type.h"
#include "window_type.h"

/** Bit in a #BlueprintPastePreview tile marking an element footprint highlight. */
static const uint BLUEPRINT_PREVIEW_HIGHLIGHT_BIT = 7;

/** Per-tile overlays shown in the viewport while the paste tool is active. */
struct BlueprintPastePreview {
	uint16_t width = 0;               ///< Size of the paste area along the X axis.
	uint16_t height = 0;              ///< Size of the paste area along the Y axis.
	std::vector<uint8_t> tiles;       ///< Per tile: bits 0..5 = #TrackBits overlay, bit 7 = footprint highlight.
	std::vector<bool> blocked;        ///< Per tile: whether this part cannot be built at the current position.
};

Window *ShowBlueprintToolbar();
Window *ShowBlueprintUndoToolbar();
void ShowBlueprintUndoList();
TileIndex GetBlueprintPasteOrigin(TileIndex cursor);
const BlueprintPastePreview *GetBlueprintPastePreview(TileIndex origin);

/* Tiles the selected undo history entries cover, highlighted on the map. */
bool IsTileInBlueprintUndoHighlight(TileIndex tile);

/* The undo button also sits on the construction toolbars, which reach it through these. */
bool HasBlueprintUndoRecord();
void UndoLastBlueprintPaste();

#endif /* BLUEPRINT_GUI_H */
