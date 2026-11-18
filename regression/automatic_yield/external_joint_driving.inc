/* Private native movement fixture: four transport modes, two shared city markets. */
#include "external_city.h"
#include "external_city_cmd.h"
#include "aircraft.h"
#include "airport.h"
#include "airport_cmd.h"
#include "roadveh.h"
#include "road_cmd.h"
#include "ship.h"
#include "water_cmd.h"
#include "terraform_cmd.h"
#include "landscape_cmd.h"
#include "depot_base.h"
#include "timetable_cmd.h"
#include "settings_cmd.h"
#include "town.h"
#include "engine_func.h"
#include "station_func.h"
#include "linkgraph/linkgraph.h"

static FILE *joint_log;
static bool joint_ready=false;
static uint joint_ticks=0;
static bool JointTransfer(){return std::string_view(std::getenv("PXP_JOINT_MODE"))=="transfer";}
static void JointRequire(CommandCost r,const char *what){if(r.Failed()){fprintf(joint_log,"FAIL %s: %s\n",what,GetString(r.GetErrorMessage()).c_str());fflush(joint_log);exit(2);}}
static uint JointCargo(const Vehicle *v){uint n=0;for(v=v->First();v;v=v->Next())n+=v->cargo.StoredCount();return n;}
static uint JointExternalCargo(const Vehicle *v){uint n=0;for(v=v->First();v;v=v->Next())for(const auto*cp:*v->cargo.Packets())if(cp->external_source_city!=UINT16_MAX)n+=cp->Count();return n;}
static uint JointCapacity(const Vehicle *v){uint n=0;for(v=v->First();v;v=v->Next())n+=v->cargo_cap;return n;}
struct JointObservation { Vehicle *v=nullptr; uint city=0, phase_mask=0, visits=0, returns=0, unloaded=0, final_units=0, transfer_units=0, city_sold=0; bool outside=false, returning=false, terminal=false; int previous_phase=-1; StateTicks stop_due=INVALID_STATE_TICKS; uint full_wait=0; };
static std::array<JointObservation,6> joint_vehicles;
static uint64_t joint_transactions=0, joint_loaded=0, joint_sold=0;
static uint64_t JointHash(){uint64_t h=1469598103934665603ULL;auto mix=[&](uint64_t x){h=(h^x)*1099511628211ULL;};
 for(const auto*c:ExternalCity::Iterate()){mix(c->index.base());mix(c->market_station.base());mix(c->loaded_units);mix(c->delivered_units);for(auto x:c->stock)mix(x);for(auto x:c->demand)mix(x);for(auto x:c->industry_inputs)mix(x);
  if(auto*st=Station::GetIfValid(c->market_station))for(CargoType cargo:EnumRange(NUM_CARGO))if(st->goods[cargo].data)for(const auto&[via,packets]:*st->goods[cargo].data->cargo.Packets()){mix(via.base());for(const auto*cp:packets){mix(cp->Count());mix(cp->GetFirstStation().base());mix(cp->external_source_city);}}
 }
 for(const Vehicle*v:Vehicle::Iterate()){mix(v->index.base());mix(v->cargo.StoredCount());mix(v->profit_this_year);mix(v->external_return_tick.base());mix(v->current_order_time);mix(v->cur_real_order_index);for(const auto*cp:*v->cargo.Packets()){mix(cp->Count());mix(cp->GetFirstStation().base());mix(cp->external_source_city);mix(cp->GetFeederShare());}}
 return h;
}
static void JointSave(const std::string& name){AutoRestoreBackup pause(_pause_mode,_pause_mode);_pause_mode.Set(PauseMode::Normal);ScrollMainWindowToTile(TileXY(230,60),true);assert(SaveOrLoad(std::string(std::getenv("PXP_JOINT_OUTPUT"))+"/"+name+".sav",SaveLoadOperation::Save,DetailedFileType::GameFile,Subdirectory::None,false)==SaveLoadResult::Ok);fprintf(joint_log,"SAVED %s hash=%llu\n",name.c_str(),(unsigned long long)JointHash());fflush(joint_log);}
static void JointInsert(Vehicle*v,uint i,const Order&o){auto result=CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(v->index,VehicleOrderID(i),o));if(result.Failed()){fprintf(joint_log,"INSERT vehicle=%u type=%u index=%u order=%u flags=%u dest=%u\n",v->index.base(),uint(v->type),i,uint(o.GetType()),uint(o.GetNonStopType()),o.GetDestination().base());fflush(joint_log);}JointRequire(result,"order insert");}
static Vehicle *JointBuy(TileIndex tile,EngineID engine,const std::string&name){auto c=CmdBuildVehicle(DoCommandFlag::Execute,tile,engine,false,INVALID_CARGO,ClientID::Invalid);JointRequire(c,"vehicle purchase");auto*v=Vehicle::Get(c.GetResultDataWithType().GetOrDefault<VehicleID>(VehicleID::Invalid()));if(!name.empty())JointRequire(CmdRenameVehicle(DoCommandFlag::Execute,v->index,name),"vehicle name");return v;}

