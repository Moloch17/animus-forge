#!/usr/bin/env python3
"""Train several classes' fighting stages at once, each in its own training server.

    tools/forge_classes.py run druid mage warrior [...]     train these classes, ANIMUS_FORGE_PARALLEL (2) at a time
    tools/forge_classes.py status                           which instances exist, are up, and how far their queue is
    ANIMUS_FORGE_INSTANCE=druid tools/forge_classes.py attach|stop|logs

Run from the AzerothCore checkout root (where docker-compose.yml is). Nothing in the sim or the learner is shared
between two runs but the cores, so a second class is a second `ac-worldserver` container: the same image, source
tree, built worldserver, database and GPU, its own output directory (`var/animus-forge/<class>`), its own
`AnimusForge.Classes` and `AnimusForge.Queue`, and its own share of the map-update and torch threads. Compose
cannot make services on the fly, so each instance is an override file, `env/instances/<class>.yml`, written from
the template below and merged over docker-compose.yml with `-f`. Any AnimusForge.* or worldserver.conf key can be
set there as an `AC_` environment variable (AnimusForge.Queue -> AC_ANIMUS_FORGE_QUEUE), which beats the conf file.

Why two and not three: a worldserver starts at ~3.5 GB, and one long `forge bench` sweep climbed to 19.6 GB
(docs/manual/07-operations.md, "Performance"); 30 GB holds two of those, not three. ANIMUS_FORGE_PARALLEL=3 is a
choice, not a default.

A class is done when the last stage of its queue has written finished.json; the instance is then stopped and the
next class starts. Ctrl+C leaves the running instances up (they keep training); `run` again resumes the schedule,
skipping classes whose queue is finished.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

CLASSES = ("warrior", "paladin", "hunter", "rogue", "priest", "deathknight", "shaman", "mage", "warlock", "druid")
# A class's fighting line: stage8_duel through stage19_triage (docs/manual/04-curriculum.md, "Training one class at
# a time"); the movement root before it is trained once for every class, in the shared instance.
CLASS_QUEUE = ("stage8_duel", "stage9_pack", "stage10_gauntlet", "stage11_endurance", "stage12_pvp", "stage13_evade",
               "stage14_hide", "stage15_stealth", "stage16_companion", "stage17_party", "stage18_tanking",
               "stage19_triage")
OUTPUT_ROOT = "/azerothcore/var/animus-forge"       # inside the container; var/animus-forge on the host
INSTANCES = Path("env/instances")
COMPOSE = Path("docker-compose.yml")
# Host ports for an instance's TensorBoard and dashboard: the base service has 16006/18800; instance i takes +10*i.
PORT_BASE = 16006, 18800
PORT_STEP = 10
POLL_SECONDS = 60

TEMPLATE = """# Written by modules/mod-animus-forge/tools/forge_classes.py: one class's fighting stages in a training server
# of its own beside ac-worldserver. Start it with
#   docker compose -f docker-compose.yml -f {file} up -d {service}
# or through the tool (ANIMUS_FORGE_INSTANCE={name} tools/forge_classes.py attach|stop). Edit freely; the tool
# rewrites it only when the file is missing.
services:
  {service}:
    extends:
      file: docker-compose.yml
      service: ac-worldserver
    container_name: ac-animus-forge-worldserver-{name}
    environment:
      AC_ANIMUS_FORGE_OUTPUT_DIR: {output}
      AC_ANIMUS_FORGE_CLASSES: "{classes}"
      AC_ANIMUS_FORGE_QUEUE: "{queue}"
      # The cores are shared between the instances: each gets a share of the map-update and torch threads.
      AC_MAP_UPDATE_THREADS: "{map_threads}"
      AC_ANIMUS_FORGE_LEARNER_TORCH_THREADS: "{torch_threads}"
    ports:
      - "127.0.0.1:{tensorboard}:6006"
      - "127.0.0.1:{dashboard}:8800"
