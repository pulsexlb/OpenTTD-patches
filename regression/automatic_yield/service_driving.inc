/* Real native arrivals and PXP split/couple lifecycle; no forced speed or position. */
#include "timetable_cmd.h"
#include "waypoint_cmd.h"
#include "newgrf_station.h"
void StationServiceDrivingTick()
{
 if (HasModalProgress() || _generating_world) return;
 const std::string mode = std::getenv("PXP_SERVICE_MODE");
 const bool split = mode != "station";
 const bool reload = std::getenv("PXP_SERVICE_RELOAD") != nullptr;
 const bool checkpoint_coupling = mode.starts_with("recouple-checkpoint");
 const bool reversed_checkpoint = mode == "recouple-checkpoint-reversed";
 const bool waypoint_coupling = mode.starts_with("recouple-waypoint") || checkpoint_coupling;
 const bool late_partner = mode == "recouple-waypoint-late" || checkpoint_coupling;
 const bool auto_slot = mode.starts_with("recouple-slot");
 const bool full_auto_slot = mode == "recouple-slot-full";
 const bool recoupling = mode == "recouple" || waypoint_coupling || auto_slot;
 static TraceRestrictSlot *wait_slot = nullptr;
 static uint full_wait_ticks = 0;
 static bool full_slot_released = false;
 static bool waypoint_waited = false;
 static VehicleID late_target = VehicleID::Invalid();
 static uint late_stuck_ticks = 0;
 static bool late_released = false;
 static bool checkpoint_leg_reached = false;
 const bool dispatch = mode.starts_with("split-dispatch");
 const bool shared_dispatch = mode.starts_with("split-dispatch-shared");
 const bool shared_both = mode == "split-dispatch-shared-both";
 const std::string first_number = dispatch ? "D123" : "G123";
 static bool setup = false, first = false, saved = false, recouple_posted = false, return_posted = false;
 static bool before_arrival_saved = false;
 static uint shared_wait_ticks = 0;
 static bool shared_departed = false;
 static uint ticks = 0;
 static StationID first_station = StationID::Invalid(), second_station = StationID::Invalid();
 if (!setup && _game_mode == GameMode::Editor) {
  demo_started = true;
  DemoBuild();
  first_station = StationID::Invalid();
  DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute, TileXY(120,40), RAILTYPE_RAIL, Axis::X, 1, 8, StationClassID{0}, 0, StationID::Invalid(), true), "build intermediate stop");
  first_station = GetStationIndex(TileXY(120,40)); second_station = GetStationIndex(TileXY(235,40));
  Order stop; stop.MakeGoToStation(first_station); stop.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
  DemoRequire(CmdInsertOrder(DoCommandFlag::Execute, InsertOrderCmdData(demo_slow->index, VehicleOrderID{0}, stop)), "insert intermediate stop");
  DemoRequire(CmdSetOrderServiceNumber(DoCommandFlag::Execute, OrderTargetType::Vehicle, demo_slow->index.base(), VehicleOrderID{0}, 1, "G123"), "first stop number");
  DemoRequire(CmdSetOrderServiceNumber(DoCommandFlag::Execute, OrderTargetType::Vehicle, demo_slow->index.base(), VehicleOrderID{1}, 1, "G456"), "second stop number");
  DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute, OrderTargetType::Vehicle, demo_slow->index.base(), VehicleOrderID{0}, MTF_WAIT_TIME, 64, {}), "first station dwell");
  if (split) {
   DemoRequire(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::Vehicle, demo_slow->index.base(), VehicleOrderID{0}, MOF_DECOUPLE, ODF_DECOUPLE, INVALID_CARGO, {}), "enable actual decoupling");
   DemoRequire(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::Vehicle, demo_slow->index.base(), VehicleOrderID{1}, MOF_DECOUPLE_VALUE, mode == "split-both" ? 2 : 1, INVALID_CARGO, {}), "split count");
   if (mode == "split-load") DemoRequire(CmdModifyOrder(DoCommandFlag::Execute, OrderTargetType::Vehicle, demo_slow->index.base(), VehicleOrderID{1}, MOF_FIRST_ORDERS, ODOF_KEEP_ORDERS, INVALID_CARGO, {}), "engine also loads");
  }
  if (dispatch) {
   if (shared_dispatch) DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{1},MOF_SECOND_ORDERS,ODOF_KEEP_ORDERS,INVALID_CARGO,{}),"wagons retain shared dispatch list");
   if (shared_both) DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{1},MOF_FIRST_ORDERS,ODOF_KEEP_ORDERS,INVALID_CARGO,{}),"both parts retain shared dispatch list");
   if (mode == "split-dispatch-shared-schedule") {
    OrderList *independent=OrderList::Create();independent->SetCompany(demo_slow->owner);independent->InitializePlayerCreated();
    Order exit;exit.MakeGoToStation(second_station);exit.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);independent->InsertOrderAt(std::move(exit),0);
    DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{1},MOF_DECOUPLE_FIRST_SCHEDULE,independent->index.base(),INVALID_CARGO,{}),"engine adopts independent schedule");
   }
   auto &schedules = demo_slow->orders->GetScheduledDispatchScheduleSet(); schedules.emplace_back();
   schedules[0].SetScheduledDispatchDuration(10000); schedules[0].SetScheduledDispatchStartTick(_state_ticks);
   schedules[0].SetSupplementaryName(DispatchSchedule::SupplementaryNameType::RouteID, 1, "D123"); schedules[0].AddScheduledDispatch(0, 0, 1);
   DemoRequire(CmdTimetableSeparation(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),false),"select dispatch instead of automatic separation");
   DemoRequire(CmdSchDispatchSetEnabled(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),true),"enable real dispatch");
   DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MTF_ASSIGN_SCHEDULE,0,{}),"assign dispatch to split stop");
   DemoRequire(CmdSetOrderServiceNumber(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},2,""),"use dispatch number at split stop");
  }
  DemoRequire(CmdSetVehicleServiceNumber(DoCommandFlag::Execute, demo_slow->index, "OLD"), "inbound number");
  DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute, demo_slow->index, VehicleOrderID{0}), "activate first stop");
  assert(demo_slow->GetActiveServiceNumber() == "OLD");
  assert(demo_slow->GetServiceNumberForDeparture(demo_slow->GetOrder(0), first_number) == first_number);
  if (mode == "split-implicit") {
   /* Recreate the two order indices left by an intermediate implicit stop.
    * Let the train drive to the scheduled stop and run the real split. */
   Order implicit;
   implicit.MakeImplicit(second_station);
   demo_slow->orders->InsertOrderAt(std::move(implicit), 0);
   demo_slow->cur_implicit_order_index = 0;
   demo_slow->cur_real_order_index = 1;
  }
  fprintf(demo_log, "SERVICE inbound=OLD first_outbound=G123 second_outbound=G456 mode=%s\n", mode.c_str());
  setup = true;
 } else if (!setup && reload && _game_mode == GameMode::Normal) {
  demo_log = fopen((std::string(std::getenv("PXP_DEMO_OUTPUT")) + "/reload.log").c_str(), "w");
  for (auto *t : Train::Iterate()) if (t->IsPrimaryVehicle()) { if (t->unitnumber == 1) demo_slow=t; if (t->unitnumber == 2 && t->IsEngine()) demo_fast=t; }
  assert(demo_slow != nullptr);
  first_station=GetStationIndex(TileXY(120,40)); second_station=GetStationIndex(TileXY(235,40));
  _current_company=demo_slow->owner;SetLocalCompany(demo_slow->owner);_pause_mode={};
  setup=true; first=!split;
  if(checkpoint_coupling){
   recouple_posted=true;
   for(const Train*t:Train::Iterate())if(t->IsPrimaryVehicle()&&t!=demo_slow&&t!=demo_fast&&t->last_station_visited==first_station)late_target=t->index;
   assert(late_target!=VehicleID::Invalid());
   assert(demo_slow->flags.Test(VehicleRailFlag::CoupleCheckReversePending));
   assert(demo_slow->cur_speed==0);
  }
  if (!split) assert(demo_slow->GetActiveServiceNumber()=="G123");
 }
 if (!setup || _game_mode != GameMode::Normal) return;
 ticks++;
 if(checkpoint_coupling&&ticks%256==0){
  const Train*target=Train::GetIfValid(late_target);
  fprintf(demo_log,"CHECKPOINT tick=%u xy=%u,%u speed=%u flags=%u reverse=%u type=%u dest=%u real=%u target=%u partner_order=%u partner_status=%u\n",ticks,TileX(demo_slow->GetMovingFront()->tile),TileY(demo_slow->GetMovingFront()->tile),demo_slow->cur_speed,demo_slow->flags.base(),demo_slow->reverse_distance,uint(demo_slow->current_order.GetType()),demo_slow->current_order.GetDestination().base(),uint(demo_slow->cur_real_order_index),demo_slow->couple_target.base(),target?uint(target->current_order.GetType()):UINT_MAX,target?target->vehstatus.base():0);fflush(demo_log);
 }
 if (shared_dispatch && !reload && !before_arrival_saved && !saved && TileX(demo_slow->GetMovingFront()->tile)>=118) {
  DemoSave("dispatch-before-arrival");before_arrival_saved=true;
 }
 const bool collector_waits = checkpoint_coupling ? demo_slow->flags.Test(VehicleRailFlag::CoupleCheckReservationReleased) : demo_slow->current_order.IsType(OT_GOTO_COUPLE) && demo_slow->flags.Test(VehicleRailFlag::Stuck);
 if (late_partner && late_target != VehicleID::Invalid() && !late_released && collector_waits && ++late_stuck_ticks == 60) {
  Train *target = Train::Get(late_target);
  assert(target->vehstatus.Test(VehState::Stopped));
  /* A native turnaround while held gives the saved pending-checkpoint
   * state both possible orientations, without moving or stopping by hand. */
  if(reversed_checkpoint&&!reload)DemoRequire(CmdReverseTrainDirection(DoCommandFlag::Execute,demo_slow->index,false),"turn held checkpoint collector away from next waypoint");
  if(checkpoint_coupling&&!reload)DemoSave("couple-checkpoint-waiting");
  DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,target->index,true),"enable newly available partner behind waypoint");
  if(checkpoint_coupling&&!reload)DemoSave("couple-checkpoint-ready");
  late_released = true;
  fprintf(demo_log,"LATE partner available tick=%u blocked_ticks=%u\n",ticks,late_stuck_ticks);
 }
 if(checkpoint_coupling && collector_waits && demo_slow->cur_speed==0) waypoint_waited=true;
 if(checkpoint_coupling&&late_released&&!checkpoint_leg_reached){
  if(TileX(demo_slow->GetMovingFront()->tile)>152){
   fprintf(demo_log,"FAIL checkpoint departed away from its intermediate waypoint tick=%u x=%u\n",ticks,TileX(demo_slow->GetMovingFront()->tile));fflush(demo_log);exit(3);
  }
  if(demo_slow->cur_real_order_index==2){
   assert(demo_slow->last_station_visited==GetStationIndex(TileXY(140,40)));
   checkpoint_leg_reached=true;
   fprintf(demo_log,"CHECKPOINT correct_next_waypoint_reached tick=%u reversed_fixture=%u\n",ticks,reversed_checkpoint);
  }
 }
 if (waypoint_coupling && demo_slow->current_order.IsType(OT_WAITING) && demo_slow->cur_speed == 0 && IsRailWaypointTile(demo_slow->GetMovingFront()->tile)) {
  if (!waypoint_waited) { fprintf(demo_log,"WAYPOINT held tick=%u returning_to_carriages_behind=1\n",ticks); DemoSave("couple-waypoint-stopped"); }
  waypoint_waited = true;
 }
 if (!reload && ticks==350) DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_fast->index,true), "start express");
 if (!split) {
  if (!first && demo_slow->last_station_visited==first_station && demo_slow->current_order.IsAnyLoadingType()) {
   assert(demo_slow->GetActiveServiceNumber()=="G123");first=true;
   fprintf(demo_log,"ARRIVED first tick=%u actual=G123\n",ticks);
   DemoSave("service-first");
  }
  if (first && demo_slow->last_station_visited==second_station && demo_slow->current_order.IsAnyLoadingType()) {
   assert(demo_slow->GetActiveServiceNumber()=="G456");
   fprintf(demo_log,"STRICT PASS station actual_first=G123 actual_second=G456 reload=%u ticks=%u\n",reload,ticks);fflush(demo_log);_exit_game=true;
  }
 } else {
  Train *carriages=nullptr;
  for(auto *t:Train::Iterate())if(t->IsPrimaryVehicle()&&t!=demo_slow&&t!=demo_fast&&t->last_station_visited==first_station)carriages=t;
  if(carriages!=nullptr && demo_slow->last_station_visited==first_station) {
   if(auto_slot&&!carriages->current_order.IsType(OT_WAIT_COUPLE))return;
   assert(carriages->GetActiveServiceNumber()==first_number);
   if(mode=="split-both")assert(demo_slow->GetActiveServiceNumber()==first_number);
   else {assert(demo_slow->GetActiveServiceNumber().empty());assert(demo_slow->service_number_suppressed);}
   if (auto_slot && wait_slot == nullptr) {
    for (auto *slot:TraceRestrictSlot::Iterate()) if(slot->name=="service-auto-wait")wait_slot=slot;
    if(wait_slot==nullptr){wait_slot=TraceRestrictSlot::Create(carriages->owner,VehicleType::Train);wait_slot->name="service-auto-wait";wait_slot->max_occupancy=1;if(full_auto_slot)assert(wait_slot->Occupy(demo_fast));}
    DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,carriages->index.base(),carriages->cur_real_order_index,MOF_WAIT_COUPLE_SLOT,wait_slot->index.base(),INVALID_CARGO,{}),"wait acquires automatic slot");
    DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,carriages->index.base(),carriages->cur_real_order_index,MOF_WAIT_COUPLE_COMPANY,carriages->owner.base()+1,INVALID_CARGO,{}),"waiting company restriction");
    /* The command selects the slot; actual acquisition belongs to the next native train tick. */
   }
   if(auto_slot&&!full_auto_slot){
    if(!wait_slot->IsOccupant(carriages->index))return;
    assert(carriages->wait_couple_acquired_slot==wait_slot->index);
   }
   if(full_auto_slot&&!full_slot_released){
    assert(wait_slot->IsOccupant(demo_fast->index));assert(!wait_slot->IsOccupant(carriages->index));assert(carriages->cur_speed==0);
    if(++full_wait_ticks>=200){wait_slot->Vacate(demo_fast);full_slot_released=true;fprintf(demo_log,"AUTO_SLOT full_wait_ticks=%u released=1\n",full_wait_ticks);}
   }
   if(!saved){
    if(shared_dispatch){
     const auto &schedule=carriages->orders->GetDispatchScheduleByIndex(0);
     const auto record=carriages->dispatch_records.find(0);assert(record!=carriages->dispatch_records.end());
     if(_state_ticks<record->second.dispatched){assert(carriages->current_order.IsAnyLoadingType());assert(carriages->cur_speed==0);}
     /* This fixture arrives during the first period: a single arrival must
      * consume only its first future slot, even when two parts load. */
     assert(schedule.GetScheduledDispatchLastDispatch()==10000);
     assert(record->second.dispatched==schedule.GetScheduledDispatchStartTick()+10000);
     if(shared_both){const auto engine_record=demo_slow->dispatch_records.find(0);assert(engine_record!=demo_slow->dispatch_records.end());assert(engine_record->second.dispatched==record->second.dispatched);}
     fprintf(demo_log,"DISPATCH_ONCE shared=1 both=%u last=%d departure=%lld state=%lld wagon_lateness=%d reload=%u\n",shared_both,schedule.GetScheduledDispatchLastDispatch(),(long long)record->second.dispatched.base(),(long long)_state_ticks.base(),carriages->lateness_counter,reload);
    }
    fprintf(demo_log,"SPLIT tick=%u locomotive=%u number=%s suppressed=%u carriages=%u number=%s\n",ticks,demo_slow->index.base(),demo_slow->GetActiveServiceNumber().c_str(),demo_slow->service_number_suppressed,carriages->index.base(),carriages->GetActiveServiceNumber().c_str());
    assert(carriages->unitnumber != 0 && carriages->Primary() == carriages);
    for (const Train *part = carriages->First(); part != nullptr; part = part->Next()) {
     /* Upstream keeps physical-head subtype flags separately from the
      * logical identity. Every member must resolve to the one owner. */
     assert(part->Primary() == carriages);
     assert(part == carriages || part == carriages->First() || !part->IsPrimaryVehicle());
    }
    if (!reload) {
     const bool legacy_carrier = mode == "split-legacy-carrier";
     if (legacy_carrier) carriages->ClearFrontWagon();
     DemoSave("service-split");
     if (legacy_carrier) carriages->SetFrontWagon();
    }
    saved=true;
   }
   if(recoupling&&!recouple_posted){
    /* The completed split stop must not execute a second split immediately after recoupling. */
    DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0}),"remove completed split stop from engine schedule");
    Order couple;couple.MakeGoToCouple();
    DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{0},couple)),"insert recouple");
    DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MOF_COUPLE_STATION,first_station.base()+1,INVALID_CARGO,{}),"recouple station");
    if (waypoint_coupling) {
     DemoRequire(CmdBuildRailWaypoint(DoCommandFlag::Execute,TileXY(150,40),Axis::X,1,1,StationClassID{STAT_CLASS_WAYP},0,StationID::Invalid(),true),"build stop before reverse coupling");
     Order point;point.MakeGoToWaypoint(GetStationIndex(TileXY(150,40)));point.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);point.SetWaypointFlags(checkpoint_coupling ? OrderWaypointFlags{OrderWaypointFlag::Reverse} : OrderWaypointFlags{OrderWaypointFlag::Stop});
     DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{0},point)),"stop waypoint before coupling");
     DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MTF_WAIT_TIME,64,{}),"waypoint coupling dwell");
     if(checkpoint_coupling){
      DemoSignal(135,40);
      SetPresentSignals(TileXY(135,40),SignalAlongTrackdir(TRACKDIR_X_NE));
      DemoRequire(CmdBuildRailWaypoint(DoCommandFlag::Execute,TileXY(140,40),Axis::X,1,1,StationClassID{STAT_CLASS_WAYP},0,StationID::Invalid(),true),"build intermediate coupling waypoint");
      Order via;via.MakeGoToWaypoint(GetStationIndex(TileXY(140,40)));via.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
      DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{1},via)),"retain intermediate coupling leg");
      DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{2},MOF_COUPLE_CHECK_POINT,GetStationIndex(TileXY(150,40)).base()+1,INVALID_CARGO,{}),"check partner before intermediate leg");
     }
     if (late_partner) {
      DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,carriages->index,true),"hold partner until collector waits behind waypoint");
      late_target = carriages->index;
     }
    }
    DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,demo_slow->index,VehicleOrderID{0}),"activate recouple");
    recouple_posted=true;
   }
   if(!recoupling) {
    if(shared_both){
     const auto record=carriages->dispatch_records.find(0);assert(record!=carriages->dispatch_records.end());
     if(demo_slow->current_order.IsAnyLoadingType()){
      assert(demo_slow->cur_speed==0);shared_wait_ticks++;
     }else if(!shared_departed){
      assert(_state_ticks>=record->second.dispatched);assert(shared_wait_ticks>1000);
      shared_departed=true;fprintf(demo_log,"DISPATCH_DEPARTURE tick=%lld selected=%lld waited=%u reload=%u\n",(long long)_state_ticks.base(),(long long)record->second.dispatched.base(),shared_wait_ticks,reload);
     }
     if(!shared_departed || TileX(demo_slow->GetMovingFront()->tile)<=130)return;
     /* An unpowered part may legally advance past loading at the booked
      * time; it still cannot move until a locomotive couples to it. */
     assert(carriages->cur_speed==0);
    }
    fprintf(demo_log,"STRICT PASS %s engine_only_cleared=1 carriage_number_retained=1 actual_dispatch_departure=%u reload=%u\n",mode.c_str(),shared_departed,reload);fflush(demo_log);_exit_game=true;
   }
  }
  if(recoupling&&recouple_posted&&demo_slow->GetActiveServiceNumber()=="G123") {
   if(checkpoint_coupling)assert(checkpoint_leg_reached);
   uint wagons=0;for(const Train*t=demo_slow->First();t;t=t->Next())if(t->IsWagon()&&!t->IsArticulatedPart())wagons++;
   if(wagons==3){if(auto_slot){assert(wait_slot!=nullptr&&wait_slot->occupants.empty());assert(demo_slow->wait_couple_acquired_slot==TraceRestrictSlotID::Invalid());if(full_auto_slot)assert(full_slot_released&&full_wait_ticks>=200);fprintf(demo_log,"AUTO_SLOT acquired_and_returned=1 reload=%u\n",reload);}if(waypoint_coupling)assert(waypoint_waited);if(late_partner)assert(late_released&&late_stuck_ticks>=60);fprintf(demo_log,"STRICT PASS %s actual_wagons=3 restored=G123 waypoint_waited=%u reload=%u ticks=%u\n",mode.c_str(),waypoint_waited,reload,ticks);fflush(demo_log);_exit_game=true;}
  }
  if((mode=="recouple"||auto_slot)&&recouple_posted&&!return_posted&&TileX(demo_slow->GetMovingFront()->tile)>=130) {
   DemoRequire(CmdReverseTrainDirection(DoCommandFlag::Execute,demo_slow->index,false),"native reverse to waiting carriages");return_posted=true;
  }
 }
 if(ticks>20000){fprintf(demo_log,"FAIL service timeout mode=%s tile=%u,%u type=%u number=%s\n",mode.c_str(),TileX(demo_slow->tile),TileY(demo_slow->tile),uint(demo_slow->current_order.GetType()),demo_slow->GetActiveServiceNumber().c_str());fflush(demo_log);exit(3);}
}
