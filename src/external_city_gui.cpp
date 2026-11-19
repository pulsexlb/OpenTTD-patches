/* SPDX-License-Identifier: GPL-2.0-only */
/** @file external_city_gui.cpp Cities independent of entrances, with editable edge segments. */
#include "stdafx.h"
#include "external_city.h"
#include "external_city_cmd.h"
#include "depot_base.h"
#include "depot_map.h"
#include "infrastructure_func.h"
#include "company_base.h"
#include "company_func.h"
#include "command_func.h"
#include "window_gui.h"
#include "window_func.h"
#include "strings_func.h"
#include "dropdown_func.h"
#include "querystring_gui.h"
#include "settings_type.h"
#include "gfx_func.h"
#include "palette_func.h"
#include "landscape.h"
#include "train.h"
#include "roadveh.h"
#include "aircraft.h"
#include "station_base.h"
#include "transparency.h"
#include "vehicle_gui.h"
#include "cargotype.h"
#include "industrytype.h"
#include "date_func.h"
#include "zoom_func.h"
#include "core/backup_type.hpp"
#include "blitter/factory.hpp"
#include "tilehighlight_func.h"
#include "viewport_func.h"
#include "table/strings.h"
#include "table/sprites.h"
#include "spritecache.h"
#include "timer/timer.h"
#include "timer/timer_game_realtime.h"
#include <charconv>
#include "safeguards.h"

static TileIndex external_pick_origin = INVALID_TILE;
static Owner external_pick_owner = OWNER_NONE;
static RailTypes external_pick_railtypes{};
static RoadTypes external_pick_roadtypes{};
void SetExternalReturnHighlight(TileIndex origin, Owner owner, RailTypes compatible, RoadTypes roads)
{
 external_pick_origin = origin; external_pick_owner = owner; external_pick_railtypes = compatible; external_pick_roadtypes = roads;
 MarkWholeScreenDirty();
}
void ClearExternalReturnHighlight()
{
 external_pick_origin = INVALID_TILE; external_pick_owner = OWNER_NONE; external_pick_railtypes = {};
 MarkWholeScreenDirty();
}
bool IsExternalReturnHighlighted(TileIndex tile)
{
 if (external_pick_origin == INVALID_TILE || !Depot::IsExternalConnection(tile) || !IsInfraTileUsageAllowed(GetDepotVehicleType(tile), external_pick_owner, tile) ||
     GetTileType(tile) != GetTileType(external_pick_origin)) return false;
 if (IsRailDepotTile(tile) && !external_pick_railtypes.Test(GetRailType(tile))) return false;
 if (IsRoadDepotTile(tile) && (GetPresentRoadTypes(tile) & external_pick_roadtypes).None()) return false;
 const auto city = GetExternalCityForEntrance(external_pick_origin);
 return city != ExternalCityID::Invalid() && city == GetExternalCityForEntrance(tile);
}

static void ShowExternalCityVehicles(ExternalCityID city);
/** Visual navigation aid immediately outside the playable edge; no map tile is occupied. */
static Coord3D<int> FlightBeaconPosition(const ExternalBoundary &region)
{
 const TileIndex tile = GetExternalFlightGateway(region);
 int x = TileX(tile) * TILE_SIZE, y = TileY(tile) * TILE_SIZE;
 switch (region.edge) {
  case DiagDirection::NE: x -= TILE_SIZE; break;
  case DiagDirection::SW: x += TILE_SIZE; break;
  case DiagDirection::NW: y -= TILE_SIZE; break;
  case DiagDirection::SE: y += TILE_SIZE; break;
  default: break;
 }
 return {x + 7, y + 7, GetTilePixelZ(tile)};
}

static SpriteID FlightRadarSprite()
{
 return SPR_AIRPORT_RADAR_1 + (uint64_t(_state_ticks.base()) / 4) % 12;
}

/** Only redraw radar footprints; never advance or modify simulation state. */
static IntervalTimer<TimerGameRealtime> flight_radar_refresh({TimeType::Milliseconds{120}, TimerGameRealtime::Trigger::Unpaused}, [](auto) {
 if (_game_mode != GameMode::Normal || !_settings_game.vehicle.external_connections || ExternalBoundary::GetNumItems() == 0 || GetMainWindow() == nullptr || IsInvisibilitySet(TransparencyOption::Structures)) return;
 Rect bounds{};
 for (uint frame = 0; frame < 12; ++frame) {
  const Sprite *sprite = GetSprite(SPR_AIRPORT_RADAR_1 + frame, SpriteType::Normal, LowZoomMask(ZoomLevel::Min));
  bounds.left = std::min<int>(bounds.left, sprite->x_offs);
  bounds.top = std::min<int>(bounds.top, sprite->y_offs);
  bounds.right = std::max<int>(bounds.right, sprite->x_offs + sprite->width);
  bounds.bottom = std::max<int>(bounds.bottom, sprite->y_offs + sprite->height);
 }
 for (const auto *region : ExternalBoundary::Iterate()) {
  if (!ExternalCity::IsValidID(region->city) || GetExternalFlightGateway(*region) == INVALID_TILE) continue;
  const auto p = FlightBeaconPosition(*region);
  const Point anchor = RemapCoords(p.x, p.y, p.z);
  MarkAllViewportsDirty(anchor.x + bounds.left, anchor.y + bounds.top, anchor.x + bounds.right, anchor.y + bounds.bottom);
 }
});

void DrawExternalFlightBeacons()
{
 if (!_settings_game.vehicle.external_connections || IsInvisibilitySet(TransparencyOption::Structures)) return;
 for (const auto *region : ExternalBoundary::Iterate()) {
  if (!ExternalCity::IsValidID(region->city) || GetExternalFlightGateway(*region) == INVALID_TILE) continue;
  const auto p = FlightBeaconPosition(*region);
  AddSortableSpriteToDraw(SPR_AIRPORT_APRON, PAL_NONE, p.x - 7, p.y - 7, p.z, {{}, {16, 16, 0}, {}});
  AddSortableSpriteToDraw(FlightRadarSprite(), PAL_NONE, p.x, p.y, p.z, {{}, {2, 2, 8}, {}}, IsTransparencySet(TransparencyOption::Structures));
 }
}

static const ExternalBoundary *FlightBeaconAt(const Viewport *vp, Point point)
{
 if (!_settings_game.vehicle.external_connections || vp->zoom >= ZoomLevel::DrawMap || IsInvisibilitySet(TransparencyOption::Structures)) return nullptr;
 point.x = ScaleByZoom(point.x - vp->left, vp->zoom) + vp->virtual_left;
 point.y = ScaleByZoom(point.y - vp->top, vp->zoom) + vp->virtual_top;
 const Sprite *sprite = GetSprite(FlightRadarSprite(), SpriteType::Normal, LowZoomMask(vp->zoom));
 for (const auto *region : ExternalBoundary::Iterate()) {
  if (!ExternalCity::IsValidID(region->city) || GetExternalFlightGateway(*region) == INVALID_TILE) continue;
  const auto p = FlightBeaconPosition(*region);
  const Point anchor = RemapCoords(p.x, p.y, p.z);
  const Rect hit{anchor.x + sprite->x_offs, anchor.y + sprite->y_offs, anchor.x + sprite->x_offs + sprite->width - 1, anchor.y + sprite->y_offs + sprite->height - 1};
  if (hit.Contains(point)) return region;
 }
 return nullptr;
}

bool ClickExternalFlightBeacon(const Viewport *vp, Point point)
{
 const auto *region = FlightBeaconAt(vp, point);
 if (region == nullptr) return false;
 ShowExternalCityVehicles(region->city);
 return true;
}

enum ExternalCityWidgets : WidgetID { EC_VEHICLES, EC_CAPTION, EC_CITY, EC_TIME, EC_NEW, EC_RENAME, EC_REGION, EC_DRAW, EC_DELETE, EC_HINT, EC_OVERVIEW, EC_SHOW, EC_LOCATE, EC_ECONOMY, EC_STOP, EC_BOUNDARY_TOGGLE, EC_BOUNDARY_PLANE };
static constexpr NWidgetPart _external_city_widgets[] = {
 NWidget(NWID_HORIZONTAL),
  NWidget(WWT_CLOSEBOX, Colours::Grey),
  NWidget(WWT_CAPTION, Colours::Grey, EC_CAPTION), SetStringTip(STR_EXTERNAL_CITY_TITLE),
 EndContainer(),
 NWidget(WWT_PANEL, Colours::Grey),
  NWidget(NWID_VERTICAL), SetPadding(WidgetDimensions::unscaled.framerect), SetPIP(0, 4, 0),
   NWidget(NWID_HORIZONTAL),
    NWidget(WWT_DROPDOWN, Colours::Grey, EC_CITY), SetMinimalSize(300, 14), SetFill(1, 0), SetStringTip(STR_EXTERNAL_CITY_UNASSIGNED, STR_EXTERNAL_CITY_LINK_HELP),
    NWidget(WWT_PUSHTXTBTN, Colours::Grey, EC_NEW), SetMinimalSize(100, 14), SetFill(1, 0), SetStringTip(STR_EXTERNAL_CITY_NEW),
   EndContainer(),
   NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
    NWidget(WWT_PUSHTXTBTN, Colours::Grey, EC_TIME), SetMinimalSize(200, 14), SetFill(1, 0), SetStringTip(STR_EXTERNAL_CITY_TIME, STR_EXTERNAL_CITY_TIME_HELP),
    NWidget(WWT_PUSHTXTBTN, Colours::Grey, EC_STOP), SetMinimalSize(200, 14), SetFill(1, 0), SetStringTip(STR_EXTERNAL_CITY_STOP),
   EndContainer(),
   NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
    NWidget(WWT_PUSHTXTBTN, Colours::Grey, EC_ECONOMY), SetMinimalSize(200, 14), SetFill(1, 0), SetStringTip(STR_EXTERNAL_ECONOMY_OPEN),
    NWidget(WWT_PUSHTXTBTN, Colours::Grey, EC_RENAME), SetMinimalSize(200, 14), SetFill(1, 0), SetStringTip(STR_EXTERNAL_CITY_RENAME),
   EndContainer(),
   NWidget(WWT_PUSHTXTBTN, Colours::Grey, EC_VEHICLES), SetFill(1, 0), SetStringTip(STR_EXTERNAL_CITY_VEHICLES),
   NWidget(WWT_PANEL, Colours::Grey, EC_OVERVIEW), SetMinimalSize(400, 145), SetFill(1, 0), SetToolTip(STR_EXTERNAL_BOUNDARY_OVERVIEW_HELP), EndContainer(),
   NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
    NWidget(WWT_TEXTBTN, Colours::Grey, EC_SHOW), SetMinimalSize(200, 14), SetFill(1, 0), SetStringTip(STR_EXTERNAL_BOUNDARY_SHOW),
    NWidget(WWT_PUSHTXTBTN, Colours::Grey, EC_LOCATE), SetMinimalSize(200, 14), SetFill(1, 0), SetStringTip(STR_EXTERNAL_BOUNDARY_LOCATE),
   EndContainer(),
   NWidget(WWT_TEXTBTN, Colours::Grey, EC_BOUNDARY_TOGGLE), SetFill(1, 0), SetStringTip(STR_EXTERNAL_BOUNDARY_EDIT),
   NWidget(NWID_SELECTION, Colours::Invalid, EC_BOUNDARY_PLANE),
    NWidget(NWID_VERTICAL), SetPIP(0, 4, 0),
   NWidget(WWT_DROPDOWN, Colours::Grey, EC_REGION), SetMinimalSize(400, 14), SetFill(1, 0), SetStringTip(STR_EXTERNAL_BOUNDARY_NEW),
   NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
    NWidget(WWT_PUSHTXTBTN, Colours::Grey, EC_DRAW), SetMinimalSize(200, 14), SetFill(1, 0), SetStringTip(STR_EXTERNAL_BOUNDARY_DRAW),
    NWidget(WWT_PUSHTXTBTN, Colours::Grey, EC_DELETE), SetMinimalSize(200, 14), SetFill(1, 0), SetStringTip(STR_EXTERNAL_BOUNDARY_REMOVE),
   EndContainer(),
   NWidget(WWT_TEXT, Colours::Grey, EC_HINT), SetMinimalSize(400, 28), SetFill(1, 0), SetStringTip(STR_EXTERNAL_BOUNDARY_HELP),
    EndContainer(),
   EndContainer(),
  EndContainer(),
 EndContainer(),
};
static WindowDesc _external_city_desc(__FILE__, __LINE__, WindowPosition::Center, nullptr, 420, 350,
 WindowClass::ExternalCity, WindowClass::None, WindowDefaultFlag::Construction, _external_city_widgets);
