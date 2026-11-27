/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file airport_gui.cpp The GUI for airports. */

#include "stdafx.h"
#include "economy_func.h"
#include "error.h"
#include "widget_type.h"
#include "window_gui.h"
#include "station_gui.h"
#include "terraform_gui.h"
#include "sound_func.h"
#include "window_func.h"
#include "strings_func.h"
#include "table/strings.h"
#include "viewport_func.h"
#include "company_func.h"
#include "command_func.h"
#include "sprite.h"
#include "gfx_func.h"
#include "tilehighlight_func.h"
#include "company_base.h"
#include "station_type.h"
#include "newgrf_airport.h"
#include "newgrf_config.h"
#include "newgrf_airporttiles.h"
#include "newgrf_callbacks.h"
#include "dropdown_type.h"
#include "dropdown_func.h"
#include "picker_gui.h"
#include "core/backup_type.hpp"
#include "core/geometry_func.hpp"
#include "hotkeys.h"
#include "vehicle_func.h"
#include "gui.h"
#include "command_func.h"
#include "airport_cmd.h"
#include "station_cmd.h"
#include "air_type.h"
#include "air.h"
#include "air_map.h"
#include "station_map.h"
#include "engine_base.h"
#include "window_type.h"
#include "zoom_func.h"
#include "timer/timer.h"
#include "timer/timer_game_calendar.h"

#include "widgets/airport_widget.h"

#include "safeguards.h"

static AirType _cur_airtype;                     ///< Air type of the current build-air toolbar.
static AirportTileType _airport_tile_type;       ///< Current airport tile type (hangar, infrastructure...
static DiagDirection _rotation_dir;              ///< Exit direction for new hangars, or rotation for heliports and infrastructure.
static bool _remove_button_clicked;              ///< Flag whether 'remove' toggle-button is currently enabled
static AirportClassID _selected_airport_class;   ///< the currently visible airport class
static int _selected_airport_index;              ///< the index of the selected airport in the current class or -1
static uint8_t _selected_airport_layout;         ///< selected airport layout number.
static DiagDirection _selected_rotation;         ///< selected rotation for airport.
static uint8_t _selected_track_gfx_index;        ///< selected track gfx index.

/** The exact variant (or built-in graphic) chosen in the infrastructure picker. */
struct AirportInfraSelection {
	AirportTiles builtin;
	DiagDirection direction;
	std::optional<AirportTileGraphics> graphics;
};

static AirportInfraSelection GetAirportInfraSelection(AirportTileType type);
static constexpr int CLOSE_INFRA_PICKER_ON_TOOL_ABORT = 1;

static void ShowBuildAirportPicker(Window *parent);
static void ShowHangarPicker(Window *parent);
static void ShowHeliportPicker(Window *parent);
static void ShowAirportInfraNoCatchPicker(Window *parent);
static void ShowAirportInfraWithCatchPicker(Window *parent);
static void ShowTrackGfxPicker(Window *parent);
Window *ShowBuildAirToolbar(AirType airtype);

SpriteID GetCustomAirportSprite(const AirportSpec *as, uint8_t layout);

/**
 * Place an airport.
 * @param tile Position to put the new airport.
 */
void CcBuildAirport(const CommandCost &result, TileIndex tile)
{
	if (result.Failed()) return;

	if (_settings_client.sound.confirm) SndPlayTileFx(SND_1F_CONSTRUCTION_OTHER, tile);
	if (!_settings_client.gui.persistent_buildingtools) ResetObjectToPlace();
}

static void PlaceAirport(TileIndex tile)
{
	if (_selected_airport_index == -1) return;

	const AirportSpec *as = AirportClass::Get(_selected_airport_class)->GetSpec(_selected_airport_index);
	if (as == nullptr || as->layouts.empty()) return;
	uint8_t airport_type = as->GetIndex();
	uint8_t layout = _selected_airport_layout;
	bool adjacent = _ctrl_pressed;

	auto proc = [=](bool test, StationID to_join) -> bool {
		if (test) {
			return adjacent || Command<Commands::BuildAirport>::Do(CommandFlagsToDCFlags(GetCommandFlags<Commands::BuildAirport>()), tile, airport_type, layout, _cur_airtype, _selected_rotation, StationID::Invalid(), adjacent).Succeeded();
		} else {
			return Command<Commands::BuildAirport>::Post(STR_ERROR_CAN_T_BUILD_AIRPORT_HERE, CommandCallback::BuildAirport, tile, airport_type, layout, _cur_airtype, _selected_rotation, to_join, adjacent);
		}
	};

	ShowSelectStationIfNeeded(TileArea(tile, _thd.size.x / TILE_SIZE, _thd.size.y / TILE_SIZE), proc);
}

/**
 * Get the other tile of a runway.
 * @param tile The tile.
 * @return the other extreme of the runway if the tile checked is the start or end of a runway
 *         or the same tile otherwise.
 */
 TileIndex GetOtherEndOfRunway(TileIndex tile)
{
	if (IsValidTile(tile) && IsAirportTile(tile) && IsRunwayExtreme(tile)) {
		AirportTileType att = GetAirportTileType(tile);
		DiagDirection dir = GetRunwayExtremeDirection(tile);
		if (att == ATT_RUNWAY_END) dir = ReverseDiagDir(dir);
		return GetRunwayExtreme(tile, dir);
	}
	return tile;
}

/** Airport build toolbar window handler. */
struct BuildAirToolbarWindow : Window {
	const bool allow_by_tile;
	int last_user_action; // Last started user action.

	struct InfraPlacement {
		AirportInfraSelection selection;
		TileIndex start;
		AirportTileType type;
		bool removing;
		bool dragged = false; ///< The cursor left the first tile at any point during sizing.

		bool IsNewGRFPlacement() const { return !this->removing && this->selection.graphics.has_value(); }
	};
	std::optional<InfraPlacement> infra_placement;

