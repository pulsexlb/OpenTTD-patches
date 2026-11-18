# -*- coding: utf-8 -*-
from pathlib import Path
import os,sys,subprocess,shutil,json,hashlib,re
import argparse
parser=argparse.ArgumentParser(description='Native four-mode shared-city transport, CargoDist, accounting and save/load checks')
parser.add_argument('--build-dir',type=Path,required=True)
parser.add_argument('--baseset',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--modes',default='direct,shortage,transfer,freight,roundtrip')
parser.add_argument('--distributions',default='manual,asymmetric,symmetric')
parser.add_argument('--phases',default='new,0,1,2,concurrent')
args=parser.parse_args()
here=Path(__file__).resolve().parent;root=here.parents[1];build=args.build_dir.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
def link(out,sources):
 commands=json.loads((build/'compile_commands.json').read_text());objects={}
 for name,source in sources.items():
  cmd=next(x['command'] for x in commands if x['file'].replace('\\','/').endswith('/src/'+name+'.cpp'))
  cc=cmd[:cmd.index(' -D')].strip().strip('"');flags=cmd[:cmd.index(' -o ')]
  cpp=out/(name+'.cpp');obj=out/(name+'.obj');cpp.write_bytes(source.encode('utf8'))
  rsp=out/(name+'.rsp');rsp.write_text(flags[flags.index(' -D'):]+f' -I"{root.as_posix()}/src" -o "{obj.as_posix()}" -c "{cpp.as_posix()}"',encoding='utf8')
  subprocess.run([cc,'@'+str(rsp)],cwd=build,check=True);objects['CMakeFiles/openttd_lib.dir/src/'+name+'.cpp.obj']='"'+obj.as_posix()+'"'
 section=next(x for x in (build/'build.ninja').read_text().split('\n\n') if x.startswith('build runtime/openttd.exe:'))
 linked=section.splitlines()[0].split(': ',1)[1].split(' ',1)[1].split(' |')[0]
 for old,new in objects.items():linked=linked.replace(old,new)
 rsp=out/'link.rsp';rsp.write_text(linked+' '+re.search(r'^  LINK_LIBRARIES = (.*)$',section,re.M)[1],encoding='utf8')
 exe=out/'diagnostic.exe';subprocess.run([cc,'-O3','-mwindows','-fstack-protector','@'+str(rsp),'-o',str(exe)],cwd=build,check=True);return exe
src=(root/'src/openttd.cpp').read_text(encoding='utf8').replace('#include "safeguards.h"','#include "'+(here/'native.h').as_posix()+'"\nvoid JointTick();\n#include "safeguards.h"',1).replace('void StateGameLoop()\n{','void StateGameLoop()\n{\n JointTick();',1)
src+='\n'+(here/'external_joint_driving.inc').read_text(encoding='utf8')
g=(root/'src/external_city.cpp').read_text(encoding='utf8').replace('void ServiceExternalCityVehicle(Vehicle *train, bool arrival)','static void ServiceExternalCityVehicleNative(Vehicle *train, bool arrival)',1)
g+='\nvoid JointMarketBefore(Vehicle*,bool);void JointMarketAfter(Vehicle*,bool);\nvoid ServiceExternalCityVehicle(Vehicle*v,bool arrival){JointMarketBefore(v,arrival);ServiceExternalCityVehicleNative(v,arrival);JointMarketAfter(v,arrival); }\n'
e=(root/'src/economy.cpp').read_text(encoding='utf8').replace('#include "safeguards.h"','void JointDelivery(VehicleID,const CargoPacket*,uint,Money,bool);\n#include "safeguards.h"',1)
e=e.replace('this->visual_profit += profit;','this->visual_profit += profit; JointDelivery(this->front->index,cp,count,profit,false);',1)
e=e.replace('this->visual_transfer += profit;','this->visual_transfer += profit; JointDelivery(this->front->index,cp,count,profit,true);',1)
exe=link(out,{'openttd':src,'external_city':g,'economy':e})
for name,source in [('lang',build/'lang'),('baseset',args.baseset.resolve())]:shutil.copytree(source,out/name,dirs_exist_ok=True)
shutil.copy2(here/'regression.cfg',out/'test.cfg')
(out/'build-identity.json').write_text(json.dumps({'production_executable_sha256':hashlib.sha256((build/'runtime/openttd.exe').read_bytes()).hexdigest()},indent=2),encoding='utf8')
rows=[]
for dist in args.distributions.split(','):
 for mode in args.modes.split(','):
  if mode=='freight' and dist=='symmetric':continue # Freight UI offers asymmetric routing; already covered above.
  base=out/(dist+'-'+mode);base.mkdir(exist_ok=True)
  for phase in args.phases.split(','):
   case=base if phase=='new' else base/('reload-'+phase);case.mkdir(exist_ok=True)
   env={k:v for k,v in os.environ.items() if not k.startswith('PXP_')};env.update(PXP_JOINT_MODE=mode,PXP_JOINT_DIST=dist,PXP_JOINT_OUTPUT=str(case))
   if phase!='new':env['PXP_JOINT_RELOAD']='1'
   save=base/(phase+'.sav' if phase in ('ready','concurrent') else 'phase-'+phase+'.sav')
   run_args=['-e'] if phase=='new' else ['-g',str(save)]
   with (case/'console.log').open('w',encoding='utf8') as f:proc=subprocess.run([str(exe),'-c',str(out/'test.cfg'),'-v','null:until_exit','-s','null','-m','null',*run_args],cwd=out,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=180,creationflags=subprocess.CREATE_NO_WINDOW)
   log=(case/'run.log').read_text(encoding='utf8') if (case/'run.log').exists() else '';ok=proc.returncode==0 and 'STRICT PASS' in log
   if phase!='new' and ok:
    key=phase if phase in ('ready','concurrent') else 'phase-'+phase
    before=re.search('SAVED '+key+r' hash=(\d+)',(base/'run.log').read_text())[1];after=re.search(r'LOADED hash=(\d+)',log)[1];ok=before==after
   rows.append(dict(mode=mode,distribution=dist,phase=phase,passed=ok,returncode=proc.returncode,result=next((line for line in log.splitlines() if line.startswith('STRICT PASS')),log.splitlines()[-1] if log else 'No event log')))
   (out/'results.json').write_text(json.dumps(rows,indent=2),encoding='utf8');print(rows[-1],flush=True)
   assert ok,log[-5000:]
