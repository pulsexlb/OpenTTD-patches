/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint_gui.cpp GUI for the blueprint (copy and paste) feature. */

#include "stdafx.h"
#include "blueprint.h"
#include "blueprint_cmd.h"
#include <set>
#include "blueprint_gui.h"
#include "blueprint_version.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "core/math_func.hpp"
#include "dropdown_func.h"
#include "error.h"
#include "fileio_func.h"
#include "gfx_func.h"
#include "hotkeys.h"
#include "map_func.h"
#include "palette_func.h"
#include "zoom_func.h"
#include "network/network.h"
#include "newgrf_roadstop.h"
#include "newgrf_station.h"
#include "newgrf_object.h"
#include "object_cmd.h"
#include "querystring_gui.h"
#include "textbuf_gui.h"
#include "air.h"
#include "airport_cmd.h"
#include "rail_cmd.h"
#include "rail_gui.h"
#include "road_cmd.h"
#include "road_func.h"
#include "sound_func.h"
#include "station_cmd.h"
#include "strings_func.h"
#include "tile_map.h"
#include "tilehighlight_func.h"
#include "track_func.h"
#include "tunnelbridge_cmd.h"
#include "viewport_func.h"
#include "water_cmd.h"
#include "water_map.h"
#include "waypoint_cmd.h"
#include "window_func.h"
#include "window_gui.h"

#include <algorithm>
#include <filesystem>

#include "widgets/blueprint_widget.h"
#include "widgets/misc_widget.h"

#include "table/sprites.h"
#include "table/strings.h"

#include "safeguards.h"

bool SetClipboardContents(const std::string &contents); ///< Implemented per OS.

/** Identifying prefix of a blueprint in clipboard text form; the trailing digit is the format version. */
static constexpr std::string_view BLUEPRINT_CLIPBOARD_PREFIX = "OTTD-BP-5;";

/** Also accept older exports: v3 has no styles, v4 has station styles only. */
static bool HasBlueprintTextPrefix(std::string_view text, std::string_view prefix)
{
	if (text.starts_with(prefix)) return true;
	std::string legacy(prefix);
	for (char version : {'3', '4'}) {
		legacy[legacy.size() - 2] = version;
		if (text.starts_with(legacy)) return true;
	}
	return false;
}

/** Base64 alphabet used for blueprint clipboard strings. */
static constexpr std::string_view BASE64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** Encode bytes as base64 (RFC 4648, with padding). */
static std::string Base64Encode(std::span<const uint8_t> data)
{
	std::string result;
	result.reserve((data.size() + 2) / 3 * 4);
	for (size_t i = 0; i < data.size(); i += 3) {
		uint32_t chunk = data[i] << 16;
		if (i + 1 < data.size()) chunk |= data[i + 1] << 8;
		if (i + 2 < data.size()) chunk |= data[i + 2];
		result.push_back(BASE64_ALPHABET[(chunk >> 18) & 0x3F]);
		result.push_back(BASE64_ALPHABET[(chunk >> 12) & 0x3F]);
		result.push_back(i + 1 < data.size() ? BASE64_ALPHABET[(chunk >> 6) & 0x3F] : '=');
		result.push_back(i + 2 < data.size() ? BASE64_ALPHABET[chunk & 0x3F] : '=');
	}
	return result;
}

/** Decode base64 text; whitespace is not tolerated. */
static std::optional<std::vector<uint8_t>> Base64Decode(std::string_view text)
{
	while (!text.empty() && text.back() == '=') text.remove_suffix(1);
	std::vector<uint8_t> result;
	result.reserve(text.size() * 3 / 4);
	uint32_t chunk = 0;
	uint bits = 0;
	for (char c : text) {
		size_t value = BASE64_ALPHABET.find(c);
		if (value == std::string_view::npos) return std::nullopt;
		chunk = (chunk << 6) | static_cast<uint32_t>(value);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			result.push_back(static_cast<uint8_t>(chunk >> bits));
		}
	}
	return result;
}

/**
 * Try to parse a blueprint from its text form.
 * @param text The text to parse; surrounding whitespace is tolerated.
 * @param[out] blueprint Filled with the parsed blueprint on success.
 * @return Whether the text was a valid blueprint.
 */
static bool ParseBlueprintText(std::string_view text, Blueprint &blueprint)
{
	if (text.size() > 8 * 1024 * 1024) return false;

	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
	while (!text.empty() && (text.front() == '\n' || text.front() == '\r' || text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
	if (!HasBlueprintTextPrefix(text, BLUEPRINT_CLIPBOARD_PREFIX)) return false;

	auto bytes = Base64Decode(text.substr(BLUEPRINT_CLIPBOARD_PREFIX.size()));
	if (!bytes.has_value()) return false;

	Blueprint result;
	DeserialisationBuffer reader(bytes->data(), bytes->size());
	if (!DeserialiseBlueprint(reader, result, StringValidationSettings{})) return false;
	if (result.width == 0 || result.height == 0) return false;
	if (result.width > MAX_BLUEPRINT_DIMENSION || result.height > MAX_BLUEPRINT_DIMENSION) return false;
	if (result.IsEmpty()) return false;
	size_t corners = static_cast<size_t>(result.width + 1) * (result.height + 1);
	if (!result.corner_heights.empty() && result.corner_heights.size() != corners) return false;

	blueprint = std::move(result);
	return true;
}

/**
 * Encode a blueprint into its shareable text form.
 * @param blueprint The blueprint to encode.
 * @return The text form of the blueprint.
 */
static std::string EncodeBlueprintText(const Blueprint &blueprint)
{
	std::vector<uint8_t> buffer;
	BufferSerialisationRef writer(buffer);
	SerialiseBlueprint(writer, blueprint);
	std::string text{BLUEPRINT_CLIPBOARD_PREFIX};
	text += Base64Encode(buffer);
	return text;
}

/** Identifying prefix of one page of slots saved to a file; the trailing digit is the format version. */
static constexpr std::string_view BLUEPRINT_SET_FILE_PREFIX = "OTTD-BPSET-5;";

/** Identifying prefix of the autosave, which holds every page; the trailing digit is the format version. */
static constexpr std::string_view BLUEPRINT_ALL_FILE_PREFIX = "OTTD-BPALL-5;";

/** File extension used for exported blueprint sets, appended to the player-typed file name. */
static constexpr std::string_view BLUEPRINT_SET_FILE_EXTENSION = ".txt";

/** Maximum length of a blueprint set file name (without extension) in characters including '\0'. */
static const uint MAX_LENGTH_BLUEPRINT_FILE_NAME_CHARS = 48;

/**
 * Validate and trim a player-typed file name.
 * @param name Raw text entered by the player.
 * @return The trimmed name, or \c std::nullopt if it is empty or contains characters that are unsafe in a file name.
 */
static std::optional<std::string> SanitizeBlueprintFileName(std::string_view name)
{
	while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
	while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
	if (name.empty() || name == "." || name == "..") return std::nullopt;
	if (name.find_first_of("/\\:*?\"<>|") != std::string_view::npos) return std::nullopt;
	return std::string{name};
}

/**
 * Encode a run of blueprint slots into their shareable text form.
 * @param prefix Format prefix identifying what the run is: one page or every page.
 * @param first_slot First slot of the run, counted across all pages.
 * @param count Number of slots to encode.
 * @return The text form of those slots, in order.
 */
static std::string EncodeBlueprintSlotsText(std::string_view prefix, uint first_slot, uint count)
{
	std::vector<Blueprint> run;
	run.reserve(count);
	for (uint i = 0; i < count; i++) run.push_back(GetBlueprint(first_slot + i));

	std::vector<uint8_t> buffer;
	BufferSerialisationRef writer(buffer);
	WriteBlueprintVector(writer, run);

	std::string text{prefix};
	text += Base64Encode(buffer);
	return text;
}

/**
 * Try to parse a run of blueprint slots from its text form.
 * @param text The text to parse; surrounding whitespace is tolerated.
 * @param prefix Format prefix the text must carry.
 * @param count Number of slots the run must hold; a different length is rejected.
 * @param[out] slots Filled with exactly \a count blueprints on success.
 * @return Whether the text was a valid run of that many slots.
 */
static bool ParseBlueprintSlotsText(std::string_view text, std::string_view prefix, uint count, std::vector<Blueprint> &slots)
{
	if (text.size() > 8 * 1024 * 1024) return false;

	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
	while (!text.empty() && (text.front() == '\n' || text.front() == '\r' || text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
	if (!HasBlueprintTextPrefix(text, prefix)) return false;

	auto bytes = Base64Decode(text.substr(prefix.size()));
	if (!bytes.has_value()) return false;

	DeserialisationBuffer reader(bytes->data(), bytes->size());
	std::vector<Blueprint> result;
	if (!ReadBlueprintVector(reader, result, StringValidationSettings{})) return false;
	if (result.size() != count) return false;

	for (const Blueprint &bp : result) {
		if (bp.width > MAX_BLUEPRINT_DIMENSION || bp.height > MAX_BLUEPRINT_DIMENSION) return false;
		size_t corners = static_cast<size_t>(bp.width + 1) * (bp.height + 1);
		if (!bp.corner_heights.empty() && bp.corner_heights.size() != corners) return false;
	}

	slots = std::move(result);
	return true;
}

/**
 * Absolute path of the blueprint set directory.
 *
 * Deliberately not resolved via FioFOpenFile()'s multi-searchpath lookup: that mechanism tries the current
 * working directory before the personal data directory, which is wrong here specifically because this patch's
 * own `blueprint/` folder (containing this very file) can sit inside the source tree - i.e. inside the working
 * directory used to launch the game - and would silently "win" over the real, writable personal directory.
 * Reading, writing and listing all go through this single, unambiguous path instead.
 * @return The directory, always ending in a path separator.
 */
static std::string GetBlueprintBaseDirectory()
{
	return _personal_dir + "blueprint" PATHSEP;
}

/**
 * Write a run of blueprint slots to a file in the blueprint directory.
 * @param file_name File name including extension, relative to #GetBlueprintBaseDirectory.
 * @param prefix Format prefix to write.
 * @param first_slot First slot of the run, counted across all pages.
 * @param count Number of slots to write.
 * @return Whether the file was written successfully.
 */
static bool WriteBlueprintFile(const std::string &file_name, std::string_view prefix, uint first_slot, uint count)
{
	auto file = FileHandle::Open(GetBlueprintBaseDirectory() + file_name, "wb");
	if (!file.has_value()) return false;

	std::string text = EncodeBlueprintSlotsText(prefix, first_slot, count);
	return fwrite(text.data(), 1, text.size(), *file) == text.size();
}

/**
 * Read the raw contents of a file in the blueprint directory.
 * @param file_name File name including extension, relative to #GetBlueprintBaseDirectory.
 * @param[out] content Filled with the file contents on success.
 * @return Whether the file was read in full.
 */
static bool ReadBlueprintFileText(const std::string &file_name, std::string &content)
{
	auto file = FileHandle::Open(GetBlueprintBaseDirectory() + file_name, "rb");
	if (!file.has_value()) return false;

	content.clear();
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), *file)) > 0) {
		content.append(buf, n);
		if (content.size() > 8 * 1024 * 1024) return false;
	}

	return true;
}

/**
 * Read one page of blueprints from a file in the blueprint directory.
 * @param file_name File name including extension, relative to #GetBlueprintBaseDirectory.
 * @param[out] slots Filled with exactly #NUM_BLUEPRINT_SLOTS blueprints on success.
 * @return Whether a valid blueprint set was read.
 */
static bool ReadBlueprintSetFile(const std::string &file_name, std::vector<Blueprint> &slots)
{
	std::string content;
	if (!ReadBlueprintFileText(file_name, content)) return false;
	return ParseBlueprintSlotsText(content, BLUEPRINT_SET_FILE_PREFIX, NUM_BLUEPRINT_SLOTS, slots);
}

/** File in the blueprint directory holding the automatic snapshot of every slot on every page. */
static constexpr std::string_view BLUEPRINT_AUTOSAVE_FILE = "autosave.txt";

/**
 * Persist every blueprint slot of every page to the autosave file. Called after
 * every change to a slot (copy, import, rename), so the slots survive a game
 * restart; failures are silent - the autosave is a convenience, not a
 * player-initiated export.
 */
static void SaveBlueprintAutosave()
{
	WriteBlueprintFile(std::string{BLUEPRINT_AUTOSAVE_FILE}, BLUEPRINT_ALL_FILE_PREFIX, 0, NUM_BLUEPRINT_TOTAL_SLOTS);
}

/** Restore the blueprint slots from the autosave file, once per game launch. */
static void LoadBlueprintAutosave()
{
	static bool loaded = false;
	if (loaded) return;
	loaded = true;

	std::string content;
	if (!ReadBlueprintFileText(std::string{BLUEPRINT_AUTOSAVE_FILE}, content)) return;

	std::vector<Blueprint> slots;
	if (ParseBlueprintSlotsText(content, BLUEPRINT_ALL_FILE_PREFIX, NUM_BLUEPRINT_TOTAL_SLOTS, slots)) {
		for (uint i = 0; i < NUM_BLUEPRINT_TOTAL_SLOTS; i++) GetBlueprint(i) = std::move(slots[i]);
		return;
	}

	/* Autosaves written before the toolbar had pages hold a single page of slots; keep them by loading it as the first page. */
	if (ParseBlueprintSlotsText(content, BLUEPRINT_SET_FILE_PREFIX, NUM_BLUEPRINT_SLOTS, slots)) {
		for (uint i = 0; i < NUM_BLUEPRINT_SLOTS; i++) GetBlueprint(i) = std::move(slots[i]);
	}
}

/** Client-side state of the blueprint toolbar. */
struct BlueprintToolbarState {
	uint8_t page = 0;              ///< Active slot page (0-based); the slot buttons show this page.
	uint8_t slot = 0;              ///< Active blueprint slot within #page (0-based).
	bool scratch = false;          ///< Work with the scratch buffer rather than with #slot of #page.
	BlueprintPasteOptions options; ///< Filters, transformation and terraform settings for pasting.
	bool undo_terrain = true;      ///< Restore the landscape too when undoing a paste, not only remove what was built.
};

/**
 * The scratch buffer behind the toolbar's last slot button.
 *
 * It is deliberately not one of the #NUM_BLUEPRINT_TOTAL_SLOTS: it belongs to
 * no page, is never autosaved, never exported with a page and does not survive
 * a restart. That is the whole point of it - a place to put something you are
 * about to paste twice without spending a numbered slot on it, and without the
 * copy quietly overwriting a blueprint you meant to keep.
 */
static Blueprint _blueprint_scratch;

/** Current state of the blueprint toolbar; kept while the game is running. */
static BlueprintToolbarState _blueprint_state;

/**
 * Window colour of each slot page. The pages hold unrelated blueprints and are
 * switched blind from a dropdown, so each one recolours its slot buttons: which
 * page is on the toolbar is then readable without looking at the dropdown.
 * The first keeps the toolbar's own dark green, so a player who never leaves it
 * sees no change.
 */
static const Colours BLUEPRINT_PAGE_COLOURS[NUM_BLUEPRINT_SLOT_PAGES] = {
	Colours::DarkGreen, Colours::Blue, Colours::Orange, Colours::Mauve, Colours::Brown, Colours::Grey,
};

/** First slot of the active page, as an index across all pages. */
static uint ActiveBlueprintPageFirstSlot()
{
	return _blueprint_state.page * NUM_BLUEPRINT_SLOTS;
}

/** The numbered slot the toolbar is working with, as an index across all pages. */
static uint ActiveBlueprintSlot()
{
	return ActiveBlueprintPageFirstSlot() + _blueprint_state.slot;
}

/** The blueprint the toolbar is working with: the scratch buffer, or the active numbered slot. */
static Blueprint &ActiveBlueprint()
{
	return _blueprint_state.scratch ? _blueprint_scratch : GetBlueprint(ActiveBlueprintSlot());
}

