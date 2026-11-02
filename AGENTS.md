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
ctest --test-dir build -L ffmpeg -j 8                  # one module's tests, in parallel
ctest --test-dir build -R tokenize --output-on-failure # tests whose name matches
build/tests/protocol_tests --gtest_filter='tokenize.*' # one executable, directly
```

Where tests go:

| Code under test | Test location | CMake |
|---|---|---|
| `src/common`, `src/core`, `src/protocol` | `src/tests/<area>/<name>_test.cpp` | add the file to the area's executable in `src/tests/CMakeLists.txt` |
| A module's producer or consumer | `src/modules/<module>/tests/<name>_test.cpp` | `<module>_tests` in that folder's `CMakeLists.txt`, labels `module <module>` |

Each test executable is declared with `caspar_add_test()` (`src/CMakeModules/CasparCG_Testing.cmake`),
which registers every GoogleTest case with CTest under the given labels. Test names are
`<suite>.<behaviour_in_snake_case>`, e.g. `tokenize.keeps_quoted_strings_together`.

Labels say what a test checks: `unit` (pure logic, needs nothing) or `module` plus the module's name.
CTest gets them joined into one label (`module:ffmpeg`); `-L`/`-LE` match regular expressions, so
`-L ffmpeg` and `-L module` both select it.

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
- **Test media** (`test_media.h`): `make_clip({.format = L"1080i5000", .frames = 10, .codec = ...})`
  encodes a clip into the output folder. Every frame carries its index as a barcode and the audio is
  a ramp of sample numbers, so check exact output with `read_index(frame)`, `ramp_samples(frame,
  channels)` and `decode_clip(path)` instead of comparing pixels. The barcode format is described in
  the header. Generate media in the test; don't commit media files.
- **`http_server`**: serves a folder on 127.0.0.1 for network sources (`server.url("clip.mkv")`).
  It can throttle (`set_rate`), delay or hang responses, `stop()` mid-download and `start()` again on
  the same port, and logs every request with its Range header and bytes sent.
- **`producer_harness`**: runs one producer the way a channel does. Construct it with the channel
  format and the module's init (`producer_harness h(L"1080i5000", {ffmpeg::init})`), `create()` a
  producer from AMCP-style parameters (`{path, L"LOOP"}`), `receive(*producer, n)` frames and
  `call(*producer, {L"SEEK", L"30"})`. Ticks are paced in real time by default. Each received frame
  has its stamped `index`, its field and its audible `samples`; check sequences with
  `runs(indices(frames))`, e.g. `"0..49 49*10"` (frames 0 to 49, then 49 held ten more ticks), and
  `runs(samples(frames))`. The harness waits for every producer's teardown when it is destroyed, so
  declare producers after it. See `src/modules/ffmpeg/tests/producer_test.cpp` for examples.

Things that look like bugs but aren't:

- In interlaced channels a producer delivers one frame per field, so a 1080i clip's frames each
  arrive twice (fields a and b), and `nb_frames()` counts fields.
- MKV stores timestamps in whole milliseconds, so audio after a seek can be off by up to 24 samples.
  `make_clip` writes MOV by default for that reason.

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
