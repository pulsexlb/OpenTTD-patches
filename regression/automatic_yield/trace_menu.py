"""Exercise real trace-restriction widget hit testing and dropdown windows in a private executable."""
from pathlib import Path
import argparse,json,re,subprocess,shutil,os
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir',type=Path,required=True)
p.add_argument('--baseset',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--before-fix',action='store_true')
a=p.parse_args();root=Path(__file__).resolve().parents[2];build=a.build_dir.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
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

here=root/'regression/automatic_yield'
s=(root/'src/openttd.cpp').read_text(encoding='utf8')
s=s.replace('#include "safeguards.h"',f'#include "{here.as_posix()}/native.h"\nvoid ProbeTraceMenu(TileIndex);\nvoid GuiProbeTick();\n#include "safeguards.h"',1)
s=s.replace('void StateGameLoop()\n{','void StateGameLoop()\n{\n GuiProbeTick();',1)
s+=f'\n#include "{here.as_posix()}/native.inc"\n'+r"""
void GuiProbeTick() {
 if(HasModalProgress()||_generating_world)return;
 static bool setup=false;
 if(!setup&&_game_mode==GameMode::Editor){demo_started=true;DemoBuild();setup=true;}
 if(setup&&_game_mode==GameMode::Normal){ProbeTraceMenu(TileXY(30,40));_exit_game=true;}
}
"""
t=(root/'src/tracerestrict_gui.cpp').read_text(encoding='utf8').replace('class TraceRestrictWindow : public Window {','class TraceRestrictWindow : public Window {\n friend void ProbeTraceMenu(TileIndex);',1)
t+=r"""
void ProbeTraceMenu(TileIndex tile) {
 FILE*f=fopen("TRACE_MENU_RESULT_PATH","w");
 auto*p=GetTraceRestrictProgram(MakeTraceRestrictRefId(tile,TRACK_X),true);
 TraceRestrictInstructionItem cond{},end{};cond.SetType(TRIT_COND_UNDEFINED);end.SetType(TRIT_COND_ENDIF);
 p->items={cond.AsProgramItem(),end.AsProgramItem()};auto valid=p->Validate();assert(valid.Succeeded());
 auto*w=new TraceRestrictWindow(_program_desc,tile,TRACK_X);w->selected_instruction=1;w->UpdateButtonState();
 for(int id:{int(TR_WIDGET_TYPE_COND),int(TR_WIDGET_INSERT)}){
  auto*widget=w->GetWidget<NWidgetCore>(id);auto r=widget->GetCurrentRect();
  fprintf(f,"widget=%d disabled=%u rect=%d,%d,%d,%d\n",id,widget->IsDisabled(),r.left,r.top,r.right,r.bottom);
  fprintf(f,"hit=%d expected=%d\n",GetWidgetFromPos(w,(r.left+r.right)/2,(r.top+r.bottom)/2),id); assert(GetWidgetFromPos(w,(r.left+r.right)/2,(r.top+r.bottom)/2)==id); w->OnClick({},id,1);auto*d=FindWindowByClass(WindowClass::DropdownMenu);fprintf(f,"opened=%u\n",d!=nullptr);assert(d!=nullptr);
  if(d){d->OnMouseLoop();fprintf(f,"after_release=%u\n",FindWindowByClass(WindowClass::DropdownMenu)!=nullptr);HideDropDownMenu(w);}
 }
 fclose(f);
}
"""
t=t.replace('TRACE_MENU_RESULT_PATH',(out/'result.log').as_posix())
if a.before_fix:t=t.replace('this->current_left_plane != left_sel->shown_plane || ','',1)
exe=link(out,{'openttd':s,'tracerestrict_gui':t})
for folder,source in [('lang',build/'lang'),('baseset',a.baseset.resolve())]:shutil.copytree(source,out/folder,dirs_exist_ok=True)
shutil.copy(here/'regression.cfg',out/'test.cfg')
env=dict(os.environ,PXP_DEMO_OUTPUT=str(out),PXP_STRICT='basic')
with (out/'stderr.log').open('w') as f:run=subprocess.run([str(exe),'-v','null:until_exit','-s','null','-m','null','-x','-X','-Q','-c',str(out/'test.cfg'),'-e'],cwd=out,env=env,stdout=f,stderr=f,timeout=60)
print((out/'result.log').read_text());assert run.returncode==0