/**
 * The blueprint behind one of the toolbar's slot buttons.
 * @param widget A widget between #WID_BT_SLOT_1 and #WID_BT_SLOT_SCRATCH.
 * @return The scratch buffer for #WID_BT_SLOT_SCRATCH, else that slot of the active page.
 */
static Blueprint &BlueprintOfSlotWidget(WidgetID widget)
{
	if (widget == WID_BT_SLOT_SCRATCH) return _blueprint_scratch;
	return GetBlueprint(ActiveBlueprintPageFirstSlot() + (widget - WID_BT_SLOT_1));
}

/** The letter naming a slot page, as shown on the toolbar button and in its menu. */
static std::string BlueprintPageLetter(uint page)
{
	return std::string(1, static_cast<char>('A' + page));
}

/**
 * A page's slots as one glyph each, in slot order: 'X' for a slot that holds a
 * blueprint, '-' for an empty one. Reading which slots are taken matters more
 * when picking a page than a count does - it is the difference between "this
 * page has room" and "room in slot 3".
 */
static std::string BlueprintPageOccupancy(uint page)
{
	std::string occupancy(NUM_BLUEPRINT_SLOTS, '-');
	for (uint i = 0; i < NUM_BLUEPRINT_SLOTS; i++) {
		if (!GetBlueprint(page * NUM_BLUEPRINT_SLOTS + i).IsEmpty()) occupancy[i] = 'X';
	}
	return occupancy;
}

/** Restore the toolbar toggles from their openttd.cfg settings, once per game launch. */
static void LoadBlueprintStateFromSettings()
{
	static bool loaded = false;
	if (loaded) return;
	loaded = true;

	const GUISettings &s = _settings_client.gui;
	BlueprintPasteOptions &o = _blueprint_state.options;
	o.paste_rail = s.blueprint_paste_rail;
	o.paste_road = s.blueprint_paste_road;
	o.paste_water = s.blueprint_paste_water;
	o.paste_air = s.blueprint_paste_air;
	o.convert_railtype = s.blueprint_convert_railtype;
	o.mirror_signals = s.blueprint_mirror_signals;
	o.upgrade_bridges = s.blueprint_upgrade_bridges;
	o.with_stations = s.blueprint_with_stations;
	o.terraform_mode = static_cast<BlueprintTerraformMode>(std::min<uint8_t>(s.blueprint_terraform_mode, 2));
	_blueprint_state.undo_terrain = s.blueprint_undo_terrain;
}

/** Write the toolbar toggles through to their openttd.cfg settings, so they survive a game restart. */
static void SaveBlueprintStateToSettings()
{
	GUISettings &s = _settings_client.gui;
	const BlueprintPasteOptions &o = _blueprint_state.options;
	s.blueprint_paste_rail = o.paste_rail;
	s.blueprint_paste_road = o.paste_road;
	s.blueprint_paste_water = o.paste_water;
	s.blueprint_paste_air = o.paste_air;
	s.blueprint_convert_railtype = o.convert_railtype;
	s.blueprint_mirror_signals = o.mirror_signals;
	s.blueprint_upgrade_bridges = o.upgrade_bridges;
	s.blueprint_with_stations = o.with_stations;
	s.blueprint_terraform_mode = static_cast<uint8_t>(o.terraform_mode);
	s.blueprint_undo_terrain = _blueprint_state.undo_terrain;
}

/** What each toolbar's Info entry describes. */
enum class BlueprintInfoSubject : uint8_t {
	Blueprint, ///< The blueprint toolbar.
	Undo,      ///< The undo toolbar.
};

static constexpr std::initializer_list<NWidgetPart> _nested_blueprint_info_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_BIN_CAPTION), SetStringTip(STR_BLUEPRINT_INFO_CAPTION_BLUEPRINT, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(WWT_EMPTY, Colours::DarkGreen, WID_BIN_TEXT), SetMinimalSize(340, 0), SetPadding(WidgetDimensions::unscaled.framerect),
	EndContainer(),
};

static WindowDesc _blueprint_info_desc(__FILE__, __LINE__,
	WindowPosition::Center, nullptr, 0, 0,
	WindowClass::BlueprintInfo, WindowClass::None,
	{},
	_nested_blueprint_info_widgets
);

/** A short description of what a toolbar is for and how it works. */
struct BlueprintInfoWindow : Window {
	BlueprintInfoSubject subject;

	BlueprintInfoWindow(WindowDesc &desc, BlueprintInfoSubject subject) : Window(desc), subject(subject)
	{
		this->InitNested(static_cast<int>(subject));
	}

	/** The patch version, then what this toolbar is for. */
	std::string GetText() const
	{
		StringID body = this->subject == BlueprintInfoSubject::Undo ? STR_BLUEPRINT_INFO_TEXT_UNDO : STR_BLUEPRINT_INFO_TEXT_BLUEPRINT;
		return GetString(STR_BLUEPRINT_INFO_WINDOW_VERSION, std::string{BLUEPRINT_PATCH_VERSION}) + "\n\n" + GetString(body);
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget != WID_BIN_CAPTION) return this->Window::GetWidgetString(widget, stringid);
		return GetString(this->subject == BlueprintInfoSubject::Undo ? STR_BLUEPRINT_INFO_CAPTION_UNDO : STR_BLUEPRINT_INFO_CAPTION_BLUEPRINT);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_BIN_TEXT) return;
		size.height = GetStringHeight(this->GetText(), size.width);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_BIN_TEXT) return;
		DrawStringMultiLine(r, this->GetText(), TextColour::Begin, SA_LEFT | SA_TOP);
	}
};

/**
 * Show what a toolbar does.
 * @param subject Which toolbar to describe.
 */
static void ShowBlueprintInfo(BlueprintInfoSubject subject)
{
	CloseWindowByClass(WindowClass::BlueprintInfo);
	new BlueprintInfoWindow(_blueprint_info_desc, subject);
}

/** Window classes carrying the undo button, refreshed together when the stack changes. */
static const WindowClass BLUEPRINT_UNDO_TOOLBARS[] = {
	WindowClass::BlueprintToolbar, WindowClass::BlueprintUndoToolbar, WindowClass::BlueprintUndoList,
};

/** Refresh every toolbar showing the undo button, so it greys out in step with the stack. */
static void InvalidateBlueprintUndoToolbars()
{
	for (WindowClass wc : BLUEPRINT_UNDO_TOOLBARS) InvalidateWindowClassesData(wc, 0, true);
}

/** Object undo filter, kept for this session like the active blueprint transformation. */
static bool _blueprint_undo_objects = true;

/** The kinds of infrastructure the undo buttons are currently allowed to remove. */
static BlueprintUndoFilters GetBlueprintUndoFilters()
{
	const GUISettings &g = _settings_client.gui;
	return {g.blueprint_undo_rail, g.blueprint_undo_road, g.blueprint_undo_water, g.blueprint_undo_air, _blueprint_undo_objects};
}

/** Is there a paste this company can still undo? */
bool HasBlueprintUndoRecord()
{
	return GetBlueprintUndoRecord() != nullptr;
}

/**
 * Undo the most recent paste of the local company, if there is one left to undo.
 *
 * Shared by the blueprint toolbar and the construction toolbars, which all offer
 * the same button.
 */
/**
 * Post an undo command for one record.
 *
 * Takes a copy rather than a reference into the stack: in single player the
 * command runs and its callback consumes the record before this returns, which
 * would leave a reference dangling for a caller undoing several at once.
 *
 * @param record What to undo.
 * @return Whether a command was posted.
 */
static bool PostBlueprintUndo(const BlueprintUndoRecord record)
{
	BlueprintUndoFilters filters = GetBlueprintUndoFilters();
	if (filters.None()) return false;

	BlueprintUndoData data;
	data.built = record.built;
	FilterBlueprintUndoData(data.built, filters);
	data.restore_terrain = _settings_client.gui.blueprint_undo_terrain;

	/* Nothing left to do that the player has not already excluded, either through
	 * the transport filters or by turning the landscape off.
	 *
	 * Blueprint::IsEmpty() counts elements only: for a blueprint the corner
	 * heights are context rather than content. For an undo record they can be the
	 * whole of it, so an action that only moved land has to be asked about
	 * separately or it looks like there is nothing to undo. */
	bool restores_land = data.restore_terrain && !data.built.corner_heights.empty();
	if (data.built.IsEmpty() && !restores_land) return false;

	if (_networking && GetBlueprintSerialisedSize(data) > MAX_BLUEPRINT_COMMAND_SIZE) {
		ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_UNDO_TOO_LARGE), {}, WarningLevel::Info);
		return false;
	}

	/* The record is consumed by #CcUndoBlueprintPaste once the undo has really
	 * run, not here: in a network game the command still has to reach the server,
	 * and an undo that fails for want of money should stay retryable. */
	Command<Commands::UndoBlueprintPaste>::Post(STR_ERROR_CAN_T_UNDO_BLUEPRINT_PASTE, CommandCallback::UndoBlueprintPaste, record.origin, data);
	return true;
}

void UndoLastBlueprintPaste()
{
	const BlueprintUndoRecord *record = GetBlueprintUndoRecord();
	if (record == nullptr || !PostBlueprintUndo(*record)) {
		ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_NOTHING_TO_UNDO), {}, WarningLevel::Info);
	}
}

/**
 * Summarise what a record would take back, as a line for the history list.
 *
 * Named per kind rather than per transport type: "1 rail" for a depot, a signal
 * and a piece of track alike says nothing about what pressing undo would do.
 *
 * @param built What the action built.
 * @return Text like "9 track, 1 train depot, 3 signals".
 */
static std::string DescribeBlueprintUndoRecord(const Blueprint &built, const BlueprintUndoFilters &filters)
{
	std::string text;
	auto add = [&text](StringID str, size_t count) {
		if (count == 0) return;
		if (!text.empty()) text += ", ";
		text += GetString(str, count);
	};

	uint bridges = 0, tunnels = 0;
	for (const BlueprintTunnelBridge &tb : built.tunnel_bridges) {
		/* A bridge or tunnel belongs to whatever it carries, exactly as
		 * #FilterBlueprintUndoData decides when the undo actually runs. */
		switch (tb.transport) {
			case TransportType::Rail: if (!filters.rail) continue; break;
			case TransportType::Road: if (!filters.road) continue; break;
			case TransportType::Water: if (!filters.water) continue; break;
			default: break;
		}
		if (tb.is_bridge) {
			bridges++;
		} else {
			tunnels++;
		}
	}

	/* Station tiles are recorded one by one; count the platforms they form, which
	 * is what a player recognises as "a station". */
	uint stations = 0, waypoints = 0;
	for (const BlueprintStationRect &rect : GroupStationTiles(built)) {
		if (rect.is_waypoint) {
			waypoints++;
		} else {
			stations++;
		}
	}

	auto rail = [&](size_t count) { return filters.rail ? count : 0; };
	auto road = [&](size_t count) { return filters.road ? count : 0; };
	auto water = [&](size_t count) { return filters.water ? count : 0; };

	add(STR_BLUEPRINT_UNDO_LIST_TRACK, rail(built.rail_tracks.size()));
	add(STR_BLUEPRINT_UNDO_LIST_SIGNAL, rail(built.signals.size()));
	add(STR_BLUEPRINT_UNDO_LIST_RAIL_DEPOT, rail(built.rail_depots.size()));
	add(STR_BLUEPRINT_UNDO_LIST_STATION, rail(stations));
	add(STR_BLUEPRINT_UNDO_LIST_WAYPOINT, rail(waypoints));
	add(STR_BLUEPRINT_UNDO_LIST_ROAD, road(built.roads.size()));
	add(STR_BLUEPRINT_UNDO_LIST_ROAD_DEPOT, road(built.road_depots.size()));
	add(STR_BLUEPRINT_UNDO_LIST_ROAD_STOP, road(built.road_stops.size()));
	add(STR_BLUEPRINT_UNDO_LIST_BRIDGE, bridges);
	add(STR_BLUEPRINT_UNDO_LIST_TUNNEL, tunnels);
	add(STR_BLUEPRINT_UNDO_LIST_CANAL, water(built.canals.size()));
	add(STR_BLUEPRINT_UNDO_LIST_LOCK, water(built.locks.size()));
	add(STR_BLUEPRINT_UNDO_LIST_SHIP_DEPOT, water(built.ship_depots.size()));
	add(STR_BLUEPRINT_UNDO_LIST_DOCK, water(built.docks.size()));
	add(STR_BLUEPRINT_UNDO_LIST_BUOY, water(built.buoys.size()));
	add(STR_BLUEPRINT_UNDO_LIST_AIRPORT, filters.air ? built.airports.size() : 0);
	add(STR_BLUEPRINT_UNDO_LIST_OBJECT, filters.objects ? built.objects.size() : 0);
	if (!built.corner_heights.empty()) {
		if (!text.empty()) text += ", ";
		text += GetString(STR_BLUEPRINT_UNDO_LIST_LAND);
	}
	return text;
}

/** Cached paste preview overlays; rebuilt lazily when #_paste_preview_dirty. */
static BlueprintPastePreview _paste_preview;
static bool _paste_preview_dirty = true;
static BlueprintPasteData _paste_preview_data;         ///< Prepared paste data matching #_paste_preview.
static TileIndex _paste_preview_origin = INVALID_TILE; ///< Origin the blocked mask of #_paste_preview was computed for.
static Money _paste_preview_cost = 0;                  ///< Estimated cost of pasting at #_paste_preview_origin; 0 when unknown.

/** Rebuild #_paste_preview from the active slot, transformation and filters. */
static void RebuildPastePreview()
{
	_paste_preview_dirty = false;
	_paste_preview_origin = INVALID_TILE;

	_paste_preview_data = PrepareBlueprintPaste(ActiveBlueprint(), _blueprint_state.options, GetCurrentRailType());
	const Blueprint &bp = _paste_preview_data.blueprint;

	_paste_preview.width = bp.width;
	_paste_preview.height = bp.height;
	_paste_preview.tiles.assign(static_cast<size_t>(bp.width) * bp.height, 0);
	_paste_preview.blocked.assign(static_cast<size_t>(bp.width) * bp.height, false);

	auto mark = [&](int x, int y, uint8_t bits) {
		if (x < 0 || y < 0 || x >= bp.width || y >= bp.height) return;
		_paste_preview.tiles[static_cast<size_t>(y) * bp.width + x] |= bits;
	};
	auto highlight = [&](int x, int y) {
		mark(x, y, 1 << BLUEPRINT_PREVIEW_HIGHLIGHT_BIT);
	};

	for (const BlueprintRailTrack &rt : bp.rail_tracks) mark(rt.offset.x, rt.offset.y, TrackToTrackBits(rt.track));
	for (const BlueprintRailDepot &depot : bp.rail_depots) highlight(depot.offset.x, depot.offset.y);
	for (const BlueprintRoad &road : bp.roads) {
		/* Show straight-through roads as a line overlay, anything else as a highlight. */
		if (road.bits == ROAD_X) {
			mark(road.offset.x, road.offset.y, TRACK_BIT_X);
		} else if (road.bits == ROAD_Y) {
			mark(road.offset.x, road.offset.y, TRACK_BIT_Y);
		} else {
			highlight(road.offset.x, road.offset.y);
		}
	}
	for (const BlueprintRoadDepot &depot : bp.road_depots) highlight(depot.offset.x, depot.offset.y);
	for (const BlueprintCanal &canal : bp.canals) highlight(canal.offset.x, canal.offset.y);
	for (const BlueprintLock &lock : bp.locks) {
		TileIndexDiffC d = TileIndexDiffCByDiagDir(lock.dir);
		highlight(lock.offset.x - d.x, lock.offset.y - d.y);
		highlight(lock.offset.x, lock.offset.y);
		highlight(lock.offset.x + d.x, lock.offset.y + d.y);
	}
	for (const BlueprintShipDepot &depot : bp.ship_depots) {
		highlight(depot.offset.x, depot.offset.y);
		highlight(depot.offset.x + (depot.axis == Axis::X ? 1 : 0), depot.offset.y + (depot.axis == Axis::Y ? 1 : 0));
	}
	for (const BlueprintTunnelBridge &tb : bp.tunnel_bridges) {
		highlight(tb.offset.x, tb.offset.y);
		highlight(tb.other_end.x, tb.other_end.y);
	}
	for (const BlueprintStationTile &st : bp.station_tiles) {
		mark(st.offset.x, st.offset.y, (st.axis == Axis::X ? TRACK_BIT_X : TRACK_BIT_Y) | (1 << BLUEPRINT_PREVIEW_HIGHLIGHT_BIT));
	}
	for (const BlueprintObject &v : bp.objects) {
		for (uint y = 0; y < v.h; ++y) for (uint x = 0; x < v.w; ++x) highlight(v.offset.x + x, v.offset.y + y);
	}
	for (const BlueprintRoadStop &stop : bp.road_stops) highlight(stop.offset.x, stop.offset.y);
	for (const BlueprintDock &dock : bp.docks) {
		TileIndexDiffC d = TileIndexDiffCByDiagDir(dock.dir);
		highlight(dock.offset.x, dock.offset.y);
		highlight(dock.offset.x + d.x, dock.offset.y + d.y);
	}
	for (const BlueprintBuoy &buoy : bp.buoys) highlight(buoy.offset.x, buoy.offset.y);
	for (const BlueprintAirport &airport : bp.airports) {
		if (!airport.tiles.empty()) {
			for (const BlueprintAirportTile &apt : airport.tiles) highlight(apt.offset.x, apt.offset.y);
			continue;
		}
		for (uint dy = 0; dy < airport.h; dy++) {
			for (uint dx = 0; dx < airport.w; dx++) highlight(airport.offset.x + dx, airport.offset.y + dy);
		}
	}
}

