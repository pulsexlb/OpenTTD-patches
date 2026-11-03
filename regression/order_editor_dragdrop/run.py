"""Real native order-editor click/drop regression on Windows/Ninja builds.

Loads a stock save, opens the actual native window and sends its real callbacks.
An optional original crash save tests standalone schedule 8116 verbatim.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

STOCK_CASES = ["waypoint-click", "label-click", "empty-end", "end-marker", "move-distinct",
               "delete-selection", "vehicle-click", "vehicle-label", "foreign-readonly"]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--baseset", type=Path, required=True)
    parser.add_argument("--stock-save", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--player-save", type=Path)
    parser.add_argument("--player-config", type=Path)
    parser.add_argument("--modes", help="Comma-separated subset; default all stock cases plus supplied player save")
    parser.add_argument("--before-objects", type=Path, help="An earlier frozen production objects directory for negative reproduction")
    parser.add_argument("--expect-crash", action="store_true", help="Require matching null+0x220 crashes; never count as fixed-code acceptance")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    repo = here.parents[1]
    build, output = args.build_dir.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    commands = json.loads((build / "compile_commands.json").read_text(encoding="utf8"))
    command = next(item["command"] for item in commands if item["file"].replace("\\", "/").endswith("/src/openttd.cpp"))
    compiler = command[:command.index(" -D")].strip().strip('"')
    source = (repo / "src/openttd.cpp").read_text(encoding="utf8")
    source = source.replace('#include "safeguards.h"', 'void OrderEditorDragDropTick();\n#include "safeguards.h"', 1)
    source = source.replace("void StateGameLoop()\n{", "void StateGameLoop()\n{\n OrderEditorDragDropTick();", 1)
    source += f'\n#include "{here.as_posix()}/native.inc"\n'
    cpp, obj = output / "entry.cpp", output / "entry.obj"
    cpp.write_text(source, encoding="utf8")
    flags = command[:command.index(" -o ")]
    rsp = output / "compile.rsp"
    rsp.write_text(flags[flags.index(" -D"):] + f' -I"{repo.as_posix()}/src" -o "{obj.as_posix()}" -c "{cpp.as_posix()}"', encoding="utf8")
    subprocess.run([compiler, "@" + str(rsp)], cwd=build, check=True)
    section = next(part for part in (build / "build.ninja").read_text(encoding="utf8").split("\n\n") if part.startswith("build runtime/openttd.exe:"))
    objects = section.splitlines()[0].split(": ", 1)[1].split(" ", 1)[1].split(" |")[0]
    frozen, hashes = [], {}
    for item in objects.split():
        path = Path(item)
        if path.as_posix() == "CMakeFiles/openttd_lib.dir/src/openttd.cpp.obj":
            frozen.append('"' + obj.as_posix() + '"')
        elif path.name.endswith(".obj"):
            original = args.before_objects.resolve() / "objects" / path if args.before_objects else build / path
            target = output / "objects" / path
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(original, target)
            hashes[path.as_posix()] = digest(target)
            frozen.append('"' + target.as_posix() + '"')
        else:
            frozen.append(item)
    manifest = output / "object-hashes.json"
    manifest.write_text(json.dumps(hashes, indent=2, sort_keys=True), encoding="utf8")
    libraries = re.search(r"^  LINK_LIBRARIES = (.*)$", section, re.M).group(1)
    rsp = output / "link.rsp"
    rsp.write_text(" ".join(frozen) + " " + libraries, encoding="utf8")
    exe = output / "order-editor-ui.exe"
    subprocess.run([compiler, "-O3", "-mwindows", "-fstack-protector", "@" + str(rsp), "-o", str(exe)], cwd=build, check=True)
    shutil.copytree(args.baseset.resolve(), output / "baseset", dirs_exist_ok=True)
    shutil.copytree(build / "lang", output / "lang", dirs_exist_ok=True)
    config = output / "stock.cfg"
    shutil.copy2(here.parent / "automatic_yield/regression.cfg", config)
    identity = {"observer_sha256": digest(exe), "observer_cpp_sha256": digest(cpp),
                "observer_inc_sha256": digest(here / "native.inc"), "production_object_manifest_sha256": digest(manifest),
                "source_gui_object_sha256": hashes["CMakeFiles/openttd_lib.dir/src/order_gui.cpp.obj"],
                "negative_object_override": str(args.before_objects) if args.before_objects else None,
                "stock_save_sha256": digest(args.stock_save), "player_save_sha256": digest(args.player_save) if args.player_save else None}
    (output / "identity.json").write_text(json.dumps(identity, indent=2), encoding="utf8")
    modes = args.modes.split(",") if args.modes else STOCK_CASES + (["player-waypoint"] if args.player_save else [])
    env_base = {key: value for key, value in os.environ.items() if not key.startswith("PXP_")}
    results = []
    for mode in modes:
        if mode not in STOCK_CASES + ["player-waypoint"]:
            raise SystemExit("Unknown native UI case: " + mode)
        folder = output / mode
        folder.mkdir(exist_ok=True)
        player = mode == "player-waypoint"
        if player and (not args.player_save or not args.player_config):
            raise SystemExit("player-waypoint requires --player-save and --player-config")
        save = args.player_save.resolve() if player else args.stock_save.resolve()
        before = digest(save)
        env = dict(env_base, PXP_UI_MODE=mode, PXP_UI_LOG=str(folder / "events.log"))
        argv = [str(exe), "-v", "null:until_exit", "-s", "null", "-m", "null", "-x"]
        if player:
            argv += ["-c", str(args.player_config.resolve()), "-g", str(save)]
            cwd = args.player_config.resolve().parent
        else:
            argv += ["-X", "-Q", "-c", str(config), "-g", str(save)]
            cwd = output
        with (folder / "stderr.log").open("w", encoding="utf8") as stream:
            run = subprocess.run(argv, cwd=cwd, env=env, stdout=stream, stderr=stream,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), timeout=180)
        events = (folder / "events.log").read_text(encoding="utf8").splitlines()
        last = events[-1] if events else "NO OBSERVER EVENT (inspect stderr/crash log)"
        after = digest(save)
        matched_crash = run.returncode == 201 and "EXCEPTION code=C0000005" in last and "fault=0000000000000220" in last
        accepted = (matched_crash if args.expect_crash else run.returncode == 0 and last.startswith("STRICT PASS")) and before == after
        result = {"mode": mode, "returncode": run.returncode, "accepted": accepted,
                  "negative_reproduction": args.expect_crash, "last_event": last, "input_sha256_before": before, "input_sha256_after": after}
        results.append(result)
        (output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf8")
        print(mode, "MATCHING CRASH" if args.expect_crash and accepted else "PASS" if accepted else "FAIL", last, flush=True)
    if not all(result["accepted"] for result in results):
        raise SystemExit("Native UI regression did not meet its stated acceptance; see results.json")


if __name__ == "__main__":
    main()
