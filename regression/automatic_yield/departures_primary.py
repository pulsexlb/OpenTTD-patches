"""Replay a saved PXP logical primary through the real departure-board UI.

The supplied config must resolve the save's NewGRFs. The input save is read-only.
Example: --company 1 --unit 267 --station 554. IDs are test inputs, never game rules.
"""
from pathlib import Path
import argparse, json, os, re, subprocess

p = argparse.ArgumentParser(description=__doc__)
for name in ['build-dir', 'save', 'config', 'output']:
    p.add_argument('--' + name, type=Path, required=True)
for name in ['company', 'unit', 'station']:
    p.add_argument('--' + name, type=int, required=True)
p.add_argument('--expect-grfs', type=int)
p.add_argument('--gui-source', type=Path, help='Optional negative-control departures_gui.cpp')
a = p.parse_args()
r = Path(__file__).resolve().parents[2]
b, out = a.build_dir.resolve(), a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
commands = json.loads((b / 'compile_commands.json').read_text())
objects = {}
probe = r'''
#include "train.h"
#include "newgrf_config.h"
#include "core/backup_type.hpp"
#include "core/random_func.hpp"
#include "sl/saveload.h"
#include "sl/saveload_func.h"
void DeparturePrimaryProbe()
{
 if (_game_mode != GameMode::Normal) return;
 static bool done = false; if (done) return; done = true;
 const bool reload = std::getenv("PXP_PRIMARY_RELOAD") != nullptr;
 const std::string path = std::getenv("PXP_PRIMARY_OUTPUT");
 FILE *log = fopen((path + (reload ? "/reload.log" : "/fresh.log")).c_str(), "w");
 const Train *subject = nullptr;
 for (const Train *t : Train::Iterate()) if (t->IsPrimaryVehicle() && t->owner.base() == std::strtoul(std::getenv("PXP_PRIMARY_COMPANY"), nullptr, 10) && t->unitnumber == std::strtoul(std::getenv("PXP_PRIMARY_UNIT"), nullptr, 10)) subject = t;
 assert(subject != nullptr && subject->orders != nullptr);
 unsigned active = 0; for (const auto &g : _grfconfig) if (g->status == GRFStatus::Activated) active++;
 fprintf(log, "RESOURCES grfs=%zu active=%u first=%u primary=%u previous=%u frontwagon=%u\n", _grfconfig.size(), active, subject->First()->index.base(), subject->index.base(), subject->Previous() ? subject->Previous()->index.base() : UINT_MAX, subject->IsFrontWagon());
 if (const char *count = std::getenv("PXP_PRIMARY_GRFS")) assert(active == std::strtoul(count, nullptr, 10));
 const StationID station{static_cast<uint16_t>(std::strtoul(std::getenv("PXP_PRIMARY_STATION"), nullptr, 10))};
 ShowDeparturesWindow(station);
 auto *w = static_cast<DeparturesWindow *>(FindWindowById(WindowClass::DepartureBoard, station.base())); assert(w);
 AutoRestoreBackup limit(_settings_client.gui.max_departures, _settings_client.gui.max_departures);
 const auto random = _random;
 for (auto mode : {DSM_LIVE, DSM_SCHEDULE_24H}) for (uint cap : {20U, 200U}) {
  _settings_client.gui.max_departures = cap;
  w->source_mode = mode; w->mode = DM_DEPARTURES; w->cargo_mode = DCF_ALL_CARGOES;
  w->show_types[VehicleType::Train] = true; w->show_empty = true;
  w->calc_tick_countdown = 0; w->vehicles_invalid = true; w->OnGameTick();
  uint listed = 0, rows = 0;
  for (const Vehicle *v : w->vehicles) if (v == subject) listed++;
  for (const auto &d : w->departures) if (d->vehicle == subject) {
   rows++; fprintf(log, "ROW mode=%u cap=%u tick=%lld service=%s\n", uint(mode), cap, (long long)d->scheduled_tick.base(), GetDepartureBoardServiceNumber(*d, mode).c_str());
  }
  fprintf(log, "BOARD mode=%u cap=%u listed=%u subject_rows=%u total_rows=%zu\n", uint(mode), cap, listed, rows, w->departures.size()); fflush(log);
  assert(listed == 1);
  /* A busy live board may legitimately fill its first 20 rows before this
   * service. Enlarging only the UI horizon must expose the actual service. */
  if (mode == DSM_SCHEDULE_24H || cap == 200) assert(rows > 0);
  AutoRestoreBackup dpi(_cur_dpi, &_screen); w->OnPaint();
 }
 assert(random.state[0] == _random.state[0] && random.state[1] == _random.state[1]);
 if (!reload) {
  AutoRestoreBackup pause(_pause_mode, _pause_mode); _pause_mode.Set(PauseMode::Normal);
  assert(SaveOrLoad(path + "/board-primary.sav", SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::None, false) == SaveLoadResult::Ok);
 }
 fprintf(log, "STRICT PASS real UI lists logical primary once; timetable rows and expanded live rows rendered; RNG unchanged; reload=%u\n", reload); fclose(log); _exit_game = true;
}
'''
for name in ['openttd', 'departures_gui']:
    source_path = a.gui_source if name == 'departures_gui' and a.gui_source else r / f'src/{name}.cpp'
    source = source_path.read_text(encoding='utf8')
    if name == 'openttd':
        source = source.replace('void StateGameLoop()\n{', 'void DeparturePrimaryProbe();\nvoid StateGameLoop()\n{\n DeparturePrimaryProbe();', 1)
    else:
        source = source.replace('protected:', 'public:', 1) + '\n' + probe
    cpp, obj = out / (name + '.cpp'), out / (name + '.obj')
    cpp.write_text(source, encoding='utf8')
    command = next(c['command'] for c in commands if c['file'].replace('\\', '/').endswith('/src/' + name + '.cpp'))
    compiler = command[:command.index(' -D')].strip().strip('"')
    flags = command[:command.index(' -o ')]
    rsp = out / (name + '.rsp')
    rsp.write_text(flags[flags.index(' -D'):] + f' -I"{r.as_posix()}/src" -o "{obj.as_posix()}" -c "{cpp.as_posix()}"')
    subprocess.run([compiler, '@' + str(rsp)], cwd=b, check=True)
    objects[f'CMakeFiles/openttd_lib.dir/src/{name}.cpp.obj'] = obj
