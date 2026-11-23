#!/system/bin/sh

# Tells Android's apps that a text message arrived, for each one the person writes in the app: the
# SMS_RECEIVED broadcast, which an app waiting for a code listens for. text.sh puts the same message
# into the Messaging app; this runs as root because only the system may send the broadcast.
# /dev/nestvane-sms gives one message each time it is opened, the sender on the first line and the
# text on the second, and waits while there is none.

while true; do
    { IFS= read -r sender && IFS= read -r text; } < /dev/nestvane-sms || exit 1
    CLASSPATH=/system/framework/nestvane-sms.jar /system/bin/app_process64 /system/bin SmsBroadcast \
        "$sender" "$text" > /dev/null 2>&1
done
