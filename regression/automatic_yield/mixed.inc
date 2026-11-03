/* Mixed stations use explicit, existing platform markers. Every movement is
 * native; mandatory station work and ordinary parking are observed separately.
 * The platform bit is the only configuration difference between lane classes. */
namespace {
std::vector<uint> mixed_platform_rows;
std::vector<uint> mixed_ordinary_rows;
uint mixed_clear_x = 90;

void MixedPlatform(uint row, StationID join = StationID::Invalid())
{
 DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(69,row),RAILTYPE_RAIL,Axis::X,1,5,StationClassID{0},0,join,false),"mixed compatible platform");
 for(uint x:{54u,74u}){
  if(x==54&&std::getenv("PXP_DEMO_EXIT_ONLY"))continue;
  DemoRequire(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,TileXY(x,row),TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,AUTO_YIELD_DEFAULT_CONFIG|AUTO_YIELD_PLATFORM,""),"mixed explicit platform role");
 }
}

void MixedExtraLane(uint row)
{
 if(row==38){LayoutUpper();return;}
 if(row==44){LayoutThird();return;}
 const bool upper=row<40;
 const uint start=row==36?46:44,end=row==36?86:88;
 DemoRail(start,40,upper?TRACK_UPPER:TRACK_RIGHT);
 for(uint y=std::min(row,40u)+1;y<std::max(row,40u);y++)DemoRail(start,y,TRACK_Y);
 DemoRail(start,row,upper?TRACK_LOWER:TRACK_LEFT);
 for(uint x=start+1;x<end;x++)DemoRail(x,row,TRACK_X);
 DemoRail(end,row,upper?TRACK_RIGHT:TRACK_UPPER);
 for(uint y=std::min(row,40u)+1;y<std::max(row,40u);y++)DemoRail(end,y,TRACK_Y);
 DemoRail(end,40,upper?TRACK_LEFT:TRACK_LOWER);
 if(!std::getenv("PXP_DEMO_EXIT_ONLY"))DemoSignal(54,row,AutoYieldRole::Siding);
 DemoSignal(74,row,AutoYieldRole::Exit);
}

bool MixedInRows(uint row,const std::vector<uint>&rows)
{
 return std::find(rows.begin(),rows.end(),row)!=rows.end();
}

