# -*- coding: utf-8 -*-
from pathlib import Path
import os,sys,subprocess,shutil,json,hashlib,re
import argparse
parser=argparse.ArgumentParser(description='Actual external ship voyages, cargo and save/load checks')
parser.add_argument('--build-dir',type=Path,required=True)
parser.add_argument('--baseset',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--modes',default='same,cross,shared,rotated,west,north,south,removed,cargo,no-load,roundtrip,blocked')
parser.add_argument('--newgrf-config',type=Path)
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
src=(root/'src/openttd.cpp').read_text(encoding='utf8').replace('#include "safeguards.h"','#include "'+(root/'regression/automatic_yield/native.h').as_posix()+'"\nvoid SeaTick();\n#include "safeguards.h"',1).replace('void StateGameLoop()\n{','void StateGameLoop()\n{\n SeaTick();',1)
src+='\n'+(root/'regression/automatic_yield/external_ship_driving.inc').read_text(encoding='utf8')
exe=link(out,{'openttd':src})
for name,source in [('lang',build/'lang'),('baseset',args.baseset.resolve())]:shutil.copytree(source,out/name,dirs_exist_ok=True)
shutil.copy2(args.newgrf_config.resolve() if args.newgrf_config else here/'regression.cfg',out/'test.cfg')
(out/'build-identity.json').write_text(json.dumps({'production_executable_sha256':hashlib.sha256((build/'runtime/openttd.exe').read_bytes()).hexdigest()},indent=2),encoding='utf8')
rows=[]
for dist in ['manual','asymmetric']:
 for mode in args.modes.split(','):
  base=out/(dist+'-'+mode);base.mkdir(exist_ok=True)
  for phase in ['new','0','1','2']:
   case=base if phase=='new' else base/('reload-'+phase);case.mkdir(exist_ok=True)
   env={k:v for k,v in os.environ.items() if not k.startswith('PXP_')};env.update(PXP_SEA_MODE=mode,PXP_SEA_DIST=dist,PXP_DEMO_OUTPUT=str(case))
   if args.newgrf_config:env['PXP_SEA_NEWGRF']='1'
   if phase!='new':env['PXP_SEA_RELOAD']='1'
   run_args=['-e'] if phase=='new' else ['-g',str(base/('phase-'+phase+'.sav'))]
   with (case/'console.log').open('w',encoding='utf8') as f:proc=subprocess.run([str(exe),'-c',str(out/'test.cfg'),'-v','null:ticks=140000','-s','null','-m','null',*run_args],cwd=out,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=240,creationflags=subprocess.CREATE_NO_WINDOW)
   log=(case/'run.log').read_text(encoding='utf8');ok=proc.returncode==0 and 'STRICT PASS' in log
   if phase!='new':
    a=re.search('SAVED phase='+phase+r' hash=(\d+)',(base/'run.log').read_text(encoding='utf8'));b=re.search(r'LOADED hash=(\d+)',log);ok=ok and a is not None and b is not None and a[1]==b[1]
   rows.append(dict(mode=mode,distribution=dist,phase=phase,passed=ok,returncode=proc.returncode,result=log.splitlines()[-1]))
   (out/'results.json').write_text(json.dumps(rows,indent=2),encoding='utf8');print(rows[-1],flush=True)
   assert ok,log[-5000:]
