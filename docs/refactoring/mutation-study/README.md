# Mutation study: 3SX before and after the CodeHealth uplift

Does refactoring a decompiled codebase to CodeHealth 10 change its mutation score under
a fixed test suite? We mutated the decompiled game code (`src/sf33rd`) in two versions of
3SX and ran every mutant that the suite executes against the same 30 replayed fights. The
run took place from 13 to 18 October 2026.

| Version | Commit | Tag |
| --- | --- | --- |
| before (unhealthy) | `17d47de7` | `study/before-uplift` |
| after (healthy) | `29d0678e` | `study/after-uplift` |

## Results

| | before | after |
| --- | --- | --- |
| all mutants | 47,065 | 36,619 |
| mutants the suite executes | 7,180 (15.3%) | 9,089 (24.8%) |
| killed | 4,108 | 5,460 |
| survived (executed) | 3,072 | 3,629 |
| **killed / all mutants** | **8.7%** | **14.9%** |
| killed / executed mutants | 57.2% | 60.1% |

The two scores answer different questions. Killed/all is the conventional mutation score:
mutants on lines the suite never executes cannot change behavior, so we did not run them,
and they count as surviving. Killed/executed excludes them and thus measures how well the
oracle detects changes in the code the suite reaches. Note that it is the much higher
number, and it should not be compared with the plain mutation scores reported elsewhere.

The kills break down as follows. A mutant is killed by the first fight that fails.

| Kill reason | before | after |
| --- | --- | --- |
| state diverged from the reference | 3,084 | 4,389 |
| crash | 849 | 856 |
| hang or short trace | 175 | 215 |
| killed in fight 1 | 2,805 | 3,360 |

Per-operator counts are in [`operators.csv`](operators.csv).
[`scripts/summarize.py`](scripts/summarize.py) recomputes every number in this section from
[`results.jsonl`](results.jsonl).

## Findings

**The suite reaches much more of the healthy code.** It executes 24.8% of the mutants in
the after version, compared with 15.3% before. This is the largest effect, and it is why
killed/all almost doubles from 8.7% to 14.9%. Note that the versions do not share mutants:
the refactoring changed the code, so the comparison is between two populations rather than
a paired comparison of the same mutants.

**Among executed mutants, the score is 2.9 percentage points higher after the uplift.**
Killed/executed rises from 57.2% to 60.1%, a two-proportion z of 3.68. However, mutants on
the same line are not independent, so the test overstates the evidence. The difference
should be read as small but probably real.

**Most of that gain comes from removed void calls.** The after version has 2,546
`cxx_remove_void_call` mutants, compared with 1,456 before, and their kill rate rises from
51.8% to 60.1%. This is consistent with refactoring that extracts code into small helper
functions. Excluding this operator, killed/executed is 58.6% before and 60.1% after. Most
other operators differ by less than three percentage points between the versions.

**Boundary mutants survive most often in both versions.** The suite kills only 27.4% to
32.7% of the `>=` to `>`, `>` to `>=`, and `<` to `<=` mutants. Many of them might be
equivalent in practice, and others need an exact boundary value that 30 fights may never
produce.

## Why killed/executed is high

The oracle is a full-state regression check rather than a set of assertions. For every
frame of every fight, it compares a hash of the game's rollback state with a reference
recorded from the unmutated build of the same version. Thus, any mutant that changes the
simulated game state at any point is killed, together with every mutant that crashes or
hangs. Two factors push the score down rather than up. First, equivalent mutants are
counted as survivors, since no test can kill them. Second, rendering and sound are not
part of the state hash, so mutants there are only killed when they crash or hang.

## Method

- **Code under mutation.** Only `src/sf33rd`, the decompiled game. Both versions get the
  identical replay harness from branch `test/stress-matchups-and-motions` (files outside
  `src/sf33rd`), so both run exactly the same suite.
- **Operators.** Mull's defaults (`cxx_default`, i.e., increment, arithmetic, comparison,
  and boundary) plus `cxx_remove_void_call`; see [`mull.yml`](mull.yml).
- **Suite.** 30 forced fights of 3,600 frames each (one minute of game time) with generated
  inputs, including special-move motions; see [`suite.csv`](suite.csv). Each of the 19
  selectable characters plays three or four times on both sides, and every stage appears.
- **Oracle.** [`scripts/mull_oracle.py`](scripts/mull_oracle.py) replays the fights in order
  and stops at the first difference, crash, short trace, or hang (240 seconds per fight; an
  unmutated fight takes about 50).
- **Execution.** [`scripts/mull_drive.py`](scripts/mull_drive.py) takes the executed mutants
  of both versions from Mull's coverage dry run, shuffles them together with a fixed seed,
  and switches each on the way Mull does. Each verdict is appended to the results file as
  soon as it is known. The run took about 3,570 hours of mutant time on 30 workers.
- **Toolchain.** clang 19.1.7 (conda-forge) and Mull 0.34.1 for LLVM 19.
  [`toolchain/nosync.c`](toolchain/nosync.c) turns `fsync` into a no-op so that 30 parallel
  games do not queue on the disk journal. The traces are byte-identical with and without it.

Two interruptions affected the run. On 13 October, a stop of the driver killed 29 games in
flight, and these were recorded as kills. We removed them on 14 October and re-ran the
mutants, and the driver has since discarded any verdict that arrives after a stop signal.
On 18 October, the server shut down with 175 mutants left. We restarted the driver, which
skips mutants that already have a verdict, and it completed the remaining 175.

## Files

- `results.jsonl` - one verdict per executed mutant (16,269 lines): `version`, `mutant_id`
  (Mull's id, with paths relative to the repository root), `status` (`killed` or
  `survived`), `reason` for a kill, and `seconds` of run time.
- `operators.csv` - executed and killed mutants per operator and version.
- `setup.sh`, `mull.yml`, `suite.csv`, `scripts/`, `toolchain/nosync.c` - the study as it
  was run. `setup.sh` expects to live in a folder next to a 3sx clone named `3sx`, with
  `Mull-19-0.34.1-LLVM-19.1.1-ubuntu-amd64-24.04.deb` (from the Mull releases on GitHub)
  in `toolchain/`. It creates the two worktrees, builds them with Mull, records the
  reference traces, and lists the mutants. Run the driver from the same folder with
  `python3 -I scripts/mull_drive.py results.jsonl 30`.
