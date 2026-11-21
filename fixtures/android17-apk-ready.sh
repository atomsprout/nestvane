#!/system/bin/sh

# The image has no setup wizard and no Provision app, so nothing tells Android that setup is over.
# Until it is told, Settings exits as soon as it opens and recent tasks are not kept.
/system/bin/cmd settings put global device_provisioned 1
/system/bin/cmd settings put secure user_setup_complete 1
# A provisioned device turns its display off after a while, and shows a lock screen unless the image
# says otherwise (ro.lockscreen.disable.default). The guest is one app's window on a phone that does
# both itself.
/system/bin/cmd settings put system screen_off_timeout 2147483647

# The backup service registers its package receiver before its journal exists.
# The "users" dump only lists services whose constructors have returned.
attempt=0
while [ "$attempt" -lt 45 ]; do
    if users=$(/system/bin/dumpsys -t 10 backup users); then
        case "$users " in
            "Backup Manager is running for users:"*" 0 "*) break ;;
        esac
    fi
    attempt=$((attempt + 1))
    /system/bin/toybox sleep 3 || exit 1
done
if [ "$attempt" -eq 45 ]; then
    echo "Nestvane APK installer: backup service for user 0 is unavailable" >&2
    exit 1
fi

case "$#" in
    0) exec /system/bin/nestvane-apk-install ;;
    1)
        /system/bin/nestvane-apk-install "$1" || exit "$?"
        exec /system/bin/cmd activity start -W -n dev.nestvane.smoke/.MainActivity
        ;;
    *) exit 2 ;;
esac
