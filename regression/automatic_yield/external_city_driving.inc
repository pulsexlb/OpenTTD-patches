/* Real two-city journeys; no vehicle placement, speed override or reservation clearing. */
#include "external_city.h"
#include "external_city_cmd.h"
#include "depot_base.h"
#include "timetable_cmd.h"
#include "settings_cmd.h"

static void CitySave(const char *name)
{
 AutoRestoreBackup pause(_pause_mode,_pause_mode);_pause_mode.Set(PauseMode::Normal);
 ScrollMainWindowToTile(TileXY(240,42),true);
 assert(SaveOrLoad(std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/"+name+".sav",SaveLoadOperation::Save,DetailedFileType::GameFile,Subdirectory::None,false)==SaveLoadResult::Ok);
}
void ExternalConnectionDrivingTick()
{
 if(HasModalProgress()||_generating_world)return;
 const std::string mode=std::getenv("PXP_EXTERNAL_MODE");
 const char*reload=std::getenv("PXP_EXTERNAL_RELOAD");
 static bool setup=false,inside=false,first_done=false,second_done=false,changed=false,approach=false;
 const bool cross=mode.rfind("cross-",0)==0;
 const bool same_city=mode=="city-shared"||mode=="cross-same-city"||mode=="cross-company";
 static bool first_visible=false,queued_saved=false;
 static TileIndex expected_return=INVALID_TILE;
 static std::vector<Direction> before_directions;
 static std::vector<bool> before_backwards;
 static Train*blocker=nullptr;static bool blocker_started=false,blocker_stopped=false,blocker_released=false;static uint blocked_ticks=0;
 static uint ticks=0,phase=0,entry_speed=0,exit_speed=0;
 static StateTicks due=INVALID_STATE_TICKS;
 static ExternalCityID city_a=ExternalCityID::Invalid(),city_b=ExternalCityID::Invalid(),snapshot=ExternalCityID::Invalid();
 static std::vector<VehicleID> members;
 const TileIndex a=TileXY(254,40),b=mode=="cross-turn"?TileXY(230,126):TileXY(254,44);
 if(!setup&&_game_mode==GameMode::Editor){
  demo_started=true;DemoBuild();
  if(mode=="cross-fast-realistic"||mode=="cross-block-realistic"){
   DemoRequire(CmdChangeSetting(DoCommandFlag::Execute,"vehicle.train_braking_model",TBM_REALISTIC),"enable realistic braking through native setting callback");
   assert(_settings_game.vehicle.train_braking_model==TBM_REALISTIC);
  }
  DemoRequire(CmdSellVehicle(DoCommandFlag::Execute,demo_fast->tile,demo_fast->index,SellVehicleFlags::SellChain,ClientID::Invalid),"remove unrelated express");demo_fast=nullptr;
  while(demo_slow->GetNumOrders()!=0)DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0}),"clear fixture orders");
  DemoRequire(Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute,TileXY(246,40)),"clear old prototype depot");
  for(uint x=246;x<=254;x++)DemoRail(x,40,TRACK_X);
  DemoRequire(CmdRemoveSingleSignal(DoCommandFlag::Execute,TileXY(230,40),TRACK_X,{}),"remove junction signal before adding branch");
  DemoRail(230,40,TRACK_RIGHT);DemoRail(230,40,TRACK_LOWER);
  for(uint y=41;y<=43;y++)DemoRail(230,y,TRACK_Y);
  DemoRail(230,44,TRACK_LEFT);
  for(uint x=231;x<=254;x++)DemoRail(x,44,TRACK_X);
  _settings_game.vehicle.external_connections=true;_settings_game.vehicle.external_boundary_editing=true;
  if(mode=="cross-turn"){
   DemoRail(230,44,TRACK_Y);for(uint y=45;y<=126;++y)DemoRail(230,y,TRACK_Y);
  }
  for(TileIndex tile:{a,b})DemoRequire(CmdBuildExternalRailConnection(DoCommandFlag::Execute,tile,RAILTYPE_RAIL,tile==b&&mode=="cross-turn"?DiagDirection::NW:DiagDirection::NE),"build native city entrance");
  if(mode=="cross-block-free"||mode=="city-block-free"||mode=="cross-block-realistic"||mode=="cross-block-occupied"||mode=="cross-block-limited"||mode=="cross-block-next-red"){
   _settings_game.pf.reserve_paths=false;
   /* Native realistic braking forbids two-way block signals. Its dedicated
    * case uses a legal one-way return line and finishes at the next station. */
   for(uint y:{40U,44U})if(mode!="cross-block-realistic"||y==44)DemoRequire(CmdBuildSingleSignal(DoCommandFlag::Execute,TileXY(252,y),TRACK_X,SignalType::Block,SignalVariant::Electric,0,1,BuildSignalFlags::None,{},0,
    mode=="cross-block-realistic"?SignalAlongTrackdir(TRACKDIR_X_NE):SignalAlongTrackdir(TRACKDIR_X_NE)|SignalAlongTrackdir(TRACKDIR_X_SW)),"build legal native block signal before entrance");
   for(TileIndex tile:{a,b}){const auto state=UpdateSignalsOnSegment(tile,DiagDirection::Invalid,demo_slow->owner);assert(state==(mode=="cross-block-realistic"?SigSegState::Path:SigSegState::Free));fprintf(demo_log,"NATIVE_EXIT_SEGMENT tile=%u state=%u realistic=%u\n",tile.base(),uint(state),uint(demo_slow->UsingRealisticBraking()));}
  }
  const DepotID da=Depot::GetByTile(a)->index,db=Depot::GetByTile(b)->index;
  assert(ExternalCity::GetNumItems()==0);
  GameRandomSeedChecker random;
  DemoRequire(CmdManageExternalCity({},da,ExternalCityID::Invalid(),ExternalCityOperation::Create,4,"邻市"),"query new city");
  assert(ExternalCity::GetNumItems()==0&&random.Check());
  assert(CmdManageExternalCity({},da,ExternalCityID::Invalid(),ExternalCityOperation::Create,0,"无效").Failed());
  DemoRequire(CmdManageExternalCity(DoCommandFlag::Execute,da,ExternalCityID::Invalid(),ExternalCityOperation::Create,4,"邻市"),"create neighbour city");
  city_a=ExternalCityID{0};
  assert(CmdManageExternalCity({},db,ExternalCityID::Invalid(),ExternalCityOperation::Create,7,"邻市").Failed());
  DemoRequire(CmdManageExternalCity(DoCommandFlag::Execute,db,ExternalCityID::Invalid(),ExternalCityOperation::Create,7,"外部港口"),"create port city");
  city_b=ExternalCityID{1};
  assert(city_a!=city_b&&ExternalCity::GetNumItems()==2);
  auto boundary = [&](ExternalBoundaryID id, ExternalCityID city, uint16_t first, uint16_t last) {
   DemoRequire(CmdManageExternalCityRegion(DoCommandFlag::Execute,DepotID::Invalid(),city,ExternalCityOperation::SetBoundary,0,{},id,DiagDirection::SW,first,last),"set boundary segment");
  };
  boundary(ExternalBoundaryID::Invalid(),city_a,0,42);
  boundary(ExternalBoundaryID::Invalid(),same_city?city_a:city_b,43,uint16_t(Map::SizeY()-1));
  if(mode=="cross-turn")DemoRequire(CmdManageExternalCityRegion(DoCommandFlag::Execute,DepotID::Invalid(),city_b,ExternalCityOperation::SetBoundary,0,{},ExternalBoundaryID::Invalid(),DiagDirection::SE,0,uint16_t(Map::SizeX()-1)),"assign rotated return boundary");
  assert(GetExternalCityForEntrance(a)==city_a&&GetExternalCityForEntrance(b)==(same_city?city_a:city_b));
  assert(Depot::GetByTile(a)->external_city==ExternalCityID::Invalid());
  if(mode=="cross-company") SetupSharedExternalPortals(demo_slow, a, b);
  for(uint index=0;index<4;index++){
   Order order;
   if(index%2==0)order.MakeGoToDepot(index==0?da:db,{OrderDepotTypeFlag::PartOfOrders});
   else {order.MakeGoToStation(GetStationIndex(TileXY(235,40)));order.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);}
   DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,static_cast<VehicleOrderID>(index),order)),"insert city itinerary");
   if(index%2==0)DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),static_cast<VehicleOrderID>(index),MOF_DEPOT_ACTION,DA_EXTERNAL,INVALID_CARGO,{}),"external destination action");
  }
  if(cross){
   for(const auto &[index,target]:{std::pair{VehicleOrderID{0},db},std::pair{VehicleOrderID{2},da}}){
    if(mode=="cross-company") CheckExternalReturnSharing(demo_slow, index, target);
    GameRandomSeedChecker check;
    assert(CmdModifyOrder({},OrderTargetType::Vehicle,demo_slow->index.base(),index,MOF_EXTERNAL_RETURN_DEPOT,target.base(),INVALID_CARGO,{}).Succeeded());
    assert(demo_slow->GetOrder(index)->GetExternalReturnDepot()==DepotID::Invalid()&&check.Check());
    assert(CmdModifyOrder({},OrderTargetType::Vehicle,demo_slow->index.base(),index,MOF_EXTERNAL_RETURN_DEPOT,Depot::GetByTile(TileXY(5,40))->index.base(),INVALID_CARGO,{}).Failed());
    DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),index,MOF_EXTERNAL_RETURN_DEPOT,target.base(),INVALID_CARGO,{}),"select different return entrance");
   }
  }

  if(mode=="city-service"||mode=="city-service-edit"){
   GameRandomSeedChecker random;
   DemoRequire(CmdSetOrderServiceNumber({},OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},1,"T100"),"query external service number");
   assert(demo_slow->GetOrder(VehicleOrderID{0})->GetServiceNumberMode()==0&&random.Check());
   for(const auto &[index,number]:{std::pair{VehicleOrderID{0},"T100"},std::pair{VehicleOrderID{2},"T200"}})
    DemoRequire(CmdSetOrderServiceNumber(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),index,1,number),"set external stop service number");
   assert(demo_slow->fixed_service_number.empty());
  }
  if((mode=="cross-limited"||mode=="cross-block-limited"))DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{1},MTF_TRAVEL_SPEED,30,{}),"limit first return journey speed");
  if(mode=="city-timed")DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MTF_WAIT_TIME,160,{}),"order duration overrides city time");
  Order finish;finish.MakeGoToDepot(GetDepotDestinationIndex(TileXY(5,40)),{OrderDepotTypeFlag::PartOfOrders},ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS,ODATFB_HALT);
  DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{4},finish)),"finish itinerary at home");
  DemoRequire(CmdRenameVehicle(DoCommandFlag::Execute,demo_slow->index,"图外城市往返验收列车"),"name city example train");
  DemoSign(242,36,"图外城市：邻市（城市管理设置距离与停站）");DemoSign(242,48,"图外城市：外部港口（城市管理设置距离与停站）");
  if((mode=="cross-blocked"||mode=="cross-block-occupied"||mode=="cross-block-next-red")){
   auto result=CmdBuildVehicle(DoCommandFlag::Execute,b,EngineID{22},false,INVALID_CARGO,ClientID::Invalid);DemoRequire(result,"build return blocker");
   blocker=Train::Get(result.GetResultDataWithType().GetOrDefault<VehicleID>(VehicleID::Invalid()));
   for(uint i=0;i<3;++i){auto wagon=CmdBuildVehicle(DoCommandFlag::Execute,b,EngineID{27},false,INVALID_CARGO,ClientID::Invalid);DemoRequire(wagon,"blocker wagon");DemoRequire(CmdMoveRailVehicle(DoCommandFlag::Execute,wagon.GetResultDataWithType().GetOrDefault<VehicleID>(VehicleID::Invalid()),blocker->Last()->index,MoveRailVehicleFlags::None),"attach blocker wagon");}
   DemoRequire(CmdRenameVehicle(DoCommandFlag::Execute,blocker->index,"Cross-return exit blocker"),"name blocker");
   Order stop;stop.MakeGoToStation(GetStationIndex(TileXY(235,40)));stop.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);stop.SetLoadType(OrderLoadType::NoLoad);
   DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(blocker->index,VehicleOrderID{0},stop)),"blocker station");
   DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(blocker->index,VehicleOrderID{1},finish)),"blocker home");
  }
  CitySave("city-ready");setup=true;
 }else if(!setup&&reload&&_game_mode==GameMode::Normal){
  demo_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/reload-"+reload+".log").c_str(),"w");assert(demo_log);
  for(Train*t:Train::Iterate())if(t->IsPrimaryVehicle()&&t->unitnumber==1)demo_slow=t;
  assert(demo_slow&&(demo_slow->IsInExternalTrip()||std::string_view(reload)=="queued"&&demo_slow->external_return_pending)&&ExternalCity::GetNumItems()==2);
  city_a=ExternalCityID{0};city_b=ExternalCityID{1};first_done=std::string_view(reload)=="second"||std::string_view(reload)=="queued";
  for(Train*t:Train::Iterate())if(t->name=="Cross-return exit blocker")blocker=t;
  blocker_released=first_done;
  if(std::string_view(reload)=="queued"){blocker_released=false;blocker_started=blocker_stopped=true;assert(demo_slow->external_entry_tile==a);}
  changed=mode=="city-change"||mode=="cross-delete"||mode=="cross-reuse"||mode=="cross-incompatible";_current_company=demo_slow->owner;SetLocalCompany(demo_slow->owner);_pause_mode={};setup=true;
 }
 if(!setup||_game_mode!=GameMode::Normal)return;
 if(++ticks>20000){fprintf(demo_log,"FAIL cities timeout index=%u xy=%u,%u type=%u speed=%u\n",uint(demo_slow->cur_real_order_index),TileX(demo_slow->GetMovingFront()->tile),TileY(demo_slow->GetMovingFront()->tile),uint(demo_slow->current_order.GetType()),demo_slow->cur_speed);fflush(demo_log);exit(3);}
 if(members.empty())for(const Train*t=demo_slow->First();t;t=t->Next())members.push_back(t->index);
 if(!reload&&!approach&&!first_done&&!inside&&TileX(demo_slow->GetMovingFront()->tile)>=240){
  const auto workshop=demo_slow->FindClosestDepot();
  assert(workshop.location==INVALID_TILE||!Depot::IsExternalConnection(workshop.location));
  CitySave("city-before-entry");approach=true;
 }
 if(!demo_slow->IsInExternalTrip()&&demo_slow->cur_real_order_index==0&&demo_slow->GetMovingFront()->tile==a) entry_speed=std::max(entry_speed,uint(demo_slow->cur_speed));
 if(!demo_slow->IsInExternalTrip()&&first_done&&demo_slow->cur_real_order_index==1){
  for(const Train*t=demo_slow->First();t;t=t->Next())if(t->track==TRACK_BIT_DEPOT)exit_speed=std::max(exit_speed,uint(demo_slow->cur_speed));
 }
 if(demo_slow->IsInExternalTrip()){
  if(!inside){
   phase=first_done?2:1;
   assert(phase==1?!first_done:first_done);
   snapshot=phase==1?city_a:(same_city?city_a:city_b);
   assert(demo_slow->external_trip_city==snapshot);

   due=demo_slow->external_return_tick;assert(due>_state_ticks);
   if(mode=="city-service"||mode=="city-service-edit"){
    const std::string expected=phase==2?"T200":mode=="city-service-edit"&&reload?"T101":"T100";
    assert(demo_slow->GetActiveServiceNumber()==expected);
    if(mode=="city-service-edit"&&phase==1&&!reload){
     DemoRequire(CmdSetOrderServiceNumber(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},1,"T101"),"edit service number at actual external stop");
     assert(demo_slow->GetActiveServiceNumber()=="T101"&&demo_slow->external_return_tick==due);
    }
    fprintf(demo_log,"EXTERNAL_SERVICE phase=%u actual=%s active_trip_unchanged=1\n",phase,demo_slow->GetActiveServiceNumber().c_str());
   }

   if(cross){
    expected_return=(mode=="cross-delete"||mode=="cross-reuse"||mode=="cross-incompatible")?a:(phase==1?b:a);
    assert(demo_slow->external_exit_tile==(phase==1?b:a));
    assert(mode=="cross-delete"||mode=="cross-reuse"||Depot::GetIfValid(demo_slow->external_exit_depot)!=nullptr);
    before_directions.clear();before_backwards.clear();
    for(const Train*t=demo_slow->First();t;t=t->Next()){before_directions.push_back(t->direction);before_backwards.push_back(t->IsDrivingBackwards());}
    if(mode=="cross-edit"&&phase==1&&!reload)DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MOF_EXTERNAL_RETURN_DEPOT,UINT16_MAX,INVALID_CARGO,{}),"edit future return without changing active snapshot");
   }
   if(!reload||ticks>1){
    const int64_t leg=Vehicle::ExternalTravelTicks(demo_slow->external_distance_tiles,demo_slow->external_cruise_speed);
    const auto *destination=ExternalCity::Get(snapshot);
    const int64_t stop=phase==1&&mode=="city-timed"?160:int64_t(destination->stop_minutes)*_settings_time.ticks_per_minute;
    assert((demo_slow->external_arrival_tick-_state_ticks).base()==leg);
    assert((demo_slow->external_departure_tick-demo_slow->external_arrival_tick).base()==stop);
    assert((due-_state_ticks).base()==2*leg+stop);
   }
   fprintf(demo_log,"CITY_ENTER phase=%u city=%u due=%lld members=%zu\n",phase,snapshot.base(),(long long)due.base(),members.size());fflush(demo_log);
   inside=true;
   if(mode=="city-change"&&!changed){
    DemoRequire(CmdManageExternalCityRegion(DoCommandFlag::Execute,DepotID::Invalid(),city_b,ExternalCityOperation::SetBoundary,0,{},ExternalBoundaryID{0},DiagDirection::SW,0,42),"change boundary while train away");
    DemoRequire(CmdManageExternalCity(DoCommandFlag::Execute,Depot::GetByTile(a)->index,city_a,ExternalCityOperation::SetTime,9,{}),"change future city time");
    DemoRequire(CmdManageExternalCity(DoCommandFlag::Execute,Depot::GetByTile(a)->index,city_a,ExternalCityOperation::Rename,0,"邻市（已改名）"),"rename city while away");changed=true;
   }
   if(mode=="cross-incompatible"&&!changed){DemoRequire(Command<Commands::ConvertRail>::Do(DoCommandFlag::Execute,b,b,RAILTYPE_MONO,false),"change unused return entrance to incompatible rail");assert(GetRailType(b)==RAILTYPE_MONO);changed=true;}
   if((mode=="cross-delete"||mode=="cross-reuse")&&!changed){
    const auto removed=Depot::GetByTile(b)->index;
    DemoRequire(Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute,b),"remove unused return entrance while away");assert(!IsRailDepotTile(b));
    assert(demo_slow->GetOrder(VehicleOrderID{0})->GetExternalReturnDepot()==DepotID::Invalid());
    if(mode=="cross-reuse"){
     DemoRail(230,44,TRACK_Y);for(uint y=45;y<=47;++y)DemoRail(230,y,TRACK_Y);DemoRail(230,48,TRACK_LEFT);
     for(uint x=231;x<=254;++x)DemoRail(x,48,TRACK_X);
     DemoRequire(CmdBuildExternalRailConnection(DoCommandFlag::Execute,TileXY(254,48),RAILTYPE_RAIL,DiagDirection::NE),"build another entrance reusing the deleted ID");
     assert(Depot::GetByTile(TileXY(254,48))->index==removed);
    }
    changed=true;
   }
   if(!reload)CitySave(phase==1?"city-first":"city-second");
  }
  if(blocker&&phase==1&&!blocker_started){DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,blocker->index,true),"start return blocker");blocker_started=true;}
  assert(demo_slow->external_trip_city==snapshot&&demo_slow->external_return_tick==due&&demo_slow->IsChainInDepot());
  if(cross)assert(demo_slow->external_exit_tile==(phase==1?b:a));

  std::vector<VehicleID> actual;for(const Train*t=demo_slow->First();t;t=t->Next())actual.push_back(t->index);assert(actual==members);
  assert(CmdMoveRailVehicle({},members[1],VehicleID::Invalid(),MoveRailVehicleFlags::None).Failed());
 }else if(inside){
  assert(_state_ticks>=due&&demo_slow->external_trip_city==ExternalCityID::Invalid());
  assert(demo_slow->external_return_pending);
  if(cross){
   assert(demo_slow->tile==expected_return&&demo_slow->IsChainInDepot());
   assert(demo_slow->external_exit_depot==DepotID::Invalid()&&demo_slow->external_exit_tile==INVALID_TILE);
   const uint rotation=(uint(DiagDirToDir(GetRailDepotDirection(expected_return)))+8-uint(DiagDirToDir(GetRailDepotDirection(phase==1?a:b))))&7;
   size_t index=0;for(const Train*t=demo_slow->First();t;t=t->Next(),++index){
    assert(t->index==members[index]&&t->tile==expected_return&&t->vehstatus.Test(VehState::Hidden)&&ValidateVehicleTileHash(t));
    assert(t->direction==Direction((uint(before_directions[index])+rotation)&7));assert(t->IsDrivingBackwards()==before_backwards[index]);
   }
   fprintf(demo_log,"CROSS_RETURN phase=%u exit=%u,%u parts=%zu native_hash_valid=1 chain_and_facing_preserved=1\n",phase,TileX(expected_return),TileY(expected_return),members.size());
  }
  fprintf(demo_log,"CITY_RETURN phase=%u state=%lld due=%lld\n",phase,(long long)_state_ticks.base(),(long long)due.base());fflush(demo_log);
  inside=false;if(phase==1)first_done=true;else second_done=true;
 }
 if(blocker&&blocker_started&&!blocker_stopped&&TileX(blocker->GetMovingFront()->tile)<(mode=="cross-block-occupied"?254U:248U)){
  DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,blocker->index,true),"stop blocker on its native reserved route");blocker_stopped=true;
 }
 if(blocker&&first_done&&!second_done&&!demo_slow->IsInExternalTrip()&&!blocker_released){
  assert(blocker_stopped);
  if(mode=="cross-block-next-red"){
   assert(!demo_slow->vehstatus.Test(VehState::Crashed)&&!blocker->vehstatus.Test(VehState::Crashed));
   if(blocked_ticks>=119)assert(!demo_slow->GetMovingFront()->vehstatus.Test(VehState::Hidden)&&demo_slow->cur_speed==0&&TileX(demo_slow->GetMovingFront()->tile)>=253&&TileX(blocker->GetMovingFront()->tile)<252);
  }else assert(demo_slow->IsChainInDepot()&&demo_slow->tile==b&&demo_slow->cur_speed==0);
  if(++blocked_ticks==60&&mode!="cross-block-next-red"&&!reload&&!queued_saved){CitySave("city-queued");queued_saved=true;}
  if(blocked_ticks==120){DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,blocker->index,true),"release genuinely blocked return");blocker_released=true;fprintf(demo_log,"CROSS_BLOCKED held_ticks=120 native_exit_permission=1 next_signal_red=%u\n",mode=="cross-block-next-red");}
 }

 if((cross||mode=="city-block-free")&&first_done&&!first_visible&&!demo_slow->IsInExternalTrip()&&!demo_slow->external_return_pending&&demo_slow->cur_real_order_index==1){
  if(!demo_slow->IsChainInDepot()){
   first_visible=true;
   fprintf(demo_log,"RUNNING_RETURN speed=%u displayed=%d native_max=%d real_braking=%u reserve_paths=%u\n",demo_slow->cur_speed,demo_slow->GetDisplaySpeed(),demo_slow->GetCurrentMaxSpeed(),uint(demo_slow->UsingRealisticBraking()),_settings_game.pf.reserve_paths);fflush(demo_log);
   if(mode=="cross-basic"||mode=="cross-same-city"||mode=="cross-fast-realistic")assert(demo_slow->cur_speed>_settings_game.vehicle.rail_depot_speed_limit);
   if((mode=="cross-limited"||mode=="cross-block-limited"))assert(demo_slow->cur_speed<=30&&demo_slow->GetDisplaySpeed()<=30);
   if(mode=="cross-block-free"||mode=="city-block-free")assert(demo_slow->cur_speed>_settings_game.vehicle.rail_depot_speed_limit&&demo_slow->GetDisplaySpeed()>_settings_game.vehicle.rail_depot_speed_limit);
   if(mode=="cross-block-realistic")assert(demo_slow->cur_speed>0&&demo_slow->GetDisplaySpeed()>0);
  }
 }
 if((mode=="cross-delete"||mode=="cross-reuse"||mode=="cross-incompatible")&&first_done&&demo_slow->cur_real_order_index==1&&demo_slow->current_order.IsAnyLoadingType()&&IsRailStationTile(demo_slow->GetMovingFront()->tile)){
  assert((mode=="cross-delete"||mode=="cross-reuse")?!IsRailDepotTile(b):GetRailType(b)==RAILTYPE_MONO);
  fprintf(demo_log,"STRICT PASS cities mode=%s invalid_exit=1 original_entrance_fallback=1 native_return=1 reached_real_station=1 reload=%s\n",mode.c_str(),reload?reload:"new");fflush(demo_log);_exit_game=true;
 }
 if(mode=="cross-block-realistic"&&first_done&&demo_slow->cur_real_order_index==1&&demo_slow->current_order.IsAnyLoadingType()&&IsRailStationTile(demo_slow->GetMovingFront()->tile)){
  assert(first_visible&&demo_slow->UsingRealisticBraking());
  fprintf(demo_log,"STRICT PASS cities mode=cross-block-realistic legal_one_way_block=1 realistic_native_path=1 running_return=1 reached_real_station=1 reload=%s\n",reload?reload:"new");fflush(demo_log);_exit_game=true;
 }
 if(second_done&&demo_slow->cur_real_order_index==3&&demo_slow->current_order.IsAnyLoadingType()&&IsRailStationTile(demo_slow->GetMovingFront()->tile)){
  if((mode=="cross-blocked"||mode=="cross-block-occupied"||mode=="cross-block-next-red"))assert(blocker_released&&(reload&&std::string_view(reload)=="second"||blocked_ticks>=120));
  if(!reload){fprintf(demo_log,"NATIVE_PORTAL_SPEED entry=%u exit_with_body_inside=%u ordinary_depot_limit=%u acceleration_model=%u\n",entry_speed,exit_speed,_settings_game.vehicle.rail_depot_speed_limit,uint(_settings_game.vehicle.train_acceleration_model));assert(entry_speed>_settings_game.vehicle.rail_depot_speed_limit);if(mode!="cross-limited"&&mode!="cross-block-limited")assert(exit_speed>_settings_game.vehicle.rail_depot_speed_limit);}
  fprintf(demo_log,"STRICT PASS cities mode=%s first_done=1 second_done=1 reached_real_station=1 original_consist=1 snapshot_stable=1 reload=%s\n",mode.c_str(),reload?reload:"new");fflush(demo_log);_exit_game=true;
 }
}
