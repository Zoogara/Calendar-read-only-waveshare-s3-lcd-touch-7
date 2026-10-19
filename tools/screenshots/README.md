# Screenshot renderer

Renders the **real** calendar UI - the same LVGL widgets, fonts and layout code the firmware runs
- on your computer, with **fictional** demo data, and writes PNGs. It is how the images in
`docs/` are made. It is host-only: nothing here is part of the firmware build.

```
make -C tools/screenshots shots      # builds ./render, writes tools/screenshots/out/*.png
```

Needs a C compiler, Python 3 with Pillow (`pip install pillow`), and LVGL 8.4 - which a normal
`idf.py build` fetches into `managed_components/lvgl__lvgl` (or set `LVGL=/path/to/lvgl`).

## How it works

* `Makefile` compiles LVGL and `components/calendar_ui` (plus `event_store.c`)
  for the host. LVGL gets the same settings as the device: `shim/sdkconfig.h` is
  the `CONFIG_LV_*` part of the firmware's generated `sdkconfig.h`.
* `shim/` holds stand-in headers for ESP-IDF, FreeRTOS and `board_bsp` (logging and locks do
  nothing; there is no hardware).
* `host_stubs.c` stands in for the hardware and network modules; `demo_data.c` fills the
  event store and the weather, graph, "on this day" and feed accessors with invented data.
* `host_calendar_ui.c` and `host_screensaver.c` compile two firmware files in-place so
  `render.c` can reach their private state (which view is showing, the ambient clock) without
  changing the firmware source.
* The wall clock is pinned (`demo_set_now`) to Wed 14 Oct 2026, 14:35 in a made-up
  time zone context, so the images are reproducible.
* `render.c` drives each scene and writes a PPM; `topng.py` converts them to PNG.

## The demo data

Everything is invented: the town ("Port Hartley"), the household, its events, the news items
and the weather curves. The "On this day" entries are well-known public history. Nothing comes
from a real calendar, sensor or location.

## Changing it

Add or edit events in `demo_fill_events()`, weather in `demo_build_data()`, scenes in
`main()` (`render.c`). Output goes to `tools/screenshots/out/` (git-ignored); copy the ones you
want into `docs/`.
