# CasparCG Architecture

## Overview

CasparCG is a professional broadcast graphics and video playout server (v2.5.0 Dev). Written in C++17 with CMake, it uses a modular plugin architecture with GPU-accelerated rendering via Vulkan (macOS) or OpenGL (Windows/Linux).

## Core Data Flow

```
AMCP (TCP:5250)
  ↓
Video Channel (one per configured channel)
  ↓
Stage → layers 0..N → each has a foreground + background producer
  ↓
Mixer → composites layers via GPU (image_mixer) + audio mixing
  ↓
Output → distributes final frames to registered consumers
           (Screen, FFmpeg, DeckLink, NDI, etc.)
```

## Key Components

| Component | Location | Purpose |
|-----------|----------|---------|
| `video_channel` | `core/video_channel.h` | Orchestrates one playout channel (stage → mixer → output) |
| `stage` / `stage_delayed` | `core/producer/stage.h` | Layer management, producer lifecycle, transform application |
| `mixer` | `core/mixer/mixer.h` | Composites draw_frames via GPU, mixes audio |
| `output` | `core/consumer/output.h` | Distributes rendered frames to consumers |
| `draw_frame` | `core/frame/draw_frame.h` | Immutable composable frame tree |
| `frame_producer` | `core/producer/frame_producer.h` | Source abstraction (file, live, generated) |
| `frame_consumer` | `core/consumer/frame_consumer.h` | Sink abstraction (display, record, hardware) |
| `accelerator` | `accelerator/accelerator.h` | GPU backend factory (Vulkan or OpenGL) |
| `image_mixer` | `core/mixer/image/image_mixer.h` | GPU rendering interface |
| `executor` | `common/executor.h` | Dedicated-thread task queue (TBB-backed) |
| `amcp_command_repository` | `protocol/amcp/amcp_command_repository.h` | AMCP command registration and dispatch |

## Directory Structure

```
src/
├── accelerator/          # GPU rendering backends
│   ├── vulkan/           # Cross-platform Vulkan (all platforms)
│   │   ├── image/        # image_mixer implementation
│   │   └── util/         # device, texture, buffer, pipeline
│   └── ogl/              # OpenGL backend (Windows/Linux only)
│       ├── image/
│       └── util/
├── common/               # Shared utilities
│   ├── executor.h        # Thread-per-executor model
│   ├── memory.h          # spl::shared_ptr / spl::unique_ptr (never-null wrappers)
│   ├── array.h           # Sized heap arrays
│   ├── bit_depth.h       # 8/10/16-bit depth enum
│   ├── diagnostics/      # Performance monitoring
│   ├── gl/               # OpenGL helpers (stub on macOS)
│   └── os/               # Platform-specific (macos/, windows/, linux/)
├── core/                 # Business logic
│   ├── video_channel.h   # Channel orchestration
│   ├── video_format.h    # Resolution, fps, field_count, audio_cadence, color_space
│   ├── consumer/         # output, frame_consumer, consumer registry
│   ├── producer/         # stage, frame_producer, producer registry
│   │   ├── color/        # Solid color producer
│   │   ├── route/        # Internal channel routing
│   │   ├── separated/    # Fill+key separation
│   │   └── transition/   # Transition effects
│   ├── mixer/            # Audio/video mixing, image_mixer interface
│   ├── frame/            # draw_frame, frame_transform, pixel_format
│   ├── monitor/          # State monitoring
│   └── diagnostics/      # osd_graph (full on Win/Linux, stub on macOS)
├── modules/              # Pluggable producers and consumers
│   ├── ffmpeg/           # Video/audio file playback and encoding
│   ├── image/            # Image sequences (PNG, TIFF, SVG)
│   ├── screen/           # Display output
│   │   └── consumer/
│   │       ├── screen_consumer_vk.mm    # macOS: GLFW + Vulkan
│   │       ├── screen_consumer.cpp      # Win/Linux: SFML + OpenGL
│   │       └── vk_util/                 # Local Vulkan abstractions for screen
│   ├── decklink/         # Blackmagic hardware I/O (producer + consumer)
│   ├── oal/              # Audio output (CoreAudio on macOS, OpenAL elsewhere)
│   ├── newtek/           # NDI support
│   ├── html/             # CEF/HTML producer (conditional: ENABLE_HTML)
│   ├── artnet/           # DMX/ArtNet (disabled on macOS)
│   ├── flash/            # Flash producer (Windows/MSVC only)
│   └── bluefish/         # Bluefish444 hardware (Windows/MSVC only)
├── protocol/             # Network protocol handling
│   ├── amcp/             # AMCP text protocol (TCP:5250)
│   └── osc/              # OSC protocol
└── shell/                # Application entry point
    ├── main.cpp          # Startup, signal handling, event loop
    ├── server.h/cpp      # Channel creation, module init, ASIO context
    └── included_modules.h
```