void MixedBuild(std::string_view mode)
{
 const bool independent=mode=="mixed-independent"||mode=="mixed-independent-stops",rejoin=mode=="mixed-rejoin"||mode=="mixed-rejoin-stop"||mode=="mixed-rejoin-both-stop";
 const bool close_follower=mode.ends_with("-stop")||mode.ends_with("-signals");
 if(independent||rejoin)TopologyBuild(independent?"topology-split":"topology-rejoin");
 else if(close_follower)DemoBuild();
 else BuildFarDestination();
 if(mode=="mixed-rejoin-stop"||mode=="mixed-rejoin-both-stop"){
  /* Use a nearer real feeder depot: a distant express can legitimately let
   * the stopping train clear the later remerge before it arrives. No vehicle
   * position/speed writes or path costs are used to manufacture the conflict. */
  const EngineID engine=demo_fast->engine_type;delete demo_fast;demo_fast=nullptr;
  DemoRequire(CmdLandscapeClear(DoCommandFlag::Execute,TileXY(120,60)),"empty nearer feeder depot site");
  UpdateSignalsInBuffer(); // Finish the old rail's updates before replacing its tile type.
  DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(120,60),RAILTYPE_RAIL,DiagDirection::NE),"nearer real feeder depot");
  TopologyProgram(110,60,TRACK_X,TRACKDIR_X_NE,AutoYieldRole::Approach);
  demo_fast=DemoTrain(engine,"rejoining stopping-station express",GetStationIndex(TileXY(350,40)),TileXY(120,60));
  TopologyReplaceOrders(demo_fast,GetStationIndex(TileXY(350,40)),TileXY(446,40));
  TopologyPassStation(demo_fast,120,25);
 }
 for(uint x:{180u,210u})DemoSignal(x,40);
 if(!close_follower)for(uint x:{250u,280u,310u,340u})DemoSignal(x,40);
 mixed_platform_rows={42};mixed_ordinary_rows.clear();
 if(mode.find("p0o1")!=std::string_view::npos){mixed_platform_rows.clear();mixed_ordinary_rows.push_back(42);}
 else if(mode.find("p1o0")==std::string_view::npos){mixed_ordinary_rows.push_back(38);MixedExtraLane(38);}
 if(mode.find("p2o1")!=std::string_view::npos||mode.find("p3o2")!=std::string_view::npos){mixed_platform_rows.push_back(44);MixedExtraLane(44);}
 if(mode.find("p1o2")!=std::string_view::npos||mode.find("p3o2")!=std::string_view::npos){mixed_ordinary_rows.push_back(36);MixedExtraLane(36);}
 if(mode.find("p3o2")!=std::string_view::npos){mixed_platform_rows.push_back(46);MixedExtraLane(46);}
 StationID station=StationID::Invalid();
 for(uint row:mixed_platform_rows){MixedPlatform(row,station);if(station==StationID::Invalid())station=GetStationIndex(TileXY(69,row));}
 if(mode=="mixed-independent-stops"){
  for(uint row:{40u,20u})DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(66,row),RAILTYPE_RAIL,Axis::X,1,5,StationClassID{0},0,StationID::Invalid(),false),"separate route's normal platform");
  InsertStationStop(demo_slow,GetStationIndex(TileXY(66,40)),32);
  InsertStationStop(demo_fast,GetStationIndex(TileXY(66,20)),32);
 }
 if(mode.ends_with("-stop"))InsertStationStop(demo_slow,station,32);
 if(mode=="mixed-rejoin-both-stop"){
  DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(69,40),RAILTYPE_RAIL,Axis::X,1,5,StationClassID{0},0,station,false),"rejoining station native Main platform");
  InsertStationStop(demo_fast,station,32);
 }
 if(mode.ends_with("-signals")){
  for(uint x:{58u,62u,64u})DemoSignal(x,40,AutoYieldRole::Main);
  std::vector<uint> rows=mixed_platform_rows;rows.insert(rows.end(),mixed_ordinary_rows.begin(),mixed_ordinary_rows.end());
  for(uint row:rows)for(uint x:{58u,62u})DemoSignal(x,row);
 }
 mixed_clear_x=rejoin?160:90;
 AutoYieldSettingsChanged();
 fprintf(demo_log,"MIXED READY mode=%.*s platforms=%zu ordinary=%zu explicit_platform_config=1 common_other_config=1 slow_design=%u fast_design=%u\n",int(mode.size()),mode.data(),mixed_platform_rows.size(),mixed_ordinary_rows.size(),demo_slow->GetDisplayMaxSpeed(),demo_fast->GetDisplayMaxSpeed());
 fflush(demo_log);
}

bool MixedTailClear(const Train*train)
{
 if(demo_slot->IsOccupant(train->index))return false;
 for(const Train*part=train;part;part=part->Next()){
  const uint row=TileY(part->tile);
  if(TileX(part->tile)<=mixed_clear_x||(row!=40&&row!=20&&row!=25))return false;
 }
 return true;
}

bool MixedReleased()
{
 for(const auto*s:TraceRestrictSlot::Iterate())if((s==demo_slot||s->automatic_yield_area)&&s->IsOccupant(demo_slow->index))return false;
 return true;
}
}

