#include "../newgrf_engine.h"

// This observes only the public coupling preflight and the public NewGRF resolver.
// No private reversal or forecast helper is exported for this regression.
struct CouplingTrialCallbackObserver final : SpriteGroup {
 mutable std::vector<uint32_t> seen;
 mutable std::vector<bool> seen_flipped;
 mutable uint starts=0;
 CallbackResultSpriteGroup allow{0x400};
 CallbackResultSpriteGroup unspecified{CALLBACK_FAILED};
 CouplingTrialCallbackObserver():SpriteGroup(SpriteGroupID::Invalid(),SGT_CALLBACK){}
 const SpriteGroup *Resolve(ResolverObject &object) const override {
  if(object.callback==CBID_VEHICLE_START_STOP_CHECK){
   starts++;
   const auto *self=static_cast<const VehicleScopeResolver*>(object.GetScope());
   seen.clear();seen_flipped.clear();
   for(const Vehicle*w=self->v->First();w;w=w->Next()){
    seen.push_back(w->index.base());
    seen_flipped.push_back(Train::From(w)->flags.Test(VehicleRailFlag::Flipped));
   }
   GetVariableExtra extra{};extra.mask=UINT32_MAX;
   self->GetVariable(0x40,0,extra); // Real callback position cache must also roll back.
   return &allow;
  }
  return &unspecified;
 }
};

TEST_CASE_METHOD(TrainDecoupleFixture,"Couple - trial callback observes the real reversal block order")
{
 const bool multiheaded=GENERATE(false,true);
 const bool mixed_sprite_directions=GENERATE(false,true);
 CAPTURE(multiheaded,mixed_sprite_directions);
 AutoRestoreBackup length_limit(_settings_game.vehicle.max_train_length,uint8_t{8});
 GRFFile grf;grf.grf_version=8;
 engine->grf_prop.grffile=&grf;
 CouplingTrialCallbackObserver observer;
 engine->grf_prop.SetSpriteGroup(CargoGRFFileProps::SG_DEFAULT,&observer);
 Train *base=MakeTrain(),*a=MakeTrain(),*b=MakeTrain(),*waiting=MakeTrain(),*wagon=MakeTrain();
 a->ClearEngine();b->ClearEngine();
 if(multiheaded){
  base->SetMultiheaded();b->SetMultiheaded();a->SetWagon();
  base->other_multiheaded_part=b;b->other_multiheaded_part=base;
 }else{
  a->SetArticulatedPart();b->SetArticulatedPart();
 }
 wagon->ClearEngine();wagon->SetWagon();
 base->SetNext(a);a->SetNext(b);waiting->SetNext(wagon);
 for(Train*v:{base,a,b})v->SetPrimary(base);
 for(Train*v:{waiting,wagon})v->SetPrimary(waiting);
 for(Train*v:{base,a,b,waiting,wagon}){
  v->direction=Direction::NE;v->grf_cache.cache_valid=0x7F;
  v->grf_cache.position_consist_length=0xABCD;v->tcache.user_def_data=0xA5;
 }
 const auto cache=base->grf_cache;
 const std::array<Train*,5> all{base,a,b,waiting,wagon};
 // NewGRF construction may assign different visible orientations to the
 // articulated subparts. A physical multipart reversal must preserve these;
 // an ordinary single vehicle keeps its original facing by toggling Flipped.
 const std::array<bool,5> original_flipped{
  mixed_sprite_directions,false,mixed_sprite_directions,false,mixed_sprite_directions
 };
 for(size_t i=0;i<all.size();i++)all[i]->flags.Set(VehicleRailFlag::Flipped,original_flipped[i]);
 // The native moving Art block flips NE -> SW. A NE waiting consist must
 // therefore also reverse, so its singleton blocks become wagon, waiting.
 const std::vector<uint32_t> expected{base->index.base(),a->index.base(),b->index.base(),wagon->index.base(),waiting->index.base()};
 const std::vector<bool> expected_flipped{
  original_flipped[0],original_flipped[1],original_flipped[2],!original_flipped[4],!original_flipped[3]
 };
 for(uint repeat=0;repeat<2;repeat++){
  REQUIRE(IsCoupleArrangementValid(base,waiting));
  REQUIRE(observer.starts>repeat);
  CHECK(observer.seen==expected);
  CHECK(observer.seen_flipped==expected_flipped);
  CHECK(base->Next()==a);CHECK(a->Next()==b);CHECK(b->Next()==nullptr);
  CHECK(waiting->Next()==wagon);CHECK(wagon->Next()==nullptr);
  for(size_t i=0;i<all.size();i++){
   Train*v=all[i];
   CHECK(v->direction==Direction::NE);CHECK(v->grf_cache==cache);CHECK(v->tcache.user_def_data==0xA5);
   CHECK(v->flags.Test(VehicleRailFlag::Flipped)==original_flipped[i]);
  }
  CHECK(base->Previous()==nullptr);CHECK(a->Previous()==base);CHECK(b->Previous()==a);
  CHECK(base->First()==base);CHECK(a->First()==base);CHECK(b->First()==base);
  if(multiheaded){CHECK(base->other_multiheaded_part==b);CHECK(b->other_multiheaded_part==base);}
  else {CHECK_FALSE(base->IsArticulatedPart());CHECK(a->IsArticulatedPart());CHECK(b->IsArticulatedPart());}
  CHECK(base->Primary()==base);CHECK(a->Primary()==base);CHECK(b->Primary()==base);
  CHECK(waiting->Primary()==waiting);CHECK(wagon->Primary()==waiting);
 }
}
