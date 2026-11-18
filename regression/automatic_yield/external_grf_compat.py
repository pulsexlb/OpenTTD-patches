"""Real AXIS Steeltown + eGRVTS / WAS / Multi-SHARK off-map refit and reload checks.

Requires an existing Windows Ninja production build. Private observation binary
only; original GRF resources and user saves are never edited. Test resource
versions: AXIS 2.3.1, eGRVTS 2.1 r240, WAS 2021-07-25, Multi-SHARK 1.1.
"""
from pathlib import Path
import argparse, hashlib, json, os, re, shutil, subprocess

parser=argparse.ArgumentParser(description=__doc__)
for name in ['build-dir','baseset','output','axis']:
    parser.add_argument('--'+name,type=Path,required=True)
parser.add_argument('--kind',choices=['air','ship','road'],required=True)
for name in ['was','shark','egrvts']:parser.add_argument('--'+name,type=Path)
a=parser.parse_args();here=Path(__file__).resolve().parent;root=here.parents[1]
build=a.build_dir.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
kind=a.kind;hook,prefix={'air':('FlightTick','FLIGHT'),'ship':('SeaTick','SEA'),'road':('RoadTick','ROAD')}[kind]
cpp=(root/'src/openttd.cpp').read_text(encoding='utf8')
cpp=cpp.replace('#include "safeguards.h"',f'#include "{here.as_posix()}/native.h"\nvoid {hook}();\n#include "safeguards.h"',1)
cpp=cpp.replace('void StateGameLoop()\n{',f'void StateGameLoop()\n{{\n {hook}();',1)
cpp+='\n'+(here/'external_grf_compat'/(kind+'.inc')).read_text(encoding='utf8')
sources={'openttd':cpp}
if kind=='road':
    eco=(root/'src/economy.cpp').read_text(encoding='utf8').replace('#include "safeguards.h"','void RefitDelivery(VehicleID,const CargoPacket*,uint,Money,bool);\n#include "safeguards.h"',1)
    sources['economy']=eco.replace('this->visual_profit += profit;','this->visual_profit += profit;RefitDelivery(this->front->index,cp,count,profit,false);',1)
commands=json.loads((build/'compile_commands.json').read_text());objects={}
for name,source in sources.items():
    command=next(c['command'] for c in commands if c['file'].replace('\\','/').endswith('/src/'+name+'.cpp'))
    cc=command[:command.index(' -D')].strip().strip('"');flags=command[:command.index(' -o ')]
    path=out/(name+'.cpp');obj=out/(name+'.obj');path.write_bytes(source.encode('utf8'))
    rsp=out/(name+'.rsp');rsp.write_text(flags[flags.index(' -D'):]+f' -I"{root.as_posix()}/src" -o "{obj.as_posix()}" -c "{path.as_posix()}"')
    subprocess.run([cc,'@'+str(rsp)],cwd=build,check=True)
    objects['CMakeFiles/openttd_lib.dir/src/'+name+'.cpp.obj']='"'+obj.as_posix()+'"'
section=next(s for s in (build/'build.ninja').read_text().split('\n\n') if s.startswith('build runtime/openttd.exe:'))
linked=section.splitlines()[0].split(': ',1)[1].split(' ',1)[1].split(' |')[0]
for old,new in objects.items():linked=linked.replace(old,new)
rsp=out/'link.rsp';rsp.write_text(linked+' '+re.search(r'^  LINK_LIBRARIES = (.*)$',section,re.M)[1])
exe=out/'diagnostic.exe';subprocess.run([cc,'-O3','-mwindows','-fstack-protector','@'+str(rsp),'-o',str(exe)],cwd=build,check=True)
for name,source in [('lang',build/'lang'),('baseset',a.baseset.resolve())]:shutil.copytree(source,out/name,dirs_exist_ok=True)
(out/'newgrf').mkdir(exist_ok=True)
grfs=[(a.axis,'1 0 0 0 0 0 16 200 80 400')]
grfs+= [(a.egrvts,'0 63 0 3 0')] if kind=='road' else [(a.was,'0 6 8 0'),(a.shark,'1 1 1')]
entries=[];identities=[]
for path,params in grfs:
    assert path is not None and path.is_file(),'Supply the required real vehicle GRFs'
    target=out/'newgrf'/path.name;shutil.copy2(path,target)
    entries.append(target.as_posix()+' = '+params)
    identities.append(dict(path=str(path.resolve()),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),parameters=params))
cfg=(here/'regression.cfg').read_text().replace('starting_year = 2000','starting_year = 2050').replace('[game_creation]','[game_creation]\nending_year = 0').replace('[newgrf]','[newgrf]\n'+'\n'.join(entries))
(out/'test.cfg').write_bytes(cfg.encode('utf8'))
(out/'build-identity.json').write_text(json.dumps(dict(production_executable_sha256=hashlib.sha256((build/'runtime/openttd.exe').read_bytes()).hexdigest(),grfs=identities),indent=2))
rows=[]
for dist in ['manual','asymmetric']:
    for model,fixed in ([(353,False),(353,True),(361,False),(361,True)] if kind=='road' else [(0,False)]):
        start=out/(dist+(f'-{model}-{int(fixed)}' if kind=='road' else ''));start.mkdir(exist_ok=True)
        for phase in ['new','0','1','2']:
            folder=start if phase=='new' else start/('reload-'+phase);folder.mkdir(exist_ok=True)
            env={k:v for k,v in os.environ.items() if not k.startswith('PXP_')}
            env.update(PXP_DEMO_OUTPUT=str(folder));env['PXP_'+prefix+'_DIST']=dist
            env['PXP_'+prefix+'_MODE']={'road':'truck','ship':'cargo','air':'same'}[kind]
            if model==361:env['PXP_REFIT_ARTIC']='1'
            if fixed:env['PXP_REFIT_FIXED']='1'
            if phase!='new':env['PXP_'+prefix+'_RELOAD']='1'
            args=['-e'] if phase=='new' else ['-g',str(start/('phase-'+phase+'.sav'))]
            with (folder/'console.log').open('w') as f:
                run=subprocess.run([str(exe),'-v','null:ticks=100000','-s','null','-m','null','-x','-X','-c',str(out/'test.cfg'),*args],cwd=out,env=env,stdout=f,stderr=f,timeout=200,creationflags=subprocess.CREATE_NO_WINDOW)
            log=(folder/'run.log').read_text(encoding='utf8');console=(folder/'console.log').read_text(encoding='utf8')
            passed=run.returncode==0 and 'STRICT PASS' in log and 'not found; checksum' not in console
            if phase!='new' and kind!='air':
                before=re.search('SAVED phase='+phase+r' hash=(\d+)',(start/'run.log').read_text())
                after=re.search(r'LOADED hash=(\d+)',log)
                passed=passed and before is not None and after is not None and before[1]==after[1]
            rows.append(dict(kind=kind,distribution=dist,model=model,fixed=fixed,phase=phase,passed=passed,returncode=run.returncode))
            (out/'results.json').write_text(json.dumps(rows,indent=2));print(rows[-1],flush=True)
            assert passed,log[-2200:]