void MixedCandidateTick();
void MixedTick()
{
 const std::string_view mode=std::getenv("PXP_STRICT");
 if(mode.starts_with("mixed-outlet-")){MixedCandidateTick();return;}
 /* These compatibility cases retain the full existing real-driving assertions:
  * both occupied trains must drive in, then automatic yield controls departure.
  * Their legacy default branch is deliberately used, with a correctly marked
  * platform already present before any fixture vehicle leaves its depot. */
 if(mode=="mixed-add"||mode=="mixed-full"){
  const bool initial=!demo_started&&_game_mode==GameMode::Editor;
  if(mode=="mixed-add")OccupiedTick();else StockFullTick();
  if(initial){MixedPlatform(42);AutoYieldSettingsChanged();fprintf(demo_log,"MIXED COMPAT platform=42 marker_config=explicit kind=%.*s\n",int(mode.size()),mode.data());fflush(demo_log);}
  return;
 }
 static uint n=0,held=0,pass_held=0,independent_overlap=0,chosen_row=0;
 static bool entered=false,station_work=false,stopped=false,saved=false,cleared=false,resumed=false,started=false,conflict=false,independent_fast_work=false;
 const bool independent_stops=mode=="mixed-independent-stops";
 const bool reload=std::getenv("PXP_STRICT_RELOAD")!=nullptr,independent=mode=="mixed-independent"||independent_stops,scheduled=mode.ends_with("-stop");
 if(!demo_started&&_game_mode==GameMode::Editor){demo_started=true;MixedBuild(mode);return;}
 if(_game_mode!=GameMode::Normal)return;
 if(n==0&&reload){
  demo_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/reload-strict.log").c_str(),"w");
  for(auto*v:Train::Iterate())if(v->IsPrimaryVehicle()){if(v->unitnumber==1)demo_slow=v;else if(v->unitnumber==2)demo_fast=v;}
  for(auto*s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area==0)demo_slot=s;
  if(!demo_slow||!demo_fast||!demo_slot)exit(160);
  mixed_platform_rows={42};mixed_ordinary_rows.clear();
  if(mode.find("p0o1")!=std::string_view::npos){mixed_platform_rows.clear();mixed_ordinary_rows.push_back(42);}
  else if(mode.find("p1o0")==std::string_view::npos)mixed_ordinary_rows.push_back(38);
  if(mode.find("p2o1")!=std::string_view::npos||mode.find("p3o2")!=std::string_view::npos)mixed_platform_rows.push_back(44);
  if(mode.find("p1o2")!=std::string_view::npos||mode.find("p3o2")!=std::string_view::npos)mixed_ordinary_rows.push_back(36);
  if(mode.find("p3o2")!=std::string_view::npos)mixed_platform_rows.push_back(46);
  mixed_clear_x=mode.starts_with("mixed-rejoin")?160:90;
  started=saved=true;station_work=scheduled||(independent_stops&&demo_slow->last_station_visited==GetStationIndex(TileXY(66,40)));
  independent_fast_work=(independent_stops||mode=="mixed-rejoin-both-stop")&&demo_fast->last_station_visited==GetStationIndex(TileXY(66,20));_pause_mode={};
  if(mode=="mixed-rejoin-both-stop"&&demo_fast->cur_real_order_index>0)independent_fast_work=true;
 }
 n++;demo_tick=n;
 if(n==1&&std::getenv("PXP_STRICT_DEBUG"))_debug_levels[size_t(DebugLevelID::yapf)]=7;
 if(!reload&&!started&&n==(independent?100u:mode.starts_with("mixed-rejoin-")?150u:(scheduled||mode.ends_with("-signals"))?900u:400u)){
  started=true;DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_fast->index,true),"start mixed express");fprintf(demo_log,"MIXED START_FAST %u\n",n);
 }
 const uint row=TileY(demo_slow->tile),x=TileX(demo_slow->tile);
 const bool need=EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::NeedsSiding);
 if(need&&!conflict){conflict=true;fprintf(demo_log,"MIXED CONFLICT %u\n",n);}
 if(!entered&&x>=56&&x<74&&(MixedInRows(row,mixed_platform_rows)||MixedInRows(row,mixed_ordinary_rows))){
  chosen_row=row;entered=true;
  fprintf(demo_log,"MIXED ENTER %u row=%u scheduled=%u platform=%u\n",n,row,scheduled,MixedInRows(row,mixed_platform_rows));
  if(scheduled!=MixedInRows(row,mixed_platform_rows)){fprintf(demo_log,"FAIL mixed wrong lane class row=%u scheduled=%u\n",row,scheduled);fflush(demo_log);exit(161);}
 }
 if(demo_slow->current_order.IsAnyLoadingType()&&!station_work){station_work=true;fprintf(demo_log,"MIXED SCHEDULED_LOADING %u row=%u\n",n,row);}
 if(!scheduled&&!independent_stops&&demo_slow->current_order.IsAnyLoadingType()){fprintf(demo_log,"FAIL non-stop mixed arrival performed station work\n");exit(162);}
 if((independent_stops||mode=="mixed-rejoin-both-stop")&&demo_fast->current_order.IsAnyLoadingType()&&!independent_fast_work){independent_fast_work=true;fprintf(demo_log,"MIXED INDEPENDENT_FAST_LOADING %u\n",n);}
 const bool stop_done=!scheduled||(station_work&&!demo_slow->current_order.IsAnyLoadingType());
 if(entered&&stop_done&&row==chosen_row&&x>=72&&demo_slow->cur_speed==0){
  if(!stopped){stopped=true;fprintf(demo_log,"MIXED EXIT_STOP %u row=%u\n",n,row);}
  for(const Train*part=demo_slow;part;part=part->Next())if(TileY(part->tile)!=chosen_row||TileX(part->tile)<=54){fprintf(demo_log,"FAIL mixed tail not inside exclusive lane\n");exit(163);}
  held++;
 }
 if(!cleared&&MixedTailClear(demo_fast)){cleared=true;fprintf(demo_log,"MIXED EXPRESS_ACTUAL_TAIL_CLEAR %u\n",n);}
 if(stopped&&!cleared){
  if(demo_slow->cur_speed||TrainReservationPassesThroughTile(demo_slow,TileXY(75,chosen_row),TRACK_X)){fprintf(demo_log,"FAIL mixed departure before actual express tail clear\n");exit(164);}
  if(TileY(demo_fast->tile)==40&&TileX(demo_fast->tile)>=74&&TileX(demo_fast->tile)<=90){
   if(demo_fast->cur_speed==0){fprintf(demo_log,"FAIL mixed priority express stopped in shared corridor\n");exit(165);}pass_held++;
  }
 }
 if(stopped&&cleared&&demo_slow->cur_speed&&!resumed){resumed=true;fprintf(demo_log,"MIXED RESUME %u\n",n);}
 if(!independent&&entered&&!cleared&&row==40&&x>=88){fprintf(demo_log,"FAIL mixed slow exited before express tail\n");exit(166);}
 if(independent){
  if(need||entered){fprintf(demo_log,"FAIL mixed independent route locked by unrelated express need=%u entered=%u\n",need,entered);exit(167);}
  if(demo_slot->IsOccupant(demo_slow->index)&&demo_slot->IsOccupant(demo_fast->index)){
   independent_overlap++;
   if(demo_slow->cur_speed==0&&x>=35&&!(independent_stops&&demo_slow->current_order.IsAnyLoadingType())){fprintf(demo_log,"FAIL mixed independent slow train stopped\n");exit(168);}
  }
 }
 if(!saved&&!reload&&((stopped&&held>=2)||(independent&&independent_overlap>16))){DemoSave("strict-waiting");saved=true;}
 if(cleared&&row==40&&x>mixed_clear_x+4&&MixedReleased()){
  if((independent_stops||mode=="mixed-rejoin-both-stop")&&(!station_work||!independent_fast_work)){fprintf(demo_log,"FAIL independent platforms did not both perform station work\n");exit(171);}
  if(!saved||(independent?independent_overlap==0:!entered||!stopped||held<2||!resumed||pass_held==0||(scheduled&&!station_work)||(!reload&&!conflict))){fprintf(demo_log,"FAIL mixed missing native stage enter=%u stop=%u held=%u pass_held=%u station_work=%u resume=%u overlap=%u\n",entered,stopped,held,pass_held,station_work,resumed,independent_overlap);exit(169);}
  fprintf(demo_log,"STRICT PASS mode=%.*s reload=%u entered=%u chosen_row=%u scheduled_loading=%u exit_stop=%u held=%u pass_while_stopped=%u actual_tail_clear=1 resumed=%u subject_claims_released=1 independent_overlap=%u\n",int(mode.size()),mode.data(),reload,entered,chosen_row,station_work,stopped,held,pass_held,resumed,independent_overlap);fflush(demo_log);_exit_game=true;
 }
 if(n%512==0)fprintf(demo_log,"MIXED TRACE %u slow=%u,%u/%u order=%u need=%u fast=%u,%u/%u held=%u\n",n,x,row,demo_slow->cur_speed,uint(demo_slow->current_order.GetType()),need,TileX(demo_fast->tile),TileY(demo_fast->tile),demo_fast->cur_speed,held);
 if(n>24000){fprintf(demo_log,"FAIL mixed native timeout\n");fflush(demo_log);exit(170);}fflush(demo_log);
}

