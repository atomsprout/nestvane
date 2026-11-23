#!/system/bin/sh

# Shows a text message the person wrote in the app as one that Android received. The guest has no
# phone process and no radio, so nothing arrives the way a message does on a phone: this writes the
# message into the provider the Messaging app reads, and posts a notification for it.
# /dev/nestvane-text gives one message each time it is opened, the sender on the first line and the
# text on the second, and waits while there is none.

# Android's content tool, without its wrapper script: the guest root has no app_process link.
content() {
    CLASSPATH=/system/framework/content.jar /system/bin/app_process64 /system/bin com.android.commands.content.Content "$@"
}

# The provider drops what an app that is not the default SMS app writes, unless this is allowed.
/system/bin/cmd appops set com.android.shell WRITE_SMS allow

while true; do
    { IFS= read -r sender && IFS= read -r text; } < /dev/nestvane-text || exit 1
    now=$(/system/bin/toybox date +%s)000
    # The tool prints a SecurityException after the row is in: the provider then tries to change an
    # app-op as the phone UID, which nothing here lets it do. The message is stored all the same.
    content insert --uri content://sms/inbox \
        --bind address:s:"$sender" --bind body:s:"$text" --bind date:l:"$now" \
        --bind read:i:0 --bind seen:i:0 --bind type:i:1 > /dev/null 2>&1
    /system/bin/cmd notification post -t "$sender" "nestvane-text-$now" "$text" > /dev/null 2>&1
done
