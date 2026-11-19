/* Native whole-train entry, game-time journey and PBS-controlled return. */
#include "settings_cmd.h"
#include "timetable_cmd.h"
#include "vehicle_gui.h"
#include "depot_base.h"

static void SaveExternalExample(const char *name)
{
 AutoRestoreBackup pause(_pause_mode,_pause_mode);_pause_mode.Set(PauseMode::Normal);
 ScrollMainWindowToTile(TileXY(240,40),true);
 const std::string path=std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/"+name+".sav";
 const auto result=SaveOrLoad(path,SaveLoadOperation::Save,DetailedFileType::GameFile,Subdirectory::None,false);
 assert(result==SaveLoadResult::Ok);
 fprintf(demo_log,"EXTERNAL_SAVE name=%s state=%lld\n",name,(long long)_state_ticks.base());fflush(demo_log);
}

void ExternalConnectionDrivingTick()
{
 if(HasModalProgress()||_generating_world)return;
 const std::string mode=std::getenv("PXP_EXTERNAL_MODE");
 const bool reload=std::getenv("PXP_EXTERNAL_RELOAD")!=nullptr;
 static bool setup=false,entered=false,returned=false,saved=false,approach_saved=false,blocker_started=false,blocker_stopped=false,blocker_released=false,disabled=false;
 static uint ticks=0,outside_ticks=0,blocked_ticks=0;
 static StateTicks due=INVALID_STATE_TICKS;
 static std::vector<VehicleID> members;
 static Train*blocker=nullptr;
 const TileIndex portal=TileXY(246,40);
 if(!setup&&_game_mode==GameMode::Editor){
  demo_started=true;DemoBuild();
  DemoRequire(CmdSellVehicle(DoCommandFlag::Execute,demo_fast->tile,demo_fast->index,SellVehicleFlags::SellChain,ClientID::Invalid),"remove unused demo express");demo_fast=nullptr;
  if(mode=="decouple"){
   /* Use a real single-ended engine: the default demo can select a
    * multiheaded logical unit spanning the intermediate wagons. */
   DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_slow->index,true),"stop original depot train");
   DemoRequire(CmdSellVehicle(DoCommandFlag::Execute,demo_slow->tile,demo_slow->index,SellVehicleFlags::SellChain,ClientID::Invalid),"replace fixture with single-ended engine");
   demo_slow=DemoTrain(EngineID{8},"External journey and real-station split",GetStationIndex(TileXY(235,40)));
   DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_slow->index,true),"start single-ended fixture");
  }
  _settings_game.vehicle.external_connections=true;
  _settings_game.vehicle.external_trip_minutes=10;
  if(mode=="clock80" || mode=="clock160"){
   _settings_game.vehicle.external_trip_distance=64;
   DemoRequire(CmdChangeSetting(DoCommandFlag::Execute,"game_time.time_in_minutes",1),"enable shared game clock");
   DemoRequire(CmdChangeSetting(DoCommandFlag::Execute,"game_time.ticks_per_minute",mode=="clock80"?80:160),"set shared clock rate");
  }
  DemoRequire(CmdRenameVehicle(DoCommandFlag::Execute,demo_slow->index,"外联口往返验收列车"),"name example train");
  while(demo_slow->GetNumOrders()!=0)DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0}),"clear demonstration orders");
  Order outside;outside.MakeGoToDepot(GetDepotDestinationIndex(portal),{OrderDepotTypeFlag::PartOfOrders},ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
  DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{0},outside)),"insert portal order");
  DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MOF_DEPOT_ACTION,DA_EXTERNAL,INVALID_CARGO,{}),"choose external journey");
  if(mode=="custom")DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MTF_WAIT_TIME,160,{}),"per-order journey duration");
  Order station;station.MakeGoToStation(GetStationIndex(TileXY(235,40)));station.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
  DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{1},station)),"return to real station");
  if(mode=="decouple"){
   DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{1},MOF_DECOUPLE,ODF_DECOUPLE,INVALID_CARGO,{}),"decouple only at the real station");
   DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{2},MOF_DECOUPLE_VALUE,1,INVALID_CARGO,{}),"retain locomotive at the real station");
  }
  Order finish;finish.MakeGoToDepot(GetDepotDestinationIndex(TileXY(5,40)),{OrderDepotTypeFlag::PartOfOrders},ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS,ODATFB_HALT);
  DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{demo_slow->GetNumOrders()},finish)),"finish at origin depot");
  if(demo_slow->cur_implicit_order_index!=0)DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,demo_slow->index,VehicleOrderID{0}),"activate external journey");
  DemoSign(241,36,"地图外连接口：完整进入后计时，同一编组返回");
  DemoSign(233,44,"验收：返回入口堵塞时须等待信号");
  if(mode=="blocked")blocker=DemoTrain(EngineID{22},"Return entrance blocker",GetStationIndex(TileXY(235,40)),portal);
  if(blocker){
   while(blocker->GetNumOrders()!=0)DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,blocker->index.base(),VehicleOrderID{0}),"clear blocker schedule");
   DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(blocker->index,VehicleOrderID{0},finish)),"blocker native exit");
  }
  SaveExternalExample("external-ready");
  setup=true;
 }else if(!setup&&reload&&_game_mode==GameMode::Normal){
  demo_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/reload.log").c_str(),"w");
  for(Train*t:Train::Iterate())if(t->IsPrimaryVehicle()){
   if(t->unitnumber==1)demo_slow=t;
   if(t->unitnumber==2)demo_fast=t;
   if(t->name=="Return entrance blocker")blocker=t;
  }
  assert(demo_slow&&demo_slow->IsInExternalTrip());
  assert(Depot::GetByTile(demo_slow->tile)->is_external_connection);
  _current_company=demo_slow->owner;SetLocalCompany(demo_slow->owner);_pause_mode={};
  due=demo_slow->external_return_tick;assert(due>_state_ticks);
  setup=true;
 }
 if(!setup||_game_mode!=GameMode::Normal)return;
 ticks++;
 if(!reload&&!approach_saved&&!entered&&TileX(demo_slow->GetMovingFront()->tile)>=240){SaveExternalExample("external-before-entry");approach_saved=true;}
 if(ticks>16000){fprintf(demo_log,"FAIL external timeout mode=%s tick=%u xy=%u,%u order=%u flags=%u\n",mode.c_str(),ticks,TileX(demo_slow->GetMovingFront()->tile),TileY(demo_slow->GetMovingFront()->tile),uint(demo_slow->current_order.GetType()),demo_slow->flags.base());fflush(demo_log);exit(3);}
 if(ticks%256==0){const Order*o=demo_slow->GetOrder(demo_slow->cur_real_order_index);fprintf(demo_log,"STATE tick=%u xy=%u,%u speed=%u order=%u dest=%u index=%u extra=%u saved_extra=%u enabled=%u hidden=%u deadline=%lld\n",ticks,TileX(demo_slow->GetMovingFront()->tile),TileY(demo_slow->GetMovingFront()->tile),demo_slow->cur_speed,uint(demo_slow->current_order.GetType()),demo_slow->current_order.GetDestination().base(),uint(demo_slow->cur_real_order_index),demo_slow->current_order.GetDepotExtraFlags().base(),o?o->GetDepotExtraFlags().base():0,_settings_game.vehicle.external_connections,demo_slow->vehstatus.Test(VehState::Hidden),(long long)demo_slow->external_return_tick.base());fflush(demo_log);}
 if(members.empty())for(const Train*t=demo_slow->First();t;t=t->Next())members.push_back(t->index);
 if(demo_slow->IsInExternalTrip()){
  outside_ticks++;
  if(!entered){
   entered=true;due=demo_slow->external_return_tick;
   fprintf(demo_log,"EXTERNAL_ENTER state=%lld due=%lld members=%zu reload=%u remaining=%lld\n",(long long)_state_ticks.base(),(long long)due.base(),members.size(),reload,(long long)(due-_state_ticks).base());fflush(demo_log);
   /* The observer runs before this loop's tick increment; it sees the same
    * state-tick number as the preceding loop's final-entry callback. */
   if(!reload){
    const uint64_t stop=mode=="custom"?160:uint64_t(_settings_game.vehicle.external_stop_minutes)*_settings_game.game_time.ticks_per_minute;
    const uint64_t expected=2*Vehicle::ExternalTravelTicks(_settings_game.vehicle.external_trip_distance,demo_slow->external_cruise_speed)+stop;
    assert(uint64_t((due-_state_ticks).base())==expected);
    if(mode=="clock80" || mode=="clock160"){
     const auto &clock = _settings_game.game_time;
     assert(clock.time_in_minutes && clock.ticks_per_minute == (mode=="clock80"?80:160));
     fprintf(demo_log,"RAIL_CLOCK rate=%u speed=%u distance=%u leg=%lld stop=%lld\n",clock.ticks_per_minute,
       demo_slow->external_cruise_speed,demo_slow->external_distance_tiles,
       (long long)(demo_slow->external_arrival_tick-_state_ticks).base(),(long long)(demo_slow->external_departure_tick-demo_slow->external_arrival_tick).base());
    }
   }
  }
  assert(demo_slow->cur_speed==0&&demo_slow->IsChainInDepot());
  assert(Depot::GetByTile(portal)->is_external_connection);
  assert(!Depot::GetByTile(TileXY(5,40))->is_external_connection);
  std::vector<VehicleID> actual;for(const Train*t=demo_slow->First();t;t=t->Next()){actual.push_back(t->index);assert(t->tile==portal&&t->vehstatus.Test(VehState::Hidden));}
  assert(actual==members);
  assert(CmdMoveRailVehicle({},members[1],VehicleID::Invalid(),MoveRailVehicleFlags::None).Failed());
  assert(CmdMoveRailVehicle(DoCommandFlag::Execute,members[1],VehicleID::Invalid(),MoveRailVehicleFlags::None).Failed());
  assert(CmdReverseTrainDirection({},demo_slow->index,false).Failed());
  assert(CmdSellVehicle({},demo_slow->tile,demo_slow->index,SellVehicleFlags::SellChain,ClientID::Invalid).Failed());
  assert(CmdSkipToOrder({},demo_slow->index,VehicleOrderID{1}).Failed());
  if(!saved&&!reload){SaveExternalExample("external-inside");saved=true;}
  if(mode=="disable"&&!disabled){
   DemoRequire(CmdChangeSetting(DoCommandFlag::Execute,"vehicle.external_connections",0),"disable only new external journeys");disabled=true;
  }
  if(blocker&&!blocker_started){DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,blocker->index,true),"start native entrance blocker");blocker_started=true;}
 }
 if(blocker&&blocker_started&&!blocker_stopped&&!blocker->IsChainInDepot()&&TileX(blocker->GetMovingFront()->tile)<245){
  DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,blocker->index,true),"stop blocker on its real rail");blocker_stopped=true;
 }
 if(entered&&!demo_slow->IsInExternalTrip()){
  assert(_state_ticks>=due);
  if(!returned){returned=true;fprintf(demo_log,"EXTERNAL_RETURN state=%lld due=%lld outside_ticks=%u\n",(long long)_state_ticks.base(),(long long)due.base(),outside_ticks);}
  if(blocker_stopped&&!blocker_released){
   assert(demo_slow->IsChainInDepot()&&demo_slow->cur_speed==0);
   if(++blocked_ticks>=120){DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,blocker->index,true),"release blocked return entrance");blocker_released=true;}
  }
  if(demo_slow->last_station_visited==GetStationIndex(TileXY(235,40))){
   const bool split=Train::Get(members[1])->Primary()!=demo_slow;
   if(mode=="decouple"&&!split)return;
   if(mode!="decouple"){std::vector<VehicleID> actual;for(const Train*t=demo_slow->First();t;t=t->Next())actual.push_back(t->index);assert(actual==members);}
   if(blocker)assert(blocker_released&&blocked_ticks>=120);
   if(mode=="disable")assert(!_settings_game.vehicle.external_connections);
   fprintf(demo_log,"STRICT PASS external mode=%s entered=1 held=1 native_return=1 reached_station=1 split_only_after_return=%u blocked=%u reload=%u\n",mode.c_str(),split,blocked_ticks,reload);fflush(demo_log);_exit_game=true;
  }
 }
}
