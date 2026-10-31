namespace {
FILE *demo_log = nullptr;
Train *demo_slow = nullptr, *demo_fast = nullptr;
TraceRestrictSlot *demo_slot = nullptr;
uint demo_tick = 0;
bool demo_started = false, demo_initial = false, demo_waiting = false, demo_passed = false, demo_released = false;

void DemoRequire(CommandCost result, const char *operation)
{
    if (result.Failed()) {
        fprintf(demo_log, "ERROR %s: %s\n", operation, GetString(result.GetErrorMessage()).c_str());
        fflush(demo_log);
        std::exit(2);
    }
}
void DemoRail(uint x, uint y, Track track)
{
    DemoRequire(CmdBuildSingleRail(DoCommandFlag::Execute, TileXY(x,y), RAILTYPE_RAIL, track, BuildRailTrackFlags::None), "rail");
}
void DemoSign(uint x, uint y, const std::string &name)
{
    DemoRequire(CmdPlaceSign(DoCommandFlag::Execute, TileXY(x,y), name), "sign");
}
void DemoSignal(uint x, uint y, std::optional<AutoYieldRole> role = std::nullopt)
{
    TileIndex tile = TileXY(x,y);
    DemoRequire(CmdBuildSingleSignal(DoCommandFlag::Execute, tile, TRACK_X, SignalType::Path, SignalVariant::Electric,
        0, 1, BuildSignalFlags::None, {}, 0, SignalAlongTrackdir(TRACKDIR_X_SW)), "signal");
    if (!role) return;
    auto *program = GetTraceRestrictProgram(MakeTraceRestrictRefId(tile,TRACK_X), true);
    TraceRestrictInstructionItem item{};
    item.SetType(*role == AutoYieldRole::Clear ? TRIT_AUTO_YIELD_CLEAR : TRIT_AUTO_YIELD_AREA);
    item.SetValue(demo_slot->index);
    if (*role != AutoYieldRole::Clear) item.SetAuxField(to_underlying(*role));
    program->items = {item.AsProgramItem()};
    if (*role != AutoYieldRole::Clear) program->items.emplace_back(AUTO_YIELD_DEFAULT_CONFIG);
    DemoRequire(program->Validate(), "yield program");
    TraceRestrictCheckRefreshSignals(program, 0, TRPAUF_NONE);
}
Train *DemoTrain(EngineID engine, const std::string &name, StationID station, TileIndex depot = TileXY(5,40))
{
    auto result = CmdBuildVehicle(DoCommandFlag::Execute, depot, engine, false, INVALID_CARGO, ClientID::Invalid);
    DemoRequire(result, "locomotive");
    Train *train = Train::Get(result.GetResultDataWithType().GetOrDefault<VehicleID>(VehicleID::Invalid()));
    for (uint i = 0; i < (std::getenv("PXP_GRF_EXACT") ? 0u : 3u); i++) {
        auto wagon = CmdBuildVehicle(DoCommandFlag::Execute, depot, EngineID{27}, false, INVALID_CARGO, ClientID::Invalid);
        DemoRequire(wagon, "wagon");
        auto id = wagon.GetResultDataWithType().GetOrDefault<VehicleID>(VehicleID::Invalid());
        DemoRequire(CmdMoveRailVehicle(DoCommandFlag::Execute, id, train->Last()->index, MoveRailVehicleFlags::None), "attach wagon");
    }
    DemoRequire(CmdRenameVehicle(DoCommandFlag::Execute, train->index, name), "rename train");
    Order destination;
    destination.MakeGoToStation(station);
    destination.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
    destination.SetLoadType(OrderLoadType::NoLoad);
    DemoRequire(CmdInsertOrder(DoCommandFlag::Execute, InsertOrderCmdData(train->index, VehicleOrderID{0}, destination)), "station order");
    Order finish;
    const TileIndex finish_tile = Map::SizeX()>446 && IsRailDepotTile(TileXY(446,40)) ? TileXY(446,40) : TileXY(246,40);
    finish.MakeGoToDepot(GetDepotDestinationIndex(finish_tile), {}, ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS, ODATFB_HALT);
    DemoRequire(CmdInsertOrder(DoCommandFlag::Execute, InsertOrderCmdData(train->index, VehicleOrderID{1}, finish)), "depot order");
    fprintf(demo_log, "TRAIN id=%u number=%u speed=%u length=%u engine=%u\n",train->index.base(),train->unitnumber,train->GetDisplayMaxSpeed(),train->gcache.cached_total_length,engine.base());
    return train;
}
/* Actual native plans are also valid evidence after the reroute fixture adds
 * its second outlet. This is stricter than comparing exit IDs: include every
 * body part and crossing track, and reject incomplete/unknown plans. */
bool DemoSelectedCorridorsIndependent(const Train *left, const Train *right)
{
    YapfRailRoute a,b;
    const auto route=[&](const Train *v,YapfRailRoute &out){
        const auto *front=v->GetMovingFront();
        if(AutoYieldGetNativeRoute(v,demo_slot->index.base(),front->tile,front->GetVehicleTrackdir(),out))return true;
        /* This fixture started as a single-outlet area. Its already parked
         * train legitimately has no stored multi-outlet plan yet. Query the
         * native continuation from its actual owned endpoint without reserving
         * anything, and prepend only that continuous, unchanged PBS prefix. */
        const auto end=FollowTrainReservation(v,nullptr,FollowTrainReservationFlag::OkayUnused);
        std::vector<std::pair<TileIndex,Trackdir>> prefix;
        if(!GetTrainReservationPathTo(v,end.tile,end.trackdir,256,prefix))return false;
        for(size_t i=1;i<prefix.size();i++)if(DistanceManhattan(prefix[i-1].first,prefix[i].first)!=1)return false;
        auto future=YapfTrainPlanRoute(v,end.tile,end.trackdir,256);if(!future.complete)return false;
        std::vector<YapfRailRouteStep> joined;
        for(size_t i=0;i+1<prefix.size();i++)joined.push_back({prefix[i].first,prefix[i].second});
        joined.insert(joined.end(),future.path.begin(),future.path.end());future.path=std::move(joined);out=std::move(future);return true;
    };
    if(!route(left,a)||!route(right,b))return false;
    std::map<TileIndex,TrackBits> tracks;
    for(const auto &step:a.path)tracks[step.tile]|=TrackToTrackBits(TrackdirToTrack(step.trackdir));
    for(const Train *part=left->First();part;part=part->Next())tracks[part->tile]|=part->track;
    const auto overlap=[&](TileIndex tile,TrackBits bits){const auto i=tracks.find(tile);return i!=tracks.end()&&((i->second&bits)!=TRACK_BIT_NONE||TracksOverlap(i->second|bits));};
    for(const auto &step:b.path)if(overlap(step.tile,TrackToTrackBits(TrackdirToTrack(step.trackdir))))return false;
    for(const Train *part=right->First();part;part=part->Next())if(overlap(part->tile,part->track))return false;
    return true;
}
void DemoSave(const char *name)
{
    AutoRestoreBackup pause(_pause_mode, _pause_mode);
    _pause_mode.Set(PauseMode::Normal);
    ScrollMainWindowToTile(TileXY(65,41), true);
    const char *output = std::getenv("PXP_DEMO_OUTPUT");
    std::string path = std::string(output != nullptr ? output : ".") + "/" + name + ".sav";
    const auto result = SaveOrLoad(path, SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::None, false);
    fprintf(demo_log,"SAVE %s result=%u tick=%u slow=(%u,%u) fast=(%u,%u)\n",name,to_underlying(result),demo_tick,TileX(demo_slow->tile),TileY(demo_slow->tile),TileX(demo_fast->tile),TileY(demo_fast->tile));
    fflush(demo_log);
    if (result != SaveLoadResult::Ok) std::exit(3);
}
void DemoBuild()
{
    demo_log = fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/run.log").c_str(),"w");
    _current_company = OWNER_DEITY;
    DemoRequire(CmdFoundTown(DoCommandFlag::Execute, TileXY(100,15), TownSize::Small, false, TownLayout::Original, false, 0, "待避演示镇"), "town");
    _game_mode = GameMode::Normal;
    Company *company = DoStartupNewCompany(DSNC_NONE);
    company->name = "自动待避教学铁路";
    company->money = 10000000;
    company->current_loan = 0;
    _current_company = _local_company;
    SetLocalCompany(company->index);
    _current_company = company->index;
    _settings_game.vehicle.automatic_yield = true;
    _settings_game.vehicle.never_expire_vehicles = true;
    _settings_game.difficulty.vehicle_breakdowns = static_cast<VehicleBreakdowns>(0);
    _pause_mode = {};
    for (uint x=6;x<=245;x++) DemoRail(x,40,TRACK_X);
    DemoRail(50,40,TRACK_RIGHT); DemoRail(50,41,TRACK_Y); DemoRail(50,42,TRACK_LEFT);
    for(uint x=51;x<80;x++) DemoRail(x,42,TRACK_X);
    DemoRail(80,42,TRACK_UPPER); DemoRail(80,41,TRACK_Y); DemoRail(80,40,TRACK_LOWER);
    if (std::getenv("PXP_DEMO_PLATFORM_CHOICE")) {
        DemoRail(50,40,TRACK_UPPER); DemoRail(50,39,TRACK_Y); DemoRail(50,38,TRACK_LOWER);
        for(uint x=51;x<80;x++) DemoRail(x,38,TRACK_X);
        DemoRail(80,38,TRACK_RIGHT); DemoRail(80,39,TRACK_Y); DemoRail(80,40,TRACK_LEFT);
    }
    DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(5,40),RAILTYPE_RAIL,DiagDirection::SW),"origin depot");
    DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(246,40),RAILTYPE_RAIL,DiagDirection::NE),"destination depot");
    DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(235,40),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"destination station");
    if (std::getenv("PXP_DEMO_PLATFORM")) DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(62,42),RAILTYPE_RAIL,Axis::X,1,5,StationClassID{0},0,StationID::Invalid(),false),"siding platform");
    StationID station = GetStationIndex(TileXY(235,40));
    DemoRequire(CmdRenameStation(DoCommandFlag::Execute,station,false,"终点站：演示结束后回库"),"station name");
    demo_slot = TraceRestrictSlot::Create(company->index);
    demo_slot->name = "示例路签：正线与待避线共用";
    demo_slot->max_occupancy = 16;
    DemoSignal(16,40,std::getenv("PXP_LATE_APPROACH") ? std::nullopt : std::optional{AutoYieldRole::Approach});
    for(uint x:{25u,35u,45u}) DemoSignal(x,40);
    if (std::getenv("PXP_DEMO_MULTI_APPROACH")) DemoSignal(30,40,AutoYieldRole::Approach);
    DemoSignal(54,40,AutoYieldRole::Main);
    const bool exit_only = std::getenv("PXP_DEMO_EXIT_ONLY") != nullptr;
    if (!exit_only) DemoSignal(54,42,AutoYieldRole::Siding);
    DemoSignal(74,42,AutoYieldRole::Exit);
    if (const char *choice = std::getenv("PXP_DEMO_PLATFORM_CHOICE")) {
        DemoSignal(74,38,AutoYieldRole::Exit);
        DemoRequire(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,TileXY(74,42),TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,AUTO_YIELD_DEFAULT_CONFIG|AUTO_YIELD_PLATFORM,""),"platform lane setting");
        if (const char *layout=std::getenv("PXP_DEMO_LAYOUT")) {
            if (std::string_view(layout)=="ordinary") DemoRequire(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,TileXY(74,42),TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,AUTO_YIELD_DEFAULT_CONFIG,""),"ordinary layout");
            if (std::string_view(layout)=="platform") {
                DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(62,38),RAILTYPE_RAIL,Axis::X,1,5,StationClassID{0},0,StationID::Invalid(),false),"upper platform");
                DemoRequire(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,TileXY(74,38),TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,AUTO_YIELD_DEFAULT_CONFIG|AUTO_YIELD_PLATFORM,""),"all platform layout");
            }
        }
        if (std::getenv("PXP_DEMO_PLATFORM_PENALTY")) {
            auto *program=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(74,42),TRACK_X),true);
            TraceRestrictInstructionItem penalty{};penalty.SetType(TRIT_PF_PENALTY);penalty.SetValue(60000);
            program->items.insert(program->items.begin(),penalty.AsProgramItem());program->items.push_back(penalty.AsProgramItem());
            DemoRequire(program->Validate(),"high platform penalty");TraceRestrictCheckRefreshSignals(program,2,TRPAUF_NONE);
        }
        if (std::string_view(choice)=="fallback") {
            auto *program=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(74,38),TRACK_X),true);
            TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);program->items.push_back(deny.AsProgramItem());
            DemoRequire(program->Validate(),"ordinary lane restriction");TraceRestrictCheckRefreshSignals(program,2,TRPAUF_NONE);
        }
    }

    if (const char *back = std::getenv("PXP_DEMO_BACK_SIGNAL")) {
        DemoSignal(76,42);
        SetPresentSignals(TileXY(76,42),SignalAlongTrackdir(TRACKDIR_X_NE));
        SetSignalType(TileXY(76,42),TRACK_X,std::string_view(back)=="oneway" ? SignalType::PathOneWay : SignalType::Path);
        if (std::string_view(back)=="deny") {
            auto *program=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(76,42),TRACK_X),true);
            TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);program->items={deny.AsProgramItem()};
            DemoRequire(program->Validate(),"back signal restriction");
            TraceRestrictCheckRefreshSignals(program,0,TRPAUF_NONE);
        }
    }
    DemoSignal(ParseInteger<uint>(std::getenv("PXP_CLEAR") ? std::getenv("PXP_CLEAR") : "90").value_or(90),40,AutoYieldRole::Clear);
    DemoSignal(230,40);
    if(const char *m=std::getenv("PXP_STRICT");m && std::string_view(m)=="different") {
        DemoRail(65,40,TRACK_UPPER);DemoRail(65,39,TRACK_Y);DemoRail(65,38,TRACK_LOWER);
        for(uint x=66;x<=110;x++)DemoRail(x,38,TRACK_X);
        DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(111,38),RAILTYPE_RAIL,DiagDirection::NE),"upper destination depot");
        DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(105,38),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"upper destination");
        DemoSignal(80,38,AutoYieldRole::Clear);SetSignalType(TileXY(80,38),TRACK_X,SignalType::PathOneWay);SetSignalType(TileXY(230,40),TRACK_X,SignalType::PathOneWay);
    }
    AutoYieldSettingsChanged();
    DemoSign(15,36,"① 接近点：自动领取共用路签");
    DemoSign(52,37,"② 正线：快车优先通过");
    DemoSign(56,45,exit_only ? "待避线：入口不设信号，不需配置指令" : "③ 待避入口：慢车进入此线");
    DemoSign(75,46,exit_only ? "③ 待避出口：兼任股道标记" : "④ 待避出口：等待快车出清");
    DemoSign(92,36,exit_only ? "④ 车尾出清点：自动退出路签" : "⑤ 车尾出清点：自动退出路签");
    DemoSign(62,30,"自动待避演示：按暂停键继续，观察两辆列车");
    EngineID slow = EngineID::Invalid(), fast = EngineID::Invalid();
    const bool exact_grf = std::getenv("PXP_GRF_EXACT") != nullptr;
    if(exact_grf)fprintf(demo_log,"GRF_CATALOG configured=%zu active=%zu\n",_grfconfig_newgame.size(),_grfconfig.size());
    if(exact_grf&&std::getenv("PXP_GRF_CATALOG"))for(const auto&g:_grfconfig){
        fprintf(demo_log,"GRF file=%s status=%u\n",g->filename.c_str(),to_underlying(g->status));
        for(const auto&i:g->param_info)if(i)fprintf(demo_log,"GRF_PARAM nr=%u bit=%u bits=%u default=%u min=%u max=%u\n",i->param_nr,i->first_bit,i->num_bit,i->def_value,i->min_value,i->max_value);
        for(const auto&e:g->errors)fprintf(demo_log,"GRF_ERROR %s %s\n",e.custom_message.c_str(),e.data.c_str());
    }
    if (exact_grf) DemoRequire(CmdConvertRail(DoCommandFlag::Execute, TileXY(246,126), TileXY(5,20), RAILTYPE_ELECTRIC, false), "electrify real NewGRF fixture");
    for (const Engine *e : Engine::Iterate()) {
        if(exact_grf&&std::getenv("PXP_GRF_CATALOG")&&e->type==VehicleType::Train&&e->IsEnabled())fprintf(demo_log,"ENGINE id=%u design=%u power=%u type=%u rails=%llu\n",e->index.base(),e->GetDisplayMaxSpeed(),RailVehInfo(e->index)->power,to_underlying(RailVehInfo(e->index)->railveh_type),(unsigned long long)RailVehInfo(e->index)->railtypes.base());
        if (e->type != VehicleType::Train || !e->IsEnabled() || RailVehInfo(e->index)->power == 0 || !(RailVehInfo(e->index)->railtypes.Test(RAILTYPE_RAIL) || (exact_grf && RailVehInfo(e->index)->railtypes.Test(RAILTYPE_ELECTRIC)))) continue;
        if (exact_grf && RailVehInfo(e->index)->railveh_type != RailVehicleType::Singlehead && e->GetDisplayMaxSpeed()!=250) continue;
        uint speed=e->GetDisplayMaxSpeed();
        const std::string_view exact_mode=std::getenv("PXP_STRICT");
        const uint exact_slow=exact_mode=="grf-160-250"?160:100;
        if (exact_grf ? speed==exact_slow && slow==EngineID::Invalid() : speed>=95 && speed<=115 && (std::getenv("PXP_SERVICE_MODE") == nullptr || RailVehInfo(e->index)->railveh_type == RailVehicleType::Singlehead)) slow=e->index;
        const uint exact_fast = exact_mode=="grf-200"?200:exact_mode=="grf-160-250"?250:160;
        if (exact_grf ? speed==exact_fast && fast==EngineID::Invalid() : speed>=190 && speed<=210) fast=e->index;
    }
    if (slow == EngineID::Invalid() || fast == EngineID::Invalid()) { fprintf(demo_log,"ERROR engines\n"); std::exit(4); }
    demo_slow=DemoTrain(slow,"1号慢车：进入待避线让行",station);
    if(const char *m=std::getenv("PXP_STRICT"); m && std::string_view(m).starts_with("crossing")) {
        for(uint y=20;y<126;y++) DemoRail(85,y,TRACK_Y);
        DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(85,126),RAILTYPE_RAIL,DiagDirection::NW),"crossing source");
        DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(85,20),RAILTYPE_RAIL,Axis::Y,1,6,StationClassID{0},0,StationID::Invalid(),false),"crossing destination");
        for(auto [y,role]:{std::pair{120u,AutoYieldRole::Approach},std::pair{50u,AutoYieldRole::Main},std::pair{35u,AutoYieldRole::Clear}}) {
            TileIndex tile=TileXY(85,y);
            DemoRequire(CmdBuildSingleSignal(DoCommandFlag::Execute,tile,TRACK_Y,SignalType::PathOneWay,SignalVariant::Electric,0,1,BuildSignalFlags::None,{},0,SignalAlongTrackdir(TRACKDIR_Y_NW)),"crossing signal");
            auto *program=GetTraceRestrictProgram(MakeTraceRestrictRefId(tile,TRACK_Y),true);
            TraceRestrictInstructionItem item{};item.SetType(role==AutoYieldRole::Clear?TRIT_AUTO_YIELD_CLEAR:TRIT_AUTO_YIELD_AREA);item.SetValue(demo_slot->index);if(role!=AutoYieldRole::Clear)item.SetAuxField(to_underlying(role));
            program->items={item.AsProgramItem()};if(role!=AutoYieldRole::Clear)program->items.emplace_back(AUTO_YIELD_DEFAULT_CONFIG);
            DemoRequire(program->Validate(),"crossing program");TraceRestrictCheckRefreshSignals(program,0,TRPAUF_NONE);
        }
        if(std::string_view(m)=="crossing-foreign") {
            auto *foreign=TraceRestrictSlot::Create(company->index);foreign->name="Independent detector region";foreign->max_occupancy=16;
            for(uint y:{120u,50u,35u}) {
                auto *program=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(85,y),TRACK_Y),false);
                auto old=program->actions_used_flags;
                TraceRestrictInstructionItem item{program->items[0].base()};item.SetValue(foreign->index);program->items[0]=item.AsProgramItem();
                DemoRequire(program->Validate(),"foreign marker");TraceRestrictCheckRefreshSignals(program,0,old);
            }
            for(uint y=48;y<=52;y++)DemoRail(87,y,TRACK_Y);
            TileIndex tile=TileXY(87,50);
            DemoRequire(CmdBuildSingleSignal(DoCommandFlag::Execute,tile,TRACK_Y,SignalType::PathOneWay,SignalVariant::Electric,0,1,BuildSignalFlags::None,{},0,SignalAlongTrackdir(TRACKDIR_Y_NW)),"unused foreign siding");
            auto *program=GetTraceRestrictProgram(MakeTraceRestrictRefId(tile,TRACK_Y),true);
            TraceRestrictInstructionItem item{};item.SetType(TRIT_AUTO_YIELD_AREA);item.SetAuxField(to_underlying(AutoYieldRole::Exit));item.SetValue(foreign->index);
            program->items={item.AsProgramItem()};program->items.emplace_back(AUTO_YIELD_DEFAULT_CONFIG);DemoRequire(program->Validate(),"foreign siding marker");TraceRestrictCheckRefreshSignals(program,0,TRPAUF_NONE);
        }
        demo_fast=DemoTrain(fast,"crossing express",GetStationIndex(TileXY(85,20)),TileXY(85,126));
        AutoYieldSettingsChanged();
    } else demo_fast=DemoTrain(fast,"2号快车：正线优先通过",station);
    if (const char *choice=std::getenv("PXP_DEMO_PLATFORM_CHOICE"); choice && std::string_view(choice)=="stop") {
        Order stop;stop.MakeGoToStation(GetStationIndex(TileXY(62,42)));stop.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);stop.SetLoadType(OrderLoadType::NoLoad);
        DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_slow->index,VehicleOrderID{0},stop)),"scheduled platform stop");
        DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,demo_slow->index,VehicleOrderID{0}),"activate station order");
    }
    DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_slow->index,true),"start slow");
    ScrollMainWindowToTile(TileXY(65,41),true);
    DoZoomInOutWindow(static_cast<ZoomStateChange>(1), GetMainWindow());
    fprintf(demo_log,"READY slot=%u\n",demo_slot->index.base()); fflush(demo_log);
}
} // namespace
void EmergencyStrictTick();
void CoordinationTick();
void StreamTick();
void StationRouteTick();
void BuildFarDestination();
void LayoutTick();
void StockFullTick();
void TopologyTick();
void OccupiedTick();
void MixedTick();
void AutoYieldDemoTick() {
 if(const char *mode=std::getenv("PXP_STRICT");mode && std::string_view(mode).starts_with("mixed-")){if(!HasModalProgress()&&!_generating_world)MixedTick();return;}
 if(HasModalProgress() || _generating_world)return;
 if(const char *mode=std::getenv("PXP_STRICT");mode && std::string_view(mode).starts_with("occupied-")){OccupiedTick();return;}
 if(const char *mode=std::getenv("PXP_STRICT");mode && std::string_view(mode).starts_with("topology-")){TopologyTick();return;}
 if(const char *mode=std::getenv("PXP_STRICT");mode && std::string_view(mode).starts_with("stock-full-")){StockFullTick();return;}
 if(const char *mode=std::getenv("PXP_STRICT");mode && std::string_view(mode).starts_with("layout-")){LayoutTick();return;}
 if(const char *mode=std::getenv("PXP_STRICT");mode && std::string_view(mode).starts_with("station-")){StationRouteTick();return;}
 if(const char *mode=std::getenv("PXP_STRICT");mode && std::string_view(mode).starts_with("stream")){StreamTick();return;}
 if(const char *mode=std::getenv("PXP_STRICT");mode && std::string_view(mode).starts_with("turnover")){CoordinationTick();return;}
 if(!demo_started && _game_mode==GameMode::Editor){demo_started=true;if(std::string_view(std::getenv("PXP_STRICT")).starts_with("far-"))BuildFarDestination();else DemoBuild();}
 if(_game_mode==GameMode::Normal)EmergencyStrictTick();
}
#include "coordination.inc"
#include "stream.inc"
#include "station.inc"
#include "far.inc"
#include "layout.inc"
#include "stock_full.inc"
#include "topology.inc"
#include "occupied.inc"
#include "mixed.inc"

