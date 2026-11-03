/* Native late-arrival and serial-signal regression. Stock engines and electric
 * track keep all fixture data portable through an ordinary saved game. */
namespace {
std::array<Train *,3> stream_fast{};
std::array<bool,3> stream_tail{};
bool stream_second=false,stream_third=false;
uint stream_stop_tick=0;

void StreamVerticalSignal(uint x,uint y,AutoYieldRole role,Trackdir td=TRACKDIR_Y_NW)
{
 const TileIndex tile=TileXY(x,y);
 DemoRequire(CmdBuildSingleSignal(DoCommandFlag::Execute,tile,TRACK_Y,SignalType::PathOneWay,SignalVariant::Electric,0,1,BuildSignalFlags::None,{},0,SignalAlongTrackdir(td)),"stream feeder signal");
 auto*p=GetTraceRestrictProgram(MakeTraceRestrictRefId(tile,TRACK_Y),true);
 TraceRestrictInstructionItem item{};item.SetType(TRIT_AUTO_YIELD_AREA);item.SetAuxField(to_underlying(role));item.SetValue(demo_slot->index);
 p->items={item.AsProgramItem(),TraceRestrictProgramItem{AUTO_YIELD_DEFAULT_CONFIG|250u}};
 DemoRequire(p->Validate(),"stream feeder program");TraceRestrictCheckRefreshSignals(p,0,TRPAUF_NONE);
}
bool StreamHasTarget(const Train*train)
{
 for(auto*s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area==demo_slot->index.base()+1&&s->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets)&&s->IsOccupant(train->index))return true;
 return false;
}
bool StreamTailClear(const Train*train)
{
 if(demo_slot->IsOccupant(train->index))return false;
 for(const Train*part=train;part;part=part->Next())if((TileY(part->tile)!=40&&TileY(part->tile)!=25)||TileX(part->tile)<=90)return false;
 return true;
}
void BuildStream(bool serial)
{
 DemoBuild();
 /* Replace the initially parked express; no train position is moved. */
 delete demo_fast;demo_fast=nullptr;
 DemoRail(40,40,TRACK_LOWER);for(uint row=41;row<70;row++)DemoRail(40,row,TRACK_Y);
 DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(40,70),RAILTYPE_RAIL,DiagDirection::NW),"stream lower feeder depot");
 StreamVerticalSignal(40,68,AutoYieldRole::Approach);StreamVerticalSignal(40,50,AutoYieldRole::Main);
 /* The upper feeder merges after the third serial control without crossing
  * or shortening the slow train's exclusive parking segment below Main. */
 DemoRail(70,40,TRACK_LEFT);for(uint row=11;row<40;row++)DemoRail(70,row,TRACK_Y);
 DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(70,10),RAILTYPE_RAIL,DiagDirection::SE),"stream upper feeder depot");
 StreamVerticalSignal(70,12,AutoYieldRole::Approach,TRACKDIR_Y_SE);StreamVerticalSignal(70,30,AutoYieldRole::Main,TRACKDIR_Y_SE);
 if(serial){
  DemoSignal(58,40,AutoYieldRole::Main);DemoSignal(62,40,AutoYieldRole::Main);
  DemoSignal(60,42);DemoSignal(66,42);
 }
 DemoRequire(CmdConvertRail(DoCommandFlag::Execute,TileXY(0,0),TileXY(255,127),RAILTYPE_ELECTRIC,false),"electrified stream fixture");
 std::array<EngineID,3> engines{EngineID::Invalid(),EngineID::Invalid(),EngineID::Invalid()};
 for(const Engine*e:Engine::Iterate()){
  if(e->type!=VehicleType::Train||!e->IsEnabled()||!RailVehInfo(e->index)->power||!RailVehInfo(e->index)->railtypes.Any())continue;
  const uint speed=e->GetDisplayMaxSpeed();
  if(speed>=150&&speed<=165&&RailVehInfo(e->index)->railtypes.Test(RAILTYPE_RAIL))engines[0]=e->index;
  if(speed>=195&&speed<=210&&RailVehInfo(e->index)->railtypes.Test(RAILTYPE_RAIL))engines[1]=e->index;
  if(speed>=250&&speed<=280&&RailVehInfo(e->index)->railtypes.Test(RAILTYPE_ELECTRIC))engines[2]=e->index;
 }
 for(auto id:engines)if(id==EngineID::Invalid()){fprintf(demo_log,"FAIL missing stock stream engine\n");exit(80);}
 const StationID destination=GetStationIndex(TileXY(235,40));
 stream_fast[0]=DemoTrain(engines[0],"stream B 160",destination);
 stream_fast[1]=DemoTrain(engines[1],"stream C 200",destination,TileXY(40,70));
 stream_fast[2]=DemoTrain(engines[2],"stream D 250 capped",destination,TileXY(70,10));
 demo_fast=stream_fast[0];
 AutoYieldSettingsChanged();
 fprintf(demo_log,"STREAM ready slow=%u B=%u C=%u D=%u cap=250 serial=%u\n",demo_slow->GetDisplayMaxSpeed(),stream_fast[0]->GetDisplayMaxSpeed(),stream_fast[1]->GetDisplayMaxSpeed(),stream_fast[2]->GetDisplayMaxSpeed(),serial);
}
}

