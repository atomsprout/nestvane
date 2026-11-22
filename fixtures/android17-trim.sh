#!/system/bin/sh

# Run once after the first boot, as root: the shell user may not disable a system app's components.
# Apps that every boot starts for a broadcast and that this guest has no use for: remote key
# provisioning, on-device personalization and federated computing, the health data and device lock
# controllers, and three receivers that only start a process. Disabled once; the state is kept in /data.
if [ "$(/system/bin/cmd settings get global nestvane_trim)" != "2" ]; then
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
    /system/bin/cmd settings put global nestvane_trim 2
fi

