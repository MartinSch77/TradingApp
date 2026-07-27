#!/bin/bash
set -ex -o pipefail
# Only one axivion_ci per project: concurrent runs (e.g. build_all.sh while a
# manual run is active) share build_axivion/ and delete each other's IR right
# before the dashboard upload. clean_all.sh during a run does the same.
exec 9>"$HOME/.axivion-TradingApp.lock"
if ! flock -n 9; then
    echo "another Axivion run for TradingApp is already active — aborting" >&2
    exit 1
fi
. /home/schulemn/bauhaus-suite/bauhaus-kshrc
if [ -z "$AXIVION_USERNAME" ] && [ -z "$AXIVION_PASSWORD" ] && [ -z "$AXIVION_PASSFILE" ]; then
# You may put dashboard credentials inside such a guarded block:
export AXIVION_USERNAME=admin
export AXIVION_PASSWORD=password
fi
export BAUHAUS_CONFIG="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
# Toolchain setup command: /home/schulemn/bauhaus-suite/bin/gccsetup --cc 'gcc ' --cxx 'g++ ' --config /home/schulemn/TradingApp/axivion/compiler_config.json

# The Axivion cmake configure doesn't pass -DCMAKE_PREFIX_PATH, so find_package would
# fall back to the system Qt6 (which lacks Qt6Charts) and fail. Point it at the same
# online-installer Qt the normal build uses; prepend so any existing value still wins.
export CMAKE_PREFIX_PATH="/home/schulemn/Qt/6.10.2/gcc_64${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"


# --jobs (no N) = parallel analysis jobs, auto-sized: cores capped so each job
# keeps >=2 GB RAM (AXIVION_MIN_MEM_PER_CORE). Placed after "$@" so an explicit
# caller-supplied -j N still wins; the Ninja build phase is already parallel
# (native parallelization since Suite 7.11.5).
axivion_ci "$@" --jobs
