#include "newgrf_config.h"
/* Private regression: native airport ground movement, flight, shared external cargo and landing. */
#include "aircraft.h"
#include "airport.h"
#include "landscape_cmd.h"
#include "air_map.h"
#include "airport_cmd.h"
#include "external_city.h"
#include "external_city_cmd.h"
#include "depot_base.h"
#include "timetable_cmd.h"
#include "settings_cmd.h"
#include "town.h"
#include "station_func.h"
#include "engine_func.h"
static FILE *flight_log;
static void FlightRequire(CommandCost cost, const char *what) { if (cost.Failed()) { fprintf(flight_log,"FAIL %s: %s\n",what,GetString(cost.GetErrorMessage()).c_str());fflush(flight_log);exit(2); } }
static void FlightSave(const std::string &name) { AutoRestoreBackup pause(_pause_mode,_pause_mode);_pause_mode.Set(PauseMode::Normal);assert(SaveOrLoad(std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/"+name+".sav",SaveLoadOperation::Save,DetailedFileType::GameFile,Subdirectory::None,false)==SaveLoadResult::Ok); }
void FlightTick()
{
 if(_game_mode==GameMode::Normal&&!HasModalProgress()&&!_generating_world){const auto*cfg=GetGRFConfig(GrfID{"\x4A\x4B\x88\x08"});assert(cfg&&cfg->status==GRFStatus::Activated&&!cfg->flags.Test(GRFConfigFlag::Compatible));}

 if (_exit_game || HasModalProgress() || _generating_world) return;
 const std::string mode=std::getenv("PXP_FLIGHT_MODE"),dist=std::getenv("PXP_FLIGHT_DIST");
 const bool reload=std::getenv("PXP_FLIGHT_RELOAD"),cross=mode=="cross",removed=mode=="removed",closed=mode=="closed",unavailable=mode=="unavailable",heli=mode=="heli";
 const bool retarget=mode=="retarget",retarget_airborne=mode=="retarget-airborne",retarget_outside=mode=="retarget-outside";
 static Aircraft *v=nullptr;static bool ready=false,saved[4]={},seen=false,returned=false,loaded=false,taxi=false,takeoff=false,paid=false;
 static bool airport_changed=false;static uint closed_ticks=0, missing_ticks=0;static uint ticks=0,deliveries=0;static int previous=-1;static StateTicks stopped=INVALID_STATE_TICKS;static Money previous_profit{};
 static bool city_changed=false,landed=false;
 if (!flight_log) { flight_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/run.log").c_str(),"w");assert(flight_log); }
 if (!ready && _game_mode==GameMode::Editor) {
  _current_company=OWNER_DEITY;FlightRequire(CmdFoundTown(DoCommandFlag::Execute,TileXY(100,15),TownSize::Small,false,TownLayout::Original,false,0,"Flight demonstration"),"town");
  _settings_game.economy.place_houses=PlaceHouses::AllowedConstructed;
  for (uint x:{36U,86U}) for(uint y=41;y<44;++y) FlightRequire(CmdPlaceHouse(DoCommandFlag::Execute,TileXY(x,y),HouseID{0},true,TownID{0},false),"passenger destination");
  _game_mode=GameMode::Normal;auto *company=DoStartupNewCompany(DSNC_NONE);company->money=10000000;company->current_loan=0;_current_company=_local_company;SetLocalCompany(company->index);_current_company=company->index;_pause_mode={};
  _settings_game.vehicle.external_connections=true;_settings_game.vehicle.external_boundary_editing=true;_settings_game.vehicle.never_expire_vehicles=true;
  _settings_game.vehicle.plane_crashes=0;
  for(uint x:{40U,90U}) FlightRequire(CmdBuildAirport(DoCommandFlag::Execute,TileXY(x,40),heli?AT_HELISTATION:AT_LARGE,0,INVALID_AIRTYPE,DiagDirection::NE,StationID::Invalid(),false),"native airport");
  auto *home=Station::GetByTile(TileXY(40,40)),*other=Station::GetByTile(TileXY(90,40));assert(home&&other&&home->airport.hangar);
  for(auto *st:{home,other}){st->RecomputeCatchment();UpdateStationAcceptance(st,false);}
  FlightRequire(CmdManageExternalCity(DoCommandFlag::Execute,DepotID::Invalid(),ExternalCityID::Invalid(),ExternalCityOperation::Create,1,"External flight city"),"city");
  const auto city=ExternalCityID{0};
  FlightRequire(CmdManageExternalCityRegion(DoCommandFlag::Execute,DepotID::Invalid(),city,ExternalCityOperation::SetBoundary,0,{},ExternalBoundaryID::Invalid(),(mode=="west"?DiagDirection::NE:mode=="north"?DiagDirection::NW:mode=="south"?DiagDirection::SE:DiagDirection::SW),1,126),"boundary");
  FlightRequire(CmdManageExternalCity(DoCommandFlag::Execute,DepotID::Invalid(),city,ExternalCityOperation::SetDistance,16,{}),"distance");
  if(retarget||retarget_airborne||retarget_outside){
   FlightRequire(CmdManageExternalCity(DoCommandFlag::Execute,DepotID::Invalid(),ExternalCityID::Invalid(),ExternalCityOperation::Create,1,"Other destination city"),"second city");
   FlightRequire(CmdManageExternalCityRegion(DoCommandFlag::Execute,DepotID::Invalid(),ExternalCityID{1},ExternalCityOperation::SetBoundary,0,{},ExternalBoundaryID::Invalid(),DiagDirection::NE,1,126),"second city boundary");
   FlightRequire(CmdManageExternalCity(DoCommandFlag::Execute,DepotID::Invalid(),ExternalCityID{1},ExternalCityOperation::SetDistance,16,{}),"second city distance");
  }
  // Lock after construction: transportation must keep working with the new default policy.
  _settings_game.vehicle.external_boundary_editing=false;
  const DistributionType distribution = dist == "manual" ? DistributionType::Manual : dist == "symmetric" ? DistributionType::Symmetric : DistributionType::Asymmetric;
  FlightRequire(CmdChangeSetting(DoCommandFlag::Execute,"linkgraph.distribution_mail",to_underlying(distribution)),"cargo distribution");
  assert(_settings_game.linkgraph.GetDistributionType(GetCargoTypeByLabel(CT_MAIL)) == distribution);
  EngineID engine{273};
  auto cost=CmdBuildVehicle(DoCommandFlag::Execute,home->airport.hangar->xy,engine,false,INVALID_CARGO,ClientID::Invalid);FlightRequire(cost,"aircraft purchase");v=Aircraft::Get(cost.GetResultDataWithType().GetOrDefault<VehicleID>(VehicleID::Invalid()));v->name="Flight subject";
  FlightRequire(CmdRefitVehicle(DoCommandFlag::Execute,v->index,GetCargoTypeByLabel(CT_GOODS),0,false,false,0),"prepare goods in native hangar");
  assert(v->cargo_type==GetCargoTypeByLabel(CT_GOODS));
  Order order;order.MakeExternalFlight(ExternalCity::Get(city)->market_station);
  fprintf(flight_log,"ORDER marker=%u market=%u city=%u boundary=%u refit=%u raw=%u type=%u\n",order.IsExternalFlightOrder(),ExternalCity::Get(city)->market_station.base(),GetExternalOrderCity(order).base(),GetExternalFlightBoundary(GetExternalOrderCity(order),v->tile).base(),order.IsRefit(),order.GetRefitCargo(),uint(v->type));fflush(flight_log);FlightRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(v->index,VehicleOrderID{0},order)),"flight order");
  FlightRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_LOAD,to_underlying(OrderLoadType::FullLoad),INVALID_CARGO,{}),"external loading");
  FlightRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_LOAD,to_underlying(OrderLoadType::CargoTypeLoad),INVALID_CARGO,{}),"cargo-specific loading");
  for(CargoType cid:EnumRange(NUM_CARGO))if(CargoSpec::Get(cid)->IsValid())FlightRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_CARGO_TYPE_LOAD,to_underlying(cid==GetCargoTypeByLabel(CT_MAIL)?OrderLoadType::FullLoad:OrderLoadType::NoLoad),cid,{}),"only mail loadable");
  FlightRequire(CmdOrderRefit(DoCommandFlag::Execute,v->index,VehicleOrderID{0},CARGO_AUTO_REFIT),"aircraft city auto refit");
  FlightRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(v->index,VehicleOrderID{1},*v->GetOrder(VehicleOrderID{0}))),"duplicate aircraft refit");
  FlightRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{1}),"remove duplicate");
  if(cross||removed)FlightRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_EXTERNAL_RETURN_DEPOT,other->index.base(),INVALID_CARGO,{}),"return airport");
  FlightRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MTF_WAIT_TIME,160,{}),"external timetable");
  order.Free();order.MakeGoToStation(cross?other->index:home->index);order.SetStopLocation(OrderStopLocation::FarEnd);order.SetLoadType(OrderLoadType::NoLoad);
  FlightRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(v->index,VehicleOrderID{1},order)),"return airport delivery");
  if(retarget){
   assert(CmdModifyOrder({},OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_EXTERNAL_FLIGHT_CITY,63,INVALID_CARGO,{}).Failed());
   assert(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{1},MOF_EXTERNAL_FLIGHT_CITY,1,INVALID_CARGO,{}).Failed());
   const Order saved=*v->GetOrder(VehicleOrderID{0});
   FlightRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_EXTERNAL_FLIGHT_CITY,1,INVALID_CARGO,{}),"retarget before departure");
   const Order*changed=v->GetOrder(VehicleOrderID{0});assert(changed->GetWaitTime()==saved.GetWaitTime()&&changed->GetLoadType()==saved.GetLoadType()&&changed->GetExternalReturnAirport()==saved.GetExternalReturnAirport());city_changed=true;
  }
  FlightRequire(CmdStartStopVehicle(DoCommandFlag::Execute,v->index,true),"start flight");FlightSave("flight-start");ready=true;
 } else if(!ready&&reload&&_game_mode==GameMode::Normal) {
  for(auto *a:Aircraft::Iterate()) if(a->name=="Flight subject")v=a;assert(v);_current_company=v->owner;SetLocalCompany(v->owner);_pause_mode={};ready=true;taxi=takeoff=true;
  if(v->external_landing){seen=returned=loaded=true;}
  city_changed=GetExternalOrderCity(*v->GetOrder(VehicleOrderID{0}))==ExternalCityID{1};
 }
 if(!ready||_game_mode!=GameMode::Normal)return;
 if(v->IsInExternalTrip()&&v->external_departure_handled){assert(v->cargo_type==GetCargoTypeByLabel(CT_MAIL));assert(v->cargo.ReservedCount()==0);assert(v->cargo.StoredCount()>0);}
