"""Run real off-map trains, timetable/dispatch stops and exact cargo/money checks.

Compiles observation-only overrides against an existing Windows Ninja build.
The private diagnostic executable must never replace the production binary.
"""
from pathlib import Path
import argparse,json,os,re,subprocess,shutil,hashlib
args=argparse.ArgumentParser(description=__doc__)
args.add_argument('--build-dir',type=Path,required=True)
args.add_argument('--baseset',type=Path,required=True)
args.add_argument('--output',type=Path,required=True)
args.add_argument("--distribution",choices=["manual","symmetric","asymmetric"],default="manual")
args.add_argument("--policy",choices=["normal","no-load","no-unload","cargo","full"],default="normal")
args.add_argument('--newgrf-config',type=Path)
args.add_argument('--industrial-cargo',action='store_true')
args.add_argument('--cycles',type=int,default=1)
a=args.parse_args()
here=Path(__file__).resolve().parent;root=here.parents[1];build=a.build_dir.resolve()
p=out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)

def link(out,sources):
 out=Path(out);out.mkdir(parents=True,exist_ok=True)
 commands=json.loads((build/'compile_commands.json').read_text()); objects={}
 for name,source in sources.items():
  c=next(x['command'] for x in commands if x['file'].replace('\\','/').endswith('/src/'+name+'.cpp'))
  cc=c[:c.index(' -D')].strip().strip('"'); flags=c[:c.index(' -o ')]
  cpp=out/(name+'.cpp');obj=out/(name+'.obj');cpp.parent.mkdir(parents=True,exist_ok=True);cpp.write_bytes(source.encode('utf8'))
  rsp=out/(name+'.rsp');rsp.write_text(flags[flags.index(' -D'):]+f' -I"{(root/"src"/name).parent.as_posix()}"'+f' -I"{root.as_posix()}/src" -o "{obj.as_posix()}" -c "{cpp.as_posix()}"',encoding='utf8')
  subprocess.run([cc,'@'+str(rsp)],cwd=build,check=True);objects['CMakeFiles/openttd_lib.dir/src/'+name+'.cpp.obj']='"'+obj.as_posix()+'"'
 section=next(s for s in (build/'build.ninja').read_text().split('\n\n') if s.startswith('build runtime/openttd.exe:'))
 linked=section.splitlines()[0].split(': ',1)[1].split(' ',1)[1].split(' |')[0]
 for old,new in objects.items():linked=linked.replace(old,new)
 rsp=out/'link.rsp';rsp.write_text(linked+' '+re.search(r'^  LINK_LIBRARIES = (.*)$',section,re.M)[1],encoding='utf8')
 exe=out/'diagnostic.exe';subprocess.run([cc,'-O3','-mwindows','-fstack-protector','@'+str(rsp),'-o',str(exe)],cwd=build,check=True)
 return exe

s=(root/'src/openttd.cpp').read_text(encoding='utf8')
s=s.replace('#include "safeguards.h"',f'#include "{here.as_posix()}/native.h"\nvoid ExternalConnectionDrivingTick();\n#include "safeguards.h"',1)
s=s.replace('void StateGameLoop()\n{','void StateGameLoop()\n{\n ExternalConnectionDrivingTick();',1)
s+=f'\n#include "{here.as_posix()}/native.inc"\n'
s+='\n'+(here/'external_city_driving.inc').read_text(encoding='utf8')
if a.industrial_cargo:
 # Buy real native coal wagons; do not rewrite cargo capacities or types.
 native=(here/'native.inc').read_text(encoding='utf8').replace('EngineID{27}', 'EngineID{29}')
 native=re.sub(r'#include "([^"/]+[.]inc)"',lambda m: '#include "'+here.as_posix()+'/'+m[1]+'"',native)
 s=s.replace(f'#include "{here.as_posix()}/native.inc"',native,1)
 s=s.replace('  city_b=ExternalCityID{1};',r"""
  city_b=ExternalCityID{1};
  const CargoType coal=GetCargoTypeByLabel(CT_COAL);assert(coal<NUM_CARGO);
  for(const auto id:{city_a,city_b}){
   auto*city=ExternalCity::Get(id);city->industries_initialized=false;bool matched=false;
   for(uint32_t seed=1;seed<10000;++seed){city->economy_seed=seed;const auto profile=GetExternalCityEconomy(id);if(id==city_a?profile.produced.Test(coal):profile.accepted.Test(coal)){matched=true;break;}}
   assert(matched);city->stock={};city->demand={};city->market_initialized=false;city->statistics_initialized=false;InitializeExternalCityMarket(city);
  }
  uint active=0;for(const auto&g:_grfconfig)if(g->status==GRFStatus::Activated)++active;
  assert(active>0);fprintf(demo_log,"INDUSTRY_GRF active=%u cargo=%u source_seed=%u destination_seed=%u\n",active,uint(coal),ExternalCity::Get(city_a)->economy_seed,ExternalCity::Get(city_b)->economy_seed);
""",1)