	BuildAirToolbarWindow(bool allow_by_tile, WindowDesc &desc, AirType airtype) : Window(desc), allow_by_tile(allow_by_tile)
	{
		this->CreateNestedTree();
		this->SetupAirToolbar(airtype);
		this->FinishInitNested(TransportType::Air);

		this->DisableWidget(WID_AT_REMOVE);
		this->last_user_action = INVALID_WID_AT;

		this->OnInvalidateData();
		if (_settings_client.gui.link_terraform_toolbar) ShowTerraformToolbar(this);
		_show_airport_tracks = true;
		MarkWholeScreenDirty();
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		_show_airport_tracks = false;
		MarkWholeScreenDirty();

		if (this->IsWidgetLowered(WID_AT_AIRPORT)) SetViewportCatchmentStation(nullptr, true);
		if (_settings_client.gui.link_terraform_toolbar) CloseWindowById(WindowClass::LandInfo, 0, false);
		this->Window::Close();
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;

		bool can_build = CanBuildVehicleInfrastructure(VehicleType::Aircraft);
		this->SetWidgetDisabledState(WID_AT_AIRPORT, !can_build);
		if (!can_build) {
			CloseWindowById(WindowClass::BuildStation, TransportType::Air);

			/* Show in the tooltip why this button is disabled. */
			this->GetWidget<NWidgetCore>(WID_AT_AIRPORT)->SetToolTip(STR_TOOLBAR_DISABLED_NO_VEHICLE_AVAILABLE);
		} else {
			this->GetWidget<NWidgetCore>(WID_AT_AIRPORT)->SetToolTip(STR_TOOLBAR_AIRPORT_BUILD_AIRPORT_TOOLTIP);
		}
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget == WID_AT_CAPTION) {
			if (_settings_game.station.allow_modify_airports) {
				return GetString(GetAirTypeInfo(_cur_airtype)->strings.toolbar_caption);
			}
			return GetString(STR_TOOLBAR_AIRPORT_CAPTION);
		}
		return Window::GetWidgetString(widget, stringid);
	}

	/**
	* Configures the air toolbar for airtype given
	* @param airtype the airtype to display
	*/
	void SetupAirToolbar(AirType airtype)
	{
		if (!this->allow_by_tile) return;
		assert(airtype < AIRTYPE_END);

		_cur_airtype = airtype;
		const AirTypeInfo *ati = GetAirTypeInfo(airtype);
		SetWidgetDisabledState(WID_AT_TOGGLE_GROUND, ati->build_on_water);
		SetWidgetDisabledState(WID_AT_CHANGE_GRAPHICS, ati->build_on_water);

		this->GetWidget<NWidgetCore>(WID_AT_BUILD_TILE)->SetSprite(ati->gui_sprites.add_airport_tiles);
		this->GetWidget<NWidgetCore>(WID_AT_TRACKS)->SetSprite(ati->gui_sprites.build_track_tile);
		this->GetWidget<NWidgetCore>(WID_AT_CONVERT)->SetSprite(ati->gui_sprites.change_airtype);
		this->GetWidget<NWidgetCore>(WID_AT_INFRASTRUCTURE_CATCH)->SetSprite(ati->gui_sprites.build_catchment_infra);
		this->GetWidget<NWidgetCore>(WID_AT_INFRASTRUCTURE_NO_CATCH)->SetSprite(ati->gui_sprites.build_noncatchment_infra);
		this->GetWidget<NWidgetCore>(WID_AT_RUNWAY_LANDING)->SetSprite(ati->gui_sprites.define_landing_runway);
		this->GetWidget<NWidgetCore>(WID_AT_RUNWAY_NO_LANDING)->SetSprite(ati->gui_sprites.define_nonlanding_runway);
		this->GetWidget<NWidgetCore>(WID_AT_APRON)->SetSprite(ati->gui_sprites.build_apron);
		this->GetWidget<NWidgetCore>(WID_AT_HELIPAD)->SetSprite(ati->gui_sprites.build_helipad);
		this->GetWidget<NWidgetCore>(WID_AT_HELIPORT)->SetSprite(ati->gui_sprites.build_heliport);
		if (this->GetWidget<NWidgetCore>(WID_AT_HANGAR_STANDARD) != nullptr) this->GetWidget<NWidgetCore>(WID_AT_HANGAR_STANDARD)->SetSprite(ati->gui_sprites.build_hangar);
		if (this->GetWidget<NWidgetCore>(WID_AT_HANGAR_EXTENDED) != nullptr) this->GetWidget<NWidgetCore>(WID_AT_HANGAR_EXTENDED)->SetSprite(ati->gui_sprites.build_hangar);

		if (!AreHeliportsAvailable(airtype)) DisableWidget(WID_AT_HELIPORT);
	}

	/**
	* Switch to another air type.
	* @param airtype New air type.
	*/
	void ModifyAirType(AirType airtype)
	{
		this->SetupAirToolbar(airtype);
		this->ReInit();
	}

	/**
	* The "remove"-button click proc of the build-air toolbar.
	* @see BuildAirToolbarWindow::OnClick()
	*/
	void BuildAirClick_Remove()
	{
		if (this->IsWidgetDisabled(WID_AT_REMOVE)) return;
		CloseWindowById(WindowClass::JoinStation, 0);
		this->ToggleWidgetLoweredState(WID_AT_REMOVE);
		this->SetWidgetDirty(WID_AT_REMOVE);
		_remove_button_clicked = this->IsWidgetLowered(WID_AT_REMOVE);

		if (this->last_user_action == WID_AT_RUNWAY_LANDING ||
				this->last_user_action == WID_AT_RUNWAY_NO_LANDING) {
			SetObjectToPlace(GetAirTypeInfo(_cur_airtype)->cursor.build_hangar, PAL_NONE, _remove_button_clicked ? HT_SPECIAL : HT_RECT, this->window_class, this->window_number);
			this->LowerWidget(this->last_user_action);
			this->SetWidgetLoweredState(WID_AT_REMOVE, _remove_button_clicked);
		}

		SetSelectionRed(_remove_button_clicked);
		if (_settings_client.sound.click_beep) SndPlayFx(SND_15_BEEP);
	}

	void UpdateWidgetSize(int widget, Dimension &size, const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (!IsInsideMM(widget, WID_AT_BUILD_TILE, WID_AT_REMOVE)) return;
		NWidgetLeaf *wid = this->GetWidget<NWidgetLeaf>(widget);

		Dimension d = GetSpriteSize(wid->GetWidgetData().sprite);
		d.width += padding.width;
		d.height += padding.height;
		size = d;
	}

	void UpdateRemoveWidgetStatus(int clicked_widget)
	{
		if (!this->allow_by_tile) return;

		assert(clicked_widget != WID_AT_REMOVE);

		if (clicked_widget >= WID_AT_REMOVE_FIRST && clicked_widget <= WID_AT_REMOVE_LAST) {
			bool is_button_lowered = this->IsWidgetLowered(clicked_widget);
			_remove_button_clicked &= is_button_lowered;
			this->SetWidgetDisabledState(WID_AT_REMOVE, !is_button_lowered);
			this->SetWidgetLoweredState(WID_AT_REMOVE, _remove_button_clicked);
			SetSelectionRed(_remove_button_clicked);
		} else {
			/* When any other buttons that do not accept "removal",
			 * raise and disable the removal button. */
			this->DisableWidget(WID_AT_REMOVE);
			this->RaiseWidget(WID_AT_REMOVE);
			_remove_button_clicked = false;
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_AT_BUILD_TILE:
				HandlePlacePushButton(this, widget, GetAirTypeInfo(_cur_airtype)->cursor.add_airport_tiles, HT_RECT);
				this->last_user_action = widget;
				break;

			case WID_AT_TRACKS:
				_airport_tile_type = ATT_SIMPLE_TRACK;
				HandlePlacePushButton(this, widget, GetAirTypeInfo(_cur_airtype)->cursor.build_track_tile, HT_RAIL);
				this->last_user_action = widget;
				break;

			case WID_AT_REMOVE:
				this->BuildAirClick_Remove();
				return;

			case WID_AT_AIRPORT:
				if (HandlePlacePushButton(this, WID_AT_AIRPORT, SPR_CURSOR_AIRPORT, HT_RECT)) {
					ShowBuildAirportPicker(this);
					this->last_user_action = widget;
				}
				break;

			case WID_AT_DEMOLISH:
				HandlePlacePushButton(this, WID_AT_DEMOLISH, ANIMCURSOR_DEMOLISH, HT_RECT | HT_DIAGONAL);
				this->last_user_action = widget;
				break;

			case WID_AT_CONVERT:
				HandlePlacePushButton(this, widget, GetAirTypeInfo(_cur_airtype)->cursor.change_airtype, HT_RECT);
				this->last_user_action = widget;
				break;

			case WID_AT_INFRASTRUCTURE_CATCH:
				_airport_tile_type = ATT_INFRASTRUCTURE_WITH_CATCH;
				if (HandlePlacePushButton(this, widget, GetAirTypeInfo(_cur_airtype)->cursor.build_catchment_infra, HT_RECT | HT_DIAGONAL)) {
					ShowAirportInfraWithCatchPicker(this);
				}
				this->last_user_action = widget;
				break;

			case WID_AT_INFRASTRUCTURE_NO_CATCH:
				_airport_tile_type = ATT_INFRASTRUCTURE_NO_CATCH;
				if (HandlePlacePushButton(this, widget, GetAirTypeInfo(_cur_airtype)->cursor.build_noncatchment_infra, HT_RECT | HT_DIAGONAL)) {
					ShowAirportInfraNoCatchPicker(this);
				}
				this->last_user_action = widget;
				break;

			case WID_AT_APRON:
				_airport_tile_type = ATT_APRON_NORMAL;
				HandlePlacePushButton(this, widget, GetAirTypeInfo(_cur_airtype)->cursor.build_apron, HT_RECT | HT_DIAGONAL);
				this->last_user_action = widget;
				break;

			case WID_AT_HELIPAD:
				_airport_tile_type = ATT_APRON_HELIPAD;
				HandlePlacePushButton(this, widget, GetAirTypeInfo(_cur_airtype)->cursor.build_helipad, HT_RECT | HT_DIAGONAL);
				this->last_user_action = widget;
				break;

			case WID_AT_HELIPORT:
				_airport_tile_type = ATT_APRON_HELIPORT;
				if (HandlePlacePushButton(this, widget, GetAirTypeInfo(_cur_airtype)->cursor.build_heliport, HT_RECT | HT_DIAGONAL)) {
					ShowHeliportPicker(this);
				}
				this->last_user_action = widget;
				break;

			case WID_AT_HANGAR_STANDARD:
			case WID_AT_HANGAR_EXTENDED:
				_airport_tile_type = widget == WID_AT_HANGAR_STANDARD ? ATT_HANGAR_STANDARD : ATT_HANGAR_EXTENDED;
				if (HandlePlacePushButton(this, widget, GetAirTypeInfo(_cur_airtype)->cursor.build_hangar, HT_RECT)) {
					ShowHangarPicker(this);
				}
				this->last_user_action = widget;
				break;

			case WID_AT_RUNWAY_LANDING:
				_airport_tile_type = ATT_RUNWAY_START_ALLOW_LANDING;
				HandlePlacePushButton(this, widget, GetAirTypeInfo(_cur_airtype)->cursor.define_landing_runway, _remove_button_clicked ? HT_SPECIAL : HT_RECT);
				this->last_user_action = widget;
				break;

			case WID_AT_RUNWAY_NO_LANDING:
				_airport_tile_type = ATT_RUNWAY_START_NO_LANDING;
				HandlePlacePushButton(this, widget, GetAirTypeInfo(_cur_airtype)->cursor.define_nonlanding_runway, _remove_button_clicked ? HT_SPECIAL : HT_RECT);
				this->last_user_action = widget;
				break;

			case WID_AT_CHANGE_GRAPHICS:
				if (HandlePlacePushButton(this, widget, SPR_CURSOR_MOUSE, HT_RECT)) {
					ShowTrackGfxPicker(this);
				}
				this->last_user_action = widget;
				break;

			case WID_AT_TOGGLE_GROUND:
				HandlePlacePushButton(this, widget, SPR_CURSOR_MOUSE, HT_RECT | HT_DIAGONAL);
				this->last_user_action = widget;
				break;

			default: break;
		}

		UpdateRemoveWidgetStatus(widget);
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		switch (this->last_user_action) {
			case WID_AT_BUILD_TILE:
				VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_BUILD_STATION);
				break;

			case WID_AT_TRACKS:
				VpStartPlaceSizing(tile, VPM_RAILDIRS, DDSP_PLACE_RAIL);
				break;

			case WID_AT_AIRPORT:
				PlaceAirport(tile);
				break;

			case WID_AT_DEMOLISH:
				PlaceProc_DemolishArea(tile);
				break;

			case WID_AT_CONVERT:
				Command<Commands::ChangeAirType>::Post(STR_ERROR_CAN_T_DO_THIS, tile, _cur_airtype);
				break;

			case WID_AT_HANGAR_STANDARD:
			case WID_AT_HANGAR_EXTENDED:
				VpStartPlaceSizing(tile, HasBit(to_underlying(_rotation_dir), 0) ? VPM_FIX_Y : VPM_FIX_X, DDSP_BUILD_STATION);
				break;

			case WID_AT_INFRASTRUCTURE_CATCH:
			case WID_AT_INFRASTRUCTURE_NO_CATCH: {
				const auto type = this->last_user_action == WID_AT_INFRASTRUCTURE_CATCH ? ATT_INFRASTRUCTURE_WITH_CATCH : ATT_INFRASTRUCTURE_NO_CATCH;
				this->infra_placement = InfraPlacement{GetAirportInfraSelection(type), tile, type, _remove_button_clicked};
				VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_BUILD_STATION);
				break;
			}

			case WID_AT_APRON:
			case WID_AT_HELIPAD:
			case WID_AT_HELIPORT:
				VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_BUILD_STATION);
				break;

			case WID_AT_RUNWAY_LANDING:
			case WID_AT_RUNWAY_NO_LANDING:
				if (_remove_button_clicked) {
					Command<Commands::ChangeAirport>::Post(STR_ERROR_CAN_T_DO_THIS, tile, GetOtherEndOfRunway(tile), _cur_airtype, _airport_tile_type, ATTG_DEFAULT_GFX, (DiagDirection)0, !_remove_button_clicked, false);
				} else {
					VpStartPlaceSizing(tile, VPM_X_OR_Y, DDSP_BUILD_STATION);
				}
				break;

			case WID_AT_CHANGE_GRAPHICS:
				VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_BUILD_STATION);
				break;


			case WID_AT_TOGGLE_GROUND:
				VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_BUILD_STATION);
				break;

			default: NOT_REACHED();
		}
	}

	void OnPlaceDrag(ViewportPlaceMethod select_method, [[maybe_unused]] ViewportDragDropSelectionProcess select_proc, Point pt) override
	{
		if (this->infra_placement.has_value() && this->infra_placement->IsNewGRFPlacement()) {
			/* Keep the highlight to one tile, but remember a drag even if it returns to the start. */
			if (pt.x == -1 || TileVirtXY(pt.x, pt.y) != this->infra_placement->start) this->infra_placement->dragged = true;
			return;
		}
		VpSelectTilesWithMethod(pt.x, pt.y, select_method);
	}

	void OnPlaceMouseUp([[maybe_unused]] ViewportPlaceMethod select_method, ViewportDragDropSelectionProcess select_proc, Point pt, TileIndex start_tile, TileIndex end_tile) override
	{
		const auto infra = std::exchange(this->infra_placement, std::nullopt);
		if (pt.x == -1) return;

		switch (this->last_user_action) {
			case WID_AT_BUILD_TILE: {
				assert(_settings_game.station.allow_modify_airports);

				auto proc = [=](bool test, StationID to_join) -> bool {
					if (test) {
						return (_ctrl_pressed && !_remove_button_clicked) || Command<Commands::AddRemoveAirportTiles>::Do(CommandFlagsToDCFlags(GetCommandFlags<Commands::AddRemoveAirportTiles>()), start_tile, end_tile, !_remove_button_clicked, _cur_airtype, StationID::Invalid(), _ctrl_pressed).Succeeded();
					} else {
						return Command<Commands::AddRemoveAirportTiles>::Post(STR_ERROR_CAN_T_DO_THIS, start_tile, end_tile, !_remove_button_clicked, _cur_airtype, to_join, _ctrl_pressed);
					}
				};

				ShowSelectStationIfNeeded(TileArea(start_tile, end_tile), proc);
				break;
			}
			case WID_AT_TRACKS:
				assert(_settings_game.station.allow_modify_airports);
				Command<Commands::AddRemoveTracksAirport>::Post(STR_ERROR_CAN_T_DO_THIS, start_tile, end_tile, _cur_airtype, !_remove_button_clicked, (Track)(_thd.drawstyle & HT_DIR_MASK));
				break;
			case WID_AT_AIRPORT:
				assert(start_tile == end_tile);
				PlaceAirport(end_tile);
				break;
			case WID_AT_DEMOLISH:
				GUIPlaceProcDragXY(select_proc, start_tile, end_tile);
				break;
			case WID_AT_CONVERT:
				NOT_REACHED();
			case WID_AT_INFRASTRUCTURE_CATCH:
			case WID_AT_INFRASTRUCTURE_NO_CATCH: {
				if (!infra.has_value()) break;
				const auto &selection = infra->selection;
				if (infra->IsNewGRFPlacement()) {
					const Point cursor = GetTileBelowCursor();
					if (cursor.x == -1) break;
					if (infra->dragged || TileVirtXY(cursor.x, cursor.y) != infra->start) {
						ShowErrorMessage(GetEncodedString(STR_ERROR_CAN_T_DO_THIS), GetEncodedString(STR_ERROR_AIRPORT_NEWGRF_SINGLE_TILE), WarningLevel::Error);
						break;
					}
					const auto &graphics = *selection.graphics;
					Command<Commands::SetAirportTileGfx>::Post(STR_ERROR_CAN_T_DO_THIS, infra->start, infra->type,
							graphics.source_grfid, graphics.source_airport, graphics.source_layout, graphics.source_pos, graphics.source_rotation);
					break;
				}

				const AirportTiles gfx = infra->removing ? (infra->type == ATT_INFRASTRUCTURE_WITH_CATCH ? ATTG_DEFAULT_GFX : ATTG_NO_CATCH_FLAG) : selection.builtin;
				Command<Commands::ChangeAirport>::Post(STR_ERROR_CAN_T_DO_THIS, start_tile, end_tile, _cur_airtype, infra->type, gfx, selection.direction, !infra->removing, _ctrl_pressed);
				break;
			}

			case WID_AT_APRON:
			case WID_AT_HELIPAD:
			case WID_AT_HELIPORT:
			case WID_AT_HANGAR_STANDARD:
			case WID_AT_HANGAR_EXTENDED:
			case WID_AT_RUNWAY_LANDING:
			case WID_AT_RUNWAY_NO_LANDING: {
				bool diagonal = _ctrl_pressed &&
						(this->last_user_action != WID_AT_HANGAR_EXTENDED && this->last_user_action != WID_AT_HANGAR_STANDARD);
				bool ret = Command<Commands::ChangeAirport>::Post(STR_ERROR_CAN_T_DO_THIS, start_tile, end_tile, _cur_airtype, _airport_tile_type, ATTG_DEFAULT_GFX, _rotation_dir, !_remove_button_clicked, diagonal);
				if (ret && _remove_button_clicked &&
						(this->last_user_action == WID_AT_RUNWAY_LANDING || this->last_user_action == WID_AT_RUNWAY_NO_LANDING)) {
					VpStartPlaceSizing(start_tile, VPM_X_OR_Y, DDSP_BUILD_STATION);
				}
				break;
			}

			case WID_AT_CHANGE_GRAPHICS:
				Command<Commands::AirportChangeTrackGfx>::Post(STR_ERROR_CAN_T_DO_THIS, start_tile, end_tile, _cur_airtype, _selected_track_gfx_index, _ctrl_pressed);
				break;

			case WID_AT_TOGGLE_GROUND:
				Command<Commands::AirportToggleGround>::Post(STR_ERROR_CAN_T_DO_THIS, start_tile, end_tile, _cur_airtype, _ctrl_pressed);
				break;

			default: NOT_REACHED();
		}
	}

	void OnPlacePresize([[maybe_unused]] Point pt, TileIndex tile) override
	{
		assert(this->last_user_action == WID_AT_RUNWAY_LANDING ||
				this->last_user_action == WID_AT_RUNWAY_NO_LANDING);
		assert(_remove_button_clicked);
		VpSetPresizeRange(tile, GetOtherEndOfRunway(tile));
	}

	void OnPlaceObjectAbort() override
	{
		this->infra_placement.reset();
		if (this->IsWidgetLowered(WID_AT_AIRPORT)) SetViewportCatchmentStation(nullptr, true);

		this->RaiseButtons();

		if (Window *picker = this->FindChildWindow(WindowClass::BuildAirportInfrastructure); picker != nullptr) {
			picker->Close(CLOSE_INFRA_PICKER_ON_TOOL_ABORT);
		}
		CloseWindowById(WindowClass::BuildStation, TransportType::Air);
		CloseWindowById(WindowClass::JoinStation, 0);
	}

	/**
	 * Handler for global hotkeys of the BuildAirToolbarWindow.
	 * @param hotkey Hotkey
	 * @return EventState::Handled if hotkey was accepted.
	 */
	static EventState AirportToolbarGlobalHotkeys(int hotkey)
	{
		if (_game_mode != GameMode::Normal  || !CanBuildVehicleInfrastructure(VehicleType::Aircraft)) return EventState::NotHandled;
		extern AirType _last_built_airtype;
		Window *w = ShowBuildAirToolbar(_last_built_airtype);
		if (w == nullptr) return EventState::NotHandled;
		return w->OnHotkey(hotkey);
	}

	static inline HotkeyList hotkeys{"airtoolbar", {
		Hotkey('1', "airport", WID_AT_AIRPORT),
		Hotkey('2', "demolish", WID_AT_DEMOLISH),
		Hotkey('3', "remove", WID_AT_REMOVE),
		Hotkey('4', "tiles", WID_AT_BUILD_TILE),
		Hotkey('5', "tracks", WID_AT_TRACKS),
		Hotkey('6', "infra_with_catch", WID_AT_INFRASTRUCTURE_CATCH),
		Hotkey('7', "infra_no_catch", WID_AT_INFRASTRUCTURE_NO_CATCH),
	}, AirportToolbarGlobalHotkeys};
};

