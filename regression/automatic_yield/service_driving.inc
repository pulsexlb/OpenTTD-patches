/* Real native arrivals and PXP split/couple lifecycle; no forced speed or position. */
#include "timetable_cmd.h"
#include "waypoint_cmd.h"
#include "newgrf_station.h"
void StationServiceDrivingTick()
{
 if (HasModalProgress() || _generating_world) return;
 const std::string mode = std::getenv("PXP_SERVICE_MODE");
 const bool split = mode != "station";
 const bool reload = std::getenv("PXP_SERVICE_RELOAD") != nullptr;
 const bool recoupling = mode == "recouple" || mode == "recouple-waypoint";
 static bool waypoint_waited = false;
 const std::string first_number = mode == "split-dispatch" ? "D123" : "G123";
 static bool setup = false, first = false, saved = false, recouple_posted = false, return_posted = false;
 static uint ticks = 0;
 static StationID first_station = StationID::Invalid(), second_station = StationID::Invalid();
 if (!setup && _game_mode == GameMode::Editor) {
  demo_started = true;
  DemoBuild();
  first_station = StationID::Invalid();
  DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute, TileXY(120,40), RAILTYPE_RAIL, Axis::X, 1, 8, StationClassID{0}, 0, StationID::Invalid(), true), "build intermediate stop");
  first_station = GetStationIndex(TileXY(120,40)); second_station = GetStationIndex(TileXY(235,40));
  Order stop; stop.MakeGoToStation(first_station); stop.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
  DemoRequire(CmdInsertOrder(DoCommandFlag::Execute, InsertOrderCmdData(demo_slow->index, VehicleOrderID{0}, stop)), "insert intermediate stop");
  DemoRequire(CmdSetOrderServiceNumber(DoCommandFlag::Execute, OrderTargetType::Vehicle, demo_slow->index.base(), VehicleOrderID{0}, 1, "G123"), "first stop number");
  DemoRequire(CmdSetOrderServiceNumber(DoCommandFlag::Execute, OrderTargetType::Vehicle, demo_slow->index.base(), VehicleOrderID{1}, 1, "G456"), "second stop number");
  DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute, OrderTargetType::Vehicle, demo_slow->index.base(), VehicleOrderID{0}, MTF_WAIT_TIME, 64, {}), "first station dwell");
  if (split) {
   DemoRequire(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::Vehicle, demo_slow->index.base(), VehicleOrderID{0}, MOF_DECOUPLE, ODF_DECOUPLE, INVALID_CARGO, {}), "enable actual decoupling");
   DemoRequire(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::Vehicle, demo_slow->index.base(), VehicleOrderID{1}, MOF_DECOUPLE_VALUE, mode == "split-both" ? 2 : 1, INVALID_CARGO, {}), "split count");
   if (mode == "split-load") DemoRequire(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::Vehicle, demo_slow->index.base(), VehicleOrderID{1}, MOF_FIRST_ORDERS, ODOF_KEEP_ORDERS, INVALID_CARGO, {}), "engine also loads");
  }
  if (mode == "split-dispatch") {
   auto &schedules = demo_slow->orders->GetScheduledDispatchScheduleSet(); schedules.emplace_back();
   schedules[0].SetScheduledDispatchDuration(10000); schedules[0].SetScheduledDispatchStartTick(_state_ticks);
   schedules[0].SetSupplementaryName(DispatchSchedule::SupplementaryNameType::RouteID, 1, "D123"); schedules[0].AddScheduledDispatch(0, 0, 1);
   DemoRequire(CmdTimetableSeparation(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),false),"select dispatch instead of automatic separation");
   DemoRequire(CmdSchDispatchSetEnabled(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),true),"enable real dispatch");
   DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MTF_ASSIGN_SCHEDULE,0,{}),"assign dispatch to split stop");
   DemoRequire(CmdSetOrderServiceNumber(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},2,""),"use dispatch number at split stop");
  }
  DemoRequire(CmdSetVehicleServiceNumber(DoCommandFlag::Execute, demo_slow->index, "OLD"), "inbound number");
  DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute, demo_slow->index, VehicleOrderID{0}), "activate first stop");
  assert(demo_slow->GetActiveServiceNumber() == "OLD");
  assert(demo_slow->GetServiceNumberForDeparture(demo_slow->GetOrder(0), first_number) == first_number);
  fprintf(demo_log, "SERVICE inbound=OLD first_outbound=G123 second_outbound=G456 mode=%s\n", mode.c_str());
  setup = true;
 } else if (!setup && reload && _game_mode == GameMode::Normal) {
  demo_log = fopen((std::string(std::getenv("PXP_DEMO_OUTPUT")) + "/reload.log").c_str(), "w");
  for (auto *t : Train::Iterate()) if (t->IsPrimaryVehicle()) { if (t->unitnumber == 1) demo_slow=t; if (t->unitnumber == 2 && t->IsEngine()) demo_fast=t; }
  assert(demo_slow != nullptr);
  first_station=GetStationIndex(TileXY(120,40)); second_station=GetStationIndex(TileXY(235,40));
  _current_company=demo_slow->owner;SetLocalCompany(demo_slow->owner);_pause_mode={};
  setup=true; first=!split;
  if (!split) assert(demo_slow->GetActiveServiceNumber()=="G123");
 }
 if (!setup || _game_mode != GameMode::Normal) return;
 ticks++;
 if (mode == "recouple-waypoint" && demo_slow->current_order.IsType(OT_WAITING) && demo_slow->cur_speed == 0 && IsRailWaypointTile(demo_slow->GetMovingFront()->tile)) {
  if (!waypoint_waited) { fprintf(demo_log,"WAYPOINT held tick=%u returning_to_carriages_behind=1\n",ticks); DemoSave("couple-waypoint-stopped"); }
  waypoint_waited = true;
 }
 if (!reload && ticks==350) DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_fast->index,true), "start express");
 if (!split) {
  if (!first && demo_slow->last_station_visited==first_station && demo_slow->current_order.IsAnyLoadingType()) {
   assert(demo_slow->GetActiveServiceNumber()=="G123");first=true;
   fprintf(demo_log,"ARRIVED first tick=%u actual=G123\n",ticks);
   DemoSave("service-first");
  }
  if (first && demo_slow->last_station_visited==second_station && demo_slow->current_order.IsAnyLoadingType()) {
   assert(demo_slow->GetActiveServiceNumber()=="G456");
   fprintf(demo_log,"STRICT PASS station actual_first=G123 actual_second=G456 reload=%u ticks=%u\n",reload,ticks);fflush(demo_log);_exit_game=true;
  }
 } else {
  Train *carriages=nullptr;
  for(auto *t:Train::Iterate())if(t->IsPrimaryVehicle()&&t!=demo_slow&&t!=demo_fast&&t->last_station_visited==first_station)carriages=t;
  if(carriages!=nullptr && demo_slow->last_station_visited==first_station) {
   assert(carriages->GetActiveServiceNumber()==first_number);
   if(mode=="split-both")assert(demo_slow->GetActiveServiceNumber()=="G123");
   else {assert(demo_slow->GetActiveServiceNumber().empty());assert(demo_slow->service_number_suppressed);}
   if(!saved){
    fprintf(demo_log,"SPLIT tick=%u locomotive=%u number=%s suppressed=%u carriages=%u number=%s\n",ticks,demo_slow->index.base(),demo_slow->GetActiveServiceNumber().c_str(),demo_slow->service_number_suppressed,carriages->index.base(),carriages->GetActiveServiceNumber().c_str());
    if(!reload)DemoSave("service-split");saved=true;
   }
   if(recoupling&&!recouple_posted){
    /* The completed split stop must not execute a second split immediately after recoupling. */
    DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0}),"remove completed split stop from engine schedule");
    Order couple;couple.MakeGoToCouple();
    DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{0},couple)),"insert recouple");
    DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MOF_COUPLE_STATION,first_station.base()+1,INVALID_CARGO,{}),"recouple station");
    if (mode == "recouple-waypoint") {
     DemoRequire(CmdBuildRailWaypoint(DoCommandFlag::Execute,TileXY(150,40),Axis::X,1,1,StationClassID{STAT_CLASS_WAYP},0,StationID::Invalid(),true),"build stop before reverse coupling");
     Order point;point.MakeGoToWaypoint(GetStationIndex(TileXY(150,40)));point.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);point.SetWaypointFlags({OrderWaypointFlag::Stop});
     DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{0},point)),"stop waypoint before coupling");
     DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MTF_WAIT_TIME,64,{}),"waypoint coupling dwell");
    }
    DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,demo_slow->index,VehicleOrderID{0}),"activate recouple");
    recouple_posted=true;
   }
   if(!recoupling) {fprintf(demo_log,"STRICT PASS %s engine_only_cleared=1 carriage_number_retained=1 reload=%u\n",mode.c_str(),reload);fflush(demo_log);_exit_game=true;}
  }
  if(recoupling&&recouple_posted&&demo_slow->GetActiveServiceNumber()=="G123") {
   uint wagons=0;for(const Train*t=demo_slow->First();t;t=t->Next())if(t->IsWagon()&&!t->IsArticulatedPart())wagons++;
   if(wagons==3){if(mode=="recouple-waypoint")assert(waypoint_waited);fprintf(demo_log,"STRICT PASS %s actual_wagons=3 restored=G123 waypoint_waited=%u reload=%u ticks=%u\n",mode.c_str(),waypoint_waited,reload,ticks);fflush(demo_log);_exit_game=true;}
  }
  if(mode=="recouple"&&recouple_posted&&!return_posted&&TileX(demo_slow->GetMovingFront()->tile)>=130) {
   DemoRequire(CmdReverseTrainDirection(DoCommandFlag::Execute,demo_slow->index,false),"native reverse to waiting carriages");return_posted=true;
  }
 }
 if(ticks>20000){fprintf(demo_log,"FAIL service timeout mode=%s tile=%u,%u type=%u number=%s\n",mode.c_str(),TileX(demo_slow->tile),TileY(demo_slow->tile),uint(demo_slow->current_order.GetType()),demo_slow->GetActiveServiceNumber().c_str());fflush(demo_log);exit(3);}
}
