"""Real AXIS coke recipe/transport, wrong-entrance turnaround and reload tests.

Requires the user's exact AXIS 2.3.1 GRF; does not bundle or modify that resource.
The executable is a private observer linked against the production build.
"""
from pathlib import Path
import argparse, hashlib, json, os, re, shutil, subprocess

ap = argparse.ArgumentParser(description=__doc__)
for name in ['build-dir', 'baseset', 'output', 'axis-grf']:
    ap.add_argument('--' + name, type=Path, required=True)
a = ap.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[1]
build, out = a.build_dir.resolve(), a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
entry = (root/'src/openttd.cpp').read_text(encoding='utf8')
entry = entry.replace('#include "safeguards.h"', '#include "'+here.as_posix()+'/native.h"\nvoid AxisTick();\n#include "safeguards.h"', 1)
entry = entry.replace('void StateGameLoop()\n{', 'void StateGameLoop()\n{\n AxisTick();', 1)
entry += '\n' + (here/'external_axis_driving.inc').read_text(encoding='utf8')
market = (root/'src/external_city.cpp').read_text(encoding='utf8').replace('void ServiceExternalCityVehicle(Vehicle *train, bool arrival)', 'static void ServiceExternalCityVehicleNative(Vehicle *train, bool arrival)', 1)
market += '\nvoid AxisMarketBefore(Vehicle*,bool);void AxisMarketAfter(Vehicle*,bool);\nvoid ServiceExternalCityVehicle(Vehicle*v,bool arrival){AxisMarketBefore(v,arrival);ServiceExternalCityVehicleNative(v,arrival);AxisMarketAfter(v,arrival);}\n'
vehicle = (root/'src/vehicle.cpp').read_text(encoding='utf8').replace('bool Vehicle::BeginExternalTrip()\n{','void AxisEntranceAttempt(Vehicle*);\nbool Vehicle::BeginExternalTrip()\n{\n AxisEntranceAttempt(this);',1)
vehicle = vehicle.replace('void AxisEntranceAttempt(Vehicle*);', 'void AxisEntranceAttempt(Vehicle*); void AxisEntryStarted(Vehicle*);', 1).replace('this->StartExternalJourney(city);','AxisEntryStarted(this);this->StartExternalJourney(city);',1)
section = next(s for s in (build/'build.ninja').read_text().split('\n\n') if s.startswith('build runtime/openttd.exe:'))
linked = section.splitlines()[0].split(': ',1)[1].split(' ',1)[1].split(' |')[0]
commands = json.loads((build/'compile_commands.json').read_text())
for name, content in [('openttd', entry), ('external_city', market), ('vehicle', vehicle)]:
    cpp, obj, rsp = out/(name+'.cpp'), out/(name+'.obj'), out/(name+'.rsp')
    cpp.write_bytes(content.encode('utf8'))
    cmd = next(c['command'] for c in commands if c['file'].replace('\\','/').endswith('/src/'+name+'.cpp'))
    cc = cmd[:cmd.index(' -D')].strip().strip('"')
    flags = cmd[cmd.index(' -D'):cmd.index(' -o ')]
    rsp.write_text(flags+' -I"'+root.as_posix()+'/src" -o "'+obj.as_posix()+'" -c "'+cpp.as_posix()+'"', encoding='utf8')
    subprocess.run([cc, '@'+str(rsp)], cwd=build, check=True)
    linked = linked.replace('CMakeFiles/openttd_lib.dir/src/'+name+'.cpp.obj', '"'+obj.as_posix()+'"')
rsp, exe = out/'link.rsp', out/'external-axis-driving.exe'
rsp.write_text(linked+' '+re.search(r'^  LINK_LIBRARIES = (.*)$',section,re.M)[1], encoding='utf8')
subprocess.run([cc,'-O3','-mwindows','-fstack-protector','@'+str(rsp),'-o',str(exe)],cwd=build,check=True)
for name, src in [('lang',build/'lang'),('baseset',a.baseset.resolve())]:
    shutil.copytree(src,out/name,dirs_exist_ok=True)
(out/'newgrf').mkdir(exist_ok=True)
shutil.copy2(a.axis_grf,out/'newgrf/axis.grf')
(out/'test.cfg').write_text((here/'regression.cfg').read_text().replace('[newgrf]','[newgrf]\naxis.grf = 1 0 0 0 0 0 16 200 80 400\n'),encoding='utf8')
(out/'build-identity.json').write_text(json.dumps({'production_executable_sha256':hashlib.sha256((build/'runtime/openttd.exe').read_bytes()).hexdigest()},indent=2))
rows=[]
for dist in ['manual','asymmetric','equal','nearest']:
    case=out/dist;case.mkdir(exist_ok=True)
    for phase in ['new','input','output']:
        folder=case if phase=='new' else case/('reload-'+phase);folder.mkdir(exist_ok=True)
        env={k:v for k,v in os.environ.items() if not k.startswith('PXP_')}
        env.update(PXP_DEMO_OUTPUT=str(folder),PXP_AXIS_DIST=dist)
        if phase!='new':env['PXP_AXIS_RELOAD']='1'
        argv=['-e'] if phase=='new' else ['-g',str(case/('axis-'+phase+'.sav'))]
        with (folder/'console.log').open('w') as f:
            run=subprocess.run([str(exe),'-v','null:ticks=100000','-s','null','-m','null','-x','-X','-c',str(out/'test.cfg'),*argv],cwd=out,env=env,stdout=f,stderr=f,timeout=250,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        log=(folder/'run.log').read_text(encoding='utf8');ok=run.returncode==0 and 'STRICT PASS' in log
        if phase!='new':
            old=re.search(r'SAVED '+phase+r' hash=(\d+)',(case/'run.log').read_text())
            new=re.search(r'LOADED hash=(\d+)',log);ok=ok and old is not None and new is not None and old[1]==new[1]
        rows.append(dict(distribution=dist,phase=phase,returncode=run.returncode,passed=ok,result=log.splitlines()[-1] if log else 'empty'))
        (out/'results.json').write_text(json.dumps(rows,indent=2));print(rows[-1],flush=True)
        assert ok,log[-3000:]
