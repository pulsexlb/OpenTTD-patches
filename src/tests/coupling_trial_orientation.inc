#include "../newgrf_engine.h"

// This observes only the public coupling preflight and the public NewGRF resolver.
// No private reversal or forecast helper is exported for this regression.
struct CouplingTrialCallbackObserver final : SpriteGroup {
 mutable std::vector<uint32_t> seen;
 mutable std::vector<bool> seen_flipped;
 mutable uint starts=0;
 CallbackResultSpriteGroup allow{0x400};
 CallbackResultSpriteGroup deny{0};
 CallbackResultSpriteGroup unspecified{CALLBACK_FAILED};
 bool reject=false;
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
   return reject?&deny:&allow;
  }
  return &unspecified;
 }
};

TEST_CASE_METHOD(TrainDecoupleFixture,"Couple - trial callback observes the real reversal block order")
{
 const bool multiheaded=GENERATE(false,true);
 const bool mixed_sprite_directions=GENERATE(false,true);
 CAPTURE(multiheaded,mixed_sprite_directions);
 AllocateMap(64,64);
 auto cleanup=scope_guard([]{DeallocateMap();});
 const TileIndex tile=TileXY(20,20);
 MakeRailNormal(tile,OWNER_NONE,TRACK_BIT_X,RAILTYPE_RAIL);
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
  a->SetUnitMain(base);b->SetUnitMain(base);
 }
 wagon->ClearEngine();wagon->SetWagon();
 base->SetNext(a);a->SetNext(b);waiting->SetNext(wagon);
 for(Train*v:{base,a,b})v->SetPrimary(base);
 for(Train*v:{waiting,wagon})v->SetPrimary(waiting);
 for(Train*v:{base,a,b,waiting,wagon}){
  v->tile=tile;v->track=TRACK_BIT_X;
  v->direction=Direction::NE;v->grf_cache.cache_valid=0x7F;
  v->grf_cache.position_consist_length=0xABCD;v->tcache.user_def_data=0xA5;
 }
 const auto cache=base->grf_cache;
 const std::array<Train*,5> all{base,a,b,waiting,wagon};
 // NewGRF construction may assign different visible orientations to the subparts.
 const std::array<bool,5> original_flipped{
  mixed_sprite_directions,false,mixed_sprite_directions,false,mixed_sprite_directions
 };
 for(size_t i=0;i<all.size();i++)all[i]->flags.Set(VehicleRailFlag::Flipped,original_flipped[i]);
 // The moving front meets the waiting tail; both chains reverse in the trial.
 const std::vector<uint32_t> expected{b->index.base(),a->index.base(),base->index.base(),wagon->index.base(),waiting->index.base()};
 const std::vector<bool> expected_flipped{
  !original_flipped[2],!original_flipped[1],!original_flipped[0],!original_flipped[4],!original_flipped[3]
 };
 for(uint repeat=0;repeat<2;repeat++){
  REQUIRE(IsCoupleArrangementValid(base,waiting,wagon));
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
  else {CHECK_FALSE(base->IsArticulatedPart());CHECK(a->IsArticulatedPart());CHECK(b->IsArticulatedPart());CHECK(a->GetUnitMain()==base);CHECK(b->GetUnitMain()==base);}
  CHECK(base->Primary()==base);CHECK(a->Primary()==base);CHECK(b->Primary()==base);
  CHECK(waiting->Primary()==waiting);CHECK(wagon->Primary()==waiting);
 }

 observer.reject=true;
 base->current_order.MakeGoToCouple();
 waiting->current_order.MakeWaitCouple();
 base->cur_speed=12;
 base->current_order_time=73;
 base->x_pos=TileX(tile)*TILE_SIZE+8;
 base->y_pos=TileY(tile)*TILE_SIZE+8;
 wagon->x_pos=base->x_pos-VEHICLE_LENGTH;
 wagon->y_pos=base->y_pos;
 const uint starts=observer.starts;
 GameRandomSeedChecker random;
 CHECK_FALSE(HandleTrainCouplingContact(base,wagon));
 CHECK(observer.starts==starts+1);
 CHECK(base->current_order.IsType(OT_GOTO_COUPLE));
 CHECK(waiting->current_order.IsType(OT_WAIT_COUPLE));
 CHECK(base->cur_speed==12);
 CHECK(base->current_order_time==73);
 CHECK(base->Next()==a);
 CHECK(waiting->Next()==wagon);
 CHECK(random.Check());
}
