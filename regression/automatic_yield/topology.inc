/* Native merge/split fixtures. Stock rolling stock, real construction/orders,
 * actual tails and PBS reservations only; no engine edits or path erasure. */
namespace {
Train *topology_extra = nullptr;
uint topology_clear_x = 90;
uint topology_fast_row = 40;
bool topology_extra_started = false;

void TopologyProgram(uint x, uint y, Track track, Trackdir direction, std::optional<AutoYieldRole> role)
{
 const TileIndex tile=TileXY(x,y);
 DemoRequire(CmdBuildSingleSignal(DoCommandFlag::Execute,tile,track,SignalType::PathOneWay,SignalVariant::Electric,0,1,BuildSignalFlags::None,{},0,SignalAlongTrackdir(direction)),"topology signal");
 if(!role)return;
 auto*p=GetTraceRestrictProgram(MakeTraceRestrictRefId(tile,track),true);auto old=p->actions_used_flags;
 TraceRestrictInstructionItem item{};item.SetType(*role==AutoYieldRole::Clear?TRIT_AUTO_YIELD_CLEAR:TRIT_AUTO_YIELD_AREA);item.SetValue(demo_slot->index);
 if(*role!=AutoYieldRole::Clear)item.SetAuxField(to_underlying(*role));
 p->items={item.AsProgramItem()};if(*role!=AutoYieldRole::Clear)p->items.emplace_back(AUTO_YIELD_DEFAULT_CONFIG);
 DemoRequire(p->Validate(),"topology marker");TraceRestrictCheckRefreshSignals(p,0,old);
}
void TopologyClearProgram(uint x,uint y)
{
 auto*p=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(x,y),TRACK_X),false);auto old=p->actions_used_flags;p->items.clear();
 DemoRequire(p->Validate(),"replace topology marker");TraceRestrictCheckRefreshSignals(p,0,old);
}
void TopologyReplaceOrders(Train*train,StationID station,TileIndex depot)
{
 while(train->GetNumOrders())DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,train->index.base(),0),"topology replace orders");
 Order goal;goal.MakeGoToStation(station);goal.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);goal.SetLoadType(OrderLoadType::NoLoad);
 DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(train->index,VehicleOrderID{0},goal)),"topology terminal order");
 Order finish;finish.MakeGoToDepot(GetDepotDestinationIndex(depot),{},ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS,ODATFB_HALT);
 DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(train->index,VehicleOrderID{1},finish)),"topology finish order");
}
void TopologyPassStation(Train*train,uint x,uint y)
{
 DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(x,y),RAILTYPE_RAIL,Axis::X,1,5,StationClassID{0},0,StationID::Invalid(),false),"topology route station");
 Order via;via.MakeGoToStation(GetStationIndex(TileXY(x,y)));via.SetNonStopType(ONSF_NO_STOP_AT_ANY_STATION);via.SetLoadType(OrderLoadType::NoLoad);
 DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(train->index,VehicleOrderID{0},via)),"topology pass station");
 DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,train->index,VehicleOrderID{0}),"topology activate route");
}
bool TopologyTailClear(const Train*train)
{
 if(demo_slot->IsOccupant(train->index))return false;
 for(const Train*part=train;part;part=part->Next()){
  if(TileX(part->tile)<=topology_clear_x)return false;
  if(TileY(part->tile)!=40&&TileY(part->tile)!=25&&TileY(part->tile)!=20)return false;
 }
 return true;
}
bool TopologyAllResourcesReleased()
{
 for(auto*s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area&&!s->occupants.empty())return false;
 return true;
}
void TopologyBuild(std::string_view mode)
{
 BuildFarDestination();
 /* Ordinary PBS signals beyond the area's real tail clearance prevent the
  * distant terminal's block from becoming the measured resource. */
 for(uint x:{180u,210u,250u,280u,310u,340u})DemoSignal(x,40);
 if(mode=="topology-split"){
  const EngineID engine=demo_fast->engine_type;delete demo_fast;demo_fast=nullptr;
  for(uint x=6;x<=445;x++)DemoRail(x,20,TRACK_X);
  DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(5,20),RAILTYPE_RAIL,DiagDirection::SW),"independent origin depot");
  DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(446,20),RAILTYPE_RAIL,DiagDirection::NE),"independent finish depot");
  DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(350,20),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"independent destination");
  DemoSignal(16,20,AutoYieldRole::Approach);DemoSignal(54,20,AutoYieldRole::Main);DemoSignal(90,20,AutoYieldRole::Clear);
  for(uint x:{25u,35u,45u,180u,210u,240u,270u,300u,330u})DemoSignal(x,20);
  /* Real divergence upstream and convergence downstream, with both trains
   * committed to independent routes inside the marked station resources. */
  DemoRail(10,40,TRACK_UPPER);for(uint y=21;y<40;y++)DemoRail(10,y,TRACK_Y);DemoRail(10,20,TRACK_LOWER);
  DemoRail(170,20,TRACK_RIGHT);for(uint y=21;y<40;y++)DemoRail(170,y,TRACK_Y);DemoRail(170,40,TRACK_LEFT);
  SetSignalType(TileXY(90,20),TRACK_X,SignalType::PathOneWay);
  demo_fast=DemoTrain(engine,"independent express: upper route",GetStationIndex(TileXY(350,20)),TileXY(5,20));
  TopologyReplaceOrders(demo_fast,GetStationIndex(TileXY(350,20)),TileXY(446,20));
  TopologyPassStation(demo_slow,110,40);TopologyPassStation(demo_fast,110,20);
  topology_fast_row=20;
  DemoSign(52,16,"独立分流：共用路签，但两列车实际进路互不相交");
 }else{
  /* Both distinct entrances merge at 40,40, then the main/parking pair
   * separates and rejoins; a later physical split gives distinct exits. */
  DemoRail(110,40,TRACK_UPPER);for(uint y=26;y<40;y++)DemoRail(110,y,TRACK_Y);DemoRail(110,25,TRACK_LOWER);
  for(uint x=111;x<=445;x++)DemoRail(x,25,TRACK_X);
  DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(350,25),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"split outlet destination");
  DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(446,25),RAILTYPE_RAIL,DiagDirection::NE),"split outlet depot");
  DemoSignal(125,25);for(uint x:{180u,210u,240u,270u,300u,330u})DemoSignal(x,25);
  TopologyReplaceOrders(demo_fast,GetStationIndex(TileXY(350,25)),TileXY(446,25));
  if(mode.starts_with("topology-merge3")){
   DemoRail(70,40,TRACK_LEFT);for(uint y=11;y<40;y++)DemoRail(70,y,TRACK_Y);
   DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(70,10),RAILTYPE_RAIL,DiagDirection::SE),"third entrance depot");
   TopologyProgram(70,12,TRACK_Y,TRACKDIR_Y_SE,AutoYieldRole::Approach);
   TopologyProgram(70,30,TRACK_Y,TRACKDIR_Y_SE,AutoYieldRole::Main);
   DemoSignal(58,40,AutoYieldRole::Main);DemoSignal(62,40,AutoYieldRole::Main);
   if(mode=="topology-merge3-signals"){DemoSignal(60,42);DemoSignal(66,42);}
   topology_extra=DemoTrain(demo_fast->engine_type,"third entrance express",GetStationIndex(TileXY(350,25)),TileXY(70,10));
   TopologyReplaceOrders(topology_extra,GetStationIndex(TileXY(350,25)),TileXY(446,25));
   DemoSign(70,7,"第三入口：等待中的慢车还须等这列快车");
  }
  if(mode=="topology-rejoin"){
   /* A route-specific split and a subsequent real rejoin remain inside the
    * same protected resource. No-stop station orders prove each branch. */
   DemoRail(110,25,TRACK_RIGHT);for(uint y=26;y<40;y++)DemoRail(145,y,TRACK_Y);
   DemoRail(145,25,TRACK_RIGHT);DemoRail(145,40,TRACK_LEFT);
   TopologyClearProgram(90,40);DemoSignal(160,40,AutoYieldRole::Clear);topology_clear_x=160;
   TopologyReplaceOrders(demo_fast,GetStationIndex(TileXY(350,40)),TileXY(446,40));
   TopologyPassStation(demo_slow,120,40);TopologyPassStation(demo_fast,120,25);
   DemoSign(111,21,"分流后再合流：快车走上支线，慢车仍须等车尾");
   DemoSign(161,36,"实际共享出清点：快车整列通过后慢车恢复");
  }
  DemoSign(39,65,"两入口先合流，站内有正线、待避线及出口分流");
  DemoSign(112,44,"出口分流：共用前段仍需正确让行");
 }
 AutoYieldSettingsChanged();
 fprintf(demo_log,"TOPOLOGY READY mode=%.*s slow_design=%u fast_design=%u third=%u clear=%u\n",int(mode.size()),mode.data(),demo_slow->GetDisplayMaxSpeed(),demo_fast->GetDisplayMaxSpeed(),topology_extra!=nullptr,topology_clear_x);
 fflush(demo_log);
}
}