void StreamTick()
{
 static uint n=0,held=0;static bool entered=false,stopped=false,saved=false,resumed=false;
 static uint saved_tick=0;static bool rerouted=false,independent_release=false;
 const bool reload=std::getenv("PXP_STRICT_RELOAD")!=nullptr;
 const auto mode=std::string_view(std::getenv("PXP_STRICT"));
 const bool serial=mode=="stream-serial"||mode=="stream-reroute";
 if(!demo_started&&_game_mode==GameMode::Editor){demo_started=true;BuildStream(serial);}
 if(_game_mode!=GameMode::Normal)return;
 if(n==0&&reload){
  demo_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/reload-strict.log").c_str(),"w");
  for(auto*v:Train::Iterate())if(v->IsPrimaryVehicle()){
   if(v->unitnumber==1)demo_slow=v;
   else if(v->unitnumber>=2&&v->unitnumber<=4)stream_fast[v->unitnumber-2]=v;
  }
  for(auto*s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area==0)demo_slot=s;
  if(!demo_slow||!demo_slot||!stream_fast[0]||!stream_fast[1]||!stream_fast[2])exit(81);
  demo_fast=stream_fast[0];stream_second=true;stream_third=true;_pause_mode={};saved=true;
 }
 n++;demo_tick=n;
 if(n==1&&std::getenv("PXP_STRICT_DEBUG")){std::string path=std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/debug.log";freopen(path.c_str(),"w",stderr);_debug_levels[size_t(DebugLevelID::yapf)]=7;}
 if(!reload&&n==400)DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,stream_fast[0]->index,true),"start first stream express");
 if(TileY(demo_slow->tile)==42&&!entered){entered=true;fprintf(demo_log,"STREAM ENTER %u\n",n);}
 if(entered&&demo_slow->cur_speed==0&&TileX(demo_slow->tile)>=72&&!stopped){stopped=true;stream_stop_tick=n;fprintf(demo_log,"STREAM STOP %u\n",n);}
 if(stopped&&!stream_second&&n>=stream_stop_tick+8){stream_second=true;DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,stream_fast[1]->index,true),"start later stream express");fprintf(demo_log,"STREAM ADD_C %u\n",n);}
 for(size_t i=0;i<stream_fast.size();i++)if(!stream_tail[i]&&StreamTailClear(stream_fast[i])){stream_tail[i]=true;fprintf(demo_log,"STREAM TAIL_CLEAR %u index=%zu\n",n,i);}
 if(stream_tail[0]&&!stream_third){stream_third=true;DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,stream_fast[2]->index,true),"start third stream express");fprintf(demo_log,"STREAM ADD_D %u\n",n);}
 if(stopped&&!saved&&stream_third&&StreamHasTarget(stream_fast[1])&&StreamHasTarget(stream_fast[2])){DemoSave("strict-waiting");saved=true;saved_tick=n;fprintf(demo_log,"STREAM SAVED_ALL_LIVE %u\n",n);}
 if(mode=="stream-reroute"&&saved&&!rerouted&&n>saved_tick+32){
  DemoRail(70,25,TRACK_LEFT);for(uint x=71;x<=245;x++)DemoRail(x,25,TRACK_X);
  DemoRequire(CmdConvertRail(DoCommandFlag::Execute,TileXY(70,25),TileXY(245,25),RAILTYPE_ELECTRIC,false),"electrified divergent route");
  DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(235,25),RAILTYPE_ELECTRIC,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"divergent station");
  DemoSignal(90,25,AutoYieldRole::Clear);SetSignalType(TileXY(90,25),TRACK_X,SignalType::PathOneWay);
  Order order;order.MakeGoToStation(GetStationIndex(TileXY(235,25)));order.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
  DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(stream_fast[2]->index,VehicleOrderID{0},order)),"reroute D order");
  DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,stream_fast[2]->index,VehicleOrderID{0}),"activate independent D route");
  /* As in the native movement loop, resolve the changed order before asking
   * PBS for a path; otherwise this test re-reserves the stale destination. */
  ProcessOrders(stream_fast[2]);
  if(stream_fast[2]->current_order.GetDestination()!=GetStationIndex(TileXY(235,25))){fprintf(demo_log,"FAIL D order not activated\n");exit(86);}
  AutoYieldSettingsChanged();FreeTrainTrackReservation(stream_fast[2]);TryPathReserve(stream_fast[2],true,false);
  rerouted=true;fprintf(demo_log,"STREAM REROUTE_D %u\n",n);
 }
 if(rerouted&&!independent_release&&!StreamHasTarget(stream_fast[2])&&!stream_tail[1]&&TileY(stream_fast[2]->tile)==25&&TileX(stream_fast[2]->tile)>70){
  if(!StreamHasTarget(stream_fast[1])||demo_slow->cur_speed!=0){fprintf(demo_log,"FAIL independent D release also released C\n");fflush(demo_log);exit(85);}
  independent_release=true;fprintf(demo_log,"STREAM RELEASE_D_ONLY %u C_still_held=1\n",n);
 }
 const bool pending=(stream_second&&!stream_tail[1])||(stream_third&&!stream_tail[2]&&!independent_release)||!stream_tail[0];
 if(stopped&&pending){held++;if(demo_slow->cur_speed!=0){fprintf(demo_log,"FAIL stream departed with pending express tick=%u B=%u C=%u D=%u savedC=%u savedD=%u\n",n,stream_tail[0],stream_tail[1],stream_tail[2],StreamHasTarget(stream_fast[1]),StreamHasTarget(stream_fast[2]));fflush(demo_log);exit(82);}}
 if(stopped&&!pending&&demo_slow->cur_speed>0&&!resumed){resumed=true;fprintf(demo_log,"STREAM RESUME %u\n",n);}
 if(resumed&&TileY(demo_slow->tile)==40&&TileX(demo_slow->tile)>100&&!demo_slot->IsOccupant(demo_slow->index)){
  if(!saved||!entered||!stopped||held<2||(mode=="stream-reroute"&&!independent_release)){fprintf(demo_log,"FAIL stream missing phase or saved targets\n");exit(83);}
  fprintf(demo_log,"STRICT PASS mode=stream serial=%u reload=%u entered=1 stopped=1 B_tail=%u C_tail=%u D_tail=%u D_independent=%u held=%u resumed=1 released=1\n",serial,reload,stream_tail[0],stream_tail[1],stream_tail[2],independent_release,held);fflush(demo_log);_exit_game=true;
 }
 if(n%256==0){fprintf(demo_log,"STREAM TRACE %u slow=%u,%u/%u B=%u,%u/%u C=%u,%u/%u D=%u,%u/%u targets=%u,%u,%u\n",n,TileX(demo_slow->tile),TileY(demo_slow->tile),demo_slow->cur_speed,TileX(stream_fast[0]->tile),TileY(stream_fast[0]->tile),stream_fast[0]->cur_speed,TileX(stream_fast[1]->tile),TileY(stream_fast[1]->tile),stream_fast[1]->cur_speed,TileX(stream_fast[2]->tile),TileY(stream_fast[2]->tile),stream_fast[2]->cur_speed,StreamHasTarget(stream_fast[0]),StreamHasTarget(stream_fast[1]),StreamHasTarget(stream_fast[2]));if(n<1536)fprintf(demo_log,"STREAM CHECK %u slowmember=%u Bmember=%u need=%u free=%u\n",n,demo_slot->IsOccupant(demo_slow->index),demo_slot->IsOccupant(stream_fast[0]->index),EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::NeedsSiding),EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::FreeSiding));}
 if(n>18000){fprintf(demo_log,"FAIL stream timeout\n");fflush(demo_log);exit(84);}fflush(demo_log);
}