/**
 * Would this tile block the paste even with terraforming?
 * @param tile Target tile.
 * @param wants_track Whether the pasted element on this tile carries tracks or roads.
 * @return True when a hard obstacle occupies the tile.
 */
static bool BlueprintTileBlocked(TileIndex tile, bool wants_track)
{
	switch (GetTileType(tile)) {
		case TileType::Clear:
		case TileType::Trees:
			return false;

		case TileType::Water:
			return wants_track;

		case TileType::Railway:
		case TileType::Road:
		case TileType::Station:
		case TileType::TunnelBridge: {
			Owner owner = GetTileOwner(tile);
			return owner != _local_company && owner != OWNER_NONE && owner != OWNER_TOWN;
		}

		default: /* Houses, industries, objects, void, ... */
			return true;
	}
}

/** Test-run a build command for the paste preview; only success matters. */
template <Commands Tcmd, typename... Targs>
static bool PreviewTest(Targs... args)
{
	auto result = Command<Tcmd>::Do(DoCommandFlags{}, args...);
	if constexpr (std::is_same_v<std::decay_t<decltype(result)>, CommandCost>) {
		return result.Succeeded();
	} else {
		return std::get<0>(result).Succeeded();
	}
}

/**
 * Recompute the blocked mask of #_paste_preview for a new target position.
 *
 * With terraforming enabled the paste flattens the land first, so only hard
 * obstacles are marked. Without terraforming the actual build commands are
 * test-run on the current landscape; parts whose success depends on other
 * parts of the same paste (signals, waypoint surfaces, ship depots on
 * to-be-built canals) are left optimistic.
 * @param origin Northern tile of the target area.
 */
static void RecomputePasteBlocked(TileIndex origin)
{
	_paste_preview_origin = origin;

	/* Estimated cost of the whole paste at this origin: a test run of the real
	 * command, so terraforming and skipped parts are priced exactly like the
	 * eventual click. Shown as a hover tooltip by the toolbar window. */
	_paste_preview_cost = 0;
	if (_settings_client.gui.measure_tooltip) {
		CommandCost cost = Command<Commands::PasteBlueprint>::Do(DoCommandFlags{}, origin, _paste_preview_data);
		if (cost.Succeeded()) _paste_preview_cost = cost.GetCost();
	}

	const Blueprint &bp = _paste_preview_data.blueprint;
	_paste_preview.blocked.assign(_paste_preview.tiles.size(), false);

	auto block = [&](int x, int y) {
		if (x < 0 || y < 0 || x >= bp.width || y >= bp.height) return;
		_paste_preview.blocked[static_cast<size_t>(y) * bp.width + x] = true;
	};
	auto tile_valid = [&](TileIndexDiffC offset) {
		return TileX(origin) + offset.x <= Map::MaxX() && TileY(origin) + offset.y <= Map::MaxY();
	};
	auto target = [&](TileIndexDiffC offset) {
		return TileAddXY(origin, offset.x, offset.y);
	};

	if (_paste_preview_data.terraform_mode != BlueprintTerraformMode::None) {
		/* The land will be flattened first; only hard obstacles block the paste. */
		for (uint y = 0; y < bp.height; y++) {
			for (uint x = 0; x < bp.width; x++) {
				uint8_t overlay = _paste_preview.tiles[static_cast<size_t>(y) * bp.width + x];
				if (overlay == 0) continue;
				TileIndexDiffC offset = {static_cast<int16_t>(x), static_cast<int16_t>(y)};
				if (!tile_valid(offset) || BlueprintTileBlocked(target(offset), GB(overlay, 0, 6) != 0)) block(x, y);
			}
		}
		return;
	}

	/* No terraforming: test-run the build commands on the current landscape. */
	for (const BlueprintCanal &canal : bp.canals) {
		if (!tile_valid(canal.offset)) { block(canal.offset.x, canal.offset.y); continue; }
		if (!PreviewTest<CMD_BUILD_CANAL>(target(canal.offset), target(canal.offset), WaterClass::Canal, false)) block(canal.offset.x, canal.offset.y);
	}
	for (const BlueprintLock &lock : bp.locks) {
		TileIndexDiffC d = TileIndexDiffCByDiagDir(lock.dir);
		if (tile_valid(lock.offset) && PreviewTest<CMD_BUILD_LOCK>(target(lock.offset))) continue;
		block(lock.offset.x - d.x, lock.offset.y - d.y);
		block(lock.offset.x, lock.offset.y);
		block(lock.offset.x + d.x, lock.offset.y + d.y);
	}
	if (bp.canals.empty() && bp.locks.empty()) {
		/* Only meaningful when the depot does not sit on water built by the same paste. */
		for (const BlueprintShipDepot &depot : bp.ship_depots) {
			if (tile_valid(depot.offset) && PreviewTest<CMD_BUILD_SHIP_DEPOT>(target(depot.offset), depot.axis)) continue;
			block(depot.offset.x, depot.offset.y);
			block(depot.offset.x + (depot.axis == Axis::X ? 1 : 0), depot.offset.y + (depot.axis == Axis::Y ? 1 : 0));
		}
	}
	for (const BlueprintRailTrack &rt : bp.rail_tracks) {
		if (tile_valid(rt.offset) && PreviewTest<CMD_BUILD_SINGLE_RAIL>(target(rt.offset), rt.railtype, rt.track, BuildRailTrackFlags{})) continue;
		block(rt.offset.x, rt.offset.y);
	}
	for (const BlueprintRailDepot &depot : bp.rail_depots) {
		if (tile_valid(depot.offset) && PreviewTest<CMD_BUILD_TRAIN_DEPOT>(target(depot.offset), depot.railtype, depot.dir)) continue;
		block(depot.offset.x, depot.offset.y);
	}
	for (const BlueprintTunnelBridge &tb : bp.tunnel_bridges) {
		uint8_t road_rail_type = 0;
		switch (tb.transport) {
			case TransportType::Rail: road_rail_type = tb.railtype; break;
			case TransportType::Road: road_rail_type = tb.roadtype != INVALID_ROADTYPE ? tb.roadtype : tb.tramtype; break;
			default: break;
		}
		if (tile_valid(tb.offset) && tile_valid(tb.other_end)) {
			bool ok = tb.is_bridge ?
					PreviewTest<CMD_BUILD_BRIDGE>(target(tb.other_end), target(tb.offset), tb.transport, tb.bridge_type, road_rail_type, BuildBridgeFlags{}) :
					PreviewTest<CMD_BUILD_TUNNEL>(target(tb.offset), tb.transport, road_rail_type);
			if (ok) continue;
		}
		block(tb.offset.x, tb.offset.y);
		block(tb.other_end.x, tb.other_end.y);
	}
	for (const BlueprintRoad &road : bp.roads) {
		if (tile_valid(road.offset) && PreviewTest<CMD_BUILD_ROAD>(target(road.offset), road.bits, road.roadtype, DisallowedRoadDirections{}, TownID::Invalid(), BuildRoadFlags{})) continue;
		block(road.offset.x, road.offset.y);
	}
	for (const BlueprintRoadDepot &depot : bp.road_depots) {
		if (tile_valid(depot.offset) && PreviewTest<CMD_BUILD_ROAD_DEPOT>(target(depot.offset), depot.roadtype, depot.dir)) continue;
		block(depot.offset.x, depot.offset.y);
	}
	for (const BlueprintStationRect &rect : GroupStationTiles(bp)) {
		bool ok;
		if (rect.is_waypoint) {
			/* Test the tracks the waypoint will sit on; the waypoint itself follows them. */
			Track track = AxisToTrack(rect.axis);
			for (uint dy = 0; dy < rect.h; dy++) {
				for (uint dx = 0; dx < rect.w; dx++) {
					TileIndexDiffC offset = {static_cast<int16_t>(rect.offset.x + dx), static_cast<int16_t>(rect.offset.y + dy)};
					if (tile_valid(offset) && PreviewTest<CMD_BUILD_SINGLE_RAIL>(target(offset), rect.railtype, track, BuildRailTrackFlags{})) continue;
					block(offset.x, offset.y);
				}
			}
			continue;
		}
		uint8_t numtracks = static_cast<uint8_t>(rect.axis == Axis::X ? rect.h : rect.w);
		uint8_t plat_len = static_cast<uint8_t>(rect.axis == Axis::X ? rect.w : rect.h);
		ok = tile_valid(rect.offset) && PreviewTest<CMD_BUILD_RAIL_STATION>(target(rect.offset), rect.railtype, rect.axis, numtracks, plat_len,
				STAT_CLASS_DFLT, 0, NEW_STATION, true);
		if (ok) continue;
		for (uint dy = 0; dy < rect.h; dy++) {
			for (uint dx = 0; dx < rect.w; dx++) block(rect.offset.x + dx, rect.offset.y + dy);
		}
	}
	for (const BlueprintRoadStop &stop : bp.road_stops) {
		RoadType rt = stop.roadtype != INVALID_ROADTYPE ? stop.roadtype : stop.tramtype;
		DiagDirection ddir = stop.drive_through ? AxisToDiagDir(stop.axis) : stop.dir;
		bool ok = false;
		if (tile_valid(stop.offset)) {
			if (stop.type == StationType::RoadWaypoint) {
				/* Test the road the waypoint will sit on; the waypoint itself follows it. */
				ok = PreviewTest<CMD_BUILD_ROAD>(target(stop.offset), AxisToRoadBits(stop.axis), rt, DisallowedRoadDirections{}, TownID::Invalid(), BuildRoadFlags{});
			} else if (IsValidDiagDirection(ddir)) {
				RoadStopType stop_type = stop.type == StationType::Truck ? RoadStopType::Truck : RoadStopType::Bus;
				ok = PreviewTest<CMD_BUILD_ROAD_STOP>(target(stop.offset), 1, 1, stop_type, stop.drive_through,
						ddir, rt, ROADSTOP_CLASS_DFLT, 0, NEW_STATION, true);
			}
		}
		if (!ok) block(stop.offset.x, stop.offset.y);
	}
	for (const BlueprintObject &v : bp.objects) {
		const ObjectSpec *spec = ResolveBlueprintObject(v.spec);
		if (spec != nullptr && tile_valid(v.offset) && PreviewTest<Commands::BuildObject>(target(v.offset), static_cast<ObjectType>(spec->Index()), v.view)) continue;
		for (uint y = 0; y < v.h; ++y) for (uint x = 0; x < v.w; ++x) block(v.offset.x + x, v.offset.y + y);
	}
	for (const BlueprintDock &dock : bp.docks) {
		TileIndexDiffC d = TileIndexDiffCByDiagDir(dock.dir);
		if (tile_valid(dock.offset) && PreviewTest<CMD_BUILD_DOCK>(target(dock.offset), NEW_STATION, true)) continue;
		block(dock.offset.x, dock.offset.y);
		block(dock.offset.x + d.x, dock.offset.y + d.y);
	}
	for (const BlueprintBuoy &buoy : bp.buoys) {
		if (tile_valid(buoy.offset) && PreviewTest<CMD_BUILD_BUOY>(target(buoy.offset))) continue;
		block(buoy.offset.x, buoy.offset.y);
	}
	for (const BlueprintAirport &airport : bp.airports) {
		if (!airport.tiles.empty()) {
			AirType airtype = ValParamAirType(static_cast<AirType>(airport.air_type)) ? static_cast<AirType>(airport.air_type) : (_sorted_airtypes.empty() ? AIRTYPE_BEGIN : _sorted_airtypes.front());
			for (const BlueprintAirportTile &apt : airport.tiles) {
				if (tile_valid(apt.offset) && PreviewTest<Commands::AddRemoveAirportTiles>(target(apt.offset), target(apt.offset), true, airtype, StationID::Invalid(), false)) continue;
				block(apt.offset.x, apt.offset.y);
			}
			continue;
		}
		AirType airtype = ValParamAirType(static_cast<AirType>(airport.air_type)) ? static_cast<AirType>(airport.air_type) : (_sorted_airtypes.empty() ? AIRTYPE_BEGIN : _sorted_airtypes.front());
		if (tile_valid(airport.offset) && PreviewTest<CMD_BUILD_AIRPORT>(target(airport.offset), airport.type, airport.layout, airtype, static_cast<DiagDirection>(airport.rotation % 4), StationID::Invalid(), true)) continue;
		for (uint dy = 0; dy < airport.h; dy++) {
			for (uint dx = 0; dx < airport.w; dx++) block(airport.offset.x + dx, airport.offset.y + dy);
		}
	}
}

/** Tile categories of the hover preview schematic, in increasing draw priority. */
enum class BlueprintPreviewCell : uint8_t {
	Empty,   ///< Nothing on this tile.
	Water,   ///< Canals, locks, ship depots, buoys, aqueducts.
	Road,    ///< Roads, road depots, road tunnels and bridges.
	Rail,    ///< Rail tracks, rail depots, rail tunnels and bridges.
	Airport, ///< Airport tiles.
	Station, ///< Stations, waypoints, road stops, docks.
	Object,  ///< Complete NewGRF objects.
};

/** Colours the schematic cells are drawn in, indexed by #BlueprintPreviewCell. */
static constexpr PixelColour BLUEPRINT_PREVIEW_CELL_COLOURS[] = {
	PC_BLACK,      // Empty (not drawn)
	PC_LIGHT_BLUE, // Water
	PC_ORANGE,     // Road
	PC_WHITE,      // Rail
	PC_RED,        // Airport
	PC_YELLOW,     // Station
	PC_ORANGE,     // Object
};

/** Downscaled top-down schematic of a blueprint for the hover preview. */
struct BlueprintSchematic {
	uint16_t width;  ///< Extent along the X axis, in tiles.
	uint16_t height; ///< Extent along the Y axis, in tiles.
	std::vector<BlueprintPreviewCell> cells; ///< Cell categories, index = y * width + x.