s=s.replace('void ExternalConnectionDrivingTick();','void ExternalConnectionDrivingTick();\nvoid ProbeExternalPhases();',1)
s=s.replace('\n ExternalConnectionDrivingTick();','\n ExternalConnectionDrivingTick();\n if(_game_mode==GameMode::Normal)ProbeExternalPhases();',1)
s=s.replace('  CitySave("city-ready");setup=true;',r'''
  if(std::getenv("PXP_EXTERNAL_DISPATCH")){
   auto &schedules=demo_slow->orders->GetScheduledDispatchScheduleSet();schedules.emplace_back();
   schedules[0].SetScheduledDispatchDuration(2000);schedules[0].SetScheduledDispatchStartTick(_state_ticks);schedules[0].AddScheduledDispatch(500,0,0);
   DemoRequire(CmdTimetableSeparation(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),false),"disable timetable separation for actual dispatch");
   DemoRequire(CmdSchDispatchSetEnabled(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),true),"enable off-map dispatch");
   DemoRequire(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{0},MTF_ASSIGN_SCHEDULE,0,{}),"assign actual city stop to schedule");
  }
  CitySave("city-ready");setup=true;''',1)
if a.cycles>1:
 s=s.replace('  CitySave("city-ready");setup=true;', '  DemoRequire(CmdDeleteOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),VehicleOrderID{4}),"cyclic service without final depot halt");\n  CitySave("city-ready");setup=true;',1)
 s=s.replace('if(++ticks>20000)','if(++ticks>90000)',1)
 needle='if(second_done&&demo_slow->cur_real_order_index==3&&demo_slow->current_order.IsAnyLoadingType()&&IsRailStationTile(demo_slow->GetMovingFront()->tile)){'
 s=s.replace(needle,needle+f'\n static uint completed=0;if(++completed<{a.cycles}){{first_done=second_done=false;return;}}',1)
