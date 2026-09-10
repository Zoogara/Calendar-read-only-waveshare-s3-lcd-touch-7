**Caveat:** Developed in conjunction with Claude AI.

# Google Calendar wall display — Waveshare ESP32-S3-Touch-LCD-7

An ESP-IDF + LVGL project that turns the [Waveshare ESP32-S3-Touch-LCD-7](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7)
(800x480 IPS touch panel) into a wall-mounted calendar, similar in spirit to
the Google Calendar Android app: Month / Week / Day views you can drill into
by tapping a day, an "Up next" list, and multiple calendars shown side by
side, each in its own colour.

**Important, read first:** this has since been built, flashed, and run
extensively on real hardware (an actual Waveshare ESP32-S3-Touch-LCD-7,
against ESP-IDF v5.3.2) - display/touch/backlight bring-up, Wi-Fi
provisioning, the Google Calendar and ICS-feed fetch paths, the TF card and
its config backup, the on-device settings dialog, and the idle screensaver
have all been exercised and confirmed working, not just reviewed on paper.
The items under **Bring-up troubleshooting** below are a mix of real
findings from that hardware bring-up (several are called out inline as
"confirmed on real hardware") and a few things that never needed a tweak
but are still worth knowing about if you hit them on a different board
revision or IDF version.

