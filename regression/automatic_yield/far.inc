#include "landscape_cmd.h"
namespace {
Train *far_second=nullptr;
bool FarTailClear(const Train*v)
{
 if(demo_slot->IsOccupant(v->index))return false;
 for(const Train*part=v;part;part=part->Next())if(TileY(part->tile)!=40||TileX(part->tile)<=90)return false;
 return true;
}
}
void BuildFarDestination()
{
 DemoBuild();
 const EngineID fast_engine=demo_fast->engine_type;delete demo_fast;demo_fast=nullptr;
 for(uint x=40;x<220;x++)DemoRail(x,60,TRACK_X);
 DemoRail(40,60,TRACK_LEFT);for(uint y=41;y<60;y++)DemoRail(40,y,TRACK_Y);DemoRail(40,40,TRACK_LOWER);
 DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(220,60),RAILTYPE_RAIL,DiagDirection::NE),"distant feeder depot");
 DemoRequire(CmdBuildSingleSignal(DoCommandFlag::Execute,TileXY(210,60),TRACK_X,SignalType::PathOneWay,SignalVariant::Electric,0,1,BuildSignalFlags::None,{},0,SignalAlongTrackdir(TRACKDIR_X_NE)),"distant detector signal");
 auto*p=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(210,60),TRACK_X),true);
 TraceRestrictInstructionItem item{};item.SetType(TRIT_AUTO_YIELD_AREA);item.SetAuxField(to_underlying(AutoYieldRole::Approach));item.SetValue(demo_slot->index);
 p->items={item.AsProgramItem(),TraceRestrictProgramItem{AUTO_YIELD_DEFAULT_CONFIG}};DemoRequire(p->Validate(),"distant detector");TraceRestrictCheckRefreshSignals(p,0,TRPAUF_NONE);
 /* End the far feeder's native reservation before the shared entrance.
  * Otherwise its already legal long PBS path blocks siding access itself. */
 DemoRequire(CmdBuildSingleSignal(DoCommandFlag::Execute,TileXY(50,60),TRACK_X,SignalType::PathOneWay,SignalVariant::Electric,0,1,BuildSignalFlags::None,{},0,SignalAlongTrackdir(TRACKDIR_X_NE)),"feeder safe end");
 DemoRequire(CmdBuildSingleSignal(DoCommandFlag::Execute,TileXY(40,45),TRACK_Y,SignalType::PathOneWay,SignalVariant::Electric,0,1,BuildSignalFlags::None,{},0,SignalAlongTrackdir(TRACKDIR_Y_NW)),"feeder merge protection");
 demo_fast=DemoTrain(fast_engine,"distant express",GetStationIndex(TileXY(235,40)),TileXY(220,60));
 if(std::string_view(std::getenv("PXP_STRICT"))=="far-two")far_second=DemoTrain(fast_engine,"late distant express",GetStationIndex(TileXY(235,40)),TileXY(220,60));
 std::vector<Train*>trains{demo_slow,demo_fast};if(far_second)trains.push_back(far_second);
 for(Train*v:trains)while(v->GetNumOrders()>0)DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),0),"replace short fixture orders");
 DemoRequire(CmdLandscapeClear(DoCommandFlag::Execute,TileXY(246,40)),"remove empty short destination depot");
 for(uint x=246;x<=445;x++)DemoRail(x,40,TRACK_X);
 for(uint x:{350u,400u})DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(x,40),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"distant timetable station");
 DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(446,40),RAILTYPE_RAIL,DiagDirection::NE),"distant destination depot");
 for(Train*v:trains){
  for(uint i=0;i<2;i++){Order order;order.MakeGoToStation(GetStationIndex(TileXY(i?400:350,40)));order.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);order.SetLoadType(OrderLoadType::NoLoad);DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(v->index,static_cast<VehicleOrderID>(i),order)),"long physical order");}
  Order finish;finish.MakeGoToDepot(GetDepotDestinationIndex(TileXY(446,40)),{},ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS,ODATFB_HALT);DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(v->index,VehicleOrderID{2},finish)),"long finish order");
  if(v->cur_real_order_index!=0)DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,v->index,0),"activate distant destination");
 }
 AutoYieldSettingsChanged();fprintf(demo_log,"FAR READY width=%u target=350 next=400 slow=%u fast=%u\n",Map::SizeX(),demo_slow->GetDisplayMaxSpeed(),demo_fast->GetDisplayMaxSpeed());
 if(std::string_view(std::getenv("PXP_STRICT"))=="far-multi-outlet" || std::string_view(std::getenv("PXP_STRICT"))=="far-near-wrong-outlet"){
  // A different legal local outlet does not serve this train's timetable.
  // Its existence makes a global unique-outlet proof unavailable. The free
  // siding still rejoins the main and serves the real, distant station.
  DemoRail(76,42,TRACK_RIGHT);DemoRail(76,43,TRACK_Y);DemoRail(76,44,TRACK_LEFT);
  for(uint x=77;x<100;x++)DemoRail(x,44,TRACK_X);
  // The closer wrong outlet must not hide the farther valid first outlet.
  // Both are first clearance boundaries of separate forward branches.
  DemoSignal(std::string_view(std::getenv("PXP_STRICT"))=="far-near-wrong-outlet"?78:90,44,AutoYieldRole::Clear);
  if(std::string_view(std::getenv("PXP_STRICT"))=="far-near-wrong-outlet")DemoRequire(CmdBuildSingleSignal(DoCommandFlag::Execute,TileXY(78,44),TRACK_X,SignalType::PathOneWay,SignalVariant::Electric,0,1,BuildSignalFlags::Convert,{},0,SignalAlongTrackdir(TRACKDIR_X_SW)),"no return from wrong outlet");
  DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(100,44),RAILTYPE_RAIL,DiagDirection::NE),"independent downstream outlet");
  AutoYieldSettingsChanged();
 }
 if(std::string_view(std::getenv("PXP_STRICT"))=="far-empty-first"){
  // The first marked lane is unreachable. The second remains a legal, empty
  // siding. A large unrelated network beyond Clear must not starve its check.
  DemoRail(50,40,TRACK_UPPER);DemoRail(50,38,TRACK_LOWER);
  for(uint x=51;x<80;x++)DemoRail(x,38,TRACK_X);
  DemoRail(80,38,TRACK_RIGHT);DemoRail(80,39,TRACK_Y);DemoRail(80,40,TRACK_LEFT);
  if(!std::getenv("PXP_DEMO_EXIT_ONLY"))DemoSignal(54,38,AutoYieldRole::Siding);
  DemoSignal(74,38,AutoYieldRole::Exit);
  DemoRequire(CmdLandscapeClear(DoCommandFlag::Execute,TileXY(446,40)),"extend unused downstream network");
  for(uint x=446;x<1600;x++)DemoRail(x,40,TRACK_X);
  DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(1600,40),RAILTYPE_RAIL,DiagDirection::NE),"far downstream terminal");
  AutoYieldSettingsChanged();fprintf(demo_log,"EMPTY_FIRST unreachable_upper=1 free_lower=1 downstream=1600\n");
 }
}
