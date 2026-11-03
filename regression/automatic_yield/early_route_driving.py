"""Native early-route driving: distinct outlets, actual stops and yielding events.

Links an isolated observer executable. Production sources/executables and the
baseline run.py are not modified. Each fresh/reload case writes its evidence.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

MODES = [
    "two-through", "two-front", "two-tail", "three-through", "through-upper", "stop-main", "stop-upper",
    "stop-reselect-order", "stop-conditional", "stop-side-effect", "stable-two-paths",
    "blocked-reselect", "new-siding", "wrong-outlet-siding", "multi-main", "shared-platform",
    "split-short", "split-long", "split-three", "split-change-fast", "stop-independent", "stop-split-change", "split-distant-target", "split-alternative", "split-crossing", "split-crossing-edit",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--baseset", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--modes", help="Comma-separated cases, default all")
    parser.add_argument("--styles", default="exit,explicit")
    parser.add_argument("--braking", type=int, choices=[0, 1], default=0)
    parser.add_argument("--no-reload", action="store_true")
    parser.add_argument("--run-only", action="store_true", help="Reuse this output's already-linked observer")
    parser.add_argument("--objects-from", type=Path, help="Link a previously frozen production-object snapshot")
    parser.add_argument("--debug-yapf", action="store_true", help="Record detailed native restrictions near the inlet decision")
    parser.add_argument("--keep-going", action="store_true", help="Record failed cases and continue independent scenarios")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    repo = here.parents[1]
    build, output = args.build_dir.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    exe = output / "early-route-driving.exe"
    if not args.run_only:
        commands = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
        command = next(item["command"] for item in commands if item["file"].replace("\\", "/").endswith("/src/openttd.cpp"))
        compiler = command[:command.index(" -D")].strip().strip('"')
        source = (repo / "src/openttd.cpp").read_text(encoding="utf-8")
        source = source.replace('#include "safeguards.h"', f'#include "{here.as_posix()}/native.h"\n#include "pathfinder/yapf/yapf.h"\nvoid EarlyRouteDrivingTick();\n#include "safeguards.h"', 1)
        source = source.replace("void StateGameLoop()\n{", "void StateGameLoop()\n{\n EarlyRouteDrivingTick();", 1)
        source += f'\n#include "{here.as_posix()}/native.inc"\n#include "{here.as_posix()}/early_route_driving.inc"\n'
        cpp, obj = output / "driver.cpp", output / "driver.obj"
        cpp.write_text(source, encoding="utf-8")
        flags = command[:command.index(" -o ")]
        rsp = output / "compile.rsp"
        rsp.write_text(flags[flags.index(" -D"):] + f' -I"{repo.as_posix()}/src" -o "{obj.as_posix()}" -c "{cpp.as_posix()}"', encoding="utf-8")
        subprocess.run([compiler, "@" + str(rsp)], cwd=build, check=True)
        section = next(s for s in (build / "build.ninja").read_text(encoding="utf-8").split("\n\n") if s.startswith("build runtime/openttd.exe:"))
        objects = section.splitlines()[0].split(": ", 1)[1].split(" ", 1)[1].split(" |")[0]
        # Keep this observer's production objects stable while later incremental
        # builds proceed. No source or official executable is overwritten.
        snapshot = output / "objects"
        snapshot.mkdir(exist_ok=True)
        frozen = []
        object_hashes = {}
        for item in objects.split():
            path = Path(item)
            if path.name.endswith(".obj") and path.as_posix() != "CMakeFiles/openttd_lib.dir/src/openttd.cpp.obj":
                target = snapshot / path
                target.parent.mkdir(parents=True, exist_ok=True)
                source_object = args.objects_from.resolve() / "objects" / path if args.objects_from else build / path
                shutil.copy2(source_object, target)
                object_hashes[path.as_posix()] = hashlib.sha256(target.read_bytes()).hexdigest()
                frozen.append('"' + target.as_posix() + '"')
            else:
                frozen.append(item)
        objects = " ".join(frozen)
        (output / "object-hashes.json").write_text(json.dumps(object_hashes, indent=2, sort_keys=True), encoding="utf-8")
        objects = objects.replace("CMakeFiles/openttd_lib.dir/src/openttd.cpp.obj", f'"{obj.as_posix()}"')
        libraries = re.search(r"^  LINK_LIBRARIES = (.*)$", section, re.M).group(1)
        rsp = output / "link.rsp"
        rsp.write_text(objects + " " + libraries, encoding="utf-8")
        subprocess.run([compiler, "-O3", "-mwindows", "-fstack-protector", "@" + str(rsp), "-o", str(exe)], cwd=build, check=True)
        revision = (build / "generated/rev.cpp").read_text(encoding="utf-8")
        identity = re.search(r'_openttd_revision_simulation_hash\[\] = "([^"]*)"', revision).group(1)
        if args.objects_from:
            identity = json.loads((args.objects_from.resolve() / "build-identity.json").read_text(encoding="utf-8"))["simulation_hash"]
        (output / "build-identity.json").write_text(json.dumps({"simulation_hash": identity,
            "observer_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
            "driver_sha256": hashlib.sha256(cpp.read_bytes()).hexdigest(),
            "observer_include_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(here.iterdir()) if path.suffix in {".h", ".inc"}},
            "production_object_manifest_sha256": hashlib.sha256((output / "object-hashes.json").read_bytes()).hexdigest(),
            "braking": args.braking}, indent=2), encoding="utf-8")
    shutil.copytree(args.baseset, output / "baseset", dirs_exist_ok=True)
    shutil.copytree(build / "lang", output / "lang", dirs_exist_ok=True)
    config = (here / "regression.cfg").read_text(encoding="utf-8").replace("[vehicle]", "[vehicle]\ntrain_braking_model = " + str(args.braking))
    (output / "regression.cfg").write_text(config, encoding="utf-8")
    env_base = {key: value for key, value in os.environ.items() if not key.startswith("PXP_")}
    results = []
    for mode in args.modes.split(",") if args.modes else MODES:
        if mode not in MODES:
            raise SystemExit("Unknown early-route mode: " + mode)
        for style in args.styles.split(","):
            if style not in {"exit", "explicit"}:
                raise SystemExit("Unknown marker style: " + style)
            folder = output / (mode + "-" + style)
            folder.mkdir(exist_ok=True)
            case_config = output / "regression.cfg"
            if mode == "split-distant-target":
                case_config = folder / "regression.cfg"
                case_config.write_text(config.replace("map_x = 8", "map_x = 12") + "\n[construction]\nclear_frame_burst = 65535\n", encoding="utf-8")
            for reload in [False] if args.no_reload else [False, True]:
                env = dict(env_base, PXP_STRICT="early-route-" + mode, PXP_EARLY_MODE=mode, PXP_DEMO_OUTPUT=str(folder))
                if args.debug_yapf:
                    env["PXP_EARLY_TRACE"] = "1"
                if style == "exit":
                    env["PXP_DEMO_EXIT_ONLY"] = "1"
                argv = [str(exe), "-v", "null:until_exit", "-s", "null", "-m", "null", "-x", "-X", "-Q", "-c", str(case_config)]
                if reload:
                    env["PXP_STRICT_RELOAD"] = "1"
                    argv += ["-g", str(folder / "strict-waiting.sav")]
                else:
                    argv += ["-e"]
                log = folder / ("reload-stderr.log" if reload else "stderr.log")
                with log.open("w", encoding="utf-8") as stream:
                    run = subprocess.run(argv, cwd=output, env=env, stdout=stream, stderr=stream, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), timeout=120)
                evidence = folder / ("reload-strict.log" if reload else "run.log")
                last = evidence.read_text(encoding="utf-8").splitlines()[-1]
                result = {"mode": mode, "style": style, "reload": reload, "returncode": run.returncode, "result": last}
                results.append(result)
                (output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
                print(mode, style, "reload" if reload else "fresh", last, flush=True)
                if run.returncode or not last.startswith("STRICT PASS"):
                    if not args.keep_going:
                        raise SystemExit("Early-route native driving failed: " + str(evidence))
                    break  # A failed fresh case has no trustworthy reload checkpoint.
    if any(item["returncode"] or not item["result"].startswith("STRICT PASS") for item in results):
        raise SystemExit("One or more early-route native driving scenarios failed; see results.json")


if __name__ == "__main__":
    main()
