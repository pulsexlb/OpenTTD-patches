/* Runtime edits use native construction commands and their reservation checks.
 * Never teleport a train or discard a PBS path to force the expected branch. */
namespace {
TraceRestrictSlot *LayoutLane(uint row)
{
 const uint x=std::getenv("PXP_DEMO_EXIT_ONLY")?74:54;
 const auto ref=MakeTraceRestrictRefId(TileXY(x,row),TRACK_X);
 for(auto*s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area==demo_slot->index.base()+1&&s->automatic_yield_signal==ref&&!s->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets))return s;
 return nullptr;
}
void LayoutUpper()
{
 DemoRail(50,40,TRACK_UPPER);DemoRail(50,39,TRACK_Y);DemoRail(50,38,TRACK_LOWER);
 for(uint x=51;x<80;x++)DemoRail(x,38,TRACK_X);
 DemoRail(80,38,TRACK_RIGHT);DemoRail(80,39,TRACK_Y);DemoRail(80,40,TRACK_LEFT);
 if(!std::getenv("PXP_DEMO_EXIT_ONLY"))DemoSignal(54,38,AutoYieldRole::Siding);
 DemoSignal(74,38,AutoYieldRole::Exit);
 // The program edits invalidate marker metadata; plain rail edits below do not.
 AutoYieldSettingsChanged();
}
void LayoutDeny(uint row)
{
 auto *p=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(74,row),TRACK_X),true);
 auto old=p->actions_used_flags;TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);p->items.push_back(deny.AsProgramItem());
 DemoRequire(p->Validate(),"unavailable old lane");TraceRestrictCheckRefreshSignals(p,0,old);
}
void LayoutThird()
{
 // A longer, asymmetric third lane, outside both older branches.
 DemoRail(48,40,TRACK_RIGHT);for(uint y=41;y<44;y++)DemoRail(48,y,TRACK_Y);DemoRail(48,44,TRACK_LEFT);
 for(uint x=49;x<84;x++)DemoRail(x,44,TRACK_X);
 DemoRail(84,44,TRACK_UPPER);for(uint y=41;y<44;y++)DemoRail(84,y,TRACK_Y);DemoRail(84,40,TRACK_LOWER);
 if(!std::getenv("PXP_DEMO_EXIT_ONLY"))DemoSignal(54,44,AutoYieldRole::Siding);
 DemoSignal(74,44,AutoYieldRole::Exit);
}
void LayoutRemoveOriginal()
{
 for(uint x=50;x<=80;x++)DemoRequire(CmdLandscapeClear(DoCommandFlag::Execute,TileXY(x,42)),"remove old empty siding");
 for(uint x:{50u,80u})DemoRequire(CmdLandscapeClear(DoCommandFlag::Execute,TileXY(x,41)),"remove old empty connection");
 DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(50,40),TRACK_RIGHT),"remove old inlet");
 DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(80,40),TRACK_LOWER),"remove old outlet");
}
}
void LayoutTick()
{
 static uint n=0,held=0,pass_held=0,edited_at=0;
 static bool edited=false,entered=false,stopped=false,cleared=false,resumed=false,saved=false,orphan_released=false;
 const std::string_view mode=std::getenv("PXP_STRICT");
 const bool reload=std::getenv("PXP_STRICT_RELOAD");
 const bool fresh=mode.starts_with("layout-new-");
 const uint row=mode=="layout-new-third"?44:mode=="layout-switch"||fresh?38:42;
 if(!demo_started&&_game_mode==GameMode::Editor){demo_started=true;BuildFarDestination();return;}
 if(_game_mode!=GameMode::Normal)return;
 if(n==0){
  if(reload){
   demo_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/reload-strict.log").c_str(),"w");
   for(auto*v:Train::Iterate())if(v->IsPrimaryVehicle()){if(v->unitnumber==1)demo_slow=v;if(v->unitnumber==2)demo_fast=v;}
   for(auto*s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area==0)demo_slot=s;
   if(!demo_slow||!demo_fast||!demo_slot)exit(70);
   _pause_mode={};edited=true;orphan_released=true;
  }else if(fresh){
   if(mode=="layout-new-first"||mode=="layout-new-platform")LayoutRemoveOriginal();
   else LayoutDeny(42);
   if(mode=="layout-new-third"){LayoutUpper();LayoutDeny(38);}
   if(EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::FreeSiding)){fprintf(demo_log,"FAIL parking available before new construction\n");exit(83);}
   if(mode=="layout-new-first"||mode=="layout-new-platform"){
    // Complete a brand-new facility before the first approach registration.
    // Reservation-mode manual cancellation must not be undone by construction
    // behind a train that already crossed an inactive detector.
    LayoutUpper();
    if(mode=="layout-new-platform"){
     DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(62,38),RAILTYPE_RAIL,Axis::X,1,5,StationClassID{0},0,StationID::Invalid(),false),"new platform parking");
     DemoRequire(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,TileXY(74,38),TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,AUTO_YIELD_DEFAULT_CONFIG|AUTO_YIELD_PLATFORM,""),"new platform marker");
    }
    edited=true;fprintf(demo_log,"EDIT 0 mode=%s before_first_registration=1\n",std::string(mode).c_str());
   }
  }else if(mode=="layout-add"){
   DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(50,41),TRACK_Y),"initially disconnected empty siding");
   if(EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::FreeSiding)){fprintf(demo_log,"FAIL disconnected lane available\n");exit(71);}
  }
 }
 n++;
 if(!reload&&n==400)DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_fast->index,true),"start layout express");
 auto*old=LayoutLane(42);
 const bool early_edit=fresh||mode=="layout-add"||(mode=="layout-switch"&&demo_slow->UsingRealisticBraking());
 if(!edited&&((early_edit&&TileX(demo_slow->tile)>=25)||(!early_edit&&old&&old->IsOccupant(demo_slow->index)))){
  if(TileY(demo_slow->tile)!=40||TileX(demo_slow->tile)>=45){fprintf(demo_log,"FAIL edit too late\n");exit(72);}
  if(fresh){
   if(mode=="layout-new-third")LayoutThird();else LayoutUpper();
   if(mode=="layout-new-platform"){
    DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(62,38),RAILTYPE_RAIL,Axis::X,1,5,StationClassID{0},0,StationID::Invalid(),false),"new platform parking");
    DemoRequire(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,TileXY(74,38),TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,AUTO_YIELD_DEFAULT_CONFIG|AUTO_YIELD_PLATFORM,""),"new platform marker");
   }
  }else if(mode=="layout-add")DemoRail(50,41,TRACK_Y);
  else{
   LayoutUpper();
   if(mode=="layout-switch"){
    /* Realistic braking forbids editing a moving train's reserved route.
     * Change this connection before acquisition; conventional braking also
     * exercises native safe re-reservation of an already selected lane. */
    DemoRequire(CmdRemoveSingleRail(DoCommandFlag::Execute,TileXY(50,41),TRACK_Y),"safely disconnect old chosen inlet");
   }else if(demo_slow->UsingRealisticBraking()){
    auto blocked=CmdRemoveSingleRail({},TileXY(50,41),TRACK_Y);
    if(blocked.Succeeded()){fprintf(demo_log,"FAIL moving reservation edit unexpectedly permitted\n");exit(82);}
    fprintf(demo_log,"NATIVE_EDIT_PROTECTED %u\n",n);
   }
  }
  edited=true;edited_at=n;
  fprintf(demo_log,"EDIT %u mode=%s slow=%u,%u old_claim=%u native_reserved_old=%u\n",n,std::string(mode).c_str(),TileX(demo_slow->tile),TileY(demo_slow->tile),old&&old->IsOccupant(demo_slow->index),TrainReservationPassesThroughTile(demo_slow,TileXY(74,42),TRACK_X));
 }
 if(mode=="layout-switch"&&edited&&!orphan_released){
  if(!LayoutLane(42)->IsOccupant(demo_slow->index)){orphan_released=true;fprintf(demo_log,"OLD_LANE_FREE %u source_retained=%u\n",n,demo_slot->IsOccupant(demo_slow->index));if(!demo_slot->IsOccupant(demo_slow->index))exit(73);}
  else if(n>edited_at+128){fprintf(demo_log,"FAIL old disconnected claim persists\n");exit(74);}
 }
 if(!entered&&TileY(demo_slow->tile)==row){entered=true;fprintf(demo_log,"ENTER %u row=%u\n",n,row);}
 if(TileY(demo_slow->tile)!=row&&TileY(demo_slow->tile)!=40&&(TileY(demo_slow->tile)==38||TileY(demo_slow->tile)==42)&&TileX(demo_slow->tile)>50&&TileX(demo_slow->tile)<80){fprintf(demo_log,"FAIL wrong siding\n");exit(75);}
 if(entered&&TileX(demo_slow->tile)>=72&&TileY(demo_slow->tile)==row&&demo_slow->cur_speed==0){
  if(!stopped){stopped=true;fprintf(demo_log,"STOP %u target=%u\n",n,StreamHasTarget(demo_fast));}
  for(const Train*part=demo_slow;part;part=part->Next())if(TileY(part->tile)!=row||TileX(part->tile)<=50){fprintf(demo_log,"FAIL tail not inside parking\n");exit(76);}
  held++;if(!saved&&!reload){DemoSave("strict-waiting");saved=true;}
 }
 if(!cleared&&FarTailClear(demo_fast)){cleared=true;fprintf(demo_log,"FAST_TAIL_CLEAR %u\n",n);}
 if(stopped&&!cleared){
  if(demo_slow->cur_speed!=0||TrainReservationPassesThroughTile(demo_slow,TileXY(75,row),TRACK_X)){fprintf(demo_log,"FAIL prematurely moving or reserving exit\n");exit(77);}
  if(TileY(demo_fast->tile)==40&&TileX(demo_fast->tile)>=54&&TileX(demo_fast->tile)<=90){if(demo_fast->cur_speed==0){fprintf(demo_log,"FAIL priority stopped\n");exit(78);}pass_held++;}
 }
 if(stopped&&cleared&&demo_slow->cur_speed>0&&!resumed){resumed=true;fprintf(demo_log,"RESUME %u\n",n);}
 if(resumed&&TileY(demo_slow->tile)==40&&TileX(demo_slow->tile)>94&&!demo_slot->IsOccupant(demo_slow->index)){
  if(!edited||!entered||!stopped||held<2||pass_held==0||(mode=="layout-switch"&&!orphan_released)){fprintf(demo_log,"FAIL missing layout phase\n");exit(79);}
  for(auto*s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area&&!s->occupants.empty()){fprintf(demo_log,"FAIL leaked layout resource\n");exit(80);}
  fprintf(demo_log,"STRICT PASS mode=%s edited=1 entered=1 stopped=1 hold_ticks=%u pass_while_stopped=%u tail_clear=1 resumed=1 released=1 reload=%u\n",std::string(mode).c_str(),held,pass_held,reload);fflush(demo_log);_exit_game=true;
 }
 if(n>15000){fprintf(demo_log,"FAIL layout timeout\n");fflush(demo_log);exit(81);}
 fflush(demo_log);
}
