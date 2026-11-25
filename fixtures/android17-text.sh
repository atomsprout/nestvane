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
# "launch", package name: opens that app, as tapping its icon does; or an action of Settings, which
# opens that page. The app's tests use it.
#
# "uninstall", package name: removes that app, as dragging its icon to Uninstall does. For tests.

#
# "scan", a directory of the shared storage: files were put there from outside while Android ran,
# and the media library is to look at them (ProbeLinux::rescanShared).
#
# "shell", a command: runs it, for the app's tests; the output goes to nestvane-test/shell.txt in
# the shared storage and to the log, tagged NestvaneShell.
#
# "labels", anything: tells Nestvane the names of the installed apps (tools/app-labels).
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
    shell)
        # A service has no /sdcard of its own: the shared storage is under /data/media.
        output=$(/system/bin/sh -c "content() { CLASSPATH=/system/framework/content.jar /system/bin/app_process64 /system/bin com.android.commands.content.Content \"\$@\"; }; $sender" 2>&1; echo "exit $?")
        /system/bin/toybox mkdir -p /data/media/0/nestvane-test 2> /dev/null
        printf '$ %s\n%s\n' "$sender" "$output" >> /data/media/0/nestvane-test/shell.txt 2> /dev/null ||
            /system/bin/toybox log -t NestvaneShell "cannot write shell.txt"
        /system/bin/toybox log -t NestvaneShell "$output"
        ;;
    scan)
        # The media library's own call for it; on a directory it looks at everything inside.
        scanned=$(content call --uri content://media --method scan_file --arg "$sender" 2>&1)
        /system/bin/toybox log -t Nestvane "scan $sender: $scanned"
        ;;
    labels)
        # The names of the installed apps, for Nestvane's root prompt and Settings.
        CLASSPATH=/system/framework/nestvane-labels.jar /system/bin/app_process64 /system/bin AppLabels > /dev/null 2>&1
        ;;
    rotate)
        case "$sender" in
        0|1|2|3) /system/bin/cmd window user-rotation lock "$sender" > /dev/null 2>&1 ;;
        esac
        ;;
    launch)
        # An action of Settings opens that page, such as android.settings.INTERNAL_STORAGE_SETTINGS.
        case "$sender" in
        android.settings.*)
            /system/bin/cmd activity start-activity -a "$sender" > /dev/null 2>&1
            continue
            ;;
        esac
        resolved=$(/system/bin/cmd package resolve-activity --brief -c android.intent.category.LAUNCHER "$sender" 2>&1)
        activity=$(echo "$resolved" | /system/bin/toybox tail -n 1)
        case "$activity" in
        */*) /system/bin/toybox log -t Nestvane "launch $sender: $activity" ;;
        *) /system/bin/toybox log -t Nestvane "launch $sender: not resolved: $resolved" ;;
        esac
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
