"""Native station departure and isolated pending-turnaround reservation probe, with reload."""
from pathlib import Path
import argparse,json,os,re,shutil,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir',type=Path,required=True)
p.add_argument('--baseset',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--modes',default='reverse,straight,reverse-stuck,straight-stuck')
p.add_argument('--before-fix',action='store_true',help='Remove the two direct-entry guards in the private diagnostic copy; retain the earlier signal-recheck guard')
a=p.parse_args();here=Path(__file__).resolve().parent;r=here.parents[1];b=a.build_dir.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
for folder,source in [('lang',b/'lang'),('baseset',a.baseset.resolve())]:shutil.copytree(source,out/folder,dirs_exist_ok=True)
s=(r/'src/openttd.cpp').read_text(encoding='utf8')
s=s.replace('#include "safeguards.h"',f'#include "{here.as_posix()}/native.h"\nvoid StationReverseDrivingTick();\n#include "safeguards.h"',1)
s=s.replace('void StateGameLoop()\n{','void StateGameLoop()\n{\n StationReverseDrivingTick();',1)
s+=f'\n#include "{here.as_posix()}/native.inc"\n#include "{here.as_posix()}/station_reverse.inc"\n'
cpp=out/'driver.cpp';obj=out/'driver.obj';cpp.write_bytes(s.encode('utf8'))
cmd=next(c['command'] for c in json.loads((b/'compile_commands.json').read_text()) if c['file'].replace('\\','/').endswith('/src/openttd.cpp'))
cc=cmd[:cmd.index(' -D')].strip().strip('"');flags=cmd[:cmd.index(' -o ')]
rsp=out/'compile.rsp';rsp.write_text(flags[flags.index(' -D'):]+f' -I"{r.as_posix()}/src" -o "{obj.as_posix()}" -c "{cpp.as_posix()}"',encoding='utf8');subprocess.run([cc,'@'+str(rsp)],cwd=b,check=True)
# Instrument a private copy: observe every reservation attempt during unresolved departure.
t=(r/'src/train_cmd.cpp').read_text(encoding='utf8')
if a.before_fix:
 t=t.replace('if (v->cur_speed == 0 && (v->current_order.IsType(OT_LEAVESTATION) || v->flags.Test(VehicleRailFlag::Reversing))) return;', '', 1)
 t=t.replace('if (consist->cur_speed == 0 && (consist->current_order.IsType(OT_LEAVESTATION) || consist->flags.Test(VehicleRailFlag::Reversing))) return TPRRF_NONE;', '', 1)
t+='\nvoid ProbePendingStationRecheck(Train *t) { t->flags.Set(VehicleRailFlag::Reversing); RecheckSignalsAndTryReserve(t); t->flags.Reset(VehicleRailFlag::Reversing); }\n'
# Count requests which actually pass each entry's departure guard. This also
# exercises the two direct reservation paths not covered by the old recheck.
for signature, variable in [('static void TryLongReserveChooseTrainTrackFromReservationEnd(Train *v, bool no_reserve_vehicle_tile)', 'v'), ('TryPathReserveResultFlags TryPathReserveWithResultFlags(Train *consist, bool mark_as_stuck, bool first_tile_okay)', 'consist')]:
 start=t.index(signature+'\n{')
 pos=t.index('\n\tClearLookAheadIfInvalid(',start)
 t=t[:pos]+f'\n if ({variable}->unitnumber == 1 && {variable}->cur_speed == 0 && ({variable}->current_order.IsType(OT_LEAVESTATION) || {variable}->flags.Test(VehicleRailFlag::Reversing))) {{ ++_station_departure_guard_bypasses; ++_station_departure_reservation_attempts; }}'+t[pos:]
t='extern unsigned int _station_departure_guard_bypasses, _station_departure_reservation_attempts;\n'+t
t+='\nuint _station_departure_reservation_attempts = 0;\n'
t+='\nuint _station_departure_guard_bypasses = 0;\nvoid ProbePendingStationDirect(Train *t) { Order old = t->current_order; auto flags = t->flags; t->current_order.MakeLeaveStation(); TryPathReserve(t, true, true); TryLongReserveChooseTrainTrackFromReservationEnd(t, true); t->current_order = old; t->flags.Set(VehicleRailFlag::Reversing); TryPathReserve(t, true, true); TryLongReserveChooseTrainTrackFromReservationEnd(t, true); t->flags = flags; }\n'
tcpp=out/'train-observer.cpp';tobj=out/'train-observer.obj';tcpp.write_bytes(t.encode('utf8'))
tcmd=next(c['command'] for c in json.loads((b/'compile_commands.json').read_text()) if c['file'].replace('\\','/').endswith('/src/train_cmd.cpp'))
tflags=tcmd[:tcmd.index(' -o ')];trsp=out/'train-observer.rsp';trsp.write_text(tflags[tflags.index(' -D'):]+f' -I"{r.as_posix()}/src" -o "{tobj.as_posix()}" -c "{tcpp.as_posix()}"',encoding='utf8');subprocess.run([cc,'@'+str(trsp)],cwd=b,check=True)
section=next(s for s in (b/'build.ninja').read_text().split('\n\n') if s.startswith('build runtime/openttd.exe:'))
linked=section.splitlines()[0].split(': ',1)[1].split(' ',1)[1].split(' |')[0].replace('CMakeFiles/openttd_lib.dir/src/openttd.cpp.obj','"'+obj.as_posix()+'"')
linked=linked.replace('CMakeFiles/openttd_lib.dir/src/train_cmd.cpp.obj','"'+tobj.as_posix()+'"')
rsp=out/'link.rsp';rsp.write_text(linked+' '+re.search(r'^  LINK_LIBRARIES = (.*)$',section,re.M)[1],encoding='utf8');exe=out/'service-driving.exe'
subprocess.run([cc,'-O3','-mwindows','-fstack-protector','@'+str(rsp),'-o',str(exe)],cwd=b,check=True)
(out/'test.cfg').write_text((here/'regression.cfg').read_text(),encoding='utf8');results=[]
for mode in a.modes.split(','):
 case=out/mode;case.mkdir(exist_ok=True)
 for reload in [False,True]:
  env={k:v for k,v in os.environ.items() if not k.startswith('PXP_')};env.update(PXP_SERVICE_MODE=mode,PXP_STRICT='basic',PXP_DEMO_OUTPUT=str(case))
  argv=[str(exe),'-v','null:until_exit','-s','null','-m','null','-x','-X','-Q','-c',str(out/'test.cfg')]
  if reload:
   env['PXP_SERVICE_RELOAD']='1';argv+=['-g',str(case/('service-first.sav' if mode=='station' else 'service-split.sav'))]
  else:argv+=['-e']
  with (case/('reload-stderr.log' if reload else 'stderr.log')).open('w') as f:run=subprocess.run(argv,cwd=out,env=env,stdout=f,stderr=f,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),timeout=90)
  log=case/('reload.log' if reload else 'run.log');last=log.read_text(encoding='utf8').splitlines()[-1]
  results.append(dict(mode=mode,reload=reload,returncode=run.returncode,result=last));(out/'results.json').write_text(json.dumps(results,indent=2),encoding='utf8');print(results[-1],flush=True)
  assert run.returncode==0 and last.startswith('STRICT PASS'),str(log)
