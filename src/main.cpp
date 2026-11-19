#!/usr/bin/env python3
"""Convert any supported image to a 400x300 1-bit bitmap and send it to EE05.

The ESP32 firmware is flashed once with PlatformIO. This desktop tool converts
images locally and transfers the packed bitmap over USB serial; a new image does
not require another firmware build/upload.
"""
from __future__ import annotations

import queue
import struct
import sys
import threading
import time
import zlib
from pathlib import Path
from typing import Callable

try:
    from PIL import Image, ImageOps, ImageTk, UnidentifiedImageError
except ImportError:
    print("Pillow is missing. In the VS Code terminal run: python -m pip install -r requirements.txt")
    raise SystemExit(2)

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None

PROJECT_ROOT = Path(__file__).resolve().parent
WIDTH = 400
HEIGHT = 300
ROW_BYTES = (WIDTH + 7) // 8
PAYLOAD_BYTES = ROW_BYTES * HEIGHT
BAUD_RATE = 115200
MAGIC = b"EIMG"
HEADER_FORMAT = "<4sHHII"
READY_TEXT = "EE05_READY"
RECEIVING_TEXT = "RECEIVING_IMAGE"
DONE_TEXT = "DISPLAYED"


def load_and_resize(path: Path, fit: str) -> Image.Image:
    """Load an image, composite transparency on white, and resize to the panel."""
    try:
        with Image.open(path) as opened:
            # Use the first frame for animated GIF/WebP files.
            try:
                opened.seek(0)
            except EOFError:
                pass
            image = ImageOps.exif_transpose(opened).convert("RGBA")
    except FileNotFoundError:
        raise FileNotFoundError(f"Image file not found: {path}")
    except UnidentifiedImageError as exc:
        raise ValueError(f"Unsupported or damaged image file: {path}") from exc

    white_background = Image.new("RGBA", image.size, (255, 255, 255, 255))
    white_background.alpha_composite(image)
    image = white_background.convert("RGB")
    resampling = Image.Resampling.LANCZOS

    if fit == "stretch":
        return image.resize((WIDTH, HEIGHT), resampling)
    if fit == "contain":
        fitted = ImageOps.contain(image, (WIDTH, HEIGHT), method=resampling)
        canvas = Image.new("RGB", (WIDTH, HEIGHT), "white")
        canvas.paste(fitted, ((WIDTH - fitted.width) // 2,
                              (HEIGHT - fitted.height) // 2))
        return canvas
    if fit == "cover":
        return ImageOps.fit(image, (WIDTH, HEIGHT), method=resampling,
                            centering=(0.5, 0.5))
    raise ValueError(f"Unknown image fit mode: {fit}")


def convert_to_monochrome(path: Path, fit: str, dither: str,
                           threshold: int, invert: bool) -> Image.Image:
    image = load_and_resize(path, fit)
    gray = ImageOps.grayscale(image)

    if dither == "floyd":
        mono = gray.convert("1", dither=Image.Dither.FLOYDSTEINBERG)
    elif dither == "threshold":
        mono = gray.point(lambda value: 255 if value >= threshold else 0).convert("1")
    else:
        raise ValueError(f"Unknown dithering mode: {dither}")

    if invert:
        mono = ImageOps.invert(mono.convert("L")).convert("1")
    return mono


def pack_bitmap_msb_first(mono: Image.Image) -> bytes:
    """Pack black pixels as 1 bits, MSB-first, matching TFT_eSPI drawBitmap()."""
    if mono.size != (WIDTH, HEIGHT):
        raise ValueError(f"Expected {WIDTH}x{HEIGHT}, got {mono.width}x{mono.height}")

    pixels = mono.convert("L")
    data = bytearray(PAYLOAD_BYTES)
    for y in range(HEIGHT):
        row_offset = y * ROW_BYTES
        for x in range(WIDTH):
            # Pillow mode 1 uses 0 for black and 255 for white.
            if pixels.getpixel((x, y)) < 128:
                data[row_offset + (x >> 3)] |= 0x80 >> (x & 7)
    return bytes(data)


def wait_for_message(connection, expected: str, timeout_seconds: float,
                     notify: Callable[[str], None]) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        line = connection.readline()
        if not line:
            continue
        message = line.decode("utf-8", errors="replace").strip()
        if not message:
            continue
        notify(f"Board: {message}")
        if message == expected:
            return
        if message.startswith("ERR_"):
            raise RuntimeError(f"Board reported {message}")
    raise TimeoutError(f"Timed out waiting for {expected}. Check the USB cable and COM port.")


def send_bitmap(port: str, payload: bytes,
                notify: Callable[[str], None] = print) -> None:
    """Send a packed 400x300 image to the EE05 firmware using CRC-checked serial."""
    if serial is None:
        raise RuntimeError(
            "PySerial is missing. Install dependencies with: "
            "python -m pip install -r requirements.txt"
        )
    if len(payload) != PAYLOAD_BYTES:
        raise ValueError(f"Invalid bitmap length: {len(payload)} bytes")

    checksum = zlib.crc32(payload) & 0xFFFFFFFF
    header = struct.pack(HEADER_FORMAT, MAGIC, WIDTH, HEIGHT, len(payload), checksum)

    notify(f"Opening {port} at {BAUD_RATE} baud...")
    try:
        with serial.Serial(port=port, baudrate=BAUD_RATE, timeout=0.4,
                           write_timeout=20, xonxoff=False, rtscts=False,
                           dsrdtr=False) as connection:
            # Opening a USB serial port can reset the ESP32-S3. Firmware sends
            # EE05_READY repeatedly, so wait for it instead of assuming timing.
            notify("Waiting for EE05 firmware to become ready...")
            wait_for_message(connection, READY_TEXT, 45, notify)

            connection.write(header)
            connection.flush()
            wait_for_message(connection, RECEIVING_TEXT, 10, notify)

            notify(f"Sending {len(payload):,} bitmap bytes in acknowledged blocks...")
            # Match CHUNK_BYTES in src/main.cpp. Waiting for every ACK keeps
            # the host from overrunning the ESP32-S3 USB CDC receive buffer.
            chunk_size = 128
            for offset in range(0, len(payload), chunk_size):
                chunk = payload[offset:offset + chunk_size]
                written = connection.write(chunk)
                if written != len(chunk):
                    raise RuntimeError(
                        f"USB serial accepted only {written} of {len(chunk)} bytes "
                        f"at payload offset {offset}. Try another USB cable/port."
                    )
                connection.flush()

                expected_ack = f"ACK:{offset + len(chunk)}"
                wait_for_message(connection, expected_ack, 10, notify)

                if ((offset // chunk_size) % 10 == 0) or offset + len(chunk) == len(payload):
                    percent = min(100, int((offset + len(chunk)) * 100 / len(payload)))
                    notify(f"Transfer progress: {percent}% ({offset + len(chunk):,}/{len(payload):,} bytes)")

            notify("All bitmap bytes acknowledged. Waiting for the ePaper refresh...")
            wait_for_message(connection, DONE_TEXT, 180, notify)
    except serial.SerialException as exc:
        raise RuntimeError(
            f"Could not use {port}: {exc}\n"
            "Close PlatformIO/Arduino Serial Monitor and try again."
        ) from exc

    notify("SUCCESS: image displayed on the EE05.")


def save_preview(mono: Image.Image) -> Path:
    preview = PROJECT_ROOT / "assets" / "last_image_preview.png"
    preview.parent.mkdir(parents=True, exist_ok=True)
    mono.save(preview, format="PNG")
    return preview


class ImageSenderApp:
    def __init__(self, root):
        import tkinter as tk
        from tkinter import ttk

        self.tk = tk
        self.ttk = ttk
        self.root = root
        self.root.title("EE05 4.2-inch ePaper Image Sender")
        self.root.geometry("760x690")
        self.root.minsize(680, 620)

        self.image_path: Path | None = None
        self.preview_photo = None
        self.events: queue.Queue[tuple[str, str]] = queue.Queue()

        outer = ttk.Frame(root, padding=14)
        outer.pack(fill="both", expand=True)

        ttk.Label(outer, text="EE05 Image Sender", font=("Segoe UI", 18, "bold")).pack(anchor="w")
        ttk.Label(outer, text="XIAO ePaper Display Board EE05 · 4.2-inch · 400 × 300 · Monochrome").pack(anchor="w", pady=(2, 12))

        file_row = ttk.Frame(outer)
        file_row.pack(fill="x", pady=4)
        ttk.Button(file_row, text="Choose image…", command=self.choose_image).pack(side="left")
        self.file_label = ttk.Label(file_row, text="No image selected", wraplength=530)
        self.file_label.pack(side="left", padx=10, fill="x", expand=True)

        settings = ttk.LabelFrame(outer, text="Conversion settings", padding=10)
        settings.pack(fill="x", pady=8)

        row1 = ttk.Frame(settings)
        row1.pack(fill="x", pady=3)
        ttk.Label(row1, text="COM port:", width=14).pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(row1, textvariable=self.port_var, state="readonly", width=24)
        self.port_combo.pack(side="left")
        ttk.Button(row1, text="Refresh ports", command=self.refresh_ports).pack(side="left", padx=8)

        row2 = ttk.Frame(settings)
        row2.pack(fill="x", pady=3)
        ttk.Label(row2, text="Image fit:", width=14).pack(side="left")
        self.fit_var = tk.StringVar(value="contain")
        self.fit_combo = ttk.Combobox(row2, textvariable=self.fit_var, state="readonly", width=24,
                                       values=("contain", "cover", "stretch"))
        self.fit_combo.pack(side="left")
        self.fit_combo.bind("<<ComboboxSelected>>", self.refresh_preview_if_selected)
        ttk.Label(row2, text="contain keeps the whole image; cover crops to fill.").pack(side="left", padx=8)

        row3 = ttk.Frame(settings)
        row3.pack(fill="x", pady=3)
        ttk.Label(row3, text="Conversion:", width=14).pack(side="left")
        self.dither_var = tk.StringVar(value="floyd")
        self.dither_combo = ttk.Combobox(row3, textvariable=self.dither_var, state="readonly", width=24,
                                         values=("floyd", "threshold"))
        self.dither_combo.pack(side="left")
        self.dither_combo.bind("<<ComboboxSelected>>", self.refresh_preview_if_selected)
        ttk.Label(row3, text="floyd for photos; threshold for logos/text.").pack(side="left", padx=8)

        row4 = ttk.Frame(settings)
        row4.pack(fill="x", pady=3)
        ttk.Label(row4, text="Threshold:", width=14).pack(side="left")
        self.threshold_var = tk.IntVar(value=128)
        self.threshold_spin = ttk.Spinbox(row4, from_=1, to=255, textvariable=self.threshold_var, width=8)
        self.threshold_spin.pack(side="left")
        self.threshold_spin.bind("<KeyRelease>", self.refresh_preview_if_selected)
        self.threshold_spin.bind("<FocusOut>", self.refresh_preview_if_selected)
        ttk.Label(row4, text="Used only with threshold conversion.").pack(side="left", padx=8)
        self.invert_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(row4, text="Invert black/white", variable=self.invert_var,
                        command=self.refresh_preview_if_selected).pack(side="left", padx=12)

        preview_frame = ttk.LabelFrame(outer, text="Monochrome preview (400 × 300)", padding=8)
        preview_frame.pack(fill="both", expand=True, pady=8)
        self.preview_label = ttk.Label(preview_frame, text="Choose an image to preview it here.", anchor="center")
        self.preview_label.pack(fill="both", expand=True)

        action_row = ttk.Frame(outer)
        action_row.pack(fill="x", pady=(6, 4))
        self.send_button = ttk.Button(action_row, text="Convert and send to EE05", command=self.start_send)
        self.send_button.pack(side="left")
        self.status_var = tk.StringVar(value="Ready. Flash the EE05 receiver firmware once before sending images.")
        ttk.Label(outer, textvariable=self.status_var, wraplength=700).pack(anchor="w", pady=(4, 0))

        self.refresh_ports()
        self.root.after(150, self.poll_events)

    def refresh_ports(self):
        if list_ports is None:
            self.status_var.set("PySerial is missing. In the VS Code terminal run: python -m pip install -r requirements.txt")
            self.port_combo["values"] = []
            return
        found = list(list_ports.comports())
        values = [f"{item.device} — {item.description}" for item in found]
        self.port_combo["values"] = values
        if values:
            current = self.port_var.get()
            if current not in values:
                self.port_var.set(values[0])
        else:
            self.port_var.set("")
            self.status_var.set("No serial port found. Connect the EE05 using a USB data cable.")

    def refresh_preview_if_selected(self, _event=None):
        if self.image_path is None:
            return
        try:
            self.update_preview()
        except Exception as exc:
            self.status_var.set(f"Preview error: {exc}")

    def choose_image(self):
        from tkinter import filedialog, messagebox
        filename = filedialog.askopenfilename(
            title="Select an image for the EE05 display",
            filetypes=[("Image files", "*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp"),
                       ("All files", "*.*")]
        )
        if not filename:
            return
        self.image_path = Path(filename)
        self.file_label.configure(text=str(self.image_path))
        try:
            self.update_preview()
        except Exception as exc:
            messagebox.showerror("Image conversion error", str(exc))

    def create_payload(self):
        if self.image_path is None:
            raise ValueError("Choose an image first.")
        mono = convert_to_monochrome(
            self.image_path,
            self.fit_var.get(),
            self.dither_var.get(),
            int(self.threshold_var.get()),
            bool(self.invert_var.get()),
        )
        return mono, pack_bitmap_msb_first(mono)

    def update_preview(self):
        mono, _ = self.create_payload()
        save_preview(mono)
        display_image = mono.convert("RGB")
        display_image.thumbnail((520, 390), Image.Resampling.LANCZOS)
        self.preview_photo = ImageTk.PhotoImage(display_image)
        self.preview_label.configure(image=self.preview_photo, text="")

    def start_send(self):
        from tkinter import messagebox
        selected = self.port_var.get().split(" — ", 1)[0].strip()
        if not self.image_path:
            messagebox.showwarning("Select an image", "Choose an image file first.")
            return
        if not selected:
            messagebox.showwarning("Select a COM port", "Connect the EE05 and refresh the COM port list.")
            return
        try:
            mono, payload = self.create_payload()
            save_preview(mono)
            display_image = mono.convert("RGB")
            display_image.thumbnail((520, 390), Image.Resampling.LANCZOS)
            self.preview_photo = ImageTk.PhotoImage(display_image)
            self.preview_label.configure(image=self.preview_photo, text="")
        except Exception as exc:
            messagebox.showerror("Conversion error", str(exc))
            return

        self.send_button.configure(state="disabled")
        self.status_var.set("Starting transfer…")
        worker = threading.Thread(target=self.send_worker,
                                  args=(selected, payload), daemon=True)
        worker.start()

    def send_worker(self, port: str, payload: bytes):
        try:
            send_bitmap(port, payload,
                        notify=lambda message: self.events.put(("status", message)))
            self.events.put(("done", "Image displayed successfully. Select another image to replace it."))
        except Exception as exc:
            self.events.put(("error", str(exc)))

    def poll_events(self):
        from tkinter import messagebox
        try:
            while True:
                kind, message = self.events.get_nowait()
                self.status_var.set(message)
                if kind in ("done", "error"):
                    self.send_button.configure(state="normal")
                if kind == "error":
                    messagebox.showerror("Transfer failed", message)
        except queue.Empty:
            pass
        self.root.after(150, self.poll_events)


def main() -> int:
    """Open the desktop image-sender UI. Run with: python ee05_image_app.py"""
    try:
        import tkinter as tk
        root = tk.Tk()
    except Exception as exc:
        print(f"Could not start the desktop interface: {exc}", file=sys.stderr)
        print("On Windows, install Python from python.org with Tcl/Tk support enabled.", file=sys.stderr)
        return 2

    ImageSenderApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