	BlueprintSchematic(const Blueprint &bp) : width(bp.width), height(bp.height)
	{
		this->cells.assign(static_cast<size_t>(this->width) * this->height, BlueprintPreviewCell::Empty);
		auto mark = [&](int x, int y, BlueprintPreviewCell cell) {
			if (x < 0 || y < 0 || x >= this->width || y >= this->height) return;
			BlueprintPreviewCell &current = this->cells[static_cast<size_t>(y) * this->width + x];
			if (cell > current) current = cell;
		};

		for (const BlueprintObject &v : bp.objects) {
			for (uint y = 0; y < v.h; ++y) for (uint x = 0; x < v.w; ++x) mark(v.offset.x + x, v.offset.y + y, BlueprintPreviewCell::Object);
		}
		for (const BlueprintRailTrack &rt : bp.rail_tracks) mark(rt.offset.x, rt.offset.y, BlueprintPreviewCell::Rail);
		for (const BlueprintRailDepot &depot : bp.rail_depots) mark(depot.offset.x, depot.offset.y, BlueprintPreviewCell::Rail);
		for (const BlueprintRoad &road : bp.roads) mark(road.offset.x, road.offset.y, BlueprintPreviewCell::Road);
		for (const BlueprintRoadDepot &depot : bp.road_depots) mark(depot.offset.x, depot.offset.y, BlueprintPreviewCell::Road);
		for (const BlueprintCanal &canal : bp.canals) mark(canal.offset.x, canal.offset.y, BlueprintPreviewCell::Water);
		for (const BlueprintLock &lock : bp.locks) {
			TileIndexDiffC d = TileIndexDiffCByDiagDir(lock.dir);
			for (int i = -1; i <= 1; i++) mark(lock.offset.x + i * d.x, lock.offset.y + i * d.y, BlueprintPreviewCell::Water);
		}
		for (const BlueprintShipDepot &depot : bp.ship_depots) {
			mark(depot.offset.x, depot.offset.y, BlueprintPreviewCell::Water);
			mark(depot.offset.x + (depot.axis == Axis::X ? 1 : 0), depot.offset.y + (depot.axis == Axis::Y ? 1 : 0), BlueprintPreviewCell::Water);
		}
		for (const BlueprintTunnelBridge &tb : bp.tunnel_bridges) {
			BlueprintPreviewCell cell = tb.transport == TransportType::Rail ? BlueprintPreviewCell::Rail :
					tb.transport == TransportType::Road ? BlueprintPreviewCell::Road : BlueprintPreviewCell::Water;
			int dx = (tb.other_end.x > tb.offset.x) - (tb.other_end.x < tb.offset.x);
			int dy = (tb.other_end.y > tb.offset.y) - (tb.other_end.y < tb.offset.y);
			int x = tb.offset.x;
			int y = tb.offset.y;
			mark(x, y, cell);
			for (uint steps = 0; (x != tb.other_end.x || y != tb.other_end.y) && steps < MAX_BLUEPRINT_DIMENSION; steps++) {
				x += dx;
				y += dy;
				mark(x, y, cell);
			}
		}
		for (const BlueprintStationTile &st : bp.station_tiles) mark(st.offset.x, st.offset.y, BlueprintPreviewCell::Station);
		for (const BlueprintRoadStop &stop : bp.road_stops) mark(stop.offset.x, stop.offset.y, BlueprintPreviewCell::Station);
		for (const BlueprintDock &dock : bp.docks) {
			TileIndexDiffC d = TileIndexDiffCByDiagDir(dock.dir);
			mark(dock.offset.x, dock.offset.y, BlueprintPreviewCell::Station);
			mark(dock.offset.x + d.x, dock.offset.y + d.y, BlueprintPreviewCell::Station);
		}
		for (const BlueprintBuoy &buoy : bp.buoys) mark(buoy.offset.x, buoy.offset.y, BlueprintPreviewCell::Water);
		for (const BlueprintAirport &airport : bp.airports) {
			if (!airport.tiles.empty()) {
				for (const BlueprintAirportTile &apt : airport.tiles) mark(apt.offset.x, apt.offset.y, BlueprintPreviewCell::Airport);
				continue;
			}
			for (uint dy2 = 0; dy2 < airport.h; dy2++) {
				for (uint dx2 = 0; dx2 < airport.w; dx2++) mark(airport.offset.x + dx2, airport.offset.y + dy2, BlueprintPreviewCell::Airport);
			}
		}
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_blueprint_preview_widgets = {
	NWidget(WWT_EMPTY, Colours::Grey, WID_TT_BACKGROUND),
};

static WindowDesc _blueprint_preview_desc(__FILE__, __LINE__,
	WindowPosition::Manual, nullptr, 0, 0, // Coordinates and sizes are not used,
	WindowClass::ToolTips, WindowClass::None,
	{WindowDefaultFlag::NoFocus, WindowDefaultFlag::NoClose},
	_nested_blueprint_preview_widgets
);

/** Tooltip-style window showing a schematic preview of a blueprint slot. */
struct BlueprintPreviewWindow : public Window {
	BlueprintSchematic schematic;       ///< Schematic of the previewed blueprint.
	std::string name;                   ///< Name of the previewed blueprint; empty when unnamed.
	TooltipCloseCondition close_cond{}; ///< Condition for closing the window.

	BlueprintPreviewWindow(Window *parent, const Blueprint &blueprint, TooltipCloseCondition close_cond) :
			Window(_blueprint_preview_desc), schematic(blueprint), name(blueprint.name), close_cond(close_cond)
	{
		this->parent = parent;
		this->InitNested();
		this->flags.Reset(WindowFlag::WhiteBorder);
	}

	/** Get the drawn size of one schematic cell in pixels. */
	int GetCellSize() const
	{
		int max_dim = std::max<int>(std::max(this->schematic.width, this->schematic.height), 1);
		return Clamp(ScaleGUITrad(160) / max_dim, 1, ScaleGUITrad(5));
	}

	/** Height reserved for the name line, including the gap to the schematic; 0 when unnamed. */
	int GetNameHeight() const
	{
		if (this->name.empty()) return 0;
		return GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.vsep_normal;
	}

	Point OnInitialPosition([[maybe_unused]] int16_t sm_width, [[maybe_unused]] int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		/* Position like a tooltip: below the cursor, flipped above it when there is no room. */
		int scr_top = GetMainViewTop() + 2;
		int scr_bot = GetMainViewBottom() - 2;

		Point pt;
		pt.y = SoftClamp(_cursor.pos.y + _cursor.total_size.y + _cursor.total_offs.y + 5, scr_top, scr_bot);
		if (pt.y + sm_height > scr_bot) pt.y = std::min(_cursor.pos.y + _cursor.total_offs.y - 5, scr_bot) - sm_height;
		pt.x = sm_width >= _screen.width ? 0 : SoftClamp(_cursor.pos.x - (sm_width >> 1), 0, _screen.width - sm_width);

		return pt;
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_TT_BACKGROUND) return;

		int cell = this->GetCellSize();
		size.width  = std::max<uint>(this->schematic.width * cell, GetStringBoundingBox(this->name).width)
				+ WidgetDimensions::scaled.framerect.Horizontal() + WidgetDimensions::scaled.fullbevel.Horizontal();
		size.height = this->schematic.height * cell + this->GetNameHeight()
				+ WidgetDimensions::scaled.framerect.Vertical() + WidgetDimensions::scaled.fullbevel.Vertical();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_TT_BACKGROUND) return;
		GfxFillRect(r, PC_BLACK);
		Rect ir = r.Shrink(WidgetDimensions::scaled.bevel);
		GfxFillRect(ir, PC_DARK_GREY);

		Rect content = r.Shrink(WidgetDimensions::scaled.framerect).Shrink(WidgetDimensions::scaled.fullbevel);
		int name_height = this->GetNameHeight();
		if (name_height > 0) {
			DrawString(content.left, content.right, content.top, this->name, TextColour::White, SA_HOR_CENTER);
			content.top += name_height;
		}

		int cell = this->GetCellSize();
		int ox = content.left + (content.Width() - this->schematic.width * cell) / 2;
		int oy = content.top + (content.Height() - this->schematic.height * cell) / 2;

		for (uint y = 0; y < this->schematic.height; y++) {
			for (uint x = 0; x < this->schematic.width; x++) {
				BlueprintPreviewCell cell_content = this->schematic.cells[static_cast<size_t>(y) * this->schematic.width + x];
				if (cell_content == BlueprintPreviewCell::Empty) continue;
				GfxFillRect(ox + static_cast<int>(x) * cell, oy + static_cast<int>(y) * cell,
						ox + static_cast<int>(x + 1) * cell - 1, oy + static_cast<int>(y + 1) * cell - 1,
						BLUEPRINT_PREVIEW_CELL_COLOURS[static_cast<size_t>(cell_content)]);
			}
		}
	}

	void OnMouseLoop() override
	{
		/* Always close when the cursor is not in our window. */
		if (!_cursor.in_window) {
			this->Close();
			return;
		}

		switch (this->close_cond) {
			case TooltipCloseCondition::RightClick: if (!_right_button_down) this->Close(); break;
			case TooltipCloseCondition::Hover: if (!_mouse_hovering) this->Close(); break;
			case TooltipCloseCondition::None: break;

			case TooltipCloseCondition::ExitViewport: {
				Window *w = FindWindowFromPt(_cursor.pos.x, _cursor.pos.y);
				if (w == nullptr || IsPtInWindowViewport(w, _cursor.pos.x, _cursor.pos.y) == nullptr) this->Close();
				break;
			}
		}
	}
};

/**
 * Show the schematic hover preview of a blueprint, replacing any tooltip.
 * @param parent The window the preview belongs to.
 * @param blueprint The blueprint to preview.
 * @param close_cond Condition under which the preview closes again.
 */
static void ShowBlueprintPreview(Window *parent, const Blueprint &blueprint, TooltipCloseCondition close_cond)
{
	CloseWindowById(WindowClass::ToolTips, 0);
	if (!_cursor.in_window) return;
	new BlueprintPreviewWindow(parent, blueprint, close_cond);
}

static constexpr std::initializer_list<NWidgetPart> _nested_blueprint_text_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_BTX_CAPTION), SetStringTip(STR_BLUEPRINT_EXPORT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(WWT_EDITBOX, Colours::DarkGreen, WID_BTX_TEXT), SetMinimalSize(320, 0), SetFill(1, 0), SetPadding(2, 2, 2, 2),
	EndContainer(),
	NWidget(NWID_SELECTION, Colours::Grey, WID_BTX_SEL),
		NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
			NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BTX_COPY), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_COPY_TO_CLIPBOARD),
		EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BTX_PASTE), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_PASTE_FROM_CLIPBOARD),
			NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
				NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BTX_CANCEL), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_BUTTON_CANCEL),
				NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BTX_OK), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_IMPORT),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _blueprint_text_desc(__FILE__, __LINE__,
	WindowPosition::Center, nullptr, 0, 0,
	WindowClass::BlueprintText, WindowClass::BlueprintToolbar,
	{},
	_nested_blueprint_text_widgets
);

/** Dialog showing a blueprint as text (export) or accepting blueprint text (import). */
struct BlueprintTextWindow : Window {
	QueryString text_editbox;         ///< Editbox holding the blueprint text.
	bool import_mode;                 ///< True for the import dialog, false for the export dialog.
	std::string export_text;          ///< Full text of the exported blueprint; may exceed the editbox capacity.
	std::optional<Blueprint> pending; ///< Parsed blueprint waiting for the overwrite confirmation.

	BlueprintTextWindow(WindowDesc &desc, Window *parent, std::string &&text, bool import_mode) :
			Window(desc), text_editbox(UINT16_MAX - 1), import_mode(import_mode), export_text(std::move(text))
	{
		this->parent = parent;
		this->CreateNestedTree();
		this->GetWidget<NWidgetStacked>(WID_BTX_SEL)->SetDisplayedPlane(import_mode ? 1 : 0);
		this->querystrings[WID_BTX_TEXT] = &this->text_editbox;
		this->text_editbox.caption = import_mode ? STR_BLUEPRINT_IMPORT_CAPTION : STR_BLUEPRINT_EXPORT_CAPTION;
		this->FinishInitNested(0);

		if (import_mode) {
			this->text_editbox.ok_button = WID_BTX_OK;
			this->text_editbox.cancel_button = WID_BTX_CANCEL;
		} else {
			this->text_editbox.text.Assign(this->export_text);
		}
		this->SetFocusedWidget(WID_BTX_TEXT);
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget == WID_BTX_CAPTION) {
			return GetString(this->import_mode ? STR_BLUEPRINT_IMPORT_CAPTION : STR_BLUEPRINT_EXPORT_CAPTION);
		}
		return this->Window::GetWidgetString(widget, stringid);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_BTX_COPY:
				if (!SetClipboardContents(this->export_text)) {
					ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_CLIPBOARD_EXPORT), {}, WarningLevel::Info);
				}
				break;

			case WID_BTX_PASTE:
				if (this->text_editbox.text.InsertClipboard()) {
					this->SetWidgetDirty(WID_BTX_TEXT);
					this->OnEditboxChanged(WID_BTX_TEXT);
				} else {
					ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_CLIPBOARD_PASTE), {}, WarningLevel::Info);
				}
				break;

			case WID_BTX_CANCEL:
				this->Close();
				break;

			case WID_BTX_OK:
				this->TryImport();
				break;

			default: break;
		}
	}

	/** Parse the entered text; ask for confirmation when the active slot is occupied. */
	void TryImport()
	{
		Blueprint blueprint;
		if (!ParseBlueprintText(this->text_editbox.text.GetText(), blueprint)) {
			ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_INVALID_TEXT), {}, WarningLevel::Info);
			return;
		}

		if (!ActiveBlueprint().IsEmpty()) {
			this->pending = std::move(blueprint);
			ShowQuery(GetEncodedString(STR_BLUEPRINT_QUERY_OVERWRITE_CAPTION), GetEncodedString(STR_BLUEPRINT_QUERY_OVERWRITE),
					this, BlueprintTextWindow::OverwriteCallback);
			return;
		}

		this->ApplyImport(std::move(blueprint));
	}

	/** Store the imported blueprint in the active slot and close the dialog. */
	void ApplyImport(Blueprint &&blueprint)
	{
		ActiveBlueprint() = std::move(blueprint);
		SaveBlueprintAutosave();
		_paste_preview_dirty = true;
		SetWindowDirty(WindowClass::BlueprintToolbar, 0);
		this->Close();
	}

	/** Callback of the overwrite confirmation query; the parent is the text dialog. */
	static void OverwriteCallback(Window *w, bool confirmed)
	{
		BlueprintTextWindow *btw = static_cast<BlueprintTextWindow *>(w);
		if (!confirmed || !btw->pending.has_value()) {
			btw->pending.reset();
			return;
		}
		Blueprint blueprint = std::move(*btw->pending);
		btw->pending.reset();
		btw->ApplyImport(std::move(blueprint));
	}
};

/**
 * Open the blueprint text dialog, replacing any open one.
 * @param parent The blueprint toolbar.
 * @param text Text to show (export mode); empty for import mode.
 * @param import_mode True for the import dialog, false for the export dialog.
 */
static void ShowBlueprintTextWindow(Window *parent, std::string &&text, bool import_mode)
{
	CloseWindowById(WindowClass::BlueprintText, 0);
	new BlueprintTextWindow(_blueprint_text_desc, parent, std::move(text), import_mode);
}

/** Whether any slot of the active page has content or a name worth warning about before overwriting. */
static bool AnyBlueprintSlotOccupied()
{
	uint first = ActiveBlueprintPageFirstSlot();
	for (uint i = 0; i < NUM_BLUEPRINT_SLOTS; i++) {
		const Blueprint &bp = GetBlueprint(first + i);
		if (!bp.IsEmpty() || !bp.name.empty()) return true;
	}
	return false;
}

/** Replace every slot of the active page with the given set and refresh the toolbar/preview. */
static void ApplyImportedBlueprintSet(std::vector<Blueprint> &&slots)
{
	uint first = ActiveBlueprintPageFirstSlot();
	for (uint i = 0; i < NUM_BLUEPRINT_SLOTS; i++) GetBlueprint(first + i) = std::move(slots[i]);
	SaveBlueprintAutosave();
	_paste_preview_dirty = true;
	SetWindowDirty(WindowClass::BlueprintToolbar, 0);
}

