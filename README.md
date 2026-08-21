**Caveat:** Developed in conjunction with Claude AI.

# Google Calendar wall display — Waveshare ESP32-S3-Touch-LCD-7

An ESP-IDF + LVGL project that turns the [Waveshare ESP32-S3-Touch-LCD-7](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7)
(800x480 IPS touch panel) into a wall-mounted calendar, similar in spirit to
the Google Calendar Android app: Month / Week / Day views you can drill into
by tapping a day, an "Up next" list, and multiple calendars shown side by
side, each in its own colour.

**Important, read first:** this was written and reviewed carefully, but it
has not been compiled or flashed against a physical board in this session —
I don't have the hardware here. The architecture, Google Calendar
integration, and UI logic are solid, but a handful of very hardware-specific
details (exact RGB timing constants, one I2C-expander's register layout, the
precise `esp_lvgl_port`/`esp_lcd` function signatures for *your* installed
ESP-IDF version) are flagged below under **Bring-up troubleshooting** —
expect to spend a first bring-up session tuning those, the way you would
with any board support package for a new panel.

## What it does

- Pulls events from **multiple Google Calendars** (your primary calendar,
  a family calendar, a work calendar, etc.) and shows each in a colour you
  pick, with a legend you can tap to show/hide a calendar.
- **Month view**: a 6x7 grid with coloured event chips per day; tap a day
  to jump into Day view.
- **Week view**: an hourly grid (6am-10pm by default) across 7 day columns,
  with an all-day-event strip along the top.
- **Day view**: the same hourly grid for a single day, full width.
- **Up next**: a scrollable list of upcoming events across all visible
  calendars, soonest first.
- Auto-refreshes from Google on a timer (default every 5 minutes).

## Hardware

Waveshare ESP32-S3-Touch-LCD-7:

- ESP32-S3, 8MB octal PSRAM, 16MB flash
- 800x480 IPS, RGB565 parallel interface, ST7262 controller
- GT911 capacitive touch (I2C)
- CH422G I2C IO expander for backlight enable + touch reset

No external wiring needed — this targets the board as sold.

## Repo layout

```
main/                    app_main: boot sequence, wires everything together
components/
  app_common/            shared app_settings_t config struct (header-only)
  board_bsp/              display + touch + CH422G + LVGL bring-up
  provisioning/            first-boot Wi-Fi AP + web form, NVS config storage,
                           normal-mode Wi-Fi station connect
  gcal/                    Google service-account auth (JWT) + Calendar API
                           client + in-RAM event store
  calendar_ui/             the four LVGL screens (month/week/day/up-next)
                           plus the nav rail / top bar / legend shell
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
  the Google Calendar app.
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
- **No SD card support**, even though the board has a slot — see
  "Bring-up troubleshooting" for why it was left out.
- **No OTA** — the partition table is a single app slot. Add
  `ota_0`/`ota_1` partitions and the `esp_https_ota` component if you
  want over-the-air updates later.

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

None of these are architectural problems — they're exactly the kind of
"tune the board bring-up constants" work you'd expect when porting to a
7" RGB panel for the first time, just called out explicitly instead of
left for you to discover blind.

## Customizing

- **Refresh interval / fetch window**: `main/main.c` (`FETCH_PAST_DAYS`,
  `FETCH_FUTURE_DAYS`) and the "Refresh interval" field in the setup
  portal.
- **Week/Day hour range**: `HOUR_START`/`HOUR_END` at the top of
  `ui_week.c` / `ui_day.c` (default 6am-10pm).
- **Week starts Monday**: `ui_start_of_week()` in `ui_time.c` — flip to
  Sunday-based by changing the `mon_offset` calculation if you'd rather
  match the US convention.
- **Colours/theme**: `components/calendar_ui/include/ui_theme.h`.

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

### Step 5: Fetch the feed

Issue an HTTP GET request (via this project's ICS client, `libcurl`, or
your library of choice) to the secret iCal address obtained in Step 2.
Parse the `.ics` payload's `SUMMARY` lines - pending tasks are prefixed
with ☐ (U+2610), completed tasks with ☑ (U+2611).
