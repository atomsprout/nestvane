#!/bin/zsh
# Ends an app on a device, found by its bundle identifier: the process whose executable is inside
# the place that app is installed. (By name, another app whose name has the same word can be the
# one that is found, and the app goes on running.) Says nothing when the app is not running.
#   end-app.sh <device UDID> [bundle identifier]
set -u
device=${1:?device UDID} bundle=${2:-com.atomsprout.nestvane}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
xcrun devicectl device info apps --device $device --bundle-id $bundle --json-output $work/apps.json > /dev/null 2>&1
xcrun devicectl device info processes --device $device --json-output $work/processes.json > /dev/null 2>&1
for pid in $(python3 -I - $work/apps.json $work/processes.json $bundle <<'PYTHON'
import json, sys
try:
    apps = json.load(open(sys.argv[1]))["result"]["apps"]
    processes = json.load(open(sys.argv[2]))["result"]["runningProcesses"]
except (OSError, KeyError, ValueError):
    sys.exit(0)
for app in apps:
    if app.get("bundleIdentifier") == sys.argv[3] and app.get("url"):
        for process in processes:
            if process.get("executable", "").startswith(app["url"]):
                print(process["processIdentifier"])
PYTHON
); do
    xcrun devicectl device process terminate --device $device --pid $pid > /dev/null 2>&1
done