/** One entry in a #BlueprintFileWindow directory listing. */
struct BlueprintFileEntry {
	std::string name; ///< File or directory name, without any path.
	bool is_dir;       ///< True for directories (and the synthetic ".." parent entry).
};

/**
 * List the blueprint-set files and subdirectories directly inside a directory.
 * @param relative_dir Directory to list, relative to #GetBlueprintBaseDirectory, empty for the root.
 * @return The entries, subdirectories first, each group sorted by name; a ".." entry is prepended when not at the root.
 */
static std::vector<BlueprintFileEntry> ListBlueprintDirectory(const std::string &relative_dir)
{
	std::vector<BlueprintFileEntry> entries;

	std::error_code error_code;
	for (const auto &dir_entry : std::filesystem::directory_iterator(OTTD2FS(GetBlueprintBaseDirectory() + relative_dir), error_code)) {
		std::string name = FS2OTTD(dir_entry.path().filename().native());
		if (name.empty() || name.front() == '.') continue;

		if (dir_entry.is_directory()) {
			entries.push_back({std::move(name), true});
		} else if (std::string_view{name}.ends_with(BLUEPRINT_SET_FILE_EXTENSION)) {
			entries.push_back({std::move(name), false});
		}
	}

	std::sort(entries.begin(), entries.end(), [](const BlueprintFileEntry &a, const BlueprintFileEntry &b) {
		if (a.is_dir != b.is_dir) return a.is_dir;
		return a.name < b.name;
	});

	if (!relative_dir.empty()) entries.insert(entries.begin(), BlueprintFileEntry{"..", true});
	return entries;
}

static constexpr std::initializer_list<NWidgetPart> _nested_blueprint_file_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_BTF_CAPTION), SetStringTip(STR_BLUEPRINT_QUERY_EXPORT_FILE_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(WWT_LABEL, Colours::Grey, WID_BTF_PATH), SetFill(1, 0), SetPadding(2, 2, 0, 2), SetAlignment(SA_LEFT),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_VERTICAL),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_MATRIX, Colours::DarkGreen, WID_BTF_LIST), SetMinimalSize(220, 0), SetFill(1, 1), SetResize(1, 1),
						SetMatrixDataTip(1, 0, STR_BLUEPRINT_TOOLTIP_FILE_LIST), SetScrollbar(WID_BTF_SCROLLBAR),
				NWidget(NWID_VSCROLLBAR, Colours::DarkGreen, WID_BTF_SCROLLBAR),
			EndContainer(),
			NWidget(WWT_PANEL, Colours::DarkGreen),
				NWidget(WWT_EDITBOX, Colours::DarkGreen, WID_BTF_NAME), SetFill(1, 0), SetPadding(2, 2, 2, 2),
			EndContainer(),
		EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(WWT_PANEL, Colours::DarkGreen, WID_BTF_DETAILS), SetMinimalSize(180, 0), SetFill(1, 1), SetResize(0, 1),
			EndContainer(),
			NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BTF_DELETE), SetMinimalSize(0, 12), SetFill(1, 0),
					SetStringTip(STR_BLUEPRINT_DELETE, STR_BLUEPRINT_TOOLTIP_DELETE_FILE),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
			NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BTF_CANCEL), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_BUTTON_CANCEL),
			NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BTF_OK), SetMinimalSize(0, 12), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_EXPORT),
		EndContainer(),
		NWidget(WWT_RESIZEBOX, Colours::DarkGreen),
	EndContainer(),
};

static WindowDesc _blueprint_file_desc(__FILE__, __LINE__,
	WindowPosition::Center, nullptr, 0, 0,
	WindowClass::BlueprintFile, WindowClass::BlueprintToolbar,
	WindowDefaultFlag::Construction,
	_nested_blueprint_file_widgets
);

/** Dialog for exporting/importing one page of blueprint slots to/from a named file, with directory browsing. */
struct BlueprintFileWindow : Window {
	bool import_mode;                          ///< True for the import dialog, false for the export dialog.
	std::string current_dir;                   ///< Directory being browsed, relative to GetBlueprintBaseDirectory(); empty at the root.
	std::vector<BlueprintFileEntry> entries;    ///< Contents of #current_dir.
	Scrollbar *vscroll = nullptr;
	QueryString name_editbox;                  ///< Editbox holding the file name (without extension).
	std::string selected_file;                 ///< File whose contents are shown in the details panel; empty when none.
	std::vector<Blueprint> selected_slots;     ///< Contents of #selected_file.
	std::string pending_overwrite_file;         ///< File name awaiting an overwrite confirmation (export).
	std::vector<Blueprint> pending_import;      ///< Blueprint set awaiting a replace-all confirmation (import).

	BlueprintFileWindow(WindowDesc &desc, Window *parent, bool import_mode) :
			Window(desc), import_mode(import_mode), name_editbox(MAX_LENGTH_BLUEPRINT_FILE_NAME_CHARS * MAX_CHAR_LENGTH, MAX_LENGTH_BLUEPRINT_FILE_NAME_CHARS)
	{
		this->parent = parent;
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_BTF_SCROLLBAR);
		this->querystrings[WID_BTF_NAME] = &this->name_editbox;
		this->name_editbox.caption = import_mode ? STR_BLUEPRINT_QUERY_IMPORT_FILE_CAPTION : STR_BLUEPRINT_QUERY_EXPORT_FILE_CAPTION;
		this->name_editbox.ok_button = WID_BTF_OK;
		this->name_editbox.cancel_button = WID_BTF_CANCEL;
		this->GetWidget<NWidgetCore>(WID_BTF_OK)->SetString(import_mode ? STR_BLUEPRINT_IMPORT : STR_BLUEPRINT_EXPORT);
		this->FinishInitNested(0);
		this->RefreshFileList();
		this->SetFocusedWidget(WID_BTF_NAME);
	}

	/** Rescan #current_dir and reset the list scroll position. */
	void RefreshFileList()
	{
		this->entries = ListBlueprintDirectory(this->current_dir);
		if (std::none_of(this->entries.begin(), this->entries.end(),
				[this](const BlueprintFileEntry &e) { return !e.is_dir && e.name == this->selected_file; })) {
			this->selected_file.clear();
			this->selected_slots.clear();
			this->SetWidgetDisabledState(WID_BTF_DELETE, true);
		}
		this->vscroll->SetCount(this->entries.size());
		this->SetWidgetDirty(WID_BTF_LIST);
		this->SetWidgetDirty(WID_BTF_PATH);
	}

	/** Step #current_dir into a subdirectory, or up to its parent for "..". */
	void Navigate(const std::string &dir_name)
	{
		if (dir_name == "..") {
			/* Strip the trailing path separator, then everything after the next-to-last one. */
			size_t end = this->current_dir.size() - 1;
			size_t slash = this->current_dir.find_last_of(PATHSEPCHAR, end > 0 ? end - 1 : 0);
			this->current_dir = (slash == std::string::npos) ? std::string{} : this->current_dir.substr(0, slash + 1);
		} else {
			this->current_dir += dir_name + PATHSEP;
		}
		this->RefreshFileList();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_BTF_CAPTION:
				return GetString(this->import_mode ? STR_BLUEPRINT_QUERY_IMPORT_FILE_CAPTION : STR_BLUEPRINT_QUERY_EXPORT_FILE_CAPTION);

			case WID_BTF_PATH:
				return std::string{"blueprint" PATHSEP} + this->current_dir;

			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_BTF_LIST) return;
		fill.height = resize.height = GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.matrix.Vertical();
		size.height = 8 * resize.height;
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget == WID_BTF_DETAILS) {
			Rect ir = r.Shrink(WidgetDimensions::scaled.framerect);
			int line = GetCharacterHeight(FontSize::Normal);
			DrawString(ir.left, ir.right, ir.top, STR_BLUEPRINT_FILE_DETAILS_CAPTION);
			ir.top += line + WidgetDimensions::scaled.vsep_normal;

			if (this->selected_slots.size() != NUM_BLUEPRINT_SLOTS) {
				DrawStringMultiLine(ir.left, ir.right, ir.top, ir.bottom, STR_BLUEPRINT_FILE_DETAILS_HINT);
				return;
			}

			for (uint i = 0; i < NUM_BLUEPRINT_SLOTS; i++) {
				const Blueprint &bp = this->selected_slots[i];
				StringID str = bp.IsEmpty() ? STR_BLUEPRINT_FILE_DETAILS_SLOT_EMPTY :
						(bp.name.empty() ? STR_BLUEPRINT_FILE_DETAILS_SLOT_UNNAMED : STR_BLUEPRINT_FILE_DETAILS_SLOT_NAMED);
				if (bp.IsEmpty()) {
					DrawString(ir.left, ir.right, ir.top, GetString(str, i + 1));
				} else if (bp.name.empty()) {
					DrawString(ir.left, ir.right, ir.top, GetString(str, i + 1, bp.CountItems()));
				} else {
					DrawString(ir.left, ir.right, ir.top, GetString(str, i + 1, bp.name, bp.CountItems()));
				}
				ir.top += line;
			}
			return;
		}

		if (widget != WID_BTF_LIST) return;

		Rect ir = r.WithHeight(this->resize.step_height).Shrink(WidgetDimensions::scaled.matrix);
		auto [first, last] = this->vscroll->GetVisibleRangeIterators(this->entries);
		for (auto it = first; it != last; ++it) {
			std::string label = it->is_dir ? (it->name + PATHSEP) : it->name;
			DrawString(ir.left, ir.right, ir.top, label, it->is_dir ? TextColour::Yellow : TextColour::Black);
			ir = ir.Translate(0, this->resize.step_height);
		}
	}

	/**
	 * Show what a file holds in the details panel.
	 * @param name File name inside #current_dir, or empty to clear the panel.
	 */
	void SelectFile(const std::string &name)
	{
		this->selected_file = name;
		this->selected_slots.clear();
		if (!name.empty() && !ReadBlueprintSetFile(this->current_dir + name, this->selected_slots)) {
			this->selected_slots.clear();
		}
		this->SetWidgetDirty(WID_BTF_NAME);
		this->SetWidgetDirty(WID_BTF_DETAILS);
		this->SetWidgetDisabledState(WID_BTF_DELETE, this->selected_file.empty());
	}

	/** Callback of the delete confirmation query. */
	static void DeleteFileCallback(Window *w, bool confirmed)
	{
		BlueprintFileWindow *bfw = static_cast<BlueprintFileWindow *>(w);
		if (!confirmed || bfw->selected_file.empty()) return;

		std::error_code error_code;
		std::filesystem::remove(OTTD2FS(GetBlueprintBaseDirectory() + bfw->current_dir + bfw->selected_file), error_code);
		bfw->SelectFile({});
		bfw->RefreshFileList();
	}

	/** Validate the typed name and perform the export or import for it. */
	void TryConfirm()
	{
		auto name = SanitizeBlueprintFileName(this->name_editbox.text.GetText());
		if (!name.has_value()) {
			ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_INVALID_FILE_NAME), {}, WarningLevel::Info);
			return;
		}
		std::string relative_file = this->current_dir + *name + std::string{BLUEPRINT_SET_FILE_EXTENSION};

		if (this->import_mode) {
			std::vector<Blueprint> loaded;
			if (!ReadBlueprintSetFile(relative_file, loaded)) {
				ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_FILE_NOT_FOUND), {}, WarningLevel::Info);
				return;
			}
			if (AnyBlueprintSlotOccupied()) {
				this->pending_import = std::move(loaded);
				ShowQuery(GetEncodedString(STR_BLUEPRINT_QUERY_OVERWRITE_ALL_CAPTION), GetEncodedString(STR_BLUEPRINT_QUERY_OVERWRITE_ALL),
						this, BlueprintFileWindow::OverwriteAllCallback);
				return;
			}
			ApplyImportedBlueprintSet(std::move(loaded));
			this->Close();
		} else {
			if (FileExists(GetBlueprintBaseDirectory() + relative_file)) {
				this->pending_overwrite_file = std::move(relative_file);
				ShowQuery(GetEncodedString(STR_BLUEPRINT_QUERY_OVERWRITE_FILE_CAPTION), GetEncodedString(STR_BLUEPRINT_QUERY_OVERWRITE_FILE),
						this, BlueprintFileWindow::OverwriteFileCallback);
				return;
			}
			if (!WriteBlueprintFile(relative_file, BLUEPRINT_SET_FILE_PREFIX, ActiveBlueprintPageFirstSlot(), NUM_BLUEPRINT_SLOTS)) {
				ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_FILE_WRITE), {}, WarningLevel::Info);
				return;
			}
			this->Close();
		}
	}

	/** Callback of the export overwrite confirmation query. */
	static void OverwriteFileCallback(Window *w, bool confirmed)
	{
		BlueprintFileWindow *bfw = static_cast<BlueprintFileWindow *>(w);
		if (confirmed) {
			if (WriteBlueprintFile(bfw->pending_overwrite_file, BLUEPRINT_SET_FILE_PREFIX, ActiveBlueprintPageFirstSlot(), NUM_BLUEPRINT_SLOTS)) {
				bfw->pending_overwrite_file.clear();
				bfw->Close();
				return;
			}
			ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_FILE_WRITE), {}, WarningLevel::Info);
		}
		bfw->pending_overwrite_file.clear();
	}

	/** Callback of the import replace-all confirmation query. */
	static void OverwriteAllCallback(Window *w, bool confirmed)
	{
		BlueprintFileWindow *bfw = static_cast<BlueprintFileWindow *>(w);
		if (confirmed && bfw->pending_import.size() == NUM_BLUEPRINT_SLOTS) {
			ApplyImportedBlueprintSet(std::move(bfw->pending_import));
			bfw->pending_import.clear();
			bfw->Close();
			return;
		}
		bfw->pending_import.clear();
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_BTF_LIST: {
				auto it = this->vscroll->GetScrolledItemFromWidget(this->entries, pt.y, this, WID_BTF_LIST);
				if (it == this->entries.end()) return;

				if (it->is_dir) {
					this->Navigate(it->name);
					return;
				}

				this->name_editbox.text.Assign(it->name.substr(0, it->name.size() - BLUEPRINT_SET_FILE_EXTENSION.size()));
				this->SelectFile(it->name);
				if (click_count >= 2) this->TryConfirm();
				return;
			}

			case WID_BTF_DELETE:
				if (this->selected_file.empty()) return;
				ShowQuery(GetEncodedString(STR_BLUEPRINT_QUERY_DELETE_FILE_CAPTION), GetEncodedString(STR_BLUEPRINT_QUERY_DELETE_FILE),
						this, BlueprintFileWindow::DeleteFileCallback);
				return;

			case WID_BTF_CANCEL:
				this->Close();
				return;

			case WID_BTF_OK:
				this->TryConfirm();
				return;

			default: return;
		}
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_BTF_LIST);
	}
};

/**
 * Open the blueprint file dialog, replacing any open one.
 * @param parent The blueprint toolbar.
 * @param import_mode True for the import dialog, false for the export dialog.
 */
static void ShowBlueprintFileWindow(Window *parent, bool import_mode)
{
	CloseWindowById(WindowClass::BlueprintFile, 0);
	new BlueprintFileWindow(_blueprint_file_desc, parent, import_mode);
}

/** Blueprint toolbar managing class. */
struct BlueprintToolbarWindow : Window {
	WidgetID last_user_action = INVALID_WIDGET; ///< Last started user action.
	uint renaming_slot = 0;                     ///< Slot targeted by the in-flight rename query, if any, counted across all pages.

	BlueprintToolbarWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->InitNested(window_number);
	}

	Point OnInitialPosition(int16_t sm_width, [[maybe_unused]] int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		return AlignInitialConstructionToolbar(sm_width);
	}

	/**
	 * Get the sprite visualising the current transformation.
	 * @return Sprite to show on the transformation indicator button.
	 */
	SpriteID GetTransformationSprite() const
	{
		return SPR_BLUEPRINT_TRANSFORM_FIRST + _blueprint_state.options.rotation + (_blueprint_state.options.reflected ? 4 : 0);
	}

	/** Push the toolbar state into the widget lowered states and dynamic sprites. */
	void UpdateWidgetStates()
	{
		Colours page_colour = BLUEPRINT_PAGE_COLOURS[_blueprint_state.page];
		this->GetWidget<NWidgetCore>(WID_BT_PAGE)->colour = page_colour;
		for (uint i = 0; i < NUM_BLUEPRINT_SLOTS; i++) {
			this->SetWidgetLoweredState(WID_BT_SLOT_1 + i, !_blueprint_state.scratch && _blueprint_state.slot == i);
			this->GetWidget<NWidgetCore>(WID_BT_SLOT_1 + i)->colour = page_colour;
		}
		this->SetWidgetLoweredState(WID_BT_SLOT_SCRATCH, _blueprint_state.scratch);

		this->GetWidget<NWidgetCore>(WID_BT_TRANSFORMATION)->SetSprite(this->GetTransformationSprite());

		this->SetWidgetDisabledState(WID_BT_HEIGHT_UP, _blueprint_state.options.height_offset >= MAX_BLUEPRINT_HEIGHT_OFFSET);
		this->SetWidgetDisabledState(WID_BT_HEIGHT_DOWN, _blueprint_state.options.height_offset <= -MAX_BLUEPRINT_HEIGHT_OFFSET);

		this->SetWidgetDisabledState(WID_BT_PASTE, ActiveBlueprint().IsEmpty());

		/* Keep the selection rectangle in sync with the pasted size while the paste tool is active. */
		if (this->IsWidgetLowered(WID_BT_PASTE)) this->UpdatePasteSelectSize();
	}

	/**
	 * Arm the paste tool for the active blueprint, so that picking a slot leaves
	 * the ghost on the cursor with nothing else to press. An empty slot arms
	 * nothing - there would be no ghost to show - and a paste tool that is
	 * already armed is left alone rather than toggled back off.
	 */
	void ArmPasteTool()
	{
		if (ActiveBlueprint().IsEmpty() || this->IsWidgetLowered(WID_BT_PASTE)) return;

		/* Paste is greyed out while the active blueprint is empty, and that flag is only
		 * refreshed on the next paint - but HandlePlacePushButton refuses a disabled
		 * widget outright. Arming straight after a copy into an empty slot, or after
		 * picking a filled slot while an empty one was active, would silently do nothing.
		 * The check above has already established there is something to paste, so clear
		 * the flag now rather than waiting for the paint that would clear it anyway. */
		this->SetWidgetDisabledState(WID_BT_PASTE, false);

		HandlePlacePushButton(this, WID_BT_PASTE, SPR_CURSOR_QUERY, HT_RECT);
		if (this->IsWidgetLowered(WID_BT_PASTE)) this->UpdatePasteSelectSize();
		this->last_user_action = WID_BT_PASTE;
	}

	/** Set the viewport selection rectangle to the transformed size of the active blueprint. */
	void UpdatePasteSelectSize() const
	{
		const Blueprint &blueprint = ActiveBlueprint();
		bool swap_axes = (_blueprint_state.options.rotation % 2) != 0;
		uint16_t w = swap_axes ? blueprint.height : blueprint.width;
		uint16_t h = swap_axes ? blueprint.width : blueprint.height;
		SetTileSelectSize(std::max<uint16_t>(w, 1), std::max<uint16_t>(h, 1));
	}

	void OnPaint() override
	{
		this->UpdateWidgetStates();
		this->DrawWidgets();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (IsInsideMM(widget, WID_BT_SLOT_1, WID_BT_SLOT_1 + NUM_BLUEPRINT_SLOTS)) {
			return GetString(STR_BLUEPRINT_SLOT, widget - WID_BT_SLOT_1 + 1);
		}

		switch (widget) {
			case WID_BT_PAGE:
				return GetString(STR_BLUEPRINT_PAGE, BlueprintPageLetter(_blueprint_state.page));

			case WID_BT_HEIGHT_DISPLAY:
				return GetString(_blueprint_state.options.height_offset < 0 ? STR_BLUEPRINT_HEIGHT_NEGATIVE : STR_BLUEPRINT_HEIGHT_POSITIVE, abs(_blueprint_state.options.height_offset));

			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	/** Draw a small "has content" marker in the top-right corner of filled slot buttons. */
	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (!IsInsideMM(widget, WID_BT_SLOT_1, WID_BT_SLOT_SCRATCH + 1)) return;
		if (BlueprintOfSlotWidget(widget).IsEmpty()) return;

		int size = ScaleGUITrad(3);
		int inset = ScaleGUITrad(2);
		GfxFillRect(r.right - inset - size, r.top + inset, r.right - inset, r.top + inset + size, PC_GREEN);
	}

	/** Replace the tooltip of named or filled slot buttons with the name and schematic preview. */
	bool OnTooltip([[maybe_unused]] Point pt, WidgetID widget, TooltipCloseCondition close_cond) override
	{
		if (!IsInsideMM(widget, WID_BT_SLOT_1, WID_BT_SLOT_SCRATCH + 1)) return false;

		const Blueprint &blueprint = BlueprintOfSlotWidget(widget);
		if (blueprint.IsEmpty() && blueprint.name.empty()) return false;

		ShowBlueprintPreview(this, blueprint, close_cond);
		return true;
	}

	/**
	 * Open a rename query for a blueprint slot, prefilled with its current name.
	 * @param slot Slot to rename, counted across all pages.
	 */
	void ShowRenameQuery(uint slot)
	{
		this->renaming_slot = slot;
		ShowQueryString(GetBlueprint(slot).name, STR_BLUEPRINT_QUERY_RENAME_CAPTION, MAX_LENGTH_BLUEPRINT_NAME_CHARS,
				this, CS_ALPHANUMERAL, {QueryStringFlag::LengthIsInChars});
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		if (!str.has_value()) return;
		GetBlueprint(this->renaming_slot).name = std::move(*str);
		SaveBlueprintAutosave();
		this->SetDirty();
	}

	/** Entries of the "Export/Import" menu. */
	enum ExportImportMenuItem : int {
		EIMI_EXPORT_SLOT, ///< Export the active slot as text.
		EIMI_EXPORT_ALL,  ///< Export the active page to a file.
		EIMI_IMPORT_SLOT, ///< Import text into the active slot.
		EIMI_IMPORT_ALL,  ///< Import a page from a file.
		EIMI_CLEAR_SLOT,  ///< Throw away the contents of the active slot.
	};

	/** Callback of the clear-slot confirmation query. */
	static void ClearSlotCallback(Window *w, bool confirmed)
	{
		if (!confirmed) return;

		ActiveBlueprint().Clear();
		SaveBlueprintAutosave();
		_paste_preview_dirty = true;
		if (w->IsWidgetLowered(WID_BT_PASTE)) MarkWholeScreenDirty();
		w->SetDirty();
	}

	/** Open the slot page menu, listing every page with which of its slots are filled. */
	void ShowPageMenu()
	{
		DropDownList list;
		for (uint page = 0; page < NUM_BLUEPRINT_SLOT_PAGES; page++) {
			std::string label = GetString(STR_BLUEPRINT_PAGE_MENU_ITEM, BlueprintPageLetter(page), BlueprintPageOccupancy(page));
			list.push_back(MakeDropDownListStringItem(std::move(label), page));
		}
		ShowDropDownList(this, std::move(list), _blueprint_state.page, WID_BT_PAGE);
	}

	/** Entries of the paste-options menu. */
	enum PasteOptionsMenuItem : int {
		POMI_RAIL,             ///< Toggle: paste rail transport infrastructure.
		POMI_ROAD,             ///< Toggle: paste road transport infrastructure.
		POMI_WATER,            ///< Toggle: paste water transport infrastructure.
		POMI_AIR,              ///< Toggle: paste air transport infrastructure.
		POMI_CONVERT_RAILTYPE, ///< Toggle: convert rail to the current rail type when pasting.
		POMI_MIRROR_SIGNALS,   ///< Toggle: mirror signals when pasting.
		POMI_UPGRADE_BRIDGES,  ///< Toggle: upgrade bridges when pasting.
		POMI_WITH_STATIONS,    ///< Toggle: paste stations and waypoints too.
		POMI_TERRAFORM_NONE,   ///< Terraform mode: leave the landscape alone.
		POMI_TERRAFORM_MINIMAL,///< Terraform mode: only the tiles that receive elements.
		POMI_TERRAFORM_FULL,   ///< Terraform mode: the whole pasted area.
	};

	/** Build the paste-options menu, with every entry ticked to match the current settings. */
	DropDownList BuildPasteOptionsList() const
	{
		const BlueprintPasteOptions &o = _blueprint_state.options;
		DropDownList list;
		list.push_back(MakeDropDownListCheckedItem(o.paste_rail, STR_BLUEPRINT_OPTIONS_MENU_RAIL, POMI_RAIL));
		list.push_back(MakeDropDownListCheckedItem(o.paste_road, STR_BLUEPRINT_OPTIONS_MENU_ROAD, POMI_ROAD));
		list.push_back(MakeDropDownListCheckedItem(o.paste_water, STR_BLUEPRINT_OPTIONS_MENU_WATER, POMI_WATER));
		list.push_back(MakeDropDownListCheckedItem(o.paste_air, STR_BLUEPRINT_OPTIONS_MENU_AIR, POMI_AIR));
		list.push_back(MakeDropDownListDividerItem());
		list.push_back(MakeDropDownListCheckedItem(o.with_stations, STR_BLUEPRINT_OPTIONS_MENU_WITH_STATIONS, POMI_WITH_STATIONS));
		list.push_back(MakeDropDownListCheckedItem(o.convert_railtype, STR_BLUEPRINT_OPTIONS_MENU_CONVERT_RAILTYPE, POMI_CONVERT_RAILTYPE));
		list.push_back(MakeDropDownListCheckedItem(o.mirror_signals, STR_BLUEPRINT_OPTIONS_MENU_MIRROR_SIGNALS, POMI_MIRROR_SIGNALS));
		list.push_back(MakeDropDownListCheckedItem(o.upgrade_bridges, STR_BLUEPRINT_OPTIONS_MENU_UPGRADE_BRIDGES, POMI_UPGRADE_BRIDGES));
		list.push_back(MakeDropDownListDividerItem());
		/* The three terraform modes are mutually exclusive, so exactly one of them
		 * carries the tick and picking one replaces rather than toggles it. */
		list.push_back(MakeDropDownListCheckedItem(o.terraform_mode == BlueprintTerraformMode::None, STR_BLUEPRINT_OPTIONS_MENU_TERRAFORM_NONE, POMI_TERRAFORM_NONE));
		list.push_back(MakeDropDownListCheckedItem(o.terraform_mode == BlueprintTerraformMode::Minimal, STR_BLUEPRINT_OPTIONS_MENU_TERRAFORM_MINIMAL, POMI_TERRAFORM_MINIMAL));
		list.push_back(MakeDropDownListCheckedItem(o.terraform_mode == BlueprintTerraformMode::Full, STR_BLUEPRINT_OPTIONS_MENU_TERRAFORM_FULL, POMI_TERRAFORM_FULL));
		return list;
	}

	/**
	 * Apply one paste-options menu entry.
	 * @param index The chosen #PasteOptionsMenuItem.
	 */
	void TogglePasteOption(int index)
	{
		BlueprintPasteOptions &o = _blueprint_state.options;
		switch (index) {
			case POMI_RAIL: o.paste_rail = !o.paste_rail; break;
			case POMI_ROAD: o.paste_road = !o.paste_road; break;
			case POMI_WATER: o.paste_water = !o.paste_water; break;
			case POMI_AIR: o.paste_air = !o.paste_air; break;
			case POMI_CONVERT_RAILTYPE: o.convert_railtype = !o.convert_railtype; break;
			case POMI_MIRROR_SIGNALS: o.mirror_signals = !o.mirror_signals; break;
			case POMI_UPGRADE_BRIDGES: o.upgrade_bridges = !o.upgrade_bridges; break;
			case POMI_WITH_STATIONS: o.with_stations = !o.with_stations; break;
			case POMI_TERRAFORM_NONE: o.terraform_mode = BlueprintTerraformMode::None; break;
			case POMI_TERRAFORM_MINIMAL: o.terraform_mode = BlueprintTerraformMode::Minimal; break;
			case POMI_TERRAFORM_FULL: o.terraform_mode = BlueprintTerraformMode::Full; break;
			default: return;
		}

		SaveBlueprintStateToSettings();
		_paste_preview_dirty = true;
		if (this->IsWidgetLowered(WID_BT_PASTE)) MarkWholeScreenDirty();
		this->SetDirty();
	}

	/** Switch the slot buttons to another page, keeping the slot number within it. */
	void SetPage(uint8_t page)
	{
		if (page == _blueprint_state.page) return;

		_blueprint_state.page = page;
		_paste_preview_dirty = true;
		if (this->IsWidgetLowered(WID_BT_PASTE)) MarkWholeScreenDirty();
		this->SetDirty();
	}

	/** Open the "Export/Import" menu: single slot via text, or the whole page via a file. */
	void ShowExportImportMenu()
	{
		DropDownList list;
		list.push_back(MakeDropDownListStringItem(STR_BLUEPRINT_EXPORT_MENU_SLOT, EIMI_EXPORT_SLOT, ActiveBlueprint().IsEmpty()));
		list.push_back(MakeDropDownListStringItem(STR_BLUEPRINT_EXPORT_MENU_ALL, EIMI_EXPORT_ALL, !AnyBlueprintSlotOccupied()));
		list.push_back(MakeDropDownListDividerItem());
		list.push_back(MakeDropDownListStringItem(STR_BLUEPRINT_IMPORT_MENU_SLOT, EIMI_IMPORT_SLOT));
		list.push_back(MakeDropDownListStringItem(STR_BLUEPRINT_IMPORT_MENU_ALL, EIMI_IMPORT_ALL));
		list.push_back(MakeDropDownListDividerItem());
		list.push_back(MakeDropDownListStringItem(STR_BLUEPRINT_CLEAR_MENU_SLOT, EIMI_CLEAR_SLOT, ActiveBlueprint().IsEmpty()));
		ShowDropDownList(this, std::move(list), -1, WID_BT_EXPORT_IMPORT);
	}

	void OnDropdownSelect(WidgetID widget, int index, [[maybe_unused]] int click_result) override
	{
		if (widget == WID_BT_PAGE) {
			this->SetPage(static_cast<uint8_t>(index));
			return;
		}

		if (widget == WID_BT_PASTE_OPTIONS) {
			this->TogglePasteOption(index);
			/* The menu stays open for the next toggle, so its ticks have to be redrawn. */
			ReplaceDropDownList(this, this->BuildPasteOptionsList(), -1);
			return;
		}

		if (widget != WID_BT_EXPORT_IMPORT) return;
		switch (index) {
			case EIMI_EXPORT_SLOT:
				ShowBlueprintTextWindow(this, EncodeBlueprintText(ActiveBlueprint()), false);
				break;

			case EIMI_EXPORT_ALL:
				ShowBlueprintFileWindow(this, false);
				break;

			case EIMI_IMPORT_SLOT:
				ShowBlueprintTextWindow(this, std::string{}, true);
				break;

			case EIMI_IMPORT_ALL:
				ShowBlueprintFileWindow(this, true);
				break;

			case EIMI_CLEAR_SLOT:
				/* Slots are the only place a blueprint lives, and clearing one cannot
				 * be undone, so ask before throwing the contents away. */
				ShowQuery(GetEncodedString(STR_BLUEPRINT_QUERY_CLEAR_SLOT_CAPTION), GetEncodedString(STR_BLUEPRINT_QUERY_CLEAR_SLOT),
						this, BlueprintToolbarWindow::ClearSlotCallback);
				break;

			default: break;
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (IsInsideMM(widget, WID_BT_SLOT_1, WID_BT_SLOT_SCRATCH + 1)) {
			/* The scratch buffer is deliberately not nameable: it is thrown away on
			 * exit, so a name on it would only ever mislead. */
			if (click_count >= 2 && widget != WID_BT_SLOT_SCRATCH) {
				this->ShowRenameQuery(ActiveBlueprintPageFirstSlot() + (widget - WID_BT_SLOT_1));
				return;
			}

			_blueprint_state.scratch = (widget == WID_BT_SLOT_SCRATCH);
			if (!_blueprint_state.scratch) _blueprint_state.slot = static_cast<uint8_t>(widget - WID_BT_SLOT_1);
			_paste_preview_dirty = true;
			if (this->IsWidgetLowered(WID_BT_PASTE)) MarkWholeScreenDirty();
			this->ArmPasteTool();
			this->SetDirty();
			return;
		}

		switch (widget) {
			case WID_BT_PAGE:
				this->ShowPageMenu();
				return;

			case WID_BT_PASTE_OPTIONS:
				ShowDropDownList(this, this->BuildPasteOptionsList(), -1, WID_BT_PASTE_OPTIONS, 0, DropDownOption::Persist);
				return;

			case WID_BT_COPY:
				HandlePlacePushButton(this, WID_BT_COPY, SPR_CURSOR_CLONE_TRAIN, HT_RECT);
				this->last_user_action = widget;
				break;

			case WID_BT_PASTE:
				HandlePlacePushButton(this, WID_BT_PASTE, SPR_CURSOR_QUERY, HT_RECT);
				if (this->IsWidgetLowered(WID_BT_PASTE)) this->UpdatePasteSelectSize();
				this->last_user_action = widget;
				break;

			case WID_BT_EXPORT_IMPORT:
				this->ShowExportImportMenu();
				return;

			case WID_BT_INFO:
				ShowBlueprintInfo(BlueprintInfoSubject::Blueprint);
				return;

			case WID_BT_TRANSFORMATION:
				_blueprint_state.options.ResetTransformation();
				break;

			case WID_BT_ROTATE_CCW:
				_blueprint_state.options.AddRotation(1);
				break;

			case WID_BT_ROTATE_CW:
				_blueprint_state.options.AddRotation(-1);
				break;

			case WID_BT_REFLECT_NW_SE:
				_blueprint_state.options.AddReflection(0);
				break;

			case WID_BT_REFLECT_NE_SW:
				_blueprint_state.options.AddReflection(2);
				break;

			case WID_BT_HEIGHT_UP:
				_blueprint_state.options.height_offset = std::min<int>(_blueprint_state.options.height_offset + 1, MAX_BLUEPRINT_HEIGHT_OFFSET);
				break;

			case WID_BT_HEIGHT_DOWN:
				_blueprint_state.options.height_offset = std::max<int>(_blueprint_state.options.height_offset - 1, -MAX_BLUEPRINT_HEIGHT_OFFSET);
				break;

			default: return;
		}
		SaveBlueprintStateToSettings();
		_paste_preview_dirty = true;
		if (this->IsWidgetLowered(WID_BT_PASTE)) MarkWholeScreenDirty();
		this->SetDirty();
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		switch (this->last_user_action) {
			case WID_BT_COPY:
				VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_BLUEPRINT_COPY);
				break;

			case WID_BT_PASTE: {
				TileIndex origin = GetBlueprintPasteOrigin(tile);
				if (origin == INVALID_TILE) break;

				BlueprintPasteData data = PrepareBlueprintPaste(ActiveBlueprint(), _blueprint_state.options, GetCurrentRailType());
				if (data.blueprint.IsEmpty()) break;
				if (_networking && GetBlueprintSerialisedSize(data) > MAX_BLUEPRINT_COMMAND_SIZE) {
					ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_TOO_LARGE), {}, WarningLevel::Info);
					break;
				}
				Command<Commands::PasteBlueprint>::Post(STR_ERROR_CAN_T_PASTE_BLUEPRINT, CommandCallback::PasteBlueprint, origin, data);
				break;
			}

			default: NOT_REACHED();
		}
	}

	void OnPlaceDrag(ViewportPlaceMethod select_method, [[maybe_unused]] ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt) override
	{
		VpSelectTilesWithMethod(pt.x, pt.y, select_method);
	}

	void OnPlaceMouseUp([[maybe_unused]] ViewportPlaceMethod select_method, ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt, TileIndex start_tile, TileIndex end_tile) override
	{
		if (pt.x == -1) return;

		switch (select_proc) {
			case DDSP_BLUEPRINT_COPY: {
				TileArea area(start_tile, end_tile);
				if (area.w > MAX_BLUEPRINT_DIMENSION || area.h > MAX_BLUEPRINT_DIMENSION) {
					ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_AREA_TOO_LARGE), {}, WarningLevel::Info);
					break;
				}
				uint count = CopyAreaToBlueprint(area, ActiveBlueprint());
				SaveBlueprintAutosave();
				_paste_preview_dirty = true;
				if (count == 0) {
					ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_NOTHING_TO_COPY), {}, WarningLevel::Info);
				} else {
					if (_settings_client.sound.confirm) SndPlayTileFx(SND_1F_CONSTRUCTION_OTHER, end_tile);
					/* A copy into the scratch buffer is made to be stamped down straight
					 * away, so it hands the cursor to the paste tool. A copy into a
					 * numbered slot does not: filling several slots in a row is the
					 * normal way to use them, and re-arming between each would fight it. */
					if (_blueprint_state.scratch) this->ArmPasteTool();
				}
				this->SetDirty();
				break;
			}

			default: NOT_REACHED();
		}
	}

	TileIndex last_cost_origin = INVALID_TILE; ///< Origin the paste cost tooltip was last shown for.

	void OnPlaceObjectAbort() override
	{
		this->RaiseWidget(WID_BT_COPY);
		this->RaiseWidget(WID_BT_PASTE);
		this->last_cost_origin = INVALID_TILE;
		this->SetDirty();
	}

	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		/* Show the estimated paste cost (computed with the blocked mask during
		 * viewport drawing) as a hover tooltip, like the drag measurement tooltips. */
		if (!_settings_client.gui.measure_tooltip || !this->IsWidgetLowered(WID_BT_PASTE)) return;
		if (_paste_preview_origin == INVALID_TILE || _paste_preview_origin == this->last_cost_origin) return;
		this->last_cost_origin = _paste_preview_origin;
		if (_paste_preview_cost <= 0) return;
		GuiShowTooltips(this, GetEncodedString(STR_MESSAGE_ESTIMATED_COST, _paste_preview_cost), TooltipCloseCondition::ExitViewport);
	}

	static EventState BlueprintToolbarGlobalHotkeys(int hotkey)
	{
		if (_game_mode != GameMode::Normal) return EventState::NotHandled;
		Window *w = FindWindowById(WindowClass::BlueprintToolbar, 0);
		if (w == nullptr) w = ShowBlueprintToolbar();
		if (w == nullptr) return EventState::NotHandled;
		return w->OnHotkey(hotkey);
	}

	/** Hotkey numbers for actions that no longer have a button of their own. */
	enum BlueprintHotkey : int {
		HK_MIRROR_SIGNALS = WID_BT_INFO + 1, ///< Toggle mirror-signals, which now lives in the paste-options menu.
	};

	EventState OnHotkey(int hotkey) override
	{
		if (hotkey == HK_MIRROR_SIGNALS) {
			this->TogglePasteOption(POMI_MIRROR_SIGNALS);
			return EventState::Handled;
		}
		return this->Window::OnHotkey(hotkey);
	}

	static inline HotkeyList hotkeys{"blueprint_toolbar", {
		Hotkey('1', "slot_1", WID_BT_SLOT_1),
		Hotkey('2', "slot_2", WID_BT_SLOT_2),
		Hotkey('3', "slot_3", WID_BT_SLOT_3),
		Hotkey('4', "slot_4", WID_BT_SLOT_4),
		Hotkey('5', "slot_5", WID_BT_SLOT_5),
		Hotkey('6', "slot_6", WID_BT_SLOT_6),
		Hotkey('7', "slot_7", WID_BT_SLOT_7),
		Hotkey('8', "slot_8", WID_BT_SLOT_8),
		Hotkey('9', "slot_scratch", WID_BT_SLOT_SCRATCH),
		Hotkey('C', "copy", WID_BT_COPY),
		Hotkey('V', "paste", WID_BT_PASTE),
		Hotkey('Q', "rotate_ccw", WID_BT_ROTATE_CCW),
		Hotkey('E', "rotate_cw", WID_BT_ROTATE_CW),
		Hotkey('F', "reflect_nw_se", WID_BT_REFLECT_NW_SE),
		Hotkey('G', "reflect_ne_sw", WID_BT_REFLECT_NE_SW),
		Hotkey('M', "mirror_signals", BlueprintToolbarWindow::HK_MIRROR_SIGNALS),
		Hotkey('X', "reset_transform", WID_BT_TRANSFORMATION),
	}, BlueprintToolbarGlobalHotkeys};
};