void TopologyTick()
{
 static uint n=0,held=0,first_pass_held=0,extra_pass_held=0,negative_overlap=0;
 static bool entered=false,stopped=false,saved=false,resumed=false,conflict=false,first_clear=false,extra_clear=false,first_started=false,portable_saved=false;
 const std::string_view mode=std::getenv("PXP_STRICT");const bool reload=std::getenv("PXP_STRICT_RELOAD")!=nullptr;
 const bool independent=mode=="topology-split",third=mode.starts_with("topology-merge3");
 if(!demo_started&&_game_mode==GameMode::Editor){demo_started=true;TopologyBuild(mode);return;}
 if(_game_mode!=GameMode::Normal)return;
 if(n==0&&reload){
  demo_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/reload-strict.log").c_str(),"w");
  for(auto*v:Train::Iterate())if(v->IsPrimaryVehicle()){if(v->unitnumber==1)demo_slow=v;else if(v->unitnumber==2)demo_fast=v;else if(v->unitnumber==3)topology_extra=v;}
  for(auto*s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area==0)demo_slot=s;
  if(!demo_slow||!demo_fast||!demo_slot||(third&&!topology_extra))exit(130);
  topology_clear_x=mode=="topology-rejoin"?160:90;topology_fast_row=independent?20:40;
  first_started=true;topology_extra_started=third;entered=TileY(demo_slow->tile)==42;saved=true;_pause_mode={};
 }
 n++;demo_tick=n;
 if(n==1&&std::getenv("PXP_STRICT_DEBUG"))_debug_levels[size_t(DebugLevelID::yapf)]=7;
 if(!first_started&&!reload&&n==(independent?100u:400u)){first_started=true;DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_fast->index,true),"start topology express");}
 const bool need=EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::NeedsSiding);
 if(need&&!conflict){conflict=true;fprintf(demo_log,"TOPOLOGY CONFLICT %u\n",n);}
 if(!entered&&TileY(demo_slow->tile)==42){entered=true;fprintf(demo_log,"TOPOLOGY ENTER %u\n",n);}
 if(entered&&TileY(demo_slow->tile)==42&&TileX(demo_slow->tile)>=72&&demo_slow->cur_speed==0){
  if(!stopped){stopped=true;fprintf(demo_log,"TOPOLOGY STOP %u\n",n);}
  for(const Train*part=demo_slow;part;part=part->Next())if(TileY(part->tile)!=42||TileX(part->tile)<=50){fprintf(demo_log,"FAIL topology tail not safely parked\n");exit(131);}
  held++;
 }
 if(third&&n==1000&&!topology_extra_started&&!reload){topology_extra_started=true;DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,topology_extra->index,true),"start third entrance express");fprintf(demo_log,"TOPOLOGY START_THIRD %u\n",n);}
 if(!reload&&!portable_saved&&n==(third?1000u:independent?300u:800u)){
  if(demo_fast->vehstatus.Test(VehState::Stopped)||(third&&topology_extra->vehstatus.Test(VehState::Stopped))){fprintf(demo_log,"FAIL portable snapshot needs future start command\n");exit(139);}
  DemoSave("topology-portable");portable_saved=true;
 }
 if(!first_clear&&TopologyTailClear(demo_fast)){first_clear=true;fprintf(demo_log,"TOPOLOGY FIRST_TAIL_CLEAR %u\n",n);}
 if(third&&!extra_clear&&TopologyTailClear(topology_extra)){extra_clear=true;fprintf(demo_log,"TOPOLOGY THIRD_TAIL_CLEAR %u\n",n);}
 const bool all_clear=first_clear&&(!third||extra_clear);
 if(stopped&&!all_clear){
  if(demo_slow->cur_speed||TrainReservationPassesThroughTile(demo_slow,TileXY(75,42),TRACK_X)){fprintf(demo_log,"FAIL topology moved/reserved while actual fast tail pending tick=%u\n",n);exit(132);}
  const auto pass=[&](Train*v,uint&counter){if(TileY(v->tile)==40&&TileX(v->tile)>=74&&TileX(v->tile)<=90){if(v->cur_speed==0){fprintf(demo_log,"FAIL topology express stopped in safe main corridor\n");exit(133);}counter++;}};
  pass(demo_fast,first_pass_held);if(third)pass(topology_extra,extra_pass_held);
 }
 if(stopped&&all_clear&&demo_slow->cur_speed&&!resumed){resumed=true;fprintf(demo_log,"TOPOLOGY RESUME %u\n",n);}
 if(!independent&&entered&&!all_clear&&TileY(demo_slow->tile)==40&&TileX(demo_slow->tile)>=80){fprintf(demo_log,"FAIL topology left before all tails clear\n");exit(134);}
 if(independent){
  if(need||entered){fprintf(demo_log,"FAIL independent split was locked by unrelated express tick=%u need=%u entered=%u\n",n,need,entered);exit(135);}
  if(demo_slot->IsOccupant(demo_slow->index)&&demo_slot->IsOccupant(demo_fast->index)){negative_overlap++;if(demo_slow->cur_speed==0&&TileX(demo_slow->tile)>=35){fprintf(demo_log,"FAIL independent split stopped slow train\n");exit(136);}}
 }
 if(!saved&&!reload&&((stopped&&(!third||StreamHasTarget(topology_extra)))||(independent&&negative_overlap>16))){DemoSave("strict-waiting");saved=true;}
 if(all_clear&&TileY(demo_slow->tile)==40&&TileX(demo_slow->tile)>topology_clear_x+4&&!demo_slot->IsOccupant(demo_slow->index)&&TopologyAllResourcesReleased()){
  if(!saved||(independent?negative_overlap==0:!entered||!stopped||held<2||!resumed||first_pass_held==0||(third&&extra_pass_held==0)||(!reload&&!conflict))){fprintf(demo_log,"FAIL topology missing mandatory stage negative_overlap=%u held=%u first_pass=%u extra_pass=%u\n",negative_overlap,held,first_pass_held,extra_pass_held);exit(137);}
  fprintf(demo_log,"STRICT PASS mode=%.*s reload=%u entered=%u stopped=%u held=%u first_pass_held=%u third_pass_held=%u all_actual_tails_clear=1 resumed=%u released=1 independent_overlap=%u\n",int(mode.size()),mode.data(),reload,entered,stopped,held,first_pass_held,extra_pass_held,resumed,negative_overlap);fflush(demo_log);_exit_game=true;
 }
 if(n%256==0)fprintf(demo_log,"TOPOLOGY TRACE %u slow=%u,%u/%u need=%u first=%u,%u/%u target=%u third=%u,%u/%u target=%u\n",n,TileX(demo_slow->tile),TileY(demo_slow->tile),demo_slow->cur_speed,need,TileX(demo_fast->tile),TileY(demo_fast->tile),demo_fast->cur_speed,StreamHasTarget(demo_fast),topology_extra?TileX(topology_extra->tile):0,topology_extra?TileY(topology_extra->tile):0,topology_extra?topology_extra->cur_speed:0,topology_extra&&StreamHasTarget(topology_extra));
 if(n>20000){fprintf(demo_log,"FAIL topology timeout\n");fflush(demo_log);exit(138);}fflush(demo_log);
}
