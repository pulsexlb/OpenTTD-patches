"""Native Windows/Ninja driving regressions; does not replace the production executable."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--baseset", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--modes", help="Comma-separated scenarios (default: all)")
    parser.add_argument("--braking", type=int, choices=[0, 1], default=1)
    parser.add_argument("--debug-yapf", action="store_true", help="Capture native debug output for stream scenarios")
    parser.add_argument("--overtaking-source", type=Path, help="Compile a saved production implementation for before/after reproduction")
    parser.add_argument("--newgrf-config", type=Path, help="Optional installed real NewGRF catalogue for exact-speed grf-* scenarios")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    repo = here.parents[1]
    build = args.build_dir.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    revision = (build / "generated/rev.cpp").read_text(encoding="utf-8")
    identity = re.search(r'_openttd_revision_simulation_hash\[\] = "([^"]*)"', revision).group(1)
    build_identity = {"simulation_hash": identity, "braking": args.braking}
    if args.overtaking_source:
        build_identity["production_implementation_override"] = str(args.overtaking_source.resolve())
        build_identity["override_sha256"] = hashlib.sha256(args.overtaking_source.read_bytes()).hexdigest()
    (output / "build-identity.json").write_text(json.dumps(build_identity, indent=2), encoding="utf-8")
    commands = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    command = next(c["command"] for c in commands if c["file"].replace("\\", "/").endswith("/src/openttd.cpp"))
    compiler = command[:command.index(" -D")].strip().strip('"')
    source = (repo / "src/openttd.cpp").read_text(encoding="utf-8")
    source = source.replace('#include "safeguards.h"', f'#include "{here.as_posix()}/native.h"\n#include "safeguards.h"', 1)
    source = source.replace("void StateGameLoop()\n{", "void StateGameLoop()\n{\n AutoYieldDemoTick();", 1)
    source += f'\n#include "{here.as_posix()}/native.inc"\n'
    cpp = output / "driver.cpp"
    cpp.write_bytes(source.encode("utf-8"))
    obj = output / "driver.obj"
    rsp = output / "compile.rsp"
    flags = command[:command.index(" -o ")]
    rsp.write_text(flags[flags.index(" -D"):] + f' -I"{repo.as_posix()}/src" -o "{obj.as_posix()}" -c "{cpp.as_posix()}"', encoding="utf-8")
    subprocess.run([compiler, "@" + str(rsp)], cwd=build, check=True)
    section = next(s for s in (build / "build.ninja").read_text(encoding="utf-8").split("\n\n") if s.startswith("build runtime/openttd.exe:"))
    objects = section.splitlines()[0].split(": ", 1)[1].split(" ", 1)[1].split(" |")[0]
    objects = objects.replace("CMakeFiles/openttd_lib.dir/src/openttd.cpp.obj", f'"{obj.as_posix()}"')
    if args.overtaking_source:
        original = next(c["command"] for c in commands if c["file"].replace("\\", "/").endswith("/src/tracerestrict_overtaking.cpp"))
        before_obj = output / "overtaking.obj"
        before_rsp = output / "overtaking.rsp"
        before_flags = original[:original.index(" -o ")]
        before_rsp.write_text(before_flags[before_flags.index(" -D"):] + f' -I"{repo.as_posix()}/src" -o "{before_obj.as_posix()}" -c "{args.overtaking_source.resolve().as_posix()}"', encoding="utf-8")
        subprocess.run([compiler, "@" + str(before_rsp)], cwd=build, check=True)
        objects = objects.replace("CMakeFiles/openttd_lib.dir/src/tracerestrict_overtaking.cpp.obj", f'"{before_obj.as_posix()}"')
    libraries = re.search(r"^  LINK_LIBRARIES = (.*)$", section, re.M).group(1)
    rsp = output / "link.rsp"
    rsp.write_text(objects + " " + libraries, encoding="utf-8")
    exe = output / "yield-regression.exe"
    subprocess.run([compiler, "-O3", "-mwindows", "-fstack-protector", "@" + str(rsp), "-o", str(exe)], cwd=build, check=True)
    shutil.copytree(args.baseset, output / "baseset", dirs_exist_ok=True)
    shutil.copytree(build / "lang", output / "lang", dirs_exist_ok=True)
    shutil.copy2(here / "regression.cfg", output / "regression.cfg")
    config = (output / "regression.cfg").read_text(encoding="utf-8")
    if args.newgrf_config:
        config = config.replace("[newgrf]\n", "").replace("starting_year = 2000", "starting_year = 2137")
        grf_lines = []
        for line in args.newgrf_config.read_text(encoding="utf-8").splitlines():
            if " = " in line and not line.lstrip().startswith((";", "#")):
                key, value = line.split(" = ", 1)
                prefix, _, filename = key.rpartition("|")
                source_file = Path(filename or key)
                if source_file.is_absolute() and source_file.is_file():
                    local_grf = output / "newgrf" / source_file.name
                    local_grf.parent.mkdir(exist_ok=True)
                    shutil.copy2(source_file, local_grf)
                    key = (prefix + "|" if prefix else "") + source_file.name
                    line = key + " = " + value
            grf_lines.append(line)
        config += "\n" + "\n".join(grf_lines) + "\n"
    (output / "regression.cfg").write_text(config.replace("[vehicle]", "[vehicle]\ntrain_braking_model = " + str(args.braking)), encoding="utf-8")
    environment = {k: v for k, v in os.environ.items() if not k.startswith("PXP_")}
    results = []
    modes = args.modes.split(",") if args.modes else ["basic", "red", "unknown", "front", "tail", "two", "delete", "cancel", "different", "reroute", "crossing", "crossing-foreign", "turnover-far", "turnover-near", "stream", "stream-serial", "stream-reroute", "station-same", "station-merge", "station-merge-split", "station-different", "station-priority", "far-basic", "far-two", "far-empty-first", "normal-free-first", "layout-add", "layout-switch", "layout-legal-old", "stock-full-fastest", "stock-full-closest", "stock-full-near", "stock-full-long-block", "stock-full-long-train"]
    if not args.modes:
        modes += ["layout-new-first", "layout-new-second", "layout-new-third", "layout-new-platform", "far-multi-outlet", "far-near-wrong-outlet", "topology-merge2", "topology-merge3", "topology-merge3-signals", "topology-split", "topology-rejoin", "occupied-second", "occupied-third", "occupied-platform"]
        modes += ["mixed-p1o1-pass", "mixed-p1o1-stop", "mixed-p2o1-pass", "mixed-p2o1-stop", "mixed-p1o2-pass", "mixed-p1o2-stop", "mixed-p3o2-pass", "mixed-p3o2-stop", "mixed-p3o2-signals", "mixed-shared", "mixed-independent", "mixed-rejoin", "mixed-add", "mixed-full", "mixed-outlet-explicit", "mixed-outlet-conditional", "mixed-outlet-single-clear", "mixed-outlet-change", "mixed-outlet-blocked"]
        modes += ["mixed-independent-stops", "mixed-rejoin-stop", "mixed-rejoin-both-stop"]
    for mode in modes:
        if mode.startswith("grf-") and mode not in {"grf-basic", "grf-two", "grf-200", "grf-160-250", "grf-consecutive"}:
            raise SystemExit("Unknown exact-speed scenario: " + mode)
        for style in ["exit", "explicit"]:
            folder = output / (mode + "-" + style)
            folder.mkdir(exist_ok=True)
            for reload in ([False, True] if mode.startswith(("layout-new-", "grf-", "topology-", "occupied-", "mixed-")) or mode in ["basic", "red", "unknown", "two", "stream", "stream-serial", "stream-reroute", "station-same", "station-merge", "station-merge-split", "station-different", "station-priority", "far-basic", "far-two", "far-empty-first", "far-multi-outlet", "far-near-wrong-outlet", "normal-free-first", "layout-add", "layout-switch", "layout-legal-old", "stock-full-fastest", "stock-full-closest", "stock-full-long-train"] else [False]):
                env = dict(environment, PXP_STRICT=mode, PXP_DEMO_OUTPUT=str(folder))
                if mode.startswith("grf-"):
                    if not args.newgrf_config:
                        raise SystemExit("grf-* scenarios require --newgrf-config with installed real vehicle GRFs")
                    env["PXP_GRF_EXACT"] = "1"
                if args.debug_yapf:
                    env["PXP_STRICT_DEBUG"] = "1"
                    env["PXP_GRF_CATALOG"] = "1"
                if style == "exit":
                    env["PXP_DEMO_EXIT_ONLY"] = "1"
                case_config = output / "regression.cfg"
                if mode.startswith(("far-", "layout-", "stock-full-", "topology-", "occupied-", "mixed-")):
                    case_config = folder / "regression.cfg"
                    case_config.write_text((output / "regression.cfg").read_text().replace("map_x = 8", "map_x = " + ("11" if mode=="far-empty-first" else "9")), encoding="utf-8")
                argv = [str(exe), "-v", "null:until_exit", "-s", "null", "-m", "null", "-x", "-c", str(case_config)]
                if not args.newgrf_config:
                    argv += ["-X", "-Q"]
                if reload:
                    env["PXP_STRICT_RELOAD"] = "1"
                    argv += ["-g", str(folder / "strict-waiting.sav")]
                else:
                    argv += ["-e"]
                with (folder / ("reload-stderr.log" if reload else "stderr.log")).open("w", encoding="utf-8") as stream:
                    run = subprocess.run(argv, cwd=output, env=env, stdout=stream, stderr=stream, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), timeout=90)
                log = folder / ("reload-strict.log" if reload else "run.log")
                last = log.read_text(encoding="utf-8").splitlines()[-1]
                results.append({"mode": mode, "style": style, "reload": reload, "returncode": run.returncode, "result": last})
                (output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
                print(mode, style, "reload" if reload else "new", last, flush=True)
                if run.returncode or not last.startswith("STRICT PASS"):
                    raise SystemExit("Native driving regression failed: " + str(log))


if __name__ == "__main__":
    main()
