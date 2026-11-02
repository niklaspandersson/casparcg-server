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
