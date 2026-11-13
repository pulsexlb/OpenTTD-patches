/* Native station turnaround: inspect reservation calls, not just final cleared bits. */
extern uint _station_departure_reservation_attempts;
extern uint _station_departure_guard_bypasses;
void ProbePendingStationRecheck(Train *);
void ProbePendingStationDirect(Train *);
void StationReverseDrivingTick()
{
 if (HasModalProgress() || _generating_world) return;
 const bool reload = std::getenv("PXP_SERVICE_RELOAD") != nullptr;
 const std::string_view mode = std::getenv("PXP_SERVICE_MODE");
 const bool straight = mode.starts_with("straight");
 const bool stuck = mode.ends_with("-stuck");
 static bool setup=false, arrived=false, saved=false;
 static uint ticks=0;
 static StationID station=StationID::Invalid(), destination=StationID::Invalid();
 if (!setup && _game_mode==GameMode::Editor) {
  demo_started=true;DemoBuild();
  DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(120,40),RAILTYPE_RAIL,Axis::X,1,8,StationClassID{0},0,StationID::Invalid(),true),"build turnaround stop");
  DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(27,40),RAILTYPE_RAIL,Axis::X,1,8,StationClassID{0},0,StationID::Invalid(),true),"build return stop");
  station=GetStationIndex(TileXY(120,40));destination=GetStationIndex(TileXY(straight?235:27,40));
  while(demo_slow->GetNumOrders())DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0}),"clear orders");
  for (StationID st : {station,destination}) {
   Order o;o.MakeGoToStation(st);o.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);o.SetLoadType(OrderLoadType::NoLoad);
   DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{demo_slow->GetNumOrders()},o)),"insert stop");
  }
  if (demo_slow->cur_real_order_index != 0) DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,demo_slow->index,VehicleOrderID{0}),"activate stop");
  _settings_game.pf.reserve_paths=true;
  setup=true;
 } else if (!setup && reload && _game_mode==GameMode::Normal) {
  demo_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/reload.log").c_str(),"w");
  for(auto *t:Train::Iterate())if(t->IsPrimaryVehicle()&&t->unitnumber==1)demo_slow=t;
  assert(demo_slow);_current_company=demo_slow->owner;SetLocalCompany(demo_slow->owner);_pause_mode={};
  station=GetStationIndex(TileXY(120,40));destination=GetStationIndex(TileXY(straight?235:27,40));
  setup=true;arrived=true;saved=true;
 }
 if(!setup||_game_mode!=GameMode::Normal)return;
 ticks++;
 if (demo_slow->last_station_visited==station && demo_slow->current_order.IsAnyLoadingType()) {
  if(!arrived)fprintf(demo_log,"ARRIVED tick=%u direction=%u speed=%u\n",ticks,uint(demo_slow->GetMovingFront()->GetMovingDirection()),demo_slow->cur_speed);
  arrived=true;
  if(!saved){ProbePendingStationRecheck(demo_slow);ProbePendingStationDirect(demo_slow);fprintf(demo_log,"DIRECT_PROBE guard_bypasses=%u\n",_station_departure_guard_bypasses);fflush(demo_log);if(_station_departure_guard_bypasses)exit(3);if(stuck){demo_slow->flags.Set(VehicleRailFlag::Stuck);demo_slow->wait_counter=0;}DemoSave("service-split");saved=true;}
 }
 if(arrived && demo_slow->last_station_visited==destination && demo_slow->current_order.IsAnyLoadingType()) {
  fprintf(demo_log,"%s station_departure mode=%s reload=%u reservation_before_target_resolution=%u arrival=1 reached_correct_destination=1 ticks=%u\n",_station_departure_reservation_attempts==0?"STRICT PASS":"FAIL",straight?"straight":"reverse",reload,_station_departure_reservation_attempts,ticks);fflush(demo_log);
  if(_station_departure_reservation_attempts!=0)exit(3);
  _exit_game=true;
 }
 if(ticks>16000){fprintf(demo_log,"FAIL timeout x=%u order=%u speed=%u\n",TileX(demo_slow->tile),uint(demo_slow->current_order.GetType()),demo_slow->cur_speed);fflush(demo_log);exit(3);}
}
