#!/system/bin/sh

# Run once after the first boot, as root: the shell user may not disable a system app's components.
# Apps that every boot starts for a broadcast and that this guest has no use for: remote key
# provisioning, on-device personalization and federated computing, the health data and device lock
# controllers, and three receivers that only start a process. Disabled once; the state is kept in /data.
# The home screen's first layout (fixtures/home) is only read when the launcher has none yet. A guest
# from before that layout existed (state 2) has the launcher's empty one: it is dropped, once.
state="$(/system/bin/cmd settings get global nestvane_trim)"
# The device's name was taken from the image's model before that became "Nestvane" (state 3 and earlier).
# Media played a third of the way up before the image made full level the default (state 4 and earlier):
# a guest that has started before keeps the level it stored, so it is set once.
# There is no Bluetooth controller. The Bluetooth service is there since 1.16, because Settings and
# apps expect it, but turning Bluetooth on only makes its app stop, six times, at every boot. So the
# user may not turn it on (state 5 and earlier had no Bluetooth service at all).
if [ "$state" != "6" ]; then
    /system/bin/cmd package set-user-restriction --user 0 no_bluetooth 1
    /system/bin/cmd settings put global bluetooth_on 0
fi
if [ "$state" = "2" ] || [ "$state" = "3" ] || [ "$state" = "4" ]; then
    [ "$state" = "2" ] && /system/bin/cmd package clear --user 0 com.android.launcher3
    [ "$state" != "4" ] && /system/bin/cmd settings put global device_name Nestvane
    /system/bin/cmd media_session volume --stream 3 --set 15
    /system/bin/cmd settings put global nestvane_trim 6
elif [ "$state" = "5" ]; then
    /system/bin/cmd settings put global nestvane_trim 6
elif [ "$state" != "6" ]; then
    for package in com.android.rkpdapp com.android.federatedcompute.services \
            com.android.ondevicepersonalization.services com.android.healthconnect.controller \
            com.android.devicelockcontroller; do
        /system/bin/cmd package disable --user 0 "$package"
    done
    for component in android.ext.services/com.android.adservices.service.common.AdExtBootCompletedReceiver \
            com.android.shell/.HeapDumpReceiver \
            com.android.photopicker/.PhotopickerDeviceConfigReceiver; do
        /system/bin/cmd package disable --user 0 "$component"
    done
    /system/bin/cmd settings put global nestvane_trim 6
fi


# WebView's flags, read from this file on a debuggable build such as this one. V8 is told not to make
# machine code: here code made while running is interpreted instruction by instruction anyway, and
# V8's own interpreter is faster than that. Written at every boot, so that a change here takes effect.
echo "_ --js-flags=--jitless,--no-decommit-pooled-pages --disable-features=WebViewSurfaceControl" > /data/local/tmp/webview-command-line
/system/bin/toybox chmod 644 /data/local/tmp/webview-command-line
