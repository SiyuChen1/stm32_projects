#!/usr/bin/env python3
"""
vl53l9_visualizer.py

Live viewer for the VL53L9 STM32 firmware (53L9A1_PostprocessSingle, as modified).
Reads the binary amplitude+depth frames the firmware streams over its COM1 UART
(the same port picocom uses, e.g. /dev/ttyACM0) and renders them with matplotlib.

Wire protocol (little-endian, matches vl53l9_app.c: vis_frame_header_t / send_vis_frame()):

    header (12 bytes):
        magic          4s   b"VL59"
        frame_counter  u32
        width          u8
        height         u8
        crc16          u16   CRC-16/CCITT (poly 0x1021, init 0x0000) over the payload below

    payload:
        amplitude      width*height x float32  (AF32, native - "signal_rate", raw photon-count-rate)
        depth          width*height x uint16   (ZF32 on the wire pre-quantized to uint16 mm, round-to-nearest)
        ambient        width*height x float32  (IF32, native - "ambient_rate", raw photon-count-rate)

amplitude = strength of the sensor's own reflected laser pulse per zone (depends on distance,
target reflectivity, angle). ambient = background/environmental IR light per zone, independent
of the sensor's laser (sunlight, room lighting, etc.) - a scene-lighting/SNR indicator, not a
depth-related signal. They are visually similar (both look like brightness images) but are
physically distinct measurements.

depth is quantized to uint16 (its real range, 0-8800mm plus a 12000mm "invalid pixel" sentinel -
see INVALID_DEPTH_MM below - is known-safe). amplitude/ambient are sent as native float32: an
earlier version quantized those to uint16 too, but they are raw, undocumented-range photon-count
rates, and on real hardware that visibly clipped/saturated all texture out of the amplitude image
- so they stay lossless. For the default AR_PRECISION usecase (54x42 pixels) that's ~22.7 KB/frame.
At 3,000,000 bps that lands around 7-10 fps end-to-end (this firmware became acquisition/processing-
bound before it became UART-bound again - see the fps discussion, further baud increases won't
scale linearly from here). Plain-text printf() traces from the firmware (e.g. "Processed frame
n. ...") share the same UART; this script just ignores anything that isn't a valid, CRC-checked
"VL59" packet.

Usage:
    python3 vl53l9_visualizer.py                        # defaults: /dev/ttyACM0 @ 3000000
    python3 vl53l9_visualizer.py -p /dev/ttyACM0 -b 3000000
    python3 vl53l9_visualizer.py --save-npz out.npz     # also dump each frame to disk
"""
import argparse
import struct
import sys
import time

import numpy as np
import serial

try:
    import matplotlib

    matplotlib.use("TkAgg")
except ImportError:
    pass
import matplotlib.pyplot as plt

MAGIC = b"VL59"
HEADER_FMT = "<4sIBBH"  # magic, frame_counter, width, height, crc16
HEADER_LEN = struct.calcsize(HEADER_FMT)
assert HEADER_LEN == 12

# Must match `const float invalid_distance = 12000.0f;` in vl53l9_transform.c - the transform
# library's own sentinel for "this depth pixel failed validity checks", not a real measurement.
INVALID_DEPTH_MM = 12000

CRC16_INIT = 0x0000
CRC16_POLY = 0x1021


def crc16_ccitt(data, crc=CRC16_INIT):
    """Must match crc16_ccitt() in vl53l9_app.c bit-for-bit."""
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ CRC16_POLY) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


