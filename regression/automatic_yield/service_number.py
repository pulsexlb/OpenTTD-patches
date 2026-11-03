"""Exercise station service-number commands, order controls, board rendering and reload.

Uses an isolated observer linked against the configured production objects. The
input should be basic-exit/strict-waiting.sav produced by run.py; never edits it.
"""
from pathlib import Path
import argparse
import json
import os
import re
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir', type=Path, required=True)
p.add_argument('--baseset', type=Path, required=True)
p.add_argument('--save', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--dispatch-slots', action='store_true', help='Check real 24-hour slot identities, repetitions and arrival/departure pairs')
p.add_argument('--schedule-source', type=Path, help='Optional saved departures.cpp for a negative-control build')
a = p.parse_args()
r = Path(__file__).resolve().parents[2]
b, out = a.build_dir.resolve(), a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
for folder, source in [('lang', b / 'lang'), ('baseset', a.baseset.resolve())]:
    shutil.copytree(source, out / folder, dirs_exist_ok=True)
commands = json.loads((b / 'compile_commands.json').read_text())
objects = {}

ui = r'''
void ServiceNumberUIProbe(const Vehicle *v, bool reload)
{
 ShowOrdersWindow(v);
 auto *w = static_cast<OrdersWindow *>(FindWindowById(WindowClass::VehicleOrders, v->index));
 assert(w != nullptr);
 w->selected_order = 0;
 assert(w->CanSetStationServiceNumber());
 if (!reload) {
  const std::string inbound(v->fixed_service_number.c_str());
  assert(v->GetOrder(0)->GetServiceNumberMode() == 0);
  w->OnDropdownSelect(WID_O_SERVICE_NUMBER, 1, 0);
  w->OnQueryTextFinished(std::nullopt);
  assert(v->GetOrder(0)->GetServiceNumberMode() == 0);
  w->OnQueryTextFinished(std::string("G123"));
  assert(v->GetOrder(0)->GetServiceNumber() == "G123");
  w->selected_order = 1;
  w->OnDropdownSelect(WID_O_SERVICE_NUMBER, 1, 0);
  w->selected_order = 0; // The query must still edit the originally selected stop.
  w->OnQueryTextFinished(std::string("G456"));
  assert(v->GetOrder(1)->GetServiceNumber() == "G456");
  assert(v->GetServiceNumberForDeparture(v->GetOrder(0), "D456") == "G123");
  assert(v->GetServiceNumberForDeparture(v->GetOrder(1), "D456") == "G456");
  assert(std::string_view(v->fixed_service_number) == inbound); // Editing a future stop does not execute it.
 } else {
  assert(v->GetOrder(0)->GetServiceNumber() == "G123");
  assert(v->GetOrder(1)->GetServiceNumber() == "G456");
  w->OnDropdownSelect(WID_O_SERVICE_NUMBER, 2, 0);
  assert(v->GetOrder(0)->GetServiceNumberMode() == 2);
 }
}
'''
board = r'''
#include "core/backup_type.hpp"
#include "station_cmd.h"
#include "order_cmd.h"
#include "timetable_cmd.h"
#include "command_func.h"
void ServiceSlotProbe(const Vehicle *v)
{
 Vehicle *train=Vehicle::Get(v->index);
 _settings_time.time_in_minutes=true;
 _settings_time.ticks_per_minute=74;
 const auto day=_settings_time.FromTickMinutes(_settings_time.NowInTickMinutes().ToSameDayClockTime(0,0));
 const uint duration=60*_settings_time.ticks_per_minute;
 for(uint i=0;i<train->GetNumOrders();++i){
  assert(CmdSetOrderServiceNumber(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID(i),0,"").Succeeded());
  Order *o=train->GetOrder(i);o->SetTravelTime(1000);o->SetTravelTimetabled(true);o->SetWaitTime(32);o->SetWaitTimetabled(true);
 }
 auto &ds=train->orders->GetDispatchScheduleByIndex(0);
 ds.GetScheduledDispatchMutable().clear();ds.SetScheduledDispatchDuration(duration);ds.SetScheduledDispatchStartTick(day);
 ds.SetSupplementaryName(DispatchSchedule::SupplementaryNameType::RouteID,1,"D223");
 ds.SetSupplementaryName(DispatchSchedule::SupplementaryNameType::RouteID,2,"D230");
 for(uint i=1;i<=3;i++)ds.AddScheduledDispatch(i*10*_settings_time.ticks_per_minute,0,i==3?0:i);
 assert(CmdTimetableSeparation(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),false).Succeeded());
 assert(CmdSchDispatchSetEnabled(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),true).Succeeded());
 assert(CmdChangeTimetable(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},MTF_ASSIGN_SCHEDULE,0,{}).Succeeded());
 /* A representative vehicle's old identity must never label its future timetable slots. */
 train->fixed_service_number="STALE-VEHICLE";train->service_number_suppressed=true;
 train->dispatch_records[0].route_id=2;
 ShowDeparturesWindow(v->GetOrder(0)->GetDestination().ToStationID());
 auto *w=static_cast<DeparturesWindow*>(FindWindowById(WindowClass::DepartureBoard,v->GetOrder(0)->GetDestination().base()));assert(w);
 w->source_mode=DSM_SCHEDULE_24H;w->mode=DM_SEPARATE;w->cargo_mode=DCF_ALL_CARGOES;w->show_empty=true;w->show_types[VehicleType::Train]=true;
 FILE *log=fopen((std::string(std::getenv("PXP_SERVICE_OUTPUT"))+"/dispatch-slots.log").c_str(),"w");
 for(uint8_t mode:{uint8_t{2},uint8_t{0},uint8_t{1}}){
  assert(CmdSetOrderServiceNumber(DoCommandFlag::Execute,OrderTargetType::Vehicle,v->index.base(),VehicleOrderID{0},mode,mode==1?"MANUAL":"").Succeeded());
  const auto position=ds.BackupPosition();const auto rng=_random;
  w->vehicles_invalid=true;w->calc_tick_countdown=0;w->OnGameTick();
  uint departures=0,arrivals=0,empty=0,first=0,second=0;bool later_repeat=false;
  for(const auto &d:w->departures)if(d->vehicle==v){
   if(d->type==D_DEPARTURE){
    departures++;
    const auto offset=(d->scheduled_tick-day).base()%duration;
    const std::string expected=offset==10*_settings_time.ticks_per_minute?"D223":offset==20*_settings_time.ticks_per_minute?"D230":"";
    fprintf(log,"mode=%u tick=%lld offset=%lld slot_number=%s displayed=%s\n",mode,(long long)d->scheduled_tick.base(),(long long)offset,d->schedule_route_id.c_str(),GetDepartureBoardServiceNumber(*d,DSM_SCHEDULE_24H).c_str());fflush(log);
    assert(d->schedule_route_id==expected);
    assert(GetDepartureBoardServiceNumber(*d,DSM_SCHEDULE_24H)==(mode==1?"MANUAL":expected));
    if(expected.empty())empty++;if(expected=="D223")first++;if(expected=="D230")second++;
    if(d->scheduled_tick>=day+duration)later_repeat=true;
   }else arrivals++;
  }
  assert(departures>3&&arrivals>0&&first>0&&second>0&&empty>0&&later_repeat);
  assert(ds.GetScheduledDispatchStartTick()==position.scheduled_dispatch_start_tick&&ds.GetScheduledDispatchLastDispatch()==position.scheduled_dispatch_last_dispatch);
  assert(_random.state[0]==rng.state[0]&&_random.state[1]==rng.state[1]);
  fprintf(log,"PASS mode=%u departures=%u arrivals=%u D223=%u D230=%u unnamed=%u repeat=1\n",mode,departures,arrivals,first,second,empty);fflush(log);
 }
 w->mode=DM_DEPARTURES;w->vehicles_invalid=true;w->calc_tick_countdown=0;w->OnGameTick();
 uint departure_only=0;
 for(const auto &d:w->departures)if(d->vehicle==v){assert(d->type==D_DEPARTURE);departure_only++;}
 assert(departure_only==72);
 fprintf(log,"PASS departure-only mode: 72 departures, zero arrival rows\n");
 fprintf(log,"PASS slots match departure times, repeated periods preserve IDs, separate mode represents arrival and departure events\n");fclose(log);
}
void ServiceNumberBoardProbe(const Vehicle *v, const char *expected_dispatch)
{
 _settings_client.gui.departure_show_vehicle = true;
 _settings_client.gui.departure_show_schedule_route_id = false;
 ShowDeparturesWindow(StationID(v->GetOrder(0)->GetDestination().base()));
 auto *w = static_cast<DeparturesWindow *>(FindWindowById(WindowClass::DepartureBoard, v->GetOrder(0)->GetDestination().base()));
 assert(w != nullptr);
 w->source_mode = DSM_LIVE;
 w->mode = DM_DEPARTURES;
 w->cargo_mode = DCF_ALL_CARGOES;
 w->show_types[VehicleType::Train] = true;
 w->show_empty = true;
 w->calc_tick_countdown = 0;
 w->vehicles_invalid = true;
 w->OnGameTick();
 FILE *diag = fopen((std::string(std::getenv("PXP_SERVICE_OUTPUT")) + "/board-state.log").c_str(), "a");
 fprintf(diag, "vehicles=%zu rows=%zu orders=%u source=%u\n", w->vehicles.size(), w->departures.size(), v->GetNumOrders(), v->GetOrder(0)->GetDestination().base()); fclose(diag);
 size_t found = 0;
 for (const auto &d : w->departures) if (d->vehicle == v) {
  assert(d->schedule_route_id == expected_dispatch);
  found++;
 }
 assert(found > 0);
 AutoRestoreBackup dpi(_cur_dpi, &_screen);
 w->OnPaint();
}
'''
entry = r'''
#include "train.h"
#include "vehicle_cmd.h"
#include "station_cmd.h"
#include "station_map.h"
#include "order_cmd.h"
#include "timetable_cmd.h"
#include "order_serialisation.h"
#include "company_func.h"
#include "pbs.h"
#include "core/random_func.hpp"
void ServiceNumberUIProbe(const Vehicle*, bool);
void ServiceNumberBoardProbe(const Vehicle*, const char*);
void ServiceSlotProbe(const Vehicle*);
void ServiceNumberProbeTick()
{
 FILE *stage = fopen((std::string(std::getenv("PXP_SERVICE_OUTPUT")) + "/stages.log").c_str(), "a");
 fprintf(stage, "tick mode=%u modal=%u generating=%u\n", uint(_game_mode), HasModalProgress(), _generating_world); fclose(stage);
 if (_game_mode != GameMode::Normal || HasModalProgress() || _generating_world) return;
 static bool once = false; if (once) return; once = true;
 const bool reload = std::getenv("PXP_SERVICE_RELOAD") != nullptr;
 Train *v = nullptr;
 for (auto *t : Train::Iterate()) if (t->IsPrimaryVehicle() && t->unitnumber == 1) v = t;
 assert(v != nullptr);
 stage = fopen((std::string(std::getenv("PXP_SERVICE_OUTPUT")) + "/stages.log").c_str(), "a"); fprintf(stage, "found train\n"); fclose(stage);
 SetLocalCompany(v->owner);
 _current_company = v->owner;
 const auto name = std::string(v->name.c_str());
 const auto tile = v->tile;
 const auto order = v->cur_real_order_index;
 const auto direction = v->direction;
 const auto reservation = FollowTrainReservation(v, nullptr, FollowTrainReservationFlag::OkayUnused);
 assert(CmdSetVehicleServiceNumber({}, VehicleID::Invalid(), "X").Failed());
 assert(CmdSetVehicleServiceNumber({}, v->index, std::string(MAX_LENGTH_VEHICLE_NAME_CHARS, 'A')).Failed());
 assert(v->Next() && CmdSetVehicleServiceNumber({}, v->Next()->index, "X").Failed());
 assert(CmdSetVehicleServiceNumber({}, v->index, "dry-run").Succeeded());
 _current_company = OWNER_NONE;
 assert(CmdSetVehicleServiceNumber(DoCommandFlag::Execute, v->index, "foreign").Failed());
 _current_company = v->owner;
 if (!reload) {
  if (!IsRailStationTile(TileXY(242,40))) {
   assert(CmdBuildRailStation(DoCommandFlag::Execute, TileXY(242,40), RAILTYPE_RAIL, Axis::X, 1, 2, StationClassID{0}, 0, StationID::Invalid(), true).Succeeded());
   Order next; next.MakeGoToStation(GetStationIndex(TileXY(242,40)));
   next.SetNonStopType(ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS);
   assert(CmdInsertOrder(DoCommandFlag::Execute, InsertOrderCmdData(v->index, VehicleOrderID{1}, next)).Succeeded());
   assert(CmdDeleteOrder(DoCommandFlag::Execute, OrderTargetType::Vehicle, v->index.base(), VehicleOrderID{2}).Succeeded());
  }
  for (Order *o : v->Orders()) {
   o->SetTravelTime(1000); o->SetTravelTimetabled(true);
   o->SetWaitTime(32); o->SetWaitTimetabled(true);
  }
  auto &schedules = v->orders->GetScheduledDispatchScheduleSet();
  schedules.clear(); schedules.emplace_back();
  schedules[0].SetScheduledDispatchDuration(10000);
  schedules[0].SetScheduledDispatchStartTick(_state_ticks);
  schedules[0].SetSupplementaryName(DispatchSchedule::SupplementaryNameType::RouteID, 1, "D456");
  schedules[0].AddScheduledDispatch(0, 0, 1);
  auto &record = v->dispatch_records[0]; record.route_id = 1; record.dispatched = _state_ticks;
 }
 const auto rng = _random;
 ServiceNumberUIProbe(v, reload);
 stage = fopen((std::string(std::getenv("PXP_SERVICE_OUTPUT")) + "/stages.log").c_str(), "a"); fprintf(stage, "UI done\n"); fclose(stage);
 assert(v->orders->GetDispatchScheduleByIndex(0).GetScheduledDispatch()[0].route_id == 1);
 BaseConsist copied; copied.CopyConsistPropertiesFrom(v);
 assert(std::string_view(copied.fixed_service_number) == std::string_view(v->fixed_service_number));
 assert(CmdSetOrderServiceNumber({}, OrderTargetType::Vehicle, v->index.base(), VehicleOrderID{999}, 1, "X").Failed());
 assert(CmdSetOrderServiceNumber({}, OrderTargetType::Vehicle, v->index.base(), VehicleOrderID{0}, 3, "X").Failed());
 assert(CmdSetOrderServiceNumber({}, OrderTargetType::Vehicle, v->index.base(), VehicleOrderID{0}, 1, "").Failed());
 assert(CmdSetOrderServiceNumber({}, OrderTargetType::Vehicle, v->index.base(), VehicleOrderID{0}, 2, "X").Failed());
 assert(std::string_view(v->name) == name && tile == v->tile && order == v->cur_real_order_index && direction == v->direction);
 const auto after = FollowTrainReservation(v, nullptr, FollowTrainReservationFlag::OkayUnused);
 assert(reservation.tile == after.tile && reservation.trackdir == after.trackdir);
 assert(_random.state[0] == rng.state[0] && _random.state[1] == rng.state[1]);
 if (!reload) {
  const std::string json = OrderListToJSONString(v->orders);
  assert(json.find("service-number") != std::string::npos);
  const auto errors = ImportJsonOrderList(v, json);
  assert(!errors.HasErrors());
  assert(v->GetOrder(0)->GetServiceNumber() == "G123" && v->GetOrder(1)->GetServiceNumber() == "G456");
  // Replacing an order list intentionally drops runtime dispatch history;
  // restore this fixture's synthetic history independently of the imported settings.
  auto &record = v->dispatch_records[0]; record.route_id = 1; record.dispatched = _state_ticks;
 }
 ServiceNumberBoardProbe(v, "D456");
 if (reload) {
  assert(CmdSetOrderServiceNumber(DoCommandFlag::Execute, OrderTargetType::Vehicle, v->index.base(), VehicleOrderID{0}, 0, "").Succeeded());
  assert(CmdSetVehicleServiceNumber(DoCommandFlag::Execute, v->index, "").Succeeded());
  v->dispatch_records[0].route_id = 0;
  ServiceNumberBoardProbe(v, "");
 }
 if (!reload) {
  const std::string path = std::string(std::getenv("PXP_SERVICE_OUTPUT")) + "/numbered.sav";
  assert(SaveOrLoad(path, SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::None, false) == SaveLoadResult::Ok);
 }
 FILE *f = fopen((std::string(std::getenv("PXP_SERVICE_OUTPUT")) + (reload ? "/reload.pass" : "/fresh.pass")).c_str(), "w");
 fprintf(f, "PASS commands, ownership, order controls, per-stop exclusive source, board rows, consist copy, native reservation unchanged; reload=%u\n", reload); fclose(f);
 if (!reload && std::getenv("PXP_SERVICE_SLOTS")) ServiceSlotProbe(v);
 _exit_game = true;
}
'''
for name, suffix, additions in [('openttd', 'openttd', entry), ('orders', 'order_gui', ui), ('board', 'departures_gui', board)]:
    source = (r / f'src/{suffix}.cpp').read_text(encoding='utf8')
    if name == 'openttd':
        source = source.replace('void StateGameLoop()\n{', 'void ServiceNumberProbeTick();\nvoid StateGameLoop()\n{\n ServiceNumberProbeTick();', 1)
    if name == 'orders':
        source = source.replace('private:', 'public:')
    if name == 'board':
        source = source.replace('protected:', 'public:', 1)
        source = source.replace('/* Vehicle name */', '''FILE *probe = fopen((std::string(std::getenv("PXP_SERVICE_OUTPUT")) + "/board-rows.log").c_str(), "a");
        fprintf(probe, "vehicle=%u service=%.*s draw_name=%u\\n", d->vehicle->index.base(), int(service_number.size()), service_number.data(), _settings_client.gui.departure_show_vehicle && service_number.empty()); fclose(probe);
        /* Vehicle name */''', 1)
    cpp, obj = out / (name + '.cpp'), out / (name + '.obj')
    cpp.write_bytes((source + '\n' + additions).encode('utf8'))
    cmd = next(c['command'] for c in commands if c['file'].replace('\\', '/').endswith('/src/' + suffix + '.cpp'))
    compiler = cmd[:cmd.index(' -D')].strip().strip('"')
    flags = cmd[:cmd.index(' -o ')]
    rsp = out / (name + '.rsp')
    rsp.write_text(flags[flags.index(' -D'):] + f' -I"{r.as_posix()}/src" -o "{obj.as_posix()}" -c "{cpp.as_posix()}"', encoding='utf8')
    subprocess.run([compiler, '@' + str(rsp)], cwd=b, check=True)
    objects[f'CMakeFiles/openttd_lib.dir/src/{suffix}.cpp.obj'] = obj
section = next(s for s in (b / 'build.ninja').read_text().split('\n\n') if s.startswith('build runtime/openttd.exe:'))
linked = section.splitlines()[0].split(': ', 1)[1].split(' ', 1)[1].split(' |')[0]
for original, obj in objects.items():
    linked = linked.replace(original, '"' + obj.as_posix() + '"')
if a.schedule_source:
    cmd = next(c['command'] for c in commands if c['file'].replace('\\', '/').endswith('/src/departures.cpp'))
    flags = cmd[:cmd.index(' -o ')]
    obj = out / 'old-departures.obj'
    rsp = out / 'old-departures.rsp'
    rsp.write_text(flags[flags.index(' -D'):] + f' -I"{r.as_posix()}/src" -o "{obj.as_posix()}" -c "{a.schedule_source.resolve().as_posix()}"', encoding='utf8')
    subprocess.run([compiler, '@' + str(rsp)], cwd=b, check=True)
    linked = linked.replace('CMakeFiles/openttd_lib.dir/src/departures.cpp.obj', '"' + obj.as_posix() + '"')
rsp = out / 'link.rsp'
rsp.write_text(linked + ' ' + re.search(r'^  LINK_LIBRARIES = (.*)$', section, re.M)[1], encoding='utf8')
exe = out / 'service-test.exe'
subprocess.run([compiler, '-O3', '-mwindows', '-fstack-protector', '@' + str(rsp), '-o', str(exe)], cwd=b, check=True)
(out / 'test.cfg').write_text((Path(__file__).parent / 'regression.cfg').read_text(encoding='utf8'), encoding='utf8')
for reload in [False, True]:
    env = dict(os.environ, PXP_SERVICE_OUTPUT=str(out))
    if a.dispatch_slots:
        env['PXP_SERVICE_SLOTS'] = '1'
    if reload:
        env['PXP_SERVICE_RELOAD'] = '1'
    with (out / ('reload.log' if reload else 'fresh.log')).open('w') as log:
        run = subprocess.run([str(exe), '-v', 'null:until_exit', '-s', 'null', '-m', 'null', '-x', '-X', '-Q', '-c', str(out / 'test.cfg'), '-g', str(out / 'numbered.sav' if reload else a.save.resolve())], cwd=out, env=env, stdout=log, stderr=log, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0), timeout=90)
    assert run.returncode == 0, run.returncode
    print((out / ('reload.pass' if reload else 'fresh.pass')).read_text(), flush=True)
rows = (out / 'board-rows.log').read_text()
assert 'service=G123 draw_name=0' in rows and 'service=D456 draw_name=0' in rows, rows
assert 'service= draw_name=1' in rows, rows
print('PASS actual departure-board draw branch: fixed then dispatch, names hidden; no number restores name', flush=True)
if a.dispatch_slots:
    assert 'PASS slots match departure times' in (out / 'dispatch-slots.log').read_text()
