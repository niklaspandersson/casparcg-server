#!/usr/bin/env python3
"""
Video analysis utilities for CasparCG self-tests.
Uses ffprobe and frame extraction for verification.
"""

import json
import os
import subprocess
import tempfile
from dataclasses import dataclass
from typing import Optional, List, Tuple, Dict, Any


@dataclass
class VideoInfo:
    """Video file information."""
    width: int
    height: int
    duration: float
    frame_count: int
    fps: float
    codec: str
    pixel_format: str


@dataclass
class FrameColor:
    """Average color of a frame region."""
    r: int
    g: int
    b: int
    a: int = 255

    def matches(self, other: 'FrameColor', tolerance: int = 10) -> bool:
        """Check if colors match within tolerance."""
        return (
            abs(self.r - other.r) <= tolerance and
            abs(self.g - other.g) <= tolerance and
            abs(self.b - other.b) <= tolerance
        )

    def __str__(self) -> str:
        return f"RGB({self.r}, {self.g}, {self.b})"


# Standard test colors (matching CasparCG color producer)
COLORS = {
    'BLACK': FrameColor(0, 0, 0),
    'WHITE': FrameColor(255, 255, 255),
    'RED': FrameColor(255, 0, 0),
    'GREEN': FrameColor(0, 255, 0),
    'BLUE': FrameColor(0, 0, 255),
    'YELLOW': FrameColor(255, 255, 0),
    'CYAN': FrameColor(0, 255, 255),
    'MAGENTA': FrameColor(255, 0, 255),
    'GRAY': FrameColor(128, 128, 128),
    'ORANGE': FrameColor(255, 165, 0),
}


