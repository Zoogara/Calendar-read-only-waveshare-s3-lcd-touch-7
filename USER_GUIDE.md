# User guide

A quick tour of everything the calendar display does and every setting it
has. For building, wiring and the background behind each feature, see
[README.md](README.md).

## Getting around the calendar

- **Views** - the rail down the left switches between **Month**, **Week**,
  **Day** and **Up next** (a list of what's coming, soonest first).
- **Tap a day** in Month view to open it in Day view.
- **Legend chips** (one per calendar, in its colour) - tap to hide or show
  that calendar. This lasts until the next restart.
- **Sync now** by tapping "updated HH:MM" at the top right (see
  *Top bar*). If a calendar can't be reached, the display keeps showing its
  last events until the next successful sync.
- Past events are drawn paler than upcoming ones.

The calendar syncs every few minutes on its own (see *Refresh interval*).
Events the calendar's owner declined, and cancelled events, aren't shown.

## Top bar

Left to right:

| Item | What it does |
|---|---|
| **‹ ›** | Step back / forward a month, week or day, depending on the view |
| Title | What's showing: the month, the week's dates or the day |
| Padlock | Locks the screen now. Only there when a PIN is set - see *Screen lock* |
| Clock icon | Turns the ambient clock on or off until the next restart. Dimmed = off: the display goes straight to the screensaver when idle instead |
| Gear | Opens the menu: **Display** (settings dialog), **Setup**, **Update** - see below |
| Time / "updated HH:MM" | The current time, and when the calendar last synced underneath. **Tap "updated" (or the time just above it) to sync now.** A **⚠** before "updated" means the last sync missed a calendar; it retries within 20 seconds |

**Gear menu:**

- **Display** - the settings dialog (see below).
- **Setup** - erases the saved Wi-Fi and calendar settings and restarts into
  the setup portal. Asks first.
- **Update** - downloads and installs new firmware from the *Firmware update
  URL*, then restarts. Asks first; tells you if no URL is set.

## When nobody's using it

- **Ambient clock** - after the *Screen timeout* without a touch, the
  calendar gives way to a large clock, each digit in one of your calendars'
  colours, dimmer at night (outside the *Week/day view* hours). The date
  shows top left. With a location set (see *Config web page*), the
  **weather** shows top right - an icon for the current conditions (a night
  version after dark) and the temperature. A small **bell** bottom left
  means a reminder is waiting. The clock, date and weather shift a few
  pixels every 10 minutes to protect the panel.
- **Tap the date** on the clock for *On this day* - notable events in
  history for today's date, from Wikipedia: the year and headline of
  each, with a sentence underneath. Drag to scroll.
- **Tap the weather** on the clock for the details, headed with the place
  and date: the temperature now and what it feels like, wind (direction,
  speed, gusts), rain so far today and air pressure; Today and Tomorrow
  with an icon, min / max, the chance and amount of rain and a short
  summary; and a line each for the five days after.
- Both close with a tap, or after a minute untouched, and go back to the
  clock - not the calendar. They also work while the screen is locked.
- **Screensaver** - if nobody's in front of it (with the optional presence
  sensor fitted; without one it always does this), it goes
  to a dark screensaver with the backlight off instead, and drops from the
  clock to the screensaver after 5 minutes with nobody there. Someone
  returning brings the clock back, never the calendar.
- **Touch** anywhere else to get back to the calendar. If it was away for 15
  minutes or more, it comes back on today in Month view.
- **Brightness** follows the room light automatically (with the optional
  light sensor fitted), fading smoothly rather than jumping.

## Reminders

15 minutes before a timed event (not all-day events), a card pops up with
its details. It stays until you tap it or the event starts. Pop-ups only
appear on the calendar - while the clock or screensaver is showing, the
bell stands in for them.

## Screen lock

Off until a PIN is set on the config page.

- **Locks** when you tap the padlock, after the *Lock after* minutes
  without a touch, and every time the device restarts.
- **While locked** only the clock or screensaver shows, and reminder
  pop-ups are held back (the bell still shows).
- **To unlock**, touch the screen and enter the 4-digit PIN - it opens the
  calendar as soon as the last digit is right. **✕** goes back to the
  clock; so does leaving the keypad for 30 seconds.
- **Five wrong PINs** in a row disable the keypad for 30 seconds.

It's a lock against casual use, not real security: the PIN is stored in
plain text, including on the SD card backup.

## Settings dialog (gear → Display)

| Setting | Choices |
|---|---|
| Screen timeout | 1, 2, 5, 10, 15 or 30 min, or Never |
| Refresh interval | 1, 2, 5, 10, 15, 30 or 60 min |
| Week/day view start | 4 AM - 10 AM |
| Week/day view end | 5 PM - midnight |
| Config web password | Sets the password for the config page (below) |

**Save & Restart** applies them. A value set on the config page that isn't
one of these choices appears as an extra entry and is kept unless you pick
something else.

## Config web page

Browse to the device's IP address from any device on the same network and
log in with the config web password (any user name). With no password set,
the page is turned off. **Save & restart** applies changes. Reload the page
before changing anything - a page left open from earlier saves its old
values back.

**Google service account** - the email and private key that let the
device read your calendars (see the README's *Google Cloud setup*).

**Calendars** - up to 8 rows:

- **Google** or **ICS URL** - a Google calendar ID (your Gmail address for
  your own calendar), which must be shared with the service account; or an
  ICS feed's secret address. ICS feeds don't show repeating events yet.
- **Label** and **colour** - how it appears in the legend and on events.
- **on** - shown when the device starts (the legend chips toggle it after).
- **daily** - fetch only once a day, for calendars that rarely change.

**Weather** - **Latitude** and **Longitude** of where you want the weather
for, in decimal degrees (south and west are negative, e.g. `-36.05` and
`146.46` - long-press a spot in Google Maps to see them), and an optional
**Location name** for the weather panel's heading. Leave the latitude blank
to turn the weather off. The weather comes from Open-Meteo, a free forecast
service; it's a forecast model's estimate for that spot rather than a
weather station's readings.

**Screen lock** - **PIN** (exactly 4 digits; blank keeps the current one),
**Remove the PIN** (turns the lock off), **Lock after** minutes without a
touch (0 = padlock only).

**Other:**

| Setting | Notes |
|---|---|
| Timezone | A POSIX TZ string, e.g. `AEST-10AEDT,M10.1.0,M4.1.0/3` for Melbourne |
| Refresh interval | Seconds between syncs |
| Firmware update URL | Where gear → Update fetches firmware from (optional) |
| Screen timeout | Seconds without a touch before the clock or screensaver; 0 = never |
| Week/day view hours | First and last hour shown in Week and Day view; also when the clock counts as daytime |
| Calendar cache window | Days back (up to 90) and ahead (up to 365) to fetch |

**Display brightness** - **Minimum brightness in the dark** (10.5% - 30%)
and **Room brightness where full backlight kicks in** (lux).

## First-time setup

On first start, or after gear → Setup, the device creates its own Wi-Fi
network, **GCal-Display-Setup**. Join it, browse to `http://192.168.4.1`,
fill in Wi-Fi and the Google details, and save.

## SD card

With a card in the slot, every restart saves a copy of all settings to
`/gcal/config.json` on it. If the device's own settings are ever wiped (for
example by flashing other firmware), it picks them back up from the card.
The copy includes passwords and keys in plain text - keep the card safe.
