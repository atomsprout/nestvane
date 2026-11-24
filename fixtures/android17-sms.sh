#!/system/bin/sh

# Tells Android's apps that a text message arrived, for each one the person writes in the app: the
# SMS_RECEIVED broadcast, which an app waiting for a code listens for. text.sh puts the same message
# into the Messaging app; this runs as root because only the system may send the broadcast.
# /dev/nestvane-sms gives one message each time it is opened, the sender on the first line and the
# text on the second, and waits while there is none.

# An empty sender is a request of the app's instead, which needs root: "clean" clears the apps in the
# background, as Clear all among the recent apps does (the person long-pressed Home in Nestvane).

while true; do
    { IFS= read -r sender && IFS= read -r text; } < /dev/nestvane-sms || exit 1
    if [ -z "$sender" ]; then
        case "$text" in
        clean)
            # Only apps the person installed are ended by package: a system app such as Settings runs
            # as the system user, and ending a package's processes goes by user (it stopped Android).
            installed=$(/system/bin/cmd package list packages -3 2> /dev/null)
            for package in $(CLASSPATH=/system/framework/nestvane-clean.jar /system/bin/app_process64 /system/bin \
                             CleanBackground 2> /dev/null); do
                case "$installed" in
                *"package:$package"*) /system/bin/cmd activity kill "$package" > /dev/null 2>&1 ;;
                esac
            done
            /system/bin/cmd activity kill-all > /dev/null 2>&1
            ;;
        esac
        continue
    fi
    CLASSPATH=/system/framework/nestvane-sms.jar /system/bin/app_process64 /system/bin SmsBroadcast \
        "$sender" "$text" > /dev/null 2>&1
done