class VideoAnalyzer:
    """Analyze video files for test verification."""

    def __init__(self, ffprobe_path: str = "ffprobe", ffmpeg_path: str = "ffmpeg"):
        self.ffprobe_path = ffprobe_path
        self.ffmpeg_path = ffmpeg_path

    def _parse_ffprobe_stderr(self, stderr: str) -> Optional[VideoInfo]:
        """Parse ffprobe's human-readable stderr output.

        This is a fallback when JSON output fails due to codec errors.
        Parses lines like:
          Duration: 00:00:02.52, start: 0.000000, bitrate: 33 kb/s
          Stream #0:0: Video: h264, yuv420p, 1280x720, 50 fps
        """
        import re

        width = height = 0
        fps = 25.0
        duration = 0.0
        codec = 'unknown'
        pixel_format = 'unknown'

        # Parse duration line
        duration_match = re.search(r'Duration: (\d+):(\d+):(\d+\.\d+)', stderr)
        if duration_match:
            h, m, s = duration_match.groups()
            duration = int(h) * 3600 + int(m) * 60 + float(s)

        # Parse video stream line
        # Example: Stream #0:0: Video: h264 (High 4:4:4 Predictive), yuv444p, 1280x720, 50 fps
        video_match = re.search(r'Stream.*Video: (\w+).*?, (\w+).*?, (\d+)x(\d+)', stderr)
        if video_match:
            codec = video_match.group(1)
            pixel_format = video_match.group(2)
            width = int(video_match.group(3))
            height = int(video_match.group(4))

        # Parse fps
        fps_match = re.search(r'(\d+(?:\.\d+)?)\s*fps', stderr)
        if fps_match:
            fps = float(fps_match.group(1))

        if width == 0 or height == 0:
            return None

        frame_count = int(duration * fps) if duration > 0 else 0

        return VideoInfo(
            width=width,
            height=height,
            duration=duration,
            frame_count=frame_count,
            fps=fps,
            codec=codec,
            pixel_format=pixel_format
        )

    def get_video_info(self, filepath: str) -> Optional[VideoInfo]:
        """Get video file information using ffprobe."""
        if not os.path.exists(filepath):
            print(f"File not found: {filepath}")
            return None

        try:
            # Primary approach: Use JSON output with quiet mode
            cmd = [
                self.ffprobe_path,
                "-v", "quiet",
                "-print_format", "json",
                "-show_format",
                "-show_streams",
                filepath
            ]
            result = subprocess.run(cmd, capture_output=True, text=True)
            output = result.stdout.strip()

            # If JSON output failed or is incomplete, try text-based fallback
            if not output or output == '{' or not output.endswith('}'):
                # Fallback: parse ffprobe's human-readable stderr output
                # which is more robust against codec errors
                cmd = [
                    self.ffprobe_path,
                    "-hide_banner",
                    filepath
                ]
                result = subprocess.run(cmd, capture_output=True, text=True)

                # Parse stderr for video stream info
                # Example: "Stream #0:0: Video: h264, yuv420p, 1280x720, 50 fps"
                stderr_output = result.stderr
                video_info = self._parse_ffprobe_stderr(stderr_output)

                if video_info:
                    return video_info

                print(f"Could not get video info from {filepath}")
                return None

            data = json.loads(output)

            # Find video stream
            video_stream = None
            for stream in data.get('streams', []):
                if stream.get('codec_type') == 'video':
                    video_stream = stream
                    break

            if not video_stream:
                print("No video stream found")
                return None

            # Parse frame rate (can be "30/1" or "30000/1001")
            fps_str = video_stream.get('r_frame_rate', '25/1')
            if '/' in fps_str:
                num, den = map(int, fps_str.split('/'))
                fps = num / den if den else 25.0
            else:
                fps = float(fps_str)

            # Get frame count
            frame_count = int(video_stream.get('nb_frames', 0))
            if frame_count == 0:
                # Estimate from duration
                duration = float(data.get('format', {}).get('duration', 0))
                frame_count = int(duration * fps)

            return VideoInfo(
                width=int(video_stream.get('width', 0)),
                height=int(video_stream.get('height', 0)),
                duration=float(data.get('format', {}).get('duration', 0)),
                frame_count=frame_count,
                fps=fps,
                codec=video_stream.get('codec_name', 'unknown'),
                pixel_format=video_stream.get('pix_fmt', 'unknown')
            )

        except Exception as e:
            print(f"Error getting video info: {e}")
            return None

    def extract_frame(self, filepath: str, frame_number: int, output_path: str) -> bool:
        """Extract a specific frame as PNG."""
        try:
            # Calculate timestamp from frame number
            info = self.get_video_info(filepath)
            if not info:
                return False

            timestamp = frame_number / info.fps

            cmd = [
                self.ffmpeg_path,
                "-y",
                "-ss", str(timestamp),
                "-i", filepath,
                "-vframes", "1",
                "-f", "image2",
                output_path
            ]
            result = subprocess.run(cmd, capture_output=True, text=True)
            return result.returncode == 0

        except Exception as e:
            print(f"Error extracting frame: {e}")
            return False

    def get_average_color(self, filepath: str, frame_number: int = 0,
                          region: Optional[Tuple[int, int, int, int]] = None) -> Optional[FrameColor]:
        """
        Get average color of a frame or region.
        Region is (x, y, width, height) - if None, uses entire frame.
        """
        try:
            info = self.get_video_info(filepath)
            if not info:
                return None

            timestamp = frame_number / info.fps

            # Build crop filter if region specified
            vf = ""
            if region:
                x, y, w, h = region
                vf = f"-vf crop={w}:{h}:{x}:{y}"

            # Use ffmpeg to get average color
            # Extract frame and compute mean values
            cmd = [
                self.ffmpeg_path,
                "-ss", str(timestamp),
                "-i", filepath,
                "-vframes", "1",
            ]

            if vf:
                cmd.extend(["-vf", f"crop={region[2]}:{region[3]}:{region[0]}:{region[1]}"])

            cmd.extend([
                "-f", "rawvideo",
                "-pix_fmt", "rgb24",
                "-"
            ])

            result = subprocess.run(cmd, capture_output=True)

            if result.returncode != 0:
                return None

            # Calculate average from raw RGB data
            data = result.stdout
            if len(data) == 0:
                return None

            pixels = len(data) // 3
            r_sum = sum(data[i] for i in range(0, len(data), 3))
            g_sum = sum(data[i] for i in range(1, len(data), 3))
            b_sum = sum(data[i] for i in range(2, len(data), 3))

            return FrameColor(
                r=r_sum // pixels,
                g=g_sum // pixels,
                b=b_sum // pixels
            )

        except Exception as e:
            print(f"Error getting average color: {e}")
            return None

    def verify_solid_color(self, filepath: str, expected_color: FrameColor,
                           frame_number: int = 0, tolerance: int = 15) -> bool:
        """Verify that a frame is a solid color."""
        actual = self.get_average_color(filepath, frame_number)
        if not actual:
            print(f"  Could not get color from frame {frame_number}")
            return False

        if actual.matches(expected_color, tolerance):
            print(f"  Color match: expected {expected_color}, got {actual}")
            return True
        else:
            print(f"  Color mismatch: expected {expected_color}, got {actual}")
            return False

    def verify_frame_count(self, filepath: str, expected_count: int,
                           tolerance: int = 2) -> bool:
        """Verify video has expected number of frames."""
        info = self.get_video_info(filepath)
        if not info:
            return False

        diff = abs(info.frame_count - expected_count)
        if diff <= tolerance:
            print(f"  Frame count OK: expected ~{expected_count}, got {info.frame_count}")
            return True
        else:
            print(f"  Frame count mismatch: expected ~{expected_count}, got {info.frame_count}")
            return False

    def verify_resolution(self, filepath: str, expected_width: int,
                          expected_height: int) -> bool:
        """Verify video resolution."""
        info = self.get_video_info(filepath)
        if not info:
            return False

        if info.width == expected_width and info.height == expected_height:
            print(f"  Resolution OK: {info.width}x{info.height}")
            return True
        else:
            print(f"  Resolution mismatch: expected {expected_width}x{expected_height}, "
                  f"got {info.width}x{info.height}")
            return False

    def analyze_alpha_channel(self, filepath: str, frames: List[int] = None) -> Dict[int, Dict[str, Any]]:
        """Analyze alpha channel values at specified frames.

        Returns dict of frame_number -> {'min_alpha': int, 'max_alpha': int, 'avg_alpha': float,
                                         'has_transparency': bool, 'sample_rgba': (r,g,b,a)}
        """
        if frames is None:
            frames = [0, 10, 20, 30, 40, 50]

        results = {}
        info = self.get_video_info(filepath)
        if not info:
            return results

        for frame_num in frames:
            if frame_num >= info.frame_count:
                continue

            try:
                timestamp = frame_num / info.fps

                # Extract frame as RGBA raw data
                cmd = [
                    self.ffmpeg_path,
                    "-ss", str(timestamp),
                    "-i", filepath,
                    "-vframes", "1",
                    "-f", "rawvideo",
                    "-pix_fmt", "rgba",
                    "-"
                ]
                result = subprocess.run(cmd, capture_output=True, timeout=10)

                if result.returncode != 0:
                    continue

                data = result.stdout
                if len(data) < 4:
                    continue

                # Analyze RGBA values
                pixels = len(data) // 4
                r_vals = []
                g_vals = []
                b_vals = []
                a_vals = []

                for i in range(min(pixels, 10000)):  # Sample up to 10000 pixels
                    offset = i * 4
                    r_vals.append(data[offset])
                    g_vals.append(data[offset + 1])
                    b_vals.append(data[offset + 2])
                    a_vals.append(data[offset + 3])

                # Get center pixel
                center_offset = (pixels // 2) * 4
                if center_offset + 3 < len(data):
                    center_rgba = (data[center_offset], data[center_offset+1],
                                   data[center_offset+2], data[center_offset+3])
                else:
                    center_rgba = (0, 0, 0, 0)

                results[frame_num] = {
                    'min_alpha': min(a_vals),
                    'max_alpha': max(a_vals),
                    'avg_alpha': sum(a_vals) / len(a_vals),
                    'has_transparency': min(a_vals) < 255,
                    'center_rgba': center_rgba,
                    'avg_rgb': (sum(r_vals)//len(r_vals), sum(g_vals)//len(g_vals), sum(b_vals)//len(b_vals))
                }
            except Exception as e:
                print(f"  Error analyzing frame {frame_num}: {e}")

        return results

    def get_color_at_regions(self, filepath: str, frame_number: int,
                             regions: Dict[str, Tuple[int, int, int, int]]) -> Dict[str, FrameColor]:
        """Get average colors for multiple regions."""
        results = {}
        for name, region in regions.items():
            color = self.get_average_color(filepath, frame_number, region)
            if color:
                results[name] = color
        return results


class TestPatternVerifier:
    """Verify specific test patterns."""

    def __init__(self, analyzer: VideoAnalyzer):
        self.analyzer = analyzer

    def verify_color_bars(self, filepath: str, frame_number: int = 10) -> bool:
        """
        Verify SMPTE-style color bars.
        Standard bars from left to right: White, Yellow, Cyan, Green, Magenta, Red, Blue
        """
        info = self.analyzer.get_video_info(filepath)
        if not info:
            return False

        bar_width = info.width // 7
        bar_height = info.height // 2  # Sample from middle

        expected_colors = [
            ('White', COLORS['WHITE']),
            ('Yellow', COLORS['YELLOW']),
            ('Cyan', COLORS['CYAN']),
            ('Green', COLORS['GREEN']),
            ('Magenta', COLORS['MAGENTA']),
            ('Red', COLORS['RED']),
            ('Blue', COLORS['BLUE']),
        ]

        all_passed = True
        for i, (name, expected) in enumerate(expected_colors):
            x = i * bar_width + bar_width // 4
            region = (x, info.height // 4, bar_width // 2, bar_height // 2)
            actual = self.analyzer.get_average_color(filepath, frame_number, region)

            if actual and actual.matches(expected, tolerance=20):
                print(f"  {name} bar: OK ({actual})")
            else:
                print(f"  {name} bar: FAIL - expected {expected}, got {actual}")
                all_passed = False

        return all_passed

    def verify_gradient(self, filepath: str, frame_number: int = 10,
                        horizontal: bool = True) -> bool:
        """Verify a gradient (brightness should increase across frame)."""
        info = self.analyzer.get_video_info(filepath)
        if not info:
            return False

        samples = 10
        last_brightness = -1
        increasing = True

        for i in range(samples):
            if horizontal:
                x = (i * info.width // samples) + info.width // (samples * 2)
                region = (x, info.height // 4, info.width // samples // 2, info.height // 2)
            else:
                y = (i * info.height // samples) + info.height // (samples * 2)
                region = (info.width // 4, y, info.width // 2, info.height // samples // 2)

            color = self.analyzer.get_average_color(filepath, frame_number, region)
            if color:
                brightness = (color.r + color.g + color.b) // 3
                if last_brightness >= 0 and brightness < last_brightness - 5:
                    increasing = False
                    print(f"  Sample {i}: brightness {brightness} (decreased from {last_brightness})")
                else:
                    print(f"  Sample {i}: brightness {brightness}")
                last_brightness = brightness

        return increasing


if __name__ == "__main__":
    # Quick test
    analyzer = VideoAnalyzer()

    # Test with a sample file if it exists
    test_file = "test_output.mp4"
    if os.path.exists(test_file):
        info = analyzer.get_video_info(test_file)
        if info:
            print(f"Video: {info.width}x{info.height}, {info.fps}fps, {info.frame_count} frames")

        color = analyzer.get_average_color(test_file, 10)
        if color:
            print(f"Average color at frame 10: {color}")
    else:
        print(f"Test file '{test_file}' not found - skipping analysis test")