/**
 * Add the depot icons depending on availability of construction.
 * @return Panel with hangar buttons.
 */
static std::unique_ptr<NWidgetBase> MakeNWidgetHangars()
{
	auto hor = std::make_unique<NWidgetHorizontal>();

	/* Add the widget for building standard hangar. */
	hor->Add(std::make_unique<NWidgetLeaf>(WWT_IMGBTN, Colours::DarkGreen, WID_AT_HANGAR_STANDARD, WidgetData{.sprite = 0}, STR_TOOLBAR_AIRPORT_BUILD_HANGAR_STANDARD));

	return hor;
}

static constexpr NWidgetPart _nested_air_tile_toolbar_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_AT_CAPTION), SetStringTip(STR_JUST_STRING, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS), SetTextStyle(TextColour::White),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(NWID_VERTICAL),
		NWidget(NWID_HORIZONTAL),
			NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_AIRPORT), SetFill(0, 1), SetMinimalSize(22, 22), SetSpriteTip(SPR_IMG_AIRPORT, STR_TOOLBAR_AIRPORT_BUILD_PRE_AIRPORT_TOOLTIP),

			NWidget(WWT_PANEL, Colours::DarkGreen), SetMinimalSize(4, 22), SetFill(1, 1), EndContainer(),

			NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_DEMOLISH), SetFill(0, 1), SetMinimalSize(22, 22), SetSpriteTip(SPR_IMG_DYNAMITE, STR_TOOLTIP_DEMOLISH_BUILDINGS_ETC),
			NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_REMOVE), SetFill(0, 1),
			SetSpriteTip(SPR_IMG_REMOVE, STR_TOOLBAR_AIRPORT_TOGGLE_BUILD_REMOVE),

			NWidget(WWT_PANEL, Colours::DarkGreen), SetMinimalSize(4, 22), SetFill(1, 1), EndContainer(),

			NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_BUILD_TILE), SetFill(0, 1), SetMinimalSize(22, 22), SetSpriteTip(0, STR_TOOLBAR_AIRPORT_ADD_TILES),
			NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_TRACKS), SetFill(0, 1), SetMinimalSize(22, 22), SetSpriteTip(0, STR_TOOLBAR_AIRPORT_SET_TRACKS),

			NWidget(WWT_PANEL, Colours::DarkGreen), SetMinimalSize(4, 22), SetFill(1, 1), EndContainer(),

			NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_INFRASTRUCTURE_CATCH), SetFill(0, 1),
					SetSpriteTip(0, STR_TOOLBAR_AIRPORT_INFRASTRUCTURE_CATCHMENT),
			NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_INFRASTRUCTURE_NO_CATCH), SetFill(0, 1),
					SetSpriteTip(0, STR_TOOLBAR_AIRPORT_INFRASTRUCTURE_NO_CATCHMENT),
		EndContainer(),
		NWidget(NWID_HORIZONTAL),
			NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_RUNWAY_LANDING), SetFill(0, 1),
					SetSpriteTip(0, STR_TOOLBAR_AIRPORT_DEFINE_RUNWAY_LANDING),
			NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_RUNWAY_NO_LANDING), SetFill(0, 1),
					SetSpriteTip(0, STR_TOOLBAR_AIRPORT_DEFINE_RUNWAY_NO_LANDING),
			NWidget(WWT_PANEL, Colours::DarkGreen), SetMinimalSize(4, 22), SetFill(1, 1), EndContainer(),

			NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_APRON), SetFill(0, 1),
					SetSpriteTip(0, STR_TOOLBAR_AIRPORT_BUILD_APRON),
			NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_HELIPAD), SetFill(0, 1),
					SetSpriteTip(0, STR_TOOLBAR_AIRPORT_BUILD_HELIPAD),
			NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_HELIPORT), SetFill(0, 1),
					SetSpriteTip(0, STR_TOOLBAR_AIRPORT_BUILD_HELIPORT),
			NWidget(WWT_PANEL, Colours::DarkGreen), SetMinimalSize(4, 22), SetFill(1, 1), EndContainer(),
			NWidgetFunction(MakeNWidgetHangars),
			NWidget(WWT_PANEL, Colours::DarkGreen), SetMinimalSize(4, 22), SetFill(1, 1), EndContainer(),
			NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_CONVERT), SetFill(0, 1),
					SetSpriteTip(0, STR_TOOLBAR_AIRPORT_CHANGE_AIRTYPE),
			NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_AT_CHANGE_GRAPHICS), SetFill(0, 1),
					SetStringTip(STR_TOOLBAR_AIRPORT_ROTATE_GRAPHICS, STR_TOOLBAR_AIRPORT_ROTATE_GRAPHICS_TOOLTIP),
			NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_AT_TOGGLE_GROUND), SetFill(0, 1),
					SetStringTip(STR_TOOLBAR_AIRPORT_TOGGLE_GROUND, STR_TOOLBAR_AIRPORT_TOGGLE_GROUND_TOOLBAR),
		EndContainer(),
	EndContainer(),
};

static const NWidgetPart _nested_air_nontile_toolbar_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_AT_CAPTION), SetStringTip(STR_JUST_STRING, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_AIRPORT), SetFill(0, 1), SetMinimalSize(42, 22),
				SetSpriteTip(SPR_IMG_AIRPORT, STR_TOOLBAR_AIRPORT_BUILD_PRE_AIRPORT_TOOLTIP),
	NWidget(WWT_PANEL, Colours::DarkGreen), SetMinimalSize(4, 22), SetFill(1, 1), EndContainer(),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_AT_DEMOLISH), SetFill(0, 1), SetMinimalSize(22, 22), SetSpriteTip(SPR_IMG_DYNAMITE, STR_TOOLTIP_DEMOLISH_BUILDINGS_ETC),
	EndContainer(),
};

static WindowDesc _air_tile_toolbar_desc(__FILE__, __LINE__,
	WindowPosition::AlignToolbar, "toolbar_air", 0, 0,
	WindowClass::BuildToolbar, WindowClass::None,
	WindowDefaultFlag::Construction,
	_nested_air_tile_toolbar_widgets,
	&BuildAirToolbarWindow::hotkeys
);

