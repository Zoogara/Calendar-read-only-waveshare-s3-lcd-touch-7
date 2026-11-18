# Host-side unit tests for the pure parsing code. They need only a C compiler:
#   make -C tests/host test
# The firmware sources are compiled unchanged; shim/ stands in for the few ESP-IDF headers they
# include and fake_http.c for the HTTP client, so the real ICS parser runs on canned text.

ROOT := $(abspath ../..)
CC   ?= cc
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable \
          -fsanitize=address,undefined -fno-omit-frame-pointer \
          -Ishim -I. -I$(ROOT)/components/rss_feed/include -I$(ROOT)/components/gcal/include \
          -I$(ROOT)/components/app_common/include \
          -I$(ROOT)/components/ha_weather/include \
          -I$(ROOT)/components/calendar_ui/include \
          -I$(ROOT)/components/provisioning/include

SRC := test_main.c test_rss.c test_ics.c test_wind.c test_auth.c test_urldecode.c test_jwt.c test_urlenc.c test_timefmt.c test_eventtext.c test_setcrypt.c fake_http.c fake_store.c \
       $(ROOT)/components/provisioning/auth_guard.c $(ROOT)/components/provisioning/url_decode.c $(ROOT)/components/gcal/jwt_build.c $(ROOT)/components/gcal/url_encode.c $(ROOT)/components/calendar_ui/time_fmt.c $(ROOT)/components/gcal/event_text.c $(ROOT)/components/provisioning/settings_crypt.c \
       $(ROOT)/components/ha_weather/ha_wind_calc.c \
       $(ROOT)/components/rss_feed/rss_parse.c $(ROOT)/components/gcal/ics_client.c

# mbedTLS (the settings backup's PBKDF2 and AES-GCM): the system's if there is one (apt install libmbedtls-dev),
# otherwise ESP-IDF's own copy, built once into build-mbedtls/ (set MBEDTLS_SRC to another checkout's
# components/mbedtls/mbedtls if IDF_PATH isn't set).
MBEDTLS_SRC ?= $(firstword $(wildcard $(IDF_PATH)/components/mbedtls/mbedtls $(HOME)/esp/esp-idf/components/mbedtls/mbedtls))
ifneq ($(wildcard /usr/include/mbedtls/gcm.h /usr/local/include/mbedtls/gcm.h /opt/homebrew/include/mbedtls/gcm.h),)
MBED_CFLAGS :=
MBED_LIBS   := -lmbedcrypto
MBED_DEP    :=
else ifneq ($(MBEDTLS_SRC),)
MBED_CFLAGS := -I$(MBEDTLS_SRC)/include
MBED_LIBS   := build-mbedtls/libmbedcrypto.a
MBED_DEP    := build-mbedtls/libmbedcrypto.a
build-mbedtls/libmbedcrypto.a:
	sh build_mbedtls.sh $(MBEDTLS_SRC) $(CC)
else
$(error mbedTLS not found: install it (libmbedtls-dev / brew install mbedtls) or set MBEDTLS_SRC)
endif

run_tests: $(SRC) $(wildcard *.h shim/*.h) $(MBED_DEP)
	$(CC) $(CFLAGS) $(MBED_CFLAGS) $(SRC) $(MBED_LIBS) -lm -o $@

test: run_tests
	./run_tests

clean:
	rm -rf run_tests run_tests.dSYM

.PHONY: test clean
