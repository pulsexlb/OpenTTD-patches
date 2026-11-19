from pathlib import Path
import subprocess,sys,shutil,argparse
args=argparse.ArgumentParser(description='Compile the test NML and run real fixed-recipe supply chains with reloads.')
for name in ['build-dir','baseset','output']:args.add_argument('--'+name,type=Path,required=True)
args.add_argument('--nmlc',type=Path,required=True)
a=args.parse_args();h=Path(__file__).resolve().parent;r=h.parents[1];p=a.output.resolve();p.mkdir(parents=True,exist_ok=True)
out=p/'static';out.mkdir(exist_ok=True);(out/'lang-src').mkdir(exist_ok=True);(out/'newgrf').mkdir(exist_ok=True)
(out/'lang-src/english.lng').write_text('##grflangid 0x01\nSTR_GRF_NAME :PXP static recipe compatibility test\nSTR_GRF_DESCRIPTION :Generated regression resource, not a third-party package.\n')
grf=out/'newgrf/static.grf'
subprocess.run([sys.executable,str(a.nmlc.resolve()),'--grf',str(grf),'--lang-dir',str(out/'lang-src'),str(h/'external_static_industry.nml')],cwd=out,check=True)
s=(h/'external_transport_driving.py').read_text()
s=s.replace('here=Path(__file__).resolve().parent;r=here.parents[1];',f'here=Path({h.as_posix()!r});r=here.parents[1];')
needle="s+='\\n'+(here/('external_'+a.suite+'_driving.inc')).read_text(encoding='utf8')"
assert needle in s
extra=r'''
fixture=(here/'external_production_driving.inc').read_text(encoding='utf8')
fixture=fixture.replace('IndustryType(i==0?18:6)','StaticType(i==0?1:0)')
fixture=fixture.replace('static FILE* chain_log;',r"""
#include "newgrf_config.h"
static IndustryType StaticType(uint local){for(IndustryType i=0;i<NUM_INDUSTRYTYPES;++i){const auto*s=GetIndustrySpec(i);if(s->enabled&&s->grf_prop.grffile&&s->grf_prop.local_id==local)return i;}assert(false);return IT_INVALID;}
static void StaticProbe(){
 auto*c=ExternalCity::Create();const auto type=StaticType(0);assert(GetIndustrySpec(type)->grf_prop.grffile!=nullptr);assert(ExternalIndustryUsesRecipe(type));
 c->industries_initialized=true;c->industry_types={type,IT_INVALID,IT_INVALID};InitializeExternalCityMarket(c);
 auto coal=GetCargoTypeByLabel(CT_COAL),steel=GetCargoTypeByLabel(CT_STEEL),goods=GetCargoTypeByLabel(CT_GOODS);
 assert(c->stock[steel]==0&&c->stock[goods]==0);RefillExternalCityMarkets();assert(c->stock[steel]==0&&c->stock[goods]==0);
 StoreExternalCityInputs(c,coal,7);RefillExternalCityMarkets();assert(c->stock[steel]==3&&c->stock[goods]==1);
 StoreExternalCityInputs(c,coal,1);RefillExternalCityMarkets();assert(c->stock[steel]==4&&c->stock[goods]==2);
 c->stock[goods]=4096;StoreExternalCityInputs(c,coal,8);RefillExternalCityMarkets();
 assert(c->stock[steel]==5&&c->industry_inputs[ExternalIndustryCargoIndex(0,coal)]==5); // Three units fit fractional output space.
 c->stock[goods]=2;c->industry_counter=1;TickExternalCityIndustries(256);assert(c->stock[steel]==8&&c->stock[goods]==4);delete c;
}
static FILE* chain_log;
""")
fixture=fixture.replace(' if(!setup&&_game_mode==GameMode::Editor){',' if(!setup&&_game_mode==GameMode::Editor){\n StaticProbe();',1)
fixture=fixture.replace(' if(!setup||_game_mode!=GameMode::Normal)return;',' if(!setup||_game_mode!=GameMode::Normal)return;\n assert(ExternalIndustryUsesRecipe(StaticType(0)));',1)
fixture=fixture.replace('real original chain','real static NewGRF chain')
s+='\n'+fixture
'''
s=s.replace(needle,extra)
s=s.replace("shutil.copy2(here/'regression.cfg',out/'test.cfg')", "(out/'test.cfg').write_bytes((here/'regression.cfg').read_text().replace('[newgrf]', '[newgrf]\\n'+str(out/'newgrf/static.grf')+' = \\n').encode())")
runner=p/'static_driver.py';runner.write_bytes(s.encode())
subprocess.run([sys.executable,str(runner),'--build-dir',str(a.build_dir.resolve()),'--baseset',str(a.baseset.resolve()),'--output',str(out),'--suite','production'],cwd=r,check=True)
