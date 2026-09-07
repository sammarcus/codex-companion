// LovyanGFX device config for the LILYGO T-Display-S3 (non-touch, ST7789,
// 8-bit parallel bus). Every pin and panel value below is taken from
// docs/hardware-recon.md, which cites the vendor sources:
//
//   vendor/T-Display-S3/examples/factory/pin_config.h            (pin numbers)
//   vendor/T-Display-S3/examples/T-Display-S3-Queue/ST7789_Handler.h
//                                                               (bus + panel + light cfg)
//
// LovyanGFX has no boards.hpp autodetect entry for this board variant, so a
// hand-written LGFX_Device subclass is the only supported config path.

#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// --- pin map (hardware-recon.md section 1) ---------------------------------
#define PIN_LCD_BL     38
#define PIN_LCD_D0     39
#define PIN_LCD_D1     40
#define PIN_LCD_D2     41
#define PIN_LCD_D3     42
#define PIN_LCD_D4     45
#define PIN_LCD_D5     46
#define PIN_LCD_D6     47
#define PIN_LCD_D7     48
#define PIN_POWER_ON   15
#define PIN_LCD_RES     5
#define PIN_LCD_CS      6
#define PIN_LCD_DC      7
#define PIN_LCD_WR      8
#define PIN_LCD_RD      9
#define PIN_BUTTON_1    0
#define PIN_BUTTON_2   14

class LGFX_TDisplayS3 : public lgfx::LGFX_Device {
  lgfx::Bus_Parallel8 _bus_instance;
  lgfx::Panel_ST7789  _panel_instance;
  lgfx::Light_PWM     _light_instance;

public:
  LGFX_TDisplayS3(void) {
    {  // 8-bit parallel bus
      auto cfg = _bus_instance.config();
      cfg.freq_write = 16000000;   // 16 MHz, matches both vendor code paths
      cfg.pin_wr = PIN_LCD_WR;
      cfg.pin_rd = PIN_LCD_RD;
      cfg.pin_rs = PIN_LCD_DC;     // D/C
      cfg.pin_d0 = PIN_LCD_D0;
      cfg.pin_d1 = PIN_LCD_D1;
      cfg.pin_d2 = PIN_LCD_D2;
      cfg.pin_d3 = PIN_LCD_D3;
      cfg.pin_d4 = PIN_LCD_D4;
      cfg.pin_d5 = PIN_LCD_D5;
      cfg.pin_d6 = PIN_LCD_D6;
      cfg.pin_d7 = PIN_LCD_D7;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {  // ST7789 panel
      auto cfg = _panel_instance.config();
      cfg.pin_cs          = PIN_LCD_CS;
      cfg.pin_rst         = PIN_LCD_RES;
      cfg.pin_busy        = -1;
      cfg.offset_rotation = 1;     // vendor value: puts native portrait into landscape
      cfg.offset_x        = 35;    // panel RAM offset on the long axis
      cfg.offset_y        = 0;
      cfg.readable        = false;
      cfg.invert          = true;
      cfg.rgb_order       = false;
      cfg.dlen_16bit      = false;
      cfg.bus_shared      = false;
      cfg.panel_width     = 170;
      cfg.panel_height    = 320;
      // memory_width / memory_height are deliberately NOT set: the vendor
      // example does not set them either, so the Panel_ST7789 constructor
      // default (memory_height=320) and Panel_Device default (memory_width=240)
      // are what the proven LILYGO config actually runs with.
      _panel_instance.config(cfg);
    }
    {  // backlight
      auto cfg = _light_instance.config();
      cfg.pin_bl      = PIN_LCD_BL;
      cfg.invert      = false;
      cfg.freq        = 22000;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    setPanel(&_panel_instance);
  }
};