static WindowDesc _air_nontile_toolbar_desc(__FILE__, __LINE__,
	WindowPosition::AlignToolbar, "toolbar_air_nontile", 0, 0,
	WindowClass::BuildToolbar, WindowClass::None,
	WindowDefaultFlag::Construction,
	_nested_air_nontile_toolbar_widgets,
	&BuildAirToolbarWindow::hotkeys
);


/**
 * Open the build airport toolbar window.
 * If the terraform toolbar is linked to the toolbar, that window is also opened.
 * @param airtype air type for constructing (a valid air type or
 *  			INVALID_AIRTYPE if the build-airports-by-tile is dissabled).
 * @return newly opened airport toolbar, or nullptr if the toolbar could not be opened.
 */
Window *ShowBuildAirToolbar(AirType airtype)
{
	if (!Company::IsValidID(_local_company)) return nullptr;

	/* Constructing airports tile by tile is only possible when they may be modified. */
	if (!_settings_game.station.allow_modify_airports) airtype = INVALID_AIRTYPE;

	if (airtype != INVALID_AIRTYPE && !ValParamAirType(airtype)) return nullptr;

	CloseWindowByClass(WindowClass::BuildToolbar);
	assert((airtype == INVALID_AIRTYPE) != (_settings_game.station.allow_modify_airports));

	_cur_airtype = airtype;
	_remove_button_clicked = false;

	if (airtype == INVALID_AIRTYPE) {
		return new BuildAirToolbarWindow(false, _air_nontile_toolbar_desc, airtype);
	} else {
		return new BuildAirToolbarWindow(true, _air_tile_toolbar_desc, airtype);
	}
}

class BuildAirportWindow : public PickerWindowBase {
	SpriteID preview_sprite; ///< Cached airport preview sprite.
	int line_height;
	Scrollbar *vscroll;

	/** Build a dropdown list of available airport classes */
	static DropDownList BuildAirportClassDropDown()
	{
		DropDownList list;

		for (const auto &cls : AirportClass::Classes()) {
			if (cls.Index() == APC_CUSTOM) continue;
			bool unavailable = true;
			for (const auto &as : cls.Specs()) {
				if (!as->IsAvailable(_cur_airtype)) continue;
				unavailable = false;
				break;
			}
			list.push_back(MakeDropDownListStringItem(GetString(cls.name), cls.Index().base(), unavailable));
		}

		return list;
	}

public:
	BuildAirportWindow(WindowDesc &desc, Window *parent) : PickerWindowBase(desc, parent)
	{
		this->CreateNestedTree();

		this->vscroll = this->GetScrollbar(WID_AP_SCROLLBAR);
		this->vscroll->SetCapacity(5);
		this->vscroll->SetPosition(0);

		this->FinishInitNested(TransportType::Air);

		this->SetWidgetLoweredState(WID_AP_BTN_DONTHILIGHT, !_settings_client.gui.station_show_coverage);
		this->SetWidgetLoweredState(WID_AP_BTN_DOHILIGHT, _settings_client.gui.station_show_coverage);
		this->OnInvalidateData();

		/* Ensure airport class is valid (changing NewGRFs). */
		_selected_airport_class = Clamp(_selected_airport_class, APC_BEGIN, (AirportClassID)(AirportClass::GetClassCount() - 1));
		const AirportClass *ac = AirportClass::Get(_selected_airport_class);
		this->vscroll->SetCount(ac->GetSpecCount());

		/* Ensure the airport index is valid for this class (changing NewGRFs). */
		_selected_airport_index = Clamp(_selected_airport_index, -1, ac->GetSpecCount() - 1);

		/* Only when no valid airport was selected, we want to select the first airport. */
		bool selectFirstAirport = true;
		if (_selected_airport_index != -1) {
			const AirportSpec *as = ac->GetSpec(_selected_airport_index);
			if (as != nullptr && as->IsAvailable(_cur_airtype)) {
				/* Ensure the airport layout is valid. */
				_selected_airport_layout = Clamp<uint8_t>(_selected_airport_layout, 0, (uint8_t)as->layouts.size() - 1);
				_selected_rotation = (DiagDirection)Clamp<uint>(static_cast<uint>(_selected_rotation), 0, 3);
				selectFirstAirport = false;
				this->UpdateSelectSize();
			}
		}

		if (selectFirstAirport) this->SelectFirstAvailableAirport(true);
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		CloseWindowById(WindowClass::JoinStation, 0);
		this->PickerWindowBase::Close();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_AP_CLASS_DROPDOWN:
				return GetString(AirportClass::Get(_selected_airport_class)->name);

			case WID_AP_LAYOUT_NUM: {
				const AirportSpec *as = this->GetSelectedAirportSpec();
				if (as != nullptr) {
					StringID string = GetAirportTextCallback(as, _selected_airport_layout, CBID_AIRPORT_LAYOUT_NAME);
					if (string != STR_UNDEFINED) {
						return GetString(string);
					} else if (as->layouts.size() > 1) {
						return GetString(STR_STATION_BUILD_AIRPORT_LAYOUT_NAME, _selected_airport_layout + 1);
					}
				}
				return GetString(STR_EMPTY);
			}

			case WID_AP_ROTATION:
				if (_selected_airport_index != -1) {
					return GetString(STR_AIRPORT_ROTATION_0 + static_cast<uint>(_selected_rotation));
				}
				return GetString(STR_EMPTY);

			default: break;
		}
		return Window::GetWidgetString(widget, stringid);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_AP_CLASS_DROPDOWN: {
				Dimension d = {0, 0};
				for (const auto &cls : AirportClass::Classes()) {
					d = maxdim(d, GetStringBoundingBox(cls.name));
				}
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}

			case WID_AP_AIRPORT_LIST: {
				for (int i = 0; i < NUM_AIRPORTS; i++) {
					const AirportSpec *as = AirportSpec::Get(i);
					if (!as->enabled) continue;

					size.width = std::max(size.width, GetStringBoundingBox(as->name).width + padding.width);
				}

				this->line_height = GetCharacterHeight(FontSize::Normal) + padding.height;
				size.height = 5 * this->line_height;
				break;
			}

			case WID_AP_AIRPORT_SPRITE:
				for (int i = 0; i < NUM_AIRPORTS; i++) {
					const AirportSpec *as = AirportSpec::Get(i);
					if (!as->enabled) continue;
					for (uint8_t layout = 0; layout < static_cast<uint8_t>(as->layouts.size()); layout++) {
						SpriteID sprite = GetCustomAirportSprite(as, layout);
						if (sprite != 0) {
							Dimension d = GetSpriteSize(sprite);
							d.width += WidgetDimensions::scaled.framerect.Horizontal();
							d.height += WidgetDimensions::scaled.framerect.Vertical();
							size = maxdim(d, size);
						}
					}
				}
				break;

			case WID_AP_EXTRA_TEXT:
				for (int i = NEW_AIRPORT_OFFSET; i < NUM_AIRPORTS; i++) {
					const AirportSpec *as = AirportSpec::Get(i);
					if (!as->enabled) continue;
					for (uint8_t layout = 0; layout < static_cast<uint8_t>(as->layouts.size()); layout++) {
						StringID string = GetAirportTextCallback(as, layout, CBID_AIRPORT_ADDITIONAL_TEXT);
						if (string == STR_UNDEFINED) continue;

						Dimension d = GetStringMultiLineBoundingBox(string, size);
						size = maxdim(d, size);
					}
				}
				break;

			default: break;
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_AP_AIRPORT_LIST: {
				Rect row = r.WithHeight(this->line_height).Shrink(WidgetDimensions::scaled.bevel);
				Rect text = r.WithHeight(this->line_height).Shrink(WidgetDimensions::scaled.matrix);
				const auto specs = AirportClass::Get(_selected_airport_class)->Specs();
				auto [first, last] = this->vscroll->GetVisibleRangeIterators(specs);
				for (auto it = first; it != last; ++it) {
					const AirportSpec *as = *it;
					if (!as->IsAvailable(_cur_airtype)) {
						GfxFillRect(row, PC_BLACK, FillRectMode::Checker);
					}
					DrawString(text, as->name, (static_cast<int>(as->index) == _selected_airport_index) ? TextColour::White : TextColour::Black);
					row = row.Translate(0, this->line_height);
					text = text.Translate(0, this->line_height);
				}
				break;
			}

			case WID_AP_AIRPORT_SPRITE:
				if (this->preview_sprite != 0) {
					Dimension d = GetSpriteSize(this->preview_sprite);
					DrawSprite(this->preview_sprite, GetCompanyPalette(_local_company), CentreBounds(r.left, r.right, d.width), CentreBounds(r.top, r.bottom, d.height));
				}
				break;

