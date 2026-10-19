#!/bin/sh
# Development build: like the normal build, plus sdkconfig.dev (plain-http firmware URLs for the
# authenticated /ota trigger - what tools/gcal_remote.py ota uses). Builds into build-dev/ with
# its own sdkconfig, so the default (https-only) build and its sdkconfig are untouched.
#   tools/dev_build.sh build
#   tools/dev_build.sh -p /dev/cu.usbmodem... flash
# Remember to send the display a dev build for each wireless update: a default build
# does not accept plain-http updates.
cd "$(dirname "$0")/.." || exit 1
exec idf.py -B build-dev -DSDKCONFIG=sdkconfig.dev.out \
     -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.dev" "$@"
