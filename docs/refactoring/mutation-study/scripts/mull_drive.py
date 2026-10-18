"""Run Mull mutants through mull_oracle, both study versions in one queue.

  mull_drive.py RESULTS.jsonl WORKERS [LIMIT]

Mutants come from Mull's own coverage dry run (only mutants on lines the suite
executes), are switched on the way Mull does it - an environment variable named
after the mutant id, set to 1 - and every verdict is appended to RESULTS.jsonl
the moment it is known. Restarting skips mutants already in the file.

The queue is the two versions' mutants shuffled together with a fixed seed, so
whenever the run is stopped, what has finished is a uniform random sample of
each version's covered mutants, taken at the same pace for both.
"""

import json
import os
import random
import signal
import sqlite3
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
STUDY = HERE.parent
sys.path.insert(0, str(HERE))
import mull_oracle  # noqa: E402

VERSIONS = {
    name: dict(exe=str(STUDY / name / "bm" / "3sx"), refdir=STUDY / f"ref-{name}",
               covered=STUDY / "mrep" / f"{name}-covdry.sqlite")
    for name in ("before", "after")
}
SUITE = mull_oracle.read_suite(STUDY / "suite.csv")

# Set when the driver is told to stop. The games share its process group, so the
# same signal kills them mid-fight; a verdict that comes back after this is the
# signal's doing, not the mutant's, and must not be recorded.
STOPPING = threading.Event()
FRAMES = 3600


def covered_mutants(name):
    db = sqlite3.connect(VERSIONS[name]["covered"])
    return [(name, r[0]) for r in db.execute("select mutant_id from mutant order by mutant_id")]


def run_one(version, mutant_id, lock, results_path):
    if STOPPING.is_set():
        return None
    info = VERSIONS[version]
    started = time.time()
    failure = None
    # Inside the study folder rather than /tmp, so a /tmp cleanup cannot fake a kill.
    (STUDY / "work").mkdir(exist_ok=True)
    work = Path(mull_oracle.tempfile.mkdtemp(prefix="mutant-", dir=STUDY / "work"))
    try:
        for fight in SUITE:
            if STOPPING.is_set():
                return None
            failure = check_fight_with_mutant(info, fight, work, mutant_id)
            if failure:
                break
    finally:
        mull_oracle.shutil.rmtree(work, ignore_errors=True)

    # SIGTERM (-15) and SIGKILL (-9) come from outside - a stop, or the machine -
    # never from a mutant; the oracle reports its own hang limit as "timed out".
    interrupted = failure is not None and ("exit status -15" in failure or "exit status -9" in failure)
    if STOPPING.is_set() or interrupted:
        return None

    record = dict(version=version, mutant_id=mutant_id, status="killed" if failure else "survived",
                  reason=failure, seconds=round(time.time() - started, 1))
    with lock:
        with open(results_path, "a") as stream:
            stream.write(json.dumps(record) + "\n")
            stream.flush()
    return record


def check_fight_with_mutant(info, fight, work, mutant_id):
    # Same oracle as mull_oracle.check_fight, with the mutant's switch added to
    # the environment the game is started in.
    original = mull_oracle.environment

    def with_mutant(profile_file):
        env = original(profile_file)
        env[mutant_id] = "1"
        return env

    return mull_oracle.check_fight(info["exe"], info["refdir"], fight, FRAMES, work, with_mutant)


def main():
    results_path = Path(sys.argv[1])
    workers = int(sys.argv[2])
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else None

    queue = covered_mutants("before") + covered_mutants("after")
    random.Random(20261013).shuffle(queue)

    done = set()
    if results_path.exists():
        for line in results_path.read_text().splitlines():
            r = json.loads(line)
            done.add((r["version"], r["mutant_id"]))
    todo = [m for m in queue if m not in done]
    if limit is not None:
        todo = todo[:limit]
    print(f"{len(queue)} covered mutants, {len(done)} already done, running {len(todo)} with {workers} workers",
          flush=True)

    signal.signal(signal.SIGTERM, lambda *_: STOPPING.set())
    signal.signal(signal.SIGINT, lambda *_: STOPPING.set())

    lock = threading.Lock()
    finished = 0
    with ThreadPoolExecutor(workers) as pool:
        for record in pool.map(lambda m: run_one(m[0], m[1], lock, results_path), todo):
            if STOPPING.is_set():
                print("stopping: in-flight mutants are not recorded", flush=True)
                pool.shutdown(wait=True, cancel_futures=True)
                break
            finished += 1
            if finished % 50 == 0:
                print(f"{finished}/{len(todo)} finished", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