static constexpr std::initializer_list<NWidgetPart> _nested_blueprint_toolbar_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen), SetStringTip(STR_BLUEPRINT_TOOLBAR, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BT_PAGE), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_PAGE, STR_BLUEPRINT_TOOLTIP_PAGE),

		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_1), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_2), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_3), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_4), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_5), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_6), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_7), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BT_SLOT_8), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT, STR_BLUEPRINT_TOOLTIP_SLOT),
		NWidget(WWT_TEXTBTN, Colours::Cream, WID_BT_SLOT_SCRATCH), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_SLOT_SCRATCH, STR_BLUEPRINT_TOOLTIP_SLOT_SCRATCH),

		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), EndContainer(),

		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_BT_COPY), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_COPY, STR_BLUEPRINT_TOOLTIP_COPY),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_BT_PASTE), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_PASTE, STR_BLUEPRINT_TOOLTIP_PASTE),

		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), EndContainer(),

		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_TRANSFORMATION), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_TRANSFORM_FIRST, STR_BLUEPRINT_TOOLTIP_TRANSFORMATION),
		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_ROTATE_CCW), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_ROTATE_CCW, STR_BLUEPRINT_TOOLTIP_ROTATE_CCW),
		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_ROTATE_CW), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_ROTATE_CW, STR_BLUEPRINT_TOOLTIP_ROTATE_CW),
		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_REFLECT_NW_SE), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_REFLECT_NW_SE, STR_BLUEPRINT_TOOLTIP_REFLECT_NW_SE),
		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_REFLECT_NE_SW), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_REFLECT_NE_SW, STR_BLUEPRINT_TOOLTIP_REFLECT_NE_SW),

		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), EndContainer(),

		NWidget(WWT_PANEL, Colours::DarkGreen),
			NWidget(WWT_LABEL, Colours::Grey, WID_BT_HEIGHT_DISPLAY), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetStringTip(STR_BLUEPRINT_HEIGHT_POSITIVE, STR_BLUEPRINT_TOOLTIP_HEIGHT_DISPLAY),
		EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_HEIGHT_UP), SetMinimalSize(12, 11),
								SetFill(0, 1), SetSpriteTip(SPR_ARROW_UP, STR_BLUEPRINT_TOOLTIP_HEIGHT_UP),
			NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_HEIGHT_DOWN), SetMinimalSize(12, 11),
								SetFill(0, 1), SetSpriteTip(SPR_ARROW_DOWN, STR_BLUEPRINT_TOOLTIP_HEIGHT_DOWN),
		EndContainer(),

		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), EndContainer(),

		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_EXPORT_IMPORT), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_EXPORT_IMPORT, STR_BLUEPRINT_TOOLTIP_EXPORT_IMPORT),
		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_PASTE_OPTIONS), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_OPTIONS, STR_BLUEPRINT_TOOLTIP_PASTE_OPTIONS),

		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), EndContainer(),

		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BT_INFO), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_IMG_QUERY, STR_BLUEPRINT_TOOLTIP_INFO),
	EndContainer(),
};

static WindowDesc _blueprint_toolbar_desc(__FILE__, __LINE__,
	WindowPosition::Manual, "toolbar_blueprint", 0, 0,
	WindowClass::BlueprintToolbar, WindowClass::None,
	WindowDefaultFlag::Construction,
	_nested_blueprint_toolbar_widgets,
	&BlueprintToolbarWindow::hotkeys
);

/**
 * The undo toolbar.
 *
 * Undo lives on a toolbar of its own rather than among the blueprint tools: it
 * is not a blueprint action any more once hand-built work is recorded too, and
 * a button sitting on the rail toolbar was read as "undo the track I just laid"
 * during testing.
 */
struct BlueprintUndoToolbarWindow : Window {
	BlueprintUndoToolbarWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->InitNested(window_number);
	}

	Point OnInitialPosition(int16_t sm_width, [[maybe_unused]] int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		/* Sit with the other construction toolbars, but next to the blueprint
		 * toolbar rather than on top of it when both are open. */
		Point pt = AlignInitialConstructionToolbar(sm_width);
		const Window *w = FindWindowById(WindowClass::BlueprintToolbar, 0);
		if (w != nullptr && w->top == pt.y) {
			pt.x = w->left + (_current_text_dir == TD_RTL ? w->width : -sm_width);
		}
		return pt;
	}

	void OnPaint() override
	{
		this->SetWidgetDisabledState(WID_BUT_UNDO, !HasBlueprintUndoRecord());
		this->DrawWidgets();
	}

	/** Entries of the undo-options menu. */
	enum UndoOptionsMenuItem : int {
		UOMI_RAIL,    ///< Toggle: undo rail infrastructure.
		UOMI_ROAD,    ///< Toggle: undo road infrastructure.
		UOMI_WATER,   ///< Toggle: undo water infrastructure.
		UOMI_AIR,     ///< Toggle: undo airports.
		UOMI_OBJECTS, ///< Toggle: undo NewGRF objects.
		UOMI_TERRAIN, ///< Toggle: restore the landscape too, not only remove what was built.
	};

	/** Build the undo-options menu, with every entry ticked to match the current settings. */
	DropDownList BuildUndoOptionsList() const
	{
		const GUISettings &g = _settings_client.gui;
		DropDownList list;
		list.push_back(MakeDropDownListCheckedItem(g.blueprint_undo_rail, STR_BLUEPRINT_OPTIONS_MENU_RAIL, UOMI_RAIL));
		list.push_back(MakeDropDownListCheckedItem(g.blueprint_undo_road, STR_BLUEPRINT_OPTIONS_MENU_ROAD, UOMI_ROAD));
		list.push_back(MakeDropDownListCheckedItem(g.blueprint_undo_water, STR_BLUEPRINT_OPTIONS_MENU_WATER, UOMI_WATER));
		list.push_back(MakeDropDownListCheckedItem(g.blueprint_undo_air, STR_BLUEPRINT_OPTIONS_MENU_AIR, UOMI_AIR));
		list.push_back(MakeDropDownListCheckedItem(_blueprint_undo_objects, STR_BLUEPRINT_OPTIONS_MENU_OBJECTS, UOMI_OBJECTS));
		list.push_back(MakeDropDownListDividerItem());
		list.push_back(MakeDropDownListCheckedItem(g.blueprint_undo_terrain, STR_BLUEPRINT_OPTIONS_MENU_RESTORE_LAND, UOMI_TERRAIN));
		return list;
	}

	/**
	 * Apply one undo-options menu entry.
	 * @param index The chosen #UndoOptionsMenuItem.
	 */
	void ToggleUndoOption(int index)
	{
		GUISettings &g = _settings_client.gui;
		switch (index) {
			case UOMI_RAIL: g.blueprint_undo_rail = !g.blueprint_undo_rail; break;
			case UOMI_ROAD: g.blueprint_undo_road = !g.blueprint_undo_road; break;
			case UOMI_WATER: g.blueprint_undo_water = !g.blueprint_undo_water; break;
			case UOMI_AIR: g.blueprint_undo_air = !g.blueprint_undo_air; break;
			case UOMI_OBJECTS: _blueprint_undo_objects = !_blueprint_undo_objects; break;

			case UOMI_TERRAIN:
				g.blueprint_undo_terrain = !g.blueprint_undo_terrain;
				_blueprint_state.undo_terrain = g.blueprint_undo_terrain;
				break;

			default: return;
		}

		/* The history re-states what an undo would take back, so it follows the filters. */
		InvalidateBlueprintUndoToolbars();
		this->SetDirty();
	}

	void OnDropdownSelect(WidgetID widget, int index, [[maybe_unused]] int click_result) override
	{
		if (widget != WID_BUT_OPTIONS) return;
		this->ToggleUndoOption(index);
		/* The menu stays open for the next toggle, so its ticks have to be redrawn. */
		ReplaceDropDownList(this, this->BuildUndoOptionsList(), -1);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_BUT_UNDO:
				UndoLastBlueprintPaste();
				break;

			case WID_BUT_OPTIONS:
				ShowDropDownList(this, this->BuildUndoOptionsList(), -1, WID_BUT_OPTIONS, 0, DropDownOption::Persist);
				return;

			case WID_BUT_LIST:
				ShowBlueprintUndoList();
				break;

			case WID_BUT_INFO:
				ShowBlueprintInfo(BlueprintInfoSubject::Undo);
				return;

			default: return;
		}
		this->SetDirty();
	}

	static EventState BlueprintUndoToolbarGlobalHotkeys(int hotkey)
	{
		if (_game_mode != GameMode::Normal) return EventState::NotHandled;
		Window *w = FindWindowById(WindowClass::BlueprintUndoToolbar, 0);
		if (w == nullptr) w = ShowBlueprintUndoToolbar();
		if (w == nullptr) return EventState::NotHandled;
		return w->OnHotkey(hotkey);
	}

	static inline HotkeyList hotkeys{"blueprint_undo_toolbar", {
		Hotkey('Z', "undo", WID_BUT_UNDO),
	}, BlueprintUndoToolbarGlobalHotkeys};
};

