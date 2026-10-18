"""Recompute the tables in README.md from results.jsonl.

  summarize.py RESULTS.jsonl [OPERATORS.csv]

Prints the per-version scores and kill reasons, and writes the per-operator
counts to OPERATORS.csv when given. Mutant totals (executed or not) come from
Mull's dry run and are not in the results file, so they are fixed here.
"""

import collections
import csv
import json
import math
import sys

ALL_MUTANTS = {"before": 47065, "after": 36619}
VERSIONS = ("before", "after")


def kill_reason(reason):
    if "diverged" in reason:
        return "diverged"
    if "exit status" in reason:
        return "crash"
    return "hang or short trace"


def main(results, operators_csv=None):
    status = collections.Counter()
    reasons = collections.Counter()
    operators = collections.defaultdict(collections.Counter)
    seen = set()
    for line in open(results):
        r = json.loads(line)
        key = (r["version"], r["mutant_id"])
        assert key not in seen, f"duplicate verdict for {key}"
        seen.add(key)
        status[r["version"], r["status"]] += 1
        operators[r["version"], r["mutant_id"].split(":")[0]][r["status"]] += 1
        if r["status"] == "killed":
            reasons[r["version"], kill_reason(r["reason"])] += 1

    print("version  all  executed  killed  survived  killed/executed  killed/all")
    for v in VERSIONS:
        k, s = status[v, "killed"], status[v, "survived"]
        print(f"{v:7} {ALL_MUTANTS[v]:6} {k + s:9} {k:7} {s:9} {100 * k / (k + s):15.1f}% {100 * k / ALL_MUTANTS[v]:10.1f}%")

    (k1, n1), (k2, n2) = [(status[v, "killed"], status[v, "killed"] + status[v, "survived"]) for v in VERSIONS]
    p = (k1 + k2) / (n1 + n2)
    z = (k2 / n2 - k1 / n1) / math.sqrt(p * (1 - p) * (1 / n1 + 1 / n2))
    print(f"two-proportion z, killed/executed after vs before: {z:.2f}")

    print("\nkill reasons")
    for v in VERSIONS:
        print(v, {why: reasons[v, why] for why in ("diverged", "crash", "hang or short trace")})

    if operators_csv:
        with open(operators_csv, "w", newline="") as stream:
            out = csv.writer(stream)
            out.writerow(["operator", "version", "executed", "killed", "killed_pct"])
            for v, op in sorted(operators, key=lambda vo: (vo[1], VERSIONS.index(vo[0]))):
                c = operators[v, op]
                n = c["killed"] + c["survived"]
                out.writerow([op, v, n, c["killed"], f"{100 * c['killed'] / n:.1f}"])


if __name__ == "__main__":
    main(*sys.argv[1:])
