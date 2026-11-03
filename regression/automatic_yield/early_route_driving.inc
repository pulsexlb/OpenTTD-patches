/* Real-train multi-outlet regressions. This observer only constructs tracks,
 * orders and signal programs with native commands and records actual movement.
 * It never writes speed/position, erases PBS or edits native engine properties. */
#include "tracerestrict_early_route.h"
#include "pbs.h"
namespace {
uint early_tick = 0, early_held = 0, early_pass_held = 0, early_expected_row = 40;
uint early_first_plan_x = UINT_MAX, early_plan_changes = 0;
uint early_independent_checks = 0;
uint early_recheck_waits = 0;
bool early_entered = false, early_waited = false, early_tail_clear = false, early_resumed = false;
bool early_saved = false, early_started_fast = false, early_loaded = false, early_loaded_next = false;
bool early_edited = false, early_plan_seen = false, early_seen_detector = false;
bool early_plan_after_edit = false, early_seen_upper_clear = false, early_current_stop_had_plan = false;
bool early_deferred_context = false, early_resolved_plan = false;
bool early_alternative_used = false, early_alternative_early = false;
bool early_crossing_checked = false;
std::vector<uint32_t> early_previous_plan;
StationID early_stop = StationID::Invalid(), early_main_goal = StationID::Invalid(), early_upper_goal = StationID::Invalid();
TraceRestrictSlot *early_auxiliary_slot = nullptr;

std::string_view EarlyMode() { return std::getenv("PXP_EARLY_MODE"); }
bool EarlySplitMode(std::string_view mode) { return mode.starts_with("split-"); }
bool EarlyCrossingMode(std::string_view mode) { return mode=="split-crossing"||mode=="split-crossing-edit"; }
uint EarlyLastShared(std::string_view mode) { return mode=="split-long"||mode=="split-change-fast"?110:EarlyCrossingMode(mode)?81:82; }
uint EarlyFinishX() { return EarlyMode()=="split-distant-target"?2366:246; }
bool EarlyNeedsWait(std::string_view mode)
{
 return mode=="two-through"||mode=="two-front"||mode=="two-tail"||mode=="three-through"||mode=="through-upper"||mode=="stable-two-paths"||mode=="new-siding"||mode=="wrong-outlet-siding"||mode=="multi-main"||mode=="shared-platform";
}
bool EarlyIsStop(std::string_view mode) { return mode.starts_with("stop-")||mode=="shared-platform"; }
bool EarlyUnknown(std::string_view mode) { return mode=="stop-conditional"||mode=="stop-side-effect"; }
void EarlyOrder(Train *train, Order order)
{
 DemoRequire(CmdInsertOrder(DoCommandFlag::Execute, InsertOrderCmdData(train->index, VehicleOrderID(train->GetNumOrders()), order)), "early native order");
}
Order EarlyStationOrder(StationID station, uint wait = 0)
{
 Order order; order.MakeGoToStation(station); order.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
 order.SetLoadType(OrderLoadType::NoLoad);
 if(wait){order.SetWaitTime(wait);order.SetWaitTimetabled(true);}
 return order;
}
void EarlyReplaceOrders(Train *train, StationID first, StationID next, TileIndex depot, std::string_view mode)
{
 while(train->GetNumOrders()) DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute, OrderTargetType::Vehicle, train->index.base(), 0), "clear early orders");
 EarlyOrder(train, EarlyStationOrder(first, EarlyIsStop(mode)?128:0));
 if(mode=="stop-conditional") {
  EarlyOrder(train, EarlyStationOrder(early_main_goal)); EarlyOrder(train, EarlyStationOrder(early_upper_goal));
  Order condition; condition.MakeConditional(VehicleOrderID{2}); condition.SetConditionVariable(OrderConditionVariable::LoadPercentage);
  condition.SetConditionComparator(OrderConditionComparator::LessThan); condition.SetConditionValue(50);
  DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(train->index,VehicleOrderID{1},condition)),"insert actual conditional departure");
  DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,train->index.base(),VehicleOrderID{1},MOF_COND_DESTINATION,3,INVALID_CARGO,""),"set actual conditional branch");
 }else{
  if(mode=="stop-side-effect") {
   early_auxiliary_slot=TraceRestrictSlot::Create(_current_company);early_auxiliary_slot->name="Early-route side effect, not a detector";early_auxiliary_slot->max_occupancy=16;
   Order release;release.MakeReleaseSlot();release.SetDestination(early_auxiliary_slot->index);EarlyOrder(train,release);
  }
  if(next!=StationID::Invalid()) EarlyOrder(train,EarlyStationOrder(next));
 }
 Order finish;finish.MakeGoToDepot(GetDepotDestinationIndex(depot),{},ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS,ODATFB_HALT);EarlyOrder(train,finish);
 if(train->cur_implicit_order_index!=0)DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,train->index,VehicleOrderID{0}),"activate early route");
}
void EarlyUpperOutlet(uint fork_x = 110, uint row = 25, uint clear_x = 125)
{
 DemoRail(fork_x,40,TRACK_UPPER);
 for(uint y=row+1;y<40;y++)DemoRail(fork_x,y,TRACK_Y);
 DemoRail(fork_x,row,TRACK_LOWER);
 for(uint x=fork_x+1;x<=245;x++)DemoRail(x,row,TRACK_X);
 DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(235,row),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"early upper goal");
 DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(246,row),RAILTYPE_RAIL,DiagDirection::NE),"early upper depot");
 DemoSignal(clear_x,row,AutoYieldRole::Clear);DemoSignal(180,row);DemoSignal(210,row);DemoSignal(230,row);
 early_upper_goal=GetStationIndex(TileXY(235,row));
}
void EarlyAlternativeOutlet()
{
 /* A short route rejoins Main at80 and diverges at82. The longer free
  * route stays entirely south of Main, using76,60 and100,55 to reach the
  * identical goal. Native costs and vehicle specifications are unchanged. */
 DemoRail(82,40,TRACK_RIGHT);for(uint y=41;y<55;y++)DemoRail(82,y,TRACK_Y);DemoRail(82,55,TRACK_LEFT);
 for(uint x=83;x<=245;x++)DemoRail(x,55,TRACK_X);
 DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(235,55),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"alternative split goal");
 DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(246,55),RAILTYPE_RAIL,DiagDirection::NE),"alternative split final depot");
 DemoSignal(125,55,AutoYieldRole::Clear);DemoSignal(180,55);DemoSignal(230,55);
 early_upper_goal=GetStationIndex(TileXY(235,55));
 DemoRail(76,42,TRACK_RIGHT);for(uint y=43;y<60;y++)DemoRail(76,y,TRACK_Y);DemoRail(76,60,TRACK_LEFT);
 for(uint x=77;x<100;x++)DemoRail(x,60,TRACK_X);DemoRail(100,60,TRACK_UPPER);
 for(uint y=56;y<60;y++)DemoRail(100,y,TRACK_Y);DemoRail(100,55,TRACK_LOWER);
}
void EarlyIndependentPlatforms()
{
 LayoutUpper();
 /* Preserve the upper inbound fork, replace only its empty outlet merge. */
 DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(80,38),TRACK_RIGHT),"independent upper exit curve");
 DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(80,39),TRACK_Y),"independent upper exit leg");
 DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(80,40),TRACK_LEFT),"independent upper merge");
 for(uint x=80;x<=90;x++)DemoRail(x,38,x==90?TRACK_UPPER:TRACK_X);
 for(uint y=26;y<38;y++)DemoRail(90,y,TRACK_Y);
 DemoRail(90,25,TRACK_LOWER);
 for(uint x=91;x<=245;x++)DemoRail(x,25,TRACK_X);
 DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(69,40),RAILTYPE_RAIL,Axis::X,1,5,StationClassID{0},0,StationID::Invalid(),false),"native cheap main platform");
 early_stop=GetStationIndex(TileXY(69,40));
 DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(69,38),RAILTYPE_RAIL,Axis::X,1,5,StationClassID{0},0,early_stop,false),"native compatible upper platform");
 for(uint x:{54u,74u}) {
  if(x==54&&std::getenv("PXP_DEMO_EXIT_ONLY"))continue;
  DemoRequire(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,TileXY(x,38),TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,AUTO_YIELD_DEFAULT_CONFIG|AUTO_YIELD_PLATFORM,""),"explicit upper platform class");
 }
 DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(235,25),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"independent upper destination");
 DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(246,25),RAILTYPE_RAIL,DiagDirection::NE),"independent upper depot");
 DemoSignal(125,25,AutoYieldRole::Clear);DemoSignal(180,25);DemoSignal(210,25);DemoSignal(230,25);
 early_upper_goal=GetStationIndex(TileXY(235,25));
}
void EarlyBuild()
{
 const auto mode=EarlyMode();DemoBuild();
 if(mode=="two-front"||mode=="two-tail") {
  const auto registration=mode=="two-front"?AutoYieldRegistration::Front:AutoYieldRegistration::Tail;
  DemoRequire(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,TileXY(16,40),TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,AUTO_YIELD_DEFAULT_CONFIG|(uint32_t(registration)<<26),""),"head or tail detector registration");
 }
 TopologyClearProgram(90,40);DemoSignal(125,40,AutoYieldRole::Clear);DemoSignal(180,40);DemoSignal(210,40);
 early_main_goal=GetStationIndex(TileXY(235,40));
 if(EarlyIsStop(mode)&&mode!="shared-platform") {
  EarlyIndependentPlatforms();
  if(EarlyUnknown(mode)) {
   /* Both future branches are physically legal from the cheap current-stop
    * platform. Once the real conditional/slot command has executed, the
    * next PF can prove its route without returning through the detector. */
   DemoRail(110,40,TRACK_UPPER);for(uint y=26;y<40;y++)DemoRail(110,y,TRACK_Y);DemoRail(110,25,TRACK_LOWER);
  }
  if(mode=="stop-reselect-order"||mode=="stop-split-change") {
   /* Realistic braking may already own the approach through x=53 when
    * the first plan becomes observable. Keep the actual platform choice
    * ahead of that prefix, so changing the next order tests legal replanning
    * instead of demanding cancellation of a committed PBS branch. */
   DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(50,40),TRACK_UPPER),"relocate platform-choice inlet");
   DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(50,39),TRACK_Y),"remove old platform-choice inlet leg");
   DemoRail(60,40,TRACK_UPPER);DemoRail(60,39,TRACK_Y);DemoRail(60,38,TRACK_LOWER);
  }
  early_expected_row=mode=="stop-main"?40:EarlyUnknown(mode)?40:38;
  const StationID next=mode=="stop-main"||mode=="stop-reselect-order"||mode=="stop-split-change"?early_main_goal:early_upper_goal;
  const TileIndex finish=mode=="stop-main"?TileXY(246,40):TileXY(246,25);
  EarlyReplaceOrders(demo_slow,early_stop,next,finish,mode);
 }else{
  if(mode=="split-alternative")EarlyAlternativeOutlet();
  else EarlyUpperOutlet(mode=="split-crossing-edit"?82:EarlySplitMode(mode)?EarlyLastShared(mode):110);
  /* split-crossing turns into the throat at80 and out again at81: two
   * adjacent switch curves, no actual same-direction straight Main rail.
   * Orthogonal cross-line priority remains covered by the untouched core tests. */
  if(mode=="split-distant-target") {
   for(uint y:{25u,40u}) {
    DemoRequire(CmdLandscapeClear(DoCommandFlag::Execute,TileXY(246,y)),"extend empty distant destination line");
    for(uint x=246;x<2366;x++)DemoRail(x,y,TRACK_X);
    DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(2350,y),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"distant real native goal");
    DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(2366,y),RAILTYPE_RAIL,DiagDirection::NE),"distant native destination depot");
   }
   early_main_goal=GetStationIndex(TileXY(2350,40));early_upper_goal=GetStationIndex(TileXY(2350,25));
   TopologyReplaceOrders(demo_fast,early_main_goal,TileXY(EarlyFinishX(),40));
  }
  if(mode=="three-through"||mode=="split-three") {
   DemoRail(110,40,TRACK_RIGHT);for(uint y=41;y<55;y++)DemoRail(110,y,TRACK_Y);DemoRail(110,55,TRACK_LEFT);
   for(uint x=111;x<=245;x++)DemoRail(x,55,TRACK_X);
   DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(235,55),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"third outlet goal");
   DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(246,55),RAILTYPE_RAIL,DiagDirection::NE),"third outlet depot");
   DemoSignal(125,55,AutoYieldRole::Clear);DemoSignal(180,55);DemoSignal(230,55);
  }
  if(mode=="stable-two-paths"||mode=="blocked-reselect") {
   DemoRail(150,25,TRACK_RIGHT);for(uint y=26;y<40;y++)DemoRail(150,y,TRACK_Y);DemoRail(150,40,TRACK_LEFT);
  }
  if(mode=="through-upper") {
   TopologyReplaceOrders(demo_slow,early_upper_goal,TileXY(246,25));TopologyReplaceOrders(demo_fast,early_upper_goal,TileXY(246,25));
   if(demo_slow->cur_implicit_order_index!=0)DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,demo_slow->index,VehicleOrderID{0}),"activate upper branch");
  }
  if(EarlySplitMode(mode)&&mode!="split-change-fast") {
   TopologyReplaceOrders(demo_slow,early_upper_goal,TileXY(EarlyFinishX(),mode=="split-alternative"?55:25));
   if(demo_slow->cur_implicit_order_index!=0)DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,demo_slow->index,VehicleOrderID{0}),"activate independent upper departure");
  }
  if(mode=="wrong-outlet-siding") {
   LayoutUpper();
   DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(80,38),TRACK_RIGHT),"remove wrong outlet curve");
   DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(80,39),TRACK_Y),"remove wrong outlet leg");
   DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(80,40),TRACK_LEFT),"remove wrong outlet merge");
   for(uint x=80;x<105;x++)DemoRail(x,38,TRACK_X);DemoRail(105,38,TRACK_UPPER);
   for(uint y=26;y<38;y++)DemoRail(105,y,TRACK_Y);DemoRail(105,25,TRACK_LOWER);
   for(uint x=106;x<=110;x++)DemoRail(x,25,TRACK_X);
  }
  if(mode=="multi-main") for(uint x:{58u,62u,66u})DemoSignal(x,40,AutoYieldRole::Main);
  if(mode=="new-siding")LayoutDeny(42);
  if(mode=="shared-platform") {
   MixedPlatform(42);early_stop=GetStationIndex(TileXY(69,42));
   EarlyReplaceOrders(demo_slow,early_stop,early_main_goal,TileXY(246,40),mode);
  }
 }
 AutoYieldSettingsChanged();
 _debug_levels[size_t(DebugLevelID::yapf)]=3;
 fprintf(demo_log,"EARLY READY mode=%.*s slow=%u fast=%u expected_platform=%u ordinary42=1 native_only=1\n",int(mode.size()),mode.data(),demo_slow->GetDisplayMaxSpeed(),demo_fast->GetDisplayMaxSpeed(),early_expected_row);
 fflush(demo_log);
}
bool EarlyResourceEmpty(const Train *train)
{
 for(const auto *slot:TraceRestrictSlot::Iterate()) if((slot==demo_slot||slot->automatic_yield_area)&&slot->IsOccupant(train->index)) return false;
 return true;
}
bool EarlyTailPast(const Train *train,uint x)
{
 for(const Train *part=train;part;part=part->Next()) if(TileX(part->tile)<=x||TileY(part->tile)==42||TileY(part->tile)==44||TileY(part->tile)==38) return false;
 return true;
}
void EarlyFail(const char *reason)
{
 if(EarlySplitMode(EarlyMode()))for(const auto *train:{demo_slow,demo_fast}){
  const auto *front=train->GetMovingFront();YapfRailRoute route;
  const bool known=AutoYieldGetNativeRoute(train,demo_slot->index.base(),front->tile,front->GetVehicleTrackdir(),route);
  fprintf(demo_log,"SPLIT FAILURE_PROOF train=%u order=%u target=%u index=%u native=%u words=%zu steps=%zu end=%u,%u front_td=%u\n",train->index.base(),uint(train->current_order.GetType()),train->current_order.GetDestination().base(),uint(train->cur_real_order_index),known,train->early_route.size(),route.path.size(),route.path.empty()?0:TileX(route.path.back().tile),route.path.empty()?0:TileY(route.path.back().tile),uint(front->GetVehicleTrackdir()));
 }
 fprintf(demo_log,"FAIL early %s tick=%u slow=%u,%u/%u order=%u idx=%u plan=%zu fast=%u,%u/%u\n",reason,early_tick,TileX(demo_slow->tile),TileY(demo_slow->tile),demo_slow->cur_speed,uint(demo_slow->current_order.GetType()),uint(demo_slow->cur_real_order_index),demo_slow->early_route.size(),TileX(demo_fast->tile),TileY(demo_fast->tile),demo_fast->cur_speed);fflush(demo_log);exit(202);
}
void EarlyReservationLog(const char *event)
{
 const auto origin=FollowTrainReservation(demo_slow,nullptr,FollowTrainReservationFlag::OkayUnused);
 const TileIndex fork=TileXY(50,40);const auto bits=GetReservedTrackbits(fork);
 const auto owner=[&](Track track){
  const Train *holder=(bits&TrackToTrackBits(track))?GetTrainForReservation(fork,track):nullptr;
  return holder?holder->Primary()->index.base():UINT32_MAX;
 };
 fprintf(demo_log,"EARLY PBS %s tick=%u front=%u,%u td=%u native_origin=%u,%u td=%u okay=%u fork50_mask=%u owner_main=%u owner_upper=%u owner_siding=%u subject=%u member=%u\n",event,early_tick,TileX(demo_slow->GetMovingFront()->tile),TileY(demo_slow->GetMovingFront()->tile),uint(demo_slow->GetMovingFront()->GetVehicleTrackdir()),TileX(origin.tile),TileY(origin.tile),uint(origin.trackdir),origin.okay,uint(bits),owner(TRACK_X),owner(TRACK_UPPER),owner(TRACK_RIGHT),demo_slow->index.base(),demo_slot->IsOccupant(demo_slow->index));
}
void EarlyCheckpointPlan(bool load)
{
 const auto path=std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/plan-at-checkpoint.bin";
 if(load){
  FILE *stream=fopen(path.c_str(),"rb");if(!stream)EarlyFail("missing saved plan evidence");
  uint32_t count=0;if(fread(&count,sizeof(count),1,stream)!=1||count>25000)EarlyFail("invalid saved plan evidence");
  std::vector<uint32_t> expected(count);if(count&&fread(expected.data(),sizeof(uint32_t),count,stream)!=count)EarlyFail("truncated saved plan evidence");
  fclose(stream);if(expected!=demo_slow->early_route)EarlyFail("saved native route did not reload identically");
  fprintf(demo_log,"EARLY PLAN_RELOAD_IDENTICAL words=%u\n",count);
 }else{
  FILE *stream=fopen(path.c_str(),"wb");if(!stream)EarlyFail("cannot write plan evidence");
  const uint32_t count=uint32_t(demo_slow->early_route.size());fwrite(&count,sizeof(count),1,stream);
  if(count)fwrite(demo_slow->early_route.data(),sizeof(uint32_t),count,stream);fclose(stream);
 }
}
}