/* A normal Main platform and a compatible parking platform serve one actual
 * station. This makes false candidate rejection observable in real routing:
 * Main is the shorter path and a station stop there can block the express. */
namespace {
StationID mixed_candidate_station=StationID::Invalid();
StationID mixed_upper_goal=StationID::Invalid();
bool mixed_candidate_changed=false;

void MixedCandidateGoal(Train*train,StationID station,uint index)
{
 Order next;next.MakeGoToStation(station);next.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);next.SetLoadType(OrderLoadType::NoLoad);
 DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(train->index,static_cast<VehicleOrderID>(index),next)),"mixed candidate destination");
}

void MixedCandidateBuild(std::string_view mode)
{
 DemoBuild();LayoutUpper();MixedPlatform(42);
 mixed_candidate_station=GetStationIndex(TileXY(69,42));
 DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(69,40),RAILTYPE_RAIL,Axis::X,1,5,StationClassID{0},0,mixed_candidate_station,false),"normal Main station platform");
 TopologyClearProgram(90,40);DemoSignal(100,40,AutoYieldRole::Clear);
 DemoRail(85,40,TRACK_UPPER);for(uint y=26;y<40;y++)DemoRail(85,y,TRACK_Y);DemoRail(85,25,TRACK_LOWER);
 for(uint x=86;x<=245;x++)DemoRail(x,25,TRACK_X);
 DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(120,25),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"mixed separate outlet destination");
 DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(120,40),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"mixed main outlet destination");
 mixed_upper_goal=GetStationIndex(TileXY(120,25));
 DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(246,25),RAILTYPE_RAIL,DiagDirection::NE),"mixed separate outlet depot");
 DemoSignal(95,25);DemoSignal(100,25,AutoYieldRole::Clear);DemoSignal(230,25);
 SetSignalType(TileXY(100,40),TRACK_X,SignalType::PathOneWay);SetSignalType(TileXY(100,25),TRACK_X,SignalType::PathOneWay);
 DemoSignal(180,40);DemoSignal(210,40);
 if(mode=="mixed-outlet-single-clear")TopologyClearProgram(100,25);
 if(mode=="mixed-outlet-blocked"){
  auto*p=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(74,42),TRACK_X),false);const auto old=p->actions_used_flags;
  TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);p->items.push_back(deny.AsProgramItem());
  DemoRequire(p->Validate(),"mixed truly forbidden platform departure");TraceRestrictCheckRefreshSignals(p,0,old);
 }
 while(demo_slow->GetNumOrders())DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),0),"replace mixed candidate orders");
 MixedCandidateGoal(demo_slow,GetStationIndex(TileXY(120,40)),0);
 InsertStationStop(demo_slow,mixed_candidate_station,128);
 if(mode=="mixed-outlet-conditional"||mode=="mixed-outlet-single-clear"){
  MixedCandidateGoal(demo_slow,mixed_upper_goal,2);
  Order conditional;conditional.MakeConditional(VehicleOrderID{2});conditional.SetConditionVariable(OrderConditionVariable::LoadPercentage);conditional.SetConditionComparator(OrderConditionComparator::LessThan);conditional.SetConditionValue(50);
  DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{1},conditional)),"native unknown-next conditional");
  DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{1},MOF_COND_DESTINATION,3,INVALID_CARGO,""),"native conditional branch destination");
 }
 Order finish;const TileIndex depot=mode=="mixed-outlet-conditional"||mode=="mixed-outlet-single-clear"||mode=="mixed-outlet-change"?TileXY(246,25):TileXY(246,40);
 finish.MakeGoToDepot(GetDepotDestinationIndex(depot),{},ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS,ODATFB_HALT);
 DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,static_cast<VehicleOrderID>(demo_slow->GetNumOrders()),finish)),"mixed candidate final depot");
 AutoYieldSettingsChanged();
 fprintf(demo_log,"MIXED CANDIDATE READY mode=%.*s platform_station=%u main_station=%u explicit_platform_config=1 two_clear=%u slow=%u fast=%u\n",int(mode.size()),mode.data(),mixed_candidate_station.base(),GetStationIndex(TileXY(69,40)).base(),mode!="mixed-outlet-single-clear",demo_slow->GetDisplayMaxSpeed(),demo_fast->GetDisplayMaxSpeed());fflush(demo_log);
}

