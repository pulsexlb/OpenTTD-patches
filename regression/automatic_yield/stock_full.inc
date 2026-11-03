/* Fully occupied two-siding scenarios using unmodified, save-portable stock
 * engines. Initial signal holds only stage the occupants; automatic yielding
 * controls the entire contested departure, actual wait and final release. */
namespace {
Train *stock_holder=nullptr,*stock_other=nullptr,*stock_nearest=nullptr;
EngineID StockEngine(uint speed)
{
 for(const Engine*e:Engine::Iterate())if(e->type==VehicleType::Train&&e->IsEnabled()&&RailVehInfo(e->index)->power&&RailVehInfo(e->index)->railtypes.Test(RAILTYPE_RAIL)&&e->GetDisplayMaxSpeed()==speed)return e->index;
 fprintf(demo_log,"FAIL no stock engine at %u\n",speed);exit(100);
}
void StockHold(uint row,bool hold)
{
 auto*p=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(74,row),TRACK_X),false);auto old=p->actions_used_flags;
 p->items.resize(2);if(hold){TraceRestrictInstructionItem wait{};wait.SetType(TRIT_WAIT_AT_PBS);p->items.push_back(wait.AsProgramItem());}
 DemoRequire(p->Validate(),"stock staging hold");TraceRestrictCheckRefreshSignals(p,0,old);
}
bool StockParked(const Train*v,uint row){return TileY(v->tile)==row&&TileX(v->tile)>=72&&v->cur_speed==0;}
bool StockOutside(const Train*v){return TileY(v->tile)==40&&TileX(v->tile)>94&&!demo_slot->IsOccupant(v->index);}
}
void StockFullTick()
{
 static uint n=0,ready=0,held=0,pass_held=0;
 static bool first_parked=false,both_parked=false,unlocked=false,holder_left=false,holder_clear=false;
 static bool entered=false,stopped=false,fast_clear=false,resumed=false,saved=false,front_main=false;
 const std::string_view mode=std::getenv("PXP_STRICT");
 const bool closest=mode=="stock-full-closest",nearby=mode=="stock-full-near",long_block=mode=="stock-full-long-block",long_train=mode=="stock-full-long-train",reload=std::getenv("PXP_STRICT_RELOAD");
 if(!demo_started&&_game_mode==GameMode::Editor){
  demo_started=true;BuildFarDestination();
  if(!long_block)for(uint x=100;x<230;x+=20)DemoSignal(x,40);
  DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_slow->index,true),"stage initial stock arrival in depot");
  auto*p=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(54,40),TRACK_X),false);auto old=p->actions_used_flags;
  TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);p->items.push_back(deny.AsProgramItem());DemoRequire(p->Validate(),"stage initial siding occupancy");TraceRestrictCheckRefreshSignals(p,0,old);
  stock_holder=DemoTrain(StockEngine(160),"stock fastest occupied siding",GetStationIndex(TileXY(350,40)));
  if(long_train){
   for(uint i=0;i<16;i++){
    auto result=CmdBuildVehicle(DoCommandFlag::Execute,TileXY(5,40),EngineID{27},false,INVALID_CARGO,ClientID::Invalid);DemoRequire(result,"long stock holder wagon");
    const VehicleID id=result.GetResultDataWithType().GetOrDefault<VehicleID>(VehicleID::Invalid());
    DemoRequire(CmdMoveRailVehicle(DoCommandFlag::Execute,id,stock_holder->Last()->index,MoveRailVehicleFlags::None),"extend native stock holder");
   }
   fprintf(demo_log,"LONG_HOLDER speed=%u length=%u\n",stock_holder->GetDisplayMaxSpeed(),stock_holder->gcache.cached_total_length);
  }
  StockHold(42,true);DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,stock_holder->index,true),"start stock holder");
  return;
 }
 if(_game_mode!=GameMode::Normal)return;
 if(reload&&n==0){
  demo_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/reload-strict.log").c_str(),"w");
  for(auto*v:Train::Iterate())if(v->IsPrimaryVehicle()){
   if(v->unitnumber==1)demo_slow=v;if(v->unitnumber==2)demo_fast=v;if(v->unitnumber==3)stock_holder=v;if(v->unitnumber==4)stock_other=v;if(v->unitnumber==5)stock_nearest=v;
  }
  for(auto*s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area==0)demo_slot=s;
  if(!demo_slow||!demo_fast||!stock_holder||!stock_other||(closest&&!stock_nearest))exit(101);
  first_parked=both_parked=unlocked=holder_left=holder_clear=true;_pause_mode={};front_main=closest;
 }
 n++;demo_tick=n;
 if(std::getenv("PXP_STRICT_DEBUG"))_debug_levels[size_t(DebugLevelID::yapf)]=both_parked&&n>=ready+600&&n<ready+2200?7:0;
 if(!reload&&!first_parked&&StockParked(stock_holder,42)){
  first_parked=true;LayoutUpper();StockHold(38,true);
  stock_other=DemoTrain(StockEngine(144),"stock second occupied siding",GetStationIndex(TileXY(350,40)));
  DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,stock_other->index,true),"start other siding occupant");
  fprintf(demo_log,"FIRST_PARKED %u speed=%u\n",n,stock_holder->GetDisplayMaxSpeed());
 }
 if(!reload&&first_parked&&!both_parked&&StockParked(stock_other,38)){
  both_parked=true;ready=n;DemoSignal(54,40,AutoYieldRole::Main);
  if(closest)stock_nearest=DemoTrain(StockEngine(120),"stock closest slow arrival",GetStationIndex(TileXY(350,40)));
  if(!nearby)DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_slow->index,true),"start stock main arrival");
  fprintf(demo_log,"BOTH_PARKED %u incoming=%u occupants=%u/%u express=%u\n",n,demo_slow->GetDisplayMaxSpeed(),stock_holder->GetDisplayMaxSpeed(),stock_other->GetDisplayMaxSpeed(),demo_fast->GetDisplayMaxSpeed());
 }
 // Leave a real gap for the front train's tail to release the shared PBS block.
 // A congested native block cannot justify evicting an occupant through it.
 if(!reload&&both_parked&&closest&&n==ready+1100)DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,stock_nearest->index,true),"start nearest arrival");
 if(!reload&&both_parked&&nearby&&n==ready+1000)DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_slow->index,true),"start close-express arrival");
 if(!reload&&both_parked&&n==ready+(nearby?0u:closest?1200u:400u))DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_fast->index,true),"start stock priority express");
 if(!reload&&both_parked&&!unlocked&&TileX(demo_slow->tile)>=35&&demo_slot->IsOccupant(demo_fast->index)){
  StockHold(42,false);StockHold(38,false);unlocked=true;fprintf(demo_log,"UNLOCK %u\n",n);
 }
 Train*arrival=closest?stock_nearest:demo_slow;
 if(closest&&TileY(demo_slow->tile)==40&&TileX(demo_slow->tile)>=54){front_main=true;}
 if(closest&&TileY(demo_slow->tile)==42){fprintf(demo_log,"FAIL front arrival stole closest train slot\n");exit(102);}
 if(unlocked&&!holder_left&&stock_holder->cur_speed>0){holder_left=true;fprintf(demo_log,"FASTEST_DEPART %u\n",n);}
 if(holder_left&&!holder_clear&&StockOutside(stock_holder)){holder_clear=true;fprintf(demo_log,"FASTEST_TAIL_CLEAR %u\n",n);}
 if(unlocked&&!fast_clear&&stock_other&&stock_other->cur_speed>0){fprintf(demo_log,"FAIL slower occupied train departed first\n");exit(103);}
 if(arrival&&unlocked&&TileY(arrival->tile)==42&&!entered){entered=true;fprintf(demo_log,"ARRIVAL_ENTER %u\n",n);}
 if(arrival&&entered&&StockParked(arrival,42)){
  if(!stopped){stopped=true;fprintf(demo_log,"ARRIVAL_STOP %u\n",n);}
  held++;
  if(!saved&&!reload&&!nearby&&!long_block){DemoSave("strict-waiting");saved=true;}
 }
 if(!fast_clear&&FarTailClear(demo_fast)){fast_clear=true;fprintf(demo_log,"EXPRESS_TAIL_CLEAR %u\n",n);}
 if((nearby||long_block)&&holder_left&&!fast_clear){fprintf(demo_log,"FAIL close or long-block express was cut off\n");exit(104);}
 if(stopped&&!fast_clear){
  if(arrival->cur_speed!=0||TrainReservationPassesThroughTile(arrival,TileXY(75,42),TRACK_X)){fprintf(demo_log,"FAIL incoming left its held exit\n");exit(105);}
  if(TileY(demo_fast->tile)==40&&TileX(demo_fast->tile)>=74&&TileX(demo_fast->tile)<=90){if(demo_fast->cur_speed==0){fprintf(demo_log,"FAIL express stopped in conflict\n");exit(106);}pass_held++;}
 }
 if(stopped&&fast_clear&&arrival->cur_speed>0&&!resumed){resumed=true;fprintf(demo_log,"ARRIVAL_RESUME %u\n",n);}
 if(unlocked&&fast_clear&&holder_clear&&arrival&&StockOutside(arrival)&&StockOutside(stock_other)){
  if(!nearby&&!long_block&&(!entered||!stopped||held<2||!resumed||pass_held==0||(closest&&!front_main))){fprintf(demo_log,"FAIL missing stock turnover phase\n");exit(107);}
  for(auto*s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area&&!s->occupants.empty()){fprintf(demo_log,"FAIL stock slot leak\n");exit(108);}
  fprintf(demo_log,"STRICT PASS mode=%s fastest_departed=1 incoming_entered=%u stopped=%u held=%u pass_while_stopped=%u resumed=%u closest_front_main=%u released=1 reload=%u\n",std::string(mode).c_str(),entered,stopped,held,pass_held,resumed,front_main,reload);fflush(demo_log);_exit_game=true;
 }
 if(n%512==0){fprintf(demo_log,"STOCK %u ready=%u slow=%u,%u/%u holder=%u,%u/%u other=%u,%u/%u express=%u,%u/%u",n,ready,TileX(demo_slow->tile),TileY(demo_slow->tile),demo_slow->cur_speed,TileX(stock_holder->tile),TileY(stock_holder->tile),stock_holder->cur_speed,stock_other?TileX(stock_other->tile):0,stock_other?TileY(stock_other->tile):0,stock_other?stock_other->cur_speed:0,TileX(demo_fast->tile),TileY(demo_fast->tile),demo_fast->cur_speed);if(stock_nearest)fprintf(demo_log," nearest=%u,%u/%u registered=%u",TileX(stock_nearest->tile),TileY(stock_nearest->tile),stock_nearest->cur_speed,demo_slot->IsOccupant(stock_nearest->index));fprintf(demo_log,"\n");}
 if(n>20000){fprintf(demo_log,"FAIL stock turnover timeout\n");fflush(demo_log);exit(109);}fflush(demo_log);
}
