"""Decodes the backtrace.dump a crashing casparcg writes into its working folder.

    python3 decode_backtrace.py <casparcg binary> <backtrace.dump>

The dump holds raw return addresses (boost::stacktrace::safe_dump_to in main.cpp's signal handler).
The binary's load address is found from the first frame, which is always in caspar::signal_handler;
frames outside the binary (libc, libstdc++) are printed as addresses.
"""

import struct
import subprocess
import sys
from pathlib import Path


def decode(binary: Path, dump: Path) -> str:
    data = dump.read_bytes()
    frames = [a for a in struct.unpack(f"<{len(data) // 8}Q", data[: len(data) // 8 * 8]) if a]
    if not frames:
        return "(empty backtrace.dump)"

    symbols = subprocess.run(["nm", "-C", str(binary)], capture_output=True, text=True).stdout
    handler = next((l for l in symbols.splitlines() if "caspar::signal_handler(int)" in l), None)
    if handler is None:
        return "(caspar::signal_handler not found in the binary; can't place the frames)"
    offset = int(handler.split()[0], 16)

    # The first frame is a little way into signal_handler, and the binary loads page-aligned.
    base = (frames[0] - offset) & ~0xFFF
    size = binary.stat().st_size

    lines = []
    for n, frame in enumerate(frames):
        relative = frame - base
        if 0 <= relative < size:
            # With -i, inlined calls come first, each as a function line and a location line.
            out = subprocess.run(
                ["addr2line", "-C", "-f", "-i", "-e", str(binary), hex(relative - 1)], capture_output=True, text=True
            ).stdout.splitlines()
            for function, location in zip(out[0::2], out[1::2]):
                lines.append(f"#{n} {function[:160]}  {location}")
        else:
            lines.append(f"#{n} {hex(frame)} (outside the binary)")
    return "\n".join(lines)


if __name__ == "__main__":
    print(decode(Path(sys.argv[1]), Path(sys.argv[2])))
