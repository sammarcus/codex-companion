# LILYGO T-Display-S3 Hardware Recon

Source clones (local, read-only recon, no flashing, boards not attached):

- `vendor/T-Display-S3` = https://github.com/Xinyuan-LilyGO/T-Display-S3, commit `ec889e789b3cf093412689a143f7f37b42b56af7` (2026-08-31)
- `vendor/LovyanGFX` = https://github.com/lovyan03/LovyanGFX, commit `45dc36beb61f67dfd33a19954a1227acb02cad25` (2026-08-25)

All paths below are relative to `/Users/sam/claude/codex-buddy/vendor/`. Every value has a file+line cite. Nothing here was flashed or hardware-verified, it's static repo recon only.

---

## 1. GPIO pin header (canonical: `T-Display-S3/examples/factory/pin_config.h`)

`examples/factory/pin_config.h` is the fullest pin file in the repo (factory is the `default_envs` in `platformio.ini`, see section 3). Other examples' `pin_config.h` files carry an identical LCD/button/battery/IIC pinout; `diff examples/factory/pin_config.h examples/tft/pin_config.h` shows the only differences are factory's extra WiFi/NTP/LVGL macros and its SD-card block (factory has no SD, since factory doesn't use the SD port): the display and control pins are byte-identical across examples.

| Macro | Value | File : line |
|---|---|---|
| `PIN_LCD_BL` | 38 | `T-Display-S3/examples/factory/pin_config.h:33` |
| `PIN_LCD_D0` | 39 | `T-Display-S3/examples/factory/pin_config.h:35` |
| `PIN_LCD_D1` | 40 | `T-Display-S3/examples/factory/pin_config.h:36` |
| `PIN_LCD_D2` | 41 | `T-Display-S3/examples/factory/pin_config.h:37` |
| `PIN_LCD_D3` | 42 | `T-Display-S3/examples/factory/pin_config.h:38` |
| `PIN_LCD_D4` | 45 | `T-Display-S3/examples/factory/pin_config.h:39` |
| `PIN_LCD_D5` | 46 | `T-Display-S3/examples/factory/pin_config.h:40` |
| `PIN_LCD_D6` | 47 | `T-Display-S3/examples/factory/pin_config.h:41` |
| `PIN_LCD_D7` | 48 | `T-Display-S3/examples/factory/pin_config.h:42` |
| `PIN_POWER_ON` | 15 | `T-Display-S3/examples/factory/pin_config.h:44` |
| `PIN_LCD_RES` | 5 | `T-Display-S3/examples/factory/pin_config.h:46` |
| `PIN_LCD_CS` | 6 | `T-Display-S3/examples/factory/pin_config.h:47` |
| `PIN_LCD_DC` | 7 | `T-Display-S3/examples/factory/pin_config.h:48` |
| `PIN_LCD_WR` | 8 | `T-Display-S3/examples/factory/pin_config.h:49` |
| `PIN_LCD_RD` | 9 | `T-Display-S3/examples/factory/pin_config.h:50` |
| `PIN_BUTTON_1` | 0 | `T-Display-S3/examples/factory/pin_config.h:52` |
| `PIN_BUTTON_2` | 14 | `T-Display-S3/examples/factory/pin_config.h:53` |
| `PIN_BAT_VOLT` | 4 | `T-Display-S3/examples/factory/pin_config.h:54` |
| `PIN_IIC_SCL` | 17 | `T-Display-S3/examples/factory/pin_config.h:56` |
| `PIN_IIC_SDA` | 18 | `T-Display-S3/examples/factory/pin_config.h:57` |
| `PIN_TOUCH_INT` | 16 | `T-Display-S3/examples/factory/pin_config.h:59` |
| `PIN_TOUCH_RES` | 21 | `T-Display-S3/examples/factory/pin_config.h:60` |
| `PIN_SD_CMD` (external expansion) | 13 | `T-Display-S3/examples/factory/pin_config.h:63` |
| `PIN_SD_CLK` (external expansion) | 11 | `T-Display-S3/examples/factory/pin_config.h:64` |
| `PIN_SD_D0` (external expansion) | 12 | `T-Display-S3/examples/factory/pin_config.h:65` |

`PIN_LCD_RES` is the display reset pin (LovyanGFX calls this `pin_rst`, see section 4).

Note `PIN_LCD_RD`/`PIN_LCD_WR` (9/8) collide numerically with nothing else on the header; `PIN_TOUCH_RES` (21) is a separate reset line from `PIN_LCD_RES` (5): touch reset, not LCD reset. Cross-referenced identically in the LovyanGFX-based examples, e.g. `T-Display-S3/examples/T-Display-S3-Queue/ST7789_Handler.h:39-56` (see section 4).

---

## 2. Display offset / invert / rotation values used by the LILYGO examples

Two independent example code paths in the repo agree on the same panel geometry:

### 2a. `examples/factory/factory.ino` (ESP-IDF `esp_lcd` driver, the default build)

```
esp_lcd_panel_invert_color(panel_handle, true);      // factory.ino:310
esp_lcd_panel_swap_xy(panel_handle, true);            // factory.ino:312
esp_lcd_panel_mirror(panel_handle, false, true);      // factory.ino:315 (USB-on-left orientation)
// esp_lcd_panel_mirror(panel_handle, true, false);   // factory.ino:318 (commented alt: USB-on-right)
esp_lcd_panel_set_gap(panel_handle, 0, 35);           // factory.ino:322 (gap_x=0, gap_y=35)
```
File: `T-Display-S3/examples/factory/factory.ino:310-322`. Panel resolution macros used to size buffers: `EXAMPLE_LCD_H_RES 320`, `EXAMPLE_LCD_V_RES 170` (`T-Display-S3/examples/factory/pin_config.h:27-28`).

### 2b. LovyanGFX-based examples (`ST7789_Handler.h`, several sibling examples)

```
cfg.offset_rotation = 1;      // ST7789_Handler.h:58
cfg.offset_x = 35;            // ST7789_Handler.h:59
cfg.readable = false;         // ST7789_Handler.h:60
cfg.invert = true;            // ST7789_Handler.h:61
cfg.rgb_order = false;        // ST7789_Handler.h:62
cfg.dlen_16bit = false;       // ST7789_Handler.h:63
cfg.bus_shared = false;       // ST7789_Handler.h:64
cfg.panel_width = 170;        // ST7789_Handler.h:66
cfg.panel_height = 320;       // ST7789_Handler.h:67
```
File: `T-Display-S3/examples/T-Display-S3-Queue/ST7789_Handler.h:58-67` (byte-identical `offset_x = 35` block also in `examples/T-Display-S3-Gingoduino/ST7789_Handler.h:59` and `examples/T-Display-S3-USB-Device/ST7789_Handler.h:77`; the Piano/BLE variants set `offset_rotation=1; offset_x=35` inline at `examples/T-Display-S3-Piano/PianoDisplay.h:106`, `examples/T-Display-S3-Piano-Debug/T-Display-S3-Piano-Debug.ino:42`, `examples/T-Display-S3-BLE-Sender/SenderDisplay.h:94`, `examples/T-Display-S3-BLE-Receiver/PianoDisplay.h:91`). None of these set `offset_y` explicitly, so it stays at the LovyanGFX default of 0 (`LovyanGFX/src/lgfx/v1/panel/Panel_Device.hpp:73`, see section 4).

**Reconciling the two:** the IDF path uses `swap_xy(true)` + `set_gap(x=0, y=35)` in the panel's native (portrait, memory) coordinate frame before rotation; the LovyanGFX path expresses the same physical offset as `offset_x=35` (its landscape frame after `offset_rotation=1`) with `panel_width/height` already declared as the rotated 170x320. Both encode "35px offset on the long axis, 0 on the short axis" for this panel. `offset_x` in the LovyanGFX cfg is not the same axis as IDF's `set_gap` x-arg; do not port one value to the other's field name without accounting for the axis swap.

### 2c. `examples/tft/tft.ino` (TFT_eSPI path)
Only sets rotation (`tft.setRotation(3)`, `:64`), no runtime gap call. Offsets in
that path are baked into the TFT_eSPI User_Setup via `CGRAM_OFFSET` and the
driver's ST7789 rotation tables. Not the path this firmware takes.

---

## 3. `platformio.ini` (repo root: `T-Display-S3/platformio.ini`)

- `default_envs = factory`: `platformio.ini:15` (all other envs commented out as alternatives, lines 16-33)
- `src_dir = examples/${platformio.default_envs}`: `platformio.ini:35`
- `[env]` (shared base, `platformio.ini:38-61`):
  - `platform = espressif32@6.5.0`: `:39`
  - `board = lilygo-t-display-s3`: `:40` (resolves to `T-Display-S3/boards/lilygo-t-display-s3.json`, see below)
  - `framework = arduino`: `:41`
  - `debug_tool = esp-builtin`: `:43`
  - `upload_protocol = esptool`: `:46` (comment at `:48-49` notes `upload_protocol = esp-builtin` as the alternative "when using ESP32-USB-JTAG debugging")
  - `build_flags` (`:51-61`):
    ```
    -DLV_LVGL_H_INCLUDE_SIMPLE
    -DARDUINO_USB_CDC_ON_BOOT=1
    ; -UARDUINO_USB_CDC_ON_BOOT      (commented alt)
    -DDISABLE_ALL_LIBRARY_WARNINGS
    -DARDUINO_USB_MODE=1
    -DTOUCH_MODULES_CST_MUTUAL       ; "Early use of CST328"
    ; -DTOUCH_MODULES_CST_SELF       (commented alt, "Use CST816 by default")
    ```
- `[env:factory]` (`:81-89`) adds no extra build_flags beyond the shared `[env]` block, only `lib_ignore` (TFT_eSPI, GFX Library for Arduino, arduino-nofrendo, Adafruit MPR121, DabbleESP32, PCF8575 library, PCA95x5).
- `[env:usb_hid_pad]` overrides to `-DARDUINO_USB_MODE=0` (native USB HID mode instead of CDC): `:196`.
- `[env:ota]` sets `upload_protocol = espota` and a hardcoded `upload_port = 192.168.36.172` (placeholder, meant to be edited): `:206-208`.
- LovyanGFX-using envs (`T-Display-S3-Piano`, `T-Display-S3-Queue`, `T-Display-S3-Gingoduino`, `T-Display-S3-BLE-Receiver`, `T-Display-S3-BLE-Sender`, `T-Display-S3-USB-Device`) each declare `lib_deps = lovyan03/LovyanGFX` (e.g. `:233-234`, `:292-293`): this is the ONLY place `lib_deps` appears pulling LovyanGFX; the default `factory` env does not depend on LovyanGFX (it uses the ESP-IDF `esp_lcd` API directly, section 2a).
- No `board_build.partitions` override anywhere in `platformio.ini`: partition table comes from the board JSON default (below), not overridden per-env.

### Board definition: `T-Display-S3/boards/lilygo-t-display-s3.json`
```
build.arduino.ldscript      = "esp32s3_out.ld"        (line 4)
build.arduino.memory_type   = "qio_opi"                (line 5)
build.arduino.partitions    = "default_16MB.csv"       (line 6)   <- partition CSV
build.core                  = "esp32"                  (line 8)
build.extra_flags           = ["-DBOARD_HAS_PSRAM"]    (line 10)
build.f_cpu                 = "240000000L"             (line 12)
build.f_flash                = "80000000L"             (line 13)
build.flash_mode            = "qio"                    (line 14)
build.hwids                 = [["0X303A", "0x1001"]]   (lines 16-19)  <- USB VID/PID, see section 6
build.mcu                   = "esp32s3"                (line 21)
upload.flash_size           = "16MB"                   (line 37)
upload.maximum_ram_size     = 327680                   (line 38)
upload.maximum_size         = 16777216                 (line 39)
upload.require_upload_port  = true                     (line 40)
upload.speed                = 921600                   (line 41)
```
`default_16MB.csv` is the stock PlatformIO/Arduino-ESP32 16MB partition table name; the CSV file itself is not vendored inside this repo (it ships with the `espressif32` PlatformIO platform package), so its exact partition layout is UNCONFIRMED from this clone: only the filename reference is confirmed, at `boards/lilygo-t-display-s3.json:6`.

---

## 4. LovyanGFX `Bus_Parallel8` + `Panel_ST7789` config, in-repo (LILYGO) usage vs library defaults

### 4a. In-repo LILYGO usage (`T-Display-S3/examples/T-Display-S3-Queue/ST7789_Handler.h`)
Bus config (`:37-50`):
```cpp
auto cfg = _bus_instance.config();
cfg.pin_wr = 8;      // :39  == PIN_LCD_WR
cfg.pin_rd = 9;      // :40  == PIN_LCD_RD
cfg.pin_rs = 7;       // :41  D/C == PIN_LCD_DC
cfg.pin_d0 = 39;      // :42
cfg.pin_d1 = 40;      // :43
cfg.pin_d2 = 41;      // :44
cfg.pin_d3 = 42;      // :45
cfg.pin_d4 = 45;      // :46
cfg.pin_d5 = 46;      // :47
cfg.pin_d6 = 47;      // :48
cfg.pin_d7 = 48;      // :49
_bus_instance.config(cfg);
_panel_instance.setBus(&_bus_instance);
```
No `freq_write` override in this example: it takes the library default of 16 MHz (section 4b). This is consistent with `factory.ino`'s IDF-path `EXAMPLE_LCD_PIXEL_CLOCK_HZ = 16 * 1000 * 1000` (`T-Display-S3/examples/factory/pin_config.h:25`): both code paths agree on a 16 MHz parallel write clock for this panel.

Panel config (`:53-68`):
```cpp
auto cfg = _panel_instance.config();
cfg.pin_cs = 6;              // :55  == PIN_LCD_CS
cfg.pin_rst = 5;              // :56  == PIN_LCD_RES
cfg.pin_busy = -1;            // :57  (no BUSY line on this panel)
cfg.offset_rotation = 1;      // :58
cfg.offset_x = 35;            // :59
cfg.readable = false;         // :60
cfg.invert = true;            // :61
cfg.rgb_order = false;        // :62
cfg.dlen_16bit = false;       // :63
cfg.bus_shared = false;       // :64
cfg.panel_width = 170;        // :66
cfg.panel_height = 320;       // :67
```
Backlight (`:71-78`): `pin_bl = 38` (== `PIN_LCD_BL`), `invert = false`, `freq = 22000`, `pwm_channel = 7`.

No `dummy_read_pixel`/`dummy_read_bits` override in this example: takes the `Panel_ST7789` class default of `dummy_read_pixel = 16` (section 4b), since `readable = false` here anyway (dummy-read only matters when read-back is enabled).

### 4b. LovyanGFX library defaults / field definitions (for fields the LILYGO examples don't override)
- `Bus_Parallel8::config_t` (ESP32-S3 platform variant), `LovyanGFX/src/lgfx/v1/platforms/esp32s3/Bus_Parallel8.hpp:39-68`:
  - `freq_write = 16000000`: `:45` ("max 80MHz" per adjacent comment)
  - `freq_read = 8000000`: `:46`
  - `pin_ctrl[3]` union = `{pin_rd, pin_wr, pin_rs}`: `:47-55`
  - `pin_data[8]` union = `{pin_d0..pin_d7}`: `:56-67`
  - `port = 0`: `:41` ("LCD_CAM peripheral number... only 0 for ESP32-S3")
- `Panel_Device::config_t` (base class all panels inherit), `LovyanGFX/src/lgfx/v1/panel/Panel_Device.hpp:55-105`:
  - `memory_width/memory_height` default 240 (overridden per-panel-class)
  - `panel_width/panel_height` default 240 (overridden by LILYGO examples to 170/320)
  - `offset_x = 0`: `:69`
  - `offset_y = 0`: `:73` (LILYGO examples never set this, so it stays 0)
  - `offset_rotation = 0`: `:77`
  - `dummy_read_pixel = 8`: `:81`
  - `dummy_read_bits = 1`: `:85`
  - `readable = true`: default, overridden to `false` by LILYGO examples
  - `invert = false`: default, overridden to `true` by LILYGO examples
  - `rgb_order = false`: matches LILYGO's explicit `false`
  - `dlen_16bit = false`: matches LILYGO's explicit `false`
  - `bus_shared = true`: default, overridden to `false` by LILYGO examples
- `Panel_ST7789` class-specific constructor override, `LovyanGFX/src/lgfx/v1/panel/Panel_ST7789.hpp:29-33`:
  ```cpp
  Panel_ST7789(void) {
    _cfg.panel_height = _cfg.memory_height = 320;
    _cfg.dummy_read_pixel = 16;
  }
  ```
  i.e. the `Panel_ST7789` class itself bumps `dummy_read_pixel` from the base-class default of 8 to 16, and sets `panel_height`/`memory_height` to 320 before user code runs: the LILYGO example then explicitly re-sets `panel_width = 170` on top of this (memory_width stays whatever `Panel_LCD`/`Panel_ST7789` sets it to; not itself overridden in `ST7789_Handler.h`).

### 4c. No dedicated LovyanGFX board file for this exact board variant
`LovyanGFX/src/lgfx_user/` contains `Lilygo_T_Display_S3_AMOLED.hpp` (grep hit): that is the **AMOLED** T-Display-S3 variant (a different LILYGO product, RM67162 panel per `LovyanGFX/src/lgfx/v1/panel/Panel_RM67162.hpp`), not the plain parallel-ST7789 T-Display-S3 this recon covers. There is no `LGFX_TDisplayS3`/`board_LILYGO_TDISPLAYS3` entry in `LovyanGFX/src/lgfx/boards.hpp` (grep for `TDISPLAY`/`T_DISPLAY` returned zero matches): LovyanGFX has no autodetect/boards.hpp entry for the plain ST7789 T-Display-S3, so the in-repo LILYGO `ST7789_Handler.h`-style hand-written `LGFX_Device` subclass (section 4a) is the only LovyanGFX config path for this board found in either clone.

`examples/HowToUse/2_user_setting/2_user_setting.ino` (the file the task named) is a generic/Japanese-commented SPI-panel template (257 lines): its `Bus_Parallel8` reference is a single commented-out alternative line (`:50`, `//lgfx::Bus_Parallel8  _bus_instance;`), not a filled-in T-Display-S3 config; it carries no `freq_write`/`dummy_read`/`offset_x` values applicable to this board. The T-Display-S3-specific parallel8 config is the LILYGO in-repo `ST7789_Handler.h` (section 4a), not this generic example.

---

## 5. TFT_eSPI setup, for cross-checking only

`T-Display-S3/lib/TFT_eSPI/User_Setups/Setup206_LilyGo_T_Display_S3.h` (48
lines) declares `ST7789_DRIVER`, `CGRAM_OFFSET`, `TFT_RGB_ORDER TFT_RGB`,
`TFT_INVERSION_ON`, `TFT_PARALLEL_8_BIT`, `TFT_WIDTH 170`, `TFT_HEIGHT 320`, and
the same pins as `pin_config.h` **pin for pin**, which is the independent
confirmation of section 1. This firmware uses LovyanGFX, not TFT_eSPI, so the
rest of that file only matters if something switches libraries. `CGRAM_OFFSET`
is TFT_eSPI's own analogue of the `offset_x` in sections 2 and 4; the exact
pixel count it applies per rotation lives in `TFT_Drivers/ST7789_Rotation.h` and
was never traced.

---

## 6. Panel resolution / memory width

- Physical panel: **170 x 320** (portrait native / 320x170 landscape after rotation), confirmed independently in three places:
  - `T-Display-S3/lib/TFT_eSPI/User_Setups/Setup206_LilyGo_T_Display_S3.h:18-19` (`TFT_WIDTH 170`, `TFT_HEIGHT 320`)
  - `T-Display-S3/examples/T-Display-S3-Queue/ST7789_Handler.h:66-67` (`panel_width = 170`, `panel_height = 320`)
  - `T-Display-S3/examples/factory/pin_config.h:27-28` (`EXAMPLE_LCD_H_RES 320`, `EXAMPLE_LCD_V_RES 170`)
- LovyanGFX `Panel_ST7789` class sets `memory_height = 320` in its constructor (`LovyanGFX/src/lgfx/v1/panel/Panel_ST7789.hpp:31`); `memory_width` is not touched by that constructor and is not explicitly set by the LILYGO example either, so it stays at the `Panel_Device` base default of `240` (`LovyanGFX/src/lgfx/v1/panel/Panel_Device.hpp:52`, the line right above the excerpt in section 4b) unless some other base class in the `Panel_LCD`/`Panel_ST7789` chain overrides it: not confirmed further than that from this recon (would need to read `Panel_LCD.hpp`'s constructor too, not done here). Flag this one line as UNCONFIRMED: **`memory_width` for `Panel_ST7789` as used by the LILYGO example is not verified to be 170 vs the base-class 240 default**: the example never sets it explicitly and this recon didn't trace `Panel_LCD.hpp`.

---

## 7. USB CDC vendor/product ID

Confirmed directly from the vendored board definition, no core install present to cross-check against:
```
"hwids": [["0X303A", "0x1001"]]
```
`T-Display-S3/boards/lilygo-t-display-s3.json:16-19`. VID `0x303A` = Espressif Systems' USB VID; PID `0x1001` is Espressif's generic "ESP32-S3 USB-CDC/JTAG native" default PID used across many ESP32-S3 boards, not a LILYGO-specific PID. This is confirmed from the vendored repo tree itself (not a guess), but no local copy of the Arduino-ESP32 core (`hardware/espressif/esp32` or the `arduino-esp32` package) was found on disk to independently cross-check the PID's exact CDC-descriptor string against: that broader "is this the exact PID Arduino-ESP32's USB stack burns into the CDC descriptor at runtime" claim is UNCONFIRMED beyond the board-json literal above.

Corroborating build flag confirming native USB CDC is enabled by default: `-DARDUINO_USB_CDC_ON_BOOT=1` in `platformio.ini:53` (see section 3); `[env:usb_hid_pad]` flips this off (`-DARDUINO_USB_MODE=0`, `platformio.ini:196`) for that one example, meaning that example enumerates as native USB HID instead of CDC, likely under a different PID at the OS level than `0x1001`: but no PID override for that env is present in `platformio.ini` or the board json, so what PID it would actually enumerate as is UNCONFIRMED from this repo (would come from the Arduino-ESP32 core's runtime USB descriptor code, not vendored here).

---

## 8. What is still unconfirmed

Five items were flagged UNCONFIRMED by this recon and they live in
`docs/open-questions.md` now, with the command that settles each: the contents
of `default_16MB.csv` (H1), the TFT_eSPI `CGRAM_OFFSET` per rotation (moot, this
firmware uses LovyanGFX directly), `Panel_ST7789`'s effective `memory_width` as
used by the LILYGO example (H3, 170 vs the base-class default of 240, never
traced through `Panel_LCD.hpp`), whether `0x1001` is the literal runtime CDC
descriptor PID (H4, resolved on hardware: a real board reports `303a:1001`
through `ioreg`), and what PID the `usb_hid_pad` env would enumerate as (moot,
the shipping env stays in CDC mode).
