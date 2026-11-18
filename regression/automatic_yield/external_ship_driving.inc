/* Actual native ship movement, orders, loading and reload regression. Private driver only. */
#include "external_city.h"
#include "external_city_cmd.h"
#include "ship.h"
#include "water_cmd.h"
#include "terraform_cmd.h"
#include "depot_base.h"
#include "timetable_cmd.h"
#include "settings_cmd.h"
#include "industry_cmd.h"
#include "town.h"
#include "station_func.h"
#include "landscape_cmd.h"
#include "engine_func.h"
static FILE *sea_log;
static void SeaRequire(CommandCost r,const char *n){if(r.Failed()){fprintf(sea_log,"FAIL %s: %s\n",n,GetString(r.GetErrorMessage()).c_str());fflush(sea_log);exit(2);}}
static void SeaSave(const std::string &n){AutoRestoreBackup p(_pause_mode,_pause_mode);_pause_mode.Set(PauseMode::Normal);ScrollMainWindowToTile(TileXY(247,42),true);assert(SaveOrLoad(std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/"+n+".sav",SaveLoadOperation::Save,DetailedFileType::GameFile,Subdirectory::None,false)==SaveLoadResult::Ok);}
static uint64_t SeaHash(){uint64_t h=1469598103934665603ULL;auto mix=[&](uint64_t x){h=(h^x)*1099511628211ULL;};for(const auto*c:ExternalCity::Iterate()){mix(c->loaded_units);mix(c->delivered_units);for(auto x:c->stock)mix(x);for(auto x:c->demand)mix(x);}for(const Ship*v:Ship::Iterate()){mix(v->index.base());mix(v->tile.base());mix(v->cargo.StoredCount());mix(v->external_return_tick.base());mix(v->external_arrival_tick.base());mix(v->external_departure_tick.base());mix(v->current_order_time);mix(v->cur_real_order_index);mix(v->cur_speed);mix(v->vcache.cached_max_speed);mix(uint(v->direction));mix(uint(v->rotation));mix(v->profit_this_year);}return h;}
void SeaTick(){
 if(_exit_game||HasModalProgress()||_generating_world)return;
 const std::string mode=std::getenv("PXP_SEA_MODE"),dist=std::getenv("PXP_SEA_DIST");
 const bool reload=std::getenv("PXP_SEA_RELOAD"),cargo=mode=="cargo",same=mode=="same",removed=mode=="removed",none=mode=="no-load";
 const TileIndex edge=mode=="west"?TileXY(1,40):mode=="north"?TileXY(230,1):mode=="south"?TileXY(230,126):TileXY(254,40);
 const TileIndex other=mode=="rotated"?TileXY(230,126):TileXY(254,43);
 static Ship *v=nullptr;static bool ready=false,saved[3]={},seen=false,returned=false,loaded=false,blocked_once=false;static uint ticks=0,deliveries=0,queued=0;static int prev=-1;static Money last_profit{};static bool paid=false;static uint32_t before_cargo=0;static StateTicks stop_start=INVALID_STATE_TICKS;
 if(!ready&&!sea_log){sea_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/run.log").c_str(),"w");assert(sea_log);}
 if(!ready&&_game_mode==GameMode::Editor){
  _current_company=OWNER_DEITY;SeaRequire(CmdFoundTown(DoCommandFlag::Execute,TileXY(100,15),TownSize::Small,false,TownLayout::Original,false,0,"Harbour demonstration"),"town");
  _settings_game.economy.place_houses=PlaceHouses::AllowedConstructed;
  for(uint x=222;x<226;++x)SeaRequire(CmdPlaceHouse(DoCommandFlag::Execute,TileXY(x,36),HouseID{0},true,TownID{0},false),"passenger demand");
  if(cargo)SeaRequire(CmdBuildIndustry(DoCommandFlag::Execute,TileXY(221,34),IndustryType{1},0,true,12345),"coal demand");
  _game_mode=GameMode::Normal;auto*c=DoStartupNewCompany(DSNC_NONE);c->money=10000000;c->current_loan=0;_current_company=_local_company;SetLocalCompany(c->index);_current_company=c->index;_pause_mode={};_settings_game.vehicle.external_connections=true;_settings_game.vehicle.external_boundary_editing=true;_settings_game.vehicle.never_expire_vehicles=true;
  SeaRequire(CmdBuildCanal(DoCommandFlag::Execute,TileXY(254,43),TileXY(1,40),WaterClass::Canal,false),"horizontal waterway");
  SeaRequire(CmdBuildCanal(DoCommandFlag::Execute,TileXY(231,126),TileXY(230,1),WaterClass::Canal,false),"vertical waterway");
  SeaRequire(CmdTerraformLand(DoCommandFlag::Execute,TileXY(225,39),SLOPE_N,true),"dock bank corner one");
  SeaRequire(CmdTerraformLand(DoCommandFlag::Execute,TileXY(226,39),SLOPE_N,true),"dock bank corner two");
  SeaRequire(CmdBuildDock(DoCommandFlag::Execute,TileXY(225,39),StationID::Invalid(),false),"native map dock");
  const StationID station=GetStationIndex(TileXY(225,39));Station::Get(station)->RecomputeCatchment();UpdateStationAcceptance(Station::Get(station),false);
  assert(Station::Get(station)->goods[cargo?GetCargoTypeByLabel(CT_COAL):GetCargoTypeByLabel(CT_PASSENGERS)].status.Test(GoodsEntry::State::Acceptance));
  SeaRequire(CmdBuildShipDepot(DoCommandFlag::Execute,TileXY(216,41),Axis::X,false),"ordinary home shipyard");
  assert(!Vehicle::IsExternalPortal(TileXY(216,41)));
  assert(GetExternalShipConnectionDirection(TileXY(240,40))==DiagDirection::Invalid);
  _settings_game.vehicle.external_connections=false;assert(CmdBuildShipDepot({},edge,Axis::X,true).Failed());_settings_game.vehicle.external_connections=true;
  for(auto t:{edge,other}){GameRandomSeedChecker rng;SeaRequire(CmdBuildShipDepot({},t,Axis::X,true),"water entrance preview");assert(!Depot::IsExternalConnection(t)&&rng.Check());SeaRequire(CmdBuildShipDepot(DoCommandFlag::Execute,t,Axis::X,true),"water entrance construction");assert(Depot::IsExternalConnection(t)&&Depot::IsExternalConnection(GetOtherShipDepotTile(t)));}
  ExternalCity *city=nullptr;
  for(uint i=0;i<64;++i){SeaRequire(CmdManageExternalCity(DoCommandFlag::Execute,DepotID::Invalid(),ExternalCityID::Invalid(),ExternalCityOperation::Create,1,"Harbour city "+std::to_string(i)),"create city");city=ExternalCity::Get(ExternalCityID{uint16_t(i)});if(!cargo||GetExternalCityEconomy(city->index).produced.Test(GetCargoTypeByLabel(CT_COAL)))break;}
  assert(city&&(!cargo||GetExternalCityEconomy(city->index).produced.Test(GetCargoTypeByLabel(CT_COAL))));
  for(auto d:{DiagDirection::NE,DiagDirection::SE,DiagDirection::SW,DiagDirection::NW})SeaRequire(CmdManageExternalCityRegion(DoCommandFlag::Execute,DepotID::Invalid(),city->index,ExternalCityOperation::SetBoundary,0,{},ExternalBoundaryID::Invalid(),d,0,GetExternalBoundaryLength(d)-1),"city coast");
  SeaRequire(CmdManageExternalCity(DoCommandFlag::Execute,DepotID::Invalid(),city->index,ExternalCityOperation::SetDistance,16,{}),"distance");
  for(const char*n:{"linkgraph.distribution_pax","linkgraph.distribution_default"})SeaRequire(CmdChangeSetting(DoCommandFlag::Execute,n,dist=="asymmetric"?1:0),"distribution");
  for(const char*n:{"linkgraph.recalc_interval","linkgraph.recalc_time"})SeaRequire(CmdChangeSetting(DoCommandFlag::Execute,n,1),"graph interval");
  EngineID engine{uint16_t(cargo?212:207)};
  if(std::getenv("PXP_SEA_NEWGRF")){
   engine=EngineID::Invalid();for(const auto*e:Engine::Iterate())if(e->type==VehicleType::Ship&&!e->grf_prop.grfid.Empty()&&IsEngineBuildable(e->index,VehicleType::Ship,c->index)&&e->GetDefaultCargoType()==GetCargoTypeByLabel(CT_PASSENGERS)){engine=e->index;break;}
   assert(engine!=EngineID::Invalid());fprintf(sea_log,"REAL GRF %s engine=%u\n",Engine::Get(engine)->grf_prop.grfid.AsString().c_str(),engine.base());
  }
  auto result=CmdBuildVehicle(DoCommandFlag::Execute,TileXY(216,41),engine,false,cargo?GetCargoTypeByLabel(CT_COAL):INVALID_CARGO,ClientID::Invalid);SeaRequire(result,"buy native ship");v=Ship::Get(result.GetResultDataWithType().GetOrDefault<VehicleID>(VehicleID::Invalid()));
  SeaRequire(CmdRenameVehicle(DoCommandFlag::Execute,v->index,"Sea subject"),"ship name");assert(v->cargo_type==(cargo?GetCargoTypeByLabel(CT_COAL):GetCargoTypeByLabel(CT_PASSENGERS)));
  Order o;o.MakeGoToDepot(Depot::GetByTile(edge)->index,{OrderDepotTypeFlag::PartOfOrders},ONSF_STOP_EVERYWHERE);SeaRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(v->index,VehicleOrderID{0},o)),"external destination");
  SeaRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_DEPOT_ACTION,DA_EXTERNAL,INVALID_CARGO,{}),"external voyage");
  if(!same)SeaRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_EXTERNAL_RETURN_DEPOT,Depot::GetByTile(other)->index.base(),INVALID_CARGO,{}),"chosen water return");
  SeaRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_LOAD,to_underlying(none?OrderLoadType::NoLoad:OrderLoadType::FullLoad),INVALID_CARGO,{}),"loading rule");
  SeaRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MTF_WAIT_TIME,160,{}),"timetabled city stop");
  o.MakeGoToStation(station);o.SetStopLocation(OrderStopLocation::FarEnd);o.SetLoadType(mode=="roundtrip"?OrderLoadType::FullLoad:OrderLoadType::NoLoad);SeaRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(v->index,VehicleOrderID{1},o)),"map dock delivery");
  SeaRequire(CmdStartStopVehicle(DoCommandFlag::Execute,v->index,true),"start ship");SeaSave("ship-start");ready=true;
 }else if(!ready&&reload&&_game_mode==GameMode::Normal){for(auto*s:Ship::Iterate())if(s->name=="Sea subject")v=s;assert(v);_current_company=v->owner;SetLocalCompany(v->owner);_pause_mode={};ready=true;fprintf(sea_log,"LOADED hash=%llu\n",(unsigned long long)SeaHash());}
 if(!ready||_game_mode!=GameMode::Normal)return;++ticks;
 assert(!v->vehstatus.Test(VehState::Crashed));
 if(returned&&!v->IsInExternalTrip()&&v->profit_this_year>last_profit)paid=true;
 if(v->IsInExternalTrip()){
  assert(Command<Commands::LandscapeClear>::Do({},v->tile).Failed()); // The occupied original entrance cannot disappear.
  if(mode=="blocked"&&!blocked_once){if(IsWaterTile(TileXY(252,43)))SeaRequire(Command<Commands::LandscapeClear>::Do({DoCommandFlag::Execute,DoCommandFlag::AllowRemoveWater},TileXY(252,43)),"block water approach");blocked_once=true;}
  seen=true;const auto phase=v->GetExternalTripPhase();int i=phase==ExternalTripPhase::Outbound?0:phase==ExternalTripPhase::Stopped&&v->external_arrival_handled?1:phase==ExternalTripPhase::Returning?2:-1;
  assert(v->IsInDepot()&&v->vehstatus.Test(VehState::Hidden)&&v->cur_speed==0);
  assert(v->GetExternalDisplaySpeed()==(phase==ExternalTripPhase::Stopped?0:v->external_cruise_speed));
  if(i==1&&prev!=1){stop_start=_state_ticks;assert(v->current_order.IsType(OT_WAITING));}
  if(i==2){assert(none?v->cargo.StoredCount()==0:v->cargo.StoredCount()==v->cargo_cap);loaded=true;if(prev==1&&stop_start!=INVALID_STATE_TICKS)assert(_state_ticks-stop_start>=StateTicksDelta{159});}
  if(i>=0&&!saved[i]&&!reload){saved[i]=true;SeaSave("phase-"+std::to_string(i));fprintf(sea_log,"SAVED phase=%d hash=%llu\n",i,(unsigned long long)SeaHash());fflush(sea_log);}
  if(removed&&IsShipDepotTile(other))SeaRequire(Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute,GetShipDepotNorthTile(other)),"remove return entrance");
  if(i!=prev){fprintf(sea_log,"PHASE %d cargo=%u order=%u\n",i,v->cargo.StoredCount(),v->cur_real_order_index);fflush(sea_log);}prev=i;
 }
 if(mode=="blocked"&&v->external_return_pending){assert(v->IsInDepot()&&v->vehstatus.Test(VehState::Hidden));if(++queued==96)SeaRequire(CmdBuildCanal(DoCommandFlag::Execute,TileXY(252,43),TileXY(252,43),WaterClass::Canal,false),"reopen water approach");}
 if(seen&&!v->IsInExternalTrip()&&!v->IsInDepot()&&!returned){returned=true;const TileIndex expected=GetShipDepotNorthTile(same||removed?edge:other);assert(DistanceManhattan(v->tile,expected)<=1);assert(v->cur_speed>0&&v->cur_speed<=v->GetCurrentMaxSpeed());if(mode=="blocked")assert(queued>=96);fprintf(sea_log,"RETURN tile=%u speed=%u max=%u\n",v->tile.base(),v->cur_speed,v->GetCurrentMaxSpeed());fflush(sea_log);}
 if(returned&&loaded&&v->last_station_visited==GetStationIndex(TileXY(225,39))&&DistanceManhattan(v->tile,TileXY(225,39))<=3&&v->cargo.StoredCount()==0&&(none||paid)){
  ++deliveries;fprintf(sea_log,"DELIVERY %u profit=%lld native_dock=1\n",deliveries,(long long)v->profit_this_year);fflush(sea_log);
  if(deliveries>=3){if(mode=="roundtrip"){uint64_t delivered=0;for(const auto*c:ExternalCity::Iterate())delivered+=c->delivered_units;assert(delivered>0);fprintf(sea_log,"EXTERNAL DELIVERY %llu\n",(unsigned long long)delivered);}fprintf(sea_log,"STRICT PASS ship mode=%s dist=%s deliveries=%u load=%u\n",mode.c_str(),dist.c_str(),deliveries,!none);fflush(sea_log);SeaSave("ship-complete");_exit_game=true;return;}
  seen=returned=loaded=paid=false;prev=-1;stop_start=INVALID_STATE_TICKS;
 }
 before_cargo=v->cargo.StoredCount();last_profit=v->profit_this_year;
 if(ticks%2000==0){fprintf(sea_log,"TICK %u tile=%u,%u order=%u phase=%u cargo=%u speed=%u deliveries=%u\n",ticks,TileX(v->tile),TileY(v->tile),uint(v->current_order.GetType()),uint(v->GetExternalTripPhase()),before_cargo,v->cur_speed,deliveries);fflush(sea_log);}
 if(ticks>120000){fprintf(sea_log,"FAIL timeout\n");fflush(sea_log);exit(3);}
}
