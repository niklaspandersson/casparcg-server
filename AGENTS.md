# Notes for AI agents

CasparCG Server: a C++20 playout server. Sources are under `src/`, which is also the CMake root.

| Directory         | What's in it                                                         |
| ----------------- | -------------------------------------------------------------------- |
| `src/common`      | Shared utilities: logging, executors, environment/config             |
| `src/core`        | Channels, stage/layer, mixer, frame and producer/consumer interfaces |
| `src/accelerator` | Image mixer backends: `ogl` (OpenGL) and `vulkan`                    |
| `src/modules/*`   | Producers and consumers: ffmpeg, decklink, html, image, ...          |
| `src/protocol`    | AMCP and the TCP server                                              |
| `src/shell`       | `main()`, server setup, the default `casparcg.config`                |

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
- Keep comments short. Explain what isn't obvious.

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

| Code under test                          | Test location                                | CMake                                                                        |
| ---------------------------------------- | -------------------------------------------- | ---------------------------------------------------------------------------- |
| `src/common`, `src/core`, `src/protocol` | `src/tests/<area>/<name>_test.cpp`           | add the file to the area's executable in `src/tests/CMakeLists.txt`          |
| A module's producer or consumer          | `src/modules/<module>/tests/<name>_test.cpp` | `<module>_tests` in that folder's `CMakeLists.txt`, labels `module <module>` |

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
- **Module contract harnesses**, for any module's producer or consumer, without a server or GPU.
  They create it from AMCP-style parameters through the registries, the way PLAY and ADD do, and
  check its teardown. **Read `src/tests/README.md` (the harness cookbook) before writing one of
  these tests**: it has a recipe for each case, what each harness doesn't model, and the
  declaration-order rule the teardown checks depend on.
    - `producer_harness`: `create()` a producer, `receive(*producer, n)` frames, `call()` commands.
      Each received frame has its stamped `index`, its field and its audible `samples`; check
      sequences with `runs(indices(frames))`, e.g. `"0..49 49*10"` (frames 0 to 49, then 49 held
      ten more ticks), and `runs(samples(frames))`.
    - `consumer_harness`: `create()` a consumer, `send(*consumer, n)` stamped frames with the audio
      ramp, `close(consumer)`, then check its output with `decode_clip()`.
    - When a harness is destroyed it fails the test if a producer or consumer is still held, takes
      over 10 s to go, or leaves threads, open files (Linux) or frames behind. Declare producers,
      consumers and frames after the harness, and what must outlive them (an `http_server`) before.
    - Exact sequences are the contract: don't loosen a `runs()` check to make a test pass.
- **Races** (`concurrency.h`): `background_loop` repeats a command on its own thread while the test
  receives frames, as AMCP calls do; `stress_iterations()` and `CASPAR_TEST_STRESS=50` repeat it.
- **Watchdog**: every call the harness makes into a producer is guarded. One that hangs for 30 s
  ends the test process with every thread's stack in `<output dir>/stacks.txt` (needs `gdb`).
  Missed deadlines and leaked threads also write `stacks.txt`. Read it to see where things are stuck.

Compositor tests (`src/accelerator/tests/`, label `compositor:accelerator`) run the image mixer on
a real backend; they need a GPU (both start without a display) and skip a backend that isn't
available. `compositor_harness(backend::vulkan)` builds frames on that backend (`bgra_frame(image)`,
`ycbcr_frame(image, {...})`), and `render(layers)` mixes them, bottom first, into an `image`
(`image.h` in the support library: `compare()`, `difference_image()`, `write_png()`). To compare
OpenGL with Vulkan on a new case, add a scene to `scenes()` in `compositor_test.cpp`; each parity test
writes both renders and their difference as PNGs to its output folder. Extend the harness when a
scene needs something it doesn't have.

End-to-end tests (`src/tests/e2e/`, pytest) run the installed server (`cmake --install <build>`
first) on a free port with a config of their own. They are registered with CTest, one test per file,
when the Python CMake finds has pytest (`python3 -m venv build/venv && build/venv/bin/pip install
pytest`, then configure with `-DPython3_EXECUTABLE=$PWD/build/venv/bin/python`):

```sh
ctest --test-dir build -L e2e --output-on-failure
# one test, directly (ctest sets these variables itself):
cd build/tests && CASPAR_BINARY=$PWD/../staging/bin/casparcg CASPAR_MAKE_CLIP=$PWD/caspar_make_clip \
  ../venv/bin/python -m pytest ../../src/tests/e2e/test_lifecycle.py -k kill
```

- Fixtures (`e2e/conftest.py`): `server` (one 720p5000 channel), `make_server(channels=(...),
accelerator=..., extra_config=..., start=...)`, `make_clip(path, frames=, format=)` (stamped clips,
  via the `caspar_make_clip` tool) and `output_dir`. `Server` (`e2e/caspar.py`) has `command("PLAY
1-10 clip")`, `amcp()` connections, `console()`, `signal()`, `wait_exit()` and the log.
- Every server is checked when the test ends: stopped with `KILL` if still running, then it must
  exit with code 0 (or the test's `server.expected_exit`), with no error lines in its log and no
  sanitizer report. Allow an expected error with `server.allow_errors("text")`.
- A test's server folder (config, `server.log`, `stacks.txt` on a hang, `backtrace.dump` on a crash)
  is kept under `test-output/e2e/<file>/<test>/`. A crash's stack is decoded into the failure
  message; `e2e/decode_backtrace.py <binary> <backtrace.dump>` does it by hand.
- Channels need an accelerator (`-L e2e` includes them; their label is `e2e:accelerator`).
  `CASPAR_TEST_ACCELERATOR=vulkan|opengl` picks one; tests with `make_server(channels=())` need none.
- Errors and crashes the server shows in many tests because of known bugs are listed in
  `KNOWN_ERRORS` and `KNOWN_CRASHES` in `e2e/conftest.py`, each pinned by a skipped test.

Sanitizer builds (Linux): configure a separate build folder with
`-DCASPARCG_SANITIZE=address` (ASan, LeakSanitizer and UBSan) or `-DCASPARCG_SANITIZE=thread` (TSan),
plus `-DENABLE_HTML=OFF`. Run the same `ctest` commands there. Findings fail the test. Known,
unfixed findings are suppressed in `src/tests/sanitizers/*.supp`, each with a comment saying why;
remove the entry when its cause is fixed, and never add one without a comment.

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
  reason in the skip message, and the bug is added to `src/tests/known-bugs.md`. The fix re-enables
  the test and removes the entry.

## Commits

Small, focused commits that each build. Conventional prefixes (`feat(ffmpeg):`, `fix(core):`,
`test(ffmpeg):`, `build:`, `docs:`) are used in the history.
