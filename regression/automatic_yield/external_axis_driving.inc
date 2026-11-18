#include "external_city.h"
#include "external_city_cmd.h"
#include "industry.h"
#include "industry_cmd.h"
#include "industrytype.h"
#include "newgrf.h"
#include "newgrf_config.h"
#include "newgrf_industries.h"
#include "depot_base.h"
#include "settings_cmd.h"
#include "town.h"
static FILE* axis_log;
static CargoType axis_coal,axis_coke,axis_tar,axis_sulphur;
static uint64_t axis_routed_loaded=0;
static uint axis_wrong_entrances=0;
void AxisEntryStarted(Vehicle*v){assert(GetExternalOrderCity(v->current_order)==v->external_trip_city);}
void AxisEntranceAttempt(Vehicle*v){if(v->type==VehicleType::Train && v->current_order.IsExternalTripOrder() && Vehicle::IsExternalPortal(v->tile) && v->tile!=v->dest_tile){++axis_wrong_entrances;fprintf(axis_log,"WRONG_ENTRANCE native turnaround id=%u tile=%u target=%u\n",v->index.base(),v->tile.base(),v->dest_tile.base());}}
static uint AxisRoutedStock(ExternalCity*c){uint n=0;const auto*st=Station::Get(c->market_station);for(CargoType ct:EnumRange(NUM_CARGO))if(st->goods[ct].data)for(const auto&[via,packets]:*st->goods[ct].data->cargo.Packets())if(via!=StationID::Invalid())for(const auto*cp:packets)n+=cp->Count();return n;}
static uint axis_before_routed=0;
void AxisMarketBefore(Vehicle*v,bool arrival){if(!arrival){auto*c=ExternalCity::GetIfValid(v->external_trip_city);axis_before_routed=c?AxisRoutedStock(c):0;}}
void AxisMarketAfter(Vehicle*v,bool arrival){if(!arrival){auto*c=ExternalCity::GetIfValid(v->external_trip_city);if(c){const uint after=AxisRoutedStock(c);if(after<axis_before_routed)axis_routed_loaded+=axis_before_routed-after;}}}
static void Require(CommandCost r,const char*n){if(r.Failed()){fprintf(axis_log,"ERROR %s: %s\n",n,GetString(r.GetErrorMessage()).c_str());fflush(axis_log);exit(2);}}
static void AxisSave(const char*n){AutoRestoreBackup pause(_pause_mode,_pause_mode);_pause_mode.Set(PauseMode::Normal);ScrollMainWindowToTile(TileXY(235,45),true);assert(SaveOrLoad(std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/"+n+".sav",SaveLoadOperation::Save,DetailedFileType::GameFile,Subdirectory::None,false)==SaveLoadResult::Ok);}
static uint64_t AxisHash(){uint64_t h=1469598103934665603ULL;auto mix=[&](uint64_t x){h=(h^x)*1099511628211ULL;};for(const auto*c:ExternalCity::Iterate()){mix(c->index.base());mix(c->loaded_units);mix(c->delivered_units);for(auto x:c->industry_types)mix(x);for(auto x:c->industry_inputs)mix(x);for(auto x:c->industry_remainders)mix(x);for(auto x:c->industry_month_produced)mix(x);for(auto x:c->industry_month_consumed)mix(x);for(CargoType ct:EnumRange(NUM_CARGO))mix(c->stock[ct]);}for(const Train*t:Train::Iterate()){mix(t->index.base());mix(t->cargo.StoredCount());mix(t->profit_this_year);}return h;}
static IndustryType AxisType(const char*name){for(IndustryType i=0;i<NUM_INDUSTRYTYPES;++i){const auto*s=GetIndustrySpec(i);if(s->enabled&&s->grf_prop.grffile&&GetString(s->name)==name)return i;}assert(false);return IT_INVALID;}
static void CargoLabels(){axis_coal=GetCargoTypeByLabel(CT_COAL);axis_coke=GetCargoTypeByLabel(CargoLabel{"COKE"});axis_tar=GetCargoTypeByLabel(CargoLabel{"CTAR"});axis_sulphur=GetCargoTypeByLabel(CargoLabel{"SULP"});assert(axis_coal<NUM_CARGO&&axis_coke<NUM_CARGO&&axis_tar<NUM_CARGO&&axis_sulphur<NUM_CARGO);}
static void RecipeProbe(IndustryType oven,Industry* native){
 auto*cfg=GetGRFConfig(GetIndustrySpec(oven)->grf_prop.grffile->grfid);
 fprintf(axis_log,"ADAPTER grfid=%s md5=%s local=%u version=%u param0=%u supported=%u\n",cfg->ident.grfid.AsString().c_str(),md5sumToString(cfg->ident.md5sum).c_str(),GetIndustrySpec(oven)->grf_prop.local_id,cfg->version,cfg->param[0],ExternalIndustryUsesAxisCokeRecipe(oven));fflush(axis_log);
 assert(ExternalIndustryUsesAxisCokeRecipe(oven));
 {AutoRestoreBackup b(cfg->param[0],0U);assert(!ExternalIndustryUsesAxisCokeRecipe(oven));}
 {AutoRestoreBackup b(cfg->ident.md5sum[0],uint8_t(cfg->ident.md5sum[0]^1));assert(!ExternalIndustryUsesAxisCokeRecipe(oven));}
 {AutoRestoreBackup b(cfg->flags,cfg->flags);cfg->flags.Set(GRFConfigFlag::Compatible);assert(!ExternalIndustryUsesAxisCokeRecipe(oven));}
 auto*c=ExternalCity::Create();c->industry_types={oven,IT_INVALID,IT_INVALID};c->industries_initialized=true;InitializeExternalCityMarket(c);
 assert(c->stock[axis_coke]==0&&c->stock[axis_tar]==0&&c->stock[axis_sulphur]==0);
 RefillExternalCityMarkets();assert(c->stock[axis_coke]==0);
 StoreExternalCityInputs(c,axis_coal,7);RefillExternalCityMarkets();assert(c->stock[axis_coke]==0&&c->industry_inputs[ExternalIndustryCargoIndex(0,axis_coal)]==7);
 StoreExternalCityInputs(c,axis_coal,1);RefillExternalCityMarkets();assert(c->stock[axis_coke]==6&&c->stock[axis_tar]==1&&c->stock[axis_sulphur]==1);
 // Compare complete batches with actual production callback of the loaded GRF.
 for(uint n:{8U,16U,256U}){
  for(auto&a:native->Accepted())a.waiting=a.cargo==axis_coal?n:0;
  for(auto&o:native->Produced())o.waiting=0;
  IndustryProductionCallback(native,0);
  for(const auto&o:native->Produced()){const uint ratio=o.cargo==axis_coke?6:1;assert(o.waiting==n/8*ratio);}
 }
 c->stock[axis_tar]=4096;StoreExternalCityInputs(c,axis_coal,9);RefillExternalCityMarkets();assert(c->industry_inputs[ExternalIndustryCargoIndex(0,axis_coal)]==9&&c->stock[axis_coke]==6);
 c->stock[axis_tar]=1;RefillExternalCityMarkets();assert(c->industry_inputs[ExternalIndustryCargoIndex(0,axis_coal)]==1&&c->stock[axis_coke]==12);
 // Existing initialized stock is preserved; the adapter must not rewrite history.
 c->stock[axis_coke]=123;InitializeExternalCityMarket(c);assert(c->stock[axis_coke]==123);
 delete c;
 fprintf(axis_log,"PASS native callback ratios, unsupported versions/economies, no-input, small-delivery accumulation, full-byproduct backpressure, existing stock retention\n");fflush(axis_log);
}
static Train* AxisTrain(uint y,CargoType cargo,StationID station,bool coal,const char*name){
 auto result=CmdBuildVehicle(DoCommandFlag::Execute,TileXY(5,y),EngineID{13},false,INVALID_CARGO,ClientID::Invalid);Require(result,"engine");auto*t=Train::Get(result.GetResultDataWithType().GetOrDefault<VehicleID>(VehicleID::Invalid()));
 for(uint i=0;i<3;++i){auto w=CmdBuildVehicle(DoCommandFlag::Execute,TileXY(5,y),EngineID{29},false,INVALID_CARGO,ClientID::Invalid);Require(w,"wagon");const auto id=w.GetResultDataWithType().GetOrDefault<VehicleID>(VehicleID::Invalid());if(Train::Get(id)->cargo_type!=cargo)Require(CmdRefitVehicle(DoCommandFlag::Execute,id,cargo,0,false,true,0),"refit");Require(CmdMoveRailVehicle(DoCommandFlag::Execute,id,t->Last()->index,MoveRailVehicleFlags::None),"attach");}
 Require(CmdRenameVehicle(DoCommandFlag::Execute,t->index,name),"name");
 for(uint i=0;i<2;++i){Order o;o.MakeGoToDepot(Depot::GetByTile(coal&&i==0?TileXY(1,39):!coal&&i==1?TileXY(1,49):TileXY(254,y))->index,{OrderDepotTypeFlag::PartOfOrders});o.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);Require(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(t->index,VehicleOrderID(i),o)),"order");Require(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,t->index.base(),VehicleOrderID(i),MOF_DEPOT_ACTION,DA_EXTERNAL,INVALID_CARGO,{}),"external");Require(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,t->index.base(),VehicleOrderID(i),MOF_LOAD,to_underlying(i==0?OrderLoadType::FullLoad:OrderLoadType::NoLoad),INVALID_CARGO,{}),"load policy");}
 Require(CmdStartStopVehicle(DoCommandFlag::Execute,t->index,true),"start");return t;
}
void AxisTick(){
 if(_exit_game||HasModalProgress()||_generating_world)return;static bool ready=false;static uint ticks=0;static bool saved_input=false,saved_output=false,seen_coke=false;static Train*coal=nullptr,*coke=nullptr;
 const bool reload=std::getenv("PXP_AXIS_RELOAD");
 if(!axis_log){axis_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/run.log").c_str(),"w");assert(axis_log);}
 if(!ready&&_game_mode==GameMode::Editor){
  CargoLabels();for(auto&g:_grfconfig){assert(g->status==GRFStatus::Activated);assert(!g->flags.Test(GRFConfigFlag::Compatible));}
  const auto mine=AxisType("Coal Mine"),oven=AxisType("Coke Oven"),blast=AxisType("Blast Furnace");
  _current_company=OWNER_DEITY;Require(CmdFoundTown(DoCommandFlag::Execute,TileXY(200,15),TownSize::Small,false,TownLayout::Original,false,0,"AXIS recipe demo"),"town");
  Require(CmdBuildIndustry(DoCommandFlag::Execute,TileXY(230,42),mine,0,true,12344),"served native coal mine");
  Require(CmdBuildIndustry(DoCommandFlag::Execute,TileXY(210,70),oven,0,true,12345),"unserved native callback reference");
  Industry*reference=nullptr;for(auto*i:Industry::Iterate())if(i->type==oven)reference=i;assert(reference);RecipeProbe(oven,reference);
  _game_mode=GameMode::Normal;auto*company=DoStartupNewCompany(DSNC_NONE);company->money=10000000;company->current_loan=0;_current_company=_local_company;SetLocalCompany(company->index);_current_company=company->index;_pause_mode={};_settings_game.vehicle.external_connections=true;_settings_game.vehicle.external_boundary_editing=true;_settings_game.vehicle.automatic_yield=true;_settings_game.vehicle.never_expire_vehicles=true;
  StationID hub=StationID::Invalid();
  for(uint y:{40U,50U}){
   for(uint x=6;x<=254;++x)Require(CmdBuildSingleRail(DoCommandFlag::Execute,TileXY(x,y),RAILTYPE_RAIL,TRACK_X,BuildRailTrackFlags::None),"rail");
   Require(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(5,y),RAILTYPE_RAIL,DiagDirection::SW),"home");Require(CmdBuildExternalRailConnection(DoCommandFlag::Execute,TileXY(254,y),RAILTYPE_RAIL,DiagDirection::NE),"entrance");
   if(y==40){Require(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(230,y),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,hub,false),"station");hub=GetStationIndex(TileXY(230,y));}
  }
  for(uint y:{40U,50U}) {
   Require(CmdBuildSingleRail(DoCommandFlag::Execute,TileXY(6,y),RAILTYPE_RAIL,TRACK_LEFT,BuildRailTrackFlags::None),"branch");
   for(uint x=1;x<=6;++x)Require(CmdBuildSingleRail(DoCommandFlag::Execute,TileXY(x,y-1),RAILTYPE_RAIL,TRACK_X,BuildRailTrackFlags::None),"supply rail");
   Require(CmdBuildSingleRail(DoCommandFlag::Execute,TileXY(6,y-1),RAILTYPE_RAIL,TRACK_RIGHT,BuildRailTrackFlags::None),"branch join");
   Require(CmdBuildExternalRailConnection(DoCommandFlag::Execute,TileXY(1,y-1),RAILTYPE_RAIL,DiagDirection::SW),"supply entrance");
  }
  for(uint i=0;i<3;++i){const TileIndex portal=i==0?TileXY(254,40):i==1?TileXY(1,49):TileXY(1,39);Require(CmdManageExternalCity(DoCommandFlag::Execute,Depot::GetByTile(portal)->index,ExternalCityID::Invalid(),ExternalCityOperation::Create,4,i==0?"AXIS coke production city":i==1?"AXIS coke buyer":"AXIS coal supplier"),"city");auto*c=ExternalCity::Get(ExternalCityID(i));c->industry_types={i==0?oven:i==1?blast:mine,IT_INVALID,IT_INVALID};c->industries_initialized=true;c->market_initialized=false;c->statistics_initialized=false;c->stock={};InitializeExternalCityMarket(c);Require(CmdManageExternalCity(DoCommandFlag::Execute,DepotID::Invalid(),c->index,ExternalCityOperation::SetDistance,12,{}),"distance");Require(CmdManageExternalCity(DoCommandFlag::Execute,DepotID::Invalid(),c->index,ExternalCityOperation::SetStopTime,0,{}),"stop");Require(CmdManageExternalCityRegion(DoCommandFlag::Execute,DepotID::Invalid(),c->index,ExternalCityOperation::SetBoundary,0,{},ExternalBoundaryID::Invalid(),i==0?DiagDirection::SW:DiagDirection::NE,i==1?45:0,i==2?44:127),"boundary");}
  const std::string dist=std::getenv("PXP_AXIS_DIST");const auto mode=dist=="manual"?DistributionType::Manual:dist=="asymmetric"?DistributionType::Asymmetric:dist=="equal"?DistributionType::AsymmetricEqual:DistributionType::AsymmetricNearest;Require(CmdChangeSetting(DoCommandFlag::Execute,"linkgraph.distribution_default",to_underlying(mode)),"distribution");Require(CmdChangeSetting(DoCommandFlag::Execute,"linkgraph.recalc_interval",1),"graph interval");Require(CmdChangeSetting(DoCommandFlag::Execute,"linkgraph.recalc_time",1),"graph time");
  Station::Get(hub)->RecomputeCatchment();UpdateStationAcceptance(Station::Get(hub),false);
  coal=AxisTrain(40,axis_coal,hub,true,"AXIS coal input");
  // Two independent services exchange actual cargo through the off-map processor.
  coke=AxisTrain(50,axis_coke,hub,false,"AXIS coke output");
  AxisSave("axis-start");ready=true;
 }else if(!ready&&reload&&_game_mode==GameMode::Normal){CargoLabels();for(auto*t:Train::Iterate()){if(t->name=="AXIS coal input")coal=t;if(t->name=="AXIS coke output")coke=t;}assert(coal&&coke);SetLocalCompany(coal->owner);_current_company=coal->owner;_pause_mode={};ready=true;fprintf(axis_log,"LOADED hash=%llu\n",(unsigned long long)AxisHash());}
 if(!ready||_game_mode!=GameMode::Normal)return;++ticks;
 auto*a=ExternalCity::Get(ExternalCityID{0});auto*b=ExternalCity::Get(ExternalCityID{1});
 const uint64_t pending=a->industry_inputs[ExternalIndustryCargoIndex(0,axis_coal)],processed=a->delivered_units-pending;
 assert(pending<=a->delivered_units&&processed%8==0);
 assert(uint64_t(a->stock[axis_coke])+a->loaded_units==processed/8*6);
 assert(a->stock[axis_tar]==processed/8&&a->stock[axis_sulphur]==processed/8);
 uint onboard=0;for(auto*t=coke;t;t=t->Next())onboard+=t->cargo.StoredCount();if(onboard)seen_coke=true;
 if(pending&&!saved_input){saved_input=true;AxisSave("axis-input");fprintf(axis_log,"SAVED input hash=%llu\n",(unsigned long long)AxisHash());}
 if(onboard&&!saved_output){saved_output=true;AxisSave("axis-output");fprintf(axis_log,"SAVED output hash=%llu\n",(unsigned long long)AxisHash());}
 if(ticks%1000==0){fprintf(axis_log,"TICK %u input=%llu pending=%llu coke=%u sold=%llu onboard=%u pos=%u,%u orders=%u,%u income=%lld,%lld\n",ticks,(unsigned long long)a->delivered_units,(unsigned long long)pending,a->stock[axis_coke],(unsigned long long)b->delivered_units,onboard,TileX(coal->tile),TileX(coke->tile),uint(coal->current_order.GetType()),uint(coke->current_order.GetType()),(long long)(coal->profit_this_year>>8),(long long)(coke->profit_this_year>>8));fflush(axis_log);}
 if(b->delivered_units>=420&&coal->profit_this_year>0&&coke->profit_this_year>0&&seen_coke&&(_settings_game.linkgraph.GetDistributionType(axis_coal)==DistributionType::Manual||axis_routed_loaded>0)){if(!reload)assert(axis_wrong_entrances>0);fprintf(axis_log,"STRICT PASS actual AXIS off-map recipe coal=%llu coke_sold=%llu routed_loaded=%llu conservation=1 native_reference=8:6:1:1\n",(unsigned long long)a->delivered_units,(unsigned long long)b->delivered_units,(unsigned long long)axis_routed_loaded);fflush(axis_log);AxisSave("axis-complete");exit(0);}
 if(ticks>90000){fprintf(axis_log,"FAIL timeout\n");fflush(axis_log);exit(3);}
}
