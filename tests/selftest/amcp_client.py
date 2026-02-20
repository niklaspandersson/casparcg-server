#!/usr/bin/env python3
"""
AMCP Client for CasparCG self-tests.
Provides a simple interface to send commands and receive responses.
"""

import socket
import time
from typing import Optional, Tuple, List


class AMCPClient:
    """Simple AMCP protocol client for CasparCG."""

    def __init__(self, host: str = "localhost", port: int = 5250, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.socket: Optional[socket.socket] = None
        self.buffer = ""

    def connect(self) -> bool:
        """Connect to CasparCG server."""
        try:
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.settimeout(self.timeout)
            self.socket.connect((self.host, self.port))
            return True
        except Exception as e:
            print(f"Connection failed: {e}")
            return False

    def disconnect(self):
        """Disconnect from server."""
        if self.socket:
            try:
                self.socket.close()
            except:
                pass
            self.socket = None

    def send(self, command: str) -> Tuple[int, str]:
        """
        Send AMCP command and return response.

        Returns:
            Tuple of (response_code, response_data)
            Response codes:
                200-202: Success
                400-404: Client error
                500+: Server error
                -1: Connection error
        """
        if not self.socket:
            return (-1, "Not connected")

        try:
            # Send command with CRLF terminator
            full_command = command.strip() + "\r\n"
            self.socket.sendall(full_command.encode('utf-8'))

            # Read response
            response = self._read_response()
            return self._parse_response(response)

        except Exception as e:
            return (-1, f"Error: {e}")

    def _read_response(self) -> str:
        """Read complete response from server."""
        response_lines = []

        while True:
            # Read data
            try:
                data = self.socket.recv(4096).decode('utf-8')
                if not data:
                    break
                self.buffer += data
            except socket.timeout:
                break

            # Process complete lines
            while "\r\n" in self.buffer:
                line, self.buffer = self.buffer.split("\r\n", 1)
                response_lines.append(line)

                # Check if this is a single-line response (2xx codes)
                if response_lines and len(response_lines) == 1:
                    first_line = response_lines[0]
                    if first_line.startswith(("202", "400", "401", "402", "403", "404", "500", "501", "502")):
                        return "\r\n".join(response_lines)

                # Multi-line responses end with empty line
                if line == "" and len(response_lines) > 1:
                    return "\r\n".join(response_lines)

        return "\r\n".join(response_lines)

    def _parse_response(self, response: str) -> Tuple[int, str]:
        """Parse AMCP response into code and data."""
        if not response:
            return (-1, "Empty response")

        lines = response.split("\r\n")
        first_line = lines[0]

        # Extract response code
        parts = first_line.split(" ", 1)
        try:
            code = int(parts[0])
            data = "\r\n".join(lines[1:]) if len(lines) > 1 else (parts[1] if len(parts) > 1 else "")
            return (code, data.strip())
        except ValueError:
            return (-1, response)

    def is_connected(self) -> bool:
        """Check if connected to server."""
        return self.socket is not None

    # Convenience methods for common commands

    def version(self) -> Tuple[int, str]:
        """Get server version."""
        return self.send("VERSION")

    def info(self, channel: Optional[int] = None) -> Tuple[int, str]:
        """Get channel or server info."""
        if channel:
            return self.send(f"INFO {channel}")
        return self.send("INFO")

    def play_color(self, channel: int, layer: int, color: str) -> Tuple[int, str]:
        """Play a solid color on specified channel/layer."""
        return self.send(f"PLAY {channel}-{layer} COLOR {color}")

    def loadbg_color(self, channel: int, layer: int, color: str) -> Tuple[int, str]:
        """Load a color to background."""
        return self.send(f"LOADBG {channel}-{layer} COLOR {color}")

    def play(self, channel: int, layer: int, producer: str = "") -> Tuple[int, str]:
        """Play content on channel/layer."""
        if producer:
            return self.send(f"PLAY {channel}-{layer} {producer}")
        return self.send(f"PLAY {channel}-{layer}")

    def loadbg(self, channel: int, layer: int, producer: str, auto_play: bool = False) -> Tuple[int, str]:
        """Load content to background on channel/layer."""
        cmd = f"LOADBG {channel}-{layer} {producer}"
        if auto_play:
            cmd += " AUTO"
        return self.send(cmd)

    def load(self, channel: int, layer: int, producer: str) -> Tuple[int, str]:
        """Load content to foreground on channel/layer (paused)."""
        return self.send(f"LOAD {channel}-{layer} {producer}")

    def play_route(self, channel: int, layer: int, source_channel: int,
                   source_layer: Optional[int] = None, mode: str = "") -> Tuple[int, str]:
        """Play route producer (routes frames from another channel/layer)."""
        if source_layer is not None:
            route = f"route://{source_channel}-{source_layer}"
        else:
            route = f"route://{source_channel}"
        if mode:
            route += f" {mode}"
        return self.send(f"PLAY {channel}-{layer} {route}")

    def stop(self, channel: int, layer: int) -> Tuple[int, str]:
        """Stop playback on channel/layer."""
        return self.send(f"STOP {channel}-{layer}")

    def clear(self, channel: int, layer: Optional[int] = None) -> Tuple[int, str]:
        """Clear channel or specific layer."""
        if layer is not None:
            return self.send(f"CLEAR {channel}-{layer}")
        return self.send(f"CLEAR {channel}")

    def add_consumer(self, channel: int, consumer: str, args: str = "") -> Tuple[int, str]:
        """Add a consumer to channel."""
        cmd = f"ADD {channel} {consumer}"
        if args:
            cmd += f" {args}"
        return self.send(cmd)

    def remove_consumer(self, channel: int, consumer: str, args: str = "") -> Tuple[int, str]:
        """Remove a consumer from channel.

        Note: REMOVE requires the same parameters as ADD to identify the consumer.
        You can also remove by index: remove_consumer(channel, "100001") for consumer at index 100001.
        """
        cmd = f"REMOVE {channel} {consumer}"
        if args:
            cmd += f" {args}"
        return self.send(cmd)

    def mixer(self, channel: int, layer: int, command: str, *args) -> Tuple[int, str]:
        """Send MIXER command."""
        args_str = " ".join(str(a) for a in args)
        return self.send(f"MIXER {channel}-{layer} {command} {args_str}".strip())

    def mixer_opacity(self, channel: int, layer: int, opacity: float) -> Tuple[int, str]:
        """Set layer opacity."""
        return self.mixer(channel, layer, "OPACITY", opacity)

    def mixer_blend(self, channel: int, layer: int, mode: str) -> Tuple[int, str]:
        """Set layer blend mode."""
        return self.mixer(channel, layer, "BLEND", mode)

    def mixer_fill(self, channel: int, layer: int, x: float, y: float,
                   width: float, height: float) -> Tuple[int, str]:
        """Set layer fill (position and scale)."""
        return self.mixer(channel, layer, "FILL", x, y, width, height)

    def mixer_brightness(self, channel: int, layer: int, brightness: float) -> Tuple[int, str]:
        """Set layer brightness."""
        return self.mixer(channel, layer, "BRIGHTNESS", brightness)

    def mixer_contrast(self, channel: int, layer: int, contrast: float) -> Tuple[int, str]:
        """Set layer contrast."""
        return self.mixer(channel, layer, "CONTRAST", contrast)

    def mixer_saturation(self, channel: int, layer: int, saturation: float) -> Tuple[int, str]:
        """Set layer saturation."""
        return self.mixer(channel, layer, "SATURATION", saturation)

    def mixer_levels(self, channel: int, layer: int,
                     min_input: float, max_input: float, gamma: float,
                     min_output: float, max_output: float) -> Tuple[int, str]:
        """Set layer levels control (input/output range and gamma)."""
        return self.mixer(channel, layer, "LEVELS",
                          min_input, max_input, gamma, min_output, max_output)

    def mixer_chroma(self, channel: int, layer: int,
                     enable: int, target_hue: float, hue_width: float,
                     min_sat: float, min_bri: float, softness: float,
                     spill: float, spill_sat: float, show_mask: int = 0) -> Tuple[int, str]:
        """Set layer chroma key parameters (modern 9-parameter format)."""
        return self.mixer(channel, layer, "CHROMA",
                          enable, target_hue, hue_width, min_sat, min_bri,
                          softness, spill, spill_sat, show_mask)

    def mixer_chroma_legacy(self, channel: int, layer: int,
                            mode: str, threshold: float = 0.0,
                            softness: float = 0.0, spill: float = 0.0) -> Tuple[int, str]:
        """Set layer chroma key parameters (legacy format: NONE|GREEN|BLUE)."""
        return self.mixer(channel, layer, "CHROMA", mode, threshold, softness, spill)

    def mixer_invert(self, channel: int, layer: int, invert: int = 1) -> Tuple[int, str]:
        """Set layer invert mode (0=off, 1=on)."""
        return self.mixer(channel, layer, "INVERT", invert)

    # NDI-related commands

    def ndi_list(self) -> Tuple[int, str]:
        """List available NDI sources on the network."""
        return self.send("NDI LIST")

    def play_ndi(self, channel: int, layer: int, source_name: str,
                 low_bandwidth: bool = False) -> Tuple[int, str]:
        """Play NDI source on channel/layer.

        Args:
            channel: Channel number
            layer: Layer number
            source_name: NDI source name (e.g., "COMPUTER (NDI Source)")
            low_bandwidth: Use low bandwidth mode
        """
        cmd = f"PLAY {channel}-{layer} [NDI] \"{source_name}\""
        if low_bandwidth:
            cmd += " LOW_BANDWIDTH"
        return self.send(cmd)

    def add_ndi_consumer(self, channel: int, name: str = "",
                         allow_fields: bool = False) -> Tuple[int, str]:
        """Add NDI consumer to broadcast channel as NDI source.

        Args:
            channel: Channel number
            name: Optional custom NDI source name
            allow_fields: Allow interlaced field output
        """
        args = ""
        if name:
            args += f"NAME \"{name}\""
        if allow_fields:
            args += " ALLOW_FIELDS"
        return self.add_consumer(channel, "NDI", args.strip())

    def remove_ndi_consumer(self, channel: int) -> Tuple[int, str]:
        """Remove NDI consumer from channel."""
        return self.remove_consumer(channel, "NDI")

    # HTML/CG related commands

    def play_html(self, channel: int, layer: int, url_or_template: str) -> Tuple[int, str]:
        """Play HTML template on channel/layer.

        Args:
            channel: Channel number
            layer: Layer number
            url_or_template: URL or template name (e.g., "http://example.com" or "template")
        """
        return self.send(f"PLAY {channel}-{layer} [HTML] {url_or_template}")

    def call(self, channel: int, layer: int, javascript: str) -> Tuple[int, str]:
        """Execute JavaScript on HTML producer.

        Args:
            channel: Channel number
            layer: Layer number
            javascript: JavaScript code to execute
        """
        return self.send(f'CALL {channel}-{layer} "{javascript}"')

    def cg_add(self, channel: int, layer: int, cg_layer: int, template: str,
               play_on_load: int = 1, data: str = "") -> Tuple[int, str]:
        """Add CG template.

        Args:
            channel: Channel number
            layer: Layer number
            cg_layer: CG layer (0-based)
            template: Template name
            play_on_load: 1 to auto-play, 0 to pause
            data: Optional XML/JSON data
        """
        if data:
            return self.send(f'CG {channel}-{layer} ADD {cg_layer} {template} {play_on_load} "{data}"')
        return self.send(f'CG {channel}-{layer} ADD {cg_layer} {template} {play_on_load}')

    def cg_play(self, channel: int, layer: int, cg_layer: int) -> Tuple[int, str]:
        """Play CG template."""
        return self.send(f'CG {channel}-{layer} PLAY {cg_layer}')

    def cg_stop(self, channel: int, layer: int, cg_layer: int) -> Tuple[int, str]:
        """Stop CG template."""
        return self.send(f'CG {channel}-{layer} STOP {cg_layer}')

    def cg_next(self, channel: int, layer: int, cg_layer: int) -> Tuple[int, str]:
        """Advance CG template to next state."""
        return self.send(f'CG {channel}-{layer} NEXT {cg_layer}')

    def cg_remove(self, channel: int, layer: int, cg_layer: int) -> Tuple[int, str]:
        """Remove CG template."""
        return self.send(f'CG {channel}-{layer} REMOVE {cg_layer}')

    def cg_update(self, channel: int, layer: int, cg_layer: int, data: str) -> Tuple[int, str]:
        """Update CG template with data."""
        return self.send(f'CG {channel}-{layer} UPDATE {cg_layer} "{data}"')

    def cg_invoke(self, channel: int, layer: int, cg_layer: int, method: str) -> Tuple[int, str]:
        """Invoke JavaScript method on CG template."""
        return self.send(f'CG {channel}-{layer} INVOKE {cg_layer} {method}')

    # DeckLink-related commands

    def play_decklink(self, channel: int, layer: int, device: int,
                      format: str = "", filter_str: str = "",
                      freeze_on_lost: bool = False, hdr: bool = False) -> Tuple[int, str]:
        """Play DeckLink input source on channel/layer.

        Args:
            channel: Channel number
            layer: Layer number
            device: DeckLink device number (1-based)
            format: Optional video format (e.g., "1080i5000")
            filter_str: Optional video filter string
            freeze_on_lost: Freeze on signal loss
            hdr: Enable 10-bit HDR mode
        """
        cmd = f"PLAY {channel}-{layer} DECKLINK {device}"
        if format:
            cmd += f" FORMAT {format}"
        if filter_str:
            cmd += f" FILTER {filter_str}"
        if freeze_on_lost:
            cmd += " FREEZE_ON_LOST"
        if hdr:
            cmd += " 10BIT"
        return self.send(cmd)

    def add_decklink_consumer(self, channel: int, device: int,
                              embedded_audio: bool = True,
                              key_only: bool = False,
                              keyer: str = "") -> Tuple[int, str]:
        """Add DeckLink output consumer to channel.

        Args:
            channel: Channel number
            device: DeckLink device number (1-based)
            embedded_audio: Enable embedded audio output
            key_only: Output key (alpha) signal only
            keyer: Keyer mode ("internal", "external", or "")
        """
        args = ""
        if embedded_audio:
            args += " EMBEDDED_AUDIO"
        if key_only:
            args += " KEY_ONLY"
        if keyer:
            args += f" KEYER {keyer}"
        return self.add_consumer(channel, f"DECKLINK {device}", args.strip())

    def remove_decklink_consumer(self, channel: int, device: int) -> Tuple[int, str]:
        """Remove DeckLink consumer from channel."""
        return self.remove_consumer(channel, f"DECKLINK {device}")


class AMCPTestHelper:
    """Helper class for running AMCP-based tests."""

    def __init__(self, client: AMCPClient):
        self.client = client
        self.errors: List[str] = []

    def assert_success(self, result: Tuple[int, str], message: str = "") -> bool:
        """Assert that a command succeeded (2xx response)."""
        code, data = result
        if code < 200 or code >= 300:
            error = f"Command failed: {message} - Code {code}: {data}"
            self.errors.append(error)
            print(f"  FAIL: {error}")
            return False
        print(f"  OK: {message}")
        return True

    def wait(self, seconds: float):
        """Wait for specified duration."""
        time.sleep(seconds)

    def get_errors(self) -> List[str]:
        """Get list of errors encountered."""
        return self.errors

    def has_errors(self) -> bool:
        """Check if any errors occurred."""
        return len(self.errors) > 0

    def reset(self):
        """Reset error state."""
        self.errors = []


if __name__ == "__main__":
    # Quick connection test
    client = AMCPClient()
    if client.connect():
        print("Connected to CasparCG")
        code, version = client.version()
        print(f"Version: {version}")
        client.disconnect()
    else:
        print("Failed to connect to CasparCG")
