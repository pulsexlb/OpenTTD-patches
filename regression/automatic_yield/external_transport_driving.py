"""Real buses/trucks and original-industry production; native movement and reload fingerprints.

Private diagnostic executable only. No player save or production executable is modified.
"""
from pathlib import Path
import argparse,json,os,re,shutil,subprocess,hashlib
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir',type=Path,required=True)
p.add_argument('--baseset',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--suite',choices=['road','production'],required=True)
a=p.parse_args();here=Path(__file__).resolve().parent;r=here.parents[1];b=a.build_dir.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
for folder,source in [('lang',b/'lang'),('baseset',a.baseset.resolve())]:shutil.copytree(source,out/folder,dirs_exist_ok=True)
s=(r/'src/openttd.cpp').read_text(encoding='utf8');hook='RoadTick' if a.suite=='road' else 'ChainTick'
s=s.replace('#include "safeguards.h"',f'#include "{here.as_posix()}/native.h"\nvoid {hook}();\n#include "safeguards.h"',1)
s=s.replace('void StateGameLoop()\n{',f'void StateGameLoop()\n{{\n {hook}();',1)
s+='\n'+(here/('external_'+a.suite+'_driving.inc')).read_text(encoding='utf8')
cpp=out/'driver.cpp';obj=out/'driver.obj';cpp.write_bytes(s.encode('utf-8'))
cmd=next(c['command'] for c in json.loads((b/'compile_commands.json').read_text()) if c['file'].replace('\\','/').endswith('/src/openttd.cpp'))
cc=cmd[:cmd.index(' -D')].strip().strip('"');flags=cmd[:cmd.index(' -o ')]
rsp=out/'compile.rsp';rsp.write_text(flags[flags.index(' -D'):]+f' -I"{r.as_posix()}/src" -o "{obj.as_posix()}" -c "{cpp.as_posix()}"',encoding='utf-8');subprocess.run([cc,'@'+str(rsp)],cwd=b,check=True)
section=next(s for s in (b/'build.ninja').read_text().split('\n\n') if s.startswith('build runtime/openttd.exe:'))
linked=section.splitlines()[0].split(': ',1)[1].split(' ',1)[1].split(' |')[0].replace('CMakeFiles/openttd_lib.dir/src/openttd.cpp.obj','"'+obj.as_posix()+'"')
rsp=out/'link.rsp';rsp.write_text(linked+' '+re.search(r'^  LINK_LIBRARIES = (.*)$',section,re.M)[1],encoding='utf-8');exe=out/'external-driving.exe'
subprocess.run([cc,'-O3','-mwindows','-fstack-protector','@'+str(rsp),'-o',str(exe)],cwd=b,check=True)

shutil.copy2(here/'regression.cfg',out/'test.cfg')
(out/'build-identity.json').write_text(json.dumps({'production_executable_sha256':hashlib.sha256((b/'runtime/openttd.exe').read_bytes()).hexdigest(),'suite':a.suite},indent=2),encoding='utf8')
results=[]
for distribution in ['manual','asymmetric']:
 for mode in (['same','cross','shared','truck','removed','blocked','rotated'] if a.suite=='road' else ['chain']):
  start=out/(distribution+'-'+mode);start.mkdir(exist_ok=True)
  for phase in (['new','0','1','2'] if a.suite=='road' else ['new','input','output']):
   folder=start if phase=='new' else start/('reload-'+phase);folder.mkdir(exist_ok=True)
   env={k:v for k,v in os.environ.items() if not k.startswith('PXP_')};env['PXP_DEMO_OUTPUT']=str(folder)
   if a.suite=='road':env.update(PXP_ROAD_DIST=distribution,PXP_ROAD_MODE=mode)
   else:env['PXP_CHAIN_DIST']=distribution
   if phase!='new':env['PXP_ROAD_RELOAD' if a.suite=='road' else 'PXP_CHAIN_RELOAD']='1'
   save=start/(('phase-' if a.suite=='road' else 'chain-')+phase+'.sav')
   argv=['-e'] if phase=='new' else ['-g',str(save)]
   with (folder/'console.log').open('w',encoding='utf8') as log:
    run=subprocess.run([str(exe),'-c',str(out/'test.cfg'),'-v','null:ticks=100000','-s','null','-m','null',*argv],cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=250,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
   text=(folder/'run.log').read_text(encoding='utf8');passed=run.returncode==0 and 'STRICT PASS' in text
   if phase!='new':
    saved=re.search(('SAVED phase=' if a.suite=='road' else 'SAVED ')+phase+r' hash=(\d+)',(start/'run.log').read_text(encoding='utf8'))
    loaded=re.search(r'LOADED hash=(\d+)',text)
    passed=passed and saved is not None and loaded is not None and saved[1]==loaded[1]
   results.append(dict(distribution=distribution,mode=mode,phase=phase,returncode=run.returncode,passed=passed,result=text.splitlines()[-1]))
   (out/'results.json').write_text(json.dumps(results,indent=2),encoding='utf8');print(results[-1],flush=True)
   assert passed,text[-3000:]