			case WID_AP_EXTRA_TEXT: {
				const AirportSpec *as = this->GetSelectedAirportSpec();
				if (as != nullptr) {
					StringID string = GetAirportTextCallback(as, _selected_airport_layout, CBID_AIRPORT_ADDITIONAL_TEXT);
					if (string != STR_UNDEFINED) {
						DrawStringMultiLine(r, GetString(string), TextColour::Black);
					} else if (as->layouts.size() > 1) {
						DrawStringMultiLine(r, GetString(STR_STATION_BUILD_AIRPORT_LAYOUT_NAME, _selected_airport_layout + 1), TextColour::Black);
					}
				}
				break;
			}
		}
	}

	void OnPaint() override
	{
		this->DrawWidgets();

		Rect r = this->GetWidget<NWidgetBase>(WID_AP_ACCEPTANCE)->GetCurrentRect();
		int top = r.top;

		const AirportSpec *as = GetSelectedAirportSpec();
		if (as != nullptr) {
			AirType airtype = _settings_game.station.allow_modify_airports ? _cur_airtype : as->airtype;
			const AirTypeInfo *ati = GetAirTypeInfo(airtype);
			int rad = _settings_game.station.modified_catchment ? ati->catchment_radius : (uint)CA_UNMODIFIED;

			/* only show the station (airport) noise, if the noise option is activated */
			if (_settings_game.economy.station_noise_level) {
				/* show the noise of the selected airport */
				DrawString(r.left, r.right, top, GetString(STR_STATION_BUILD_NOISE, as->GetAirportNoise(airtype)));
				top += GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.vsep_normal;
			}

			/* strings such as 'Size' and 'Coverage Area' */
			top = DrawStationCoverageAreaText(Rect{r.left, top, r.right, top}, SCT_ALL, rad, false) + WidgetDimensions::scaled.vsep_normal;
			top = DrawStationCoverageAreaText(Rect{r.left, top, r.right, top}, SCT_ALL, rad, true);
		}

		/* Resize background if the window is too small.
		 * Never make the window smaller to avoid oscillating if the size change affects the acceptance.
		 * (This is the case, if making the window bigger moves the mouse into the window.) */
		if (top > r.bottom) {
			ResizeWindow(this, 0, top - r.bottom, false);
		}
	}

	void SelectOtherAirport(int airport_index)
	{
		_selected_airport_index = airport_index;
		_selected_airport_layout = 0;

		this->UpdateSelectSize();
		this->SetDirty();
	}

	/**
	 * Get the spec of the currently selected airport, or \c nullptr if there is no valid selection.
	 * The index is relative to the selected airport class, which may have been emptied by a change
	 * in the loaded NewGRF files, and an airport without layouts cannot be built at all.
	 */
	const AirportSpec *GetSelectedAirportSpec() const
	{
		if (_selected_airport_index == -1) return nullptr;
		const AirportSpec *as = AirportClass::Get(_selected_airport_class)->GetSpec(_selected_airport_index);
		return (as != nullptr && !as->layouts.empty()) ? as : nullptr;
	}

	void UpdateSelectSize()
	{
		const AirportSpec *as = GetSelectedAirportSpec();
		if (as == nullptr) {
			SetTileSelectSize(1, 1);
			this->DisableWidget(WID_AP_LAYOUT_DECREASE);
			this->DisableWidget(WID_AP_LAYOUT_INCREASE);
			this->DisableWidget(WID_AP_ROTATION_DECREASE);
			this->DisableWidget(WID_AP_ROTATION_INCREASE);
		} else {
			int w = as->layouts[_selected_airport_layout].size_x;
			int h = as->layouts[_selected_airport_layout].size_y;
			if (static_cast<uint>(_selected_rotation) % 2 != 0) std::swap(w, h);
			SetTileSelectSize(w, h);

			this->preview_sprite = GetCustomAirportSprite(as, _selected_airport_layout) + static_cast<uint>(_selected_rotation);

			this->SetWidgetDisabledState(WID_AP_LAYOUT_DECREASE, _selected_airport_layout == 0);
			this->SetWidgetDisabledState(WID_AP_LAYOUT_INCREASE, _selected_airport_layout + 1U >= as->layouts.size());

			const AirTypeInfo *ati = GetAirTypeInfo(as->airtype);
			int rad = _settings_game.station.modified_catchment ? ati->catchment_radius : (uint)CA_UNMODIFIED;
			if (_settings_client.gui.station_show_coverage) SetTileSelectBigSize(-rad, -rad, 2 * rad, 2 * rad);
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_AP_CLASS_DROPDOWN:
				ShowDropDownList(this, BuildAirportClassDropDown(), _selected_airport_class.base(), WID_AP_CLASS_DROPDOWN);
				break;

			case WID_AP_AIRPORT_LIST: {
				int32_t num_clicked = this->vscroll->GetScrolledRowFromWidget(pt.y, this, widget, 0, this->line_height);
				if (num_clicked == INT32_MAX) break;
				const AirportSpec *as = AirportClass::Get(_selected_airport_class)->GetSpec(num_clicked);
				if (as != nullptr && as->IsAvailable(_cur_airtype)) this->SelectOtherAirport(num_clicked);
				break;
			}

			case WID_AP_BTN_DONTHILIGHT: case WID_AP_BTN_DOHILIGHT:
				_settings_client.gui.station_show_coverage = (widget != WID_AP_BTN_DONTHILIGHT);
				this->SetWidgetLoweredState(WID_AP_BTN_DONTHILIGHT, !_settings_client.gui.station_show_coverage);
				this->SetWidgetLoweredState(WID_AP_BTN_DOHILIGHT, _settings_client.gui.station_show_coverage);
				this->SetDirty();
				if (_settings_client.sound.click_beep) SndPlayFx(SND_15_BEEP);
				this->UpdateSelectSize();
				SetViewportCatchmentStation(nullptr, true);
				break;

			case WID_AP_LAYOUT_DECREASE:
				_selected_airport_layout--;
				this->UpdateSelectSize();
				this->SetDirty();
				break;

			case WID_AP_LAYOUT_INCREASE:
				_selected_airport_layout++;
				this->UpdateSelectSize();
				this->SetDirty();
				break;

			case WID_AP_ROTATION_DECREASE:
				_selected_rotation = (DiagDirection)((static_cast<uint>(_selected_rotation) + 3) % 4);
				this->UpdateSelectSize();
				this->SetDirty();
				break;

			case WID_AP_ROTATION_INCREASE:
				_selected_rotation = (DiagDirection)((static_cast<uint>(_selected_rotation) + 1) % 4);
				this->UpdateSelectSize();
				this->SetDirty();
				break;
		}
	}

	/**
	 * Select the first available airport.
	 * @param change_class If true, change the class if no airport in the current
	 *   class is available.
	 */
	void SelectFirstAvailableAirport(bool change_class)
	{
		/* First try to select an airport in the selected class. */
		AirportClass *sel_apclass = AirportClass::Get(_selected_airport_class);
		for (const AirportSpec *as : sel_apclass->Specs()) {
			if (as->IsAvailable(_cur_airtype)) {
				this->SelectOtherAirport(as->index);
				return;
			}
		}
		if (change_class) {
			/* If that fails, select the first available airport
			 * from the first class where airports are available. */
			for (const auto &cls : AirportClass::Classes()) {
				for (const auto &as : cls.Specs()) {
					if (as->IsAvailable(_cur_airtype)) {
						_selected_airport_class = cls.Index();
						this->vscroll->SetCount(cls.GetSpecCount());
						this->SelectOtherAirport(as->index);
						return;
					}
				}
			}
		}
		/* If all airports are unavailable, select nothing. */
		this->SelectOtherAirport(-1);
	}

	void OnDropdownSelect(WidgetID widget, int index, int click_result) override
	{
		if (widget == WID_AP_CLASS_DROPDOWN) {
			_selected_airport_class = (AirportClassID)index;
			this->vscroll->SetCount(AirportClass::Get(_selected_airport_class)->GetSpecCount());
			this->SelectFirstAvailableAirport(false);
		}
	}

	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		CheckRedrawStationCoverage(this);
	}

	IntervalTimer<TimerGameCalendar> yearly_interval = {{TimerGameCalendar::Trigger::Year, TimerGameCalendar::Priority::None}, [this](auto) {
		this->InvalidateData();
	}};
};

static constexpr NWidgetPart _nested_build_airport_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen), SetStringTip(STR_STATION_BUILD_AIRPORT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPadding(WidgetDimensions::unscaled.picker),
			NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_picker, 0),
				NWidget(WWT_LABEL, Colours::DarkGreen), SetStringTip(STR_STATION_BUILD_AIRPORT_CLASS_LABEL, STR_NULL), SetFill(1, 0),
				NWidget(WWT_DROPDOWN, Colours::Grey, WID_AP_CLASS_DROPDOWN), SetFill(1, 0), SetStringTip(STR_JUST_STRING, STR_STATION_BUILD_AIRPORT_TOOLTIP),
				NWidget(WWT_EMPTY, Colours::DarkGreen, WID_AP_AIRPORT_SPRITE), SetFill(1, 0),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_MATRIX, Colours::Grey, WID_AP_AIRPORT_LIST), SetFill(1, 0), SetMatrixDataTip(1, 5, STR_STATION_BUILD_AIRPORT_TOOLTIP), SetScrollbar(WID_AP_SCROLLBAR),
					NWidget(NWID_VSCROLLBAR, Colours::Grey, WID_AP_SCROLLBAR),
				EndContainer(),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_PUSHARROWBTN, Colours::Grey, WID_AP_LAYOUT_DECREASE), SetMinimalSize(12, 0), SetArrowWidgetTypeTip(ArrowWidgetType::Decrease, STR_NULL),
					NWidget(WWT_LABEL, Colours::Grey, WID_AP_LAYOUT_NUM), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_JUST_STRING1, STR_NULL),
					NWidget(WWT_PUSHARROWBTN, Colours::Grey, WID_AP_LAYOUT_INCREASE), SetMinimalSize(12, 0), SetArrowWidgetTypeTip(ArrowWidgetType::Increase, STR_NULL),
				EndContainer(),
				NWidget(NWID_HORIZONTAL),
				NWidget(WWT_PUSHARROWBTN, Colours::Grey, WID_AP_ROTATION_DECREASE), SetMinimalSize(12, 0), SetArrowWidgetTypeTip(ArrowWidgetType::Decrease, STR_NULL),
				NWidget(WWT_LABEL, Colours::Grey, WID_AP_ROTATION), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_JUST_STRING, STR_NULL),
				NWidget(WWT_PUSHARROWBTN, Colours::Grey, WID_AP_ROTATION_INCREASE), SetMinimalSize(12, 0), SetArrowWidgetTypeTip(ArrowWidgetType::Increase, STR_NULL),
				EndContainer(),
				NWidget(WWT_EMPTY, Colours::DarkGreen, WID_AP_EXTRA_TEXT), SetFill(1, 0), SetMinimalSize(150, 0),
				NWidget(WWT_LABEL, Colours::DarkGreen), SetStringTip(STR_STATION_BUILD_COVERAGE_AREA_TITLE, STR_NULL), SetFill(1, 0),
				NWidget(NWID_HORIZONTAL), SetPIP(14, 0, 14), SetPIPRatio(1, 0, 1),
					NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_AP_BTN_DONTHILIGHT), SetMinimalSize(60, 12), SetFill(1, 0),
													SetStringTip(STR_STATION_BUILD_COVERAGE_OFF, STR_STATION_BUILD_COVERAGE_AREA_OFF_TOOLTIP),
						NWidget(WWT_TEXTBTN, Colours::Grey, WID_AP_BTN_DOHILIGHT), SetMinimalSize(60, 12), SetFill(1, 0),
													SetStringTip(STR_STATION_BUILD_COVERAGE_ON, STR_STATION_BUILD_COVERAGE_AREA_ON_TOOLTIP),
					EndContainer(),
				EndContainer(),
			EndContainer(),
			NWidget(WWT_EMPTY, Colours::DarkGreen, WID_AP_ACCEPTANCE), SetResize(0, 1), SetFill(1, 0), SetMinimalTextLines(2, WidgetDimensions::unscaled.vsep_normal),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _build_airport_desc(__FILE__, __LINE__,
	WindowPosition::Automatic, nullptr, 0, 0,
	WindowClass::BuildStation, WindowClass::BuildToolbar,
	WindowDefaultFlag::Construction,
	_nested_build_airport_widgets
);

static void ShowBuildAirportPicker(Window *parent)
{
	new BuildAirportWindow(_build_airport_desc, parent);
}

struct BuildHangarWindow : public PickerWindowBase {
	BuildHangarWindow(WindowDesc &desc, Window *parent) : PickerWindowBase(desc, parent)
	{
		this->CreateNestedTree();
		this->LowerWidget(WID_BHW_NE + _rotation_dir);
		this->FinishInitNested(TransportType::Air);
	}

	uint GetHangarSpriteHeight() const { return 48; }

	void UpdateWidgetSize(int widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (!IsInsideMM(widget, WID_BHW_NE, WID_BHW_NW + 1)) return;

		size.width  = ScaleGUITrad(64) + 2;
		size.height = ScaleGUITrad(52) + 2;
	}

	void DrawWidget(const Rect &r, int widget) const override
	{
		if (!IsInsideMM(widget, WID_BHW_NE, WID_BHW_NW + 1)) return;

		int x = r.left + 1 + ScaleGUITrad(TILE_PIXELS - 1);
		/* Height of depot sprite in OpenGFX is TILE_PIXELS + GetHangarSpriteHeight(). */
		int y = r.bottom - ScaleGUITrad(TILE_PIXELS - 1);

		SpriteID ground = GetAirTypeInfo(_cur_airtype)->base_sprites.ground[0];
		DiagDirection dir = (DiagDirection)(widget - WID_BHW_NE + static_cast<int>(DiagDirection::NE));
		PaletteID palette = GetCompanyPalette(_local_company);
		extern const DrawTileSpriteSpan _airport_hangars[4];
		const DrawTileSpriteSpan *dts = &_airport_hangars[static_cast<uint>(dir)];
		DrawSprite(ground, PAL_NONE, x, y);
		DrawRailTileSeqInGUI(x, y, dts, ground, 0, palette);
	}

