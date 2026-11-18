#include "external_city.h"
#include "external_city_cmd.h"
#include "industry.h"
#include "industry_cmd.h"
#include "industrytype.h"
#include "depot_base.h"
#include "timetable_cmd.h"
#include "settings_cmd.h"
#include "town.h"
static FILE* chain_log;
static void Require(CommandCost r,const char*n){if(r.Failed()){fprintf(chain_log,"ERROR %s: %s\n",n,GetString(r.GetErrorMessage()).c_str());fflush(chain_log);exit(2);}}
static void ChainSave(const char*n){AutoRestoreBackup pause(_pause_mode,_pause_mode);_pause_mode.Set(PauseMode::Normal);ScrollMainWindowToTile(TileXY(235,45),true);assert(SaveOrLoad(std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/"+n+".sav",SaveLoadOperation::Save,DetailedFileType::GameFile,Subdirectory::None,false)==SaveLoadResult::Ok);}
static uint64_t ChainHash(){uint64_t h=1469598103934665603ULL;auto mix=[&](uint64_t x){h=(h^x)*1099511628211ULL;};for(const auto*c:ExternalCity::Iterate()){mix(c->index.base());mix(c->loaded_units);mix(c->delivered_units);for(auto x:c->industry_types)mix(x);for(auto x:c->industry_inputs)mix(x);for(auto x:c->industry_remainders)mix(x);for(auto x:c->industry_month_produced)mix(x);for(auto x:c->industry_month_consumed)mix(x);for(auto ctype:EnumRange(NUM_CARGO)){mix(c->stock[ctype]);mix(c->demand[ctype]);}}for(const Train*t:Train::Iterate()){mix(t->index.base());mix(t->cargo.StoredCount());mix(t->profit_this_year);}return h;}
static Train* ChainTrain(uint y,EngineID wagon,StationID station,bool city_first,const char*name){
 auto result=CmdBuildVehicle(DoCommandFlag::Execute,TileXY(5,y),EngineID{13},false,INVALID_CARGO,ClientID::Invalid);Require(result,"engine");auto*t=Train::Get(result.GetResultDataWithType().GetOrDefault<VehicleID>(VehicleID::Invalid()));
 for(uint i=0;i<3;++i){auto w=CmdBuildVehicle(DoCommandFlag::Execute,TileXY(5,y),wagon,false,INVALID_CARGO,ClientID::Invalid);Require(w,"wagon");Require(CmdMoveRailVehicle(DoCommandFlag::Execute,w.GetResultDataWithType().GetOrDefault<VehicleID>(VehicleID::Invalid()),t->Last()->index,MoveRailVehicleFlags::None),"attach");}
 Require(CmdRenameVehicle(DoCommandFlag::Execute,t->index,name),"name");
 for(uint i=0;i<2;++i){Order o;bool external=(i==0)==city_first;if(external)o.MakeGoToDepot(Depot::GetByTile(TileXY(254,y))->index,{OrderDepotTypeFlag::PartOfOrders});else o.MakeGoToStation(station);o.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);Require(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(t->index,VehicleOrderID(i),o)),"order");if(external)Require(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,t->index.base(),VehicleOrderID(i),MOF_DEPOT_ACTION,DA_EXTERNAL,INVALID_CARGO,{}),"external");Require(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,t->index.base(),VehicleOrderID(i),MOF_LOAD,to_underlying(i==0?OrderLoadType::FullLoad:OrderLoadType::NoLoad),INVALID_CARGO,{}),"load policy");}
 Require(CmdStartStopVehicle(DoCommandFlag::Execute,t->index,true),"start");return t;
}
void ChainTick(){
 if(HasModalProgress()||_generating_world)return;static bool setup=false;static uint ticks=0;static bool saved_input=false,saved_output=false,goods_seen=false;static Train* ore,*steel,*goods;static uint max_goods=0;
 const bool reload=std::getenv("PXP_CHAIN_RELOAD");
 if(!setup){chain_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/run.log").c_str(),"w");assert(chain_log);}
 if(!setup&&_game_mode==GameMode::Editor){
  _current_company=OWNER_DEITY;Require(CmdFoundTown(DoCommandFlag::Execute,TileXY(100,15),TownSize::Small,false,TownLayout::Original,false,0,"Production demo"),"town");
  _settings_game.economy.place_houses=PlaceHouses::AllowedConstructed;
  for(uint x=230;x<236;++x)Require(CmdPlaceHouse(DoCommandFlag::Execute,TileXY(x,62),HouseID{0},true,TownID{0},false),"goods receiver houses");
  Require(CmdBuildIndustry(DoCommandFlag::Execute,TileXY(230,42),IndustryType{8},0,true,12345),"native steel mill");
  _game_mode=GameMode::Normal;auto*company=DoStartupNewCompany(DSNC_NONE);company->money=10000000;company->current_loan=0;_current_company=_local_company;SetLocalCompany(company->index);_current_company=company->index;_pause_mode={};_settings_game.vehicle.external_connections=true;_settings_game.vehicle.external_boundary_editing=true;_settings_game.vehicle.automatic_yield=true;_settings_game.vehicle.never_expire_vehicles=true;
  StationID hub=StationID::Invalid(),town=StationID::Invalid();
  for(uint y:{40U,47U,60U}){
   for(uint x=6;x<=254;++x)Require(CmdBuildSingleRail(DoCommandFlag::Execute,TileXY(x,y),RAILTYPE_RAIL,TRACK_X,BuildRailTrackFlags::None),"rail");
   Require(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(5,y),RAILTYPE_RAIL,DiagDirection::SW),"home");
   Require(CmdBuildExternalRailConnection(DoCommandFlag::Execute,TileXY(254,y),RAILTYPE_RAIL,DiagDirection::NE),"entrance");
   Require(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(230,y),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,y==47?hub:StationID::Invalid(),false),"station");
   if(y==40)hub=GetStationIndex(TileXY(230,y));if(y==60)town=GetStationIndex(TileXY(230,y));
  }
  Require(CmdRenameStation(DoCommandFlag::Execute,hub,false,"Steel mill: ore in / steel out"),"hub name");Require(CmdRenameStation(DoCommandFlag::Execute,town,false,"Goods consumers"),"town name");
  for(uint i=0;i<2;++i){const uint y=i==0?40:47;Require(CmdManageExternalCity(DoCommandFlag::Execute,Depot::GetByTile(TileXY(254,y))->index,ExternalCityID::Invalid(),ExternalCityOperation::Create,4,i==0?"Iron mine city":"Factory city"),"city");auto*c=ExternalCity::Get(ExternalCityID(i));c->industry_types={IndustryType(i==0?18:6),IT_INVALID,IT_INVALID};c->industries_initialized=true;c->market_initialized=false;c->statistics_initialized=false;c->stock={};c->demand={};InitializeExternalCityMarket(c);Require(CmdManageExternalCity(DoCommandFlag::Execute,DepotID::Invalid(),c->index,ExternalCityOperation::SetDistance,12,{}),"distance");Require(CmdManageExternalCity(DoCommandFlag::Execute,DepotID::Invalid(),c->index,ExternalCityOperation::SetStopTime,0,{}),"stop");Require(CmdManageExternalCityRegion(DoCommandFlag::Execute,DepotID::Invalid(),c->index,ExternalCityOperation::SetBoundary,0,{},ExternalBoundaryID::Invalid(),DiagDirection::SW,i==0?0:44,i==0?43:127),"boundary");}
  const auto dist=std::getenv("PXP_CHAIN_DIST");for(const char*n:{"linkgraph.distribution_pax","linkgraph.distribution_default"})Require(CmdChangeSetting(DoCommandFlag::Execute,n,dist&&std::string(dist)=="asymmetric"?1:0),"distribution");
  Require(CmdChangeSetting(DoCommandFlag::Execute,"linkgraph.recalc_interval",1),"graph interval");Require(CmdChangeSetting(DoCommandFlag::Execute,"linkgraph.recalc_time",1),"graph time");
  Station::Get(hub)->RecomputeCatchment();UpdateStationAcceptance(Station::Get(hub),false);Station::Get(town)->RecomputeCatchment();UpdateStationAcceptance(Station::Get(town),false);
  assert(Station::Get(hub)->goods[GetCargoTypeByLabel(CT_IRON_ORE)].status.Test(GoodsEntry::State::Acceptance));assert(Station::Get(town)->goods[GetCargoTypeByLabel(CT_GOODS)].status.Test(GoodsEntry::State::Acceptance));
  ore=ChainTrain(40,EngineID{35},hub,true,"Chain ore");steel=ChainTrain(47,EngineID{36},hub,false,"Chain steel");goods=ChainTrain(60,EngineID{32},town,true,"Chain goods");ChainSave("chain-start");setup=true;
 }else if(!setup&&reload&&_game_mode==GameMode::Normal){for(Train*t:Train::Iterate()){if(t->name=="Chain ore")ore=t;if(t->name=="Chain steel")steel=t;if(t->name=="Chain goods")goods=t;}assert(ore&&steel&&goods);_current_company=ore->owner;SetLocalCompany(ore->owner);_pause_mode={};setup=true;fprintf(chain_log,"LOADED hash=%llu\n",(unsigned long long)ChainHash());}
 if(!setup||_game_mode!=GameMode::Normal)return;
 ++ticks;auto*a=ExternalCity::Get(ExternalCityID{0});auto*b=ExternalCity::Get(ExternalCityID{1});const CargoType steelcargo=GetCargoTypeByLabel(CT_STEEL),goodscargo=GetCargoTypeByLabel(CT_GOODS);
 const uint inputs=b->industry_inputs[ExternalIndustryCargoIndex(0,steelcargo)];assert(uint64_t(inputs)+b->stock[goodscargo]+b->loaded_units==b->delivered_units);
 uint onboard=0;for(const Train*t=goods;t;t=t->Next())onboard+=t->cargo.StoredCount();if(onboard){goods_seen=true;max_goods=std::max(max_goods,onboard);}
 if(inputs&&!saved_input){saved_input=true;ChainSave("chain-input");fprintf(chain_log,"SAVED input hash=%llu\n",(unsigned long long)ChainHash());}
 if(b->stock[goodscargo]&&!saved_output){saved_output=true;ChainSave("chain-output");fprintf(chain_log,"SAVED output hash=%llu\n",(unsigned long long)ChainHash());}
 if(ticks%1000==0){fprintf(chain_log,"TICK %u ore_loaded=%llu steel_sold=%llu input=%u goods_stock=%u goods_loaded=%llu onboard=%u orders=%u,%u,%u pos=%u,%u,%u profits=%lld,%lld,%lld\n",ticks,(unsigned long long)a->loaded_units,(unsigned long long)b->delivered_units,inputs,b->stock[goodscargo],(unsigned long long)b->loaded_units,onboard,uint(ore->current_order.GetType()),uint(steel->current_order.GetType()),uint(goods->current_order.GetType()),TileX(ore->tile),TileX(steel->tile),TileX(goods->tile),(long long)ore->profit_this_year,(long long)steel->profit_this_year,(long long)goods->profit_this_year);fflush(chain_log);}
 if(goods_seen&&onboard==0&&goods->profit_this_year>0&&goods->current_order.IsAnyLoadingType()&&IsRailStationTile(goods->tile)){assert(a->loaded_units>0&&b->delivered_units>0&&b->loaded_units>0);fprintf(chain_log,"STRICT PASS real original chain ore=%llu steel=%llu goods=%llu town_received=%u income=%lld conservation=1\n",(unsigned long long)a->loaded_units,(unsigned long long)b->delivered_units,(unsigned long long)b->loaded_units,max_goods,(long long)(goods->profit_this_year>>8));fflush(chain_log);ChainSave("chain-complete");exit(0);}
 if(ticks>90000){fprintf(chain_log,"FAIL timeout\n");fflush(chain_log);exit(3);}
}
