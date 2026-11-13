"""Actual native arrivals, wagon-only service inheritance and recoupling, with reload."""
from pathlib import Path
import argparse,json,os,re,shutil,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir',type=Path,required=True)
p.add_argument('--baseset',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--modes',default='station,split,split-implicit,split-legacy-carrier,split-load,split-both,split-dispatch,split-dispatch-shared,split-dispatch-shared-both,split-dispatch-shared-schedule,recouple,recouple-waypoint,recouple-waypoint-late,recouple-checkpoint,recouple-checkpoint-reversed,recouple-slot,recouple-slot-full')
a=p.parse_args();here=Path(__file__).resolve().parent;r=here.parents[1];b=a.build_dir.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
for folder,source in [('lang',b/'lang'),('baseset',a.baseset.resolve())]:shutil.copytree(source,out/folder,dirs_exist_ok=True)
s=(r/'src/openttd.cpp').read_text(encoding='utf8')
s=s.replace('#include "safeguards.h"',f'#include "{here.as_posix()}/native.h"\nvoid StationServiceDrivingTick();\n#include "safeguards.h"',1)
s=s.replace('void StateGameLoop()\n{','void StateGameLoop()\n{\n StationServiceDrivingTick();',1)
s+=f'\n#include "{here.as_posix()}/native.inc"\n#include "{here.as_posix()}/service_driving.inc"\n'
cpp=out/'driver.cpp';obj=out/'driver.obj';cpp.write_bytes(s.encode('utf8'))
cmd=next(c['command'] for c in json.loads((b/'compile_commands.json').read_text()) if c['file'].replace('\\','/').endswith('/src/openttd.cpp'))
cc=cmd[:cmd.index(' -D')].strip().strip('"');flags=cmd[:cmd.index(' -o ')]
rsp=out/'compile.rsp';rsp.write_text(flags[flags.index(' -D'):]+f' -I"{r.as_posix()}/src" -o "{obj.as_posix()}" -c "{cpp.as_posix()}"',encoding='utf8');subprocess.run([cc,'@'+str(rsp)],cwd=b,check=True)
section=next(s for s in (b/'build.ninja').read_text().split('\n\n') if s.startswith('build runtime/openttd.exe:'))
linked=section.splitlines()[0].split(': ',1)[1].split(' ',1)[1].split(' |')[0].replace('CMakeFiles/openttd_lib.dir/src/openttd.cpp.obj','"'+obj.as_posix()+'"')
rsp=out/'link.rsp';rsp.write_text(linked+' '+re.search(r'^  LINK_LIBRARIES = (.*)$',section,re.M)[1],encoding='utf8');exe=out/'service-driving.exe'
subprocess.run([cc,'-O3','-mwindows','-fstack-protector','@'+str(rsp),'-o',str(exe)],cwd=b,check=True)
(out/'test.cfg').write_text((here/'regression.cfg').read_text(),encoding='utf8');results=[]
for mode in a.modes.split(','):
 case=out/mode;case.mkdir(exist_ok=True)
 for reload in [False,True]:
  env={k:v for k,v in os.environ.items() if not k.startswith('PXP_')};env.update(PXP_SERVICE_MODE=mode,PXP_STRICT='basic',PXP_DEMO_OUTPUT=str(case))
  argv=[str(exe),'-v','null:until_exit','-s','null','-m','null','-x','-X','-Q','-c',str(out/'test.cfg')]
  if reload:
   env['PXP_SERVICE_RELOAD']='1';argv+=['-g',str(case/('service-first.sav' if mode=='station' else 'couple-checkpoint-waiting.sav' if mode.startswith('recouple-checkpoint') else 'service-split.sav'))]
  else:argv+=['-e']
  with (case/('reload-stderr.log' if reload else 'stderr.log')).open('w') as f:run=subprocess.run(argv,cwd=out,env=env,stdout=f,stderr=f,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),timeout=90)
  log=case/('reload.log' if reload else 'run.log');last=log.read_text(encoding='utf8').splitlines()[-1]
  results.append(dict(mode=mode,reload=reload,returncode=run.returncode,result=last));(out/'results.json').write_text(json.dumps(results,indent=2),encoding='utf8');print(results[-1],flush=True)
  assert run.returncode==0 and last.startswith('STRICT PASS'),str(log)
