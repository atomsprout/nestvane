#!/system/bin/sh

# Starts the rotation watcher (tools/rotation-watch), which tells the app which way up Android's
# display is. It is a Java program, and starting one takes the processor for a second or two: started
# with the other services at the end of boot it kept the home screen waiting five seconds longer. So it
# waits until the home screen has had time to come up. Until it reports, the app takes the display to
# be the way it was made, which is how the app asks Android to hold it at the start.

/system/bin/toybox sleep 15
CLASSPATH=/system/framework/nestvane-rotation.jar exec /system/bin/app_process64 /system/bin RotationWatch