static constexpr StringID edge_names[] = { STR_EXTERNAL_EDGE_NE, STR_EXTERNAL_EDGE_SE, STR_EXTERNAL_EDGE_SW, STR_EXTERNAL_EDGE_NW };

static Colours BoundaryColour(ExternalCityID city)
{
 static constexpr Colours colours[] = { Colours::LightBlue, Colours::Orange, Colours::Yellow, Colours::Purple, Colours::Pink, Colours::Green, Colours::Cream, Colours::Red };
 return colours[city.base() % std::size(colours)];
}
static TileIndex BoundaryTile(DiagDirection edge, uint coordinate)
{
 const uint low = _settings_game.construction.freeform_edges ? 1 : 0;
 const bool vertical = edge == DiagDirection::NE || edge == DiagDirection::SW;
 coordinate = Clamp<uint>(coordinate, low, (vertical ? Map::SizeY() : Map::SizeX()) - 2);
 switch (edge) {
  case DiagDirection::NE: return TileXY(low, coordinate);
  case DiagDirection::SE: return TileXY(coordinate, Map::SizeY() - 2);
  case DiagDirection::SW: return TileXY(Map::SizeX() - 2, coordinate);
  case DiagDirection::NW: return TileXY(coordinate, low);
  default: return INVALID_TILE;
 }
}
static Point BoundaryOverviewPoint(const Rect &rect, DiagDirection edge, uint coordinate)
{
 const int cx = (rect.left + rect.right) / 2, cy = (rect.top + rect.bottom) / 2;
 const Point top{cx, rect.top + 16}, right{rect.right - 55, cy}, bottom{cx, rect.bottom - 16}, left{rect.left + 55, cy};
 const std::pair<Point, Point> ends[] = {{top, right}, {right, bottom}, {left, bottom}, {top, left}};
 const auto &[a, b] = ends[uint(edge)];
 const uint length = GetExternalBoundaryLength(edge);
 return {a.x + int(int64_t(b.x - a.x) * coordinate / length), a.y + int(int64_t(b.y - a.y) * coordinate / length)};
}

