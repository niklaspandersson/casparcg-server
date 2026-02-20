# CasparCG Self-Tests

Automated tests for verifying CasparCG functionality during the macOS/Vulkan port.

## Overview

These tests connect to CasparCG via AMCP, execute various commands, and optionally record/analyze output to verify correct behavior.

## Requirements

- Python 3.8+
- CasparCG built (the test runner will start it automatically)
- FFmpeg/ffprobe (for video analysis tests)

## Quick Start

Simply run the test script - it automatically starts and stops CasparCG:

```bash
cd tests/selftest
./run_tests.sh
```

The script will:
1. Start CasparCG with the appropriate test config (macOS or Linux/Windows)
2. Wait for the server to be ready
3. Run the tests
4. Shut down CasparCG when done

## Usage

```bash
# Run all tests
./run_tests.sh

# List available tests
./run_tests.sh --list

# Run tests for a specific phase
./run_tests.sh --phase 3

# Run a specific test
./run_tests.sh --test color_playback

# Use an already-running CasparCG instance
./run_tests.sh --no-server

# Connect to different host/port
./run_tests.sh --no-server --host 192.168.1.100 --port 5250
```

## Test Organization

Tests are organized by implementation phase from [MACOS_SUPPORT.md](../../MACOS_SUPPORT.md):

| Phase | Tests | Description |
|-------|-------|-------------|
| 1 | `build_verification` | Binary runs, responds to AMCP |
| 2 | `connection` | Basic AMCP connection |
| 3 | `color_playback`, `multi_layer`, `alpha_blend` | Basic rendering |
| 4 | `blend_modes` | All blend modes |
| 5 | `transforms` | Geometric transforms |
| 6 | `color_adjust` | Color processing effects |
| 8 | `video_playback` | FFmpeg producer |
| 9 | `screen_output` | Screen consumer |
| 10 | `recording` | FFmpeg consumer |

## Files

- `amcp_client.py` - AMCP protocol client
- `video_analyzer.py` - FFmpeg-based video analysis
- `config.py` - Test configuration
- `test_runner.py` - Main test runner
- `casparcg_test.config` - Recommended CasparCG configuration for testing

## Adding New Tests

1. Add a test method to `TestRunner` in `test_runner.py`:
   ```python
   def test_my_feature(self) -> bool:
       """Test description."""
       # Test implementation
       return True  # or False
   ```

2. Register the test in `_register_tests()`:
   ```python
   self.register_test("my_feature", 5, self.test_my_feature,
                      "Test description for listing")
   ```

## Verification Methods

### AMCP Response Verification
Tests verify that AMCP commands return success codes (200-202).

### Visual Verification
Some tests require visual inspection of the screen output.

### Video Analysis
The `VideoAnalyzer` class can:
- Extract video metadata (resolution, fps, frame count)
- Sample average colors from frames or regions
- Verify solid colors match expected values
- Analyze color bars and gradients

## Environment Variables

- `CCG_HOST` - CasparCG host (default: localhost)
- `CCG_PORT` - AMCP port (default: 5250)
- `CCG_OUTPUT_DIR` - Output directory for recordings (default: test_output)

## Troubleshooting

### Connection Failed
- Verify CasparCG is running
- Check host and port settings
- Ensure AMCP protocol is enabled in config

### Video Analysis Fails
- Install FFmpeg: `brew install ffmpeg` (macOS)
- Verify ffprobe is in PATH

### Color Verification Fails
- Adjust `color_tolerance` in `config.py`
- Video compression can shift colors slightly
