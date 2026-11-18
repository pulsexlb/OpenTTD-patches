"""Native map-edge portal journeys, retained vehicles, blocked return and reload."""
from pathlib import Path
import argparse,json,os,re,shutil,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir',type=Path,required=True)
p.add_argument('--baseset',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--modes')
p.add_argument('--cities',action='store_true',help='Run the two-city itinerary and reload each destination')
p.add_argument('--edge-portals',action='store_true',help='Build the boundary portal through the confirmed-conversion command')
a=p.parse_args();here=Path(__file__).resolve().parent;r=here.parents[1];b=a.build_dir.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
for folder,source in [('lang',b/'lang'),('baseset',a.baseset.resolve())]:shutil.copytree(source,out/folder,dirs_exist_ok=True)
s=(r/'src/openttd.cpp').read_text(encoding='utf-8')
s=s.replace('#include "safeguards.h"',f'#include "{here.as_posix()}/native.h"\nvoid ExternalConnectionDrivingTick();\n#include "safeguards.h"',1)
s=s.replace('void StateGameLoop()\n{','void StateGameLoop()\n{\n ExternalConnectionDrivingTick();',1)
s+=f'\n#include "{here.as_posix()}/native.inc"\n'
external=(here/('external_city_driving.inc' if a.cities else 'external_driving.inc')).read_text(encoding='utf-8')
if a.edge_portals:
 external=external.replace('TileXY(246,40)','TileXY(254,40)')
 marker='  _settings_game.vehicle.external_connections=true;'
 construction=r'''
  DemoRequire(Command<Commands::LandscapeClear>::Do(DoCommandFlag::Execute,TileXY(246,40)),"clear old prototype depot");
  for(uint x=246;x<=254;x++)DemoRequire(CmdBuildSingleRail(DoCommandFlag::Execute,TileXY(x,40),RAILTYPE_RAIL,TRACK_X,BuildRailTrackFlags::None),"extend native rail to actual edge");
  const TileIndex endpoint=TileXY(254,40);
  const DiagDirection entrance=GetExternalRailConnectionDirection(endpoint);
  assert(entrance==DiagDirection::NE);
  assert(CmdBuildExternalRailConnection({},endpoint,RAILTYPE_RAIL,entrance).Failed()); // setting still off
  _settings_game.vehicle.external_connections=true;
  assert(CmdBuildExternalRailConnection({},TileXY(253,40),RAILTYPE_RAIL,entrance).Failed());
  assert(CmdBuildExternalRailConnection({},endpoint,RAILTYPE_RAIL,ReverseDiagDir(entrance)).Failed());
  SetTrackReservation(endpoint,TRACK_BIT_X);
  assert(CmdBuildExternalRailConnection({},endpoint,RAILTYPE_RAIL,entrance).Failed());
  SetTrackReservation(endpoint,TRACK_BIT_NONE); // fixture only; no real train owns this synthetic reservation
  DemoRequire(CmdBuildExternalRailConnection({},endpoint,RAILTYPE_RAIL,entrance),"query edge conversion");
  assert(IsPlainRailTile(endpoint));
  DemoRequire(CmdBuildExternalRailConnection(DoCommandFlag::Execute,endpoint,RAILTYPE_RAIL,entrance),"build confirmed edge portal");
  assert(IsRailDepotTile(endpoint)&&GetRailDepotDirection(endpoint)==entrance);
'''
 assert marker in external
 external=external.replace(marker,construction,1)
s+='\n'+external
cpp=out/'driver.cpp';obj=out/'driver.obj';cpp.write_bytes(s.encode('utf-8'))
cmd=next(c['command'] for c in json.loads((b/'compile_commands.json').read_text()) if c['file'].replace('\\','/').endswith('/src/openttd.cpp'))
cc=cmd[:cmd.index(' -D')].strip().strip('"');flags=cmd[:cmd.index(' -o ')]
rsp=out/'compile.rsp';rsp.write_text(flags[flags.index(' -D'):]+f' -I"{r.as_posix()}/src" -o "{obj.as_posix()}" -c "{cpp.as_posix()}"',encoding='utf-8');subprocess.run([cc,'@'+str(rsp)],cwd=b,check=True)
section=next(s for s in (b/'build.ninja').read_text().split('\n\n') if s.startswith('build runtime/openttd.exe:'))
linked=section.splitlines()[0].split(': ',1)[1].split(' ',1)[1].split(' |')[0].replace('CMakeFiles/openttd_lib.dir/src/openttd.cpp.obj','"'+obj.as_posix()+'"')
rsp=out/'link.rsp';rsp.write_text(linked+' '+re.search(r'^  LINK_LIBRARIES = (.*)$',section,re.M)[1],encoding='utf-8');exe=out/'external-driving.exe'
subprocess.run([cc,'-O3','-mwindows','-fstack-protector','@'+str(rsp),'-o',str(exe)],cwd=b,check=True)
(out/'test.cfg').write_text((here/'regression.cfg').read_text(),encoding='utf-8');results=[]
for mode in (a.modes or ('city-basic,city-timed,city-change,city-shared,cross-basic,cross-turn,cross-edit,cross-blocked,cross-delete,cross-reuse,cross-incompatible,city-service,city-service-edit,cross-same-city,cross-fast-realistic,cross-limited,cross-block-free,city-block-free,cross-block-realistic,cross-block-occupied,cross-block-limited,cross-block-next-red' if a.cities else 'basic,custom,disable,blocked,decouple')).split(','):
 case=out/mode;case.mkdir(exist_ok=True)
 for reload in ([False,'first'] if a.cities and mode in ('cross-delete','cross-reuse','cross-incompatible','cross-block-realistic') else [False,'first','second','queued'] if a.cities and mode in ('cross-blocked','cross-block-occupied') else [False,'first','second'] if a.cities else [False,True]):
  env={k:v for k,v in os.environ.items() if not k.startswith('PXP_')};env.update(PXP_EXTERNAL_MODE=mode,PXP_STRICT='basic',PXP_DEMO_OUTPUT=str(case))
  argv=[str(exe),'-v','null:until_exit','-s','null','-m','null','-x','-X','-Q','-c',str(out/'test.cfg')]
  if reload:env['PXP_EXTERNAL_RELOAD']=str(reload) if a.cities else '1';argv+=['-g',str(case/('city-'+reload+'.sav' if a.cities else 'external-inside.sav'))]
  else:argv+=['-e']
  with (case/('reload-stderr.log' if reload else 'stderr.log')).open('w') as f:run=subprocess.run(argv,cwd=out,env=env,stdout=f,stderr=f,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),timeout=90)
  log=case/(('reload-'+str(reload)+'.log' if a.cities else 'reload.log') if reload else 'run.log');last=log.read_text(encoding='utf-8').splitlines()[-1]
  results.append(dict(mode=mode,reload=reload,returncode=run.returncode,result=last));(out/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8');print(results[-1],flush=True)
  assert run.returncode==0 and last.startswith('STRICT PASS'),str(log)