static bool external_city_names_visible = true;
struct ExternalCityWindow : Window {
 ExternalCityOperation query = ExternalCityOperation::Create;
 ExternalCityID city_id = ExternalCityID::Invalid(), query_city = ExternalCityID::Invalid(), draw_city = ExternalCityID::Invalid();
 ExternalBoundaryID region_id = ExternalBoundaryID::Invalid(), draw_region = ExternalBoundaryID::Invalid();
 DiagDirection edge = DiagDirection::NE, draw_edge = DiagDirection::NE;
 std::optional<uint> first;
 TileIndex first_tile = INVALID_TILE;
 bool placing = false;
 bool show_boundaries = true;
 bool edit_boundaries = false;
 bool CanEditBoundaries() const { return _game_mode == GameMode::Editor || _settings_game.vehicle.external_boundary_editing; }
 ExternalCityWindow(DepotID portal) : Window(_external_city_desc) {
  if (const Depot *d = Depot::GetIfValid(portal); d != nullptr && Depot::IsExternalConnection(d->xy)) {
   city_id = GetExternalCityForEntrance(d->xy); edge = ReverseDiagDir(Depot::GetExternalDirection(d->xy));
  }
  show_boundaries = external_city_names_visible;
  if (portal == DepotID::Invalid()) for (const auto *city : ExternalCity::Iterate()) { city_id = city->index; break; }
  this->SelectCityBoundary();
  edit_boundaries = CanEditBoundaries() && region_id == ExternalBoundaryID::Invalid();
  this->CreateNestedTree();
  this->GetWidget<NWidgetStacked>(EC_BOUNDARY_PLANE)->SetDisplayedPlane(edit_boundaries ? 0 : SZSP_HORIZONTAL);
  this->FinishInitNested(0);
  MarkWholeScreenDirty();
 }
 ~ExternalCityWindow() override { MarkWholeScreenDirty(); }
 void SelectCityBoundary() {
  region_id = ExternalBoundaryID::Invalid();
  for (const auto *region : ExternalBoundary::Iterate()) if (region->city == city_id) {
   if (region_id == ExternalBoundaryID::Invalid()) region_id = region->index;
   if (region->edge == edge) { region_id = region->index; break; }
  }
  if (const auto *region = ExternalBoundary::GetIfValid(region_id)) edge = region->edge;
 }
 ExternalCity *City() const { return ExternalCity::GetIfValid(city_id); }
 bool CanEdit(Owner owner) const { return Company::IsValidID(_local_company) && (!Company::IsValidID(owner) || owner == _local_company); }
 void OnPaint() override {
  if (!ExternalBoundary::IsValidID(region_id)) region_id = ExternalBoundaryID::Invalid();
  const auto *region = ExternalBoundary::GetIfValid(region_id);
  const auto *city = City();
  this->SetWidgetDisabledState(EC_ECONOMY, city == nullptr);
  this->SetWidgetDisabledState(EC_VEHICLES, city == nullptr);
  this->SetWidgetDisabledState(EC_LOCATE, region == nullptr);
  this->SetWidgetLoweredState(EC_SHOW, show_boundaries);
  this->SetWidgetLoweredState(EC_BOUNDARY_TOGGLE, edit_boundaries);
  this->SetWidgetDisabledState(EC_BOUNDARY_TOGGLE, !CanEditBoundaries());
  this->SetWidgetDisabledState(EC_NEW, !CanEditBoundaries() || !Company::IsValidID(_local_company) || !ExternalCity::CanAllocateItem());
  this->SetWidgetDisabledState(EC_RENAME, city == nullptr || !CanEdit(city->owner));
  this->SetWidgetDisabledState(EC_TIME, city == nullptr || !CanEdit(city->owner));
  this->SetWidgetDisabledState(EC_STOP, city == nullptr || !CanEdit(city->owner));
  this->SetWidgetDisabledState(EC_DRAW, !CanEditBoundaries() || city == nullptr || !Company::IsValidID(_local_company) || (region != nullptr && !CanEdit(region->owner)));
  this->SetWidgetDisabledState(EC_DELETE, !CanEditBoundaries() || region == nullptr || !CanEdit(region->owner));
  this->DrawWidgets();
 }
 std::string RegionText(const ExternalBoundary *region) const {
  const auto *city = ExternalCity::GetIfValid(region->city);
  return GetString(edge_names[uint(region->edge)]) + " 路 " + GetString(STR_EXTERNAL_BOUNDARY_LABEL, region->first, region->last, city == nullptr ? std::string{} : std::string(city->name));
 }
 std::string GetWidgetString(WidgetID widget, StringID stringid) const override {
  const auto *city = City();
  if (widget == EC_CITY) return city == nullptr ? GetString(STR_EXTERNAL_CITY_UNASSIGNED) : GetString(STR_EXTERNAL_CITY_LINK, city->name);
  if (widget == EC_TIME) return GetString(STR_EXTERNAL_CITY_TIME, city == nullptr ? _settings_game.vehicle.external_trip_distance : city->distance_tiles);
  if (widget == EC_STOP) return GetString(STR_EXTERNAL_CITY_STOP, city == nullptr ? _settings_game.vehicle.external_stop_minutes : city->stop_minutes);
  if (widget == EC_BOUNDARY_TOGGLE) return GetString(edit_boundaries ? STR_EXTERNAL_BOUNDARY_COLLAPSE : STR_EXTERNAL_BOUNDARY_EDIT);
  if (widget == EC_REGION) { const auto *region = ExternalBoundary::GetIfValid(region_id); return region == nullptr ? GetString(STR_EXTERNAL_BOUNDARY_NEW) : RegionText(region); }
  if (widget == EC_HINT) return GetString(placing ? (first.has_value() ? STR_EXTERNAL_BOUNDARY_SECOND : STR_EXTERNAL_BOUNDARY_FIRST) : STR_EXTERNAL_BOUNDARY_HELP);
  return this->Window::GetWidgetString(widget, stringid);
 }
 void DrawWidget(const Rect &rect, WidgetID widget) const override {
  if (widget != EC_OVERVIEW) return;
  GfxFillRect(rect.Shrink(2), PC_BLACK);
  for (uint e = 0; e < 4; ++e) {
   auto side = DiagDirection(e); auto a = BoundaryOverviewPoint(rect, side, 0), b = BoundaryOverviewPoint(rect, side, GetExternalBoundaryLength(side));
   GfxDrawLine(a.x, a.y, b.x, b.y, PC_DARK_GREY, 5);
  }
  auto draw = [&](const ExternalBoundary *region) {
   const auto a = BoundaryOverviewPoint(rect, region->edge, region->first), b = BoundaryOverviewPoint(rect, region->edge, uint(region->last) + 1);
   if (region->index == region_id) GfxDrawLine(a.x, a.y, b.x, b.y, PC_WHITE, 9);
   GfxDrawLine(a.x, a.y, b.x, b.y, _colour_value[BoundaryColour(region->city)], 5);
  };
  for (const auto *region : ExternalBoundary::Iterate()) if (region->index != region_id) draw(region);
  if (const auto *region = ExternalBoundary::GetIfValid(region_id)) draw(region);
  DrawString(rect.left + 3, rect.right - 3, rect.top + 3, STR_EXTERNAL_BOUNDARY_OVERVIEW, TextColour::White, AlignmentH::Centre, false, FontSize::Small);
  const auto ne = BoundaryOverviewPoint(rect, DiagDirection::NE, GetExternalBoundaryLength(DiagDirection::NE) / 2);
  const auto se = BoundaryOverviewPoint(rect, DiagDirection::SE, GetExternalBoundaryLength(DiagDirection::SE) / 2);
  const auto nw = BoundaryOverviewPoint(rect, DiagDirection::NW, GetExternalBoundaryLength(DiagDirection::NW) / 2);
  const auto sw = BoundaryOverviewPoint(rect, DiagDirection::SW, GetExternalBoundaryLength(DiagDirection::SW) / 2);
  for (const auto &[p, e] : {std::pair{ne,0},std::pair{se,1},std::pair{sw,2},std::pair{nw,3}}) {
   DrawString(p.x - 50, p.x + 50, p.y + (e == 0 || e == 3 ? -19 : 8), edge_names[e], TextColour::White, AlignmentH::Centre, false, FontSize::Small);
  }
 }
 void SelectOverview(Point point) {
  const Rect rect = this->GetWidget<NWidgetBase>(EC_OVERVIEW)->GetCurrentRect();
  const ExternalBoundary *nearest = nullptr; double distance = 100;
  for (const auto *region : ExternalBoundary::Iterate()) {
   auto a = BoundaryOverviewPoint(rect, region->edge, region->first), b = BoundaryOverviewPoint(rect, region->edge, uint(region->last)+1);
   const double dx=b.x-a.x, dy=b.y-a.y, length=dx*dx+dy*dy;
   const double t=length==0?0:std::clamp(((point.x-a.x)*dx+(point.y-a.y)*dy)/length,0.0,1.0);
   const double x=point.x-a.x-t*dx, y=point.y-a.y-t*dy;
   if (x*x+y*y < distance) { distance=x*x+y*y; nearest=region; }
  }
  if (nearest == nullptr) return;
  if (placing) ResetObjectToPlace();
  region_id=nearest->index;city_id=nearest->city;edge=nearest->edge;
  MarkWholeScreenDirty();this->SetDirty();
  ScrollMainWindowToTile(BoundaryTile(edge,(uint(nearest->first)+nearest->last)/2),true);
 }
 void OnInvalidateData(int = 0, bool gui_scope = true) override {
  if (!gui_scope) return;
  if (!CanEditBoundaries()) {
   if (placing) ResetObjectToPlace();
   if (edit_boundaries) {
    edit_boundaries = false;
    this->GetWidget<NWidgetStacked>(EC_BOUNDARY_PLANE)->SetDisplayedPlane(SZSP_HORIZONTAL);
    this->ReInit();
   }
  }
  MarkWholeScreenDirty();
 }
 void Post(ExternalCityOperation op, ExternalCityID city, uint16_t minutes = 0, const std::string &name = {}, ExternalBoundaryID region = ExternalBoundaryID::Invalid(), DiagDirection side = DiagDirection::NE, uint16_t start = 0, uint16_t end = 0) {
  Command<Commands::ManageExternalCity>::Post(STR_EXTERNAL_CITY_ERROR, DepotID::Invalid(), city, op, minutes, name, region, side, start, end);
 }
 void OnClick(Point point, WidgetID widget, int) override {
  const auto *city = City();
  if (!CanEditBoundaries() && (widget == EC_NEW || widget == EC_DRAW || widget == EC_DELETE || widget == EC_BOUNDARY_TOGGLE)) return;
  switch (widget) {
   case EC_BOUNDARY_TOGGLE:
    if (placing) ResetObjectToPlace();
    edit_boundaries = !edit_boundaries;
    this->GetWidget<NWidgetStacked>(EC_BOUNDARY_PLANE)->SetDisplayedPlane(edit_boundaries ? 0 : SZSP_HORIZONTAL);
    this->ReInit(); break;
   case EC_VEHICLES: if (city != nullptr) ShowExternalCityVehicles(city_id); break;
   case EC_ECONOMY: if (city != nullptr) ShowExternalCityEconomyWindow(city_id); break;
   case EC_OVERVIEW: SelectOverview(point); break;
   case EC_SHOW: show_boundaries = !show_boundaries; external_city_names_visible = show_boundaries; MarkWholeScreenDirty(); break;
   case EC_LOCATE:
    if (const auto *region = ExternalBoundary::GetIfValid(region_id)) ScrollMainWindowToTile(BoundaryTile(region->edge, (uint(region->first) + region->last) / 2), true);
    break;
   case EC_CITY: {
    DropDownList list; list.push_back(MakeDropDownListStringItem(GetString(STR_EXTERNAL_CITY_UNASSIGNED), -1));
    for (const ExternalCity *item : ExternalCity::Iterate()) list.push_back(MakeDropDownListStringItem(std::string(item->name), item->index.base()));
    ShowDropDownList(this, std::move(list), city == nullptr ? -1 : city->index.base(), EC_CITY); break;
   }
   case EC_REGION: {
    DropDownList list; list.push_back(MakeDropDownListStringItem(GetString(STR_EXTERNAL_BOUNDARY_NEW), -1));
    for (const auto *item : ExternalBoundary::Iterate()) if (item->city == city_id) list.push_back(MakeDropDownListStringItem(RegionText(item), item->index.base()));
    ShowDropDownList(this, std::move(list), region_id == ExternalBoundaryID::Invalid() ? -1 : region_id.base(), EC_REGION); break;
   }
   case EC_DRAW:
    if (city == nullptr) return;
    ResetObjectToPlace();
    SetObjectToPlaceWnd(SPR_CURSOR_SIGN, PAL_NONE, HT_RECT, this);
    placing = true; first.reset(); draw_city = city_id; draw_region = region_id; draw_edge = edge;
    this->LowerWidget(EC_DRAW); this->SetDirty(); break;
   case EC_DELETE: Post(ExternalCityOperation::RemoveBoundary, city_id, 0, {}, region_id); break;
   case EC_NEW: query = ExternalCityOperation::Create; query_city = ExternalCityID::Invalid(); ShowQueryString({}, STR_EXTERNAL_CITY_NEW_TITLE, 64, this, CS_ALPHANUMERAL, {QueryStringFlag::LengthIsInChars}); break;
   case EC_RENAME:
    if (city == nullptr) return;
    query = ExternalCityOperation::Rename; query_city = city_id; ShowQueryString(city->name, STR_EXTERNAL_CITY_RENAME, 64, this, CS_ALPHANUMERAL, {QueryStringFlag::LengthIsInChars}); break;
   case EC_TIME:
    if (city == nullptr) return;
    query = ExternalCityOperation::SetDistance; query_city = city_id; ShowQueryString(std::to_string(city->distance_tiles), STR_EXTERNAL_CITY_TIME_TITLE, 5, this, CS_NUMERAL, {}); break;
   case EC_STOP:
    if (city == nullptr) return;
    query = ExternalCityOperation::SetStopTime; query_city = city_id; ShowQueryString(std::to_string(city->stop_minutes), STR_EXTERNAL_CITY_STOP_TITLE, 4, this, CS_NUMERAL, {}); break;
  }
 }
 void OnPlaceObject(Point, TileIndex tile) override {
  if (!placing || !CanEditBoundaries()) return;
  /* Infer the side from actual edge tiles. A corner belongs to two sides:
   * keep its tile until the second click resolves which side is intended. */
  for (DiagDirection side : {draw_edge, DiagDirection::NE, DiagDirection::SE, DiagDirection::SW, DiagDirection::NW}) {
   uint start = 0, end = 0;
   if (!GetExternalBoundaryCoordinate(tile, side, end)) continue;
   if (first.has_value() && !GetExternalBoundaryCoordinate(first_tile, side, start)) continue;
   draw_edge = side;
   if (!first.has_value()) { first_tile = tile; first = end; MarkWholeScreenDirty(); this->SetDirty(); return; }
   Post(ExternalCityOperation::SetBoundary, draw_city, 0, {}, draw_region, side, uint16_t(std::min(start, end)), uint16_t(std::max(start, end)));
   ResetObjectToPlace();
   return;
  }
 }
 void OnPlaceObjectAbort() override { placing = false; first.reset(); first_tile = INVALID_TILE; this->RaiseButtons(); MarkWholeScreenDirty(); this->SetDirty(); }
 void OnDropdownSelect(WidgetID widget, int result, int) override {
  if (placing) ResetObjectToPlace();
  if (widget == EC_CITY) {
   city_id = result < 0 ? ExternalCityID::Invalid() : ExternalCityID{uint16_t(result)};
   this->SelectCityBoundary();
   if (CanEditBoundaries() && region_id == ExternalBoundaryID::Invalid() && !edit_boundaries) {
    edit_boundaries = true;
    this->GetWidget<NWidgetStacked>(EC_BOUNDARY_PLANE)->SetDisplayedPlane(0);
    this->ReInit();
   }
  }
  if (widget == EC_REGION) {
   region_id = result < 0 ? ExternalBoundaryID::Invalid() : ExternalBoundaryID{uint16_t(result)};
   if (const auto *region = ExternalBoundary::GetIfValid(region_id)) { city_id = region->city; edge = region->edge; }
  }
  MarkWholeScreenDirty();
  this->SetDirty();
 }
 void OnQueryTextFinished(std::optional<std::string> text) override {
  if (!text.has_value() || text->empty()) return;
  uint16_t minutes = _settings_game.vehicle.external_trip_minutes;
  if (query == ExternalCityOperation::SetDistance || query == ExternalCityOperation::SetStopTime) {
   unsigned value = 0; const auto result = std::from_chars(text->data(), text->data() + text->size(), value);
   if (result.ec != std::errc{} || result.ptr != text->data() + text->size() || value > (query == ExternalCityOperation::SetDistance ? 65535U : 1440U) || (query == ExternalCityOperation::SetDistance && value == 0)) return;
   minutes = uint16_t(value);
  }
  Post(query, query_city, minutes, query == ExternalCityOperation::Create || query == ExternalCityOperation::Rename ? *text : std::string{});
 }
};
void ShowExternalCityWindow(DepotID portal)
{
 if (BringWindowToFrontById(WindowClass::ExternalCity, 0) == nullptr) new ExternalCityWindow(portal);
}
struct ExternalCityLabel { Rect rect; std::string text; bool selected; PixelColour colour; };
struct ExternalCityLabelMark {
	Viewport *vp;
	int left, top, right, bottom;
};
static std::vector<ExternalCityLabelMark> _external_city_label_marks;

void MarkExternalCityLabelsDirty(Viewport *vp)
{
	if (vp == nullptr) return;
	for (const auto &mark : _external_city_label_marks) {
		if (mark.vp != vp) continue;
		MarkViewportDirty(vp, mark.left, mark.top, mark.right, mark.bottom, VMDF_NOT_LANDSCAPE);
	}
}

static void ClearExternalCityLabelMarks(const Viewport *vp)
{
	std::erase_if(_external_city_label_marks, [vp](const auto &mark) { return mark.vp == vp; });
}

