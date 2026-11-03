# Harness cookbook

Recipes for testing a module's producer or consumer with the harnesses in `support/`. The
harnesses aren't tied to a module: they take any module's `init` and create producers and
consumers through the registries, the way AMCP does. The examples use ffmpeg because it has the
tests; each recipe names the test it comes from, so check that file when in doubt.

Which tool for what:

| You want to test                                                    | Use                                        |
| ------------------------------------------------------------------- | ------------------------------------------ |
| One producer: frames, audio, commands, parameters, teardown         | `producer_harness`                         |
| One consumer: what it writes or sends, failures, teardown           | `consumer_harness`                         |
| A producer reading over the network                                 | `producer_harness` plus `http_server`      |
| Commands racing the channel's ticks                                 | a harness plus `background_loop`           |
| Mixing: transforms, blending, keying, OpenGL against Vulkan         | `compositor_harness` (`accelerator/tests`) |
| Layers, transitions, several producers or consumers, AMCP's replies | the end-to-end tests (`e2e/`)              |

## Exact sequences are the contract

Tests check exact frame indices and sample numbers, e.g. `runs(indices(frames)) == "0..49 49*10"`
and `runs(samples(frames)) == "0..47999"`. That's intentional: a playout server promises every frame
and every sample, in order, on the tick it belongs to (960 samples per frame at 50p). A change
that shifts a frame or a sample is a behaviour change to look at, not test noise. Don't loosen
these checks to make a test pass; if the new output is right, say why in the commit message.

The exceptions are formats that can't be exact, and the test says so where it applies (e.g. MKV's
millisecond timestamps, AAC's lossy audio in `writes_mp4_with_the_default_codecs`). Check what is
exact and leave the rest out rather than adding a tolerance.

## Producer behaviour

From `modules/ffmpeg/tests/producer_test.cpp`:

```cpp
class ffmpeg_producer : public ::testing::Test
{
  protected:
    producer_harness harness{L"720p5000", {ffmpeg::init}}; // channel format, module init
};

TEST_F(ffmpeg_producer, seek_command_jumps_to_the_frame)
{
    auto producer = harness.create({path(make_clip({.frames = 50}))}); // the parameters PLAY gets

    auto before = harness.receive(*producer, 5);                       // one frame per tick
    EXPECT_EQ(harness.call(*producer, {L"SEEK", L"30"}), L"30");       // CALL's reply
    auto after = harness.receive(*producer, 5);

    EXPECT_EQ(runs(indices(before)) + " " + runs(indices(after)), "0..4 30..34");
    EXPECT_EQ(runs(samples(after)), "28800..33599");
}
```

- `make_clip()` writes a clip whose frames carry their index and whose audio is a sample-number
  ramp. For another module, add a helper next to it in `test_media.h` that stamps that module's
  input the same way, so `read_index()` and `ramp_samples()` work on its frames.
- In interlaced formats every field is its own tick: a 1080i clip's frames each arrive twice.
- `harness.underflows()` counts ticks that got no frame.

## Consumer output

From `modules/ffmpeg/tests/consumer_test.cpp`:

```cpp
TEST(ffmpeg_consumer, writes_every_frame_and_its_audio)
{
    consumer_harness harness(L"720p5000", {ffmpeg::init});
    auto             file     = output_dir() / "out.mov";
    auto             consumer = harness.create(lossless_file(file)); // the parameters ADD gets

    ASSERT_TRUE(harness.send(*consumer, 50)) << harness.error(); // stamped frames, audio ramp
    harness.close(consumer); // waits until it has flushed and is destroyed

    auto written = decode_clip(file);
    EXPECT_EQ(runs(written.indices), "0..49");
    EXPECT_EQ(runs(written.audio_samples), "0..47999");
}
```

- `send()` returns false when the consumer fails or asks to be removed; `error()` says why.
- Call `close()` before reading the output: a consumer may still be writing until it's destroyed.
- A consumer that doesn't write files (a device, a stream) needs its own way to see what it sent;
  add it to the support library so the next module can use it.

## Network source

From `modules/ffmpeg/tests/producer_test.cpp` and `producer_teardown_test.cpp`:

```cpp
TEST(ffmpeg_producer_network, plays_over_http)
{
    auto        clip = make_clip({.frames = 50});
    http_server server(clip.parent_path()); // before the harness: it must outlive the producer

    producer_harness harness(L"720p5000", {ffmpeg::init});
    auto             producer = harness.create({u16(server.url(clip.filename().string()))});
    EXPECT_EQ(runs(indices(harness.receive(*producer, 50))), "0..49");
}
```

- `server.set_rate(bytes_per_second)`, `set_delay()` and `set_hang(true)` make the source slow or
  unresponsive; `stop()` drops every connection mid-download and `start()` listens again.
- `server.requests()` lists each request with its Range header and bytes sent, for checking how a
  producer reads (seeks, retries, prefetch).

## Teardown and leak regressions

From `modules/ffmpeg/tests/producer_teardown_test.cpp`. The harness does the checking when it is
destroyed, so a teardown test often has no assertions of its own:

```cpp
class ffmpeg_producer_teardown : public ::testing::Test
{
  protected:
    http_server      server{output_dir()};               // 1. outlives everything below
    producer_harness harness{L"720p5000", {ffmpeg::init}}; // 2. takes its baseline here
};

TEST_F(ffmpeg_producer_teardown, during_playback)
{
    auto producer = harness.create({u16(make_clip({.frames = 50}).string()), L"LOOP"}); // 3.
    harness.receive(*producer, 10);
} // producer released, then the harness checks
```

What the harness checks when destroyed, and fails the test on:

- the test no longer holds any producer or consumer it created (message: "declare it after the
  harness");
- each one is destroyed within 10 s, including teardown on its own threads;
- no thread or open file (Linux) is left that wasn't there when the harness was constructed;
- no frame from its frame factory is still referenced (message: "declare received frames after
  the harness").

**Declaration order is the contract.** C++ destroys in reverse order, so:

- Producers, consumers and received frames go after the harness (as locals in the test body, or
  members declared after it).
- Anything that must outlive the producer's teardown, like an `http_server`, goes before the
  harness. It is then part of the baseline and its threads and sockets don't count as leaks.

A teardown that hangs writes every thread's stack to `stacks.txt` in the test's output folder;
read that to see where it's stuck. Teardown that logs errors when aborted early is allowed with
`allow_log_errors()` in the fixture's `SetUp()`, with a comment saying why.

## Races

From `modules/ffmpeg/tests/producer_race_test.cpp`: `background_loop` repeats a command on its own
thread while the test receives frames, the way AMCP calls arrive. `set_pacing(fast)` makes them
overlap more, and `stress_iterations()` repeats the body. Run with `CASPAR_TEST_STRESS=50`, ideally
in a sanitizer build, to hunt for rare races.
