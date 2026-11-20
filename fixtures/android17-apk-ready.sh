#!/system/bin/sh

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