static void AddExternalCityLabelMark(const Viewport *vp, const Rect &rect)
{
	const int view_left = UnScaleByZoom(vp->virtual_left, vp->zoom);
	const int view_top = UnScaleByZoom(vp->virtual_top, vp->zoom);
	const int pad = ScaleByZoom(4, vp->zoom) + 1;
	const int left = ScaleByZoom(rect.left - view_left, vp->zoom) + vp->virtual_left - pad;
	const int top = ScaleByZoom(rect.top - view_top, vp->zoom) + vp->virtual_top - pad;
	const int right = ScaleByZoom(rect.right - view_left, vp->zoom) + vp->virtual_left + pad;
	const int bottom = ScaleByZoom(rect.bottom - view_top, vp->zoom) + vp->virtual_top + pad;
	_external_city_label_marks.push_back({const_cast<Viewport *>(vp), left, top, right, bottom});
}
static std::optional<ExternalCityLabel> GetExternalCityLabel(const Viewport *vp, const ExternalBoundary *region)
{
 if (IsInvisibilitySet(TransparencyOption::Signs)) return std::nullopt;
 auto *window = dynamic_cast<ExternalCityWindow *>(FindWindowById(WindowClass::ExternalCity, 0));
 const bool detailed = window != nullptr && window->show_boundaries;
 const int view_left = UnScaleByZoom(vp->virtual_left, vp->zoom);
 const int view_right = view_left + vp->width;
 const int view_top = UnScaleByZoom(vp->virtual_top, vp->zoom);
 const int view_bottom = view_top + vp->height;
 auto project = [&](DiagDirection edge, uint coordinate) {
  const TileIndex tile = BoundaryTile(edge, coordinate);
  Point point = RemapCoords2(TileX(tile)*TILE_SIZE+TILE_SIZE/2, TileY(tile)*TILE_SIZE+TILE_SIZE/2);
  return Point{UnScaleByZoom(point.x, vp->zoom), UnScaleByZoom(point.y, vp->zoom)};
 };
  const auto *city = ExternalCity::GetIfValid(region->city);
  if (city == nullptr || region->edge >= DiagDirection::End) return std::nullopt;
  const uint low = _settings_game.construction.freeform_edges ? 1 : 0;
  const uint limit = GetExternalBoundaryLength(region->edge)-2;
  uint first=std::max<uint>(region->first,low), last=std::min<uint>(region->last,limit);
  if (first>last) return std::nullopt;
  const TileIndex origin = BoundaryTile(region->edge,low);
  const int origin_x=RemapCoords(TileX(origin)*TILE_SIZE+TILE_SIZE/2,TileY(origin)*TILE_SIZE+TILE_SIZE/2,0).x;
  const int step=(region->edge==DiagDirection::NE||region->edge==DiagDirection::SW?1:-1)*2*TILE_SIZE*ZOOM_BASE;
  const int a=(vp->virtual_left-origin_x)/step+int(low), b=(vp->virtual_left+vp->virtual_width-origin_x)/step+int(low);
  if (std::max(a,b)+2<int(first) || std::min(a,b)-2>int(last)) return std::nullopt;
  const uint visible_first=uint(std::max<int>(first,std::min(a,b)-2));
  const uint visible_last=uint(std::max(0,std::min<int>(last,std::max(a,b)+2)));
  if (visible_first>visible_last) return std::nullopt;
  const bool selected=detailed && region->index==window->region_id;
  const Point middle=project(region->edge,(visible_first+visible_last)/2);
  if (middle.y<view_top-30||middle.y>view_bottom+30) return std::nullopt;
  const std::string text=selected?GetString(STR_EXTERNAL_BOUNDARY_LABEL,region->first,region->last,city->name):std::string(city->name);
  const Dimension size=GetStringBoundingBox(text,FontSize::Small);
  const int width=std::min<int>(size.width+10,std::max(1,vp->width-8));
  const int x=std::clamp(middle.x-width/2,view_left+3,std::max(view_left+3,view_right-width-3));
  int label_y = middle.y - 18;
  if (GetExternalFlightGateway(*region) != INVALID_TILE && !IsInvisibilitySet(TransparencyOption::Structures)) {
   const auto beacon = FlightBeaconPosition(*region);
   Point top = RemapCoords(beacon.x, beacon.y, beacon.z);
   top.y += GetSprite(SPR_AIRPORT_RADAR_1, SpriteType::Normal, LowZoomMask(vp->zoom))->y_offs;
   const int beacon_x = UnScaleByZoom(top.x, vp->zoom);
   if (beacon_x >= x - 20 && beacon_x <= x + width + 20) label_y = std::min(label_y, UnScaleByZoom(top.y, vp->zoom) - int(size.height) - 7);
  }
  const int y=std::clamp(label_y,view_top+3,std::max(view_top+3,view_bottom-int(size.height)-7));
  return ExternalCityLabel{{x-1,y-1,x+width,y+int(size.height)+4},text,selected,PC_BLACK};
}
std::string GetExternalConnectionCityName(TileIndex tile, DiagDirection inward)
{
 uint coordinate;
 if (IsValidDiagDirection(inward) && GetExternalBoundaryCoordinate(tile, ReverseDiagDir(inward), coordinate)) {
  const auto *city = ExternalCity::GetIfValid(GetExternalCityAtBoundary(ReverseDiagDir(inward), coordinate));
  if (city != nullptr) return std::string(city->name);
 }
 return GetString(STR_EXTERNAL_CITY_BOUNDARY_UNASSIGNED);
}
std::string GetExternalCityTooltipAt(const Viewport *vp, Point point)
{
 const auto *beacon = FlightBeaconAt(vp, point);
 auto beacon_tooltip = [&]() { return beacon == nullptr ? std::string{} : GetString(STR_EXTERNAL_FLIGHT_BEACON, std::string(ExternalCity::Get(beacon->city)->name)); };
 if (!external_city_names_visible || !_settings_game.vehicle.external_connections) return beacon_tooltip();
 point.x += UnScaleByZoom(vp->virtual_left, vp->zoom) - vp->left;
 point.y += UnScaleByZoom(vp->virtual_top, vp->zoom) - vp->top;
 for (const auto *region : ExternalBoundary::Iterate()) {
  const auto label = GetExternalCityLabel(vp, region);
  if (!label || !label->rect.Contains(point)) continue;
  const auto *city = ExternalCity::GetIfValid(region->city);
  if (city == nullptr) continue;
  const auto profile = GetExternalCityEconomy(city->index);
  std::string text = std::string(city->name);
  auto append = [&](StringID title, CargoTypes cargoes, bool supply) {
   text += "\n" + GetString(title);
   if (cargoes.None()) text += " " + GetString(STR_JUST_NOTHING);
   for (CargoType cargo : cargoes) {
    text += "\n" + GetString(CargoSpec::Get(cargo)->name);
    if (supply) text += ": " + GetString(STR_EXTERNAL_MARKET_STOCK, cargo, city->stock[cargo]);
   }
  };
  append(STR_EXTERNAL_ECONOMY_PRODUCTS, profile.produced, true);
  append(STR_EXTERNAL_ECONOMY_REQUIRES, profile.accepted, false);
  return text;
 }
 return beacon_tooltip();
}

void DrawExternalCityBoundaries(const Viewport *vp, DrawPixelInfo *dpi)
{
	ClearExternalCityLabelMarks(vp);
	/* A map pick remains visible even if the city's name overlay is hidden. */
	if (external_pick_origin != INVALID_TILE) {
		AutoRestoreBackup current_dpi(_cur_dpi, dpi);
		for (const Depot *portal : Depot::Iterate()) {
			if (!IsExternalReturnHighlighted(portal->xy)) continue;
			Point point=RemapCoords2(TileX(portal->xy)*TILE_SIZE+TILE_SIZE/2,TileY(portal->xy)*TILE_SIZE+TILE_SIZE/2);
			point.x=UnScaleByZoom(point.x,vp->zoom); point.y=UnScaleByZoom(point.y,vp->zoom);
			if(point.x<UnScaleByZoom(vp->virtual_left,vp->zoom)||point.x>UnScaleByZoom(vp->virtual_left,vp->zoom)+vp->width ||
			   point.y<UnScaleByZoom(vp->virtual_top,vp->zoom)||point.y>UnScaleByZoom(vp->virtual_top,vp->zoom)+vp->height)continue;
			const auto *city=ExternalCity::GetIfValid(GetExternalCityForEntrance(portal->xy));
			const auto text=GetString(STR_EXTERNAL_RETURN_ITEM,city==nullptr?std::string{}:std::string(city->name),TileX(portal->xy),TileY(portal->xy));
			const Dimension size=GetStringBoundingBox(text,FontSize::Small);
			const int x=point.x-int(size.width)/2,y=point.y-20;
			GfxFillRect(x-4,y-3,x+size.width+4,y+size.height+3,PC_WHITE);
			GfxFillRect(x-2,y-1,x+size.width+2,y+size.height+1,PC_BLACK);
			DrawString(x,x+size.width,y,text,TextColour::White,AlignmentH::Centre,false,FontSize::Small);
			AddExternalCityLabelMark(vp, {x-4,y-3,x+int(size.width)+4,y+int(size.height)+3});
		}
	}
	auto *window = dynamic_cast<ExternalCityWindow *>(FindWindowById(WindowClass::ExternalCity, 0));
	if (!external_city_names_visible || ExternalBoundary::GetNumItems()==0) return;
	const bool detailed = window != nullptr && window->show_boundaries;
	/* This hook runs in the viewport's main-thread finishing phase. Selection is
	 * client-local and never writes the shared boundary registry or route state. */
	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	AutoRestoreBackup current_dpi(_cur_dpi, dpi);
	const int view_top = UnScaleByZoom(vp->virtual_top, vp->zoom);
	const int view_bottom = view_top + vp->height;
	auto project = [&](DiagDirection edge, uint coordinate) {
		const TileIndex tile = BoundaryTile(edge, coordinate);
		Point point = RemapCoords2(TileX(tile)*TILE_SIZE+TILE_SIZE/2, TileY(tile)*TILE_SIZE+TILE_SIZE/2);
		return Point{UnScaleByZoom(point.x, vp->zoom), UnScaleByZoom(point.y, vp->zoom)};
	};
	std::vector<ExternalCityLabel> labels;
	auto collect = [&](const ExternalBoundary *region) {
		const auto *city = ExternalCity::GetIfValid(region->city);
		if (city == nullptr || region->edge >= DiagDirection::End) return;
		const uint low = _settings_game.construction.freeform_edges ? 1 : 0;
		const uint limit = GetExternalBoundaryLength(region->edge)-2;
		uint first=std::max<uint>(region->first,low), last=std::min<uint>(region->last,limit);
		if (first>last) return;
		const TileIndex origin = BoundaryTile(region->edge,low);
		const int origin_x=RemapCoords(TileX(origin)*TILE_SIZE+TILE_SIZE/2,TileY(origin)*TILE_SIZE+TILE_SIZE/2,0).x;
		const int step=(region->edge==DiagDirection::NE||region->edge==DiagDirection::SW?1:-1)*2*TILE_SIZE*ZOOM_BASE;
		const int a=(vp->virtual_left-origin_x)/step+int(low), b=(vp->virtual_left+vp->virtual_width-origin_x)/step+int(low);
		if (std::max(a,b)+2<int(first) || std::min(a,b)-2>int(last)) return;
		const uint visible_first=uint(std::max<int>(first,std::min(a,b)-2));
		const uint visible_last=uint(std::max(0,std::min<int>(last,std::max(a,b)+2)));
		if (visible_first>visible_last) return;
		const bool selected=detailed && region->index==window->region_id;
		const PixelColour colour=_colour_value[BoundaryColour(region->city)];
		if (detailed) {
			Point previous=project(region->edge,visible_first);
			if (visible_first==visible_last) GfxFillRect(blitter,dpi,previous.x-2,previous.y-2,previous.x+2,previous.y+2,colour);
			for (uint coordinate=visible_first+1;coordinate<=visible_last;++coordinate) {
				const Point next=project(region->edge,coordinate);
				GfxDrawLine(blitter,dpi,previous.x,previous.y,next.x,next.y,selected?PC_WHITE:PC_BLACK,selected?7:5);
				GfxDrawLine(blitter,dpi,previous.x,previous.y,next.x,next.y,colour,3);
				previous=next;
			}
			for (uint coordinate:{first,last}) {
				const Point point=project(region->edge,coordinate);
				GfxFillRect(blitter,dpi,point.x-4,point.y-4,point.x+4,point.y+4,selected?PC_WHITE:PC_BLACK);
				GfxFillRect(blitter,dpi,point.x-2,point.y-2,point.x+2,point.y+2,colour);
			}
		}
		if (auto label = GetExternalCityLabel(vp, region)) {
			label->colour = colour;
			labels.push_back(*label);
		}
	};
	for (const auto *region : ExternalBoundary::Iterate()) collect(region);
	/* Keep the selected boundary's label on top of the others. */
	std::stable_sort(labels.begin(), labels.end(), [](const ExternalCityLabel &a, const ExternalCityLabel &b) { return a.selected < b.selected; });
	/* Label rectangles can meet at a map corner. Nudge later labels down instead
	 * of drawing them on top of each other. */
	std::vector<Rect> placed;
	for (auto &label : labels) {
		const int height = label.rect.bottom - label.rect.top;
		for (uint attempt = 0; attempt < 4; ++attempt) {
			bool clash = false;
			for (const Rect &other : placed) {
				if (label.rect.left < other.right + 2 && other.left < label.rect.right + 2 &&
						label.rect.top < other.bottom + 2 && other.top < label.rect.bottom + 2) {
					clash = true;
					break;
				}
			}
			if (!clash) break;
			int top = label.rect.bottom + 2;
			if (top + height > view_bottom - 3) top = view_top + 3;
			label.rect.top = top;
			label.rect.bottom = top + height;
		}
		placed.push_back(label.rect);
	}
	for (const auto &label : labels) {
		const int x = label.rect.left + 1, y = label.rect.top + 1;
		const int width = label.rect.right - x;
		const auto &text = label.text;
		const Dimension size = GetStringBoundingBox(text, FontSize::Small);
		if (!IsTransparencySet(TransparencyOption::Signs)) {
			GfxFillRect(blitter,dpi,x-1,y-1,x+width,y+size.height+4,label.selected?PC_WHITE:label.colour);
			GfxFillRect(blitter,dpi,x,y,x+width-1,y+size.height+3,PC_BLACK);
		}
		DrawString(x+4,x+width-4,y+2,text,TextColour::White,AlignmentH::Centre,false,FontSize::Small);
		AddExternalCityLabelMark(vp, label.rect);
	}
	if (window!=nullptr && window->placing&&window->first.has_value()) {
		const Point point=project(window->draw_edge,*window->first);
		GfxFillRect(blitter,dpi,point.x-6,point.y-6,point.x+6,point.y+6,PC_WHITE);
		GfxFillRect(blitter,dpi,point.x-3,point.y-3,point.x+3,point.y+3,_colour_value[BoundaryColour(window->draw_city)]);
	}
}