++ticks;assert(!v->vehstatus.Test(VehState::Crashed));
 if(v->state==AS_RUNNING)taxi=true;
 if(v->state==AS_FLYING_TAKEOFF||v->state==AS_FLYING_HELICOPTER_TAKEOFF)takeoff=true;
 if(returned&&v->profit_this_year>previous_profit){paid=true;fprintf(flight_log,"PAYMENT tick=%u order=%u increase=%lld\n",ticks,uint(v->current_order.GetType()),(long long)(v->profit_this_year-previous_profit));fflush(flight_log);}
 if(!city_changed&&((retarget_airborne&&v->state==AS_FLYING_EXTERNAL&&!v->IsInExternalTrip())||(retarget_outside&&v->IsInExternalTrip()))){
  const auto original_city=v->external_trip_city;const auto original_return=v->external_return_tick;
  FlightRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MOF_EXTERNAL_FLIGHT_CITY,1,INVALID_CARGO,{}),"retarget during journey");
  if(retarget_outside){assert(v->external_trip_city==original_city&&v->external_return_tick==original_return&&GetExternalOrderCity(v->current_order)==ExternalCityID{0});}
  else assert(GetExternalOrderCity(v->current_order)==ExternalCityID{1});
  city_changed=true;fprintf(flight_log,"RETARGET outside=%u actual_city=%u planned_city=%u\n",retarget_outside,GetExternalOrderCity(v->current_order).base(),GetExternalOrderCity(*v->GetOrder(VehicleOrderID{0})).base());fflush(flight_log);
 }
 if(v->IsInExternalTrip()){
  seen=true;
  const uint expected_city=(retarget||retarget_airborne||(retarget_outside&&deliveries>0))?1:0;
  assert(v->tile==GetExternalFlightGateway(*ExternalBoundary::Get(ExternalBoundaryID{uint16_t(expected_city)})));
  assert(v->external_trip_city==ExternalCityID{uint16_t(expected_city)});
  if(!airport_changed&&!reload&&(removed||closed||unavailable)){
   if(removed||unavailable){_settings_game.station.allow_modify_airports=true;FlightRequire(Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute,TileXY(90,40)),"remove selected return airport");}
   if(unavailable)FlightRequire(Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute,TileXY(40,40)),"remove last airport");
   if(closed)FlightRequire(CmdOpenCloseAirport(DoCommandFlag::Execute,GetStationIndex(TileXY(40,40))),"close home airport");
   airport_changed=true;
  }
  if(unavailable&&v->external_return_pending){
   assert(v->vehstatus.Test(VehState::Hidden));
   if(++missing_ticks==96){
    for(uint x:{40U,90U})FlightRequire(CmdBuildAirport(DoCommandFlag::Execute,TileXY(x,40),AT_LARGE,0,INVALID_AIRTYPE,DiagDirection::NE,StationID::Invalid(),false),"rebuild airport while aircraft safely waits outside");
    auto *home=Station::GetByTile(TileXY(40,40));home->RecomputeCatchment();UpdateStationAcceptance(home,false);
    FlightRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{1}),"replace deleted-airport order");
    Order order;order.MakeGoToStation(home->index);order.SetStopLocation(OrderStopLocation::FarEnd);order.SetLoadType(OrderLoadType::NoLoad);
    FlightRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(v->index,VehicleOrderID{1},order)),"new airport destination");
    fprintf(flight_log,"RECOVERY no airports: hidden wait 96 ticks, rebuilt native airport\n");fflush(flight_log);
   }
  }
  int phase=v->GetExternalTripPhase()==ExternalTripPhase::Outbound?0:v->GetExternalTripPhase()==ExternalTripPhase::Stopped?1:v->GetExternalTripPhase()==ExternalTripPhase::Returning?2:-1;
  assert(v->vehstatus.Test(VehState::Hidden)&&v->cur_speed==0&&v->state==AS_FLYING_EXTERNAL);
  if(phase==1&&previous!=1)stopped=_state_ticks;
  if(phase==2){assert(v->cargo.StoredCount()==v->cargo_cap);loaded=true;if(previous==1&&stopped!=INVALID_STATE_TICKS)assert(_state_ticks-stopped>=StateTicksDelta{159});}
  if(phase>=0&&!saved[phase]&&!reload){saved[phase]=true;FlightSave("phase-"+std::to_string(phase));}
  if(phase!=previous){fprintf(flight_log,"PHASE %d cargo=%u order=%u\n",phase,v->cargo.StoredCount(),v->cur_real_order_index);fflush(flight_log);}previous=phase;
 }
 if(seen&&!v->IsInExternalTrip()&&v->external_landing&&!returned){returned=true;assert(v->cur_speed>0&&!v->vehstatus.Test(VehState::Hidden));fprintf(flight_log,"RETURN speed=%u target=%u\n",v->cur_speed,v->targetairport.base());fflush(flight_log);if(!reload&&!saved[3]){saved[3]=true;FlightSave("phase-3");}}
 if(returned&&!v->external_landing&&v->current_order.IsType(OT_LOADING))landed=true;
 // Native CargoPayment may be settled when OT_LOADING becomes OT_LEAVESTATION.
 // Require both actual terminal loading and payment, without demanding the two
 // separate events happen in the same tick.
 if(returned&&loaded&&landed&&!v->external_landing&&v->cargo.StoredCount()==0&&paid){
  assert(v->last_station_visited==GetStationIndex(TileXY(cross?90:40,40)));++deliveries;
  fprintf(flight_log,"DELIVERY %u airport=%u profit=%lld\n",deliveries,v->last_station_visited.base(),(long long)v->profit_this_year);fflush(flight_log);
  if(deliveries>=2){assert(taxi||heli);assert(takeoff||reload);fprintf(flight_log,"STRICT PASS native takeoff, boundary flight, cargo, timed stop, return and landing\n");fflush(flight_log);FlightSave("flight-complete");_exit_game=true;return;}
  seen=returned=loaded=paid=landed=false;previous=-1;
 }
 if(closed&&v->external_landing&&v->IsAircraftOnHold()){
  if(++closed_ticks==128)FlightRequire(CmdOpenCloseAirport(DoCommandFlag::Execute,GetStationIndex(TileXY(40,40))),"reopen airport after actual holding");
 }
 previous_profit=v->profit_this_year;
 if(ticks%1000==0){fprintf(flight_log,"TICK %u xy=%d,%d state=%u order=%u target=%u next=%d,%d cargo=%u speed=%u deliveries=%u\n",ticks,v->x_pos,v->y_pos,v->state,uint(v->current_order.GetType()),v->targetairport.base(),v->next_pos.x,v->next_pos.y,v->cargo.StoredCount(),v->cur_speed,deliveries);fflush(flight_log);}
 if(ticks>90000){fprintf(flight_log,"FAIL timeout\n");fflush(flight_log);exit(3);}
}