## Rendering Pipeline

### GPU Backend Selection

```
accelerator.set_backend(backend)
  ├── macOS:       Always Vulkan (hardcoded in Bootstrap_macOS.cmake)
  ├── Windows:     OpenGL by default, Vulkan optional
  └── Linux:       OpenGL by default, Vulkan optional
```

The backend is configured once at startup via `casparcg.config` (`<accelerator>auto|vulkan|opengl</accelerator>`). On macOS, OpenGL requests are silently upgraded to Vulkan.

### Vulkan Pipeline (macOS, optionally Win/Linux)

```
draw_frame tree
  → frame_visitor traversal
  → vulkan::image_mixer (composites layers on GPU)
  → Vulkan pipeline (shaders compiled to SPIR-V)
  → GPU texture
  → DMA copy to CPU (host-visible memory)
  → const_frame delivered to consumers
```

### OpenGL Pipeline (Windows/Linux)

```
draw_frame tree
  → frame_visitor traversal
  → ogl::image_mixer (composites layers on GPU)
  → OpenGL FBO
  → PBO readback to CPU
  → const_frame delivered to consumers
```

## Frame Composition

```cpp
draw_frame::over(frame1, frame2)   // Alpha compositing
draw_frame::mask(fill, key)        // Keying
draw_frame::still(frame)           // Freeze frame
draw_frame::push(frame)            // Transform stack push
draw_frame::pop(frame)             // Transform stack pop
```

### frame_transform Properties

- Position (x, y), Scale, Rotation, Opacity
- Clip (crop), Perspective
- Color levels, Saturation
- Applied via `stage::apply_transform()` with tweening support

### Pixel Formats and Color

- **Pixel formats:** gray, bgra, rgba, argb, abgr, ycbcr, ycbcra, luma, bgr, rgb, uyvy, gbrp, gbrap
- **Color spaces:** BT.601, BT.709, BT.2020
- **Bit depths:** 8-bit, 10-bit, 16-bit

### Interlaced Support

Stage manages separate frame pairs (foreground1/foreground2, background1/background2) for interlaced channels. Producers can deliver per-field frames.

## Threading Model

CasparCG uses a dedicated-executor pattern where each subsystem has its own thread:

| Thread | Purpose |
|--------|---------|
| **Stage executor** | Processes load/play/stop commands, produces frames |
| **Mixer/GPU dispatch** | Renders frames on GPU device thread |
| **Output executor** | Delivers frames to consumers |
| **ASIO worker** | TCP connections, async I/O (background thread) |
| **Main thread** | macOS: Cocoa/GCD event loop for GLFW windows. Other: signal handling |
| **TBB thread pool** | Parallel processing (frame manipulation, etc.) |

The `executor` class wraps a `tbb::concurrent_bounded_queue` with a dedicated thread. Operations are queued via `begin_invoke()` (async, returns future) or `invoke()` (sync, blocks until complete).

## Module System

### Registration Pattern

Every module implements:
```cpp
void init(const core::module_dependencies& dependencies);   // Register producers/consumers/commands
void uninit();                                                // Optional cleanup
```

`module_dependencies` provides access to:
- `producer_registry` - Register frame producers by name
- `consumer_registry` - Register frame consumers by name
- `cg_registry` - Register CG (graphics template) producers
- `command_repository` - Register custom AMCP commands

### Platform Availability

| Module | macOS | Windows | Linux | Notes |
|--------|-------|---------|-------|-------|
| ffmpeg | Yes | Yes | Yes | Video/audio files |
| image | Yes | Yes | Yes | Still images |
| screen | Yes (Vulkan) | Yes (OpenGL) | Yes (OpenGL) | Display output |
| decklink | Yes | Yes | Yes | Blackmagic I/O |
| oal | Yes (CoreAudio) | Yes (OpenAL) | Yes (OpenAL) | Audio output |
| newtek | Yes | Yes | Yes | NDI |
| html | Conditional | Yes | Yes | CEF/HTML (ENABLE_HTML flag) |
| artnet | No | Yes | Yes | DMX (boost::variant issues on macOS) |
| flash | No | Yes | No | MSVC only |
| bluefish | No | Yes | No | MSVC only |

