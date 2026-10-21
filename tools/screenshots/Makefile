# Host-side renderer: builds the real calendar_ui sources and LVGL 8.4 for this computer and
# renders screens with demo data to PNG files. Not part of the firmware build.
#   make -C tools/screenshots           # build ./render and write PNGs to ./out
# Needs: a C compiler, Python 3 with Pillow, and LVGL fetched by a normal `idf.py build`
# (managed_components/lvgl__lvgl) - or set LVGL=/path/to/lvgl-8.4.0.

ROOT    := $(abspath ../..)
LVGL    ?= $(ROOT)/managed_components/lvgl__lvgl
BUILD   := build
CC      ?= cc
CFLAGS  := -O1 -g -w -std=gnu11 -MMD -MP -DLV_CONF_KCONFIG_EXTERNAL_INCLUDE=\"sdkconfig.h\" -DHOST_RENDER \
           -Ishim -I$(LVGL) -I$(LVGL)/src \
           -I$(ROOT)/components/calendar_ui/include -I$(ROOT)/components/app_common/include \
           -I$(ROOT)/components/gcal/include \
           -I$(ROOT)/components/weather/include -I$(ROOT)/components/onthisday/include \
           -I$(ROOT)/components/rss_feed/include -I$(ROOT)/components/provisioning/include \
           -I$(ROOT)/components/presence_sensor/include -I$(ROOT)/components/light_sensor/include \
           -I$(ROOT)/components/ota_update/include -I$(ROOT)/components/board_bsp/include \
           -include shim/host_prefix.h

LVGL_DIRS := core draw draw/sw hal misc widgets font extra extra/themes extra/themes/default \
             extra/layouts extra/layouts/flex extra/layouts/grid \
             extra/others
LVGL_SRC  := $(foreach d,$(LVGL_DIRS),$(wildcard $(LVGL)/src/$(d)/*.c)) $(wildcard $(LVGL)/src/extra/widgets/*/*.c)
UI_SRC    := $(filter-out %ui_settings_dialog.c %/calendar_ui.c %/ui_screensaver.c,$(wildcard $(ROOT)/components/calendar_ui/*.c))
GCAL_SRC  := $(ROOT)/components/gcal/event_store.c
APP_SRC   := render.c demo_data.c host_stubs.c host_calendar_ui.c host_screensaver.c

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(subst $(ROOT)/,root/,$(subst $(LVGL)/,lvgl/,$(LVGL_SRC) $(UI_SRC) $(GCAL_SRC))) $(APP_SRC))

all: render

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/lvgl/%.o: $(LVGL)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/root/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# -MMD writes a .d file per object listing every header (and the firmware .c files that
# host_calendar_ui.c / host_screensaver.c include), so a change to any of them rebuilds
# what depends on it - including after switching git branches, whose headers differ.
-include $(OBJS:.o=.d)

render: $(OBJS)
	$(CC) $(OBJS) -lm -o $@

shots: render
	./render out
	python3 topng.py out

clean:
	rm -rf $(BUILD) render out