/* Dedicated entrance menu, with a live list of actual off-map journeys. */
enum ExternalEntranceWidgets : WidgetID { EE_CAPTION, EE_POSITION, EE_TIME, EE_COUNT, EE_LOCATE, EE_CITIES, EE_HELP, EE_LIST, EE_SCROLL, EE_HSCROLL, EE_ECONOMY };
static constexpr NWidgetPart _external_entrance_widgets[] = {
 NWidget(NWID_HORIZONTAL),
  NWidget(WWT_CLOSEBOX,Colours::Grey),
  NWidget(WWT_CAPTION,Colours::Grey,EE_CAPTION),SetStringTip(STR_EXTERNAL_ENTRANCE_CAPTION),
  NWidget(WWT_RESIZEBOX,Colours::Grey),
 EndContainer(),
 NWidget(WWT_PANEL,Colours::Grey),
  NWidget(NWID_VERTICAL),SetPadding(WidgetDimensions::unscaled.framerect),SetPIP(0,5,0),
   NWidget(WWT_TEXT,Colours::Grey,EE_POSITION),SetMinimalSize(560,14),SetFill(1,0),SetResize(1,0),SetStringTip(STR_EXTERNAL_ENTRANCE_POSITION),
   NWidget(WWT_TEXT,Colours::Grey,EE_TIME),SetMinimalSize(560,14),SetFill(1,0),SetResize(1,0),SetStringTip(STR_EXTERNAL_CITY_TIME),
   NWidget(WWT_TEXT,Colours::Grey,EE_COUNT),SetMinimalSize(560,14),SetFill(1,0),SetResize(1,0),SetStringTip(STR_EXTERNAL_ENTRANCE_COUNT),
   NWidget(NWID_HORIZONTAL,NWidContainerFlag::EqualSize),
    NWidget(WWT_PUSHTXTBTN,Colours::Grey,EE_LOCATE),SetMinimalSize(180,14),SetFill(1,0),SetResize(1,0),SetStringTip(STR_EXTERNAL_ENTRANCE_LOCATE),
    NWidget(WWT_PUSHTXTBTN,Colours::Grey,EE_CITIES),SetMinimalSize(180,14),SetFill(1,0),SetResize(1,0),SetStringTip(STR_EXTERNAL_CITY_TITLE),
    NWidget(WWT_PUSHTXTBTN,Colours::Grey,EE_ECONOMY),SetMinimalSize(180,14),SetFill(1,0),SetResize(1,0),SetStringTip(STR_EXTERNAL_ECONOMY_OPEN),
   EndContainer(),
  EndContainer(),
 EndContainer(),
 NWidget(NWID_HORIZONTAL),
  NWidget(WWT_MATRIX,Colours::Grey,EE_LIST),SetMinimalSize(560,200),SetFill(1,1),SetResize(1,1),SetScrollbar(EE_SCROLL),SetToolTip(STR_EXTERNAL_ENTRANCE_LIST_HELP),
  NWidget(NWID_VSCROLLBAR,Colours::Grey,EE_SCROLL),
 EndContainer(),
 NWidget(NWID_HSCROLLBAR,Colours::Grey,EE_HSCROLL),
};
static WindowDesc _external_entrance_desc(__FILE__,__LINE__,WindowPosition::Center,nullptr,590,350,
 WindowClass::ExternalEntrance,WindowClass::None,WindowDefaultFlag::Construction,_external_entrance_widgets);