void EmergencyStrictTick() {
 static uint n=0, stopped_ticks=0, pass_stopped_ticks=0, second_pass_stopped_ticks=0, third_pass_stopped_ticks=0, express_min=UINT_MAX;
 static bool conflict=false;
 static bool entered=false, stopped=false, passed=false, saved=false, unknown=false, cancelled=false;
 static bool rerouted=false;
 static bool registered_slow=false,registered_fast=false,main_pass=false;
 static Train *second=nullptr;
 static Train *third=nullptr;
 static Train *normal_middle=nullptr;
 const std::string_view raw_mode=std::getenv("PXP_STRICT");
 const bool distant_scenario=raw_mode.starts_with("far-");
 const bool consecutive=raw_mode=="grf-consecutive";
 const std::string_view mode=distant_scenario?(raw_mode=="far-two"?"two":"basic"):raw_mode.starts_with("grf-")?((raw_mode=="grf-two"||consecutive)?"two":"basic"):raw_mode;
 const bool reload=std::getenv("PXP_STRICT_RELOAD");
 if(n==0){
  if(reload){
   demo_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/reload-strict.log").c_str(),"w");
   for(auto*v:Train::Iterate())if(v->IsPrimaryVehicle()){if(v->unitnumber==1)demo_slow=v;if(v->unitnumber==2)demo_fast=v;if(v->unitnumber==3)second=v;if(v->unitnumber==4)third=v;}
  for(auto*s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area==0)demo_slot=s;
   if(!demo_slow||!demo_fast||!demo_slot)exit(31);
   _pause_mode={};
  }
  if(mode=="different" && !reload){Order o;o.MakeGoToStation(GetStationIndex(TileXY(105,38)));o.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_fast->index,VehicleOrderID{0},o)),"different destination");DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,demo_fast->index,VehicleOrderID{0}),"activate different destination");}
  if(mode=="two" && !reload){
   EngineID engine=demo_fast->engine_type;
   if(consecutive){
    engine=EngineID::Invalid();
    for(const Engine*e:Engine::Iterate())if(engine==EngineID::Invalid()&&e->type==VehicleType::Train&&e->IsEnabled()&&RailVehInfo(e->index)->power&&RailVehInfo(e->index)->railtypes.Test(RAILTYPE_ELECTRIC)&&e->GetDisplayMaxSpeed()==200)engine=e->index;
    if(engine==EngineID::Invalid())exit(111);
   }
   second=distant_scenario?far_second:DemoTrain(engine,"second express",GetStationIndex(TileXY(235,40)));
   if(consecutive){
    engine=EngineID::Invalid();
    for(const Engine*e:Engine::Iterate())if(engine==EngineID::Invalid()&&e->type==VehicleType::Train&&e->IsEnabled()&&RailVehInfo(e->index)->power&&RailVehInfo(e->index)->railtypes.Test(RAILTYPE_ELECTRIC)&&e->GetDisplayMaxSpeed()==250)engine=e->index;
    if(engine==EngineID::Invalid())exit(112);
    third=DemoTrain(engine,"third express",GetStationIndex(TileXY(235,40)));
   }
  }
  if(mode=="normal-free-first"&&!reload){
   EngineID engine=EngineID::Invalid();for(const Engine*e:Engine::Iterate())if(e->type==VehicleType::Train&&e->IsEnabled()&&RailVehInfo(e->index)->power&&RailVehInfo(e->index)->railtypes.Test(RAILTYPE_RAIL)&&e->GetDisplayMaxSpeed()==64)engine=e->index;
   if(engine==EngineID::Invalid())exit(110);
   normal_middle=DemoTrain(engine,"middle arrival must not cancel free normal yielding",GetStationIndex(TileXY(235,40)));
  }
  if(mode=="front"||mode=="tail")DemoRequire(CmdProgramSignalTraceRestrict(DoCommandFlag::Execute,TileXY(16,40),TRACK_X,TRDCT_MODIFY_DUAL_ITEM,0,AUTO_YIELD_DEFAULT_CONFIG|((mode=="front"?1u:2u)<<26),""),"detector mode");
  if(raw_mode.starts_with("grf-")){
   const auto exact=[&](const Train*train,int speed){
    if(train==nullptr||train->GetDisplayMaxSpeed()!=speed||train->gcache.cached_power==0){fprintf(demo_log,"FAIL real vehicle design speed or power after construction/load\n");exit(116);}
    fprintf(demo_log,"EXACT reload=%u number=%u design=%u power=%u\n",reload,train->unitnumber,train->GetDisplayMaxSpeed(),train->gcache.cached_power);
   };
   exact(demo_slow,raw_mode=="grf-160-250"?160:100);
   exact(demo_fast,raw_mode=="grf-200"?200:raw_mode=="grf-160-250"?250:160);
   if(mode=="two")exact(second,consecutive?200:160);
   if(consecutive)exact(third,250);
  }
 }
 n++;demo_tick=n;
 if(raw_mode=="normal-free-first"&&std::getenv("PXP_STRICT_DEBUG"))_debug_levels[size_t(DebugLevelID::yapf)]=n>=900&&n<1700?7:0;
 if(mode=="reroute"&&std::getenv("PXP_STRICT_DEBUG"))_debug_levels[size_t(DebugLevelID::yapf)]=7;
 if(distant_scenario&&n==1&&std::getenv("PXP_STRICT_DEBUG"))_debug_levels[size_t(DebugLevelID::yapf)]=7;
 if(distant_scenario&&n%256==0)fprintf(demo_log,"FAR TRACE %u slow=%u,%u/%u target=%u fast=%u,%u/%u member=%u need=%u free=%u lane=%u\n",n,TileX(demo_slow->tile),TileY(demo_slow->tile),demo_slow->cur_speed,demo_slow->current_order.GetDestination().base(),TileX(demo_fast->tile),TileY(demo_fast->tile),demo_fast->cur_speed,demo_slot->IsOccupant(demo_fast->index),EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::NeedsSiding),EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::FreeSiding),StreamHasTarget(demo_fast));
 if(mode.starts_with("crossing") && n%64==0 && n<2200) {
  fprintf(demo_log,"TRACE %u slow=%u,%u speed=%u fast=%u,%u speed=%u member=%u need=%u free=%u\n",n,TileX(demo_slow->tile),TileY(demo_slow->tile),demo_slow->cur_speed,TileX(demo_fast->tile),TileY(demo_fast->tile),demo_fast->cur_speed,demo_slot->IsOccupant(demo_fast->index),EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::NeedsSiding),EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::FreeSiding));
  for(auto*s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area && !s->occupants.empty())fprintf(demo_log,"SLOT %u flags=%u count=%zu slow=%u fast=%u\n",s->index.base(),s->flags.base(),s->occupants.size(),s->IsOccupant(demo_slow->index),s->IsOccupant(demo_fast->index));
 }
 if(!reload&&mode=="normal-free-first"&&n==200)DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,normal_middle->index,true),"start middle normal arrival");
 /* The faster real NewGRF engines accelerate more quickly than the stock
  * fixture. Start them behind the same branch decision, late enough that the
  * slow train can finish braking before the express passes its held exit. */
 const bool exact_grf=std::getenv("PXP_GRF_EXACT")!=nullptr;
 const bool realistic_braking=_settings_game.vehicle.train_braking_model==TBM_REALISTIC;
 if(!reload && n==((distant_scenario||mode=="normal-free-first")?400u:mode=="tail"?680u:raw_mode=="grf-160-250"?(realistic_braking?500u:300u):exact_grf?(realistic_braking?850u:700u):700u))DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,demo_fast->index,true),"start express");
 if(second&&((!distant_scenario&&!reload&&n==(exact_grf?1000u:850u))||(distant_scenario&&second->vehstatus.Test(VehState::Stopped)&&TileY(demo_fast->tile)==40&&TileX(demo_fast->tile)>=54)))DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,second->index,true),"start second express");
 if(third&&!reload&&n==1150)DemoRequire(CmdStartStopVehicle(DoCommandFlag::Execute,third->index,true),"start third express");
 if(exact_grf&&demo_fast&&TileY(demo_fast->tile)==40&&TileX(demo_fast->tile)>=45&&TileX(demo_fast->tile)<=90)express_min=std::min<uint>(express_min,demo_fast->cur_speed);
 if(mode=="red" || mode=="unknown" || mode=="delete" || mode=="cancel" || mode=="reroute") {
  if((!reload && n==900) || n==(reload?400u:2000u)){
   auto*p=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(35,40),TRACK_X),true);
   auto old=p->actions_used_flags;p->items.clear();
   if(!reload && n==900){TraceRestrictInstructionItem w{};w.SetType(TRIT_WAIT_AT_PBS);p->items.push_back(w.AsProgramItem());}
   DemoRequire(p->Validate(),"red/green approach");TraceRestrictCheckRefreshSignals(p,0,old);
   if(demo_fast){FreeTrainTrackReservation(demo_fast);TryPathReserve(demo_fast,true,false);}
  }
 }
 bool member_fast=demo_fast && demo_slot->IsOccupant(demo_fast->index);
 if(mode=="crossing-foreign" && demo_fast)for(auto *s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area==0 && s!=demo_slot && s->IsOccupant(demo_fast->index))member_fast=true;
 if(!conflict && !reload && !entered && EvaluateAutoYield(demo_slow,demo_slot->index.base(),AutoYieldCondition::NeedsSiding)){conflict=true;fprintf(demo_log,"CONFLICT %u\n",n);}
 registered_slow|=demo_slot->IsOccupant(demo_slow->index);registered_fast|=member_fast;
 if(!entered && TileY(demo_slow->tile)==42){entered=true;fprintf(demo_log,"ENTER %u\n",n);}
 unsigned targets=0;
 for(auto*s:TraceRestrictSlot::Iterate())if(s->flags.Test(TraceRestrictSlot::Flag::AutoYieldTargets))targets+=s->occupants.size();
 if(mode=="unknown" && entered && !unknown && targets>0){
  // Make the forecast incomplete without changing the already chosen branch.
  DemoSignal(85,40);
  auto*p=GetTraceRestrictProgram(MakeTraceRestrictRefId(TileXY(85,40),TRACK_X),true);
  TraceRestrictInstructionItem deny{};deny.SetType(TRIT_PF_DENY);p->items={deny.AsProgramItem()};
  DemoRequire(p->Validate(),"unknown forecast");TraceRestrictCheckRefreshSignals(p,0,TRPAUF_NONE);
  unknown=true;fprintf(demo_log,"UNKNOWN_WITH_TARGETS %u count=%u\n",n,targets);
 }
 if(entered && demo_slow->cur_speed==0 && TileX(demo_slow->tile)>=72 && TileY(demo_slow->tile)==42){
  if(!stopped){fprintf(demo_log,"STOP %u targets=%u\n",n,targets);stopped=true;}
  stopped_ticks++;
  if(!saved && demo_fast && !reload&&(!distant_scenario||!second||StreamHasTarget(second))){DemoSave("strict-waiting");saved=true;}
 }
 if(stopped && !cancelled && (mode=="delete" || mode=="cancel") && stopped_ticks>=64){
  if(mode=="delete"){FreeTrainTrackReservation(demo_fast);delete demo_fast;demo_fast=nullptr;}
  else demo_slot->Vacate(demo_fast);
  cancelled=true;member_fast=false;fprintf(demo_log,"CANCEL %u mode=%s\n",n,std::string(mode).c_str());
 }
 if(mode=="reroute" && stopped_ticks>=64 && !rerouted){
  DemoRail(65,40,TRACK_UPPER);DemoRail(65,39,TRACK_Y);DemoRail(65,38,TRACK_LOWER);
  for(uint x=66;x<=110;x++)DemoRail(x,38,TRACK_X);
  DemoRequire(CmdBuildTrainDepot(DoCommandFlag::Execute,TileXY(111,38),RAILTYPE_RAIL,DiagDirection::NE),"reroute depot");
  DemoRequire(CmdBuildRailStation(DoCommandFlag::Execute,TileXY(105,38),RAILTYPE_RAIL,Axis::X,1,6,StationClassID{0},0,StationID::Invalid(),false),"reroute station");
  DemoSignal(80,38,AutoYieldRole::Clear);SetSignalType(TileXY(80,38),TRACK_X,SignalType::PathOneWay);
  SetSignalType(TileXY(230,40),TRACK_X,SignalType::PathOneWay);AutoYieldSettingsChanged();

  Order o;o.MakeGoToStation(GetStationIndex(TileXY(105,38)));o.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
  DemoRequire(CmdInsertOrder(DoCommandFlag::Execute,InsertOrderCmdData(demo_fast->index,VehicleOrderID{0},o)),"reroute destination");
  DemoRequire(CmdSkipToOrder(DoCommandFlag::Execute,demo_fast->index,VehicleOrderID{0}),"activate reroute");
  FreeTrainTrackReservation(demo_fast);TryPathReserve(demo_fast,true,false);rerouted=true;fprintf(demo_log,"REROUTE %u\n",n);
 }
 const bool selected_independent=mode=="reroute"&&rerouted&&DemoSelectedCorridorsIndependent(demo_slow,demo_fast);
 if(mode=="reroute" && rerouted && (selected_independent || TileY(demo_fast->tile)==38 || TrainReservationPassesThroughTile(demo_fast,TileXY(65,40),TRACK_UPPER))) {
  TraceRestrictProgramInput input(TileXY(74,42),TRACKDIR_X_SW,nullptr,nullptr);
  TraceRestrictProgramResult result;
  GetExistingTraceRestrictProgram(input.tile,TRACK_X)->Execute(demo_slow,input,result);
  /* Verify the real committed alternative and the actual controlled exit,
   * rather than treating a conservative query from behind it as proof that
   * the express still needs the old, conflicting route. */
  if(!(result.flags&TRPRF_WAIT_AT_PBS)){if(!cancelled&&selected_independent)fprintf(demo_log,"REROUTE_SELECTED_INDEPENDENT %u native_plans_and_bodies=disjoint\n",n);cancelled=true;}
 }
 if(mode.starts_with("crossing") && demo_fast && TileY(demo_fast->tile)>=38 && TileY(demo_fast->tile)<=42) {
  main_pass=true;if(demo_slow->cur_speed==0 && TileY(demo_slow->tile)==42)pass_stopped_ticks++;
 }
 if(!mode.starts_with("crossing") && mode!="different" && mode!="reroute" && demo_fast && TileX(demo_fast->tile)>=74 && TileX(demo_fast->tile)<81&&(!distant_scenario||TileY(demo_fast->tile)==40)){
  if(TileY(demo_fast->tile)!=40){fprintf(demo_log,"FAIL express not on main\n");exit(32);}
  main_pass=true;if(demo_slow->cur_speed==0 && TileY(demo_slow->tile)==42)pass_stopped_ticks++;
 }
 if(second && TileX(second->tile)>=74 && TileX(second->tile)<81&&(!distant_scenario||TileY(second->tile)==40)){
  if(TileY(second->tile)!=40){fprintf(demo_log,"FAIL second express not on main\n");exit(39);}
  if(demo_slow->cur_speed==0 && TileY(demo_slow->tile)==42)second_pass_stopped_ticks++;
 }
 if(third && TileX(third->tile)>=74 && TileX(third->tile)<81){
  if(TileY(third->tile)!=40){fprintf(demo_log,"FAIL third express not on main\n");exit(113);}
  if(demo_slow->cur_speed==0 && TileY(demo_slow->tile)==42)third_pass_stopped_ticks++;
 }
 const bool second_pending=second && (distant_scenario?!FarTailClear(second):(TileX(second->tile)<=90 || demo_slot->IsOccupant(second->index)));
 const bool third_pending=third && (TileX(third->tile)<=90 || demo_slot->IsOccupant(third->index));
 bool crossing_tail_clear=mode.starts_with("crossing") && demo_fast && TileY(demo_fast->tile)<40;
 if(crossing_tail_clear)for(const Train *part=demo_fast;part;part=part->Next())if(part->tile==TileXY(85,40))crossing_tail_clear=false;
 /* Different outlets: the exact crossing tail, not the unrelated area's
  * downstream Clear marker, is the release event for this slow train. */
 const bool complete=cancelled || (mode.starts_with("crossing") ? crossing_tail_clear : (demo_fast && (distant_scenario?FarTailClear(demo_fast):(TileX(demo_fast->tile)>90 && !member_fast)) && !second_pending && !third_pending));
 if(complete && !passed){passed=true;fprintf(demo_log,"CLEAR %u\n",n);}
 static bool far_resumed=false;if(distant_scenario&&stopped&&passed&&demo_slow->cur_speed>0&&!far_resumed){far_resumed=true;fprintf(demo_log,"RESUME %u\n",n);}
 if(stopped && !passed && demo_slow->cur_speed!=0){fprintf(demo_log,"FAIL moving while express pending %u xy=%u,%u front=%u,%u td=%u speed=%u member=%u order=%u fast=%u,%u target=%u\n",n,TileX(demo_slow->tile),TileY(demo_slow->tile),TileX(demo_slow->GetMovingFront()->tile),TileY(demo_slow->GetMovingFront()->tile),demo_slow->GetVehicleTrackdir(),demo_slow->cur_speed,demo_slot->IsOccupant(demo_slow->index),demo_slow->current_order.GetType(),TileX(demo_fast->tile),TileY(demo_fast->tile),demo_fast->current_order.GetDestination().base());exit(33);}
 if(mode!="different" && entered && !passed && TileY(demo_slow->tile)==40 && TileX(demo_slow->tile)>=80){fprintf(demo_log,"FAIL left siding before clearance %u\n",n);exit(34);}
 if(stopped && !passed && TrainReservationPassesThroughTile(demo_slow,TileXY(75,42),TRACK_X)){fprintf(demo_log,"FAIL reservation beyond held exit\n");exit(35);}
 if(passed && TileX(demo_slow->tile)>94 && TileY(demo_slow->tile)==40 && !demo_slot->IsOccupant(demo_slow->index)){
  if(mode=="different" ? (!entered||stopped) : (!entered||!stopped||stopped_ticks<2||(!cancelled&&(!main_pass||pass_stopped_ticks==0))||!registered_slow||(!reload&&!registered_fast))){fprintf(demo_log,"FAIL missing stage\n");exit(36);}
  if((mode=="two" && second_pass_stopped_ticks==0) || (!reload && mode!="different" && !conflict) || (mode=="unknown" && !unknown)){fprintf(demo_log,"FAIL missing conflict or second pass\n");exit(40);}
  if(consecutive&&third_pass_stopped_ticks==0){fprintf(demo_log,"FAIL missing third pass\n");exit(114);}
  if(exact_grf&&mode=="basic"&&express_min==0){fprintf(demo_log,"FAIL isolated express stopped on main\n");exit(115);}
  for(auto*s:TraceRestrictSlot::Iterate())if(s->automatic_yield_area && !s->occupants.empty()){fprintf(demo_log,"FAIL leaked internal slot\n");exit(37);}
  if(distant_scenario&&!far_resumed){fprintf(demo_log,"FAIL missing far resume\n");exit(41);}
  fprintf(demo_log,"STRICT PASS mode=%s entered=%u stopped=%u hold_ticks=%u main_pass=%u released=1 slots_released=1 reload=%u conflict=%u pass_while_stopped=%u second_pass_while_stopped=%u third_pass_while_stopped=%u express_min=%u\n",std::string(distant_scenario?raw_mode:mode).c_str(),entered,stopped,stopped_ticks,main_pass,reload,conflict,pass_stopped_ticks,second_pass_stopped_ticks,third_pass_stopped_ticks,express_min);fflush(demo_log);_exit_game=true;
 }
 if(n>15000){fprintf(demo_log,"FAIL timeout\n");fflush(demo_log);exit(38);}
 fflush(demo_log);
}