void EarlyRouteDrivingTick()
{
 if(HasModalProgress()||_generating_world)return;
 const auto mode=EarlyMode();const bool reload=std::getenv("PXP_STRICT_RELOAD")!=nullptr;
 const bool waiting=EarlyNeedsWait(mode),scheduled=EarlyIsStop(mode);
 if(!demo_started&&_game_mode==GameMode::Editor){demo_started=true;EarlyBuild();return;}
 if(_game_mode!=GameMode::Normal)return;
 if(!early_tick&&reload){
  demo_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/reload-strict.log").c_str(),"w");
  for(auto *train:Train::Iterate())if(train->IsPrimaryVehicle()){if(train->unitnumber==1)demo_slow=train;else if(train->unitnumber==2)demo_fast=train;}
  for(auto *slot:TraceRestrictSlot::Iterate())if(slot->automatic_yield_area==0&&slot->name=="示例路签：正线与待避线共用")demo_slot=slot;
  if(!demo_slow||!demo_fast||!demo_slot)exit(203);
  const uint goal_x=mode=="split-distant-target"?2350:235;
  early_main_goal=GetStationIndex(TileXY(goal_x,40));early_upper_goal=GetStationIndex(TileXY(goal_x,mode=="split-alternative"?55:25));
  early_stop=scheduled?GetStationIndex(TileXY(69,mode=="shared-platform"?42:40)):StationID::Invalid();
  early_expected_row=mode=="stop-main"?40:EarlyUnknown(mode)?40:38;
  early_started_fast=waiting||EarlySplitMode(mode)||mode=="stop-independent"||mode=="stop-split-change";early_saved=true;early_edited=mode=="split-crossing-edit"||mode=="new-siding"||mode=="blocked-reselect"||mode=="stop-reselect-order"||mode=="stop-split-change"||mode=="split-change-fast";
  early_entered=(waiting||EarlySplitMode(mode))&&(TileY(demo_slow->tile)==42||TileY(demo_slow->tile)==44);early_loaded=scheduled&&demo_slow->last_station_visited==early_stop;
  /* This checkpoint is on the exclusive directed connector from the siding,
   * after its entrance. Reconstruct entry from that physical saved position,
   * rather than requiring the resumed train to enter the siding a second time. */
  if(mode=="split-alternative"&&TileX(demo_slow->tile)==76&&TileY(demo_slow->tile)>42&&TileY(demo_slow->tile)<=60)early_entered=true;
  early_plan_seen=!demo_slow->early_route.empty();_pause_mode={};_debug_levels[size_t(DebugLevelID::yapf)]=3;
  fprintf(demo_log,"EARLY RELOAD bytes=%zu actual_row=%u actual_order=%u\n",demo_slow->early_route.size(),TileY(demo_slow->tile),uint(demo_slow->current_order.GetType()));
  EarlyCheckpointPlan(true);
 }
 early_tick++;demo_tick=early_tick;
 if(std::getenv("PXP_EARLY_TRACE"))_debug_levels[size_t(DebugLevelID::yapf)]=((early_tick>=700&&early_tick<=1150)||(EarlySplitMode(mode)&&early_tick<=2200))?7:3;
 const uint x=TileX(demo_slow->tile),row=TileY(demo_slow->tile);
 if(std::getenv("PXP_EARLY_TRACE")&&early_tick>=700&&early_tick<=1150&&early_tick%32==0){
  const bool need=EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::NeedsSiding);
  const bool free=EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::FreeSiding);
  const bool full=EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::Full);
  fprintf(demo_log,"EARLY ADMISSION %u need=%u free=%u full=%u slow=%u,%u/%u fast=%u,%u/%u\n",early_tick,need,free,full,x,row,demo_slow->cur_speed,TileX(demo_fast->tile),TileY(demo_fast->tile),demo_fast->cur_speed);
  EarlyReservationLog("admission");
 }
 if(demo_slot->IsOccupant(demo_slow->index)&&!early_seen_detector){early_seen_detector=true;fprintf(demo_log,"EARLY DETECTOR %u x=%u row=%u\n",early_tick,x,row);}
 if(EarlyUnknown(mode)&&demo_slot->IsOccupant(demo_slow->index)&&(!early_loaded||demo_slow->current_order.IsAnyLoadingType())&&
   (x>=20||TrainReservationPassesThroughTile(demo_slow,TileXY(16,40),TRACK_X))) {
  if(!demo_slow->early_route.empty())EarlyFail("unresolved future order received a fabricated complete route");
  if(demo_slow->early_route_area!=demo_slot->index.base())EarlyFail("successful native prefix lost deferred detector context");
  if(!early_deferred_context){early_deferred_context=true;fprintf(demo_log,"EARLY DEFERRED_CONTEXT %u area=%u raw_words=0 actual_native_prefix=1\n",early_tick,demo_slow->early_route_area);}
 }
 if(demo_slow->early_route!=early_previous_plan){
  if(!demo_slow->early_route.empty()){
   if(!early_plan_seen){early_plan_seen=true;early_first_plan_x=x;}
   if(early_edited)early_plan_after_edit=true;
   early_plan_changes++;fprintf(demo_log,"EARLY PLAN %u x=%u row=%u words=%zu version=%u changes=%u\n",early_tick,x,row,demo_slow->early_route.size(),demo_slow->early_route[0],early_plan_changes);
   EarlyReservationLog("plan");
  }else fprintf(demo_log,"EARLY PLAN_INVALIDATED %u x=%u row=%u\n",early_tick,x,row);
  early_previous_plan=demo_slow->early_route;
 }
 if(!reload&&(waiting||EarlySplitMode(mode)||mode=="stop-independent"||mode=="stop-split-change")&&!early_started_fast&&early_tick==(mode=="shared-platform"?900u:mode=="stop-split-change"?800u:400u)){
  early_started_fast=true;DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_fast->index,true),"start early-route express");fprintf(demo_log,"EARLY START_FAST %u\n",early_tick);
 }
 if(!reload&&!early_edited&&mode=="split-crossing-edit"&&early_plan_seen&&x<45){
  const auto owned=FollowTrainReservation(demo_slow,nullptr,FollowTrainReservationFlag::OkayUnused);
  if(GetReservedTrackbits(TileXY(81,40))!=TRACK_BIT_NONE||GetReservedTrackbits(TileXY(82,40))!=TRACK_BIT_NONE)EarlyFail("edited split already has native PBS reservation");
  for(uint y=26;y<40;y++)DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(82,y),TRACK_Y),"remove old empty split connector");
  DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(82,40),TRACK_UPPER),"remove old empty split switch");
  DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(82,25),TRACK_LOWER),"remove old empty split curve");
  DemoRail(81,40,TRACK_UPPER);for(uint y=26;y<40;y++)DemoRail(81,y,TRACK_Y);
  DemoRail(81,25,TRACK_LOWER);DemoRail(82,25,TRACK_X);
  if(!demo_slow->early_route.empty())EarlyFail("real switch edit retained obsolete native route");
  const auto after=FollowTrainReservation(demo_slow,nullptr,FollowTrainReservationFlag::OkayUnused);
  if(owned.tile!=after.tile||owned.trackdir!=after.trackdir)EarlyFail("switch edit changed an unrelated owned PBS prefix");
  early_edited=true;fprintf(demo_log,"SPLIT SWITCH_EDIT %u old_fork=82 new_fork=81 plan_invalidated=1 native_prefix_preserved=1\n",early_tick);
 }
 if(!reload&&!early_edited&&mode=="new-siding"&&x>=25&&x<45){EarlyReservationLog("before_new_lane");LayoutThird();AutoYieldSettingsChanged();early_edited=true;fprintf(demo_log,"EARLY NEW_LANE %u row=44 legal_commands=1\n",early_tick);EarlyReservationLog("after_new_lane");}
 if(!reload&&!early_edited&&(mode=="stop-reselect-order"||mode=="stop-split-change")&&early_plan_seen&&x<45){
  EarlyReservationLog("before_order_change");
  const auto platform_fork_bits=GetReservedTrackbits(TileXY(60,40));
  fprintf(demo_log,"EARLY ORDER_EDIT_WINDOW %u actual_platform_fork=60,40 reserved_mask=%u owned_prefix_unchanged=1\n",early_tick,uint(platform_fork_bits));
  if(platform_fork_bits!=TRACK_BIT_NONE)EarlyFail("platform choice already reserved before next-order edit");
  DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),1),"change known next target");
  DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{1},EarlyStationOrder(early_upper_goal))),"replace known next target");
  early_edited=true;fprintf(demo_log,"EARLY ORDER_CHANGE %u x=%u target=upper\n",early_tick,x);
  EarlyReservationLog("after_order_change");
 }
 if(!reload&&!early_edited&&mode=="split-change-fast"&&row==42&&x>=54&&TileX(demo_fast->tile)<100){
  const auto reserved=GetReservedTrackbits(TileXY(110,40));
  if(reserved!=TRACK_BIT_NONE)EarlyFail("express split already reserved before lawful route change");
  DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_fast->index.base(),0),"change express outlet");
  DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_fast->index,VehicleOrderID{0},EarlyStationOrder(early_upper_goal))),"express independent upper outlet");
  DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,demo_fast->index,VehicleOrderID{0}),"activate changed express order");
  early_edited=true;fprintf(demo_log,"SPLIT EXPRESS_ORDER_CHANGE %u fork_unreserved=1 native_reservations_untouched=1\n",early_tick);
 }
 if(!reload&&!early_edited&&mode=="blocked-reselect"&&early_plan_seen&&x<45){
  EarlyReservationLog("before_exit_layout_change");
  DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(120,40),TRACK_X),"remove unreserved old exit connector");
  early_edited=true;fprintf(demo_log,"EARLY NATIVE_EXIT_DISCONNECTED %u x=%u removed=120,40 alternate_exit=upper native_reservations_untouched=1\n",early_tick,x);
  EarlyReservationLog("after_exit_layout_change");
 }
 if((waiting||EarlySplitMode(mode))&&!early_entered&&(row==42||row==44)&&x>=54&&x<74){
  early_entered=true;fprintf(demo_log,"EARLY ENTER %u row=%u\n",early_tick,row);
  if(mode=="new-siding"&&row!=44)EarlyFail("new legal lane not selected");
 }
 if(scheduled&&demo_slow->current_order.IsAnyLoadingType()&&demo_slow->last_station_visited==early_stop&&!early_loaded){
  early_loaded=true;fprintf(demo_log,"EARLY CURRENT_STOP %u row=%u target=%u\n",early_tick,row,early_stop.base());
  early_current_stop_had_plan=!demo_slow->early_route.empty();
  fprintf(demo_log,"EARLY STOP_PLAN confirmed=%u deferred_expected=%u\n",early_current_stop_had_plan,EarlyUnknown(mode));
  if(EarlyUnknown(mode)&&early_current_stop_had_plan)EarlyFail("unresolved future order received a fabricated complete route");
  if(mode!="shared-platform"&&row!=early_expected_row)EarlyFail("native arrival used incompatible platform");
 }
 const bool after_stop=!scheduled||(early_loaded&&!demo_slow->current_order.IsAnyLoadingType());
 bool shared_tail_clear=early_started_fast;
 for(const Train *part=demo_fast;part;part=part->Next())if(TileX(part->tile)<=EarlyLastShared(mode)&&TileY(part->tile)==40)shared_tail_clear=false;
 if(EarlySplitMode(mode)&&mode!="split-alternative"&&!EarlyCrossingMode(mode)&&shared_tail_clear&&row==42&&x>=72){
  if(!early_tail_clear){early_tail_clear=true;fprintf(demo_log,"SPLIT LAST_SHARED_TAIL_CLEAR %u fork=%u fast=%u,%u\n",early_tick,EarlyLastShared(mode),TileX(demo_fast->tile),TileY(demo_fast->tile));}
  /* Native route extension can defer a read-only decision for this tick.
   * The acceptance gate is actual departure before the unrelated area's
   * boundary, not a single intermediate predicate returning false. */
  if(EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::MustWait))early_recheck_waits++;
  else early_independent_checks++;
  if(demo_slow->cur_speed&&!early_resumed){early_resumed=true;fprintf(demo_log,"SPLIT INDEPENDENT_RESUME %u fast_before_area_clear=%u\n",early_tick,TileX(demo_fast->tile)<125);if(TileX(demo_fast->tile)>=125)EarlyFail("departure waited for the entire unrelated exit corridor");}
 }
 if(EarlyCrossingMode(mode)&&row==42&&x>=69&&x<=74&&demo_slot->IsOccupant(demo_fast->index)) {
  if(EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::MustWait))EarlyFail("crossing-only independent exits retain a PXP overtaking dependency");
  if(TileX(demo_fast->tile)<80)early_crossing_checked=true;
  early_independent_checks++;
 }
 if(mode=="split-alternative"&&((x==76&&row>42)||(row==60&&x>=76&&x<=100))){
  if(!early_alternative_used){
   early_alternative_used=true;early_alternative_early=TileX(demo_fast->tile)<125;
   fprintf(demo_log,"SPLIT ALTERNATIVE_USED %u slow=%u,%u fast=%u,%u before_unrelated_clear=%u\n",early_tick,x,row,TileX(demo_fast->tile),TileY(demo_fast->tile),early_alternative_early);
  }
  if(demo_slot->IsOccupant(demo_fast->index)&&demo_fast->cur_speed){
   if(EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::MustWait))EarlyFail("new independent path retains the old throat obligation");
   early_independent_checks++;
  }
 }
 if((mode=="stop-independent"||mode=="stop-split-change")&&early_started_fast&&TileX(demo_fast->Last()->tile)>60&&row==38&&x>=69&&after_stop&&demo_slot->IsOccupant(demo_fast->index)){
  if(EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::MustWait))EarlyFail("independent platform departure has an unrelated express obligation");
  early_independent_checks++;
 }
 if(EarlyUnknown(mode)&&after_stop&&x<50)EarlyFail("resolved future order returned through the old detector");
 if(EarlyUnknown(mode)&&after_stop&&!early_resolved_plan&&demo_slow->current_order.IsType(OT_GOTO_STATION)&&
   demo_slow->current_order.GetDestination()!=early_stop&&!demo_slow->early_route.empty()) {
  if(demo_slow->early_route_area!=demo_slot->index.base()||early_seen_upper_clear||(row==25&&x>=125))EarlyFail("resolved future route was not replanned before clearance");
  early_resolved_plan=true;fprintf(demo_log,"EARLY RESOLVED_PLAN %u area=%u words=%zu order=%u index=%u\n",early_tick,demo_slow->early_route_area,demo_slow->early_route.size(),uint(demo_slow->current_order.GetType()),uint(demo_slow->cur_real_order_index));
 }
 if((waiting||EarlySplitMode(mode))&&early_entered&&after_stop&&(row==42||row==44)&&x>=72&&demo_slow->cur_speed==0){
  if(!early_waited){early_waited=true;fprintf(demo_log,"EARLY EXIT_WAIT %u row=%u\n",early_tick,row);}
  for(const Train *part=demo_slow;part;part=part->Next())if(TileY(part->tile)!=row||TileX(part->tile)<=50)EarlyFail("tail has not cleared inlet");
  early_held++;
 }
 if(waiting&&!early_tail_clear&&EarlyTailPast(demo_fast,125)&&EarlyResourceEmpty(demo_fast)){early_tail_clear=true;fprintf(demo_log,"EARLY EXPRESS_TAIL_CLEAR %u\n",early_tick);}
 if(waiting&&early_waited&&!early_tail_clear){
  if(demo_slow->cur_speed||TrainReservationPassesThroughTile(demo_slow,TileXY(75,row),TRACK_X))EarlyFail("departed or reserved beyond wait before actual express tail clear");
  if(TileY(demo_fast->tile)==40&&TileX(demo_fast->tile)>=74&&TileX(demo_fast->tile)<=110){if(!demo_fast->cur_speed)EarlyFail("priority express stopped in shared corridor");early_pass_held++;}
 }
 if(waiting&&early_waited&&early_tail_clear&&demo_slow->cur_speed&&!early_resumed){early_resumed=true;fprintf(demo_log,"EARLY RESUME %u\n",early_tick);}
 if(row==25&&x>=125&&x<=130&&!early_seen_upper_clear){early_seen_upper_clear=true;fprintf(demo_log,"EARLY ACTUAL_UPPER_CLEAR %u\n",early_tick);}
 if(!reload&&!early_saved&&(((waiting||EarlySplitMode(mode))&&early_held>=2)||(mode=="split-alternative"&&early_alternative_used)||(EarlyCrossingMode(mode)&&early_crossing_checked)||(scheduled&&early_loaded&&demo_slow->current_order.IsAnyLoadingType())||(!waiting&&!scheduled&&early_edited&&!EarlySplitMode(mode)&&x>115))){EarlyCheckpointPlan(false);DemoSave("strict-waiting");early_saved=true;}
 const StationID final_goal=(mode=="through-upper"||(EarlySplitMode(mode)&&mode!="split-change-fast")||(scheduled&&mode!="stop-main"&&mode!="shared-platform"))?early_upper_goal:early_main_goal;
 if(demo_slow->current_order.IsAnyLoadingType()&&demo_slow->last_station_visited==final_goal&&!early_loaded_next){early_loaded_next=true;fprintf(demo_log,"EARLY FINAL_STOP %u row=%u goal=%u\n",early_tick,row,final_goal.base());}
 if(early_loaded_next&&EarlyResourceEmpty(demo_slow)){
  if(!early_saved)EarlyFail("missing real save checkpoint");
  if(!reload&&!EarlyUnknown(mode)&&(!early_plan_seen||early_first_plan_x>=50))EarlyFail("native plan was not established before inlet fork");
  if(waiting&&(!early_entered||!early_waited||early_held<2||!early_tail_clear||!early_resumed||!early_pass_held))EarlyFail("missing full enter wait pass resume sequence");
  if(EarlySplitMode(mode)&&mode!="split-alternative"&&!EarlyCrossingMode(mode)&&(!early_entered||!early_waited||!early_tail_clear||!early_resumed))EarlyFail("missing shared-junction wait and independent departure sequence");
  if(EarlyCrossingMode(mode)&&(!early_entered||!early_crossing_checked))EarlyFail("missing live crossing-only departure check before express reaches junction");
  if(mode=="split-alternative"&&(!early_entered||!early_alternative_used||!early_alternative_early))EarlyFail("legal independent split route not actually used before unrelated express clearance");
  if((mode=="stop-independent"||mode=="stop-split-change")&&!early_started_fast)EarlyFail("independent express never started");
  if((EarlySplitMode(mode)||mode=="stop-independent"||mode=="stop-split-change")&&!early_independent_checks)EarlyFail("no live independent express overlap checked");
  if(scheduled&&!early_loaded)EarlyFail("mandatory stop skipped");
  if(EarlyUnknown(mode)&&(!early_deferred_context||!early_resolved_plan||!early_seen_upper_clear))EarlyFail("missing deferred context and later actual native replanning");
  if((mode=="new-siding"||mode=="blocked-reselect"||mode=="stop-reselect-order")&&!early_edited)EarlyFail("runtime edit not executed");
  if(!reload&&(mode=="new-siding"||mode=="blocked-reselect"||mode=="stop-reselect-order")&&!early_plan_after_edit)EarlyFail("valid route plan was not rebuilt after runtime edit");
  if(mode=="split-crossing-edit"&&(!early_edited||(!reload&&!early_plan_after_edit)))EarlyFail("modified split route not rebuilt and actually executed");
  if(mode=="blocked-reselect"&&!early_seen_upper_clear)EarlyFail("native route did not use the legal alternate exit");
  fprintf(demo_log,"STRICT PASS early=%.*s reload=%u plan_before_fork=%u first_plan_x=%u plan_changes=%u entered=%u waited=%u held=%u express_while_held=%u actual_tail_clear=%u resumed=%u station_stop=%u correct_next_goal=1 resources_released=1 independent_live_checks=%u\n",int(mode.size()),mode.data(),reload,early_plan_seen,early_first_plan_x,early_plan_changes,early_entered,early_waited,early_held,early_pass_held,early_tail_clear,early_resumed,early_loaded,early_independent_checks);fflush(demo_log);_exit_game=true;
 }
 if(early_tick%512==0)fprintf(demo_log,"EARLY TRACE %u slow=%u,%u/%u order=%u idx=%u plan=%zu fast=%u,%u/%u held=%u\n",early_tick,x,row,demo_slow->cur_speed,uint(demo_slow->current_order.GetType()),uint(demo_slow->cur_real_order_index),demo_slow->early_route.size(),TileX(demo_fast->tile),TileY(demo_fast->tile),demo_fast->cur_speed,early_held);
 if(early_tick>(mode=="split-distant-target"?60000u:24000u))EarlyFail("native run timeout");
 fflush(demo_log);
}