static TileIndex ExternalEffectiveReturn(const Vehicle *train)
{
 if (!train->IsInExternalTrip()) return train->tile;
 const Depot *exit=Depot::GetIfValid(train->external_exit_depot);
 if (exit != nullptr && exit->xy==train->external_exit_tile && Depot::IsExternalConnection(exit->xy) &&
     IsInfraTileUsageAllowed(train->type, train->owner, exit->xy) && ((train->type == VehicleType::Train && IsRailDepotTile(exit->xy) && Train::From(train)->compatible_railtypes.Test(GetRailType(exit->xy))) ||
     (train->type == VehicleType::Road && IsRoadDepotTile(exit->xy) && (RoadVehicle::From(train)->compatible_roadtypes & GetPresentRoadTypes(exit->xy)).Any()) ||
     (train->type == VehicleType::Ship && IsShipDepotTile(exit->xy)))) return exit->xy;
 return train->tile;
}
std::string GetExternalTripStatus(const Vehicle *vehicle)
{
 const auto *v = vehicle->Primary();
 const auto *city = ExternalCity::GetIfValid(v->external_trip_city);
 const auto name = city == nullptr ? GetString(STR_EXTERNAL_UNASSIGNED_CITY) : std::string(city->name);
 StringID state = STR_EXTERNAL_PHASE_LEGACY;
 switch (v->GetExternalTripPhase()) {
  case ExternalTripPhase::Outbound: state = STR_EXTERNAL_PHASE_OUTBOUND; break;
  case ExternalTripPhase::Stopped: state = STR_EXTERNAL_PHASE_STOP; break;
  case ExternalTripPhase::Returning: state = STR_EXTERNAL_PHASE_RETURN; break;
  case ExternalTripPhase::Queued: state = STR_EXTERNAL_PHASE_QUEUED; break;
  default: break;
 }
 return GetString(STR_EXTERNAL_STATUS_SPEED, GetString(state, name), v->GetExternalDisplaySpeed());
}
struct ExternalEntranceWindow : Window {
 ExternalCityID city_list = ExternalCityID::Invalid();
 bool CityMode() const { return city_list != ExternalCityID::Invalid(); }
 ExternalCityID CityID() const { return CityMode() ? city_list : GetExternalCityForEntrance(Tile()); }
 bool Valid() const { return CityMode() ? ExternalCity::IsValidID(city_list) : Depot::IsExternalConnection(Tile()); }
 bool Matches(const Vehicle *v) const { return CityMode() ? v->external_trip_city == city_list : (v->external_entry_tile == Tile() || ExternalEffectiveReturn(v) == Tile()); }
 ExternalEntranceWindow(ExternalCityID city):Window(_external_entrance_desc),city_list(city) { this->InitNested(0x80000000U + city.base());scroll=this->GetScrollbar(EE_SCROLL);hscroll=this->GetScrollbar(EE_HSCROLL);this->OnResize();this->RefreshCount(); }
 uint count=0;
 int64_t counted_at=-1,shown_second=-1;
 std::vector<VehicleID> trains;
 Scrollbar *scroll=nullptr;
 Scrollbar *hscroll=nullptr;
 ExternalEntranceWindow(TileIndex tile):Window(_external_entrance_desc){this->InitNested(tile.base());scroll=this->GetScrollbar(EE_SCROLL);hscroll=this->GetScrollbar(EE_HSCROLL);this->OnResize();this->RefreshCount();}
 TileIndex Tile() const { return TileIndex{this->window_number}; }
 int ImageHeight() const { return ScaleGUITrad(std::max({GetVehicleHeight(VehicleType::Train),GetVehicleHeight(VehicleType::Road),GetVehicleHeight(VehicleType::Ship),GetVehicleHeight(VehicleType::Aircraft)})); }
 int RowHeight() const { return 4*GetCharacterHeight(FontSize::Normal)+ImageHeight()+WidgetDimensions::scaled.matrix.Vertical()+ScaleGUITrad(10); }
 void RefreshCount(){
  trains.clear();
  int image_width=0;
  for(const Vehicle *train:Vehicle::Iterate()) {
   /* The physical chain head can also carry a primary subtype. Only its
    * schedule owner has an independent journey and a return deadline. */
   if(!train->IsPrimaryVehicle() || train->Primary() != train || (!train->IsInExternalTrip()&&!train->external_return_pending)) continue;
   if(Matches(train)){
    trains.push_back(train->index);
    image_width=std::max(image_width,GetVehicleWidth(train->First(),EngineImageType::InDetails));
   }
  }
  std::sort(trains.begin(),trains.end(),[](VehicleID a,VehicleID b){
   const Vehicle *left=Vehicle::Get(a),*right=Vehicle::Get(b);
   if(left->external_return_pending!=right->external_return_pending)return left->external_return_pending;
   if(left->external_return_tick!=right->external_return_tick)return left->external_return_tick<right->external_return_tick;
   return a<b;
  });
  count=trains.size();scroll->SetCount(count);hscroll->SetCount(image_width);counted_at=_state_ticks.base()/32;this->SetDirty();
 }
 void OnGameTick() override {
  if(counted_at!=_state_ticks.base()/32)this->RefreshCount();
  const int64_t second=_state_ticks.base()*60/std::max<uint>(1,_settings_game.game_time.ticks_per_minute);
  if(shown_second!=second){shown_second=second;this->SetWidgetDirty(EE_LIST);}
 }
 void OnInvalidateData(int=0,bool gui_scope=true) override { if(gui_scope&&scroll!=nullptr)this->RefreshCount(); }
 void OnPaint() override {
  if(!Valid()){this->Close();return;}
  this->SetWidgetDisabledState(EE_LOCATE, CityMode());
  this->SetWidgetDisabledState(EE_ECONOMY, !ExternalCity::IsValidID(CityID()));
  this->DrawWidgets();
 }
 void UpdateWidgetSize(WidgetID widget,Dimension &size,const Dimension &,Dimension &fill,Dimension &resize) override {
  if(widget==EE_LIST){fill.height=resize.height=RowHeight();size.height=4*RowHeight();}
 }
 void OnResize() override {
  if(scroll==nullptr)return;
  scroll->SetCapacityFromWidget(this,EE_LIST);
  if(hscroll!=nullptr)hscroll->SetCapacity(this->GetWidget<NWidgetBase>(EE_LIST)->GetCurrentRect().Shrink(WidgetDimensions::scaled.matrix).Width());
  this->GetWidget<NWidgetCore>(EE_LIST)->SetMatrixDimension(1,scroll->GetCapacity());
 }
 std::string GetWidgetString(WidgetID widget,StringID string) const override {
  const auto *city=ExternalCity::GetIfValid(CityID());
  if (CityMode() && widget == EE_CAPTION) return GetString(STR_EXTERNAL_CITY_VEHICLES_CAPTION, city == nullptr ? std::string{} : std::string(city->name));
  if (CityMode() && widget == EE_POSITION) return GetString(STR_EXTERNAL_CITY_VEHICLES);
  if(widget==EE_CAPTION)return GetString(STR_EXTERNAL_ENTRANCE_CAPTION,city==nullptr?GetString(STR_EXTERNAL_UNASSIGNED_CITY):std::string(city->name));
  if(widget==EE_TIME)return GetString(STR_EXTERNAL_ENTRANCE_JOURNEY,city==nullptr?_settings_game.vehicle.external_trip_distance:city->distance_tiles,city==nullptr?_settings_game.vehicle.external_stop_minutes:city->stop_minutes);
  if(widget==EE_POSITION&&Depot::IsExternalConnection(Tile()))return GetString(STR_EXTERNAL_ENTRANCE_POSITION,edge_names[uint(ReverseDiagDir(Depot::GetExternalDirection(Tile())))],TileX(Tile()),TileY(Tile()));
  if(widget==EE_COUNT)return GetString(STR_EXTERNAL_ENTRANCE_COUNT,count);
  return Window::GetWidgetString(widget,string);
 }
 void DrawWidget(const Rect &rect,WidgetID widget) const override {
  if(widget!=EE_LIST)return;
  const int line=GetCharacterHeight(FontSize::Normal);
  Rect row=rect.WithHeight(RowHeight()).Shrink(WidgetDimensions::scaled.matrix);
  if(trains.empty()){DrawString(row.left,row.right,row.top,STR_EXTERNAL_ENTRANCE_LIST_EMPTY,TextColour::Black);return;}
  for(size_t i=scroll->GetPosition();i<std::min<size_t>(trains.size(),scroll->GetPosition()+scroll->GetCapacity());++i,row=row.Translate(0,RowHeight())){
   const Vehicle *train=Vehicle::GetIfValid(trains[i]);
   if(train==nullptr||(!train->IsInExternalTrip()&&!train->external_return_pending))continue;
   const auto number=train->GetActiveServiceNumber();
   const auto name=number.empty()?GetString(STR_VEHICLE_NAME,train->index):number;
   const uint64_t ticks=train->IsInExternalTrip()?std::max<int64_t>(0,(train->external_return_tick-_state_ticks).base()):0;
   const uint64_t seconds=CeilDiv(ticks*60,uint64_t(std::max<uint>(1,_settings_game.game_time.ticks_per_minute)));
   const std::string time=train->external_return_pending?GetString(STR_EXTERNAL_LIST_QUEUED):GetString(STR_EXTERNAL_LIST_TIME,seconds/60,seconds%60);
   const int time_width=GetStringBoundingBox(time).width+12;
   DrawString(row.left,row.right-time_width,row.top,GetString(STR_EXTERNAL_LIST_IDENTITY,name,GetString(STR_COMPANY_NAME,train->owner)),TextColour::Black);
   DrawString(row.right-time_width,row.right,row.top,time,TextColour::Black,AlignmentH::End);
   const TileIndex entry=train->external_entry_tile==INVALID_TILE?train->tile:train->external_entry_tile;
   DrawString(row.left,row.right,row.top+line,GetExternalTripStatus(train),TextColour::Black);
   const TileIndex destination = ExternalEffectiveReturn(train);
   if (train->type == VehicleType::Aircraft) {
    const auto *plane = Aircraft::From(train);
    const auto airport = Station::IsValidID(plane->external_return_airport) ? plane->external_return_airport : plane->external_home_airport;
    DrawString(row.left,row.right,row.top+2*line,Station::IsValidID(airport) ? GetString(STR_EXTERNAL_FLIGHT_RETURN_SUFFIX,airport) : GetString(STR_EXTERNAL_FLIGHT_INVALID_AIRPORT),TextColour::Black);
   } else DrawString(row.left,row.right,row.top+2*line,GetString(entry == destination ? STR_EXTERNAL_LIST_SAME_PORT : STR_EXTERNAL_LIST_PORT_PAIR,TileX(entry),TileY(entry),TileX(destination),TileY(destination)),TextColour::Black);
   const bool invalid=train->IsInExternalTrip()&&train->external_exit_depot!=DepotID::Invalid()&&ExternalEffectiveReturn(train)!=train->external_exit_tile;
   const Order *next=train->GetOrder(train->cur_real_order_index);
   if(train->IsInExternalTrip()&&!train->external_departure_handled&&next!=nullptr)next=train->orders->GetNext(next);
   std::string next_text=GetString(STR_EXTERNAL_LIST_NEXT_UNKNOWN);
   if(next!=nullptr&&next->IsType(OT_GOTO_STATION))next_text=GetString(STR_STATION_NAME,next->GetDestination().ToStationID());
   if(next!=nullptr&&next->IsType(OT_GOTO_WAYPOINT))next_text=GetString(STR_WAYPOINT_NAME,next->GetDestination().ToStationID());
   DrawString(row.left,row.right,row.top+3*line,invalid?GetString(STR_EXTERNAL_LIST_CHANGED):GetString(STR_EXTERNAL_LIST_NEXT,next_text),TextColour::Black);
   /* Use the real physical chain, including units before a mid-chain primary.
    * Drawing stays client-local; hidden off-map vehicles remain untouched. */
   const Rect image=row.WithHeight(ImageHeight()).Translate(0,4*line+ScaleGUITrad(4));
   DrawVehicleImage(train->First(),image,VehicleID::Invalid(),EngineImageType::InDetails,hscroll->GetPosition());
  }
 }
 void OnClick(Point point,WidgetID widget,int) override {
  if(!Valid())return;
  if(widget==EE_LOCATE && !CityMode())ScrollMainWindowToTile(Tile(),true);
  if(widget==EE_CITIES)ShowExternalCityWindow(CityMode() ? DepotID::Invalid() : Depot::GetByTile(Tile())->index);
  if(widget==EE_ECONOMY)ShowExternalCityEconomyWindow(CityID());
  if(widget==EE_LIST){
   const auto row=scroll->GetScrolledRowFromWidget(point.y,this,EE_LIST);
   if(row>=0&&size_t(row)<trains.size())if(const Vehicle *train=Vehicle::GetIfValid(trains[row])) {
    if((train->IsInExternalTrip()||train->external_return_pending)&&Matches(train))ShowVehicleViewWindow(train);
   }
  }
 }
};
static void ShowExternalCityVehicles(ExternalCityID city)
{
 if (ExternalCity::IsValidID(city) && BringWindowToFrontById(WindowClass::ExternalEntrance, 0x80000000U + city.base()) == nullptr) new ExternalEntranceWindow(city);
}
void ShowExternalEntranceWindow(TileIndex tile)
{
 if(!Depot::IsExternalConnection(tile))return;
 if(IsShipDepotTile(tile))tile=GetShipDepotNorthTile(tile);
 if(BringWindowToFrontById(WindowClass::ExternalEntrance,tile.base())==nullptr)new ExternalEntranceWindow(tile);
}

enum ExternalEconomyWidgets : WidgetID { XE_CAPTION, XE_CITY, XE_INDUSTRIES, XE_CARGOES, XE_LIST, XE_SCROLL, XE_NOTE, XE_SELECTION, XE_SELECT_PLANE, XE_DETAILS };
static constexpr NWidgetPart _external_economy_widgets[] = {
 NWidget(NWID_HORIZONTAL),
  NWidget(WWT_CLOSEBOX, Colours::Cream),
  NWidget(WWT_CAPTION, Colours::Cream, XE_CAPTION), SetStringTip(STR_EXTERNAL_ECONOMY_CAPTION),
  NWidget(WWT_SHADEBOX, Colours::Cream),
  NWidget(WWT_STICKYBOX, Colours::Cream),
 EndContainer(),
 NWidget(WWT_PANEL, Colours::Cream),
  NWidget(NWID_VERTICAL), SetPadding(WidgetDimensions::unscaled.framerect), SetPIP(0, 4, 0),
   NWidget(WWT_DROPDOWN, Colours::Cream, XE_CITY), SetMinimalSize(420, 14), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_EXTERNAL_CITY_UNASSIGNED),
   NWidget(NWID_SELECTION, Colours::Invalid, XE_SELECT_PLANE),
    NWidget(WWT_DROPDOWN, Colours::Cream, XE_SELECTION), SetMinimalSize(420, 14), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_EXTERNAL_ECONOMY_SELECT),
   EndContainer(),
  EndContainer(),
 EndContainer(),
 NWidget(NWID_HORIZONTAL),
  NWidget(WWT_PANEL, Colours::Cream, XE_LIST), SetMinimalSize(420, 200), SetFill(1, 1), SetResize(1, 1), SetScrollbar(XE_SCROLL), EndContainer(),
  NWidget(NWID_VSCROLLBAR, Colours::Cream, XE_SCROLL),
 EndContainer(),
 /* This must paint an opaque background on every redraw. An EMPTY widget
  * leaves old viewport/text pixels behind when the window moves or resizes. */
 NWidget(WWT_PANEL, Colours::Cream, XE_NOTE), SetFill(1, 0), SetResize(1, 0), EndContainer(),
 NWidget(NWID_HORIZONTAL),
  NWidget(WWT_TEXTBTN, Colours::Cream, XE_INDUSTRIES), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_EXTERNAL_ECONOMY_INDUSTRIES),
  NWidget(WWT_TEXTBTN, Colours::Cream, XE_CARGOES), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_EXTERNAL_ECONOMY_CARGOES),
  NWidget(WWT_TEXTBTN, Colours::Cream, XE_DETAILS), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_EXTERNAL_ECONOMY_DETAILS),
  NWidget(WWT_RESIZEBOX, Colours::Cream),
 EndContainer(),
};
static WindowDesc _external_economy_desc(__FILE__, __LINE__, WindowPosition::Center, "external_city_economy", 460, 360,
 WindowClass::ExternalCityEconomy, WindowClass::None, WindowDefaultFlag::Construction, _external_economy_widgets);