s=s.replace('  assert(demo_slow->external_trip_city==snapshot&&demo_slow->external_return_tick==due', '  if(std::getenv("PXP_EXTERNAL_DISPATCH")&&demo_slow->external_arrival_handled)due=demo_slow->external_return_tick;\n  assert(demo_slow->external_trip_city==snapshot&&demo_slow->external_return_tick==due',1)
s+='\n#include "schdispatch.h"\n'+r'''
static uint64_t MarketFingerprint(){
 uint64_t hash=1469598103934665603ULL;auto mix=[&](uint64_t x){hash^=x;hash*=1099511628211ULL;};
 for(const auto*c:ExternalCity::Iterate()){mix(c->index.base());mix(c->economy_seed);mix(c->distance_tiles);mix(c->stop_minutes);mix(c->delivered_units);mix(c->loaded_units);for(auto x:c->industry_types)mix(x);for(auto x:c->industry_inputs)mix(x);for(auto x:c->industry_remainders)mix(x);for(auto x:c->industry_month_consumed)mix(x);for(auto x:c->industry_month_produced)mix(x);for(uint i=0;i<NUM_CARGO;++i){mix(c->stock[CargoType(i)]);mix(c->demand[CargoType(i)]);mix(c->month_supply[CargoType(i)]);mix(c->month_demand[CargoType(i)]);mix(c->month_loaded[CargoType(i)]);mix(c->month_delivered[CargoType(i)]);}}
 for(const auto*c:Company::Iterate()){mix(c->index.base());mix(int64_t(c->money));}
 for(const Train*t:Train::Iterate()){mix(t->index.base());mix(t->cargo.StoredCount());for(auto*cp:*t->cargo.Packets()){mix(cp->Count());mix(cp->external_source_city);mix(cp->external_distance);mix(int64_t(cp->GetFeederShare()));}}
 return hash;
}
void ProbeExternalPhases(){
 static bool saved[3]={};static bool loaded=false;static FILE*f=nullptr;
 const std::string directory=std::getenv("PXP_DEMO_OUTPUT");
 if(!f){f=fopen((directory+"/phases-"+(std::getenv("PXP_PHASE_LABEL")?std::string(std::getenv("PXP_PHASE_LABEL")):"new")+".log").c_str(),"w");assert(f);}
 if(!loaded&&std::getenv("PXP_PHASE_LABEL")){fprintf(f,"LOADED fingerprint=%llu\n",(unsigned long long)MarketFingerprint());fflush(f);loaded=true;}
 if(std::getenv("PXP_EXTERNAL_RELOAD"))return;
 Train*v=nullptr;for(Train*t:Train::Iterate())if(t->IsPrimaryVehicle()&&t->unitnumber==1)v=t;
 if(!v||!v->IsInExternalTrip())return;
 static bool sale_saved=false;
 if(!sale_saved&&v->external_trip_city==ExternalCityID{1}&&!v->external_arrival_handled&&v->external_arrival_tick.base()-_state_ticks.base()<=64){CitySave("before-real-sale");sale_saved=true;}
 if(v->external_trip_city!=ExternalCityID{0})return;
 const auto phase=v->GetExternalTripPhase();int index=-1;
 if(phase==ExternalTripPhase::Outbound)index=0;
 if(phase==ExternalTripPhase::Stopped&&v->external_arrival_handled)index=1;
 if(phase==ExternalTripPhase::Returning&&v->external_departure_handled)index=2;
 static bool routed_saved=false;
 if(index==1&&!routed_saved){
  const auto*city=ExternalCity::Get(v->external_trip_city);const auto*terminal=Station::GetIfValid(city->market_station);
  if(terminal!=nullptr&&terminal->goods[v->cargo_type].data&&terminal->goods[v->cargo_type].data->cargo.AvailableCount()>terminal->goods[v->cargo_type].data->cargo.AvailableViaCount(StationID::Invalid())){CitySave("routed-stop");routed_saved=true;}
 }
 if(index<0||saved[index])return;saved[index]=true;
 assert(v->GetExternalDisplaySpeed()==(index==1?0:v->external_cruise_speed));
 assert(v->cur_speed==0&&v->IsChainInDepot());
 if(index==0)assert(v->current_order.IsType(OT_GOTO_DEPOT));
 if(index==1){assert(v->current_order.IsType(OT_WAITING)&&v->current_order_time<=2);if(std::getenv("PXP_EXTERNAL_DISPATCH")){assert(!v->dispatch_records.empty());assert(v->external_departure_tick==v->dispatch_records.begin()->second.dispatched);}}
 if(index==2)assert(!v->current_order.IsType(OT_WAITING)&&v->current_order_time<=2);
 const std::string stem="phase-"+std::to_string(index);
 AutoRestoreBackup pause(_pause_mode,_pause_mode);_pause_mode.Set(PauseMode::Normal);
 fprintf(f,"SAVED phase=%d tick=%lld arrival=%lld departure=%lld return=%lld order_time=%u loading_time=%u fingerprint=%llu\n",index,(long long)_state_ticks.base(),(long long)v->external_arrival_tick.base(),(long long)v->external_departure_tick.base(),(long long)v->external_return_tick.base(),v->current_order_time,v->current_loading_time,(unsigned long long)MarketFingerprint());fflush(f);
 assert(SaveOrLoad(directory+"/"+stem+".sav",SaveLoadOperation::Save,DetailedFileType::GameFile,Subdirectory::None,false)==SaveLoadResult::Ok);
}
'''
# Declarations must precede the included native fixture.
s=s.replace('#include "safeguards.h"','#include "schdispatch.h"\n#include "safeguards.h"',1)
g=(root/'src/external_city.cpp').read_text(encoding='utf8')
g=g.replace('#include "stdafx.h"','#include "stdafx.h"\n#include "date_func.h"',1)
needle=' const auto profile = GetExternalCityEconomy(city->index);\n Money income = 0;'
assert needle in g
g=g.replace(needle,r'''
 const auto profile=GetExternalCityEconomy(city->index);
 if(arrival && std::string(std::getenv("PXP_MARKET_POLICY"))=="full"){
  const CargoType cargo=train->cargo_type; auto &ge=station->goods[cargo]; ge.CreateData().cargo.Truncate();
  auto*cp=CargoPacket::Create(uint16_t(17),uint16_t(0),station->index,station->xy,Money(0)); cp->external_source_city=city->index.base();cp->external_distance=city->distance_tiles;
  ge.CreateData().cargo.Append(cp,StationID::Invalid()); city->stock[cargo]=17; city->month_supply[cargo]=city->month_loaded[cargo]+17;
 }
 const auto before_stock=city->stock,before_demand=city->demand;
 const auto before_delivered=city->delivered_units,before_loaded=city->loaded_units;
 const auto before_money=Company::Get(train->owner)->money,before_profit=train->profit_this_year;
 auto total=[&](){uint64_t count=0;for(const Vehicle*t=train->First();t;t=t->Next())count+=t->cargo.StoredCount();return count;};
 const uint64_t before_cargo=total();
 Money income=0;''',1)