static constexpr std::initializer_list<NWidgetPart> _nested_blueprint_undo_toolbar_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen), SetStringTip(STR_BLUEPRINT_UNDO_TOOLBAR, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BUT_UNDO), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_UNDO, STR_BLUEPRINT_TOOLTIP_UNDO),
		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), EndContainer(),

		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BUT_LIST), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_UNDO_LIST, STR_BLUEPRINT_TOOLTIP_UNDO_LIST),

		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), EndContainer(),

		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BUT_OPTIONS), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_BLUEPRINT_OPTIONS, STR_BLUEPRINT_TOOLTIP_UNDO_OPTIONS),

		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), EndContainer(),

		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_BUT_INFO), SetToolbarMinimalSize(1),
								SetFill(0, 1), SetSpriteTip(SPR_IMG_QUERY, STR_BLUEPRINT_TOOLTIP_INFO),
	EndContainer(),
};

static WindowDesc _blueprint_undo_toolbar_desc(__FILE__, __LINE__,
	WindowPosition::Manual, "toolbar_blueprint_undo", 0, 0,
	WindowClass::BlueprintUndoToolbar, WindowClass::None,
	WindowDefaultFlag::Construction,
	_nested_blueprint_undo_toolbar_widgets,
	&BlueprintUndoToolbarWindow::hotkeys
);

/** Tiles the selected history entries cover, sorted so a lookup per drawn tile is cheap. */
static std::vector<uint32_t> _undo_highlight;

/**
 * Is this tile covered by the current undo history selection?
 *
 * Called for every tile the viewport draws, so it has to be cheap when nothing
 * is selected, which is nearly always.
 *
 * @param tile Tile being drawn.
 * @return Whether to highlight it.
 */
bool IsTileInBlueprintUndoHighlight(TileIndex tile)
{
	if (_undo_highlight.empty()) return false;
	return std::binary_search(_undo_highlight.begin(), _undo_highlight.end(), tile.base());
}

/**
 * Set the tiles highlighted for the undo history selection.
 * @param tiles Tiles to highlight; may contain duplicates.
 */
static void SetBlueprintUndoHighlight(std::vector<TileIndex> &&tiles)
{
	_undo_highlight.clear();
	_undo_highlight.reserve(tiles.size());
	for (TileIndex tile : tiles) _undo_highlight.push_back(tile.base());
	std::sort(_undo_highlight.begin(), _undo_highlight.end());
	_undo_highlight.erase(std::unique(_undo_highlight.begin(), _undo_highlight.end()), _undo_highlight.end());
	MarkWholeScreenDirty();
}

static constexpr std::initializer_list<NWidgetPart> _nested_blueprint_undo_list_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen), SetStringTip(STR_BLUEPRINT_UNDO_LIST_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::DarkGreen),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_MATRIX, Colours::DarkGreen, WID_BUL_LIST), SetMinimalSize(260, 0), SetFill(1, 1), SetResize(1, 1),
				SetMatrixDataTip(1, 0, STR_BLUEPRINT_TOOLTIP_UNDO_LIST_LIST), SetScrollbar(WID_BUL_SCROLLBAR),
		NWidget(NWID_VSCROLLBAR, Colours::DarkGreen, WID_BUL_SCROLLBAR),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BUL_UNDO_SELECTED), SetMinimalSize(0, 12), SetFill(1, 0), SetResize(1, 0),
				SetStringTip(STR_BLUEPRINT_UNDO_LIST_UNDO_SELECTED, STR_BLUEPRINT_TOOLTIP_UNDO_LIST_UNDO_SELECTED),
		NWidget(WWT_RESIZEBOX, Colours::DarkGreen),
	EndContainer(),
};

static WindowDesc _blueprint_undo_list_desc(__FILE__, __LINE__,
	WindowPosition::Center, "blueprint_undo_list", 260, 140,
	WindowClass::BlueprintUndoList, WindowClass::None,
	{},
	_nested_blueprint_undo_list_widgets
);

/**
 * The undo history: what can still be taken back, newest first.
 *
 * Rows are selected the way a file list is - plain click to pick one, Ctrl to
 * add or remove, Shift for a range - and the button undoes everything selected.
 */
struct BlueprintUndoListWindow : Window {
	std::set<size_t> selected; ///< Selected rows, as indices into the stack.
	size_t anchor = 0;         ///< Row a Shift+click extends from.
	Scrollbar *vscroll = nullptr;

	BlueprintUndoListWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_BUL_SCROLLBAR);
		this->FinishInitNested(window_number);
	}

	~BlueprintUndoListWindow()
	{
		SetBlueprintUndoHighlight({});
	}

	/** Redraw the map highlight for whatever is selected now. */
	void RefreshHighlight() const
	{
		const std::vector<BlueprintUndoRecord> &stack = GetBlueprintUndoStack();
		std::vector<TileIndex> tiles;
		for (size_t index : this->selected) {
			if (index < stack.size()) CollectBlueprintUndoTiles(stack[index], GetBlueprintUndoFilters(), tiles);
		}
		SetBlueprintUndoHighlight(std::move(tiles));
	}

	/** Stack index shown on a display row; the newest record is at the top. */
	size_t RowToIndex(size_t row) const
	{
		size_t count = GetBlueprintUndoStack().size();
		return count - 1 - row;
	}

	void OnPaint() override
	{
		const std::vector<BlueprintUndoRecord> &stack = GetBlueprintUndoStack();
		this->vscroll->SetCount(stack.size());

		/* The stack changes underneath this window as actions are taken and
		 * records expire, and a stale index would select the wrong row. */
		std::erase_if(this->selected, [&](size_t index) { return index >= stack.size(); });
		this->SetWidgetDisabledState(WID_BUL_UNDO_SELECTED, this->selected.empty());
		this->DrawWidgets();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_BUL_LIST) return;
		fill.height = resize.height = GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.matrix.Vertical();
		size.height = 6 * resize.height;
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_BUL_LIST) return;

		const std::vector<BlueprintUndoRecord> &stack = GetBlueprintUndoStack();
		BlueprintUndoFilters filters = GetBlueprintUndoFilters();
		Rect ir = r.WithHeight(this->resize.step_height).Shrink(WidgetDimensions::scaled.matrix);

		size_t first = this->vscroll->GetPosition();
		size_t last = std::min(first + this->vscroll->GetCapacity(), stack.size());
		for (size_t row = first; row < last; row++) {
			size_t index = this->RowToIndex(row);
			bool chosen = this->selected.contains(index);
			if (chosen) GfxFillRect(ir.left - 2, ir.top - 1, ir.right + 2, ir.bottom + 1, PC_DARK_BLUE);
			std::string text = DescribeBlueprintUndoRecord(stack[index].built, filters);
			TextColour colour = chosen ? TextColour::White : TextColour::Black;
			if (text.empty()) {
				/* The filters exclude everything this entry holds, so undoing it would do
				 * nothing. Say what it holds rather than leaving a blank row, greyed to
				 * show it is out of reach until a filter is turned back on. */
				text = DescribeBlueprintUndoRecord(stack[index].built, BlueprintUndoFilters{});
				colour = chosen ? TextColour::Silver : TextColour::Grey;
			}
			DrawString(ir.left, ir.right, ir.top, text, colour);
			ir = ir.Translate(0, this->resize.step_height);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_BUL_LIST: {
				int32_t row = this->vscroll->GetScrolledRowFromWidget(pt.y, this, WID_BUL_LIST);
				if (row == INT32_MAX) return;
				size_t index = this->RowToIndex(static_cast<size_t>(row));

				if (_shift_pressed && !this->selected.empty()) {
					/* Extend from wherever the last plain or Ctrl click landed. */
					size_t from = std::min(this->anchor, index);
					size_t to = std::max(this->anchor, index);
					for (size_t i = from; i <= to; i++) this->selected.insert(i);
				} else if (_ctrl_pressed) {
					if (!this->selected.erase(index)) this->selected.insert(index);
					this->anchor = index;
				} else {
					this->selected.clear();
					this->selected.insert(index);
					this->anchor = index;
				}
				this->RefreshHighlight();
				this->SetDirty();
				return;
			}

			case WID_BUL_UNDO_SELECTED: {
				/* Copy first: each undo consumes its record, which renumbers the
				 * stack under us. Newest first, so that later work comes off
				 * before what it was built on. */
				const std::vector<BlueprintUndoRecord> &stack = GetBlueprintUndoStack();
				std::vector<BlueprintUndoRecord> chosen;
				for (auto it = this->selected.rbegin(); it != this->selected.rend(); ++it) {
					if (*it < stack.size()) chosen.push_back(stack[*it]);
				}

				this->selected.clear();
				this->RefreshHighlight();
				bool any = false;
				for (const BlueprintUndoRecord &record : chosen) any |= PostBlueprintUndo(record);
				if (!any) ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_ERROR_NOTHING_TO_UNDO), {}, WarningLevel::Info);
				this->SetDirty();
				return;
			}

			default: return;
		}
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		/* The filters decide both what an entry says and which tiles it marks, so a
		 * change to them has to redraw the map highlight as well as the list. */
		this->RefreshHighlight();
		this->SetDirty();
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_BUL_LIST);
	}
};

/** Show the undo history. */
void ShowBlueprintUndoList()
{
	if (!Company::IsValidID(_local_company)) return;
	AllocateWindowDescFront<BlueprintUndoListWindow>(_blueprint_undo_list_desc, 0);
}

/**
 * Show the undo toolbar.
 * @return The toolbar window if it was (already) open, else \c nullptr.
 */
Window *ShowBlueprintUndoToolbar()
{
	if (!Company::IsValidID(_local_company)) return nullptr;
	return AllocateWindowDescFront<BlueprintUndoToolbarWindow>(_blueprint_undo_toolbar_desc, 0);
}

/**
 * Command callback for a paste: play the usual construction sound, and refresh
 * the toolbar so that the undo button lights up.
 *
 * The paste is what creates the undo record, but only #Window::OnPaint reads
 * the stack back, so without this the button would sit greyed out until some
 * unrelated event happened to repaint the toolbar.
 *
 * @param result Result of the paste command.
 * @param tile Origin of the paste.
 */
extern void CcPlaySound_CONSTRUCTION_OTHER(const CommandCost &result, TileIndex tile);

void CcPasteBlueprint(const CommandCost &result, TileIndex tile)
{
	CcPlaySound_CONSTRUCTION_OTHER(result, tile);
	InvalidateBlueprintUndoToolbars();
}

/**
 * Command callback for undoing a paste: drop the record now that the undo has
 * really run, and play the usual construction sound.
 *
 * Dropping it here rather than when the command is posted means an undo that
 * failed — for want of money, say — leaves the paste undoable, and it keeps a
 * network game's round trip from losing the record on a command the server
 * rejected.
 *
 * @param result Result of the undo command.
 * @param tile Origin of the paste that was undone.
 */
void CcUndoBlueprintPaste(const CommandCost &result, TileIndex tile)
{
	if (result.Failed()) return;

	ConsumeBlueprintUndoRecord(tile, GetBlueprintUndoFilters());
	CcPlaySound_CONSTRUCTION_OTHER(result, tile);
	InvalidateBlueprintUndoToolbars();
}

/**
 * Show the blueprint toolbar.
 * @return The toolbar window if it was (already) open, else \c nullptr.
 */
Window *ShowBlueprintToolbar()
{
	if (!Company::IsValidID(_local_company)) return nullptr;
	LoadBlueprintAutosave();
	LoadBlueprintStateFromSettings();
	return AllocateWindowDescFront<BlueprintToolbarWindow>(_blueprint_toolbar_desc, 0);
}

/**
 * Get the paste preview overlays for the viewport.
 * @param origin Northern tile of the hovered target area.
 * @return The preview, or \c nullptr when the paste tool is not active.
 */
/**
 * Where a paste would land for a given cursor position.
 *
 * The blueprint straddles the cursor rather than hanging south-east of it. With
 * the origin under the cursor, placing anything longer than a few tiles means
 * aiming at a corner while judging a far end you are not pointing at; centred,
 * the cursor marks the middle of what is about to be built.
 *
 * The viewport and the paste command both go through here, so the ghost, the
 * cost estimate and what actually gets built cannot disagree about the origin.
 *
 * @param cursor Tile under the cursor.
 * @return Northern tile of the paste, or #INVALID_TILE when no paste is armed.
 */
TileIndex GetBlueprintPasteOrigin(TileIndex cursor)
{
	Window *w = FindWindowById(WindowClass::BlueprintToolbar, 0);
	if (w == nullptr || !w->IsWidgetLowered(WID_BT_PASTE)) return INVALID_TILE;

	const Blueprint &blueprint = ActiveBlueprint();
	if (blueprint.IsEmpty()) return INVALID_TILE;

	bool swap_axes = (_blueprint_state.options.rotation % 2) != 0;
	uint16_t width = swap_axes ? blueprint.height : blueprint.width;
	uint16_t height = swap_axes ? blueprint.width : blueprint.height;

	/* Clamped to the map rather than allowed to run off its northern edge: an
	 * origin outside the map is not a tile the preview or the command can use. */
	uint x = static_cast<uint>(std::max(0, static_cast<int>(TileX(cursor)) - width / 2));
	uint y = static_cast<uint>(std::max(0, static_cast<int>(TileY(cursor)) - height / 2));
	return TileXY(std::min(x, Map::MaxX()), std::min(y, Map::MaxY()));
}

const BlueprintPastePreview *GetBlueprintPastePreview(TileIndex origin)
{
	Window *w = FindWindowById(WindowClass::BlueprintToolbar, 0);
	if (w == nullptr || !w->IsWidgetLowered(WID_BT_PASTE)) return nullptr;
	if (ActiveBlueprint().IsEmpty()) return nullptr;
	if (_paste_preview_dirty) RebuildPastePreview();
	if (_paste_preview_origin != origin && origin < Map::Size()) RecomputePasteBlocked(origin);
	return &_paste_preview;
}
