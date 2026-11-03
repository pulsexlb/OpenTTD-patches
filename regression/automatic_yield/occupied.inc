/* New native parking must be usable while older sidings are physically full.
 * The setup trains drive from the depot under native PBS and wait at explicit
 * staging signals. No body relocation, specification edits or reservation
 * removal is used. The tested arrival has only normal automatic-yield control. */
namespace {
Train *occupied_old = nullptr, *occupied_second = nullptr;

void OccupiedStagingHold(uint row)
{
 auto *p=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(74,row),TRACK_X),false);
 const auto old=p->actions_used_flags;
 TraceRestrictInstructionItem wait{};wait.SetType(TRIT_WAIT_AT_PBS);p->items.push_back(wait.AsProgramItem());
 DemoRequire(p->Validate(),"stage occupied old exit");TraceRestrictCheckRefreshSignals(p,0,old);
}

void OccupiedMainStaging(bool hold)
{
 DemoSignal(54,40,AutoYieldRole::Main);
 if(!hold)return;
 auto *p=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(54,40),TRACK_X),false);
 const auto old=p->actions_used_flags;
 TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);p->items.push_back(deny.AsProgramItem());
 DemoRequire(p->Validate(),"stage occupied old route");TraceRestrictCheckRefreshSignals(p,0,old);
}

bool OccupiedParked(const Train *v,uint row)
{
 if(v==nullptr||TileY(v->tile)!=row||TileX(v->tile)<72||v->cur_speed!=0)return false;
 for(const Train *part=v;part;part=part->Next())if(TileY(part->tile)!=row||TileX(part->tile)<=50)return false;
 return true;
}

Train *OccupiedStagingTrain(const char *name)
{
 EngineID engine=EngineID::Invalid();
 /* A slower setup occupant must not itself become another legitimate fast
  * target of the tested arrival. It stays as a physical capacity constraint. */
 for(const Engine *e:Engine::Iterate())if(e->type==VehicleType::Train&&e->IsEnabled()&&RailVehInfo(e->index)->power&&RailVehInfo(e->index)->railtypes.Test(RAILTYPE_RAIL)&&e->GetDisplayMaxSpeed()>=64&&e->GetDisplayMaxSpeed()<uint(demo_slow->GetDisplayMaxSpeed())){
  if(engine==EngineID::Invalid()||e->GetDisplayMaxSpeed()>Engine::Get(engine)->GetDisplayMaxSpeed())engine=e->index;
 }
 if(engine==EngineID::Invalid()){fprintf(demo_log,"FAIL occupied staging engine unavailable\n");exit(140);}
 auto *v=DemoTrain(engine,name,GetStationIndex(TileXY(350,40)));
 DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,v->index,true),"drive old lane occupant");
 return v;
}

bool OccupiedSubjectReleased()
{
 for(const auto *slot:TraceRestrictSlot::Iterate())if(slot->automatic_yield_area&&slot->IsOccupant(demo_slow->index))return false;
 return !demo_slot->IsOccupant(demo_slow->index);
}
}

