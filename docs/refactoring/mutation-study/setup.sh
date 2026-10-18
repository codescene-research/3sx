#!/usr/bin/env bash
# Rebuilds the mutation study from scratch: toolchain, the two codebases with an
# identical replay harness, their Mull builds, reference traces, coverage and
# the mutant lists. Idempotent per step; safe to re-run.
#
#   before = 17d47de7  study/before-uplift  (last upstream commit, no refactoring)
#   after  = 29d0678e  study/after-uplift   (every refactoring commit)
#
# Both trees get the replay harness from the branch below, so they run exactly
# the same suite; nothing under src/sf33rd is touched, and only src/sf33rd is
# mutated (mull.yml).
set -euo pipefail

STUDY="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$STUDY/../3sx" && pwd)"
HARNESS_REF="test/stress-matchups-and-motions"
HARNESS_FILES="src/args.c src/args.h src/platform/app/sdl/sdl_stress_app.c
  src/platform/netplay/netplay_stress.c src/platform/netplay/netplay_stress.h
  src/platform/netplay/netplay.c src/platform/netplay/netplay.h
  src/platform/netplay/game_state.c src/platform/netplay/game_state_internal.h
  src/platform/netplay/game_state_load.c"
TC="$STUDY/toolchain"
export MULL_CONFIG="$STUDY/mull.yml"

step() { echo; echo "==> $*"; }

step "toolchain"
[ -x "$TC/clang19/bin/clang" ] || mamba create -y -q -p "$TC/clang19" -c conda-forge \
    'clang=19.1.*' 'clangxx=19.1.*' 'llvm-tools=19.1.*' 'compiler-rt=19.1.*' >/dev/null
[ -d "$TC/glenv/include/GL" ] || mamba create -y -q -p "$TC/glenv" -c conda-forge \
    libgl-devel libglx-devel libopengl-devel libegl-devel zlib >/dev/null
[ -x "$TC/mull/usr/bin/mull-runner-19" ] || dpkg -x "$TC/Mull-19-0.34.1-LLVM-19.1.1-ubuntu-amd64-24.04.deb" "$TC/mull"
[ -f "$TC/libnosync.so" ] || gcc -shared -fPIC -O2 -o "$TC/libnosync.so" "$TC/nosync.c"
"$TC/clang19/bin/clang" --version | head -1

FLAGS="-O2 -g -grecord-command-line -fpass-plugin=$TC/mull/usr/lib/mull-ir-frontend-19 -fprofile-instr-generate -fcoverage-mapping"

for v in before:17d47de7 after:29d0678e; do
    name=${v%%:*}; ref=${v##*:}; T="$STUDY/$name"

    step "$name: worktree at $ref with the harness from $HARNESS_REF"
    if [ ! -d "$T/.git" ] && [ ! -f "$T/.git" ]; then
        git -C "$REPO" worktree add --detach "$T" "$ref" >/dev/null
    fi
    # Start from the commit exactly, then apply the harness and build edits.
    git -C "$T" checkout -q "$ref" -- .
    git -C "$T" clean -fdq -- src/platform/netplay
    for f in $HARNESS_FILES; do git -C "$REPO" show "$HARNESS_REF:$f" > "$T/$f"; done
    ln -sfn "$REPO/third_party" "$T/third_party"
    # Build-only edits, the same on both: C-only warning flags (the old tree
    # applies them to C++ too, which gcc and clang reject) and no ASan.
    sed -i 's/^    \${DISABLED_WARNINGS}$/    $<$<COMPILE_LANGUAGE:C>:${DISABLED_WARNINGS}>/;
            s/\$<\$<CONFIG:Debug>:-fsanitize=address>//; s/\$<\$<CONFIG:Debug>:-fno-omit-frame-pointer>//' "$T/CMakeLists.txt"

    step "$name: Mull build"
    CC="$TC/clang19/bin/clang" CXX="$TC/clang19/bin/clang++" cmake -S "$T" -B "$T/bm" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$TC/glenv" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        "-DCMAKE_C_FLAGS=$FLAGS" "-DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate" >/dev/null
    cmake --build "$T/bm" >/dev/null
done

step "reference traces and coverage profiles (no mutant active)"
for name in before after; do
    python3 -I "$STUDY/scripts/mull_oracle.py" record "$STUDY/$name/bm/3sx" "$STUDY/ref-$name" "$STUDY/suite.csv" 3600 \
        > "$STUDY/ref-$name.log" 2>&1 &
done
wait
for name in before after; do
    grep -c recorded "$STUDY/ref-$name.log"
    "$TC/clang19/bin/llvm-profdata" merge -sparse "$STUDY/ref-$name"/profiles/*.profraw -o "$STUDY/ref-$name/suite.profdata"
done

step "mutant lists: all mutants, and those on lines the suite executes"
mkdir -p "$STUDY/mrep"
for name in before after; do
    (cd "$STUDY/$name/bm" &&
     "$TC/mull/usr/bin/mull-runner-19" ./3sx --test-program /bin/true --dry-run --include-not-covered --allow-surviving \
        --reporters SQLite --report-dir "$STUDY/mrep" --report-name "$name-dry" >/dev/null 2>&1 &&
     "$TC/mull/usr/bin/mull-runner-19" ./3sx --test-program /bin/true --dry-run --coverage-info "$STUDY/ref-$name/suite.profdata" \
        --allow-surviving --reporters SQLite --report-dir "$STUDY/mrep" --report-name "$name-covdry" >/dev/null 2>&1)
    python3 -c "import sqlite3,sys; d='$STUDY/mrep/$name'; n=lambda k: sqlite3.connect(d+'-'+k+'.sqlite').execute('select count(*) from mutant').fetchone()[0]; print('$name', n('dry'), 'mutants,', n('covdry'), 'covered')"
done
