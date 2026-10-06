# starskirmish-hillclimb

The 8 bot versions that cleared a tier in the StarSkirmish hillclimb that started Oct 2nd 2026, as source: 5 from GPT-6 Astra and 3 from Claude Opus 5.5.

Each tier is a set of opponent bots played on 3 maps, 10 games per map and opponent: 90 games for tiers D, C and B (3 opponents), 60 for A and S (2 opponents). A submission clears the tier when, on every map, it wins at least 5 of the 10 games against each opponent and at least one more than that in total: 16 of 30 on a 3-opponent map, 11 of 20 on a 2-opponent map.

| Tier | GPT-6 Astra | Claude Opus 5.5 |
|---|---|---|
| D | 1A Gateway Dragoon Baseline: 90/90, 1st submission, 4 minutes in | 1A Gateway FAP Baseline: 90/90, 1st submission, 41 minutes in |
| C | 2B One Gate Rally: 76/90, 2nd submission, 19 minutes in | 1E Second Scout Safe Natural: 77/90, 4th submission, 3.4 hours in |
| B | 3A Dark Templar Reavers: 83/90, 2nd submission, 39 minutes in | 2B Kill Adjusted Attacks: 74/90, 6th submission, 15.5 hours in |
| A | 5H Forward Regroup Routing: 41/60, 12th submission, 7.5 hours in | not cleared in 6 submissions |
| S | 10F Consistent Worker Militia: 41/60, 29th submission, 40.8 hours in | not reached |

"Nth submission" counts that agent's submissions at that tier, up to and including the one that cleared.

## Layout

```
hillclimb/gpt-6-astra/       GPT-6 Astra: 1_tier-D_... to 5_tier-S_...
hillclimb/claude-opus-5-5/   Claude Opus 5.5: 1_tier-D_... to 3_tier-B_...
libs/                        FAP and BWEM, the two libraries the bots build against
tools/                       vdiff, the diff helper; openbw/, the shim and scripts to run the bots in vanilla OpenBW
```

Each version folder is named `<N>_tier-<T>_<version>-<description>`, where `N` orders the agent's clears and `<version>` is the agent's own version number (the number goes up when at least 10% of the code changed from the previous graded version, the letter for smaller changes). Inside:

- `src/`: the bot source. File names are the same in every version of an agent, so folders diff file by file.
- `VERSION.md`: the clearing result, when it was submitted, lines of code, and a one-line summary of every graded version since the agent's previous clear.

## Diffing versions

```bash
tools/vdiff hillclimb/gpt-6-astra 5              # tier S clear against the tier A clear
tools/vdiff hillclimb/gpt-6-astra 1 5            # tier D clear against the tier S clear
tools/vdiff hillclimb/claude-opus-5-5 3 --stat   # per-file summary
tools/vdiff hillclimb/gpt-6-astra 5 --format     # reformat both sides first
```

`--format` runs clang-format (LLVM style, 100 columns, from `.clang-format`) on temporary copies of both sides. Astra writes dense, many-statements-per-line code, so its diffs are much easier to read formatted. Install clang-format with `brew install clang-format` or `pip install clang-format`.

Plain git works too:

```bash
git diff --no-index hillclimb/gpt-6-astra/4_*/src hillclimb/gpt-6-astra/5_*/src
```

`.gitattributes` marks the C++ files so hunk headers show the enclosing function.

## Building and running

The bots are C++20 BWAPI bots. Besides BWAPI they use two libraries, vendored unmodified under `libs/` with their MIT licenses:

- [FAP](https://github.com/N00byEdge/FAP), the combat simulator: header-only, `#include <FAP.hpp>`.
- [BWEM](https://github.com/N00byEdge/BWEM-community), the map analyser: `#include <bwem.h>`, compiled from `libs/bwem/BWEM/src` into a static library.

The bots run in vanilla OpenBW. Tested on Linux (Ubuntu 24.04, clang 19) with [OpenBW](https://github.com/OpenBW/openbw) and its BWAPI port ([OpenBW/bwapi](https://github.com/OpenBW/bwapi), branch `develop-openbw`, BWAPI 4.2.0). You also need the three Brood War 1.16.1 data files (`StarDat.mpq`, `BrooDat.mpq`, `Patch_rt.mpq`) and the map files; neither is included here.

Build OpenBW's BWAPI next to this repo (`-include cstdint` is needed by newer compilers):

```bash
git clone https://github.com/OpenBW/openbw
git clone -b develop-openbw https://github.com/OpenBW/bwapi
cmake -S bwapi -B bwapi/build -DCMAKE_BUILD_TYPE=Release -DOPENBW_DIR="$PWD/openbw" -DCMAKE_CXX_FLAGS="-include cstdint"
cmake --build bwapi/build -j
```

Then, from this repo, build two versions and play them against each other:

```bash
tools/openbw/build hillclimb/gpt-6-astra/5_tier-S_10F-Consistent-Worker-Militia ../bwapi ../bwapi/build
tools/openbw/build hillclimb/claude-opus-5-5/3_tier-B_2B-Kill-Adjusted-Attacks ../bwapi ../bwapi/build
tools/openbw/play ../bwapi ../bwapi/build ~/starcraft "$HOME/starcraft/maps/(2)Heartbreak Ridge.scx" \
  build/gpt-6-astra-5.so build/claude-opus-5-5-3.so
```

`build` writes `build/<agent>-<N>.so`. `play` runs two headless `BWAPILauncher` processes that join one game at full speed, both as Protoss, and leaves both bots' logs and the replay in `build/games/<time>/`. Set `CXX` to pick the compiler; it must be the one BWAPI was built with.