/* Hooks observe the real service call, not a replacement loading algorithm. */
static CargoArray joint_before_stock{},joint_before_demand{},joint_before_cargo{},joint_before_delivered{};
static std::vector<CargoArray> joint_other_stock,joint_other_demand;
static uint64_t joint_before_loaded,joint_before_sold,joint_before_routed,joint_routed_loaded=0;
void JointMarketBefore(Vehicle*v,bool arrival){
 if(!joint_ready)return;auto*c=ExternalCity::Get(v->external_trip_city);joint_before_stock=c->stock;joint_before_demand=c->demand;joint_before_delivered=c->month_delivered;joint_before_loaded=c->loaded_units;joint_before_sold=c->delivered_units;joint_before_cargo={};
 for(const Vehicle*p=v->First();p;p=p->Next())if(p->cargo_type<NUM_CARGO)joint_before_cargo[p->cargo_type]+=p->cargo.StoredCount();
 joint_before_routed=0;auto*st=Station::Get(c->market_station);for(CargoType cargo:EnumRange(NUM_CARGO))if(st->goods[cargo].data)for(const auto&[via,packets]:*st->goods[cargo].data->cargo.Packets())if(via!=StationID::Invalid())for(const auto*cp:packets)joint_before_routed+=cp->Count();
 joint_other_stock.clear();joint_other_demand.clear();for(const auto*other:ExternalCity::Iterate()){joint_other_stock.push_back(other->stock);joint_other_demand.push_back(other->demand);}
}
void JointMarketAfter(Vehicle*v,bool arrival){
 if(!joint_ready)return;auto*c=ExternalCity::Get(v->external_trip_city);auto*st=Station::Get(c->market_station);CargoArray actual{};for(const Vehicle*p=v->First();p;p=p->Next())if(p->cargo_type<NUM_CARGO)actual[p->cargo_type]+=p->cargo.StoredCount();
 uint loaded=0,sold=0;for(CargoType cargo:EnumRange(NUM_CARGO)){
  const uint available=st->goods[cargo].data?st->goods[cargo].data->cargo.AvailableCount():0;
  if(c->stock[cargo]!=available){fprintf(joint_log,"STOCK MISMATCH city=%u cargo=%u displayed=%u actual=%u arrival=%u\n",c->index.base(),uint(cargo),c->stock[cargo],available,arrival);fflush(joint_log);}assert(c->stock[cargo]==available);
  if(arrival){assert(joint_before_cargo[cargo]>=actual[cargo]);const uint n=joint_before_cargo[cargo]-actual[cargo];assert(c->month_delivered[cargo]-joint_before_delivered[cargo]==n);assert(joint_before_demand[cargo]==c->demand[cargo]);sold+=n;}
  else{assert(actual[cargo]>=joint_before_cargo[cargo]);const uint n=actual[cargo]-joint_before_cargo[cargo];assert(joint_before_stock[cargo]>=n&&joint_before_stock[cargo]-c->stock[cargo]==n);loaded+=n;}
 }
 uint i=0;for(const auto*other:ExternalCity::Iterate()){if(other!=c){assert(other->stock==joint_other_stock[i]&&other->demand==joint_other_demand[i]);}++i;}
 assert(c->loaded_units-joint_before_loaded==loaded&&c->delivered_units-joint_before_sold==sold);++joint_transactions;joint_loaded+=loaded;joint_sold+=sold;for(auto&o:joint_vehicles)if(o.v==v)o.city_sold+=sold;
 uint64_t routed=0;for(CargoType cargo:EnumRange(NUM_CARGO))if(st->goods[cargo].data)for(const auto&[via,packets]:*st->goods[cargo].data->cargo.Packets())if(via!=StationID::Invalid())for(const auto*cp:packets)routed+=cp->Count();if(!arrival&&routed<joint_before_routed)joint_routed_loaded+=joint_before_routed-routed;
 if(loaded||sold){fprintf(joint_log,"TRADE tick=%u vehicle=%u city=%u loaded=%u sold=%u stock_packets_exact=1 independent_city_unchanged=1\n",joint_ticks,v->index.base(),c->index.base(),loaded,sold);fflush(joint_log);}
}
void JointDelivery(VehicleID id,const CargoPacket*cp,uint count,Money income,bool transfer){
 if(!joint_ready||cp->external_source_city==UINT16_MAX)return;
 for(auto&obs:joint_vehicles)if(obs.v&&obs.v->index==id){if(transfer)obs.transfer_units+=count;else obs.final_units+=count;fprintf(joint_log,"PAYMENT vehicle=%u source_city=%u count=%u income=%lld transfer=%u\n",id.base(),cp->external_source_city,count,(long long)income,transfer);fflush(joint_log);break;}
}