g=g.replace(' if (!changed) return;', '')
needle=' if (train->type == VehicleType::Train) Train::From(train)->First()->CargoChanged();'
assert needle in g
g=g.replace(needle,r'''
 uint64_t stock=0,demand=0;
 for(uint i=0;i<NUM_CARGO;++i){const auto c=CargoType(i);assert(city->stock[c]<=before_stock[c]&&city->demand[c]==before_demand[c]);stock+=before_stock[c]-city->stock[c];}
 demand=city->delivered_units-before_delivered;
 if(arrival){assert(stock==0&&demand==before_cargo-total()&&demand==city->delivered_units-before_delivered);}
 else {assert(demand==0&&stock==total()-before_cargo&&stock==city->loaded_units-before_loaded);}
 assert(Company::Get(train->owner)->money==before_money+income);
 assert(train->profit_this_year==before_profit+(income<<8));
 FILE*detail=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/cargo-detail.log").c_str(),"a");assert(detail);for(CargoType c:EnumRange(NUM_CARGO))if(before_stock[c]!=city->stock[c])fprintf(detail,"CARGO id=%u label=%s loaded=%u\n",uint(c),CargoSpec::Get(c)->label.AsString().c_str(),before_stock[c]-city->stock[c]);fclose(detail);
 FILE*report=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/trade.log").c_str(),"a");assert(report);
 fprintf(report,"TRADE arrival=%u city=%u tick=%lld before=%llu after=%llu sold=%llu loaded=%llu income=%lld money_exact=1 packet_metadata=1\n",arrival,city->index.base(),(long long)_state_ticks.base(),(unsigned long long)before_cargo,(unsigned long long)total(),(unsigned long long)demand,(unsigned long long)stock,(long long)int64_t(income));fclose(report);
 if (train->type == VehicleType::Train) Train::From(train)->First()->CargoChanged();''',1)

