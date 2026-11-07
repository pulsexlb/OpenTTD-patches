/* Native station arrival, decoupling and stopped-waypoint timing regression.
 * Only fixture setup sets the incoming timetable offset. Movement, splitting,
 * stopping and departure are performed by the normal game loop. */
#include "timetable_cmd.h"
#include "waypoint_cmd.h"
#include "newgrf_station.h"
void StationServiceDrivingTick()
{
 if (HasModalProgress() || _generating_world) return;
 const std::string mode = std::getenv("PXP_SERVICE_MODE");
 const bool reload = std::getenv("PXP_SERVICE_RELOAD") != nullptr;
 const bool enabled = mode.ends_with("-on");
 const bool load = mode.starts_with("load-");
 const bool keep = mode.starts_with("keep-");
 const bool second = mode.starts_with("second-");
 const bool invalid = mode.starts_with("invalid-");
 static bool setup = false, split_seen = false, waited = false, before_saved = false;
 static uint ticks = 0, wait_ticks = 0, loading_ticks = 0;
 static StationID split_station = StationID::Invalid();
 if (!setup && _game_mode == GameMode::Editor) {
  demo_started = true; DemoBuild();
  DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(120,40),RAILTYPE_RAIL,Axis::X,1,8,StationClassID{0},0,StationID::Invalid(),true),"build split station");
  split_station=GetStationIndex(TileXY(120,40));
  DemoRequire(CmdBuildRailWaypoint(DoCommandFlag::Execute,TileXY(150,40),Axis::X,1,1,StationClassID{STAT_CLASS_WAYP},0,StationID::Invalid(),true),"build next stopped waypoint");
  Order point;point.MakeGoToWaypoint(GetStationIndex(TileXY(150,40)));point.SetWaypointFlags({OrderWaypointFlag::Stop});point.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
  Order exit;exit.MakeGoToStation(GetStationIndex(TileXY(235,40)));exit.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
  OrderList *schedule=OrderList::Create();schedule->SetCompany(demo_slow->owner);schedule->InitializePlayerCreated();
  schedule->InsertOrderAt(Order(point),0);schedule->InsertOrderAt(Order(exit),1);
  DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::OrderList,schedule->index.base(),VehicleOrderID{0},MTF_WAIT_TIME,32,{}),"independent waypoint dwell");
  Order stop;stop.MakeGoToStation(split_station);stop.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
  DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{0},stop)),"insert split stop");
  DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MOF_DECOUPLE,ODF_DECOUPLE,INVALID_CARGO,{}),"enable split");
  DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{1},MOF_DECOUPLE_VALUE,1,INVALID_CARGO,{}),"split engine from wagons");
  const auto schedule_mof=second?MOF_DECOUPLE_SECOND_SCHEDULE:load?MOF_DECOUPLE_FIRST_LOAD_SCHEDULE:MOF_DECOUPLE_FIRST_SCHEDULE;
  if (!keep) DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{1},schedule_mof,schedule->index.base(),INVALID_CARGO,{}),"adopt independent schedule");
  if (invalid) demo_slow->GetOrder(1)->SetDecoupleFirstScheduleID(OrderListID::Invalid());
  DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{1},second?MOF_DECOUPLE_SECOND_RESET_LATENESS:MOF_DECOUPLE_FIRST_RESET_LATENESS,enabled,INVALID_CARGO,{}),"set independent timing option");
  const Order before_query(*demo_slow->GetOrder(1));
  DemoRequire(CmdModifyOrder({},OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{1},MOF_DECOUPLE_FIRST_RESET_LATENESS,1,INVALID_CARGO,{}),"query must not mutate timing option");
  assert(demo_slow->GetOrder(1)->GetDecoupleResetLateness(true)==before_query.GetDecoupleResetLateness(true));
  assert(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{1},MOF_DECOUPLE_FIRST_RESET_LATENESS,2,INVALID_CARGO,{}).Failed());
  assert(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MOF_DECOUPLE_FIRST_RESET_LATENESS,1,INVALID_CARGO,{}).Failed());
  DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MTF_WAIT_TIME,64,{}),"preserve real dwell");
  DemoRequire(CmdSetOrderServiceNumber(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},1,"G123"),"wagon service number");
  if (keep) {DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{2},point)),"continuing stopped waypoint");DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{2},MTF_WAIT_TIME,32,{}),"continuing waypoint dwell");}
  DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,demo_slow->index,VehicleOrderID{0}),"activate split arrival");
  demo_slow->vehicle_flags.Reset(VehicleFlag::AutomateTimetable);demo_slow->vehicle_flags.Reset(VehicleFlag::TimetableSeparation);
  demo_slow->vehicle_flags.Set(VehicleFlag::TimetableStarted);demo_slow->timetable_start=StateTicks{0};demo_slow->lateness_counter=-50000;
  fprintf(demo_log,"SETUP mode=%s incoming_lateness=%d first_reset=%u second_reset=%u\n",mode.c_str(),demo_slow->lateness_counter,demo_slow->GetOrder(1)->GetDecoupleResetLateness(true),demo_slow->GetOrder(1)->GetDecoupleResetLateness(false));
  DemoSave("service-split");setup=true;
 } else if (!setup && reload && _game_mode==GameMode::Normal) {
  demo_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/reload.log").c_str(),"w");
  for(auto*t:Train::Iterate())if(t->IsPrimaryVehicle()){if(t->unitnumber==1)demo_slow=t;if(t->unitnumber==2)demo_fast=t;}
  assert(demo_slow&&demo_fast);split_station=GetStationIndex(TileXY(120,40));_current_company=demo_slow->owner;SetLocalCompany(demo_slow->owner);_pause_mode={};
  assert(demo_slow->GetOrder(1)->GetDecoupleResetLateness(!second)==enabled);assert(!demo_slow->GetOrder(1)->GetDecoupleResetLateness(second));setup=true;
 }
 if(!setup||_game_mode!=GameMode::Normal)return;
 ticks++;
 if(!before_saved&&!reload&&!split_seen&&TileX(demo_slow->GetMovingFront()->tile)>=118){DemoSave("timing-before-split");before_saved=true;}
 Train*carriages=nullptr;for(auto*t:Train::Iterate())if(t->IsPrimaryVehicle()&&t!=demo_slow&&t!=demo_fast&&t->last_station_visited==split_station)carriages=t;
 if(carriages&&!split_seen){
  split_seen=true;
  fprintf(demo_log,"SPLIT tick=%u engine_lateness=%d wagon_lateness=%d engine_timer=%u wagon_timer=%u wagon_number=%s\n",ticks,demo_slow->lateness_counter,carriages->lateness_counter,demo_slow->current_order_time,carriages->current_order_time,carriages->GetActiveServiceNumber().c_str());
  assert(carriages->GetActiveServiceNumber()=="G123");assert(demo_slow->GetActiveServiceNumber().empty());
  if(enabled&&!keep&&!invalid&&!second)assert(demo_slow->lateness_counter==0);else assert(demo_slow->lateness_counter<-40000);
  if(!second)assert(carriages->lateness_counter<-40000);else if(enabled)assert(carriages->lateness_counter==0);
  assert(demo_slow->current_loading_time==0); // no inherited old loading duration
  if(keep||invalid||second){fprintf(demo_log,"STRICT PASS %s continuing_timing_preserved=1 no_unconditional_reset=1 reload=%u\n",mode.c_str(),reload);fflush(demo_log);_exit_game=true;}
 }
 if(split_seen&&!keep&&!invalid&&!second){
  if(load&&!enabled&&demo_slow->current_order.IsAnyLoadingType()&&++loading_ticks==128){assert(demo_slow->cur_speed==0&&demo_slow->lateness_counter<-40000);fprintf(demo_log,"STRICT PASS %s option_off_preserves_station_early_wait=1 reload=%u\n",mode.c_str(),reload);fflush(demo_log);_exit_game=true;}
  if(demo_slow->current_order.IsType(OT_WAITING)&&IsRailWaypointTile(demo_slow->GetMovingFront()->tile)){
   assert(demo_slow->cur_speed==0);waited=true;wait_ticks++;
   if(!enabled&&wait_ticks==128){assert(demo_slow->lateness_counter<-40000);fprintf(demo_log,"STRICT PASS %s option_off_preserves_early_wait=1 reload=%u\n",mode.c_str(),reload);fflush(demo_log);_exit_game=true;}
  }
  if(enabled&&waited&&TileX(demo_slow->GetMovingFront()->tile)>153){assert(wait_ticks>=32&&wait_ticks<512);fprintf(demo_log,"STRICT PASS %s actual_split=1 waypoint_stopped=1 actual_departure=1 wait_ticks=%u reload=%u\n",mode.c_str(),wait_ticks,reload);fflush(demo_log);_exit_game=true;}
 }
 if(ticks>20000){fprintf(demo_log,"FAIL timing mode=%s tile=%u,%u order=%u lateness=%d split=%u waited=%u\n",mode.c_str(),TileX(demo_slow->tile),TileY(demo_slow->tile),demo_slow->current_order.GetType(),demo_slow->lateness_counter,split_seen,waited);fflush(demo_log);exit(3);}
}
