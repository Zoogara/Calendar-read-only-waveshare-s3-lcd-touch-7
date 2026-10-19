# Third-party notices

This project's own code is licensed under the Apache License 2.0 (see [LICENSE](LICENSE) and
[NOTICE](NOTICE)). It includes, builds on, or talks to the third-party material below, which
keeps its own licence. If you distribute **compiled firmware** (a `.bin`), ship this file and
the texts in [LICENSES/](LICENSES/) with it.

## 1. Included in this repository

| What | Where | Licence | Notes |
|---|---|---|---|
| **ESP-IDF `esp_lcd`** (v5.3.2) | `components/esp_lcd/` | Apache-2.0 | Copyright Espressif Systems (Shanghai) CO LTD. A vendored copy with **one modified file**, `rgb/esp_lcd_panel_rgb.c` (bounce-buffer streaming for the RGB panel and re-anchoring the refill at every frame restart, to stop tearing). Every other file is as shipped in ESP-IDF 5.3.2. The Apache-2.0 headers are kept. |
| **Montserrat Medium** | glyphs compiled into `components/calendar_ui/gcal_font_14.c`, `gcal_font_20.c`, `gcal_font_clock.c` | SIL OFL 1.1 ([text](LICENSES/OFL-1.1.txt)) | Copyright 2011 The Montserrat Project Authors (https://github.com/JulietaUla/Montserrat). Subset and rasterised to bitmaps with `lv_font_conv`. The font file itself is read from LVGL's `scripts/built_in_font/`, not stored here. No Reserved Font Name is declared. |
| **Font Awesome 5 Free** (Solid, Brands, Regular; 5.9.0 and 5.15.4) | glyphs in the same generated fonts, plus `gcal_font_icon_clock.c`, `gcal_font_icon_bell.c`; source `components/calendar_ui/fonts/fa-regular-400.woff` | Fonts: SIL OFL 1.1 ([text](LICENSES/OFL-1.1.txt)). Icons: CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/) | Copyright (c) Font Awesome, https://fontawesome.com. Glyphs were subset and rasterised; they are used unmodified in shape. Font Awesome's own statement is in [LICENSES/FontAwesome-Free-5.txt](LICENSES/FontAwesome-Free-5.txt). |
| **Weather Icons** v1.1 | glyphs in `components/calendar_ui/gcal_font_weather.c`, `gcal_font_weather_48.c`; source `components/calendar_ui/fonts/weathericons-regular-webfont.ttf` | SIL OFL 1.1 ([text](LICENSES/OFL-1.1.txt)) | Icon designs originally by Lukas Bischoff; icon art from v1.1 by Erik Flowers, https://github.com/erikflowers/weather-icons. Only the glyphs for the weather conditions the display maps are included. |
| **Board knowledge** | `components/board_bsp/` | (this project) | Written for this project from Waveshare's published documentation of the ESP32-S3-Touch-LCD-7 (https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7) - pin assignments - and, for the CH422G I/O expander, the I2C addresses and command values published in the community **ESPHome** `ch422g` component (https://esphome.io/components/ch422g.html), since the chip's datasheet is thin on protocol detail. Those are facts about the hardware; no Waveshare or ESPHome source code is included. |

## 2. Fetched at build time (not stored in this repository)

Pinned in `dependencies.lock`; `idf.py build` downloads them.

| What | Version | Licence |
|---|---|---|
| **ESP-IDF** | 5.3.2 | Apache-2.0. It carries its own third-party components, among them **cJSON** (MIT), **mbedTLS** (Apache-2.0), **lwIP** (BSD-3-Clause), **FreeRTOS** (MIT) and others; each is licensed in its own directory inside ESP-IDF. |
| **LVGL** | 8.4.0 | MIT - Copyright (c) 2021 LVGL Kft |
| **espressif/esp_lvgl_port** | 2.9.0 | Apache-2.0 |
| **espressif/esp_lcd_touch** | 1.2.1 | Apache-2.0 |
| **espressif/esp_lcd_touch_gt911** | 1.2.1 | Apache-2.0 |

Development tools that are used but not shipped: `lv_font_conv` (MIT) for generating the font sources.

## 3. Data sources, services and attribution

The firmware downloads data from the services below; nothing from them is stored in this repository. You are responsible for following their terms for your own use.

- **Google Calendar API** - read-only access to your own calendars through a service account you create. Subject to the Google APIs Terms of Service. "Google" and "Google Calendar" are trademarks of Google LLC.
- **Wikipedia - "On this day"** (https://en.wikipedia.org). The text comes from Wikipedia's *On this day* feed (Wikimedia REST API) and is available under CC BY-SA 4.0 (https://creativecommons.org/licenses/by-sa/4.0/). If you republish it, credit Wikipedia and share alike. The firmware identifies itself with a User-Agent that points to this repository, as Wikimedia asks of API clients.
- **Open-Meteo** (https://open-meteo.com) - weather on the `main` branch. Open-Meteo's data is CC BY 4.0 and free for non-commercial use; attribution is required and commercial use needs their subscription. Check their current terms.
- **Home Assistant** (https://www.home-assistant.io) - the `with-HA-weather` branch reads sensors from your own Home Assistant instance. "Home Assistant" is a trademark of the Open Home Foundation.
- **Australian Bureau of Meteorology (BOM)** - the BOM entities in Home Assistant carry the BOM's attribution ("Data provided by the Australian Bureau of Meteorology") and its terms: the data is the BOM's and is for your own use; do not copy or share it. In particular, **don't publish screenshots that show BOM forecasts** without checking the BOM's terms.
- **News feeds** - the RSS or Atom feeds you configure belong to their publishers. The display shows headings and short plain-text snippets for your own reading.
- **Waveshare**, **Espressif**, **ESP32** and the other names above are used only to describe what the project works with, and belong to their owners.

If you spot an attribution that is missing or wrong, please open an issue.
