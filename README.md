# Ham Clock for the Cheap Yellow Display

A desk clock for the ham shack, made for the ESP32-2432S028R "Cheap Yellow Display" (CYD),
a cheap ESP32 board with a 2.8" 320×240 touch screen.

Tap the screen to change pages:

- **Clock**: local time, UTC, date, solar flux, sunspots, A and K index, and HF conditions
  for right now (day or night at your location).
- **Band conditions**: day and night conditions for 80–10 m, plus geomagnetic field, noise
  level, X-ray, solar wind, aurora and North American E-skip.
- **Gray line**: a world map with live day/night shading, the sun's position, your location,
  and today's sunrise and sunset.

Solar and band data come from [N0NBH / hamqsl.com](https://www.hamqsl.com/solar.html) and
are fetched every 30 minutes. Everything else is calculated on the board.

## Install

### Option 1: flash the ready-made firmware (no tools to install)

1. Download `hamclock-cyd.bin` from the [latest release](../../releases/latest).
2. Plug the CYD into your computer with a USB data cable.
3. Open <https://espressif.github.io/esptool-js/> in Chrome or Edge, click **Connect**, and
   pick the CYD's serial port (usually CH340 or CP210x).
4. Set **Flash Address** to `0x0`, choose `hamclock-cyd.bin`, and click **Program**.

Or, from a terminal with [esptool](https://github.com/espressif/esptool):

```bash
esptool --port /dev/ttyUSB0 write-flash 0x0 hamclock-cyd.bin
```

`hamclock-cyd.bin` is a full install: it also clears saved Wi-Fi and settings, so you'll go
through setup again. To update a clock that's already set up and keep its settings, flash
`hamclock-cyd-update.bin` at address `0x10000` instead.

### Option 2: build it yourself

With [PlatformIO](https://platformio.org/):

```bash
git clone https://github.com/F0tia/ham-clock-cyd
cd ham-clock-cyd
pio run -t upload
```

## Set up

On first start, the clock opens its own Wi-Fi network for setup:

1. On your phone, join the Wi-Fi network **HamClock-Setup**. A setup page opens; if it
   doesn't, browse to `192.168.4.1`.
2. Tap **Configure WiFi**, pick your network, and enter its password.
3. Fill in:
   - **Callsign**
   - **Location**: your grid square (`FN31` or `FN31pr`) or latitude,longitude
     (`41.71,-72.73`). It's used for the map marker, sunrise and sunset, and day/night
     band conditions.
   - **Time zone**
   - **Clock**: 12-hour or 24-hour
   - **Change page every N seconds**: `0` keeps a page until you tap the screen.
4. Tap **Save**.

Settings are stored on the board and survive power cuts and updates (see the note on
updating above).

**To change settings later**, browse to the clock's address from any device on your network.
The address is shown at the bottom of the Band conditions page, for example
`http://192.168.0.50`. Open **Setup**, make your changes and save. They apply right away.

**To change Wi-Fi networks**, hold your finger on the screen while the clock starts up. It
opens the **HamClock-Setup** network again.

## Hardware notes

Built for the common single-USB ESP32-2432S028R with an ILI9341 screen. If your colours look
inverted or the screen stays blank (some two-USB-port boards use an ST7789), change
`-DILI9341_2_DRIVER=1` to `-DST7789_DRIVER=1` in `platformio.ini` and build it yourself.

## Credits

- Solar and band data: Paul Herrman, N0NBH, [hamqsl.com](https://www.hamqsl.com/solar.html)
- World map: [Natural Earth](https://www.naturalearthdata.com/) 110m land (public domain), via
  [world-atlas](https://github.com/topojson/world-atlas)
- Libraries: [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI),
  [WiFiManager](https://github.com/tzapu/WiFiManager),
  [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen)

## License

MIT
