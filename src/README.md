# EE05 4.2-inch ePaper logo bitmap project

This project displays the supplied transparent logo image on the Seeed Studio
XIAO ePaper Display Board EE05 v1.11 with the 4.2-inch 400x300 monochrome panel.

## Display result

- `assets/logo_400x300_bw.png` is a preview at the panel's 400x300 resolution.
- `src/logo_bitmap.h` contains the matching packed 1-bit bitmap.
- The transparent PNG background becomes white.
- Both green and dark-grey artwork become black because this is a monochrome panel.
- Anti-aliased edges are converted with Floyd-Steinberg dithering.
- The bitmap occupies 15,000 bytes of flash and is declared in PROGMEM.

## Buttons

- Key1 / GPIO2: black logo on white background.
- Key2 / GPIO3: inverted logo (white on black).
- Key3 / GPIO8: clear the display to white.

The buttons use the EE05's external pull-ups and are active LOW.

## Build

Open this folder in VS Code with PlatformIO installed, then run:

```powershell
pio run -e ee05_4in2
```

Upload:

```powershell
pio run -e ee05_4in2 -t upload
```

Serial monitor:

```powershell
pio device monitor -b 115200
```

This project uses Seeed_GFX (the `TFT_eSPI.h` / `EPaper` API), not Seeed_GFX2.
Keep the working PlatformIO Core version and board settings from the project
that previously built successfully.

## Notes

The preview is a 1-bit rendering for the 400x300 panel, not a colour rendering.
Small tagline text and tiny decorative speckles may be less distinct at this
resolution. ePaper full-screen refresh is expected to take noticeably longer
than drawing into the RAM buffer.