	void OnClick([[maybe_unused]] Point pt, int widget, [[maybe_unused]] int click_count) override
	{
		if (!IsInsideMM(widget, WID_BHW_NE, WID_BHW_NW + 1)) return;

		this->RaiseWidget(WID_BHW_NE + _rotation_dir);
		_rotation_dir = (DiagDirection)(widget - WID_BHW_NE);
		this->LowerWidget(WID_BHW_NE + _rotation_dir);
		if (_settings_client.sound.click_beep) SndPlayFx(SND_15_BEEP);
		this->SetDirty();
	}
};

static const NWidgetPart _nested_build_hangar_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_BHW_CAPTION), SetStringTip(STR_BUILD_HANGAR_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_HORIZONTAL_LTR), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1), SetPadding(WidgetDimensions::unscaled.picker),
			NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BHW_NW), SetStringTip(STR_NULL, STR_BUILD_HANGAR_ORIENTATION_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BHW_SW), SetStringTip(STR_NULL, STR_BUILD_HANGAR_ORIENTATION_TOOLTIP),
			EndContainer(),
			NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BHW_NE), SetStringTip(STR_NULL, STR_BUILD_HANGAR_ORIENTATION_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BHW_SE), SetStringTip(STR_NULL, STR_BUILD_HANGAR_ORIENTATION_TOOLTIP),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _build_hangar_desc(__FILE__, __LINE__,
	WindowPosition::Automatic, nullptr, 0, 0,
	WindowClass::BuildDepot, WindowClass::BuildToolbar,
	WindowDefaultFlag::Construction,
	_nested_build_hangar_widgets
);

static void ShowHangarPicker(Window *parent)
{
	new BuildHangarWindow(_build_hangar_desc, parent);
}

struct SelectTrackGfxWindow : public PickerWindowBase {
	SelectTrackGfxWindow(WindowDesc &desc, Window *parent) : PickerWindowBase(desc, parent)
	{
		this->CreateNestedTree();
		this->LowerWidget(_selected_track_gfx_index);
		this->FinishInitNested(TransportType::Air);
	}

	void UpdateWidgetSize(int widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (!IsInsideMM(widget, WID_BASGFAT_AUTO, WID_BASGFAT_20 + 1)) return;

		size.width = ScaleGUITrad(64) + 2;
		size.height = ScaleGUITrad(32) + 2;
	}

	void DrawWidget(const Rect &r, int widget) const override
	{
		if (!IsInsideMM(widget, WID_BASGFAT_01, WID_BASGFAT_20 + 1)) return;

		int x = r.left + 1 + ScaleGUITrad(TILE_PIXELS - 1);
		/* Height of depot sprite in OpenGFX is TILE_PIXELS + GetHangarSpriteHeight(). */
		int y = r.bottom - ScaleGUITrad(TILE_PIXELS - 1);

		SpriteID ground = GetAirTypeInfo(_cur_airtype)->base_sprites.ground[widget - WID_BASGFAT_01];
		DrawSprite(ground, PAL_NONE, x, y);
	}

	void OnClick([[maybe_unused]] Point pt, int widget, [[maybe_unused]] int click_count) override
	{
		if (!IsInsideMM(widget, WID_BASGFAT_AUTO, WID_BASGFAT_20 + 1)) return;

		this->RaiseWidget(WID_BASGFAT_AUTO + _selected_track_gfx_index);
		_selected_track_gfx_index = widget - WID_BASGFAT_AUTO;
		this->LowerWidget(WID_BASGFAT_AUTO + _selected_track_gfx_index);
		if (_settings_client.sound.click_beep) SndPlayFx(SND_15_BEEP);
		this->SetDirty();
	}
};

static const NWidgetPart _nested_select_track_gfx_desc[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_BASGFAT_CAPTION), SetStringTip(STR_SELECT_GFX_AIRPORT_TRACKS_CAPTION, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0), SetPIPRatio(1, 0, 1), SetPadding(WidgetDimensions::unscaled.picker),
			NWidget(NWID_HORIZONTAL_LTR), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_DEFAULT), SetFill(1, 0),
				SetStringTip(STR_SELECT_GFX_AIRPORT_TRACKS_DEFAULT, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_AUTO), SetFill(1, 0),
				SetStringTip(STR_SELECT_GFX_AIRPORT_TRACKS_DETECT, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL_LTR), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_01), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_06), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_07), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_08), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL_LTR), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_13), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_14), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_15), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_16), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL_LTR), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_09), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_10), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_11), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_12), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL_LTR), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_02), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_03), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_04), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_05), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL_LTR), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_17), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_18), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_19), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BASGFAT_20), SetStringTip(STR_NULL, STR_SELECT_GFX_AIRPORT_TRACKS_TOOLTIP),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _select_track_gfx_desc(__FILE__, __LINE__,
	WindowPosition::Automatic, nullptr, 0, 0,
	WindowClass::SelectTrackGfx, WindowClass::BuildToolbar,
	WindowDefaultFlag::Construction,
	_nested_select_track_gfx_desc
);

static void ShowTrackGfxPicker(Window *parent)
{
	new SelectTrackGfxWindow(_select_track_gfx_desc, parent);
}

struct BuildHeliportWindow : public PickerWindowBase {
	BuildHeliportWindow(WindowDesc &desc, Window *parent) : PickerWindowBase(desc, parent)
	{
		this->CreateNestedTree();
		this->LowerWidget(WID_BHW_NE + _rotation_dir);
		this->FinishInitNested(TransportType::Air);
	}

	uint GetHeliportSpriteHeight() const { return 91; }

	void UpdateWidgetSize(int widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (!IsInsideMM(widget, WID_BHW_NE, WID_BHW_NW + 1)) return;

		size.width  = ScaleGUITrad(64) + 2;
		size.height = ScaleGUITrad(GetHeliportSpriteHeight()) + 2;
	}

	void DrawWidget(const Rect &r, int widget) const override
	{
		if (!IsInsideMM(widget, WID_BHW_NE, WID_BHW_NW + 1)) return;

		int x = r.left + 1 + ScaleGUITrad(TILE_PIXELS - 1);
		/* Height of depot sprite in OpenGFX is TILE_PIXELS + GetHangarSpriteHeight(). */
		int y = r.bottom - ScaleGUITrad(TILE_PIXELS - 1);

		SpriteID ground = GetAirTypeInfo(_cur_airtype)->base_sprites.ground[0];
		DiagDirection dir = (DiagDirection)(widget - WID_BHW_NE + static_cast<int>(DiagDirection::NE));
		PaletteID palette = GetCompanyPalette(_local_company);
		extern const DrawTileSpriteSpan _airport_heliports[];
		const DrawTileSpriteSpan *dts = &_airport_heliports[0];
		DrawSprite(ground, PAL_NONE, x, y);
		DrawRailTileSeqInGUI(x, y, dts, ground + static_cast<uint>(dir), 0, palette);
	}

	void OnClick([[maybe_unused]] Point pt, int widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_BHW_NW:
			case WID_BHW_NE:
			case WID_BHW_SW:
			case WID_BHW_SE:
				this->RaiseWidget(WID_BHW_NE + _rotation_dir);
				_rotation_dir = (DiagDirection)(widget - WID_BHW_NE);
				this->LowerWidget(WID_BHW_NE + _rotation_dir);
				if (_settings_client.sound.click_beep) SndPlayFx(SND_15_BEEP);
				this->SetDirty();
				break;

			default:
				break;
		}
	}
};

static const NWidgetPart _nested_build_heliport_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_BHW_CAPTION), SetStringTip(STR_BUILD_HELIPORT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_HORIZONTAL_LTR), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1), SetPadding(WidgetDimensions::unscaled.picker),
			NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BHW_NW), SetStringTip(STR_NULL, STR_BUILD_HELIPORT_ORIENTATION_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BHW_SW), SetStringTip(STR_NULL, STR_BUILD_HELIPORT_ORIENTATION_TOOLTIP),
			EndContainer(),
			NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BHW_NE), SetStringTip(STR_NULL, STR_BUILD_HELIPORT_ORIENTATION_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::Grey, WID_BHW_SE), SetStringTip(STR_NULL, STR_BUILD_HELIPORT_ORIENTATION_TOOLTIP),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _build_heliport_desc(__FILE__, __LINE__,
	WindowPosition::Automatic, nullptr, 0, 0,
	WindowClass::BuildHeliport, WindowClass::BuildToolbar,
	WindowDefaultFlag::Construction,
	_nested_build_heliport_widgets
);

static void ShowHeliportPicker(Window *parent)
{
	new BuildHeliportWindow(_build_heliport_desc, parent);
}

/** One selectable built-in graphic or NewGRF source tile. */
struct AirportInfraEntry {
	AirportTiles builtin = INVALID_AIRPORTTILE;
	AirportTileGfxSet gfx{};
	StringID name = INVALID_STRING_ID;

	bool HasDirection(DiagDirection dir) const
	{
		return this->builtin != INVALID_AIRPORTTILE || this->gfx.variants[to_underlying(dir)].has_value();
	}

	DiagDirection GetAvailableDirection(DiagDirection preferred) const
	{
		if (this->HasDirection(preferred)) return preferred;
		for (DiagDirection dir = DiagDirection::Begin; dir < DiagDirection::End; dir++) {
			if (this->HasDirection(dir)) return dir;
		}
		NOT_REACHED();
	}

	const AirportTileGraphics &GetRepresentative() const
	{
		return *this->gfx.variants[to_underlying(this->GetAvailableDirection(DiagDirection::NE))];
	}
};

/** Built-in graphics, or the graphics provided by one NewGRF. */
struct AirportInfraClass {
	std::string name;
	std::vector<AirportInfraEntry> entries;
};

/**
 * Get the tile sequence of a built-in air type infrastructure graphic.
 * @param at The air type graphic.
 * @param dir The direction.
 * @param ground Receives the ground sprite the sequence expects, or 0 when it has none.
 * @return The tile sequence, or nullptr when there is none.
 */
static const DrawTileSpriteSpan *GetBuiltinInfraSpan(AirportTiles at, uint dir, SpriteID &ground)
{
	extern const DrawTileSpriteSpan _airtype_display_datas[];
	extern const DrawTileSpriteSpan _airtype_display_datas_radar[];
	extern const DrawTileSpriteSpan _airtype_display_datas_tower[];
	extern const DrawTileSpriteSpan _airtype_display_datas_transmitter[];
	extern const DrawTileSpriteSpan _airtype_display_datas_flag_NE[];
	extern const DrawTileSpriteSpan _airtype_display_datas_flag_SE[];
	extern const DrawTileSpriteSpan _airtype_display_datas_flag_SW[];
	extern const DrawTileSpriteSpan _airtype_display_datas_flag_NW[];

	static const DrawTileSpriteSpan *const flag_spans[] = {
		_airtype_display_datas_flag_NE, _airtype_display_datas_flag_SE,
		_airtype_display_datas_flag_SW, _airtype_display_datas_flag_NW,
	};

	switch (at) {
		case ATTG_NO_CATCH_FLAG:
			ground = 0;
			return flag_spans[dir];

		case ATTG_NO_CATCH_TRANSMITTER:
			return &_airtype_display_datas_transmitter[dir];

		case ATTG_NO_CATCH_TOWER:
			return &_airtype_display_datas_tower[dir];

		case ATTG_NO_CATCH_RADAR:
			ground = 0;
			return &_airtype_display_datas_radar[2];

		default:
			return &_airtype_display_datas[static_cast<uint>(at) * 4 + dir];
	}
}