"""


def service(name: str) -> str:
    return f"ac-worldserver-{name}"


def instance_file(name: str) -> Path:
    return INSTANCES / f"{name}.yml"


def compose(name: str, *args: str, check: bool = True, capture: bool = False) -> subprocess.CompletedProcess:
    command = ["docker", "compose", "-f", str(COMPOSE), "-f", str(instance_file(name)), *args]
    return subprocess.run(command, check=check, capture_output=capture, text=True)


def write_instance(name: str, classes: str, queue: tuple[str, ...], index: int, parallel: int) -> Path:
    """The override file for an instance, unless one exists (a hand edit is kept)."""
    path = instance_file(name)
    if path.exists():
        return path
    cores = os.cpu_count() or 32
    # Two instances on 32 cores: 12 map threads and 4 torch threads each; scaled with the core count and the
    # number that run at once. forge bench at that setting says whether the split is right.
    map_threads = max(2, (cores * 3 // 4) // parallel)
    torch_threads = max(1, (cores // 4) // parallel)
    INSTANCES.mkdir(parents=True, exist_ok=True)
    path.write_text(TEMPLATE.format(
        file=path, service=service(name), name=name, output=f"{OUTPUT_ROOT}/{name}", classes=classes,
        queue=", ".join(queue), map_threads=map_threads, torch_threads=torch_threads,
        tensorboard=PORT_BASE[0] + PORT_STEP * (index + 1), dashboard=PORT_BASE[1] + PORT_STEP * (index + 1)))
    return path


def is_up(name: str) -> bool:
    result = compose(name, "ps", "-q", "--status", "running", service(name), check=False, capture=True)
    return bool(result.stdout.strip())


def finished(name: str, queue: tuple[str, ...]) -> bool:
    return (Path("var/animus-forge") / name / "runs" / queue[-1] / "finished.json").exists()


def progress(name: str, queue: tuple[str, ...]) -> str:
    done = [stage for stage in queue if (Path("var/animus-forge") / name / "runs" / stage / "finished.json").exists()]
    return f"{len(done)}/{len(queue)} stages finished" + (f" (last {done[-1]})" if done else "")


def run(classes: list[str], parallel: int) -> int:
    unknown = [c for c in classes if c not in CLASSES]
    if unknown:
        print(f"not a class: {', '.join(unknown)} (one of {', '.join(CLASSES)})", file=sys.stderr)
        return 2
    if not COMPOSE.exists():
        print("run this from the AzerothCore checkout root (docker-compose.yml)", file=sys.stderr)
        return 2

    pending = [c for c in classes if not finished(c, CLASS_QUEUE)]
    for c in classes:
        if c not in pending:
            print(f"{c}: already finished ({progress(c, CLASS_QUEUE)}); skipping")
    running: list[str] = [c for c in pending if is_up(c)]
    pending = [c for c in pending if c not in running]
    for c in running:
        print(f"{c}: already up; watching it")

    try:
        while pending or running:
            while pending and len(running) < parallel:
                name = pending.pop(0)
                path = write_instance(name, name, CLASS_QUEUE, index=classes.index(name), parallel=parallel)
                print(f"{name}: starting {service(name)} from {path}")
                compose(name, "up", "-d", service(name))
                running.append(name)
                # The first start of an instance creates nothing (the venv and the build are shared), but two
                # starting in the same second race for the config-file copy: a moment apart is enough.
                time.sleep(5)

            time.sleep(POLL_SECONDS)
            for name in list(running):
                if finished(name, CLASS_QUEUE):
                    print(f"{name}: queue finished; stopping {service(name)}")
                    compose(name, "stop", service(name), check=False)
                    running.remove(name)
                elif not is_up(name):
                    print(f"{name}: {service(name)} is not running ({progress(name, CLASS_QUEUE)}); "
                          f"see `ANIMUS_FORGE_INSTANCE={name} tools/forge_classes.py logs`", file=sys.stderr)
                    running.remove(name)
    except KeyboardInterrupt:
        print(f"\nleaving {', '.join(running) or 'nothing'} running; `run` again resumes the schedule")
        return 130
    print("every class is finished")
    return 0


def status() -> int:
    if not INSTANCES.exists():
        print("no instances (env/instances/ is empty)")
        return 0
    for path in sorted(INSTANCES.glob("*.yml")):
        name = path.stem
        up = "up" if is_up(name) else "down"
        print(f"{name}: {up}, {progress(name, CLASS_QUEUE)}, {path}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p_run = sub.add_parser("run", help="train these classes, ANIMUS_FORGE_PARALLEL at a time")
    p_run.add_argument("classes", nargs="+")
    p_run.add_argument("--parallel", type=int, default=int(os.environ.get("ANIMUS_FORGE_PARALLEL", "2")))
    sub.add_parser("status")
    for name in ("attach", "stop", "logs"):
        sub.add_parser(name, help=f"docker compose {name} of the instance named by ANIMUS_FORGE_INSTANCE")
    args = parser.parse_args(argv)

    if shutil.which("docker") is None:
        print("docker is not on PATH", file=sys.stderr)
        return 2
    if args.command == "run":
        return run(args.classes, max(1, args.parallel))
    if args.command == "status":
        return status()

    name = os.environ.get("ANIMUS_FORGE_INSTANCE", "")
    if not name or not instance_file(name).exists():
        print("set ANIMUS_FORGE_INSTANCE to an instance with a file in env/instances/", file=sys.stderr)
        return 2
    if args.command == "attach":
        print("Detach with Ctrl+P Ctrl+Q; Ctrl+C stops that server.")
        os.execvp("docker", ["docker", "compose", "-f", str(COMPOSE), "-f", str(instance_file(name)), "attach",
                             service(name)])
    if args.command == "stop":
        return compose(name, "stop", service(name), check=False).returncode
    return compose(name, "logs", "--tail", "200", service(name), check=False).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
