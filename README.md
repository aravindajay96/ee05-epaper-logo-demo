# AGRO E-Paper Display Demo (Seeed Studio XIAO)

Firmware and assets for displaying the **AGRO (Robotics for Agriculture)** logo and handling interactive button triggers on a 400×300 E-Paper display driven by a **Seeed Studio XIAO** microcontroller.

<p align="center">
  <img src="Epaper_Display_Logo_preview.jpg" alt="Seeed Studio XIAO 4.2 inch ePaper Testing" width="500">
</p>

## Features

* **Custom Bitmap Rendering:** Displays a crisp 400×300 black-and-white bitmap (`logo_bitmap.h`).
* **Interactive Controls:** Supports onboard user buttons (`Key1`, `Key2`, `Key3`) to trigger display refreshes, screen inversion, and state changes.
* **Boot Diagnostics:** Displays initialization status (`EE05 OK`), panel resolution (`400 x 300`), and key press counters upon startup.

## Hardware Requirements

* **Microcontroller:** Seeed Studio XIAO (e.g., XIAO ESP32-C3 / S3 / nRF52840) with ePaper Breakout Board
* **Display:** 4.2" Monochrome E-Paper Panel (400×300 resolution, `EE05` driver)[cite: 2, 4]
* **Cable:** USB-C data cable

## Project Structure

```text
├── assets/
│   └── logo_400x300_bw.png   # Original 400x300 monochrome source image
├── src/
│   ├── driver.h              # E-Paper display pinout and driver configuration
│   ├── logo_bitmap.h         # C-array bitmap generated from the source PNG
│   └── main.cpp              # Main application logic and button handling
├── platformio.ini            # PlatformIO build configuration and dependencies
└── README.md                 # Project documentation
```

## Getting Started

### Prerequisites

* [VS Code](https://code.visualstudio.com/) with the [PlatformIO IDE](https://platformio.org/) extension installed.

### Build & Upload

1. Clone this repository:
   ```bash
   git clone [https://github.com/your-username/your-repo-name.git](https://github.com/your-username/your-repo-name.git)
   cd your-repo-name