s=s.replace('  CitySave("city-ready");setup=true;',r"""
  if(const char*dist=std::getenv("PXP_MARKET_DISTRIBUTION")){
   uint mode=std::string(dist)=="symmetric"?2:std::string(dist)=="asymmetric"?1:0;
   for(const char*name:{"linkgraph.distribution_pax","linkgraph.distribution_mail","linkgraph.distribution_default"})DemoRequire(CmdChangeSetting(DoCommandFlag::Execute,name,mode),"set real CargoDist mode");
   DemoRequire(CmdChangeSetting(DoCommandFlag::Execute,"linkgraph.recalc_interval",1),"fast graph refresh");
   DemoRequire(CmdChangeSetting(DoCommandFlag::Execute,"linkgraph.recalc_time",1),"fast graph job");
  }
  const std::string policy=std::getenv("PXP_MARKET_POLICY");
  for(VehicleOrderID index:{VehicleOrderID{0},VehicleOrderID{2}}){
   const auto load=policy=="no-load"?OrderLoadType::NoLoad:policy=="full"?OrderLoadType::FullLoad:policy=="cargo"?OrderLoadType::CargoTypeLoad:OrderLoadType::LoadIfPossible;
   if(load!=OrderLoadType::LoadIfPossible)DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),index,MOF_LOAD,to_underlying(load),INVALID_CARGO,{}),"external loading policy");
   if(policy=="no-unload")DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),index,MOF_UNLOAD,to_underlying(OrderUnloadType::NoUnload),INVALID_CARGO,{}),"external no unloading");
   if(policy=="cargo"){
    DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),index,MOF_CARGO_TYPE_LOAD,to_underlying(OrderLoadType::NoLoad),INVALID_CARGO,{}),"all cargo disabled");
    DemoRequire(CmdModifyOrder(DoCommandFlag::Execute,OrderTargetType::Vehicle,demo_slow->index.base(),index,MOF_CARGO_TYPE_LOAD,to_underlying(OrderLoadType::LoadIfPossible),demo_slow->cargo_type,{}),"selected cargo enabled");
   }
  }
  if(policy=="full")for(auto*city:ExternalCity::Iterate()){
   DemoRequire(CmdManageExternalCity(DoCommandFlag::Execute,DepotID::Invalid(),city->index,ExternalCityOperation::SetStopTime,0,{}),"zero city stop to expose inventory wait");
   auto*st=Station::GetIfValid(city->market_station);if(st==nullptr)continue;
   const CargoType cargo=demo_slow->cargo_type;auto &ge=st->goods[cargo];ge.CreateData().cargo.Truncate();
   auto*cp=CargoPacket::Create(uint16_t(17),uint16_t(0),st->index,st->xy,Money(0));cp->external_source_city=city->index.base();cp->external_distance=city->distance_tiles;
   ge.CreateData().cargo.Append(cp,StationID::Invalid());city->stock[cargo]=17;city->month_supply[cargo]=17;
  }
  CitySave("city-ready");setup=true;""",1)
s=s.replace('  assert(demo_slow->external_trip_city==snapshot&&demo_slow->external_return_tick==due','  if(std::string(std::getenv("PXP_MARKET_POLICY"))=="full"&&demo_slow->external_arrival_handled)due=demo_slow->external_return_tick;\n  assert(demo_slow->external_trip_city==snapshot&&demo_slow->external_return_tick==due',1)

g=g.replace(' if (train->type == VehicleType::Train) Train::From(train)->First()->CargoChanged();',r"""
 if(std::string(std::getenv("PXP_MARKET_POLICY"))=="no-load")assert(city->loaded_units==0);
 if(std::string(std::getenv("PXP_MARKET_POLICY"))=="no-unload")assert(city->delivered_units==0);
 for(Vehicle*part=train;part;part=part->Next())for(const CargoPacket*cp:*part->cargo.Packets())if(cp->external_source_city!=UINT16_MAX){assert(Station::IsValidID(cp->GetFirstStation()));if(!arrival&&cp->external_source_city==city->index.base())assert(cp->GetDistance(train->external_entry_tile)==train->external_distance_tiles);}
 FILE*node_log=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/routing.log").c_str(),"a");assert(node_log);
 fprintf(node_log,"TERMINAL city=%u station=%u stored=%u dist=%u flows=%u\n",city->index.base(),station->index.base(),city->stock[train->cargo_type],uint(_settings_game.linkgraph.GetDistributionType(train->cargo_type)),uint(station->goods[train->cargo_type].ConstFlows().size()));for(const auto&flow:station->goods[train->cargo_type].ConstFlows())for(const auto&share:flow)fprintf(node_log,"FLOW origin=%u via=%u share=%u\n",flow.GetOrigin().base(),share.second.base(),uint(share.first));
 if(LinkGraph::IsValidID(station->goods[train->cargo_type].link_graph)){const auto node=(*LinkGraph::Get(station->goods[train->cargo_type].link_graph))[station->goods[train->cargo_type].node];fprintf(node_log,"NODE supply=%u demand=%u\n",node.Supply(),node.Demand());}
 fclose(node_log);
 if (train->type == VehicleType::Train) Train::From(train)->First()->CargoChanged();""",1)

