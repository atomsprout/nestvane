#!/system/bin/sh

# Does what the app asks of Android. /dev/nestvane-text gives one request each time it is opened,
# on three lines, the kind and two fields, and waits while there is none.
#
# "text", sender, text: shows a text message the person wrote in the app as one that Android
# received. The guest has no phone process and no radio, so nothing arrives the way a message does
# on a phone: this writes the message into the provider the Messaging app reads, and posts a
# notification for it.
#
# "rate", frames a second: the most frames a second apps are to draw, which the app lowers while
# the phone is hot. The display keeps its rate; Android draws every second or fourth frame. 0 puts
# back what the system image sets.
#
# "launch", package name: opens that app, as tapping its icon does. The app's tests use it.
#
# "uninstall", package name: removes that app, as dragging its icon to Uninstall does. For tests.
#
# "rotate", quarter turns: which way up Android holds its display, sent when the person turns the
# device. An app that asks for one orientation still gets it.

# Android's content tool, without its wrapper script: the guest root has no app_process link.
content() {
    CLASSPATH=/system/framework/content.jar /system/bin/app_process64 /system/bin com.android.commands.content.Content "$@"
}

# The provider drops what an app that is not the default SMS app writes, unless this is allowed.
/system/bin/cmd appops set com.android.shell WRITE_SMS allow

# The emulator's system image has USB debugging on. Nothing can reach it here, and its daemon
# reconnects to the framework every five seconds for as long as it is on.
/system/bin/cmd settings put global adb_enabled 0 > /dev/null 2>&1

while true; do
    { IFS= read -r kind && IFS= read -r sender && IFS= read -r text; } < /dev/nestvane-text || exit 1
    case "$kind" in
    rate)
        if [ "$sender" -gt 0 ] 2> /dev/null; then
            /system/bin/cmd settings put system peak_refresh_rate "$sender.0" > /dev/null 2>&1
        else
            /system/bin/cmd settings delete system peak_refresh_rate > /dev/null 2>&1
        fi
        ;;
    uninstall)
        /system/bin/toybox log -t Nestvane "uninstall $sender: $(/system/bin/cmd package uninstall "$sender" 2>&1)"
        ;;
    rotate)
        case "$sender" in
        0|1|2|3) /system/bin/cmd window user-rotation lock "$sender" > /dev/null 2>&1 ;;
        esac
        ;;
    launch)
        activity=$(/system/bin/cmd package resolve-activity --brief -c android.intent.category.LAUNCHER "$sender" 2> /dev/null | /system/bin/toybox tail -n 1)
        /system/bin/toybox log -t Nestvane "launch $sender: $activity"
        case "$activity" in
        # With the action and the category of a launcher's tap: a start by name alone is refused
        # for an activity whose filter it does not match.
        */*) /system/bin/cmd activity start-activity -a android.intent.action.MAIN \
                 -c android.intent.category.LAUNCHER -n "$activity" > /dev/null 2>&1 ;;
        esac
        ;;
    text)
        now=$(/system/bin/toybox date +%s)000
        content insert --uri content://sms/inbox \
            --bind address:s:"$sender" --bind body:s:"$text" --bind date:l:"$now" \
            --bind read:i:0 --bind seen:i:0 --bind type:i:1 > /dev/null 2>&1
        /system/bin/cmd notification post -t "$sender" "nestvane-text-$now" "$text" > /dev/null 2>&1
        ;;
    esac
done
