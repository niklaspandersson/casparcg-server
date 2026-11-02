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

- Effect: the server aborts (SIGABRT) at exit after "Successfully shutdown CasparCG Server." when a
  clip was playing or had just been cleared: about 1 in 3 `KILL`s in the end-to-end tests. The log
  shows the producer's "Joined" after the shutdown message.
- Found by: `ffmpeg_producer.reports_its_length` crashing at exit, before the harness waited for
  teardown; then `e2e/test_lifecycle.py`.
- Pinned by: `test_shuts_down_while_playing` and `test_kill_right_after_clear` in
  `e2e/test_lifecycle.py`, skipped.

## ffmpeg consumer

### 7. Dropped frames aren't reported

`ffmpeg_consumer::send()` queues frames for its encoder thread with `try_push` on a queue of 64.
When the encoder can't keep up, frames are dropped and only the diagnostics graph is tagged
("dropped-frame"); nothing is logged.

- Effect: a FILE recording with gaps (video and audio) and no trace of it in the log. In a test,
  300 frames of 1080p50 sent faster than ProRes encodes them gave a file with 69.
- Found by: an overload experiment with `consumer_harness` (fast pacing).
- Pinned by: `ffmpeg_consumer_known_issues.reports_frames_it_drops`, skipped.
- Open question: whether a FILE (non-realtime) consumer should drop at all.

### 8. Warning about an option the consumer set itself

When the container's default video codec is H.264, the consumer adds `preset:v=veryfast`, even when
`-codec:v` chooses another encoder, and then warns "Unused option preset=veryfast:v".

- Effect: a misleading warning for every recording to MOV/MP4 with a non-H.264 codec.
- Found by: the consumer tests' logs.
- Pinned by: `ffmpeg_consumer_known_issues.no_warning_about_its_own_default_options`, skipped.

### 9. Question: interlaced channels are recorded as double-rate progressive

In an interlaced channel each field is its own `send()`, and the consumer writes each as a frame:
a 1080i5000 channel becomes a 1080 file at 50 frames per second with an unknown field order, not
25 interlaced frames per second. `send()` has a `TODO - field alignment` here. This may be
intended; it needs a decision.

- Found by: `ffmpeg_consumer.interlaced_channel_writes_every_field` (which only checks that every
  field is written).

## AMCP server and shutdown

### 11. Shutdown can log "local_endpoint: Bad file descriptor"

`AsyncEventServer::implementation::stop()` closes the acceptor from the thread shutting the server
down, while the AMCP io thread may still use it (asio acceptors aren't thread-safe), and `print()`
calls `acceptor_.local_endpoint()`, which throws once the acceptor is closed. The exception is logged
as an error with a stack trace.

The same race can crash the server: in one run of ten the io thread segfaulted in
`handle_accept()` → `start_accept()` → `async_accept()` (in `epoll_reactor::start_op`) while the
acceptor was being closed, and the process ended with SIGABRT before "Successfully shutdown".

- Effect: an error in the log on many ordinary shutdowns (seen with the OpenGL accelerator), and an
  occasional crash on shutdown.
- Found by: `e2e/test_lifecycle.py`; the crash stack from the server's `backtrace.dump`.
- Pinned by: `test_lifecycle.py::test_kill_logs_no_errors`, skipped; the e2e fixture allows the
  message and the crash in other tests (`KNOWN_ERRORS` and `KNOWN_CRASHES` in `e2e/conftest.py`).

### 12. A server that fails to start exits with code 0

With the AMCP port already taken, the server logs `[fatal] Failed to setup AMCP controller ... It is
likely already in use` and exits with code 0. `main()` only sets the exit code on the normal path.

- Effect: service managers and scripts see a successful run.
- Found by: `e2e/test_lifecycle.py::test_port_in_use_is_reported`.
- Pinned by: `test_lifecycle.py::test_failed_startup_exits_with_an_error`, skipped.

### 13. A client disconnecting logs an exception

`connection::ipv4_address()` (`protocol/util/AsyncEventServer.cpp`) calls
`socket_->remote_endpoint()` whenever the socket is open; once the peer has gone it throws
"Transport endpoint is not connected", e.g. while logging "Client ... disconnected". The exception is
logged as an error with a stack trace.

- Effect: an error with a stack trace in the log whenever a client disconnects at the wrong moment
  (e.g. right after sending a command).
- Found by: `e2e/test_amcp_tcp.py`.
- Pinned by: `test_amcp_tcp.py::test_early_disconnect_logs_no_errors`, skipped; the e2e fixture
  allows the message in other tests.

### 14. The server can abort after a complete shutdown

After logging "Successfully shutdown CasparCG Server.", the process sometimes ends with SIGABRT,
even with no channels and no clips (so this is not #5). No `backtrace.dump` is written, so the abort
bypasses or comes after the server's crash handler. Under gdb, a run that hit it forked a process
after the shutdown message and hung.

- Effect: about 1 in 8 `KILL`s in the end-to-end tests exit with SIGABRT instead of 0.
- Found by: `e2e/test_amcp_tcp.py` and `e2e/test_lifecycle.py` failing at teardown.
- Pinned by: `test_lifecycle.py::test_repeated_kill_never_aborts`, skipped; the e2e fixture accepts
  SIGABRT after a logged successful shutdown (`allow_abort_after_shutdown` in `e2e/caspar.py`).
- Needs investigation; a core dump would show it.

### 15. Unconfirmed: RESTART once hung, and once closed the connection without a reply

In one e2e run `RESTART` was answered by the connection closing instead of `202 RESTART OK`; in
another the server didn't exit within 15 s of `RESTART`. Neither came back in 20 further runs of
`test_restart_exits_with_code_5`; the stacks of the hang were overwritten before they were read.

- Next time: keep the run's output (`CASPAR_TEST_OUTPUT=<new folder>`) and read `stacks.txt`.

## common

### 10. `to_base64()` with empty input passes a null pointer to `memcpy`

Harmless with a length of 0, but undefined behaviour.

- Found by: UBSan in `base64.encodes_with_padding`.
- Pinned by: `nonnull-attribute:base64.cpp` in `src/tests/sanitizers/ubsan.supp`.