void JointTick(){
 if(_exit_game||HasModalProgress()||_generating_world)return;
 const bool reload=std::getenv("PXP_JOINT_RELOAD");const std::string mode=std::getenv("PXP_JOINT_MODE"),dist=std::getenv("PXP_JOINT_DIST");const bool freight=mode=="freight";
 static bool saved[4]={};static uint simultaneous=0;static bool graph_seen=false;
 if(!joint_log){joint_log=fopen((std::string(std::getenv("PXP_JOINT_OUTPUT"))+"/run.log").c_str(),"w");assert(joint_log);}
 if(!joint_ready&&_game_mode==GameMode::Editor){
  _current_company=OWNER_DEITY;JointRequire(CmdFoundTown(DoCommandFlag::Execute,TileXY(100,15),TownSize::Small,false,TownLayout::Original,false,0,"Intermodal demonstration"),"town");
  _settings_game.economy.place_houses=PlaceHouses::AllowedConstructed;
  for(uint x=JointTransfer()?184:234;x<(JointTransfer()?190:240);++x)JointRequire(CmdPlaceHouse(DoCommandFlag::Execute,TileXY(x,JointTransfer()?48:37),HouseID{0},true,TownID{0},false),"passenger demand houses");
  _game_mode=GameMode::Normal;auto*c=DoStartupNewCompany(DSNC_NONE);c->money=10000000;c->current_loan=0;_current_company=_local_company;SetLocalCompany(c->index);_current_company=c->index;_pause_mode={};
  _settings_game.vehicle.external_connections=true;_settings_game.vehicle.external_boundary_editing=true;_settings_game.vehicle.never_expire_vehicles=true;_settings_game.vehicle.plane_crashes=0;
  JointRequire(CmdChangeSetting(DoCommandFlag::Execute,"station.station_spread",64),"station spread");
  auto rail=[&](uint x,uint y,Track track){JointRequire(CmdBuildSingleRail(DoCommandFlag::Execute,TileXY(x,y),RAILTYPE_RAIL,track,BuildRailTrackFlags::None),"rail");};
  for(uint x=6;x<=254;++x)rail(x,40,TRACK_X);rail(230,40,TRACK_RIGHT);rail(230,40,TRACK_LOWER);for(uint y=41;y<=43;++y)rail(230,y,TRACK_Y);rail(230,44,TRACK_LEFT);for(uint x=231;x<=254;++x)rail(x,44,TRACK_X);
  JointRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(5,40),RAILTYPE_RAIL,DiagDirection::SW),"rail home");
  for(uint y:{40U,44U})JointRequire(CmdBuildExternalRailConnection(DoCommandFlag::Execute,TileXY(254,y),RAILTYPE_RAIL,DiagDirection::NE),"rail entrance");
  JointRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(235,40),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"rail station");
  const StationID hub=GetStationIndex(TileXY(235,40));
  auto road=[&](uint x,uint y,RoadBits bits){JointRequire(CmdBuildRoad(DoCommandFlag::Execute,TileXY(x,y),bits,ROADTYPE_ROAD,{},TownID::Invalid(),BuildRoadFlags::None),"road");};
  for(uint y:{50U,54U})for(uint x=6;x<=254;++x)road(x,y,ROAD_X);for(uint y=50;y<=54;++y)road(225,y,ROAD_Y);
  JointRequire(CmdBuildRoadDepot(DoCommandFlag::Execute,TileXY(5,50),ROADTYPE_ROAD,DiagDirection::SW),"road home");
  for(uint y:{50U,54U})JointRequire(CmdBuildExternalRoadConnection(DoCommandFlag::Execute,TileXY(254,y),ROADTYPE_ROAD,DiagDirection::NE),"road entrance");
  JointRequire(CmdBuildRoadStop(DoCommandFlag::Execute,TileXY(232,54),1,1,freight?RoadStopType::Truck:RoadStopType::Bus,true,DiagDirection::NE,ROADTYPE_ROAD,RoadStopClassID{0},0,hub,false),"shared road stop");
  JointRequire(CmdBuildCanal(DoCommandFlag::Execute,TileXY(254,83),TileXY(216,80),WaterClass::Canal,false),"water channel");
  for(uint x:{225U,226U})JointRequire(CmdTerraformLand(DoCommandFlag::Execute,TileXY(x,79),SLOPE_N,true),"dock shore");
  JointRequire(CmdBuildDock(DoCommandFlag::Execute,TileXY(225,79),hub,false),"shared dock");
  JointRequire(CmdBuildShipDepot(DoCommandFlag::Execute,TileXY(216,81),Axis::X,false),"shipyard");
  for(uint y:{80U,83U})JointRequire(CmdBuildShipDepot(DoCommandFlag::Execute,TileXY(254,y),Axis::X,true),"water entrance");
  JointRequire(CmdBuildAirport(DoCommandFlag::Execute,TileXY(210,60),AT_LARGE,0,INVALID_AIRTYPE,DiagDirection::NE,hub,false),"shared airport");
  JointRequire(CmdRenameStation(DoCommandFlag::Execute,hub,false,JointTransfer()?"联合换乘枢纽":"四种运输联合客运站"),"hub name");
  auto*station=Station::Get(hub);station->RecomputeCatchment();UpdateStationAcceptance(station,false);
  assert(GetStationIndex(TileXY(232,54))==hub&&GetStationIndex(TileXY(225,79))==hub&&GetStationIndex(TileXY(210,60))==hub);
  const CargoType pax=GetCargoTypeByLabel(freight?CT_GOODS:CT_PASSENGERS);assert(station->goods[pax].status.Test(GoodsEntry::State::Acceptance)!=JointTransfer());
  for(uint i=0;i<2;++i){JointRequire(CmdManageExternalCity(DoCommandFlag::Execute,DepotID::Invalid(),ExternalCityID::Invalid(),ExternalCityOperation::Create,1,i==0?"邻市·四种运输共享":"远市·独立库存"),"city");auto*city=ExternalCity::Get(ExternalCityID{uint16_t(i)});
   if(freight){city->industry_types={IndustryType{4},IT_INVALID,IT_INVALID};city->industries_initialized=true;city->market_initialized=false;city->stock={};city->demand={};InitializeExternalCityMarket(city);city->stock[pax]=4096;city->industry_inputs[ExternalIndustryCargoIndex(0,GetCargoTypeByLabel(CT_OIL))]=1024;} // Opening finished goods and refinery inputs; subsequent production/routing is native.
   // Explicit pre-service shortage fixture; all subsequent supply is native monthly production.
   if(mode=="shortage"&&i==0)city->stock[pax]=16;
   JointRequire(CmdManageExternalCityRegion(DoCommandFlag::Execute,DepotID::Invalid(),city->index,ExternalCityOperation::SetBoundary,0,{},ExternalBoundaryID::Invalid(),i==0?DiagDirection::SW:DiagDirection::NW,1,i==0?126:254),"boundary");
   JointRequire(CmdManageExternalCity(DoCommandFlag::Execute,DepotID::Invalid(),city->index,ExternalCityOperation::SetDistance,24,{}),"city distance");
  }
  for(const char*setting:{"linkgraph.distribution_pax","linkgraph.distribution_mail","linkgraph.distribution_default"})JointRequire(CmdChangeSetting(DoCommandFlag::Execute,setting,dist=="manual"?0:dist=="symmetric"&&std::string_view(setting)!="linkgraph.distribution_default"?2:1),"distribution");
  for(const char*setting:{"linkgraph.recalc_interval","linkgraph.recalc_time"})JointRequire(CmdChangeSetting(DoCommandFlag::Execute,setting,1),"link graph schedule");
  joint_vehicles[0].v=JointBuy(TileXY(5,40),EngineID{13},"联运·铁路");auto*train=Train::From(joint_vehicles[0].v);
  for(uint n=0;n<3;++n){auto*w=JointBuy(TileXY(5,40),EngineID{uint16_t(freight?32:27)},"");JointRequire(CmdMoveRailVehicle(DoCommandFlag::Execute,w->index,train->Last()->index,MoveRailVehicleFlags::None),"attach carriage");}
  joint_vehicles[1].v=JointBuy(TileXY(5,50),EngineID{uint16_t(freight?138:118)},"联运·公路");joint_vehicles[2].v=JointBuy(TileXY(216,81),EngineID{uint16_t(freight?212:207)},"联运·水运");
  EngineID plane=EngineID::Invalid();for(const Engine*e:Engine::IterateType(VehicleType::Aircraft))if(IsEngineBuildable(e->index,VehicleType::Aircraft,c->index)&&AircraftVehInfo(e->index)->subtype==1){plane=e->index;break;}assert(plane!=EngineID::Invalid());
  joint_vehicles[3].v=JointBuy(station->airport.hangar->xy,plane,"联运·航空");joint_vehicles[4].v=JointBuy(station->airport.hangar->xy,plane,"远市·独立航班");joint_vehicles[4].city=1;
  if(freight)for(uint i:{3U,4U})JointRequire(CmdRefitVehicle(DoCommandFlag::Execute,joint_vehicles[i].v->index,pax,0,false,false,0),"native aircraft cargo refit");
  const TileIndex entries[]={TileXY(254,40),TileXY(254,50),TileXY(254,80)},returns[]={TileXY(254,44),TileXY(254,54),TileXY(254,83)};
  for(uint i=0;i<5;++i){auto*v=joint_vehicles[i].v;Order o;if(i<3)o.MakeGoToDepot(Depot::GetByTile(entries[i])->index,{OrderDepotTypeFlag::PartOfOrders},ONSF_STOP_EVERYWHERE);else o.MakeExternalFlight(ExternalCity::Get(ExternalCityID{uint16_t(i==4)})->market_station);JointInsert(v,0,o);
   if(i<3){JointRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_DEPOT_ACTION,DA_EXTERNAL,INVALID_CARGO,{}),"external action");JointRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_EXTERNAL_RETURN_DEPOT,Depot::GetByTile(returns[i])->index.base(),INVALID_CARGO,{}),"alternate return");}
   JointRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_LOAD,to_underlying(OrderLoadType::FullLoad),INVALID_CARGO,{}),"full load");JointRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MTF_WAIT_TIME,160,{}),"timed city stop");
   o.Free();o.MakeGoToStation(hub);if(i<2)o.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);o.SetStopLocation(OrderStopLocation::FarEnd);o.SetLoadType(mode=="roundtrip"?OrderLoadType::LoadIfPossible:OrderLoadType::NoLoad);if(JointTransfer())o.SetUnloadType(OrderUnloadType::Transfer);JointInsert(v,1,o);JointRequire(CmdStartStopVehicle(DoCommandFlag::Execute,v->index,true),"start vehicle");
  }
  if(JointTransfer()){
   JointRequire(CmdBuildRoadStop(DoCommandFlag::Execute,TileXY(188,50),1,1,RoadStopType::Bus,true,DiagDirection::NE,ROADTYPE_ROAD,RoadStopClassID{0},0,StationID::Invalid(),false),"final destination stop");const StationID final=GetStationIndex(TileXY(188,50));Station::Get(final)->RecomputeCatchment();UpdateStationAcceptance(Station::Get(final),false);assert(Station::Get(final)->goods[pax].status.Test(GoodsEntry::State::Acceptance));
   auto*v=JointBuy(TileXY(5,50),EngineID{118},"换乘接驳·最终交付");joint_vehicles[5].v=v;Order o;o.MakeGoToStation(hub);o.SetStopLocation(OrderStopLocation::FarEnd);o.SetLoadType(OrderLoadType::FullLoadAny);JointInsert(v,0,o);o.MakeGoToStation(final);o.SetStopLocation(OrderStopLocation::FarEnd);o.SetLoadType(OrderLoadType::NoLoad);JointInsert(v,1,o);JointRequire(CmdStartStopVehicle(DoCommandFlag::Execute,v->index,true),"start feeder");
  }
  JointRequire(CmdChangeSetting(DoCommandFlag::Execute,"vehicle.external_boundary_editing",0),"lock boundaries after construction");joint_ready=true;JointSave("ready");
 }else if(!joint_ready&&reload&&_game_mode==GameMode::Normal){
  const std::array<std::string,6> names={"联运·铁路","联运·公路","联运·水运","联运·航空","远市·独立航班","换乘接驳·最终交付"};for(auto*v:Vehicle::Iterate())for(uint i=0;i<names.size();++i)if(v->name==names[i].c_str())joint_vehicles[i].v=v;
  for(uint i=0;i<5;++i)assert(joint_vehicles[i].v);joint_vehicles[4].city=1;SetLocalCompany(joint_vehicles[0].v->owner);_current_company=_local_company;_pause_mode={};joint_ready=true;fprintf(joint_log,"LOADED hash=%llu\n",(unsigned long long)JointHash());fflush(joint_log);
 }
 if(!joint_ready||_game_mode!=GameMode::Normal)return;++joint_ticks;
 uint outside=0;bool finished=true;for(uint i=0;i<5;++i){auto&o=joint_vehicles[i];auto*v=o.v;assert(!v->vehstatus.Test(VehState::Crashed));
  if(v->IsInExternalTrip()){
   ++outside;assert(v->external_trip_city==ExternalCityID{uint16_t(o.city)});const auto phase=v->GetExternalTripPhase();const int pi=phase==ExternalTripPhase::Outbound?0:phase==ExternalTripPhase::Stopped?1:phase==ExternalTripPhase::Returning?2:-1;
   assert(v->GetExternalDisplaySpeed()==(phase==ExternalTripPhase::Stopped?0:v->external_cruise_speed));
   if(!o.outside){o.outside=true;o.returning=false;o.terminal=false;++o.visits;fprintf(joint_log,"ENTER vehicle=%u mode=%u city=%u visits=%u\n",v->index.base(),uint(v->type),o.city,o.visits);}
   if(pi>=0)o.phase_mask|=1U<<pi;
   if(pi==1&&v->external_arrival_handled){if(o.stop_due==INVALID_STATE_TICKS)o.stop_due=v->external_departure_tick;if(ExternalCityVehicleNeedsFullLoad(v)&&_state_ticks>=o.stop_due)++o.full_wait;}
   if(pi==2){assert(!ExternalCityVehicleNeedsFullLoad(v));if(o.stop_due!=INVALID_STATE_TICKS)assert(_state_ticks>=o.stop_due);}
   o.previous_phase=pi;
   if(pi>=0&&!saved[pi]&&!reload){saved[pi]=true;JointSave("phase-"+std::to_string(pi));}
  }else if(o.outside&&!v->IsInDepot()&&!o.returning){o.returning=true;o.outside=false;++o.returns;assert(v->cur_speed>0);o.previous_phase=-1;o.stop_due=INVALID_STATE_TICKS;fprintf(joint_log,"RETURN vehicle=%u speed=%u cargo=%u\n",v->index.base(),v->cur_speed,JointCargo(v));}
  if(o.returning&&v->current_order.IsAnyLoadingType())o.terminal=true;
  if(o.returning&&o.terminal&&JointExternalCargo(v)==0){++o.unloaded;o.returning=false;fprintf(joint_log,"UNLOAD vehicle=%u visits=%u final=%u transferred=%u\n",v->index.base(),o.unloaded,o.final_units,o.transfer_units);}
  finished&=o.unloaded>=2&&o.phase_mask==7&&(JointTransfer()?o.transfer_units>0:o.final_units>0)&&(mode!="roundtrip"||o.city_sold>0);
 }
 simultaneous=std::max(simultaneous,outside);
 for(const auto*c:ExternalCity::Iterate()){const auto*st=Station::Get(c->market_station);for(CargoType cargo:EnumRange(NUM_CARGO))if(!st->goods[cargo].ConstFlows().empty())graph_seen=true;}
 if(outside>=3&&!saved[3]&&!reload){saved[3]=true;JointSave("concurrent");}
 if(JointTransfer())finished&=joint_vehicles[5].final_units>0;
 if(finished&&(!JointTransfer()||joint_vehicles[5].final_units>=100)&&(dist=="manual"||(graph_seen&&joint_routed_loaded>0))){
  uint full_wait=0;for(const auto&o:joint_vehicles)full_wait+=o.full_wait;
  assert(joint_transactions>0&&joint_loaded>0);if(mode=="shortage"&&!reload)assert(full_wait>0);
  fprintf(joint_log,"STRICT PASS joint mode=%s distribution=%s four_modes=1 two_cities=1 transactions=%llu loaded=%llu sold=%llu full_wait=%u simultaneous=%u graph=%u routed_loaded=%llu final_transfer=%u\n",mode.c_str(),dist.c_str(),(unsigned long long)joint_transactions,(unsigned long long)joint_loaded,(unsigned long long)joint_sold,full_wait,simultaneous,graph_seen,(unsigned long long)joint_routed_loaded,joint_vehicles[5].final_units);JointSave("complete");_exit_game=true;
 }
 if(joint_ticks%2000==0){
  const CargoType observed=GetCargoTypeByLabel(freight?CT_GOODS:CT_PASSENGERS);
  fprintf(joint_log,"DISTRIBUTION effective=%u routed_loaded=%llu graph_seen=%u\n",uint(_settings_game.linkgraph.GetDistributionType(observed)),(unsigned long long)joint_routed_loaded,graph_seen);
  for(const auto*st:Station::Iterate()){const auto&ge=st->goods[observed];const auto*g=LinkGraph::GetIfValid(ge.link_graph);if(g)fprintf(joint_log,"GRAPH station=%u city=%u size=%u supply=%u demand=%u flows=%u available=%u\n",st->index.base(),st->external_city,g->Size(),(*g)[ge.node].Supply(),(*g)[ge.node].Demand(),uint(ge.ConstFlows().size()),ge.data?ge.data->cargo.AvailableCount():0);}
  for(uint i=0;i<5;++i){const auto&o=joint_vehicles[i];auto*v=o.v;fprintf(joint_log,"TICK %u vehicle=%u xy=%u,%u phase=%u speed=%u cargo=%u visits=%u returns=%u unloaded=%u order=%u\n",joint_ticks,v->index.base(),TileX(v->tile),TileY(v->tile),uint(v->GetExternalTripPhase()),v->cur_speed,JointCargo(v),o.visits,o.returns,o.unloaded,uint(v->current_order.GetType()));}fflush(joint_log);}
 if(joint_ticks>180000){fprintf(joint_log,"FAIL timeout\n");fflush(joint_log);exit(3);}
}