/** Draw one tile in the requested direction, with no substitute for a missing direction. */
static void DrawInfraGraphic(int x, int y, const AirportInfraEntry &entry, DiagDirection dir)
{
	const PaletteID palette = GetCompanyPalette(_local_company);
	AirportTiles builtin = entry.builtin;
	if (builtin == INVALID_AIRPORTTILE) {
		const auto &graphics = entry.gfx.variants[to_underlying(dir)];
		if (!graphics.has_value() || DrawAirportTileGraphicsInGUI(x, y, *graphics, palette)) return;

		const AirportTileTable *source = GetAirportTileGraphicsSource(*graphics);
		if (source == nullptr || source->at_gfx >= ATTG_END) return;
		builtin = source->at_gfx;
		if (IsValidDiagDirection(source->dir)) dir = RotateDiagDir(source->dir, dir);
	}

	const SpriteID airtype_ground = GetAirTypeInfo(_cur_airtype)->base_sprites.ground[0];
	SpriteID seq_ground = airtype_ground;
	const DrawTileSpriteSpan *dts = GetBuiltinInfraSpan(builtin, to_underlying(dir), seq_ground);
	DrawSprite(airtype_ground, PAL_NONE, x, y);
	DrawRailTileSeqInGUI(x, y, dts, seq_ground, 0, palette);
}

static PickerItem GetAirportTilePickerItem(const AirportTileGraphics &graphics, int cls_id, int id)
{
	return {graphics.tile_grfid, graphics.tile_id, cls_id, id,
			PickerItemSource{graphics.source_grfid, graphics.source_airport, graphics.source_layout, graphics.source_pos, to_underlying(graphics.source_rotation)}};
}

/** Shared picker callbacks and in-session selection for one type of airport infrastructure. */
class AirportInfraPickerCallbacks : public PickerCallbacks {
private:
	using EntryIndex = std::pair<int, int>;

	const AirportTileType tile_type;
	std::vector<AirportInfraClass> classes;
	std::map<PickerItem, EntryIndex> entries_by_variant;
	mutable int sel_class = 0;
	mutable int sel_type = 0;
	DiagDirection preferred_rotation = DiagDirection::NE;

	void AddBuiltinEntries()
	{
		static const StringID catch_names[] = {
			STR_AIRPORT_TILE_BUILDING_1, STR_AIRPORT_TILE_BUILDING_2, STR_AIRPORT_TILE_BUILDING_3,
			STR_AIRPORT_TILE_BUILDING_FLAT, STR_AIRPORT_TILE_BUILDING_TERMINAL,
		};
		static const StringID nocatch_names[] = {
			STR_AIRPORT_TILE_FLAG, STR_AIRPORT_TILE_TRANSMITTER, STR_AIRPORT_TILE_TOWER,
			STR_AIRPORT_TILE_RADAR, STR_AIRPORT_TILE_PIER, STR_AIRPORT_TILE_EMPTY,
		};

		const bool nocatch = this->tile_type == ATT_INFRASTRUCTURE_NO_CATCH;
		const StringID *names = nocatch ? nocatch_names : catch_names;
		const uint first = nocatch ? ATTG_NO_CATCH_FLAG : ATTG_WITH_CATCH_BUILDING_1;
		const uint count = nocatch ? lengthof(nocatch_names) : lengthof(catch_names);

		auto &entries = this->classes.emplace_back().entries;
		for (uint i = 0; i < count; i++) {
			AirportTiles gfx = static_cast<AirportTiles>(first + i);
			entries.push_back({.builtin = gfx, .name = names[i]});
			this->entries_by_variant.emplace(PickerItem{GrfID{}, static_cast<uint16_t>(gfx), 0, static_cast<int>(i)}, EntryIndex{0, static_cast<int>(i)});
		}
	}

	void AddNewGRFEntries()
	{
		std::vector<AirportTileGfxSet> sets;
		GetGRFAirportTileCandidates(this->tile_type, sets);
		std::map<GrfID, int> classes_by_grfid;

		for (const auto &set : sets) {
			AirportInfraEntry entry{.gfx = set, .name = STR_AIRPORT_TILE_NEWGRF_DEFAULT};
			const auto &graphics = entry.GetRepresentative();
			const StationGfx gfx = ResolveAirportTileGraphics(graphics);
			if (gfx == INVALID_AIRPORTTILE) continue;
			const AirportTileSpec *spec = AirportTileSpec::Get(gfx);
			if (spec->name != STR_NULL && spec->name != INVALID_STRING_ID) entry.name = spec->name;

			const auto [it, inserted] = classes_by_grfid.try_emplace(graphics.tile_grfid, static_cast<int>(this->classes.size()));
			if (inserted) {
				const GRFConfig *config = GetGRFConfig(graphics.tile_grfid);
				std::string_view name = config != nullptr ? config->GetName() : std::string_view{};
				if (name.empty() && spec->grf_prop.grffile != nullptr) name = spec->grf_prop.grffile->filename;
				this->classes.emplace_back().name = name.empty() ? graphics.tile_grfid.AsString() : std::string(name);
			}

			const int cls_id = it->second;
			auto &entries = this->classes[cls_id].entries;
			const int id = static_cast<int>(entries.size());
			for (const auto &variant : set.variants) {
				if (!variant.has_value()) continue;
				this->entries_by_variant.emplace(GetAirportTilePickerItem(*variant, cls_id, id), EntryIndex{cls_id, id});
			}
			entries.push_back(std::move(entry));
		}
	}

	const AirportInfraEntry &GetEntry(int cls_id, int id) const { return this->classes[cls_id].entries[id]; }
	const AirportInfraEntry &GetSelectedEntry() const { return this->GetEntry(this->sel_class, this->sel_type); }

public:
	AirportInfraPickerCallbacks(const std::string &ini_group, AirportTileType tile_type) : PickerCallbacks(ini_group), tile_type(tile_type) {}

	void RefreshEntries()
	{
		std::optional<PickerItem> previous;
		if (!this->classes.empty()) previous = this->GetPickerItem(this->sel_class, this->sel_type);

		this->classes.clear();
		this->entries_by_variant.clear();
		this->AddBuiltinEntries();
		this->AddNewGRFEntries();

		this->sel_class = this->sel_type = 0;
		if (previous.has_value()) {
			if (auto it = this->entries_by_variant.find(*previous); it != this->entries_by_variant.end()) {
				std::tie(this->sel_class, this->sel_type) = it->second;
			}
		}
	}

	void ResetSelection()
	{
		this->sel_class = this->sel_type = 0;
		this->preferred_rotation = DiagDirection::NE;
	}

	AirportTileType GetTileType() const { return this->tile_type; }
	DiagDirection GetRotation() const { return this->GetSelectedEntry().GetAvailableDirection(this->preferred_rotation); }
	bool HasDirection(DiagDirection dir) const { return this->GetSelectedEntry().HasDirection(dir); }
	void SetRotation(DiagDirection dir) { this->preferred_rotation = dir; }
	void DrawDirection(int x, int y, DiagDirection dir) const { DrawInfraGraphic(x, y, this->GetSelectedEntry(), dir); }

	AirportInfraSelection GetSelection() const
	{
		const auto &entry = this->GetSelectedEntry();
		const auto dir = this->GetRotation();
		return {entry.builtin, dir, entry.gfx.variants[to_underlying(dir)]};
	}

	GrfSpecFeature GetFeature() const override { return GrfSpecFeature::AirportTiles; }
	bool IsActive() const override { return true; }
	bool HasClassChoice() const override { return this->classes.size() > 1; }

	StringID GetClassTooltip() const override { return STR_PICKER_AIRPORT_TILE_CLASS_TOOLTIP; }
	int GetClassCount() const override { return static_cast<int>(this->classes.size()); }
	int GetSelectedClass() const override { return this->sel_class; }

	void SetSelectedClass(int id) const override
	{
		if (this->sel_class == id) return;
		this->sel_class = id;
		this->sel_type = 0;
	}

	StringID GetClassName(int id) const override { return id == 0 ? STR_AIRPORT_TILE_CLASS_BUILTIN : STR_AIRPORT_TILE_CLASS_NEWGRF; }
	std::string GetClassNameString(int id) const override { return id == 0 ? PickerCallbacks::GetClassNameString(id) : this->classes[id].name; }

	StringID GetTypeTooltip() const override { return STR_PICKER_AIRPORT_TILE_TYPE_TOOLTIP; }
	StringID GetRandomTooltip() const override { return STR_PICKER_AIRPORT_TILE_RANDOM_TOOLTIP; }
	StringID GetCollectionTooltip() const override { return STR_PICKER_AIRPORT_TILE_COLLECTION_TOOLTIP; }

	int GetTypeCount(int cls_id) const override { return static_cast<int>(this->classes[cls_id].entries.size()); }
	int GetSelectedType() const override { return this->sel_type; }
	void SetSelectedType(int id) const override { this->sel_type = id; }

	PickerItem GetPickerItem(int cls_id, int id) const override
	{
		const auto &entry = this->GetEntry(cls_id, id);
		if (entry.builtin != INVALID_AIRPORTTILE) return {GrfID{}, static_cast<uint16_t>(entry.builtin), cls_id, id};
		return GetAirportTilePickerItem(entry.GetRepresentative(), cls_id, id);
	}

	StringID GetTypeName(int cls_id, int id) const override
	{
		if (!IsInsideMM(cls_id, 0, this->GetClassCount()) || !IsInsideMM(id, 0, this->GetTypeCount(cls_id))) return INVALID_STRING_ID;
		return this->GetEntry(cls_id, id).name;
	}

	std::span<const BadgeID> GetTypeBadges(int cls_id, int id) const override
	{
		const auto &entry = this->GetEntry(cls_id, id);
		if (entry.builtin != INVALID_AIRPORTTILE) return {};
		const StationGfx gfx = ResolveAirportTileGraphics(entry.GetRepresentative());
		if (gfx == INVALID_AIRPORTTILE) return {};
		return AirportTileSpec::Get(gfx)->badges;
	}

	bool IsTypeAvailable(int cls_id, int id) const override { return this->GetTypeName(cls_id, id) != INVALID_STRING_ID; }

	void DrawType(int x, int y, int cls_id, int id) const override
	{
		const auto &entry = this->GetEntry(cls_id, id);
		/* Grid thumbnails use an available variant. Only the direction buttons select a rotation. */
		DrawInfraGraphic(x, y, entry, entry.GetAvailableDirection(this->preferred_rotation));
	}

	void SetSelectedCollection([[maybe_unused]] const btree::btree_set<PickerItem> &items) const override { /* Random placement is not supported. */ }

	void FillUsedItems(btree::btree_set<PickerItem> &items) override
	{
		for (const Station *st : Station::Iterate()) {
			if (st->owner != _local_company || st->airport.tile == INVALID_TILE) continue;

			for (TileIndex tile : st->airport) {
				if (!st->TileBelongsToAirport(tile) || GetAirportTileType(tile) != this->tile_type) continue;

				PickerItem key;
				if (const AirportTileGraphics *graphics = GetAirportTileGraphics(tile); graphics != nullptr) {
					key = GetAirportTilePickerItem(*graphics, 0, 0);
				} else if (HasAirtypeGfx(tile)) {
					key = {GrfID{}, static_cast<uint16_t>(GetTileAirportGfx(tile)), 0, 0};
				} else {
					continue;
				}
				if (auto it = this->entries_by_variant.find(key); it != this->entries_by_variant.end()) {
					items.insert(this->GetPickerItem(it->second.first, it->second.second));
				}
			}
		}
	}

