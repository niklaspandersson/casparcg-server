# Known bugs found by the tests

Bugs the test suite has found and that are not fixed yet. Each one is pinned by a skipped test or a
sanitizer suppression; fixing it means re-enabling that test or removing that entry in the same
change, and removing the bug from this list.

When a test finds a bug you aren't fixing, add it here: what happens, the effect in production, how
it was found and what pins it.

## ffmpeg producer

### 1. Teardown can hang after a SEEK or loop restart

`AVProducer::Impl`'s thread can be interrupted while `seek_internal()` clears its decoders.
`~Decoder` calls `thread.join()`, which is a Boost interruption point: it throws
`thread_interrupted`, and `~Decoder` swallows it. That clears the interrupt request, so the run loop
(`while (!thread_.interruption_requested())`) never sees it, waits for frames forever, and
`~Impl`'s `join()` never returns. The decoder's own thread is also left unjoined.

- Effect: a clip stopped or replaced just after a SEEK or a loop restart never finishes tearing
  down; its threads and memory stay.
- Found by: `ffmpeg_producer_teardown.right_after_a_seek`, about 1 in 8 runs under parallel load.
- Pinned by: that test and both `ffmpeg_producer_race` tests, skipped.
- Likely fix: join under `boost::this_thread::disable_interruption`, so the request is kept.

### 2. Packets from before a SEEK reach the decoders created after it

With SEEKs arriving while the producer loops, FFmpeg logs "Cannot decode non-keyframe without valid
keyframe" (ffv1): packets read before the seek reach the new decoders. Once, the decoder thread then
died on the exception (`av_producer.cpp:190`) and the producer stopped delivering frames for good.
Same family as the loop-reset use-after-free (input reset while decoders still use the old stream).

- Effect: decode errors after SEEK or loop; occasionally a producer that stalls permanently.
- Found by: `ffmpeg_producer_race.*` in an ASan build with `CASPAR_TEST_STRESS=10`.
- Pinned by: both `ffmpeg_producer_race` tests, skipped.
- Needs investigation.

### 3. Filter graph dump is leaked

`av_producer.cpp` logs `avfilter_graph_dump()`'s string without `av_free()`ing it: about 1.4 KB per
filter graph, two graphs per PLAY, SEEK and loop restart.

- Effect: a short clip looping for a day leaks around 120 MB.
- Found by: LeakSanitizer, every producer test in an ASan build.
- Pinned by: `leak:avfilter_graph_dump` in `src/tests/sanitizers/lsan.supp`.

### 4. Destroying a producer that is still opening or reading logs errors

The interrupted open or read is reported as errors: "moov atom not found", "Stream ends
prematurely", an exception from `Input::internal_reset`, and a filter graph built with
`pix_fmt -1` after the open was aborted.

- Effect: error lines in the log when a clip is stopped soon after PLAY.
- Found by: `ffmpeg_producer_teardown.*`.
- Pinned by: `ffmpeg_producer_teardown_logs.logs_no_errors_when_destroyed_early`, skipped. The other
  teardown tests allow these messages.

### 5. A producer's teardown can outlive `main()`

`ffmpeg_producer`'s destructor hands the real producer to a detached thread, which logs through
Boost.Log. If the process exits first, that logging crashes in the already destroyed logger.

- Effect: a crash at exit, e.g. on `KILL` right after a `CLEAR` (also seen in the loop-reset kit).
- Found by: `ffmpeg_producer.reports_its_length` crashing at exit, before the harness waited for
  teardown.
- Pinned by: nothing yet; the end-to-end shutdown tests will cover `KILL` after `CLEAR`.

## common

### 6. `to_base64()` with empty input passes a null pointer to `memcpy`

Harmless with a length of 0, but undefined behaviour.

- Found by: UBSan in `base64.encodes_with_padding`.
- Pinned by: `nonnull-attribute:base64.cpp` in `src/tests/sanitizers/ubsan.supp`.
