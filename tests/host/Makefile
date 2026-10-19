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
          -I$(ROOT)/components/provisioning/include

SRC := test_main.c test_rss.c test_ics.c test_auth.c test_urldecode.c fake_http.c \
       $(ROOT)/components/provisioning/auth_guard.c $(ROOT)/components/provisioning/url_decode.c \
       $(ROOT)/components/rss_feed/rss_parse.c $(ROOT)/components/gcal/ics_client.c

run_tests: $(SRC) $(wildcard *.h shim/*.h)
	$(CC) $(CFLAGS) $(SRC) -lm -o $@

test: run_tests
	./run_tests

clean:
	rm -rf run_tests run_tests.dSYM

.PHONY: test clean
