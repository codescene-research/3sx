"""Test program for Mull: replay a fixed suite of forced fights and compare each
frame's rollback-state hash with a reference recorded from the unmutated build.

  mull_oracle.py record EXE REFDIR SUITE FRAMES   # once per version, no mutant active
  mull_oracle.py check  EXE REFDIR SUITE FRAMES   # what Mull runs for every mutant

check exits 0 when every fight matches its reference frame for frame (mutant
survived) and 1 on the first difference, a crash, a short trace or a hang
(mutant killed). Fights run one after another so a kill stops the run early,
and each trace is compared while it is being written, so a divergence at frame
200 does not wait for frame 3600. Mull's mutant switch is an environment
variable, which every child process here inherits.
"""

import csv
import os
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

STUDY = Path(__file__).resolve().parent.parent
NOSYNC = str(STUDY / "toolchain" / "libnosync.so")
FIGHT_TIMEOUT = 240  # seconds; an unmutated fight takes about 50
POLL = 0.5


def read_suite(path):
    with open(path, newline="") as stream:
        rows = [r for r in csv.DictReader(stream) if r["seed"].strip() and not r["seed"].startswith("#")]
    return [(int(r["seed"]), r["p1"].strip(), r["p2"].strip(), r["stage"].strip()) for r in rows]


def command(exe, fight, frames, out):
    seed, p1, p2, stage = fight
    cmd = [exe, "--stress", "--stress-seed", str(seed), "--stress-frames", str(frames),
           "--stress-out", str(out), "--no-sound", "--stress-motions"]
    for flag, value in (("--stress-p1", p1), ("--stress-p2", p2), ("--stress-stage", stage)):
        if value:
            cmd += [flag, value]
    return cmd


def environment(profile_file):
    env = dict(os.environ)
    env["SDL_VIDEO_DRIVER"] = "dummy"
    env["LD_PRELOAD"] = NOSYNC
    env["LLVM_PROFILE_FILE"] = profile_file
    return env


def record(exe, refdir, suite, frames):
    refdir.mkdir(parents=True, exist_ok=True)

    def one(fight):
        out = refdir / f"fight-{fight[0]}"
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir()
        env = environment(str(refdir / "profiles" / f"fight-{fight[0]}-%p.profraw"))
        result = subprocess.run(command(exe, fight, frames, out), cwd=Path(exe).parent, env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=FIGHT_TIMEOUT * 4)
        rows = (out / "state-trace.csv").read_text().splitlines()
        if result.returncode != 0 or len(rows) != frames + 1:
            raise SystemExit(f"reference fight {fight} failed: exit {result.returncode}, {len(rows) - 1} frames")
        return fight[0]

    (refdir / "profiles").mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(len(suite)) as pool:
        for seed in pool.map(one, suite):
            print(f"recorded fight {seed}")


def check_fight(exe, refdir, fight, frames, work, make_env=None):
    make_env = make_env or environment
    reference = (refdir / f"fight-{fight[0]}" / "state-trace.csv").read_text().splitlines()
    out = work / f"fight-{fight[0]}"
    out.mkdir()
    trace = out / "state-trace.csv"
    process = subprocess.Popen(command(exe, fight, frames, out), cwd=Path(exe).parent,
                               env=make_env("/dev/null"),
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    started = time.monotonic()
    checked = 0

    def compare_new_rows():
        nonlocal checked
        if not trace.exists():
            return True
        rows = trace.read_text().splitlines()
        # The last line may still be half written while the game runs.
        complete = rows if process.poll() is not None else rows[:-1]
        for i in range(checked, len(complete)):
            if i >= len(reference) or complete[i] != reference[i]:
                return False
        checked = max(checked, len(complete))
        return True

    try:
        while process.poll() is None:
            if not compare_new_rows():
                return f"fight {fight[0]}: diverged at row {checked}"
            if time.monotonic() - started > FIGHT_TIMEOUT:
                return f"fight {fight[0]}: timed out"
            time.sleep(POLL)

        if not compare_new_rows():
            return f"fight {fight[0]}: diverged at row {checked}"
        if process.returncode != 0:
            return f"fight {fight[0]}: exit status {process.returncode}"
        if checked != len(reference):
            return f"fight {fight[0]}: trace ended after {checked - 1} of {len(reference) - 1} frames"
        return None
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def check(exe, refdir, suite, frames):
    work = Path(tempfile.mkdtemp(prefix="mull-oracle-"))
    try:
        for fight in suite:
            failure = check_fight(exe, refdir, fight, frames, work)
            if failure:
                print("KILLED " + failure)
                return 1
        print("SURVIVED")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    mode, exe, refdir, suite, frames = sys.argv[1:6]
    exe = str(Path(exe).resolve())
    if mode == "record":
        record(exe, Path(refdir), read_suite(suite), int(frames))
        return 0
    return check(exe, Path(refdir), read_suite(suite), int(frames))


if __name__ == "__main__":
    sys.exit(main())