bool MixedCandidateExpressTail(uint x)
{
 for(const Train*part=demo_fast;part;part=part->Next())if(TileY(part->tile)!=40||TileX(part->tile)<=x)return false;
 return true;
}
}

void MixedCandidateTick()
{
 static uint n=0,held=0,pass_held=0,main_loading=0,fast_blocked=0;
 static bool started=false,entered=false,loading=false,stopped=false,saved=false,resumed=false,free_seen=false;
 const std::string_view mode=std::getenv("PXP_STRICT");const bool blocked=mode=="mixed-outlet-blocked",reload=std::getenv("PXP_STRICT_RELOAD");
 const bool upper=mode=="mixed-outlet-conditional"||mode=="mixed-outlet-single-clear"||mode=="mixed-outlet-change";
 if(!demo_started&&_game_mode==GameMode::Editor){demo_started=true;MixedCandidateBuild(mode);return;}
 if(_game_mode!=GameMode::Normal)return;
 if(n==0&&reload){
  demo_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/reload-strict.log").c_str(),"w");
  for(auto*v:Train::Iterate())if(v->IsPrimaryVehicle()){if(v->unitnumber==1)demo_slow=v;else if(v->unitnumber==2)demo_fast=v;}
  for(auto*s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area==0)demo_slot=s;
  if(!demo_slow||!demo_fast||!demo_slot)exit(180);
  mixed_candidate_station=GetStationIndex(TileXY(69,42));mixed_upper_goal=GetStationIndex(TileXY(120,25));
  started=saved=true;loading=true;mixed_candidate_changed=mode=="mixed-outlet-change";_pause_mode={};
 }
 n++;demo_tick=n;
 if(n==1&&std::getenv("PXP_STRICT_DEBUG"))_debug_levels[size_t(DebugLevelID::yapf)]=7;
 if(!reload&&!started&&n==900){started=true;DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_fast->index,true),"start mixed candidate express");}
 const uint x=TileX(demo_slow->tile),row=TileY(demo_slow->tile);
 if(!free_seen&&EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::FreeSiding)){free_seen=true;fprintf(demo_log,"MIXED CANDIDATE FREE %u\n",n);}
 if(row==42&&x>=56&&!entered){entered=true;fprintf(demo_log,"MIXED CANDIDATE ENTER %u current_station=%u\n",n,demo_slow->current_order.IsType(OT_GOTO_STATION));}
 if(demo_slow->current_order.IsAnyLoadingType()){
  if(!loading){loading=true;fprintf(demo_log,"MIXED CANDIDATE LOADING %u row=%u actual_station=%u\n",n,row,demo_slow->last_station_visited.base());}
  if(row==40)main_loading++;
  if(mode=="mixed-outlet-change"&&!mixed_candidate_changed){
   DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),1),"change next destination during real station work");
   MixedCandidateGoal(demo_slow,mixed_upper_goal,1);mixed_candidate_changed=true;
   fprintf(demo_log,"MIXED CANDIDATE NATIVE_NEXT_CHANGED %u still_loading=%u next_station=%u\n",n,demo_slow->current_order.IsAnyLoadingType(),mixed_upper_goal.base());
  }
 }
 if(row==42&&loading&&!demo_slow->current_order.IsAnyLoadingType()&&x>=72&&demo_slow->cur_speed==0){
  if(!stopped){stopped=true;fprintf(demo_log,"MIXED CANDIDATE EXIT_STOP %u order_index=%u\n",n,demo_slow->cur_real_order_index);}
  for(const Train*part=demo_slow;part;part=part->Next())if(TileY(part->tile)!=42||TileX(part->tile)<=54){fprintf(demo_log,"FAIL mixed candidate unsafe body parking\n");exit(181);}held++;
 }
 if(row==38&&x>=56&&x<80){fprintf(demo_log,"FAIL mandatory current station assigned ordinary siding\n");exit(182);}
 if(blocked&&entered){fprintf(demo_log,"FAIL genuinely forbidden platform departure admitted\n");exit(183);}
 if(stopped&&!MixedCandidateExpressTail(85)){
  if(demo_slow->cur_speed||TrainReservationPassesThroughTile(demo_slow,TileXY(75,42),TRACK_X)){fprintf(demo_log,"FAIL mixed candidate left before actual shared throat tail\n");exit(184);}
  if(TileY(demo_fast->tile)==40&&TileX(demo_fast->tile)>=74&&TileX(demo_fast->tile)<=85){if(!demo_fast->cur_speed){fprintf(demo_log,"FAIL mixed candidate priority express stopped\n");exit(185);}pass_held++;}
 }
 if(main_loading&&TileY(demo_fast->tile)==40&&TileX(demo_fast->tile)>=45&&TileX(demo_fast->tile)<74&&demo_fast->cur_speed==0){
  fast_blocked++;fprintf(demo_log,"MIXED CANDIDATE ACTUAL_MAIN_BLOCK %u fast=%u,%u/0 slow=%u,%u/%u main_loading_ticks=%u free_seen=%u\n",n,TileX(demo_fast->tile),TileY(demo_fast->tile),x,row,demo_slow->cur_speed,main_loading,free_seen);
  if(!blocked&&fast_blocked>16){fprintf(demo_log,"FAIL available mixed platform rejected; Main station stop physically blocked express\n");fflush(demo_log);exit(186);}
 }
 if(stopped&&MixedCandidateExpressTail(85)&&demo_slow->cur_speed&&!resumed){resumed=true;fprintf(demo_log,"MIXED CANDIDATE RESUME %u\n",n);}
 if(!saved&&!reload&&((stopped&&held>2)||(blocked&&main_loading>16))){DemoSave("strict-waiting");saved=true;}
 if(loading&&upper&&!demo_slow->current_order.IsAnyLoadingType()&&x>86&&row==40){fprintf(demo_log,"FAIL changed/conditional departure used wrong outlet\n");exit(187);}
 const bool departed=upper?row==25&&x>104:row==40&&x>104;
 if(departed&&MixedReleased()&&MixedCandidateExpressTail(100)){
  if(!saved||(blocked?entered||!main_loading:!entered||!loading||!stopped||held<2||!pass_held||!resumed||fast_blocked)||(mode=="mixed-outlet-change"&&!mixed_candidate_changed)){
   fprintf(demo_log,"FAIL missing mixed candidate driving stage free=%u enter=%u loading=%u stop=%u held=%u pass=%u main_loading=%u actual_fast_block=%u resume=%u\n",free_seen,entered,loading,stopped,held,pass_held,main_loading,fast_blocked,resumed);exit(188);
  }
  fprintf(demo_log,"STRICT PASS mode=%.*s reload=%u current_station_served=1 platform_enter=%u main_loading=%u exit_stop=%u hold_ticks=%u pass_while_stopped=%u actual_shared_tail_clear=1 native_conditional_or_changed_goal=%u selected_outlet_row=%u resumed=%u released=1 actual_fast_block=%u\n",int(mode.size()),mode.data(),reload,entered,main_loading,stopped,held,pass_held,upper,row,resumed,fast_blocked);fflush(demo_log);_exit_game=true;
 }
 if(n%256==0)fprintf(demo_log,"MIXED CANDIDATE TRACE %u slow=%u,%u/%u order=%u index=%u fast=%u,%u/%u free_seen=%u free_now=%u entered=%u main_loading=%u\n",n,x,row,demo_slow->cur_speed,uint(demo_slow->current_order.GetType()),demo_slow->cur_real_order_index,TileX(demo_fast->tile),TileY(demo_fast->tile),demo_fast->cur_speed,free_seen,EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::FreeSiding),entered,main_loading);
 if(n>18000){fprintf(demo_log,"FAIL mixed candidate native timeout\n");fflush(demo_log);exit(189);}fflush(demo_log);
}
