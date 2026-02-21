#!/usr/bin/env python3
"""
Configuration for CasparCG self-tests.
"""

import os
from dataclasses import dataclass
from typing import Optional


@dataclass
class TestConfig:
    """Test configuration settings."""

    # CasparCG connection
    host: str = "localhost"
    port: int = 5250

    # Test channels (configure in casparcg.config)
    # Channel 1: Playback channel
    # Channel 2: Recording channel (routes from channel 1)
    playback_channel: int = 1
    record_channel: int = 2

    # Output settings
    output_dir: str = "test_output"
    video_format: str = "720p5000"  # Must match casparcg.config
    width: int = 1280
    height: int = 720
    fps: float = 50.0

    # Test durations (in seconds)
    color_test_duration: float = 2.0
    blend_test_duration: float = 3.0
    transform_test_duration: float = 3.0

    # Tolerances
    color_tolerance: int = 15  # RGB units
    frame_count_tolerance: int = 5

    # FFmpeg consumer settings
    # Note: -an must be at the END because CasparCG's option parser treats
    # flags without values as key=next_arg pairs if placed before other options
    ffmpeg_args: str = "-codec:v libx264 -preset:v ultrafast -crf:v 18 -pix_fmt:v yuv420p -an"

    # Timeouts
    startup_wait: float = 1.0  # Wait after starting playback
    record_settle: float = 0.5  # Wait for recording to stabilize


# Default configuration
DEFAULT_CONFIG = TestConfig()


def get_output_path(config: TestConfig, filename: str) -> str:
    """Get full path for test output file.

    Returns an absolute path for the ffmpeg consumer.
    CasparCG's ffmpeg consumer accepts absolute paths directly.
    """
    script_dir = os.path.dirname(os.path.abspath(__file__))
    output_dir = os.path.join(script_dir, config.output_dir)
    os.makedirs(output_dir, exist_ok=True)
    return os.path.join(output_dir, filename)


def get_test_config_from_env() -> TestConfig:
    """Load configuration from environment variables."""
    config = TestConfig()

    if os.environ.get('CCG_HOST'):
        config.host = os.environ['CCG_HOST']
    if os.environ.get('CCG_PORT'):
        config.port = int(os.environ['CCG_PORT'])
    if os.environ.get('CCG_OUTPUT_DIR'):
        config.output_dir = os.environ['CCG_OUTPUT_DIR']

    return config


# Test channel configuration XML template
# Use this to set up CasparCG for testing
RECOMMENDED_CONFIG = """
<!-- Recommended casparcg.config for self-tests -->
<configuration>
    <paths>
        <media-path>media/</media-path>
        <log-path>log/</log-path>
        <data-path>data/</data-path>
        <template-path>template/</template-path>
    </paths>

    <channels>
        <!-- Channel 1: Main playback channel with screen output -->
        <channel>
            <video-mode>720p5000</video-mode>
            <consumers>
                <screen>
                    <device>1</device>
                    <windowed>true</windowed>
                </screen>
            </consumers>
        </channel>

        <!-- Channel 2: Recording channel (routes from channel 1) -->
        <channel>
            <video-mode>720p5000</video-mode>
            <consumers>
                <!-- FFmpeg consumer added dynamically during tests -->
            </consumers>
        </channel>
    </channels>

    <controllers>
        <tcp>
            <port>5250</port>
            <protocol>AMCP</protocol>
        </tcp>
    </controllers>
</configuration>
"""