Known, still-open issues (four others - the ambient clock's once-a-minute
digit change occasionally tearing, a boot-time hang/crash inside
`update_title()`, a recurring `LoadProhibited` panic inside LVGL's layout
pass, and intermittent calendar sync failures tied to internal-RAM
headroom - were root-caused and fixed; see "Ambient clock digit tearing
(fixed)", "update_title() boot-time crash (fixed)", "Unsynchronized LVGL
construction at boot (fixed)", and "Calendar sync reliability (internal-RAM
headroom) (fixed)" below):

- An intermittent crash right after the SD card mounts at boot, landing in
  LVGL's own background redraw task - self-recovers via the panic
  handler's own automatic reboot within about a second, on roughly a
  quarter to two-fifths of boots in repeated testing. It has never once
  failed to recover in testing (never a hard loop), and root cause hasn't
  been pinned down after a real investigation (stack size, heap
  corruption, an NVS-vs-LVGL-task race, and SD SPI clock speed were all
  ruled out) - see the comment above the settling-delay `vTaskDelay()` in
  `main/main.c`'s `app_main()` for the full writeup and the leading
  remaining theory (a GDMA channel-sharing interaction between the SD SPI
  bus and the RGB panel's own continuous-refresh DMA).

### update_title() boot-time crash (fixed)

`calendar_ui.c`'s `update_title()` calls `lv_refr_now()` twice, synchronously,
as part of its own dual-framebuffer sync dance (documented in its own
comment) - and it runs unconditionally on every `select_view()` call,
including the one at boot, inside `calendar_ui_init()`. That's exactly the
category of problem `select_view_force_redraw()`'s own doc comment already
warned about: calling `lv_refr_now()` before LVGL's redraw timer has ticked
even once wedges LVGL's buffer-sync logic, because nothing's ready yet for
the synchronous `refr_sync_areas()`/`lv_draw_sw_buffer_copy()` path it
forces. `select_view_force_redraw()` was deliberately never called that
early for exactly this reason, but `update_title()` wasn't written with the
same guard, since it's called from many more places than just view
switches.

First seen (2026-09-05) as a non-self-recovering watchdog hang - this
board's task watchdog isn't configured to reset on timeout, so it needed a
manual reset. Seen again (2026-09-09) as a hard `Guru Meditation Error:
Cache disabled but cached memory region accessed` panic instead - same root
cause hit at the same call site, just a different failure mode depending on
what else was going on at that exact moment (in this case, right at boot,
likely mid something else in the early-flash-cache-sensitive window).

Fixed by gating `update_title()`'s two `lv_refr_now()` calls behind a new
`s_boot_forced_redraw_ok` flag, set `true` right after
`calendar_ui_init()`'s own initial `select_view()` call returns. Before
that point a plain `lv_obj_invalidate()` (no forced synchronous refresh) is
enough - nothing's been shown on screen yet at boot, so there's no stale
second buffer to catch up on, and the next regular LVGL timer tick paints
the very first frame correctly on its own. Every other caller of
`update_title()` runs after that flag flips, so this only changes behaviour
during the exact narrow window that was actually unsafe.

### Unsynchronized LVGL construction at boot (fixed)

A recurring `Guru Meditation Error: Core 0 panic'ed (LoadProhibited)`,
always inside LVGL's own periodic redraw pass (`_lv_disp_refr_timer` →
`layout_update_core`, recursing several levels deep into the object tree)
but always in a *different* leaf function - `lv_obj_get_child_cnt()`,
`get_prop_core()` (a style property read), `lv_obj_get_scroll_bottom()`,
`lv_obj_scrollbar_invalidate()` - with no single obviously-wrong call site
in common between occurrences. First seen once, then recurring more often
(three times in one short burst) during 2026-09-09's testing.

Two theories were tested and ruled out before finding the real cause,
each still worth keeping in mind for anything similar in the future:

- **Heap corruption** (an out-of-bounds write or heap-metadata corruption
  making the object tree's own memory garbage) - ruled out by briefly
  enabling `CONFIG_HEAP_POISONING_COMPREHENSIVE`, which wraps every heap
  block in canary bytes and fully verifies heap integrity on every
  malloc/free/realloc. It never caught a corruption event before two
  further crashes while it was on - a genuinely useful negative result,
  since a real overflow or metadata corruption would have aborted
  immediately with a precise stack trace pointing at the actual bad
  write. (Heap poisoning has real overhead - enough, on this board's
  already-tight internal-RAM budget, to break TLS certificate
  verification on calendar sync. Not something to leave on; re-enable
  only to chase something similarly corruption-shaped, then turn it back
  off.)
- **A stale pointer to a validly-freed object** - `ui_settings_dialog.c`
  turned out to have a real, separate bug matching this shape exactly:
  four places (`pw_confirm()`, `pw_dismiss()`, `cancel_cb()`, `save_cb()`)
  called `lv_obj_del()` on a dialog *synchronously, from inside a click
  handler on that dialog's own child button* - a well-known LVGL
  foot-gun (their own docs: use `lv_obj_del_async()`, not `lv_obj_del()`,
  for exactly this case, since LVGL's input-device/event-dispatch
  machinery can still reference the object after the callback returns).
  Fixed by deferring each delete-and-redraw sequence through
  `lv_async_call()` instead. A real bug worth having fixed regardless,
  but *not* this crash's actual cause - it kept recurring even after this
  fix, including on fresh boots where no dialog had ever been touched.

The real cause: `calendar_ui_init()` builds the **entire** UI - nav rail,
top bar, legend, all four views, the ambient clock overlay - running in
`app_main()`'s own task, with no `bsp_lvgl_lock()` held at all. By the
time it runs, `board_bsp.c`'s `lvgl_init()` (called earlier, from
`bsp_display_init()`) has already started `esp_lvgl_port`'s background
task, which independently calls `lv_timer_handler()` on its own loop from
the moment it's created - including LVGL's own periodic layout/redraw
pass over whatever object tree exists at that instant. Two unsynchronized
tasks touching the same object tree is a textbook LVGL thread-safety
violation: LVGL's own task could walk into an object mid-construction,
its children or style list not yet linked up, and read garbage - which
exactly explains the symptom (a different leaf function each time,
whichever object happened to be mid-construction when the race hit) and
its rarity/inconsistency across boots (purely a timing race).

Fixed by wrapping `calendar_ui_init()`'s entire body in
`bsp_lvgl_lock()`/`bsp_lvgl_unlock()`. `bsp_lvgl_lock()` is `esp_lvgl_port`'s
own mutex (`lvgl_port_lock()`), the identical one its background task
already takes non-blockingly (`lvgl_port_lock(0)`) before each
`lv_timer_handler()` call - so holding it for the whole init just makes
that task skip a few cycles and retry, no deadlock risk. Confirmed on real
hardware: no recurrence since, and boot completes measurably faster too
(the background task no longer wastes cycles contending on a half-built
tree).

### Ambient clock digit tearing (fixed)

The ambient clock's once-a-minute digit change used to occasionally show a
single frame of visible tearing before settling on the correct new time.
Extensive on-device correlation testing (logging which of the RGB panel's
two physical framebuffers each redraw landed in, then deliberately
shifting where those buffers land in PSRAM and swapping which one LVGL
calls "buf1" vs "buf2") showed the tearing tracked one specific physical
framebuffer slot - `rgb_panel->fbs[0]` - regardless of its PSRAM address
or which LVGL buffer role currently pointed at it, which ruled out both a
memory/alignment explanation and an LVGL/`esp_lvgl_port`-level one.

The actual bug: ESP-IDF's own RGB LCD driver
(`esp_lcd_panel_rgb.c`'s `lcd_rgb_panel_try_restart_transmission()`, gated
by `CONFIG_LCD_RGB_RESTART_IN_VSYNC`) restarts the GDMA chain via a single
link hardcoded to `fbs[0]` on every VSYNC, regardless of which buffer was
actually current - so with two frame buffers, every VSYNC-triggered
restart silently re-anchored the scan-out back to `fbs[0]`'s stale content
whenever the most recent redraw had actually landed in the other buffer.
Simply disabling `CONFIG_LCD_RGB_RESTART_IN_VSYNC` "fixed" the flash but
traded it for a worse, permanent ~300px horizontal shift (this flag turns
out to be genuinely needed on this board/timing, guarding against a real
GDMA-vs-LCD-FIFO desync) - so the real fix is a vendored, patched copy of
the whole `esp_lcd` component under `components/esp_lcd/` (project-local
`components/<name>` overrides `$IDF_PATH/components/<name>` of the same
name - ESP-IDF's own documented mechanism for exactly this), which builds
one restart link per frame buffer instead of one hardcoded to slot 0 and
selects among them by `cur_fb_index` at restart time. See that file's own
header comment for the full patch writeup. `CONFIG_LCD_RGB_RESTART_IN_VSYNC`
stays enabled with this patch in place - do a clean `idf.py fullclean`
before rebuilding after pulling this component in fresh, so CMake actually
picks up the local override instead of a previously-cached SDK path.

Maintenance cost: any future `esp_lcd` fix or security patch from an
ESP-IDF upgrade needs to be manually re-applied to this vendored copy too
- it won't pick them up automatically. If Espressif ever fixes this
upstream, drop `components/esp_lcd/` and go back to the SDK's own copy.

### Calendar sync reliability (internal-RAM headroom) (fixed)

Intermittent calendar refresh failures (`ESP_ERR_HTTP_FETCH_HEADER`,
`ESP_ERR_HTTP_CONNECT`, `PK verify failed` certificate errors, even the
occasional truncated/`bad JSON in response`) - reproducible enough across a
long overnight test (2026-09-09/10) to rule out simple bad luck, but
resistant to some of the obvious suspects: a manual device reset didn't
fix it, neither did switching the router's Wi-Fi channel. Heap poisoning
(see the boot-crash section above) was briefly re-enabled to check for a
leak and found none - total free heap stayed essentially flat for hours.

The actual driver: **which display state is active**. This board's
internal RAM is tight enough (~45-63KB free depending on state - see
`CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN`'s own comment for the ~45-55KB
threshold TLS certificate verification needs) that whichever view is
currently on screen (Month/Week/Day/Up next, all rendered as LVGL objects
- event bars, badges, chips) measurably eats into the same budget TLS
handshakes need, and the ICS calendar specifically (a fresh handshake to a
different host, not reusing an already-warm connection the way the four
Google Calendar API fetches do) was the first thing to become flaky when
that budget got tight. Confirmed directly on real hardware: internal RAM
free jumped by ~14KB the instant the active view's content was released
(entering the ambient clock or sleep screen), and a previously-failing ICS
fetch succeeded on the very next cycle with no other change.

Three changes, together:

- **Shared LVGL styles.** Every view (`ui_month.c`, `ui_week.c`,
  `ui_day.c`, `ui_upnext.c`) plus the top bar's calendar legend
  (`calendar_ui.c`) used to set every style property - radius, background
  opacity, font, even text colour where there were really only ever two
  possible values - directly on each event bar/chip/label object,
  individually, every single time that content was rebuilt (every sync
  cycle, every view switch). In LVGL 8, each of those calls that hits an
  object with no matching style yet allocates that object its own
  dynamically-sized "local style" out of internal RAM (small allocations
  are kept off PSRAM entirely by `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`).
  Properties that are actually constant now live in a handful of static
  `lv_style_t`s per file, attached via `lv_obj_add_style()` - free beyond
  a pointer, instead of paying for a fresh local style on every object,
  every cycle. Only genuinely per-object properties (mainly `bg_color`,
  one of many per-calendar colours) stay direct per-object calls.
- **Sync no longer skips while the display's asleep or showing the
  ambient clock.** `calendar_ui_is_asleep()` covers both those states, not
  just the deeper backlight-off sleep - so with presence continuously
  detected, this used to mean *hours* with zero network activity at all.
  `main/main.c`'s `net_task()` now syncs on its normal schedule regardless
  of display state; `calendar_ui_refresh()`/`calendar_ui_notify_sync_failed()`
  touching LVGL objects that just aren't currently visible costs nothing
  that matters next to the fetch itself.
- **A touch waking the display no longer forces an extra sync.** It used
  to: `ui_screensaver.c`'s `go_calendar()` set the same event bit
  `calendar_ui_request_sync()` uses for the settings page's manual
  "tap the updated-HH:MM-label" gesture, which `net_task()` treated as
  "go sync right now." But `go_calendar()` already repopulates the view
  from `event_store`'s own cached data first (no network involved), and -
  now that sync never stops running in the background - that data is
  never more than `refresh_interval_s` stale to begin with. Forcing an
  *extra* fetch right at the exact moment the view was being reconstructed
  was actively harmful, not just redundant: it collided a fresh TLS
  handshake with precisely the moment internal RAM was tightest. The
  manual force-sync tap still works exactly as before - only the implicit
  touch-wake stopped triggering one.
- **A lightweight keep-alive**, `main.c`'s `keepalive_probe()`: a plain
  TCP connect-then-close (no TLS, no data) to the same host calendar sync
  talks to, roughly once a minute during whatever of the wait between
  sync cycles is still otherwise completely idle. Added on a theory (a
  connection sitting silent for minutes at a stretch being more exposed
  to router-side connection-tracking/ARP staleness than one with regular
  traffic) that didn't end up being the main driver - internal RAM
  headroom was - but it's cheap, harmless, and logs only on failure, so
  it was left in as a genuinely useful diagnostic even after the real
  cause was found.

Confirmed on real hardware afterward: the "view active" internal-RAM
baseline that used to sit around ~44-45KB now typically sits around
~57-58KB (matching the "view released" level from before), a deliberate
worst-case stress test (every calendar enabled, a busy month, a forced
sync) still succeeded cleanly even at its own tighter ~45-46KB moment, and
a long run of sync cycles across every display state and several touch
wakes came back clean throughout.

## What it does

- Pulls events from **multiple Google Calendars** (your primary calendar,
  a family calendar, a work calendar, etc.) and **ICS feeds**, and shows
  each in a colour you pick, with a legend you can tap to show/hide a
  calendar. Any calendar can be marked **daily** to fetch it just once a
  day instead of every refresh cycle (for feeds that rarely change, or a
  flaky host).
- **Month view**: a 6x7 grid with coloured event chips per day; tap a day
  to jump into Day view.
- **Week view**: an hourly grid (6am-10pm by default) across 7 day columns,
  with an all-day-event strip along the top.
- **Day view**: the same hourly grid for a single day, full width.
- **Up next**: a scrollable list of upcoming events across all visible
  calendars, soonest first.
- Auto-refreshes from Google on a timer (default every 5 minutes).
- **Idle screensaver**: three states, driven by the idle timer and the
  presence sensor (see "Presence sensor" below). After a configurable idle
  timeout, the calendar gives way to a big ambient clock - "HH:MM" with
  each digit tinted using the same colours as your own calendars, vivid
  during daylight hours and muted at night (no separate setting for this -
  it reuses the same view_start_hour/view_end_hour that bound the Day/Week
  views) - but only if someone's actually in front of the presence sensor
  at that moment; if not, the display goes straight to sleep instead
  (below). If the clock's showing and presence is then continuously absent
  for 5 minutes, it goes to sleep too. Independent of all of that, the
  backlight's actual physical brightness auto-dims off ambient room light
  whenever a light sensor is fitted (see "Backlight auto-dimming" below) -
  it runs continuously in both the calendar and ambient-clock states, so a
  dark room dims the physical backlight *and* mutes the clock's on-screen
  colours at the same time, from two independent mechanisms. Sleep means
  the backlight actually turns fully off (not just dimmed) and a
  slowly-regenerating noise pattern (anti-image-retention, not just a
  blank screen) replaces whatever was showing - presence returning wakes
  it back to the ambient clock, never straight to the calendar, at
  whatever brightness auto-dimming last computed rather than a flash back
  to full. Only an actual touch brings the calendar back, from any state.
  If the display's been away from the calendar 15+ minutes by the time
  that touch happens, it resets to Month view on today's date rather than
  resuming whatever view/date was showing before - the idea being that
  whatever day or week you were looking at before walking away is unlikely
  to still be what you want to see later. The clock icon in the top bar
  (left of the settings gear) toggles the ambient clock on/off for the
  current session only - toggling it off makes idle timeout go straight
  to the backlight-off sleep state instead, regardless of presence,
  matching the screensaver's pre-clock behaviour. On/off is shown by
  dimming the icon rather than swapping its shape, since there's no
  built-in "disabled clock" glyph to switch to. This is a runtime-only
  switch, not a saved setting: it's always back on after a reboot.

## Hardware

Waveshare ESP32-S3-Touch-LCD-7:

- ESP32-S3, 8MB octal PSRAM, 16MB flash
- 800x480 IPS, RGB565 parallel interface, ST7262 controller
- GT911 capacitive touch (I2C)
- CH422G I2C IO expander for backlight enable + touch reset

The board as sold needs no external wiring at all for display/touch/SD.
Two optional additions — a presence sensor on `GPIO6` (see "Presence
sensor" below) and ambient-light-driven backlight dimming (a wire to a
backlight-driver test point, plus an external light sensor - see
"Backlight auto-dimming" below) — do need it; the firmware degrades
gracefully without either (no presence sensor: idle timeout always goes
straight to sleep; no light sensor: the backlight just stays wherever it
last was, no auto-dimming).

## Repo layout

```
main/                    app_main: boot sequence, wires everything together
components/
  app_common/            shared app_settings_t config struct (header-only)
  board_bsp/              display + touch + CH422G + LVGL bring-up, plus
                           the backlight's LEDC PWM dimming channel (see
                           "Backlight auto-dimming" below)
  provisioning/            first-boot Wi-Fi AP + web form, NVS config storage,
                           normal-mode Wi-Fi station connect
  gcal/                    Google service-account auth (JWT) + Calendar API
                           client + in-RAM event store
  calendar_ui/             the four LVGL screens (month/week/day/up-next)
                           plus the nav rail / top bar / legend shell, the
                           idle-timeout ambient clock, and the settings
                           dialog
  sd_card/                 mounts the TF card slot as FAT at /sdcard, if one
                           is inserted (see "TF/SD card" below); used for a
                           config backup that survives reflashing other
                           firmware onto the board
  presence_sensor/         reads the GPIO6 presence sensor (see "Presence
                           sensor" below); drives the ambient clock's
                           brighten/dim behaviour
  light_sensor/           reads a BH1750 ambient light sensor over I2C
                           (see "Backlight auto-dimming" below); drives
                           the physical backlight brightness
  esp_lcd/                 vendored + patched copy of ESP-IDF's own
                           esp_lcd component (overrides $IDF_PATH's) -
                           fixes a real bug in the RGB panel driver, see
                           "Ambient clock digit tearing (fixed)" below
```

## Building

Requires **ESP-IDF v5.3 or newer** (see "Bring-up troubleshooting" for why).
Follow Espressif's [get-started guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html)
to install the toolchain if you haven't already, then:

```sh
cd gcal_display_esp32s3
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor      # adjust the port
```

The first build will download LVGL, `esp_lvgl_port`, and
`esp_lcd_touch_gt911` automatically via the IDF Component Manager (see
`components/board_bsp/idf_component.yml`) — no manual cloning needed.

## First boot: Wi-Fi + calendar setup portal

There are no credentials baked into the firmware. On first boot (or any
time NVS has no valid config), the device starts its own access point:

1. On your phone/laptop, connect to Wi-Fi network **`GCal-Display-Setup`**
   (open, no password).
2. Browse to `http://192.168.4.1`.
3. Fill in the form (see below for where the Google fields come from) and
   tap **Save & restart**.
4. The device reboots and connects to your normal Wi-Fi network.

To re-run the portal later (new Wi-Fi network, new calendar, etc.), erase
the `gcalcfg` NVS namespace — easiest way is `idf.py erase-flash` and
reflash, or add a small "hold touch on boot" hook calling
`provisioning_clear()` if you want that convenience (not wired up by
default, to keep first boot simple).

## Google Cloud setup (one-time, ~10 minutes)

This uses a **service account**, not a personal OAuth login — there's no
browser step on the device itself, and no token to babysit. You grant
access once, by sharing each calendar with the service account's own email
address, the same way you'd share a calendar with a person.

1. Go to the [Google Cloud Console](https://console.cloud.google.com/) and
   create a new project (or reuse one).
2. **APIs & Services → Library** → search "Google Calendar API" → **Enable**.
3. **APIs & Services → Credentials → Create Credentials → Service account**.
   Give it any name (e.g. "calendar-display"). No roles/permissions needed
   at the project level — skip those steps.
4. Open the new service account → **Keys** tab → **Add key → Create new
   key → JSON**. This downloads a `.json` file — keep it safe, it's a
   credential.
5. Open that JSON file. You need two fields out of it:
   - `client_email` — looks like
     `calendar-display@your-project.iam.gserviceaccount.com`
   - `private_key` — a multi-line block starting with
     `-----BEGIN PRIVATE KEY-----`
6. For **each** calendar you want on the display: open
   [Google Calendar](https://calendar.google.com) → that calendar's
   Settings → **Share with specific people** → add the `client_email`
   address from step 5 as a viewer ("See all event details" is enough).
   - For your own primary calendar, the "calendar ID" to enter in the
     setup form is just your Gmail address.
   - For a secondary/family/group calendar, its ID is on the same
     Settings page, under "Integrate calendar" → **Calendar ID**
     (looks like `abcd1234@group.calendar.google.com`).

In the setup portal form:

- **Service account email** → the `client_email` value.
- **Private key** → paste the whole `private_key` value, including the
  `-----BEGIN PRIVATE KEY-----` / `-----END PRIVATE KEY-----` lines,
  exactly as it appears in the JSON file (with real line breaks — if
  you're copying out of the raw JSON where newlines are written as
  `\n`, replace those with actual new lines first).
- One row per calendar: **ID** (from step 6), a short **label**, and a
  **colour** — this is exactly like assigning a colour to a calendar in
  the Google Calendar app. Two checkboxes per row: **on** (whether it's
  currently shown - also toggleable live from the legend) and **daily**
  (see below).
- **daily** (per calendar): tick it for a feed that barely ever changes
  (a public holidays ICS, say) or a third-party host that's flaky and not
  worth hammering. That calendar is then fetched only once per local
  calendar day - on the first refresh cycle on or after local midnight,
  plus once at startup - instead of every `refresh_interval_s`. Its
  events are carried forward untouched on the cycles in between, and a
  failed daily fetch still retries on the next regular cycle (the
  once-a-day quiet only starts after it has actually succeeded that day).
- **Timezone**: a POSIX TZ string. Defaults to
  `AEST-10AEDT,M10.1.0,M4.1.0/3` (Australia/Melbourne). Search "POSIX TZ
  string <your city>" if you need a different one.

Nothing here needs Google's OAuth consent screen to be published/verified
— service-account server-to-server auth doesn't go through that flow, so
there's no "unverified app" warning to fight with.

## Known limitations

- **Pagination**: each calendar fetch requests up to 250 events in the
  fetch window (`FETCH_PAST_DAYS`/`FETCH_FUTURE_DAYS` in `main/main.c`,
  14 days back / 60 days forward by default). Google's `nextPageToken`
  pagination isn't implemented, so an extremely busy calendar could be
  truncated — 250 events over ~10 weeks is generous for a household
  calendar, but widen the window with care.
- **Legend toggle isn't persisted** — hiding a calendar via the legend
  chip is a live UI filter that resets on reboot (all calendars fetched
  every cycle either way, so this is instant either way).
- **TF/SD card is only used for a config backup, nothing else yet** — see
  "TF/SD card" below. No event cache, no logging - `sd_card_init()`'s
  mount is otherwise idle once boot finishes (and is in fact deinited
  right after boot, freeing its SPI bus/DMA resources — see below).
- **Partition table has OTA slots, but nothing writes to them** — `ota_0`/
  `ota_1` exist in `partitions.csv`, but no code calls `esp_https_ota` or
  otherwise switches the active slot, so a flashed image never gets
  replaced except by re-flashing over serial.

## Bring-up troubleshooting

Things most likely to need a tweak on real hardware, roughly in the order
you'd hit them:

1. **`esp_lcd_rgb_panel_config_t` field names** (`lcd_panel_init()` in
   `board_bsp.c`): the RGB panel config struct in `esp_lcd_panel_rgb.h`
   has had minor field reshuffles across IDF releases (e.g. exactly
   which fields live directly on the struct vs. nested under `.flags`).
   If a field name doesn't compile, check the struct definition shipped
   with your IDF version and adjust - the set of fields being configured
   (timings, data width, frame buffer count/placement, pin numbers)
   is stable even if a name or two moves.
2. **`esp_lcd_new_panel_io_i2c()` signature** (`components/board_bsp/board_bsp.c`,
   `touch_init()`): written for ESP-IDF ≥5.3, which takes the
   `i2c_master_bus_handle_t` from `i2c_new_master_bus()` directly. Older
   5.2.x builds used a different signature. If this doesn't compile,
   check `esp_lcd_panel_io_i2c.h` for your installed IDF version.
3. **`lvgl_port_add_disp_rgb()`** (`lvgl_init()` in the same file): the
   exact struct fields on `lvgl_port_display_rgb_cfg_t` have shifted
   across `esp_lvgl_port` releases. If it doesn't compile, check the
   README/example in whatever version the component manager pulled
   (`managed_components/espressif__esp_lvgl_port/`), or fall back to the
   generic `lvgl_port_add_disp()` with the same `lvgl_port_display_cfg_t`.
4. **Picture doesn't show your UI at all — screen cycles through solid
   colour bars/blocks instead**: this means the panel never left its own
   built-in test pattern, regardless of anything happening on the
   LVGL/software side (a rendering bug would show a *distorted version* of
   your UI, not a clean colour-bar cycle that's completely unaffected by
   what you draw, or by RGB timing changes). On real hardware this turned
   out to be a missing physical reset pulse: this board's RGB panel has its
   own reset line wired through the CH422G expander (`CH422G_EXIO_LCD_RST`,
   EXIO3) rather than an ESP32 GPIO, and `esp_lcd`'s RGB panel driver has no
   `reset_gpio_num` field of its own to handle that automatically. Without
   `lcd_reset_pulse()` (called from `bsp_display_init()` before
   `lcd_panel_init()`) the panel simply never came out of reset, no matter
   what RGB timing values were used. This was found by cross-referencing a
   working ESPHome config for this exact board, which declares an explicit
   `reset_pin` under its CH422G `io_ex` hub that this firmware had no
   equivalent of.
5. **Picture shows your UI but shifted, rolling, has noise, or jitters**:
   with the reset pulse above in place, `lcd_panel_init()`'s RGB timing
   values are what's left to tune. The values currently in `board_bsp.c`
   (12.9MHz pclk, hsync/vsync pulse=2, all porches=4, `pclk_active_neg =
   false`, from a community report for this exact panel/pinout —
   lvgl_micropython repo, discussion #333) tested rock solid on real
   hardware; a working ESPHome config for this same board (16MHz pclk,
   hsync pulse/back/front = 4/8/8, vsync pulse/back/front = 4/16/16,
   `pclk_active_neg = true`, also noted in that comment) had occasional
   jitter on the same unit. If you still get jitter, that ESPHome
   alternative or the underlying community report are the best next things
   to try, not blind guessing at new values from scratch — and that report
   also found this panel tolerates pixel clocks up to ~12.9MHz but starts a
   slow rightward "creep" at 13.0MHz and above. If the picture is otherwise
   stable but shows brief horizontal jitter/wobble rather than a steady
   drift or colour bars, that's more likely PSRAM bus contention starving
   the RGB DMA than a timing problem, since both frame buffers
   live in PSRAM (`.flags.fb_in_psram = true`). Do **not** "fix" this with
   a bounce buffer (`bounce_buffer_size_px`) unless you also enable
   `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` + `CONFIG_SPIRAM_RODATA` (PSRAM XIP)
   — without those, ESP-IDF's own RGB LCD driver docs warn that mode
   "CAN NOT work if we disable the cache of the external memory, via e.g.
   OTA or NVS write to the main flash", and in practice it panics
   (`Cache disabled but cached memory region accessed`) the moment Wi-Fi
   does its NVS write at boot. Safer first move: turn off
   `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` in `sdkconfig.defaults` so Wi-Fi's
   RX/TX buffers land in internal RAM instead of PSRAM (there's ~260KB of
   internal RAM free at boot, plenty of headroom) and stop competing with
   the LCD DMA for PSRAM bandwidth.
6. **Colours look swapped/wrong (e.g. red and blue swapped)**: check
   `s_lcd_data_gpios[]` in `board_bsp.c` against the "ESP32-S3 ↔ LCD"
   pin table on the [Waveshare wiki page](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7) —
   the array is ordered blue-LSB→red-MSB (B3..B7, G2..G7, R3..R7) to
   match that table, which is the standard convention for this
   `esp_lcd_panel_rgb` config, but double-check.
7. **Backlight stays off / touch doesn't respond**: the CH422G register
   addresses and EXIO-bit-to-pin mapping in `ch422g.c` come from the
   community-maintained ESPHome CH422G driver (WCH's own datasheet is
   Chinese-only and thin on I2C protocol detail), not from Waveshare's
   demo source directly. If EXIO1 (touch reset) / EXIO2 (backlight)
   don't do what's expected, check Waveshare's own
   `ESP32-S3-Touch-LCD-7-Demo` repo (linked from the wiki page) for
   their exact expander init sequence.
8. **Wrong calendar/timezone displayed**: double check the POSIX TZ
   string — a wrong DST rule silently shifts events by an hour for part
   of the year rather than erroring out.
9. **A hard abort inside `spi_flash_disable_interrupts_caches_and_other_cpu()`
   (or any "Cache disabled but cached memory region accessed" panic) the
   first time a new background task does a raw NVS/flash write**: that
   kind of write needs the calling task's *own stack* to be in internal
   RAM, not PSRAM — PSRAM itself is briefly unreachable while the flash
   cache is disabled for the write, so a task whose stack lives there
   can't safely be the one doing it (or receiving an interrupt while it's
   in progress). This project already puts a couple of tasks' stacks in
   PSRAM on purpose, specifically to keep internal RAM free for exactly
   this kind of operation elsewhere (see `net_task`'s creation comment in
   `main/main.c` and `lvgl_init()`'s in `board_bsp.c`) — but the first time
   *any new* task you add does its own NVS/flash write (a settings save, a
   first-time driver init that touches its own NVS blob — Wi-Fi's own
   `esp_wifi_init()` hit this on-device, the very first time it ever ran),
   check what stack that task is running on before assuming it's a logic
   bug. Fix is always the same: split that one write onto a small,
   short-lived task with a plain (`xTaskCreate`, internal-RAM) stack — see
   `reconfigure_task` in `calendar_ui.c`, `save_task` in `config_web.c`, or
   `wifi_bringup_task` in `main.c` for three examples of the same pattern.
10. **`main_task` hangs forever after boot, tripping the task watchdog
    every few seconds with `IDLE0` never getting to run**: if this
    started after adding a forced `lv_refr_now()` (e.g. to fix tearing
    somewhere — see the dual-framebuffer note under item 5 above), check
    whether that code path can run as part of the *very first* screen
    ever shown (`calendar_ui_init()`'s own initial view selection, before
    LVGL's own redraw timer has ticked even once) — calling
    `lv_refr_now()` synchronously that early deadlocked inside LVGL's
    buffer-sync logic on-device. `select_view_force_redraw()` in
    `calendar_ui.c` is deliberately called only from *interactive*
    view-switch call sites (a nav rail tap, drilling into a day, waking
    from a long sleep), never from the startup path, for exactly this
    reason.

None of these are architectural problems — they're exactly the kind of
"tune the board bring-up constants" work you'd expect when porting to a
7" RGB panel for the first time, just called out explicitly instead of
left for you to discover blind.

## TF/SD card

The board's TF-card slot is wired for SPI: `GPIO11`=MOSI, `GPIO12`=SCK,
`GPIO13`=MISO (a dedicated bus — nothing else on the board shares those
three pins), with chip-select on `CH422G_EXIO_SD_CS` (the same I2C IO
expander that drives the backlight and touch/LCD reset), not a native
GPIO. `sd_card_init()` (`components/sd_card/`) mounts it as FAT at
`/sdcard` during boot if a card is present; a missing or unreadable card
is logged and otherwise ignored, not treated as a boot failure — see the
comment in `main/main.c`. Once boot finishes, `sd_card_deinit()` unmounts
it and frees its SPI bus/DMA buffers/GDMA channel again — nothing needs
the card mounted for the rest of the device's uptime (see "Config
backup" below), and holding those resources the whole time was
measurably eating into the internal RAM calendar refresh's TLS
handshakes need.

`sd_card_init()` deliberately leaves `format_if_mount_failed` off: a card
with an unreadable filesystem will fail to mount rather than be silently
erased. Format it FAT32 on a PC first if mounting fails and you want to
use it.

### Config backup

The card is used to back up your saved configuration (Wi-Fi, service
account, calendar list, all the settings-dialog fields) as JSON at
`/sdcard/gcal/config.json`, independent of NVS. The point: if you flash
other/test firmware onto this board and it reuses or wipes NVS, reflashing
this firmware back can pick your calendar config back up from the card on
its own, without re-running the setup portal.

- **NVS is always authoritative when it holds a valid config.** The card
  is only consulted as a fallback, when NVS comes back empty or invalid.
- **The backup refreshes once per boot, not live.** The SD card is only
  mounted in a narrow window early in boot (freed again right after, to
  keep its internal-RAM/SPI-bus footprint off calendar refresh's already
  tight budget — see "TF/SD card" above), so a settings change made
  mid-session can't write straight to it — every settings dialog (the
  on-device gear icon, the LAN config page, the first-boot setup portal)
  already restarts the device right after saving, though, so the backup
  is refreshed within seconds of any real change regardless, on the boot
  that follows. (This used to attempt writing the backup at save time
  directly, which silently failed once the SD-deinit optimization above
  landed — confirmed on-device as exactly why an added calendar
  "disappeared": the NVS save worked, the SD save didn't, silently.)
- No event cache, no logging — this is the card's only use so far.
- Nothing in the app reads or writes anywhere else on `/sdcard` — mounting
  and unmounting are wired up so a future feature can reuse the same
  pin/CS wiring without having to re-derive it.

## Presence sensor

An active-high presence/PIR sensor is wired to `GPIO6` (high = someone's
there, low = clear). `presence_sensor_init()` (`components/presence_sensor/`)
configures it as an input with the **internal pull-down enabled** - on
real hardware this line floats and reads intermittent false "detected"
spikes without it, confirmed by extended monitoring during bring-up. It's
polled (not interrupt-driven) once a second, from the same timer that
drives the ambient clock (see "Idle screensaver" above), which is
plenty of granularity for a presence-driven dim/brighten decision.

No sensor connected reads as a permanent "clear" (GPIO6 pulled low), which
just means the display always settles into full sleep after the usual
delays (see "Idle screensaver" above) - harmless, just not useful.

## Backlight auto-dimming

Two physical additions to the board as sold, both optional - without
either, the firmware just leaves the backlight wherever it last was:

- **A wire from `GPIO16` to a PWM dimming test point** on the backlight
  boost driver. The CH422G expander's own backlight line
  (`CH422G_EXIO_LCD_BL`) only ever gates the driver fully on/off from the
  factory - confirmed on real hardware that a separate test point on the
  driver board is a genuine PWM *dimming* input, unconnected to any
  ESP32-S3 pin out of the box. `board_bsp.c` drives that test point via
  GPIO16 and one of the SoC's LEDC PWM channels; the CH422G gate is left
  exactly as it ships and still used for a true, zero-current off (see
  `bsp_display_set_brightness_permille()`'s own comment for the exact
  power-up/power-down sequencing between the two).
- **A GY-30/BH1750 ambient light sensor breakout**, wired onto the same
  shared I2C bus as the CH422G expander and GT911 touch controller
  (`bsp_get_i2c_bus()`), at its default address `0x23` (`ADDR` pin
  low/floating, how these modules ship - doesn't collide with anything
  else already on that bus). `components/light_sensor/` starts it in
  Continuous High-Resolution Mode and polls it once a second, from the
  same timer that drives the ambient clock and presence sensor
  (`ui_screensaver.c`'s `ambient_brightness_tick()`).

The LEDC PWM channel runs at **1220Hz, 14-bit resolution** (16384 duty
steps) - not an arbitrary choice: this matches
[ESPHome's own recommendation](https://esphome.io/components/output/ledc.html)
for LED/backlight dimming specifically, since 1220Hz is where the ESP32
family's LEDC timer can hit its *maximum* duty resolution (14 bits on
S2/S3/C3) against an 80MHz APB clock, rather than trading resolution away
for a higher switching frequency. This isn't just theoretical: an earlier
5kHz/10-bit configuration measured a real, hard cutoff on this specific
board's driver at duty 12/1023 (~1.2%) - 11/1023 simply didn't light at
all - leaving almost no usable dimming range above it ("a bit dimmer,
then nothing" rather than a smooth fade). Moving to 1220Hz/14-bit fixed
that outright.

Raw lux is smoothed with an exponential moving average (so a hand or
shadow briefly crossing the sensor doesn't flicker the screen) and mapped
to backlight duty through a log curve, not a linear one (perceived
brightness is roughly logarithmic) - shaped by two settings, both on the
on-device settings dialog / config web page:

- **Minimum brightness in the dark** (`brightness_min_pct_x10`,
  10.5%-30.0%, stored internally in tenths of a percent since a whole
  percent is too coarse a step at this panel's dim end): the floor the
  backlight settles at in a fully dark room, rather than going to true
  black. The range itself, like the LEDC frequency/resolution above, was
  determined by bisecting on real hardware (via a temporary live
  `/test_brightness?permille=N` endpoint in `config_web.c`, applying a
  duty directly with no save/restart needed) - the panel's *visually
  useful* floor turned out to sit noticeably higher than the raw
  duty-12/1023 driver cutoff mentioned above.
- **Room brightness where full backlight kicks in**
  (`brightness_max_lux`, default 150 lux): at or above this ambient
  level the screen runs at 100%; below it, brightness fades toward the
  floor above on the log curve.

No sensor connected leaves `light_sensor_init()` failing at boot (logged,
not fatal) and the backlight simply staying at whatever brightness it was
last explicitly set to - no auto-dimming, but nothing crashes or hangs
either.

## Customizing

- **Refresh interval / fetch window**: `main/main.c` (`FETCH_PAST_DAYS`,
  `FETCH_FUTURE_DAYS`) and the "Refresh interval" field in the setup
  portal.
- **Week/Day hour range**: `view_start_hour`/`view_end_hour` in the setup
  portal or settings dialog (default 6am-10pm). The same two hours also
  decide the ambient clock's day/night colour boundary (see "Idle
  screensaver" above) - there's no separate setting for that.
- **Week starts Monday**: `ui_start_of_week()` in `ui_time.c` — flip to
  Sunday-based by changing the `mon_offset` calculation if you'd rather
  match the US convention.
- **Colours/theme**: `components/calendar_ui/include/ui_theme.h`.
- **Screen timeout / idle screensaver**: `screen_timeout_s` in the setup
  portal or settings dialog (0 disables it). How long away from the
  calendar (ambient clock and/or sleep, combined) before a touch resets to
  today/Month instead of resuming the prior view: `LONG_SLEEP_RESET_MS` in
  `ui_screensaver.c` (default 15 minutes). How long the ambient clock
  shows before giving up on presence and going to sleep (backlight off):
  `PRESENCE_AWAY_SLEEP_MS` in `ui_screensaver.c` (default 5 minutes).
- **Backlight auto-dimming curve**: the dark-room floor and the "full
  brightness" ambient-light threshold are both settings-page sliders (see
  "Backlight auto-dimming" above) - `brightness_min_pct_x10` and
  `brightness_max_lux` in `app_settings.h` for the defaults/valid ranges.
  How quickly readings smooth out (`LUX_EMA_ALPHA`), how large a change is
  worth actually writing to the backlight (`BRIGHTNESS_DEADBAND_PERMILLE`),
  and the flat-floor threshold at the very bottom of the sensor's own
  range (`LUX_DARK_FLOOR`) are all in `ui_screensaver.c`, not exposed as
  settings - they shape the curve's responsiveness rather than its
  endpoints, and haven't needed hardware-specific tuning the way the
  endpoints themselves did.

## Bonus: adding Google Tasks to a calendar feed, without OAuth

Google Tasks has no ICS feed of its own, so this display can't read it
directly. The workaround: a small Google Apps Script mirrors your Tasks
into a dedicated Google Calendar (as all-day events, checkbox-prefixed),
which you then add here as a normal calendar or ICS source.

### Prerequisites

- A Google account containing your Google Tasks and Google Calendar.
- Access to Google Apps Script.

### Step 1: Create a dedicated calendar for tasks

1. Open Google Calendar in your web browser.
2. In the left sidebar, find **Other calendars**, click the **+** icon,
   and select **Create new calendar**.
3. Name the calendar **My Tasks Sync** and click **Create calendar**.

### Step 2: Retrieve the secret iCal address

1. Under **My calendars** on the left, hover over **My Tasks Sync**,
   click the three dots, and select **Settings and sharing**.
2. Scroll down to the **Integrate calendar** section.
3. Locate the **Secret address in iCal format** field.
4. Copy the entire URL (e.g.
   `https://calendar.google.com/calendar/ical/.../basic.ics`).

### Step 3: Create and deploy the Google Apps Script

1. Navigate to [Google Apps Script](https://script.google.com) and click
   **New project**.
2. Clear any default code in the editor and paste the following script:

```javascript
function syncTasksToCalendar() {
  // CONFIGURATION
  const CALENDAR_NAME = "My Tasks Sync";
  const DAYS_LOOKBACK = 30;  // Includes completed tasks from past 30 days
  const DAYS_LOOKAHEAD = 30; // Includes upcoming tasks for next 30 days

  // 1. Locate destination calendar
  const calendars = CalendarApp.getCalendarsByName(CALENDAR_NAME);
  if (calendars.length === 0) {
    Logger.log("Calendar not found! Ensure the name matches CALENDAR_NAME exactly.");
    return;
  }
  const calendar = calendars[0];

  // 2. Define sync range
  const now = new Date();
  const pastDate = new Date();
  pastDate.setDate(now.getDate() - DAYS_LOOKBACK);

  const futureDate = new Date();
  futureDate.setDate(now.getDate() + DAYS_LOOKAHEAD);

  // 3. Clear existing sync events to prevent duplicates
  const existingEvents = calendar.getEvents(pastDate, futureDate);
  for (const event of existingEvents) {
    event.deleteEvent();
  }

  // 4. Retrieve task lists and items
  const taskLists = Tasks.Tasklists.list().items;
  if (!taskLists || taskLists.length === 0) return;

  for (const taskList of taskLists) {
    // Incomplete tasks due before futureDate
    const incompleteTasks = Tasks.Tasks.list(taskList.id, {
      showCompleted: false,
      showHidden: true,
      dueMax: futureDate.toISOString()
    }).items || [];

    // Tasks completed within past 30 days
    const completedTasks = Tasks.Tasks.list(taskList.id, {
      showCompleted: true,
      showHidden: true,
      completedMin: pastDate.toISOString()
    }).items || [];

    const allTasks = incompleteTasks.concat(completedTasks);

    for (const task of allTasks) {
      if (!task.title) continue;

      const isCompleted = task.status === "completed";
      // U+2611 (☑) for completed, U+2610 (☐) for incomplete
      const symbol = isCompleted ? "☑" : "☐";

      let targetDateString = isCompleted ? (task.completed || task.due) : task.due;
      let eventDate = targetDateString ? new Date(targetDateString) : new Date();

      if (eventDate >= pastDate && eventDate <= futureDate) {
        calendar.createAllDayEvent(`${symbol} ${task.title}`, eventDate, {
          description: task.notes || ''
        });
      }
    }
  }
}
```

3. Enable the Google Tasks API service:
   - In the left panel, click **Services** (**+**).
   - Select **Google Tasks API** from the list and click **Add**.
4. Save the project by clicking the **Save** (disk) icon.
5. Click **Run** at the top menu to execute the script once manually.
6. When prompted, click **Review Permissions**, select your Google
   account, click **Advanced**, and grant access.

### Step 4: Configure automatic background execution

1. In the left menu of the Apps Script interface, click **Triggers**
   (alarm clock icon).
2. Click **Add Trigger** in the bottom right corner.
3. Configure the trigger parameters:
   - Choose which function to run: `syncTasksToCalendar`
   - Select event source: **Time-driven**
   - Type of time based trigger: **Minutes timer**
   - Select minute interval: **Every 15 minutes** (or **Every 30
     minutes**)
4. Click **Save**.

### Step 5: Add it to the display

"My Tasks Sync" is a calendar in your own Google account, so - unlike a
calendar someone else owns and won't share - the simplest path is to add
it as a normal **Google**-source calendar rather than an ICS feed:
share it with the service account's email address exactly as described
in "Google Cloud setup" above, then add its **Calendar ID** (Settings
and sharing → Integrate calendar → Calendar ID, right next to the secret
iCal address from Step 2) as a calendar entry in the setup portal or
config web page. This reuses the service-account auth already set up for
your other calendars, with no separate feed-fetching code path involved.

If you'd rather not share it with the service account, the secret iCal
address from Step 2 works too - add it as an **ICS URL**-source calendar
instead, no sharing required. Either way, pending tasks show up
prefixed with ☐ (U+2610), completed ones with ☑ (U+2611).