class FrameReader:
    """Buffers bytes from the serial port and yields validated (frame_counter, amplitude, depth,
    ambient) frames.

    Resynchronizes on the 4-byte magic if the buffer is corrupted or interleaved with text traces.
    """

    def __init__(self, ser):
        self.ser = ser
        self.buf = bytearray()
        self.total_bytes = 0  # diagnostics: lets the caller tell "nothing arriving" from "garbage arriving"

    def _fill(self, at_least, deadline=None):
        while len(self.buf) < at_least:
            if deadline is not None and time.monotonic() > deadline:
                return False
            chunk = self.ser.read(max(1, at_least - len(self.buf)))
            if chunk:
                self.buf.extend(chunk)
                self.total_bytes += len(chunk)
        return True

    def read_frame(self, timeout=None):
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            if deadline is not None and time.monotonic() > deadline:
                return None

            idx = self.buf.find(MAGIC)
            if idx == -1:
                # no magic in what we have; keep the last 3 bytes in case a magic is split across reads
                if len(self.buf) > 3:
                    del self.buf[:-3]
                if not self._fill(len(self.buf) + 1, deadline):
                    return None  # deadline hit waiting for *any* new byte - nothing is arriving at all
                continue
            if idx > 0:
                del self.buf[:idx]  # drop stray/text bytes before the magic

            if not self._fill(HEADER_LEN, deadline):
                return None
            magic, frame_counter, width, height, crc = struct.unpack(HEADER_FMT, bytes(self.buf[:HEADER_LEN]))

            plane_px = width * height
            amp_bytes = plane_px * 4  # float32, see module docstring
            depth_bytes = plane_px * 2  # uint16, see module docstring
            ambient_bytes = plane_px * 4  # float32, see module docstring
            total = HEADER_LEN + amp_bytes + depth_bytes + ambient_bytes
            if plane_px == 0 or total > 1_000_000:
                # bogus header (e.g. we resynced on a false-positive magic match) - skip past it and retry
                del self.buf[:1]
                continue

            if not self._fill(total, deadline):
                return None
            payload = bytes(self.buf[HEADER_LEN:total])

            if crc16_ccitt(payload) != crc:
                del self.buf[:1]  # corrupt frame - resync
                continue

            del self.buf[:total]

            # amplitude/ambient are native float32 on the wire; depth is uint16 (upcast to float32 so
            # downstream code - NaN masking, colormap scaling - doesn't need to care). See module
            # docstring for why depth alone is quantized.
            amplitude = np.frombuffer(payload[:amp_bytes], dtype="<f4").reshape(height, width)
            depth = (
                np.frombuffer(payload[amp_bytes : amp_bytes + depth_bytes], dtype="<u2")
                .reshape(height, width)
                .astype(np.float32)
            )
            ambient = np.frombuffer(payload[amp_bytes + depth_bytes :], dtype="<f4").reshape(height, width)
            return frame_counter, amplitude, depth, ambient


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--port", default="/dev/ttyACM0", help="serial port (default: /dev/ttyACM0)")
    parser.add_argument("-b", "--baud", type=int, default=3000000, help="baud rate (must match BspCOMInit.BaudRate in main.c)")
    parser.add_argument("--depth-cmap", default="turbo", help="matplotlib colormap for the depth panel")
    parser.add_argument("--amp-cmap", default="gray", help="matplotlib colormap for the amplitude panel")
    parser.add_argument("--ambient-cmap", default="inferno", help="matplotlib colormap for the ambient panel")
    parser.add_argument("--save-npz", metavar="FILE", help="also append every received frame to an .npz file")
    parser.add_argument(
        "--raw",
        action="store_true",
        help="debugging aid: skip frame parsing entirely and just dump raw bytes from the port to stdout, "
        "like picocom. Use this to see the firmware's plain-text printf() traces when no VL59 frames "
        "are being parsed, to tell a hung/crashed board apart from a protocol bug.",
    )
    args = parser.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.5)
    except serial.SerialException as exc:
        print(f"error: could not open {args.port} @ {args.baud}: {exc}", file=sys.stderr)
        print("hint: close picocom first (a serial port can only have one owner), and check the", file=sys.stderr)
        print("firmware's BspCOMInit.BaudRate in main.c matches --baud.", file=sys.stderr)
        sys.exit(1)

    if args.raw:
        print(f"[raw mode] dumping bytes from {args.port} @ {args.baud} bps as-is (Ctrl+C to stop)...", file=sys.stderr)
        try:
            while True:
                data = ser.read(4096)
                if data:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
        except KeyboardInterrupt:
            pass
        finally:
            ser.close()
        return

    reader = FrameReader(ser)
    saved_frames = [] if args.save_npz else None

    fig, (ax_amp, ax_depth, ax_ambient) = plt.subplots(1, 3, figsize=(15, 5))
    fig.canvas.manager.set_window_title("VL53L9 live view")
    fig.suptitle("VL53L9 live view - waiting for first frame...")
    im_amp = im_depth = im_ambient = None
    plt.ion()
    plt.show(block=False)
    plt.pause(0.1)  # force the window to actually map/render now, don't wait for the first frame

    print(f"Listening on {args.port} @ {args.baud} bps ... (close the plot window to stop)")

    last_fps_t = time.monotonic()
    n_since = 0
    last_heartbeat_t = time.monotonic()
    last_heartbeat_bytes = 0
    got_first_frame = False

    try:
        while plt.fignum_exists(fig.number):
            frame = reader.read_frame(timeout=0.3)

            if frame is None:
                # no full frame yet this tick - still pump the GUI so the window stays responsive/visible,
                # and periodically report link health so a silent link is easy to tell apart from a garbled one
                plt.pause(0.001)
                now = time.monotonic()
                if now - last_heartbeat_t >= 2.0:
                    new_bytes = reader.total_bytes - last_heartbeat_bytes
                    if not got_first_frame:
                        if new_bytes == 0:
                            print(
                                f"... no bytes received at all in the last {now - last_heartbeat_t:.0f}s. "
                                "Check: firmware rebuilt+reflashed with the streaming changes? "
                                "Board actually running (not halted in the debugger)? Right --port?"
                            )
                        else:
                            print(
                                f"... {new_bytes} bytes received but no valid frame parsed yet. "
                                "Likely a baud-rate mismatch: check BspCOMInit.BaudRate in main.c == --baud here."
                            )
                    last_heartbeat_t = now
                    last_heartbeat_bytes = reader.total_bytes
                continue
            frame_counter, amplitude, depth, ambient = frame
            got_first_frame = True

            # valid depth: >0 (0/negative = "no target") and below the library's 12000mm invalid-pixel
            # sentinel (see INVALID_DEPTH_MM) - without excluding the sentinel it dominates the color
            # scale and everything else looks washed out by comparison.
            is_valid_depth = (depth > 0) & (depth < INVALID_DEPTH_MM - 1)
            depth_masked = np.where(is_valid_depth, depth, np.nan)

            if im_amp is None:
                im_amp = ax_amp.imshow(amplitude, cmap=args.amp_cmap)
                ax_amp.set_title("Amplitude (laser signal)")
                fig.colorbar(im_amp, ax=ax_amp, fraction=0.046)

                im_depth = ax_depth.imshow(depth_masked, cmap=args.depth_cmap)
                ax_depth.set_title("Depth (mm)")
                fig.colorbar(im_depth, ax=ax_depth, fraction=0.046)

                im_ambient = ax_ambient.imshow(ambient, cmap=args.ambient_cmap)
                ax_ambient.set_title("Ambient (background IR light)")
                fig.colorbar(im_ambient, ax=ax_ambient, fraction=0.046)
            else:
                im_amp.set_data(amplitude)
                im_amp.set_clim(float(np.nanmin(amplitude)), float(np.nanmax(amplitude)))

                im_depth.set_data(depth_masked)
                if np.any(np.isfinite(depth_masked)):
                    im_depth.set_clim(float(np.nanmin(depth_masked)), float(np.nanmax(depth_masked)))

                im_ambient.set_data(ambient)
                im_ambient.set_clim(float(np.nanmin(ambient)), float(np.nanmax(ambient)))

            # numeric readout: read actual mm values, not colors, when debugging "does depth make sense"
            h, w = depth.shape
            center_valid = bool(is_valid_depth[h // 2, w // 2])
            center_str = f"{depth[h // 2, w // 2]:.0f}mm" if center_valid else "invalid"
            valid = depth[is_valid_depth]
            stats = (
                f"center={center_str}  min={float(valid.min()):.0f}mm  max={float(valid.max()):.0f}mm"
                if valid.size
                else f"center={center_str}  (no valid pixels this frame)"
            )
            fig.suptitle(
                f"VL53L9 live view - frame {frame_counter} ({amplitude.shape[1]}x{amplitude.shape[0]}) - {stats}"
            )
            plt.pause(0.001)  # redraws and pumps the GUI event loop in one call, across backends

            if saved_frames is not None:
                saved_frames.append((frame_counter, amplitude.copy(), depth.copy(), ambient.copy()))

            n_since += 1
            now = time.monotonic()
            if now - last_fps_t >= 2.0:
                print(f"frame {frame_counter}: {n_since / (now - last_fps_t):.1f} fps (link) - {stats}")
                n_since = 0
                last_fps_t = now
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        if saved_frames:
            np.savez_compressed(
                args.save_npz,
                frame_counters=np.array([f[0] for f in saved_frames], dtype=np.uint32),
                amplitude=np.stack([f[1] for f in saved_frames]),
                depth=np.stack([f[2] for f in saved_frames]),
                ambient=np.stack([f[3] for f in saved_frames]),
            )
            print(f"saved {len(saved_frames)} frames to {args.save_npz}")


if __name__ == "__main__":
    main()
