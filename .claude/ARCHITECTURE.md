# CasparCG Architecture

## Core Data Flow

```
AMCP (TCP:5250) → Video Channel → Stage (layers) → Mixer → Output → Consumers
                                      ↑                ↑
                               Producers          Accelerator (GPU)
```

## Key Components

| Component | Location | Purpose |
|-----------|----------|---------|
| `video_channel` | core/video_channel.h | Container for one playout format |
| `stage` | core/producer/stage.h | Layer management (0-N layers) |
| `mixer` | core/mixer/mixer.h | Composites video/audio |
| `output` | core/consumer/output.h | Distributes frames to consumers |
| `draw_frame` | core/frame/draw_frame.h | Immutable frame tree (composable) |
| `frame_producer` | core/producer/frame_producer.h | Source abstraction |
| `frame_consumer` | core/consumer/frame_consumer.h | Sink abstraction |
| `accelerator` | accelerator/accelerator.h | GPU backend factory |
| `image_mixer` | core/mixer/image/image_mixer.h | GPU rendering interface |

## Directory Structure

```
src/
├── accelerator/     # GPU rendering (vk/ for Vulkan, ogl/ for OpenGL)
├── common/          # Shared utilities, logging, threading
├── core/            # Business logic (channel, stage, mixer, frame, consumer, producer)
├── modules/         # Pluggable producers/consumers
│   ├── ffmpeg/      # Video file playback/encoding
│   ├── image/       # Image sequences
│   ├── screen/      # Display output (Vulkan on macOS, OpenGL on Win/Linux)
│   ├── decklink/    # Blackmagic hardware I/O
│   ├── oal/         # OpenAL audio output
│   └── html/        # CEF/HTML (disabled on macOS)
├── protocol/        # AMCP and OSC command handling
│   └── amcp/        # Text protocol on port 5250
└── shell/           # Application entry point
```

## Rendering Pipeline

**macOS (Vulkan):**
```
draw_frame tree → frame_visitor → image_mixer → Vulkan pipeline → GPU texture → DMA to CPU
```

**Windows/Linux (OpenGL):**
```
draw_frame tree → frame_visitor → image_mixer → OpenGL FBO → GPU texture → PBO to CPU
```

## Key Patterns

1. **Producer-Consumer**: All sources implement `frame_producer`, all sinks implement `frame_consumer`
2. **Visitor Pattern**: `draw_frame` uses `frame_visitor` for composition traversal
3. **Pimpl**: Most classes hide implementation in `struct impl`
4. **Registry**: Producers/consumers/commands registered by string name at startup
5. **Async-First**: All I/O via `std::future<T>` and executors (Boost.ASIO)

## Frame Composition

```cpp
// draw_frame operators
over(frame1, frame2)      // Alpha compositing
mask(fill, key)           // Keying
push/pop(frame)           // Transform stack
```

## frame_transform Properties

- Position (x, y), Scale, Rotation, Opacity
- Clip (crop), Perspective
- Color levels, Saturation

## Module Structure

```
modules/name/
├── CMakeLists.txt       # casparcg_add_module_project()
├── name.cpp             # init(module_dependencies) / uninit()
├── producer/            # Producer implementations
└── consumer/            # Consumer implementations
```

## AMCP Commands

```
PLAY 1-1 "file.mov"      # Play on channel 1, layer 1
MIXER 1-1 OPACITY 0.5    # Set opacity
INFO 1                   # Channel info
CLEAR 1                  # Clear all layers
ADD 1 SCREEN             # Add screen consumer
```

## Video Format System

`video_format_desc`: resolution, fps, field_count (1=prog, 2=interlaced), audio_cadence, color_space

## Platform Specifics

| Platform | GPU Backend | Screen Consumer | Build |
|----------|-------------|-----------------|-------|
| macOS | Vulkan (MoltenVK) | GLFW + Vulkan | tools/macos/build.sh |
| Windows | OpenGL 4.5 | SFML + OpenGL | CMake + MSVC |
| Linux | OpenGL 4.5 (Vulkan planned) | SFML + OpenGL | CMake + Clang/GCC |

## Threading Model

- Each channel has dedicated executor
- Stage, Mixer, Output use executors for thread-safe async operations
- TBB for parallel processing
- Grand Central Dispatch on macOS

## When Modifying Code

1. **Adding a producer**: Implement `frame_producer`, register in module's `init()`
2. **Adding a consumer**: Implement `frame_consumer`, register in module's `init()`
3. **Adding AMCP command**: Add to `amcp_command_repository` in protocol/amcp/
4. **GPU rendering changes**: Edit `accelerator/vk/` (macOS) or `accelerator/ogl/` (Win/Linux)
5. **Frame transforms**: Modify `core/frame/frame_transform.h` and mixer implementations
