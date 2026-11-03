/* Native scheduled stops before shared/independent outlets. No teleporting,
 * cache overrides or manual signal bypass: all movement uses native commands. */
#include "timetable_cmd.h"
namespace {
void InsertStationStop(Train *train, StationID station, uint wait)
{
 Order stop;stop.MakeGoToStation(station);stop.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
 stop.SetLoadType(OrderLoadType::LoadIfPossible);stop.SetWaitTime(wait);stop.SetWaitTimetabled(true);
 DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(train->index,VehicleOrderID{0},stop)),"insert scheduled stop");
 DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,train->index.base(),VehicleOrderID{0},MTF_WAIT_TIME,wait,{}),"scheduled station dwell");
 DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,train->index,VehicleOrderID{0}),"activate scheduled stop");
}
void BuildStationRoute(std::string_view mode)
{
 DemoBuild();
 DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(71,42),RAILTYPE_RAIL,Axis::X,1,3,StationClassID{0},0,StationID::Invalid(),false),"slow platform");
 const StationID slow_station=GetStationIndex(TileXY(71,42));
 DemoRequire(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,TileXY(74,42),TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,AUTO_YIELD_DEFAULT_CONFIG|AUTO_YIELD_PLATFORM,""),"station siding role");
 if(mode!="station-priority")InsertStationStop(demo_slow,slow_station,32);
 if(mode=="station-same"){
  DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(71,40),RAILTYPE_RAIL,Axis::X,1,3,StationClassID{0},0,slow_station,false),"same station main platform");
  InsertStationStop(demo_fast,slow_station,128);
 }else{
  const EngineID engine=demo_fast->engine_type;delete demo_fast;demo_fast=nullptr;
  for(uint x=41;x<=245;x++)DemoRail(x,20,TRACK_X);
  DemoRail(80,20,TRACK_RIGHT);for(uint y=21;y<40;y++)DemoRail(80,y,TRACK_Y);DemoRail(80,40,TRACK_LEFT);
  DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(40,20),RAILTYPE_RAIL,DiagDirection::SW),"station feeder depot");
  DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(71,20),RAILTYPE_RAIL,Axis::X,1,3,StationClassID{0},0,StationID::Invalid(),false),"feeder intermediate station");
  DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(235,20),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"independent destination");
  DemoSignal(42,20,AutoYieldRole::Approach);DemoSignal(54,20,AutoYieldRole::Main);DemoSignal(90,20,AutoYieldRole::Clear);
  SetSignalType(TileXY(90,20),TRACK_X,SignalType::PathOneWay);
  if(mode=="station-merge-split"){
   // The stopping feeder must merge into the common throat, then leaves
   // through its own later branch. Clearance belongs to the shared throat.
   DemoRail(110,40,TRACK_UPPER);for(uint y=26;y<40;y++)DemoRail(110,y,TRACK_Y);DemoRail(110,25,TRACK_LOWER);
   for(uint x=111;x<=245;x++)DemoRail(x,25,TRACK_X);
   DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(235,25),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"station split terminal");
   DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(246,25),RAILTYPE_RAIL,DiagDirection::NE),"station split depot");
   DemoSignal(125,25);DemoSignal(230,25);
   DemoSign(110,45,"两列停站车：先共用咽喉，再分往不同终点");
  }
  const StationID goal=GetStationIndex(mode=="station-different"?TileXY(235,20):mode=="station-merge-split"?TileXY(235,25):TileXY(235,40));
  demo_fast=DemoTrain(engine,"station feeder express",goal,TileXY(40,20));
  if(mode=="station-merge-split"){
   DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_fast->index.base(),1),"replace split depot order");
   Order finish;finish.MakeGoToDepot(GetDepotDestinationIndex(TileXY(246,25)),{},ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS,ODATFB_HALT);
   DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_fast->index,VehicleOrderID{1},finish)),"insert split depot order");
  }
  InsertStationStop(demo_fast,GetStationIndex(TileXY(71,20)),mode=="station-priority"?200:128);
 }
 AutoYieldSettingsChanged();
 fprintf(demo_log,"STATION READY mode=%.*s slow=%u fast=%u\n",int(mode.size()),mode.data(),demo_slow->GetDisplayMaxSpeed(),demo_fast->GetDisplayMaxSpeed());
}
bool StationFastCleared(std::string_view mode)
{
 if(demo_slot->IsOccupant(demo_fast->index))return false;
 for(const Train*part=demo_fast;part;part=part->Next()){
  if(TileX(part->tile)<=90 || TileY(part->tile)!=(mode=="station-different"?20u:40u))return false;
 }
 return true;
}
}

