# Notes for AI agents

CasparCG Server: a C++20 playout server. Sources are under `src/`, which is also the CMake root.

| Directory | What's in it |
|---|---|
| `src/common` | Shared utilities: logging, executors, environment/config |
| `src/core` | Channels, stage/layer, mixer, frame and producer/consumer interfaces |
| `src/accelerator` | Image mixer backends: `ogl` (OpenGL) and `vulkan` |
| `src/modules/*` | Producers and consumers: ffmpeg, decklink, html, image, ... |
| `src/protocol` | AMCP and the TCP server |
| `src/shell` | `main()`, server setup, the default `casparcg.config` |

## Building (Linux)

```sh
cmake -S src -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo   # add -DENABLE_HTML=OFF if CEF isn't installed
cmake --build build --parallel
cmake --install build --prefix build/staging
```

`cmake --build` alone does not update `build/staging`. Anything that runs the installed server needs
`cmake --install` after every build, or it silently runs the previous binary.

See `BUILDING.md` for dependencies, Windows and the remaining build options.

## Code style

- Format C++ with the repo's `.clang-format`.
- Write code that reads like the code around it: naming, comment density, idioms.
- Keep comments short. Explain what isn't obvious; longer reasoning goes in the commit message.
- When a second implementation of something is needed, prefer a strategy class to branching inside
  the existing one (see the decklink consumer's `format_strategy`).

## Tests

The test harness is being built up step by step; this section grows with it.

Tests use GoogleTest and run through CTest. They are off by default; configure with
`-DBUILD_TESTING=ON` (GoogleTest is fetched unless an installed one is found):

```sh
cmake -S src -B build -DBUILD_TESTING=ON -DENABLE_HTML=OFF
cmake --build build --parallel
ctest --test-dir build -L unit --output-on-failure     # one tier
ctest --test-dir build -R tokenize --output-on-failure # tests whose name matches
build/tests/protocol_tests --gtest_filter='tokenize.*' # one executable, directly
```

Where tests go:

| Code under test | Test location | CMake |
|---|---|---|
| `src/common`, `src/core`, `src/protocol` | `src/tests/<area>/<name>_test.cpp` | add the file to the area's executable in `src/tests/CMakeLists.txt` |

Each test executable is declared with `caspar_add_test()` (`src/CMakeModules/CasparCG_Testing.cmake`),
which registers every GoogleTest case with CTest under the given labels. Test names are
`<suite>.<behaviour_in_snake_case>`, e.g. `tokenize.keeps_quoted_strings_together`.

Labels: `unit` (pure logic, needs nothing).

Every test executable links `caspar_test_support` (`src/tests/support/`, namespace `caspar::test`),
which provides `main()` and:

- **Log capture.** Each test's log goes to `<output dir>/log.txt`, and any `error` or `fatal` record
  fails the test, including records from background threads. A test that expects an error says so
  first with `allow_log_errors(L"part of the message")`. `log_records()` returns what was logged.
  `CASPAR_TEST_LOG_LEVEL` sets the level (default `debug`).
- **`output_dir()`**: a folder per test, `test-output/<suite>/<test>/` under the working directory
  (CTest runs from `build/tests`) or under `$CASPAR_TEST_OUTPUT`. Emptied when the test starts and
  kept afterwards. Put files the test writes there, never in the source tree.
- **`fake_frame_factory`**: a `core::frame_factory` backed by host memory, for running producers
  without an accelerator. `frames_alive()` counts frames still referenced.

When a test fails, read the failure message first, then `log.txt` in its output folder (its path is
printed under the failure).

Rules:

- **Commit the tests you write.** A test written to check a fix or a feature belongs in the repo,
  not in a scratch folder or a one-off script.
- **Use the provided plumbing** (harnesses, fixtures, test media helpers) instead of writing your own.
  If something is missing, extend the shared plumbing.
- **Never delete a test, disable it or loosen a tolerance to make it pass** without saying so, and
  why, in the commit message.
- A test that exposes a bug you aren't fixing in the same change is committed skipped, with the
  reason in the skip message. The fix re-enables it.

## Commits

Small, focused commits that each build. Conventional prefixes (`feat(ffmpeg):`, `fix(core):`,
`test(ffmpeg):`, `build:`, `docs:`) are used in the history.
