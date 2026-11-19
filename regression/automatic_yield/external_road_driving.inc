#include "external_city.h"
#include "external_city_cmd.h"
#include "roadveh.h"
#include "road_cmd.h"
#include "road_map.h"
#include "landscape_cmd.h"
#include "roadstop_base.h"
#include "industry.h"
#include "industry_cmd.h"
#include "depot_base.h"
#include "timetable_cmd.h"
#include "settings_cmd.h"
#include "town.h"
static FILE* road_log;
static void Require(CommandCost r,const char*n){if(r.Failed()){fprintf(road_log,"ERROR %s: %s\n",n,GetString(r.GetErrorMessage()).c_str());fflush(road_log);exit(2);}}
static void RoadSave(const char*n){AutoRestoreBackup pause(_pause_mode,_pause_mode);_pause_mode.Set(PauseMode::Normal);ScrollMainWindowToTile(TileXY(248,44),true);assert(SaveOrLoad(std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/"+n+".sav",SaveLoadOperation::Save,DetailedFileType::GameFile,Subdirectory::None,false)==SaveLoadResult::Ok);}
static uint64_t RoadHash(){uint64_t h=1469598103934665603ULL;auto mix=[&](uint64_t x){h=(h^x)*1099511628211ULL;};for(const auto*c:ExternalCity::Iterate()){mix(c->loaded_units);mix(c->delivered_units);for(auto x:c->industry_inputs)mix(x);for(auto ctype:EnumRange(NUM_CARGO)){mix(c->stock[ctype]);mix(c->demand[ctype]);}}for(const RoadVehicle*v:RoadVehicle::Iterate()){mix(v->index.base());mix(v->tile.base());mix(v->cargo.StoredCount());mix(v->external_return_tick.base());mix(v->current_order_time);mix(uint(v->cur_real_order_index));mix(v->profit_this_year);}return h;}
void RoadTick(){
 if(HasModalProgress()||_generating_world)return;
 const std::string mode=std::getenv("PXP_ROAD_MODE");const bool truck=mode=="truck",blocked=mode=="blocked",same=mode=="same",removed=mode=="removed";
 const bool reload=std::getenv("PXP_ROAD_RELOAD");static bool setup=false,saved[3]={},seen=false,returned=false,loaded=false,sold=false,block_started=false,block_stopped=false;static uint ticks=0,queued=0;static RoadVehicle *v=nullptr,*blocker=nullptr;
 const bool rotated=mode=="rotated";
 const auto entry=TileXY(254,40),exit_tile=rotated?TileXY(230,126):TileXY(254,47);
 if(!setup){road_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/run.log").c_str(),"w");assert(road_log);}
 if(!setup&&_game_mode==GameMode::Editor){
  _current_company=OWNER_DEITY;Require(CmdFoundTown(DoCommandFlag::Execute,TileXY(100,15),TownSize::Small,false,TownLayout::Original,false,0,"Road demo"),"town");
  _settings_game.economy.place_houses=PlaceHouses::AllowedConstructed;
  for(uint x=231;x<235;++x)Require(CmdPlaceHouse(DoCommandFlag::Execute,TileXY(x,49),HouseID{0},true,TownID{0},false),"passenger houses");
  if(truck)Require(CmdBuildIndustry(DoCommandFlag::Execute,TileXY(231,43),IndustryType{1},0,true,12345),"power station");
  _game_mode=GameMode::Normal;auto*c=DoStartupNewCompany(DSNC_NONE);c->money=10000000;c->current_loan=0;_current_company=_local_company;SetLocalCompany(c->index);_current_company=c->index;_pause_mode={};_settings_game.vehicle.external_connections=true;_settings_game.vehicle.external_boundary_editing=true;_settings_game.vehicle.automatic_yield=true;_settings_game.vehicle.never_expire_vehicles=true;
  auto build=[&](uint x,uint y,RoadBits bits){Require(CmdBuildRoad(DoCommandFlag::Execute,TileXY(x,y),bits,ROADTYPE_ROAD,{},TownID::Invalid(),BuildRoadFlags::None),"road");};
  for(uint y:{40U,47U})for(uint x=6;x<=254;++x)build(x,y,ROAD_X);
  for(uint y=40;y<=47;++y)build(225,y,ROAD_Y);
  if(rotated)for(uint y=47;y<=126;++y)build(230,y,ROAD_Y);
  Require(CmdBuildRoadDepot(DoCommandFlag::Execute,TileXY(5,40),ROADTYPE_ROAD,DiagDirection::SW),"ordinary home depot");
  assert(!Vehicle::IsExternalPortal(TileXY(5,40)));
  assert(GetExternalRoadConnectionDirection(TileXY(240,40),ROADTYPE_ROAD)==DiagDirection::Invalid);
  _settings_game.vehicle.external_connections=false;assert(CmdBuildExternalRoadConnection({},entry,ROADTYPE_ROAD,DiagDirection::NE).Failed());_settings_game.vehicle.external_connections=true;
  for(auto t:{entry,exit_tile}){const auto dir=rotated&&t==exit_tile?DiagDirection::NW:DiagDirection::NE;GameRandomSeedChecker random;Require(CmdBuildExternalRoadConnection({},t,ROADTYPE_ROAD,dir),"preview edge");assert(IsNormalRoadTile(t)&&random.Check());Require(CmdBuildExternalRoadConnection(DoCommandFlag::Execute,t,ROADTYPE_ROAD,dir),"build entrance");assert(Depot::IsExternalConnection(t));}
  Require(CmdBuildRoadStop(DoCommandFlag::Execute,TileXY(232,47),1,1,truck?RoadStopType::Truck:RoadStopType::Bus,true,DiagDirection::NE,ROADTYPE_ROAD,RoadStopClassID{0},0,StationID::Invalid(),false),"stop");
  const StationID st=GetStationIndex(TileXY(232,47));Station::Get(st)->RecomputeCatchment();UpdateStationAcceptance(Station::Get(st),false);
  Require(CmdManageExternalCity(DoCommandFlag::Execute,Depot::GetByTile(entry)->index,ExternalCityID::Invalid(),ExternalCityOperation::Create,4,"Road neighbour"),"city");
  auto*city=ExternalCity::Get(ExternalCityID{0});city->industry_types={IndustryType{0},IT_INVALID,IT_INVALID};city->industries_initialized=true;city->market_initialized=false;city->statistics_initialized=false;city->stock={};city->demand={};InitializeExternalCityMarket(city);
  Require(CmdManageExternalCity(DoCommandFlag::Execute,DepotID::Invalid(),city->index,ExternalCityOperation::SetDistance,20,{}),"distance");Require(CmdManageExternalCity(DoCommandFlag::Execute,DepotID::Invalid(),city->index,ExternalCityOperation::SetStopTime,1,{}),"stop time");
  Require(CmdManageExternalCityRegion(DoCommandFlag::Execute,DepotID::Invalid(),city->index,ExternalCityOperation::SetBoundary,0,{},ExternalBoundaryID::Invalid(),DiagDirection::SW,0,127),"boundary");
  if(rotated)Require(CmdManageExternalCityRegion(DoCommandFlag::Execute,DepotID::Invalid(),city->index,ExternalCityOperation::SetBoundary,0,{},ExternalBoundaryID::Invalid(),DiagDirection::SE,0,255),"south boundary");
  const auto dist=std::getenv("PXP_ROAD_DIST");for(const char*n:{"linkgraph.distribution_pax","linkgraph.distribution_default"})Require(CmdChangeSetting(DoCommandFlag::Execute,n,dist&&std::string(dist)=="asymmetric"?1:0),"distribution");
  Require(CmdChangeSetting(DoCommandFlag::Execute,"linkgraph.recalc_interval",1),"graph interval");Require(CmdChangeSetting(DoCommandFlag::Execute,"linkgraph.recalc_time",1),"graph time");
  auto result=CmdBuildVehicle(DoCommandFlag::Execute,TileXY(5,40),EngineID{uint16_t(truck?124:118)},false,INVALID_CARGO,ClientID::Invalid);Require(result,"buy vehicle");v=RoadVehicle::Get(result.GetResultDataWithType().GetOrDefault<VehicleID>(VehicleID::Invalid()));Require(CmdRenameVehicle(DoCommandFlag::Execute,v->index,"Road subject"),"name");
  if(mode=="shared") SetupSharedExternalPortals(v, entry, exit_tile);
  Order o;o.MakeGoToDepot(Depot::GetByTile(entry)->index,{OrderDepotTypeFlag::PartOfOrders});Require(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(v->index,VehicleOrderID{0},o)),"external order");Require(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_DEPOT_ACTION,DA_EXTERNAL,INVALID_CARGO,{}),"external action");
  if(mode=="shared") CheckExternalReturnSharing(v, VehicleOrderID{0}, Depot::GetByTile(exit_tile)->index);
  if(!same)Require(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_EXTERNAL_RETURN_DEPOT,Depot::GetByTile(exit_tile)->index.base(),INVALID_CARGO,{}),"return port");
  Require(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_LOAD,to_underlying(OrderLoadType::FullLoad),INVALID_CARGO,{}),"full load");Require(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MTF_WAIT_TIME,160,{}),"actual timed stop");
  o.MakeGoToStation(st);o.SetStopLocation(OrderStopLocation::FarEnd);o.SetLoadType(OrderLoadType::NoLoad);Require(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(v->index,VehicleOrderID{1},o)),"on-map delivery");
  if(blocked){auto b=CmdBuildVehicle(DoCommandFlag::Execute,exit_tile,EngineID{118},false,INVALID_CARGO,ClientID::Invalid);Require(b,"blocker");blocker=RoadVehicle::Get(b.GetResultDataWithType().GetOrDefault<VehicleID>(VehicleID::Invalid()));Require(CmdRenameVehicle(DoCommandFlag::Execute,blocker->index,"Road blocker"),"blocker name");Require(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(blocker->index,VehicleOrderID{0},o)),"blocker order");}
  Require(CmdStartStopVehicle(DoCommandFlag::Execute,v->index,true),"start");RoadSave("road-start");setup=true;
 }else if(!setup&&reload&&_game_mode==GameMode::Normal){for(auto*t:RoadVehicle::Iterate()){if(t->name=="Road subject")v=t;if(t->name=="Road blocker")blocker=t;}assert(v);_current_company=v->owner;SetLocalCompany(v->owner);_pause_mode={};setup=true;fprintf(road_log,"LOADED hash=%llu\n",(unsigned long long)RoadHash());block_started=blocker&&!blocker->IsInDepot();block_stopped=block_started&&blocker->vehstatus.Test(VehState::Stopped);}
 if(!setup||_game_mode!=GameMode::Normal)return;++ticks;
 assert(!v->vehstatus.Test(VehState::Crashed));
 if(v->IsInExternalTrip()){
  assert(v->external_entry_tile==entry);seen=true;const auto phase=v->GetExternalTripPhase();int i=phase==ExternalTripPhase::Outbound?0:phase==ExternalTripPhase::Stopped&&v->external_arrival_handled?1:phase==ExternalTripPhase::Returning?2:-1;
  assert(v->IsChainInDepot()&&v->cur_speed==0);assert(v->GetExternalDisplaySpeed()==(phase==ExternalTripPhase::Stopped?0:v->external_cruise_speed));
  if(i==1)assert(v->current_order.IsType(OT_WAITING));
  if(i==2){assert(v->cargo.StoredCount()==v->cargo_cap);loaded=true;}
  if(i>=0&&!saved[i]&&!reload){saved[i]=true;RoadSave(("phase-"+std::to_string(i)).c_str());fprintf(road_log,"SAVED phase=%d hash=%llu\n",i,(unsigned long long)RoadHash());fflush(road_log);}
  if(blocked&&!block_started){Require(CmdStartStopVehicle(DoCommandFlag::Execute,blocker->index,true),"start blocker");block_started=true;}
  if(removed&&IsRoadDepotTile(exit_tile)){Require(Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute,exit_tile),"remove unused return port");}
 }
 if(block_started&&!block_stopped&&!blocker->IsInDepot()&&TileX(blocker->tile)>=253){Require(CmdStartStopVehicle(DoCommandFlag::Execute,blocker->index,true),"stop blocker at entrance");block_stopped=true;}
 if(v->external_return_pending){++queued;assert(v->IsInDepot());if(blocked&&queued==128){assert(block_stopped);Require(CmdStartStopVehicle(DoCommandFlag::Execute,blocker->index,true),"release blocker");}}
 if(seen&&!v->IsInExternalTrip()&&!v->IsInDepot()&&!returned){returned=true;assert(v->tile==(same||removed?entry:exit_tile));assert(v->cur_speed>0);if(blocked)assert(queued>=128);fprintf(road_log,"RETURN tile=%u speed=%u queued=%u\n",v->tile.base(),v->cur_speed,queued);}
 if(returned&&loaded&&v->cargo.StoredCount()==0&&v->profit_this_year>0&&v->current_order.IsAnyLoadingType()){sold=true;fprintf(road_log,"STRICT PASS road mode=%s return=1 actual_unload=1 positive_income=1 speed=%u queued=%u hash=%llu\n",mode.c_str(),v->cur_speed,queued,(unsigned long long)RoadHash());fflush(road_log);RoadSave("road-complete");exit(0);}
 if(ticks%1000==0){fprintf(road_log,"TICK %u tile=%u,%u state=%u order=%u phase=%u cargo=%u speed=%u seen=%u return=%u sold=%u\n",ticks,TileX(v->tile),TileY(v->tile),v->state,uint(v->current_order.GetType()),uint(v->GetExternalTripPhase()),v->cargo.StoredCount(),v->cur_speed,seen,returned,sold);fflush(road_log);}
 if(ticks>40000){fprintf(road_log,"FAIL timeout\n");fflush(road_log);exit(3);}
}