void OccupiedTick()
{
 static uint n=0,ready=0,held=0,pass_held=0;
 static bool first_parked=false,staged=false,express_started=false,built=false,entered=false,stopped=false,saved=false,cleared=false,resumed=false;
 const std::string_view mode=std::getenv("PXP_STRICT");
 const bool third=mode=="occupied-third",platform=mode=="occupied-platform",reload=std::getenv("PXP_STRICT_RELOAD")!=nullptr;
 const uint row=third?44:38;
 if(!demo_started&&_game_mode==GameMode::Editor){
  demo_started=true;BuildFarDestination();
  for(uint x=100;x<230;x+=20)DemoSignal(x,40);
  DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_slow->index,true),"hold subject in depot during setup");
  OccupiedMainStaging(true);OccupiedStagingHold(42);
  occupied_old=OccupiedStagingTrain("old occupied siding: native staging train");
  return;
 }
 if(_game_mode!=GameMode::Normal)return;
 if(n==0&&reload){
  demo_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/reload-strict.log").c_str(),"w");
  for(auto *v:Train::Iterate())if(v->IsPrimaryVehicle()){
   if(v->unitnumber==1)demo_slow=v;else if(v->unitnumber==2)demo_fast=v;else if(v->unitnumber==3)occupied_old=v;else if(v->unitnumber==4)occupied_second=v;
  }
  for(auto *slot:TraceRestrictSlot::Iterate())if(slot->automatic_yield_area==0)demo_slot=slot;
  if(!demo_slow||!demo_fast||!demo_slot||!occupied_old||(third&&!occupied_second))exit(141);
  first_parked=staged=express_started=built=entered=saved=true;_pause_mode={};
 }
 n++;demo_tick=n;
 if(!reload&&!first_parked&&OccupiedParked(occupied_old,42)){
  first_parked=true;
  fprintf(demo_log,"OLD_PHYSICALLY_FULL %u row=42 length=%u\n",n,occupied_old->gcache.cached_total_length);
  if(third){LayoutUpper();OccupiedStagingHold(38);occupied_second=OccupiedStagingTrain("second old occupied siding: native staging train");}
 }
 if(!reload&&first_parked&&!staged&&(!third||OccupiedParked(occupied_second,38))){
  staged=true;ready=n;OccupiedMainStaging(false);
  DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_slow->index,true),"start occupied-layout subject");
  fprintf(demo_log,"ALL_OLD_FULL %u count=%u actual_body_occupancy=1\n",n,third?2:1);
 }
 if(!reload&&staged&&!express_started&&n==ready+400){
  express_started=true;DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_fast->index,true),"start occupied-layout express");
 }
 if(!reload&&staged&&!built&&TileX(demo_slow->tile)>=25){
  if(TileY(demo_slow->tile)!=40||TileX(demo_slow->tile)>=45||!OccupiedParked(occupied_old,42)||(third&&!OccupiedParked(occupied_second,38))){fprintf(demo_log,"FAIL occupied new construction precondition\n");exit(142);}
  const bool free_before=EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::FreeSiding);
  if(free_before){fprintf(demo_log,"FAIL physically full old lanes reported free\n");exit(143);}
  if(third)LayoutThird();else LayoutUpper();
  if(platform){
   DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(62,38),RAILTYPE_RAIL,Axis::X,1,5,StationClassID{0},0,StationID::Invalid(),false),"new platform beside occupied siding");
   DemoRequire(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,TileXY(74,38),TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,AUTO_YIELD_DEFAULT_CONFIG|AUTO_YIELD_PLATFORM,""),"new occupied-layout platform marker");
  }
  AutoYieldSettingsChanged();
  if(!EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::FreeSiding)){fprintf(demo_log,"FAIL new native empty lane not discovered\n");exit(144);}
  built=true;fprintf(demo_log,"NEW_LANE_BUILT %u row=%u old_full=1 free_before=0 free_after=1 subject=%u,%u\n",n,row,TileX(demo_slow->tile),TileY(demo_slow->tile));
 }
 if(staged&&(!OccupiedParked(occupied_old,42)||(third&&!OccupiedParked(occupied_second,38)))){fprintf(demo_log,"FAIL old physical occupants moved during new-lane test\n");exit(145);}
 if(!entered&&TileY(demo_slow->tile)==row){entered=true;fprintf(demo_log,"NEW_LANE_ENTER %u row=%u\n",n,row);}
 if(built&&TileX(demo_slow->tile)>50&&TileX(demo_slow->tile)<80&&TileY(demo_slow->tile)!=40&&TileY(demo_slow->tile)!=row&&(TileY(demo_slow->tile)==38||TileY(demo_slow->tile)==42)){fprintf(demo_log,"FAIL arrival chose occupied old siding\n");exit(146);}
 if(entered&&OccupiedParked(demo_slow,row)){
  if(!stopped){stopped=true;fprintf(demo_log,"NEW_LANE_STOP %u tail_inside=1\n",n);}
  held++;if(!saved&&!reload){DemoSave("strict-waiting");saved=true;}
 }
 if(!cleared&&FarTailClear(demo_fast)){cleared=true;fprintf(demo_log,"EXPRESS_ACTUAL_TAIL_CLEAR %u\n",n);}
 if(stopped&&!cleared){
  if(demo_slow->cur_speed||TrainReservationPassesThroughTile(demo_slow,TileXY(75,row),TRACK_X)){fprintf(demo_log,"FAIL new lane prematurely released subject\n");exit(147);}
  if(TileY(demo_fast->tile)==40&&TileX(demo_fast->tile)>=54&&TileX(demo_fast->tile)<=90){if(demo_fast->cur_speed==0){fprintf(demo_log,"FAIL occupied-layout priority express stopped\n");exit(148);}pass_held++;}
 }
 if(stopped&&cleared&&demo_slow->cur_speed&&!resumed){resumed=true;fprintf(demo_log,"NEW_LANE_RESUME %u\n",n);}
 if(built&&entered&&!cleared&&TileY(demo_slow->tile)==40&&TileX(demo_slow->tile)>=84){fprintf(demo_log,"FAIL new lane arrival left before express tail\n");exit(149);}
 if(resumed&&TileY(demo_slow->tile)==40&&TileX(demo_slow->tile)>94&&OccupiedSubjectReleased()){
  if(!built||!entered||!stopped||!saved||held<2||pass_held==0){fprintf(demo_log,"FAIL missing occupied-layout driving stage\n");exit(150);}
  fprintf(demo_log,"STRICT PASS mode=%s old_physically_occupied=%u new_row=%u entered=1 stopped=1 held=%u pass_while_stopped=%u actual_tail_clear=1 resumed=1 subject_claims_released=1 reload=%u\n",std::string(mode).c_str(),third?2:1,row,held,pass_held,reload);fflush(demo_log);_exit_game=true;
 }
 if(n%512==0)fprintf(demo_log,"OCCUPIED TRACE %u ready=%u built=%u slow=%u,%u/%u fast=%u,%u/%u held=%u\n",n,ready,built,TileX(demo_slow->tile),TileY(demo_slow->tile),demo_slow->cur_speed,TileX(demo_fast->tile),TileY(demo_fast->tile),demo_fast->cur_speed,held);
 if(n>24000){fprintf(demo_log,"FAIL occupied-layout timeout\n");fflush(demo_log);exit(151);}fflush(demo_log);
}