g=g.replace('   const uint count = ge.data->cargo.Load(', '   const uint routed_before=ge.data->cargo.AvailableCount()-ge.data->cargo.AvailableViaCount(StationID::Invalid());\n   const uint count = ge.data->cargo.Load(',1)
g=g.replace('   if (count == 0) continue;',r'''   const uint routed_after=ge.data->cargo.AvailableCount()-ge.data->cargo.AvailableViaCount(StationID::Invalid());
   if(routed_before>routed_after){FILE*f=fopen((std::string(std::getenv("PXP_DEMO_OUTPUT"))+"/routed-loads.log").c_str(),"a");assert(f);fprintf(f,"ROUTED_LOAD city=%u cargo=%u count=%u tick=%lld\n",city->index.base(),uint(cargo),routed_before-routed_after,(long long)_state_ticks.base());fclose(f);}
   if (count == 0) continue;''',1)
exe=link(out,{'openttd':s,'external_city':g})
(out/'build-identity.json').write_text(json.dumps({'production_executable_sha256':hashlib.sha256((build/'runtime/openttd.exe').read_bytes()).hexdigest()},indent=2),encoding='utf8')
for name,source in [('lang',root/'build-win64/lang'),('baseset',a.baseset.resolve())]:shutil.copytree(source,out/name,dirs_exist_ok=True)
shutil.copy2(here/'regression.cfg',out/'test.cfg')
if a.newgrf_config:
 cfg=(out/'test.cfg').read_text().replace('[newgrf]',a.newgrf_config.read_text(encoding='utf8'));(out/'test.cfg').write_bytes(cfg.encode('utf8'))
results=[]
for mode,dispatch in ([('city-basic',False)] if a.cycles>1 else [('city-basic',False),('city-timed',False),('cross-block-realistic',False),('city-timed',True)]):
 folder=out/(mode+('-dispatch' if dispatch else ''));folder.mkdir(exist_ok=True);(folder/'trade.log').unlink(missing_ok=True)
 for label in ['new','0','1','2']:
  env={k:v for k,v in os.environ.items() if not k.startswith('PXP_')};env.update(PXP_MARKET_DISTRIBUTION=a.distribution,PXP_MARKET_POLICY=a.policy,PXP_EXTERNAL_MODE=mode,PXP_STRICT='basic',PXP_DEMO_OUTPUT=str(folder))
  if dispatch:env['PXP_EXTERNAL_DISPATCH']='1'
  argv=[str(exe),'-v','null:until_exit','-s','null','-m','null','-x','-X','-Q','-c',str(out/'test.cfg')]
  if label=='new':argv+=['-e']
  else:env.update(PXP_PHASE_LABEL=label,PXP_EXTERNAL_RELOAD='first');argv+=['-g',str(folder/('phase-'+label+'.sav'))]
  with (folder/(label+'-stderr.log')).open('w') as stream:x=subprocess.run(argv,cwd=out,env=env,stdout=stream,stderr=stream,timeout=90,creationflags=subprocess.CREATE_NO_WINDOW)
  assert x.returncode==0,(folder,label)
  if label!='new':
   import re
   saved=re.search(r'SAVED phase='+label+r'.*fingerprint=(\d+)',(folder/'phases-new.log').read_text())[1]
   loaded=re.search(r'LOADED fingerprint=(\d+)',(folder/('phases-'+label+'.log')).read_text())[1]
   assert saved==loaded,(folder,label,saved,loaded)
  results.append({'mode':mode,'dispatch':dispatch,'load':label,'returncode':x.returncode});(out/'results.json').write_text(json.dumps(results,indent=2));print(results[-1],flush=True)
 # Two independent cities must actually exchange packets and pay income.
 log=(folder/'trade.log').read_text();assert 'money_exact=1' in log
 if mode=='city-basic' and a.policy not in ('no-load','no-unload'):assert any('arrival=1' in line and not 'sold=0 ' in line and not 'income=0 ' in line for line in log.splitlines())
if a.cycles>1:assert 'ROUTED_LOAD' in (out/'city-basic/routed-loads.log').read_text()
if a.industrial_cargo:
 details=(out/'city-basic/cargo-detail.log').read_text();assert any('label=COAL' in line and not 'delivered=0' in line for line in details.splitlines()),details
if a.distribution!='manual' and a.policy not in ('no-load','no-unload'):
 routing=(out/'city-basic/routing.log').read_text();assert any(int(x)>0 for x in re.findall(r'flows=(\d+)',routing)),routing[-500:]
print('PASS actual_cargo_and_money=1 phases=1 all_phase_reload_fingerprints=1 scheduled_dispatch=1')