static CargoTypes ExternalIndustryCargoes(const auto &list)
{
 CargoTypes mask;
 for (CargoType cargo : list) if (cargo < NUM_CARGO && CargoSpec::Get(cargo)->IsValid()) mask.Set(cargo);
 return mask;
}
struct ExternalEconomyWindow : Window {
 struct Line { std::string text; CargoType cargo = INVALID_CARGO; TextColour colour = TextColour::Black; int industry = -1; };
 ExternalCityID city_id;
 ExternalCityEconomy economy;
 std::vector<CargoType> cargoes;
 std::vector<Line> lines;
 std::vector<Line> input_lines, output_lines;
 size_t selected_industry = 1;
 Dimension cargo_icon_size{};
 bool show_cargoes = false, show_details = false, show_industry_overview = true;
 Scrollbar *scroll = nullptr;
 ExternalEconomyWindow(ExternalCityID city) : Window(_external_economy_desc), city_id(city) {
  economy = GetExternalCityEconomy(city_id);
  selected_industry = economy.industries.empty() ? 0 : 1;
  this->CreateNestedTree();
  this->GetWidget<NWidgetStacked>(XE_SELECT_PLANE)->SetDisplayedPlane(0);
  this->FinishInitNested(0); scroll = this->GetScrollbar(XE_SCROLL); this->OnResize(); this->Refresh();
 }
 void OnInit() override { cargo_icon_size = GetLargestCargoIconSize(); }
 int RowHeight() const { return std::max<int>(GetCharacterHeight(FontSize::Normal), cargo_icon_size.height) + ScaleGUITrad(4); }
 size_t IndustryCount() const { return ExternalCity::Get(city_id)->exchange_market ? 1 : economy.industries.size(); }
 std::string SelectedIndustryName() const {
  if (show_industry_overview) return GetString(STR_EXTERNAL_ECONOMY_ALL_INDUSTRIES);
  if (ExternalCity::Get(city_id)->exchange_market && selected_industry != 0) return GetString(STR_EXTERNAL_TRADE_NAME);
  return selected_industry == 0 || selected_industry > economy.industries.size() ? GetString(STR_EXTERNAL_ECONOMY_RESIDENTS) : GetString(GetIndustrySpec(economy.industries[selected_industry - 1])->name);
 }
 void Refresh() {
  economy = GetExternalCityEconomy(city_id); cargoes.clear();
  for (CargoType cargo : economy.produced | economy.accepted) cargoes.push_back(cargo);
  selected_industry = std::min(selected_industry, IndustryCount());
  lines.clear(); input_lines.clear(); output_lines.clear();
  if (show_cargoes) {
   for (CargoType cargo : cargoes) {
    lines.push_back({GetString(CargoSpec::Get(cargo)->name), cargo, TextColour::Yellow});
    if (show_details) {
     lines.push_back({GetString(STR_EXTERNAL_ECONOMY_SOURCE, Sources(cargo, true))});
     lines.push_back({GetString(STR_EXTERNAL_ECONOMY_DESTINATION, Sources(cargo, false))});
    }
    const auto *city = ExternalCity::Get(city_id);
    if (economy.produced.Test(cargo)) lines.push_back({GetString(STR_EXTERNAL_MARKET_STOCK, cargo, city->stock[cargo])});
    AddRating(cargo, economy.produced.Test(cargo), economy.accepted.Test(cargo));
    lines.push_back({});
   }
  } else if (show_industry_overview) {
   auto names = [](CargoTypes mask) {
    std::string text;
    for (CargoType cargo : mask) { if (!text.empty()) text += GetListSeparator(); text += GetString(CargoSpec::Get(cargo)->name); }
    return text.empty() ? GetString(STR_JUST_NOTHING) : text;
   };
   auto card = [&](int index, std::string name, CargoTypes needs, CargoTypes products) {
    lines.push_back({std::move(name), INVALID_CARGO, TextColour::White, index});
    lines.push_back({GetString(STR_EXTERNAL_ECONOMY_NEEDS_LIST, names(needs)), INVALID_CARGO, TextColour::Black, index});
    lines.push_back({GetString(STR_EXTERNAL_ECONOMY_OUTPUT_LIST, names(products)), INVALID_CARGO, TextColour::Yellow, index});
    lines.push_back({{}, INVALID_CARGO, TextColour::Black, index});
   };
   card(0, GetString(STR_EXTERNAL_ECONOMY_RESIDENTS), economy.town_accepted, economy.town_produced);
   if (ExternalCity::Get(city_id)->exchange_market) card(1, GetString(STR_EXTERNAL_TRADE_NAME), economy.accepted & ~economy.town_accepted, economy.produced & ~economy.town_produced);
   for (size_t i = 0; i < economy.industries.size(); ++i) {
    const auto *spec = GetIndustrySpec(economy.industries[i]);
    card(i + 1, GetString(spec->name), ExternalIndustryCargoes(GetExternalFactoryCargoes(ExternalCity::Get(city_id), economy.industries[i], false)), ExternalIndustryCargoes(GetExternalFactoryCargoes(ExternalCity::Get(city_id), economy.industries[i], true)));
   }
   if (ExternalCity::Get(city_id)->exchange_market) {
    lines.push_back({GetString(STR_EXTERNAL_TRADE_CREDIT, ExternalCity::Get(city_id)->trade_credit)});
    const auto [supplied, required] = GetExternalCityTradeCoverage(ExternalCity::Get(city_id));
    lines.push_back({GetString(STR_EXTERNAL_TRADE_COVERAGE, supplied, required, required == 0 ? 0 : std::max<uint>(1, supplied), required)});
    lines.push_back({GetString(STR_EXTERNAL_TRADE_RULE)});
    lines.push_back({GetString(STR_EXTERNAL_TRADE_CAP, 4096)});
   }
  } else {
   CargoTypes produced = economy.town_produced, accepted = economy.town_accepted;
   if (ExternalCity::Get(city_id)->exchange_market && selected_industry != 0) {
    produced = economy.produced & ~economy.town_produced; accepted = economy.accepted & ~economy.town_accepted;
   } else if (selected_industry != 0) {
    const auto *spec = GetIndustrySpec(economy.industries[selected_industry - 1]);
    produced = ExternalIndustryCargoes(GetExternalFactoryCargoes(ExternalCity::Get(city_id), economy.industries[selected_industry - 1], true)); accepted = ExternalIndustryCargoes(GetExternalFactoryCargoes(ExternalCity::Get(city_id), economy.industries[selected_industry - 1], false));
    const auto *city = ExternalCity::Get(city_id);
    const IndustryType type = economy.industries[selected_industry - 1];
    if (show_details && !ExternalIndustryUsesRecipe(type)) {
     lines.push_back({GetString(STR_EXTERNAL_PRODUCTION_ABSTRACT)});
    } else if (show_details) {
     const size_t slot = GetExternalIndustrySlot(city, type);
     bool has_input = false, full = produced.Any(), raw = spec->behaviour.Test(IndustryBehaviour::CutTrees) || spec->life_type.Any({IndustryLifeType::Extractive, IndustryLifeType::Organic});
     for (uint rate : spec->production_rate) raw |= rate != 0;
     for (CargoType cargo : accepted) has_input |= city->industry_inputs[ExternalIndustryCargoIndex(slot, cargo)] != 0;
     for (CargoType cargo : produced) full &= city->stock[cargo] >= 4096;
     if (ExternalIndustryUsesAxisCokeRecipe(type)) {
      const CargoType tar = GetCargoTypeByLabel(CargoLabel{"CTAR"});
      const auto &products = GetIndustrySpec(type)->produced_cargo;
      const bool has_tar = tar < NUM_CARGO && std::ranges::find(products, tar) != products.end();
      lines.push_back({GetString(has_tar ? STR_EXTERNAL_PRODUCTION_AXIS_COKE : STR_EXTERNAL_PRODUCTION_AXIS_COKE_TWO)});
     }
     else if (ExternalIndustryUsesAxisBlastRecipe(type)) lines.push_back({GetString(STR_EXTERNAL_PRODUCTION_AXIS_BLAST)});
     else if (show_details) lines.push_back({GetString(STR_EXTERNAL_PRODUCTION_RECIPE)});
     if (!ExternalIndustryUsesAxisBlastRecipe(type)) lines.push_back({GetString(full ? STR_EXTERNAL_PRODUCTION_FULL : has_input ? STR_EXTERNAL_PRODUCTION_READY : raw ? STR_EXTERNAL_PRODUCTION_RAW : STR_EXTERNAL_PRODUCTION_MISSING), INVALID_CARGO, TextColour::Yellow});
    }
    if (show_details) lines.push_back({});
   }
   auto list = [&](StringID title, CargoTypes mask) {
    lines.push_back({GetString(title == STR_EXTERNAL_ECONOMY_PRODUCTS ? STR_EXTERNAL_ECONOMY_OUTBOUND : STR_EXTERNAL_ECONOMY_INBOUND), INVALID_CARGO, TextColour::White});
    if (mask.None()) lines.push_back({GetString(STR_JUST_NOTHING), INVALID_CARGO, TextColour::Yellow});
    for (CargoType cargo : mask) {
     lines.push_back({GetString(CargoSpec::Get(cargo)->name), cargo, TextColour::Yellow});
     const auto *city = ExternalCity::Get(city_id);
     if (title == STR_EXTERNAL_ECONOMY_PRODUCTS) lines.push_back({GetString(STR_EXTERNAL_MARKET_STOCK, cargo, city->stock[cargo])});
     if (show_details && !city->exchange_market && selected_industry != 0 && ExternalIndustryUsesRecipe(economy.industries[selected_industry - 1])) {
      const size_t slot = GetExternalIndustrySlot(city, economy.industries[selected_industry - 1]);
      const size_t index = ExternalIndustryCargoIndex(slot, cargo);
      if (title == STR_EXTERNAL_ECONOMY_PRODUCTS) {
       lines.push_back({GetString(STR_EXTERNAL_PRODUCTION_MONTH, cargo, city->industry_month_produced[index])});
       if (city->industry_pending_output[index] != 0) lines.push_back({GetString(STR_EXTERNAL_PRODUCTION_PENDING, cargo, city->industry_pending_output[index])});
      } else {
       if (ExternalIndustryUsesAxisBlastRecipe(economy.industries[selected_industry - 1])) lines.push_back({GetString(STR_EXTERNAL_PRODUCTION_RECENT, city->industry_recent_supply[index])});
       else lines.push_back({GetString(STR_EXTERNAL_PRODUCTION_INPUT, cargo, city->industry_inputs[index])});
       lines.push_back({GetString(STR_EXTERNAL_PRODUCTION_CONSUMED, cargo, city->industry_month_consumed[index])});
      }
     }
     AddRating(cargo, title == STR_EXTERNAL_ECONOMY_PRODUCTS, title == STR_EXTERNAL_ECONOMY_REQUIRES);
     lines.push_back({});
    }
   };
   auto header = std::move(lines); lines.clear();
   list(STR_EXTERNAL_ECONOMY_REQUIRES, accepted);
   input_lines = std::move(lines); lines.clear();
   list(STR_EXTERNAL_ECONOMY_PRODUCTS, produced);
   output_lines = std::move(lines); lines = std::move(header);
  }
  scroll->SetCount(lines.size() + std::max(input_lines.size(), output_lines.size()));
  this->SetDirty();
 }
 void AddRating(CargoType cargo, bool supply = true, bool demand = true) {
  const auto *city = ExternalCity::Get(city_id);
  if (!city->statistics_initialized) { lines.push_back({GetString(STR_EXTERNAL_RATING_UNKNOWN)}); return; }
  if (supply && city->month_supply[cargo] != 0) {
   lines.push_back({GetString(STR_EXTERNAL_RATING_SUPPLY, uint64_t(city->month_loaded[cargo]) * 100 / city->month_supply[cargo])});
   lines.push_back({GetString(STR_EXTERNAL_TRANSPORTED_SUPPLY, cargo, city->month_loaded[cargo])});
  }
  if (demand) lines.push_back({GetString(STR_EXTERNAL_RATING_DEMAND, city->month_delivered[cargo])});
 }
 void SelectCity(ExternalCityID city) { city_id = city; selected_industry = 1; show_industry_overview = true; scroll->SetPosition(0); this->Refresh(); }
 void OnInvalidateData(int = 0, bool gui_scope = true) override { if (gui_scope && scroll != nullptr) this->Refresh(); }
 void UpdateWidgetSize(WidgetID widget, Dimension &size, const Dimension &, Dimension &fill, Dimension &resize) override {
  if (widget == XE_LIST) { fill.height = resize.height = RowHeight(); size.height = 12 * RowHeight() + WidgetDimensions::scaled.framerect.Vertical(); }
  if (widget == XE_NOTE) size.height = 2 * GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.framerect.Vertical();
 }
 void OnResize() override {
  if (scroll == nullptr) return;
  scroll->SetCapacity(this->GetWidget<NWidgetBase>(XE_LIST)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect).Height() / RowHeight());
 }
 void OnPaint() override {
  if (!ExternalCity::IsValidID(city_id)) { this->Close(); return; }
  this->SetWidgetLoweredState(XE_INDUSTRIES, !show_cargoes); this->SetWidgetLoweredState(XE_CARGOES, show_cargoes);
  this->SetWidgetLoweredState(XE_DETAILS, show_details);
  this->DrawWidgets();
 }
 std::string GetWidgetString(WidgetID widget, StringID string) const override {
  const auto *city = ExternalCity::GetIfValid(city_id);
  if (widget == XE_CITY || widget == XE_CAPTION) return GetString(widget == XE_CITY ? STR_EXTERNAL_CITY_LINK : STR_EXTERNAL_ECONOMY_CAPTION, city == nullptr ? std::string{} : std::string(city->name));
  if (widget == XE_SELECTION) return GetString(STR_EXTERNAL_ECONOMY_SELECT, SelectedIndustryName());
  return Window::GetWidgetString(widget, string);
 }
 std::string Sources(CargoType cargo, bool produced) const {
  std::string text;
  if ((produced ? economy.town_produced : economy.town_accepted).Test(cargo)) text = GetString(STR_EXTERNAL_ECONOMY_RESIDENTS);
  if (ExternalCity::Get(city_id)->exchange_market && !IsExternalCityPassengerOrMail(cargo)) text = GetString(STR_EXTERNAL_TRADE_NAME);
  for (IndustryType type : economy.industries) {
   const auto *spec = GetIndustrySpec(type);
   if (!ExternalIndustryCargoes(GetExternalFactoryCargoes(ExternalCity::Get(city_id), type, produced)).Test(cargo)) continue;
   if (!text.empty()) text += GetListSeparator();
   text += GetString(spec->name);
  }
  return text.empty() ? GetString(STR_JUST_NOTHING) : text;
 }
 void DrawWidget(const Rect &rect, WidgetID widget) const override {
  if (widget != XE_NOTE && widget != XE_LIST) return;
  /* Background widgets do not install a child clipping rectangle. Keep text
   * and cargo sprites inside their own panel even during partial redraws. */
  const Rect inner = rect.Shrink(WidgetDimensions::scaled.framerect);
  DrawPixelInfo clipped;
  if (!FillDrawPixelInfo(&clipped, inner)) return;
  clipped.left += inner.left; clipped.top += inner.top;
  AutoRestoreBackup dpi(_cur_dpi, &clipped);
  if (widget == XE_NOTE) {
   const auto *city = ExternalCity::Get(city_id);
   DrawStringMultiLine(inner, city->exchange_market ? GetString(STR_EXTERNAL_TRADE_CREDIT, city->trade_credit) : GetString(STR_EXTERNAL_ECONOMY_NOTE), TextColour::Black, {AlignmentH::Centre, AlignmentV::Middle}); return;
  }
  const bool rtl = _current_text_dir == TD_RTL;
  const int indent = cargo_icon_size.width + ScaleGUITrad(6);
  Rect row = inner.WithHeight(RowHeight());
  auto draw_line = [&](const Line &line, Rect row) {
   Rect text = row;
   if (line.cargo != INVALID_CARGO) {
    const SpriteID sprite = CargoSpec::Get(line.cargo)->GetCargoIcon(); const Dimension size = GetSpriteSize(sprite);
    const Rect icon = row.WithWidth(cargo_icon_size.width, rtl);
    DrawSprite(sprite, PAL_NONE, CentreBounds(icon.left, icon.right, size.width), CentreBounds(icon.top, icon.bottom, size.height));
    text = row.Indent(indent, rtl);
   }
   DrawString(text.left, text.right, text.top, line.text, line.colour);
  };
  const int middle = (inner.left + inner.right) / 2;
  const int gap = ScaleGUITrad(6);
  if (!show_cargoes && !show_industry_overview) GfxFillRect(middle, inner.top, middle, inner.bottom, PC_GREY);
  for (uint i = scroll->GetPosition(); i < uint(std::min(scroll->GetCount(), scroll->GetPosition() + scroll->GetCapacity())); ++i, row = row.Translate(0, RowHeight())) {
   if (i < lines.size()) {
    draw_line(lines[i], row);
    if (!show_cargoes && show_industry_overview && lines[i].industry >= 0 && lines[i].text.empty()) GfxFillRect(row.left, row.top + RowHeight() / 2, row.right, row.top + RowHeight() / 2, PC_GREY);
    continue;
   }
   const size_t n = i - lines.size();
   Rect left = row, right = row; left.right = middle - gap; right.left = middle + gap;
   if (n < input_lines.size()) draw_line(input_lines[n], rtl ? right : left);
   if (n < output_lines.size()) draw_line(output_lines[n], rtl ? left : right);
  }
 }
 void OnClick(Point point, WidgetID widget, int) override {
  if (widget == XE_LIST && !show_cargoes && show_industry_overview) {
   const Rect inner = this->GetWidget<NWidgetBase>(XE_LIST)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect);
   if (point.y >= inner.top && point.y <= inner.bottom) {
    const size_t line = scroll->GetPosition() + (point.y - inner.top) / RowHeight();
    if (line < lines.size() && lines[line].industry >= 0) { selected_industry = lines[line].industry; show_industry_overview = false; scroll->SetPosition(0); this->Refresh(); }
   }
  }
  if (widget == XE_DETAILS) { show_details = !show_details; scroll->SetPosition(0); this->Refresh(); }
  if (widget == XE_CITY) {
   DropDownList list;
   for (const auto *city : ExternalCity::Iterate()) list.push_back(MakeDropDownListStringItem(std::string(city->name), city->index.base()));
   ShowDropDownList(this, std::move(list), city_id.base(), XE_CITY);
  }
  if (widget == XE_SELECTION) {
   DropDownList list; list.push_back(MakeDropDownListStringItem(GetString(STR_EXTERNAL_ECONOMY_ALL_INDUSTRIES), -2));
   list.push_back(MakeDropDownListStringItem(GetString(STR_EXTERNAL_ECONOMY_RESIDENTS), 0));
   if (ExternalCity::Get(city_id)->exchange_market) list.push_back(MakeDropDownListStringItem(GetString(STR_EXTERNAL_TRADE_NAME), 1));
   for (size_t i = 0; i < economy.industries.size(); ++i) list.push_back(MakeDropDownListStringItem(GetIndustrySpec(economy.industries[i])->name, i + 1));
   ShowDropDownList(this, std::move(list), show_industry_overview ? -2 : int(selected_industry), XE_SELECTION);
  }
  if (widget == XE_INDUSTRIES || widget == XE_CARGOES) {
   show_cargoes = widget == XE_CARGOES;
   if (!show_cargoes) show_industry_overview = true;
   this->GetWidget<NWidgetStacked>(XE_SELECT_PLANE)->SetDisplayedPlane(show_cargoes ? SZSP_HORIZONTAL : 0);
   scroll->SetPosition(0); this->Refresh(); this->ReInit();
  }
 }
 void OnDropdownSelect(WidgetID widget, int result, int) override {
  if (widget == XE_CITY && result >= 0 && result < 64 && ExternalCity::IsValidID(ExternalCityID{uint16_t(result)})) this->SelectCity(ExternalCityID{uint16_t(result)});
  if (widget == XE_SELECTION && (result == -2 || (result >= 0 && size_t(result) <= IndustryCount()))) {
   show_industry_overview = result == -2;
   if (!show_industry_overview) selected_industry = result;
   scroll->SetPosition(0); this->Refresh();
  }
 }
};
void ShowExternalCityEconomyWindow(ExternalCityID city)
{
 if (!ExternalCity::IsValidID(city)) return;
 auto *window = dynamic_cast<ExternalEconomyWindow *>(BringWindowToFrontById(WindowClass::ExternalCityEconomy, 0));
 if (window == nullptr) new ExternalEconomyWindow(city); else window->SelectCity(city);
}
