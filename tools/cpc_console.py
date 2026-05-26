#!/usr/bin/env python3
"""
frank-cpc serial console — host-side control library.

Usage as a library:
    from cpc_console import CPCConsole
    cpc = CPCConsole()           # auto-detect USB serial
    cpc.ping()                   # → 'PONG'
    cpc.disk_insert('A', '/cpc/disk/game.dsk')
    cpc.type_text('RUN"DISC\\r')
    cpc.reset()

Usage as CLI:
    python3 cpc_console.py ping
    python3 cpc_console.py disk a insert /cpc/disk/game.dsk
    python3 cpc_console.py type 'RUN"DISC\\r'
    python3 cpc_console.py interactive   # interactive REPL
"""

import sys
import time
import glob
import serial


def find_cpc_port():
    """Auto-detect the frank-cpc USB serial port."""
    patterns = [
        "/dev/tty.usbmodem*",
        "/dev/cu.usbmodem*",
        "/dev/ttyACM*",
        "/dev/ttyUSB*",
    ]
    for pat in patterns:
        ports = sorted(glob.glob(pat))
        if ports:
            return ports[0]
    return None


class CPCConsole:
    """Control a frank-cpc board over USB serial."""

    def __init__(self, port=None, baudrate=115200, timeout=2.0):
        if port is None:
            port = find_cpc_port()
            if port is None:
                raise RuntimeError("No CPC USB serial port found")
        self.ser = serial.Serial(port, baudrate, timeout=timeout)
        # Drain any boot messages
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def send(self, command, timeout=5.0):
        """Send a command and return (ok, payload).

        ok: True if response starts with 'OK', False if 'ERR'.
        payload: the rest of the line after OK/ERR.
        Also collects any '#' comment lines as extra info.
        """
        self.ser.reset_input_buffer()
        self.ser.write((command.strip() + "\r\n").encode())
        self.ser.flush()

        comments = []
        deadline = time.time() + timeout
        result = None

        while time.time() < deadline:
            line = self.ser.readline().decode("utf-8", errors="replace").strip()
            if not line:
                # Empty line after we got a result means no more data
                if result is not None:
                    break
                continue
            # Skip echo of our command
            if line == command.strip():
                continue
            if line.startswith("# "):
                comments.append(line[2:])
                continue
            if line.startswith("OK"):
                payload = line[2:].strip()
                result = (True, payload)
                # Keep reading for trailing # comments (short timeout)
                self.ser.timeout = 0.3
                continue
            if line.startswith("ERR"):
                payload = line[3:].strip()
                result = (False, payload)
                self.ser.timeout = 0.3
                continue

        # Restore original timeout
        self.ser.timeout = 2.0

        if result is not None:
            return result[0], result[1], comments

        raise TimeoutError(f"No response to: {command}")

    def cmd(self, command, timeout=5.0):
        """Send command, return payload string. Raise on ERR."""
        ok, payload, comments = self.send(command, timeout)
        if not ok:
            raise RuntimeError(f"CPC error: {payload}")
        return payload

    # ---- convenience methods ----

    def ping(self):
        return self.cmd("PING")

    def reset(self):
        return self.cmd("RESET")

    def status(self):
        return self.cmd("STATUS")

    def disk_insert(self, drive, path):
        return self.cmd(f"DISK {drive.upper()} INSERT {path}")

    def disk_eject(self, drive):
        return self.cmd(f"DISK {drive.upper()} EJECT")

    def disk_status(self, drive):
        return self.cmd(f"DISK {drive.upper()} STATUS")

    def tape_insert(self, path):
        return self.cmd(f"TAPE INSERT {path}")

    def tape_eject(self):
        return self.cmd("TAPE EJECT")

    def tape_status(self):
        return self.cmd("TAPE STATUS")

    def cart_insert(self, path):
        return self.cmd(f"CART INSERT {path}")

    def cart_eject(self):
        return self.cmd("CART EJECT")

    def cart_status(self):
        return self.cmd("CART STATUS")

    def type_text(self, text):
        """Type text into the CPC. Use \\r for Enter, \\n for 10s pause."""
        return self.cmd(f"TYPE {text}")

    def key_press(self, row, bit):
        return self.cmd(f"KEY {row} {bit} PRESS")

    def key_release(self, row, bit):
        return self.cmd(f"KEY {row} {bit} RELEASE")

    def keys_reset(self):
        return self.cmd("KEYS RESET")

    def cat(self):
        """List SD card directory. Returns (dir_path, entries)."""
        ok, payload, comments = self.send("CAT")
        if not ok:
            raise RuntimeError(f"CPC error: {payload}")
        return payload, comments

    def cd(self, path):
        return self.cmd(f"CD {path}")

    def wait_frames(self, n):
        """Wait for n frames (at 50fps)."""
        time.sleep(n / 50.0)


def interactive(cpc):
    """Simple REPL for sending commands."""
    print(f"Connected. Type commands (PING, DISK A INSERT ..., etc). Ctrl-C to exit.")
    while True:
        try:
            line = input("cpc> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not line:
            continue
        if line.lower() == "quit":
            break
        try:
            ok, payload, comments = cpc.send(line)
            for c in comments:
                print(f"  # {c}")
            status = "OK" if ok else "ERR"
            print(f"  {status} {payload}")
        except TimeoutError as e:
            print(f"  TIMEOUT: {e}")


def main():
    import argparse

    parser = argparse.ArgumentParser(description="frank-cpc serial console")
    parser.add_argument("--port", help="Serial port (auto-detect if omitted)")
    parser.add_argument("command", nargs="*", help="Command to send (or 'interactive')")
    args = parser.parse_args()

    cpc = CPCConsole(port=args.port)
    print(f"Connected to {cpc.ser.port}")

    if not args.command or args.command[0].lower() == "interactive":
        interactive(cpc)
    else:
        cmd_str = " ".join(args.command)
        try:
            ok, payload, comments = cpc.send(cmd_str)
            for c in comments:
                print(f"# {c}")
            if ok:
                print(f"OK {payload}")
            else:
                print(f"ERR {payload}")
                sys.exit(1)
        except TimeoutError as e:
            print(f"TIMEOUT: {e}", file=sys.stderr)
            sys.exit(1)

    cpc.close()


if __name__ == "__main__":
    main()
