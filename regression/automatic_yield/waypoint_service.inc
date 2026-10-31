
#include "waypoint_cmd.h"
#include "newgrf_station.h"
#include "timetable_cmd.h"
#include "order_serialisation.h"
void WaypointServiceTick()
{
 if(HasModalProgress()||_generating_world)return;
 const std::string mode=std::getenv("PXP_SERVICE_MODE");
 const bool reload=std::getenv("PXP_SERVICE_RELOAD")!=nullptr;
 const bool stopping=mode!="pass";
 const bool reversing=mode=="reverse"||mode=="reverse-only";
 static bool setup=false,reached=false,stopped=false,saved=false;
 static uint ticks=0,held=0;static StationID waypoint=StationID::Invalid(),destination=StationID::Invalid();
 if(!setup&&_game_mode==GameMode::Editor){
  demo_started=true;DemoBuild();
  DemoRequire(CmdBuildRailWaypoint(DoCommandFlag::Execute,TileXY(120,40),Axis::X,1,1,StationClassID{STAT_CLASS_WAYP},0,StationID::Invalid(),true),"build waypoint");
  waypoint=GetStationIndex(TileXY(120,40));destination=GetStationIndex(TileXY(235,40));
  if(reversing){DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(27,40),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),true),"return station");destination=GetStationIndex(TileXY(27,40));}
  while(demo_slow->GetNumOrders())DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0}),"clear fixture orders");
  Order point;point.MakeGoToWaypoint(waypoint);point.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
  DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{0},point)),"insert waypoint");
  Order goal;goal.MakeGoToStation(destination);goal.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
  DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{1},goal)),"insert final station");
  OrderWaypointFlags flags{};if(stopping&&mode!="reverse-only")flags.Set(OrderWaypointFlag::Stop);if(reversing)flags.Set(OrderWaypointFlag::Reverse);
  DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MOF_WAYPOINT_FLAGS,flags.base(),INVALID_CARGO,{}),"waypoint stop/reverse flags");
  if(stopping&&mode!="stop-zero")DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MTF_WAIT_TIME,8000,{}),"waypoint dwell");
  DemoRequire(CmdSetOrderServiceNumber(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},1,"WP123"),"waypoint service number");
  const auto json=OrderListToJSONString(demo_slow->orders);assert(json.find("WP123")!=std::string::npos);if(flags.Test(OrderWaypointFlag::Stop))assert(json.find("waypoint-stop")!=std::string::npos);
  auto imported=ImportJsonOrderList(demo_slow,json);assert(!imported.HasErrors());assert(demo_slow->GetOrder(0)->GetServiceNumber()=="WP123");assert(demo_slow->GetOrder(0)->GetWaypointFlags()==flags);
  DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MTF_TRAVEL_TIME,5000,{}),"timetable waypoint approach");
  DemoRequire(CmdSetTimetableStart(DoCommandFlag::Execute,demo_slow->index,false,_state_ticks),"start fixture timetable");
  assert(demo_slow->GetActiveServiceNumber().empty());
  setup=true;
 }else if(!setup&&reload&&_game_mode==GameMode::Normal){
  demo_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/reload.log").c_str(),"w");
  for(auto*t:Train::Iterate())if(t->IsPrimaryVehicle()){if(t->unitnumber==1)demo_slow=t;if(t->unitnumber==2)demo_fast=t;}
  assert(demo_slow);_current_company=demo_slow->owner;SetLocalCompany(demo_slow->owner);_pause_mode={};
  waypoint=GetStationIndex(TileXY(120,40));destination=GetStationIndex(TileXY(reversing?27:235,40));
  assert(demo_slow->GetOrder(0)->GetServiceNumber()=="WP123");assert(demo_slow->GetActiveServiceNumber()=="WP123");
  setup=true;reached=true;saved=true;
 }
 if(!setup||_game_mode!=GameMode::Normal)return;
 ticks++;
 if(!reached&&demo_slow->GetActiveServiceNumber()=="WP123"){
  reached=true;fprintf(demo_log,"WAYPOINT mode=%s tick=%u number=WP123 head=%u tail=%u speed=%u distance=%u\n",mode.c_str(),ticks,TileX(demo_slow->GetMovingFront()->tile),TileX(demo_slow->GetMovingBack()->tile),demo_slow->cur_speed,demo_slow->reverse_distance);
 }
 if(reached&&demo_slow->cur_speed==0&&demo_slow->reverse_distance==0&&demo_slow->current_order.IsType(OT_WAITING)){
  stopped=true;held++;assert(TileX(demo_slow->GetMovingBack()->tile)>=120);
  if(held==1){fprintf(demo_log,"DWELL wait=%u timetabled=%u clock=%u late=%d original=%u original_timetabled=%u\n",demo_slow->current_order.GetTimetabledWait(),demo_slow->current_order.IsWaitTimetabled(),demo_slow->current_order_time,demo_slow->lateness_counter,demo_slow->GetOrder(0)->GetWaitTime(),demo_slow->GetOrder(0)->IsWaitTimetabled());fflush(demo_log);}
  if(!saved){DemoSave("waypoint-stopped");saved=true;fprintf(demo_log,"STOP mode=%s tick=%u tail_cleared=1 heading=%u\n",mode.c_str(),ticks,uint(demo_slow->GetMovingFront()->GetMovingDirection()));}
 }
 if(!stopping&&reached&&!saved){DemoSave("waypoint-stopped");saved=true;}
 if(reached&&demo_slow->last_station_visited==destination&&demo_slow->current_order.IsAnyLoadingType()){
  assert(demo_slow->GetActiveServiceNumber()=="WP123");
  if(stopping)assert(stopped);else assert(!stopped);
  if(stopping&&mode!="stop-zero")assert(held>1);
  fprintf(demo_log,"STRICT PASS waypoint mode=%s reload=%u changed_on_actual_pass=1 stopped=%u held=%u destination=%u continued=1 ticks=%u\n",mode.c_str(),reload,stopped,held,destination.base(),ticks);fflush(demo_log);_exit_game=true;
 }
 if(ticks>20000){fprintf(demo_log,"FAIL timeout mode=%s reload=%u xy=%u,%u type=%u speed=%u distance=%u reached=%u stopped=%u\n",mode.c_str(),reload,TileX(demo_slow->tile),TileY(demo_slow->tile),uint(demo_slow->current_order.GetType()),demo_slow->cur_speed,demo_slow->reverse_distance,reached,stopped);fflush(demo_log);exit(3);}
}
