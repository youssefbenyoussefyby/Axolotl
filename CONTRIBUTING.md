# Contributing to AxolotlDB

AxolotlDB is a personal research and learning project. Issues, discussion and pull
requests are welcome, but there is no support commitment and the roadmap is driven by
the phase plan in [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Ground rules

- **Engine code is written by the maintainer.** The point of the project is to build
  every layer by hand. PRs that implement roadmap items under `src/` or `include/` will
  usually be declined. Bug reports with a failing test, fixes to build/CI/tooling, docs,
  and benchmark or experiment contributions are the most useful.
- **Test-first.** A behaviour change comes with a test that fails before the change and
  passes after it. Gate tests named in `docs/phases/P*.md` are the contract.
- **Never weaken a test to make it pass.** If a test is wrong, explain why in the PR.

## Building and testing

```bash
# Linux / macOS
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTING=ON
# Windows (MSVC)
cmake -B build -DENABLE_TESTING=ON

cmake --build build --parallel
ctest --test-dir build --output-on-failure --no-tests=error   # add -C Release on MSVC
```

Sanitizer build (Linux, matches CI):

```bash
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTING=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake --build build-asan --parallel && ctest --test-dir build-asan --output-on-failure
```

## Formatting

CI checks formatting with **clang-format 18.1.8**. Other versions format differently,
so use the pinned one:

```bash
pip install clang-format==18.1.8
git ls-files 'src/*' 'include/*' 'cli/*' 'server/*' 'tests/*' 'benchmarks/*' \
  | grep -E '\.(cpp|h|hpp)$' | xargs clang-format -i
```

The tree was reformatted in a single mechanical commit, which is listed in
[`.git-blame-ignore-revs`](.git-blame-ignore-revs). To keep `git blame` useful locally:

```bash
git config blame.ignoreRevsFile .git-blame-ignore-revs
```

## Tracking progress

The checklists in [`docs/phases/P*.md`](docs/phases/README.md) are the single source of
truth for roadmap progress. GitHub renders them as checkboxes, and the CLI's `:status`
and `:tasks` read the same data from `cli/data/phases.json`, which is **generated**:

```bash
# tick the box in docs/phases/P4.md: - [ ]  ->  - [x]
python tools/gen_phases.py          # regenerate cli/data/phases.json
git add docs/phases cli/data/phases.json
```

CI runs `python tools/gen_phases.py --check` and fails if the two disagree. Never edit
`phases.json` by hand. `cli/data/queries.json` (which SQL the CLI lets through) is
hand-maintained; flip a feature to `"available"` when its executor lands.

## Commits and pull requests

- Conventional-commit style subjects: `feat(executor): …`, `fix(parser): …`,
  `test(storage): …`, `docs: …`, `ci: …`, `style: …`.
- One logical change per commit; explain the *why* in the body.
- Fill in the PR template, and make sure every CI job is green.

## Research contributions

Experiments live in [`experiments/`](experiments/README.md) and follow its protocol:
a written hypothesis and baseline before running anything, fixed seeds, and results
committed with the git SHA that produced them. Ideas for new directions are welcome as a
"Research idea" issue.

## Conduct

Participation is governed by the [Code of Conduct](CODE_OF_CONDUCT.md).