### Screen Consumer Architecture

The screen module has two independent implementations:

**macOS (`screen_consumer_vk.mm`):**
- GLFW for window management
- Local `vk_util/` library (device, swapchain, render_pipeline, texture, buffer)
- Independent from `accelerator::vulkan` - uses its own Vulkan device for presentation
- GLSL shaders compiled to SPIR-V at build time
- Links: GLFW, Vulkan, Metal, QuartzCore frameworks

**Windows/Linux (`screen_consumer.cpp`):**
- SFML for window management
- OpenGL for rendering
- Links: GLEW, SFML

## AMCP Protocol

### Command Structure

Commands are registered as functions in `amcp_command_repository`:
```cpp
register_channel_command(category, name, command_func, min_params);
```

Commands receive a `command_context_simple` with client info, channel/layer indices, and parameters. They return response strings.

**Subcommand support:** `MIXER OPACITY 1-1 0.5` parses as command `"MIXER OPACITY"` with params.

### Response Codes

- **200-202:** Success
- **400-404:** Client error (bad syntax, not found)
- **500+:** Server error

### Common Commands

```
PLAY 1-1 "file.mov"       # Play on channel 1, layer 1
MIXER 1-1 OPACITY 0.5     # Set opacity
INFO 1                     # Channel info
CLEAR 1                    # Clear all layers
ADD 1 SCREEN               # Add screen consumer
REMOVE 1 SCREEN            # Remove consumer
```

## Build System

### Platform Bootstraps

| Platform | Bootstrap File | GPU Default | Window System | Build Tool |
|----------|---------------|-------------|---------------|------------|
| macOS | `Bootstrap_macOS.cmake` | Vulkan ON (required) | GLFW 3.3.8 | `tools/macos/build.sh` |
| Windows | `Bootstrap_Windows.cmake` | Vulkan OFF, OpenGL | SFML | CMake + MSVC |
| Linux | `Bootstrap_Linux.cmake` | Vulkan OFF, OpenGL | SFML + X11 | CMake + GCC/Clang |

### macOS Dependencies

- **Homebrew:** Boost (1.74+), FFmpeg, TBB, OpenAL, simde
- **Vulkan SDK:** from vulkan.lunarg.com (provides MoltenVK)
- **Fetched at build:** vk-bootstrap v1.4.328, VulkanMemoryAllocator v3.3.0, GLFW 3.3.8
- **Apple Frameworks:** Cocoa, Metal, QuartzCore, IOKit, CoreVideo, CoreFoundation
- **Deployment target:** macOS 10.15+
- **Architectures:** x86_64 (AVX2 optional), arm64

### macOS Signing (.env)

The build script (`tools/macos/build.sh --package`) reads signing credentials from `.env`:
```
SIGNING_IDENTITY="Developer ID Application: Your Name (TEAMID)"
NOTARIZE_KEYCHAIN_PROFILE="CasparCG-Notarize"
```
See `.env.example` for setup instructions. **Never commit `.env` to git.**

## Startup Sequence

1. `main()` parses command line, initializes logging
2. `server` constructor creates ASIO io_context with worker thread
3. Reads `casparcg.config` for channel definitions and settings
4. `accelerator` initializes GPU backend (Vulkan or OpenGL)
5. Modules `init()` called — registers producers, consumers, commands
6. Video channels created (stage + mixer + output per channel)
7. AMCP TCP server starts listening on port 5250
8. Main thread enters event loop (macOS: Cocoa/GCD events for GLFW, otherwise: signal wait)

## When Modifying Code

1. **Adding a producer:** Implement `frame_producer`, register in module's `init()`
2. **Adding a consumer:** Implement `frame_consumer`, register in module's `init()`
3. **Adding AMCP command:** Register via `amcp_command_repository` in `protocol/amcp/`
4. **GPU rendering changes:** Edit `accelerator/vulkan/` (all platforms) or `accelerator/ogl/` (Win/Linux legacy)
5. **Frame transforms:** Modify `core/frame/frame_transform.h` and mixer implementations
6. **Screen output:** Edit `screen_consumer_vk.mm` (macOS) or `screen_consumer.cpp` (Win/Linux)
7. **Adding a module:** Create directory under `modules/`, use `casparcg_add_module_project()`, implement `init()/uninit()`