section = next(s for s in (b / 'build.ninja').read_text().split('\n\n') if s.startswith('build runtime/openttd.exe:'))
linked = section.splitlines()[0].split(': ', 1)[1].split(' ', 1)[1].split(' |')[0]
for old, obj in objects.items(): linked = linked.replace(old, '"' + obj.as_posix() + '"')
rsp = out / 'link.rsp'
rsp.write_text(linked + ' ' + re.search(r'^  LINK_LIBRARIES = (.*)$', section, re.M)[1])
exe = out / 'departure-primary.exe'
subprocess.run([compiler, '-O3', '-mwindows', '-fstack-protector', '@' + str(rsp), '-o', str(exe)], cwd=b, check=True)
for reload in [False, True]:
    env = {k: v for k, v in os.environ.items() if not k.startswith('PXP_')}
    env.update(PXP_PRIMARY_OUTPUT=str(out), PXP_PRIMARY_COMPANY=str(a.company), PXP_PRIMARY_UNIT=str(a.unit), PXP_PRIMARY_STATION=str(a.station))
    if a.expect_grfs is not None: env['PXP_PRIMARY_GRFS'] = str(a.expect_grfs)
    if reload: env['PXP_PRIMARY_RELOAD'] = '1'
    tag = 'reload' if reload else 'fresh'
    save = out / 'board-primary.sav' if reload else a.save.resolve()
    with (out / (tag + '-stderr.log')).open('w') as f:
        result = subprocess.run([str(exe), '-v', 'null:until_exit', '-s', 'null', '-m', 'null', '-x', '-X', '-c', str(a.config.resolve()), '-g', str(save)], cwd=out, env=env, stdout=f, stderr=f, timeout=120)
    assert result.returncode == 0, (tag, result.returncode)
    assert 'STRICT PASS' in (out / (tag + '.log')).read_text(encoding='utf8')
print('PASS fresh and reload: real departure UI, live and 24-hour modes')
