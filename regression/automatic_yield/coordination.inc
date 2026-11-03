namespace {
Train *turnover_holder=nullptr;
bool turnover_held=false,turnover_entered=false,turnover_waited=false,turnover_passed=false,turnover_tail=false;
bool turnover_unlocked=false,turnover_departed=false;
uint turnover_start=0,turnover_wait_ticks=0;
void VerticalMarker(uint x,uint y,AutoYieldRole role)
{
 TileIndex tile=TileXY(x,y);
 DemoRequire(CmdBuildSingleSignal(DoCommandFlag::Execute,tile,TRACK_Y,SignalType::PathOneWay,SignalVariant::Electric,0,1,BuildSignalFlags::None,{},0,SignalAlongTrackdir(TRACKDIR_Y_NW)),"vertical detector");
 auto *p=GetTraceRestrictProgram(MakeTraceRestrictRefId(tile,TRACK_Y),true);
 TraceRestrictInstructionItem item{};item.SetType(role==AutoYieldRole::Clear?TRIT_AUTO_YIELD_CLEAR:TRIT_AUTO_YIELD_AREA);item.SetValue(demo_slot->index);if(role!=AutoYieldRole::Clear)item.SetAuxField(to_underlying(role));
 p->items={item.AsProgramItem()};if(role!=AutoYieldRole::Clear)p->items.emplace_back(AUTO_YIELD_DEFAULT_CONFIG);
 DemoRequire(p->Validate(),"vertical marker");TraceRestrictCheckRefreshSignals(p,0,TRPAUF_NONE);
}
void HoldExit(bool hold)
{
 auto*p=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(74,42),TRACK_X),false);auto old=p->actions_used_flags;
 p->items.resize(2);
 if(hold){TraceRestrictInstructionItem wait{};wait.SetType(TRIT_WAIT_AT_PBS);p->items.push_back(wait.AsProgramItem());}
 DemoRequire(p->Validate(),"hold exit");TraceRestrictCheckRefreshSignals(p,0,old);
}
void BuildTurnover(bool close_express)
{
 DemoBuild();
 for(uint x=100;x<230;x+=20)DemoSignal(x,40);
 DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_slow->index,true),"hold incoming in depot");
 /* Generate a 100-speed engine before building the actual train. Movement,
  * acceleration and cache construction remain the native game code; no
  * per-tick position or decision result is mocked. */
 EngineID slow_engine=demo_slow->engine_type;delete demo_slow;demo_slow=nullptr;
 Engine::Get(slow_engine)->VehInfo<RailVehicleInfo>().max_speed=100;
 demo_slow=DemoTrain(slow_engine,"100 incoming",GetStationIndex(TileXY(235,40)));
 EngineID fast_engine=demo_fast->engine_type;delete demo_fast;demo_fast=nullptr;
 const uint source=close_express?54:126, feeder=close_express?70:30;
 DemoRail(feeder,40,TRACK_LOWER);for(uint y=41;y<source;y++)DemoRail(feeder,y,TRACK_Y);
 DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(feeder,source),RAILTYPE_RAIL,DiagDirection::NW),"fast feeder depot");
 VerticalMarker(feeder,source-(close_express?2:6),AutoYieldRole::Approach);
 if(close_express)VerticalMarker(feeder,48,AutoYieldRole::Main);
 demo_fast=DemoTrain(fast_engine,"turnover express",GetStationIndex(TileXY(235,40)),TileXY(feeder,source));
 EngineID holder=EngineID::Invalid();
 for(const Engine*e:Engine::Iterate())if(e->type==VehicleType::Train&&e->IsEnabled()&&RailVehInfo(e->index)->power&&RailVehInfo(e->index)->railtypes.Test(RAILTYPE_RAIL)&&e->GetDisplayMaxSpeed()>=150&&e->GetDisplayMaxSpeed()<=165)holder=e->index;
 if(holder==EngineID::Invalid())exit(60);
 turnover_holder=DemoTrain(holder,"turnover holder",GetStationIndex(TileXY(235,40)));
 auto*p=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(54,40),TRACK_X),false);auto old=p->actions_used_flags;
 TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);p->items.push_back(deny.AsProgramItem());DemoRequire(p->Validate(),"force initial siding");TraceRestrictCheckRefreshSignals(p,0,old);
 HoldExit(true);
 DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,turnover_holder->index,true),"start holder");
 AutoYieldSettingsChanged();
}
}
void CoordinationTick()
{
 static uint n=0;
 const bool close_express=std::string_view(std::getenv("PXP_STRICT"))=="turnover-near";
 if(!demo_started&&_game_mode==GameMode::Editor){demo_started=true;BuildTurnover(close_express);}
 if(_game_mode!=GameMode::Normal)return;
 n++;demo_tick=n;
 if(TileY(turnover_holder->tile)==42&&!turnover_entered){
  turnover_entered=true;DemoSignal(54,40,AutoYieldRole::Main);fprintf(demo_log,"HOLDER_ENTER %u\n",n);
 }
 if(turnover_entered&&!turnover_held&&turnover_holder->cur_speed==0&&TileX(turnover_holder->tile)>=72){
  turnover_held=true;turnover_start=n;fprintf(demo_log,"HOLDER_STOP %u\n",n);
  DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_slow->index,true),"start incoming");
 }
 if(turnover_held&&n==turnover_start+500)DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_fast->index,true),"start express");
 if(turnover_held&&!turnover_unlocked&&TileX(demo_slow->tile)>=35&&demo_slot->IsOccupant(demo_fast->index)){
  HoldExit(false);turnover_unlocked=true;fprintf(demo_log,"REASSESS %u slow=%u,%u fast=%u,%u\n",n,TileX(demo_slow->tile),TileY(demo_slow->tile),TileX(demo_fast->tile),TileY(demo_fast->tile));
 }
 if(turnover_unlocked&&!turnover_departed&&turnover_holder->cur_speed>0){
  turnover_departed=true;fprintf(demo_log,"HOLDER_DEPART %u fast=%u,%u\n",n,TileX(demo_fast->tile),TileY(demo_fast->tile));
 }
 if(turnover_departed&&!turnover_tail&&!demo_slot->IsOccupant(turnover_holder->index)&&TileX(turnover_holder->tile)>90){turnover_tail=true;fprintf(demo_log,"HOLDER_CLEAR %u fast=%u,%u\n",n,TileX(demo_fast->tile),TileY(demo_fast->tile));}
 if(TileY(demo_slow->tile)==42&&demo_slow->cur_speed==0){turnover_waited=true;turnover_wait_ticks++;}
 if(TileX(demo_fast->tile)>=80&&TileX(demo_fast->tile)<90){
  if(demo_fast->cur_speed==0){fprintf(demo_log,"FAIL turnover express stopped in conflict %u\n",n);fflush(demo_log);exit(65);}
  turnover_passed=true;
  if(!close_express&&(!turnover_tail||!turnover_waited||demo_slow->cur_speed!=0)){fprintf(demo_log,"FAIL turnover passage order %u\n",n);fflush(demo_log);exit(61);}
 }
 if(turnover_unlocked&&close_express&&turnover_departed&&!turnover_passed){fprintf(demo_log,"FAIL close express cut off\n");fflush(demo_log);exit(62);}
 if(turnover_passed&&turnover_tail&&TileX(demo_slow->tile)>94&&!demo_slot->IsOccupant(demo_slow->index)){
  if(!close_express&&(!turnover_waited||turnover_wait_ticks<2)){fprintf(demo_log,"FAIL no incoming wait\n");exit(63);}
  fprintf(demo_log,"STRICT PASS mode=turnover close_express=%u incoming_wait=%u holder_clear=1 express_pass=1 released=1\n",close_express,turnover_waited);fflush(demo_log);_exit_game=true;
 }
 if(n%256==0)fprintf(demo_log,"TURN %u holder=%u,%u/%u slow=%u,%u/%u fast=%u,%u/%u evict=%u\n",n,TileX(turnover_holder->tile),TileY(turnover_holder->tile),turnover_holder->cur_speed,TileX(demo_slow->tile),TileY(demo_slow->tile),demo_slow->cur_speed,TileX(demo_fast->tile),TileY(demo_fast->tile),demo_fast->cur_speed,EvaluateAutoYield(turnover_holder,demo_slot->index.base(),AutoYieldCondition::Evacuate));
 if(n>15000){fprintf(demo_log,"FAIL turnover timeout\n");fflush(demo_log);exit(64);}fflush(demo_log);
}