	PickerItemsCollection UpdateSavedItems(const PickerItemsCollection &src) override
	{
		if (src.empty()) return {};

		/* A legacy sprite-only key can be migrated only if it identifies a unique source tile. */
		std::map<std::pair<GrfID, uint16_t>, EntryIndex> legacy_entries;
		for (const auto &[key, index] : this->entries_by_variant) {
			if (!key.source.has_value()) continue;
			auto [it, inserted] = legacy_entries.try_emplace(std::pair{key.grfid, key.local_id}, index);
			if (!inserted && it->second != index) it->second = {-1, -1};
		}

		PickerItemsCollection dst;
		for (const auto &[name, items] : src) {
			auto &dst_items = dst[name];
			for (const auto &item : items) {
				EntryIndex index{-1, -1};
				if (!item.source.has_value() && !item.grfid.Empty()) {
					if (auto it = legacy_entries.find({item.grfid, item.local_id}); it != legacy_entries.end()) index = it->second;
				} else {
					if (auto it = this->entries_by_variant.find(item); it != this->entries_by_variant.end()) index = it->second;
				}

				if (index.first >= 0) {
					dst_items.insert(this->GetPickerItem(index.first, index.second));
				} else {
					dst_items.emplace(item.grfid, item.local_id, -1, -1, item.source);
				}
			}
		}
		return dst;
	}
};

/* Register both pickers before the favourites configuration is loaded. */
static AirportInfraPickerCallbacks _airport_infra_catch_callbacks("fav_airport_infra_catch", ATT_INFRASTRUCTURE_WITH_CATCH);
static AirportInfraPickerCallbacks _airport_infra_nocatch_callbacks("fav_airport_infra_nocatch", ATT_INFRASTRUCTURE_NO_CATCH);

static AirportInfraSelection GetAirportInfraSelection(AirportTileType type)
{
	return (type == ATT_INFRASTRUCTURE_WITH_CATCH ? _airport_infra_catch_callbacks : _airport_infra_nocatch_callbacks).GetSelection();
}

/**
 * Build airport infrastructure: pick the graphics like in the rail station window.
 */
struct BuildAirportInfraWindow : public PickerWindow {
	AirportInfraPickerCallbacks &infra_callbacks;

	BuildAirportInfraWindow(WindowDesc &desc, Window *parent, AirportInfraPickerCallbacks &callbacks) :
			PickerWindow(desc, parent, static_cast<int>(callbacks.GetTileType()), callbacks), infra_callbacks(callbacks)
	{
		this->infra_callbacks.RefreshEntries();
		this->ConstructWindow();
	}

	void Close(int data = 0) override
	{
		/* Unlink first so resetting the toolbar does not close this picker again. */
		this->infra_callbacks.Close(data);
		this->Window::Close(data);
		if (data != CLOSE_INFRA_PICKER_ON_TOOL_ABORT) ResetObjectToPlace();
	}

	void UpdateDirections()
	{
		const auto selected = this->infra_callbacks.GetRotation();
		for (DiagDirection dir = DiagDirection::Begin; dir < DiagDirection::End; dir++) {
			const WidgetID widget = WID_BAI_DIR_NE + to_underlying(dir);
			const bool available = this->infra_callbacks.HasDirection(dir);
			this->SetWidgetDisabledState(widget, !available);
			this->SetWidgetLoweredState(widget, dir == selected);
			this->GetWidget<NWidgetCore>(widget)->SetToolTip(available ? STR_BUILD_AIRPORT_INFRA_TOOLTIP : STR_BUILD_AIRPORT_INFRA_UNAVAILABLE_DIRECTION);
		}
	}

	void OnInit() override
	{
		this->GetWidget<NWidgetCore>(WID_BAI_CAPTION)->SetString(
				this->infra_callbacks.GetTileType() == ATT_INFRASTRUCTURE_NO_CATCH ? STR_BUILD_AIRPORT_INFRA_NO_CATCH_CAPTION : STR_BUILD_AIRPORT_INFRA_WITH_CATCH_CAPTION);
		this->PickerWindow::OnInit();
		this->UpdateDirections();
	}

	void OnInvalidateData(int data = 0, bool gui_scope = true) override
	{
		this->PickerWindow::OnInvalidateData(data, gui_scope);
		if (gui_scope) this->UpdateDirections();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, const Dimension &padding, Dimension &fill, Dimension &resize) override
	{
		if (IsInsideMM(widget, WID_BAI_DIR_NE, WID_BAI_DIR_NW + 1)) {
			size.width = ScaleGUITrad(64);
			size.height = ScaleGUITrad(64);
			return;
		}
		this->PickerWindow::UpdateWidgetSize(widget, size, padding, fill, resize);
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (!IsInsideMM(widget, WID_BAI_DIR_NE, WID_BAI_DIR_NW + 1)) {
			this->PickerWindow::DrawWidget(r, widget);
			return;
		}

		const int x = r.left + 1 + ScaleGUITrad(TILE_PIXELS - 1);
		const int y = r.bottom - ScaleGUITrad(TILE_PIXELS - 1);
		this->infra_callbacks.DrawDirection(x, y, static_cast<DiagDirection>(widget - WID_BAI_DIR_NE));
	}

	void OnClick(Point pt, WidgetID widget, int click_count) override
	{
		if (IsInsideMM(widget, WID_BAI_DIR_NE, WID_BAI_DIR_NW + 1)) {
			const auto dir = static_cast<DiagDirection>(widget - WID_BAI_DIR_NE);
			if (!this->infra_callbacks.HasDirection(dir)) return;
			this->infra_callbacks.SetRotation(dir);
			this->UpdateDirections();
			if (_settings_client.sound.click_beep) SndPlayFx(SND_15_BEEP);
			this->SetDirty();
			return;
		}

		this->PickerWindow::OnClick(pt, widget, click_count);
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_build_airport_infra_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_BAI_CAPTION), SetStringTip(STR_BUILD_AIRPORT_INFRA_WITH_CATCH_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::DarkGreen),
		NWidget(WWT_DEFSIZEBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_VERTICAL),
			NWidgetFunction(MakePickerClassWidgets),
			NWidget(WWT_PANEL, Colours::DarkGreen),
				NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_picker, 0), SetPadding(WidgetDimensions::unscaled.picker),
					NWidget(WWT_LABEL, Colours::Invalid, WID_BAI_ROTATION_LABEL), SetMinimalSize(144, 11), SetFill(1, 0), SetStringTip(STR_STATION_BUILD_ORIENTATION),
					NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1),
						NWidget(WWT_PANEL, Colours::Grey, WID_BAI_DIR_NE), SetFill(0, 0), SetToolTip(STR_BUILD_AIRPORT_INFRA_TOOLTIP), EndContainer(),
						NWidget(WWT_PANEL, Colours::Grey, WID_BAI_DIR_SE), SetFill(0, 0), SetToolTip(STR_BUILD_AIRPORT_INFRA_TOOLTIP), EndContainer(),
					EndContainer(),
					NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_normal, 0), SetPIPRatio(1, 0, 1),
						NWidget(WWT_PANEL, Colours::Grey, WID_BAI_DIR_SW), SetFill(0, 0), SetToolTip(STR_BUILD_AIRPORT_INFRA_TOOLTIP), EndContainer(),
						NWidget(WWT_PANEL, Colours::Grey, WID_BAI_DIR_NW), SetFill(0, 0), SetToolTip(STR_BUILD_AIRPORT_INFRA_TOOLTIP), EndContainer(),
					EndContainer(),
				EndContainer(),
			EndContainer(),
		EndContainer(),
		NWidgetFunction(MakePickerTypeWidgets),
	EndContainer(),
};

static WindowDesc _build_airport_infra_desc(__FILE__, __LINE__,
	WindowPosition::Automatic, "build_airport_infra", 0, 0,
	WindowClass::BuildAirportInfrastructure, WindowClass::BuildToolbar,
	WindowDefaultFlag::Construction,
	_nested_build_airport_infra_widgets
);

static void ShowAirportInfraNoCatchPicker(Window *parent)
{
	new BuildAirportInfraWindow(_build_airport_infra_desc, parent, _airport_infra_nocatch_callbacks);
}

static void ShowAirportInfraWithCatchPicker(Window *parent)
{
	new BuildAirportInfraWindow(_build_airport_infra_desc, parent, _airport_infra_catch_callbacks);
}


/** Set the initial (default) airtype to use */
static void SetDefaultAirGui()
{
	if (_local_company == COMPANY_SPECTATOR || !Company::IsValidID(_local_company)) return;

	extern AirType _last_built_airtype;
	AirType rt;
	switch (_settings_client.gui.default_air_type) {
		case 2: {
			/* Find the most used air type */
			uint count[AIRTYPE_END];
			memset(count, 0, sizeof(count));
			for (TileIndex t = TileIndex{}; t.base() < Map::Size(); t++) {
				if (IsAirportTile(t) && GetTileOwner(t) == _local_company) {
					count[GetAirType(t)]++;
				}
			}

			rt = static_cast<AirType>(std::max_element(count + AIRTYPE_BEGIN, count + AIRTYPE_END) - count);
			if (count[rt] > 0) break;

			/* No air, just get the first available one */
			[[fallthrough]];
		}
		case 0: {
			/* Use first available type */
			rt = _sorted_airtypes.empty() ? AIRTYPE_BEGIN : _sorted_airtypes.front();
			break;
		}
		case 1: {
			/* Use last available type */
			rt = _sorted_airtypes.empty() ? AIRTYPE_BEGIN : _sorted_airtypes.back();
			break;
		}
		default:
			NOT_REACHED();
	}

	_last_built_airtype = _cur_airtype = rt;
	BuildAirToolbarWindow *w = dynamic_cast<BuildAirToolbarWindow *>(FindWindowById(WindowClass::BuildToolbar, TransportType::Air));
	if (w != nullptr) w->ModifyAirType(_cur_airtype);
}

/**
 * Create a drop down list for all the air types of the local company.
 * @param for_replacement Whether this list is for the replacement window.
 * @param all_option Whether to add an 'all types' item.
 * @return The populated and sorted #DropDownList.
 */
DropDownList GetAirTypeDropDownList(bool for_replacement, bool all_option)
{
	DropDownList list;

	if (all_option) {
		list.push_back(MakeDropDownListStringItem(STR_REPLACE_ALL_AIRTYPE, INVALID_AIRTYPE, false));
	}

	Dimension d = { 0, 0 };
	/* Get largest icon size, to ensure text is aligned on each menu item. */
	if (!for_replacement) {
		for (const auto &at : _sorted_airtypes) {
			const AirTypeInfo *ati = GetAirTypeInfo(at);
			d = maxdim(d, GetSpriteSize(ati->gui_sprites.build_helipad));
		}
	}

	for (const auto &at : _sorted_airtypes) {
		const AirTypeInfo *ati = GetAirTypeInfo(at);

		if (for_replacement) {
			list.push_back(MakeDropDownListStringItem(ati->strings.replace_text, at, false));
		} else {
			std::string str = ati->max_speed > 0
				? GetString(STR_TOOLBAR_RAILTYPE_VELOCITY, ati->strings.menu_text, ati->max_speed)
				: GetString(ati->strings.menu_text);
			list.push_back(MakeDropDownListIconItem(d, ati->gui_sprites.build_helipad, PAL_NONE, std::move(str), at, false));
		}
	}

	if (list.empty()) {
		/* Empty dropdowns are not allowed */
		list.push_back(MakeDropDownListStringItem(STR_NONE, INVALID_AIRTYPE, true));
	}

	return list;
}

void InitializeAirportGui()
{
	SetDefaultAirGui();

	_selected_airport_class = APC_BEGIN;
	_selected_airport_index = -1;
	_airport_infra_catch_callbacks.ResetSelection();
	_airport_infra_nocatch_callbacks.ResetSelection();
}

