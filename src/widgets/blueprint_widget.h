/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint_widget.h Types related to the blueprint widgets. */

#ifndef WIDGETS_BLUEPRINT_WIDGET_H
#define WIDGETS_BLUEPRINT_WIDGET_H

/** Widgets of the #BlueprintToolbarWindow class. */
enum BlueprintToolbarWidgets : WidgetID {
	WID_BT_PAGE,             ///< Dropdown selecting which page of blueprint slots the slot buttons show.
	WID_BT_SLOT_1,           ///< Switch to the first blueprint slot of the active page.
	WID_BT_SLOT_2,           ///< Switch to the second blueprint slot of the active page.
	WID_BT_SLOT_3,           ///< Switch to the third blueprint slot of the active page.
	WID_BT_SLOT_4,           ///< Switch to the fourth blueprint slot of the active page.
	WID_BT_SLOT_5,           ///< Switch to the fifth blueprint slot of the active page.
	WID_BT_SLOT_6,           ///< Switch to the sixth blueprint slot of the active page.
	WID_BT_SLOT_7,           ///< Switch to the seventh blueprint slot of the active page.
	WID_BT_SLOT_8,           ///< Switch to the eighth blueprint slot of the active page.
	WID_BT_SLOT_SCRATCH,     ///< Switch to the scratch slot: a page-independent buffer that is never stored.
	WID_BT_COPY,             ///< Select an area to copy into the active slot.
	WID_BT_PASTE,            ///< Paste the active slot onto the map.
	WID_BT_TRANSFORMATION,   ///< Shows the current transformation; click to reset it.
	WID_BT_ROTATE_CCW,       ///< Rotate the blueprint 90 degrees anticlockwise.
	WID_BT_ROTATE_CW,        ///< Rotate the blueprint 90 degrees clockwise.
	WID_BT_REFLECT_NW_SE,    ///< Reflect the blueprint against the NW-SE axis.
	WID_BT_REFLECT_NE_SW,    ///< Reflect the blueprint against the NE-SW axis.
	WID_BT_HEIGHT_DISPLAY,   ///< Shows the height offset applied when pasting.
	WID_BT_HEIGHT_UP,        ///< Increase the paste height offset.
	WID_BT_HEIGHT_DOWN,      ///< Decrease the paste height offset.
	WID_BT_EXPORT_IMPORT,    ///< Opens a menu: export/import the active slot as text, or the active page to/from a file.
	WID_BT_PASTE_OPTIONS,    ///< Opens a menu holding the paste filters, the rail/signal/bridge options and the terraform mode.
	WID_BT_INFO,             ///< Show the Blueprint patch version.
};

/** Widgets of the #BlueprintUndoToolbarWindow class. */
enum BlueprintUndoToolbarWidgets : WidgetID {
	WID_BUT_UNDO,         ///< Undo the last recorded action.
	WID_BUT_LIST,         ///< Show the undo history.
	WID_BUT_OPTIONS,      ///< Opens a menu holding the undo filters and the restore-land toggle.
	WID_BUT_INFO,         ///< Version and description menu.
};

/** Widgets of the #BlueprintUndoListWindow class (undo history). */
enum BlueprintUndoListWidgets : WidgetID {
	WID_BUL_LIST,          ///< List of undoable actions, newest first.
	WID_BUL_SCROLLBAR,     ///< Scrollbar of #WID_BUL_LIST.
	WID_BUL_UNDO_SELECTED, ///< Undo the selected actions.
};

/** Widgets of the #BlueprintInfoWindow class (what a toolbar is and how it works). */
enum BlueprintInfoWidgets : WidgetID {
	WID_BIN_CAPTION, ///< Window caption, naming the toolbar described.
	WID_BIN_TEXT,    ///< The description itself.
};

/** Widgets of the #BlueprintTextWindow class (blueprint text export/import dialog). */
enum BlueprintTextWidgets : WidgetID {
	WID_BTX_CAPTION, ///< Window caption (export or import).
	WID_BTX_TEXT,    ///< Editbox holding the blueprint text.
	WID_BTX_SEL,     ///< Selection between the export and import button rows.
	WID_BTX_COPY,    ///< Copy the text to the clipboard (export mode).
	WID_BTX_PASTE,   ///< Paste the clipboard contents into the text field (import mode).
	WID_BTX_CANCEL,  ///< Close without importing (import mode).
	WID_BTX_OK,      ///< Import the entered text (import mode).
};

/** Widgets of the #BlueprintFileWindow class (export/import a page of slots to/from a file). */
enum BlueprintFileWidgets : WidgetID {
	WID_BTF_CAPTION,   ///< Window caption (export or import).
	WID_BTF_PATH,      ///< Shows the directory currently being browsed.
	WID_BTF_LIST,      ///< List of files and subdirectories in the current directory.
	WID_BTF_SCROLLBAR, ///< Scrollbar of #WID_BTF_LIST.
	WID_BTF_DETAILS,   ///< Panel describing the selected file.
	WID_BTF_NAME,      ///< Editbox holding the file name (without extension).
	WID_BTF_DELETE,    ///< Delete the selected file.
	WID_BTF_CANCEL,    ///< Close without exporting/importing.
	WID_BTF_OK,        ///< Export or import using the current directory and file name.
};

#endif /* WIDGETS_BLUEPRINT_WIDGET_H */