void StationRouteTick()
{
 static uint n=0,overlap=0;static bool entered=false,slow_work=false,fast_work=false,started=false;
 static bool stopped=false,saved=false,cleared=false,resumed=false,slow_left=false;
 const std::string_view mode=std::getenv("PXP_STRICT");
 const bool reload=std::getenv("PXP_STRICT_RELOAD");
 const bool shared=mode!="station-different";
 const bool slow_has_stop=mode!="station-priority";
 const bool require_loading_overlap=slow_has_stop||_settings_game.vehicle.train_braking_model==TBM_REALISTIC;
 if(!demo_started&&_game_mode==GameMode::Editor){demo_started=true;BuildStationRoute(mode);}
 if(_game_mode!=GameMode::Normal)return;
 if(n==0&&reload){
  demo_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/reload-strict.log").c_str(),"w");
  for(auto*v:Train::Iterate())if(v->IsPrimaryVehicle()){if(v->unitnumber==1)demo_slow=v;if(v->unitnumber==2)demo_fast=v;}
  for(auto*s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area==0)demo_slot=s;
  if(!demo_slow||!demo_fast||!demo_slot)exit(90);
  entered=true;slow_work=true;started=!demo_fast->vehstatus.Test(VehState::Stopped);saved=true;_pause_mode={};
 }
 n++;demo_tick=n;
 if(TileY(demo_slow->tile)==42&&!entered){entered=true;fprintf(demo_log,"STATION ENTER %u\n",n);}
 /* Conventional PBS commits a shorter approach: start the contender early
  * enough to register before the slow train reserves its last siding fork. */
 if(!started&&((mode=="station-same"&&n==700)||(mode=="station-priority"&&n==(_settings_game.vehicle.train_braking_model==TBM_REALISTIC?800u:500u))||(mode!="station-same"&&mode!="station-priority"&&entered&&TileX(demo_slow->tile)>=62))){
  DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_fast->index,true),"start following station express");started=true;fprintf(demo_log,"STATION START_FAST %u\n",n);
 }
 if(demo_slow->current_order.IsAnyLoadingType()&&!slow_work){slow_work=true;fprintf(demo_log,"STATION SLOW_LOADING %u\n",n);}
 if(demo_fast->current_order.IsAnyLoadingType()&&!fast_work){
  if(slow_has_stop&&!slow_work){fprintf(demo_log,"FAIL fast stopped before slow station arrival\n");exit(91);}
  fast_work=true;fprintf(demo_log,"STATION FAST_LOADING %u\n",n);
 }
 const bool slow_done=!slow_has_stop||(slow_work&&!demo_slow->current_order.IsAnyLoadingType());
 if(slow_done&&TileY(demo_slow->tile)==42&&TileX(demo_slow->tile)>=73&&demo_slow->cur_speed==0){
  if(!stopped){stopped=true;fprintf(demo_log,"STATION EXIT_STOP %u\n",n);}
  if(demo_fast->current_order.IsAnyLoadingType())overlap++;
 }
 if(!cleared&&StationFastCleared(mode)){cleared=true;fprintf(demo_log,"STATION FAST_TAIL_CLEAR %u\n",n);}
 if(slow_done&&TileY(demo_slow->tile)==40&&TileX(demo_slow->tile)>=80){
  if(shared&&!entered){fprintf(demo_log,"FAIL station bypassed available siding tick=%u\n",n);exit(95);}
  if(shared&&!cleared){fprintf(demo_log,"FAIL station premature departure tick=%u fast_loading=%u fast=%u,%u\n",n,demo_fast->current_order.IsAnyLoadingType(),TileX(demo_fast->tile),TileY(demo_fast->tile));exit(92);}
  if(!slow_left){slow_left=true;fprintf(demo_log,"STATION LEAVE_SIDING %u\n",n);}
 }
 if(slow_done&&stopped&&cleared&&demo_slow->cur_speed>0&&!resumed){resumed=true;fprintf(demo_log,"STATION RESUME %u\n",n);}
 if(!shared&&slow_done&&demo_slow->cur_speed>0)resumed=true;
 if(!saved&&((shared&&overlap>0)||(shared&&!require_loading_overlap&&stopped&&fast_work&&!cleared)||(!shared&&slow_work&&demo_fast->current_order.IsAnyLoadingType()))){DemoSave("strict-waiting");saved=true;}
 if(slow_left&&cleared&&fast_work&&!demo_slot->IsOccupant(demo_slow->index)){
  if(!entered||(slow_has_stop&&!slow_work)||!saved||!resumed||(shared&&(!stopped||(require_loading_overlap&&overlap==0)))){fprintf(demo_log,"FAIL station missing phase overlap=%u saved=%u resumed=%u\n",overlap,saved,resumed);exit(93);}
  fprintf(demo_log,"STRICT PASS mode=%.*s reload=%u entered=1 slow_stop=%u fast_stop=1 exit_wait=%u loading_overlap=%u tail_clear=1 resumed=1 released=1\n",int(mode.size()),mode.data(),reload,slow_has_stop&&slow_work,stopped,overlap);fflush(demo_log);_exit_game=true;
 }
 if(n%256==0){fprintf(demo_log,"STATION TRACE %u slow=%u,%u/%u order=%u fast=%u,%u/%u order=%u\n",n,TileX(demo_slow->tile),TileY(demo_slow->tile),demo_slow->cur_speed,uint(demo_slow->current_order.GetType()),TileX(demo_fast->tile),TileY(demo_fast->tile),demo_fast->cur_speed,uint(demo_fast->current_order.GetType()));fflush(demo_log);}
 if(n>16000){fprintf(demo_log,"FAIL station timeout\n");fflush(demo_log);exit(94);}
}
