# Status

Nestvane is an early release. An unsigned IPA is attached to each [release](https://github.com/atomsprout/nestvane/releases) ([installing.md](installing.md)); the app is not on the App Store.
This document says what works, what does not, and what has been measured.

Every number here comes from the lab record, [`results/G0.md`](../results/G0.md), and names the device it was measured on.
Numbers were taken at different stages of the project, so each table says which stage it describes.
Unless noted, the build is a development-signed Release build.

## Requirements

- iOS 26 or later.
- Tested on iPhone 15 Pro and iPhone 17 Pro. Earlier stages were also run on an iPhone 11 and an iPad Air (M3).
- The app needs the Extended Virtual Addressing and Increased Memory Limit entitlements.

## What works

- **Android boots to the launcher.** The unmodified `init`, `zygote`, `system_server`, SurfaceFlinger, SystemUI and Launcher3 start, and the home screen is shown with a clock widget and a dock.
- **Touch.** Taps, swipes and long presses reach Android. The app drawer opens; apps start from it.
- **Settings.** The home page and the Network & internet, Apps and Display pages open; the Battery page shows the phone's level.
- **Installing apps from APK files**, through the Install APK… command, or by opening an `.apk` with the app from Files, AirDrop or a share sheet.
- **Open-source apps.** 15 apps install from their APKs: 2048, Fossify Clock, Fossify Calendar, Fossify Gallery, Fossify Notes, Markor, NewPipe, K-9 Mail, KeePassDX, AntennaPod, F-Droid, Tasks, Wikipedia, DuckDuckGo and ScriptWidget. Not all of them run well; see below.
- **The on-screen keyboard** appears and accepts input (seen in Tasks).
- **Home-screen widgets.** The widget picker lists the widgets of installed apps; a widget can be added and is still there after a restart. ScriptWidget's widgets render.
- **Network.** Android's connectivity check passes, and Wikipedia loads its front page.
- **Shared storage.** The folder `Documents/Android` is Android's shared storage and is visible in the iOS Files app. A file put there from iOS appears in Android's Files app after Android is restarted.
- **Persistence.** Installed apps, home-screen widgets and files under `/data` are still there after Android is restarted.
- **Battery level and time zone** of the phone are shown by Android.
- **Video and sound.** Android's `MediaPlayer` plays a 1280×720 H.264 video with AAC sound: the position advances one second a second, about 30 frames a second are composed (the video has 30), and the sound reaches the phone's output at the level it has in the file. NewPipe opens, lists videos and plays a YouTube live stream with sound. Measured on the samples that arrive at the phone's output; how it sounds has not been judged by ear, and resolutions above 720p have not been measured.

## What does not work yet

- **Hardware video decoding, recording and the microphone.** Video is decoded by Android's software codecs, which costs processor time that a hardware decoder would not. There is no sound input and no camera recording.
- **Google apps and Play services.** The system is AOSP. Apps that need Google's services will not run.
- **IPv6.** The guest network is IPv4 only.
- **Live changes to the shared folder from iOS.** They show up only after Android restarts.
- **Apps with heavy native code are slow**, because the native libraries inside an APK are interpreted. Some apps do not start.
- **120 Hz.** Asking for it gains nothing; see the frame rate figures below.

## Measured numbers

### Boot to the launcher

Time from starting Android to the launcher being displayed, not counting the first start after installation, which is longer.

| Device | Stage | Time |
| --- | --- | --- |
| iPhone 15 Pro | Current, without the media services | about 8 s |
| iPhone 15 Pro | Current, with the media services | about 10 s |
| iPhone 17 Pro (iOS 27.0.1) | When the frame rate work was done | 7.7 to 8.5 s; 17.6 s on the first start |
| iPhone 15 Pro | Earlier design, interpreter only | 606 s |
| iPad Air (M3) | First stage with translated code | 12.2 and 12.4 s; 38.1 s on the first start |
| iPhone 11 | First stage with translated code | 30.7 s |

Along the way on the iPhone 15 Pro the same measurement read about 11 s, then 9.1 s, then 7.1 to 7.4 s, then 6.3 to 6.6 s before many apps were installed and the media services were added.
What each step changed is in [design-notes.md](design-notes.md) and the record.

For comparison, the same Android image on a real Linux kernel in the reference emulator on a Mac (4 cores, first boot after wiping data) displays the launcher 6.84 s after the kernel starts.

### Memory and size

| Quantity | Value | Device and stage |
| --- | --- | --- |
| Memory footprint | about 2.4 to 2.7 GB | iPhone 15 Pro; 2.38 GB with fourteen apps installed, about 2.72 GB with the media services |
| App download size | about 1 GB | 1.14 GB as a zipped app archive at the stage where it was measured |
| Translated code inside the app | 1.70 GB uncompressed | After the code tables were removed from the app |
| Memory limit of the process | 6,138 MiB | iPhone 15 Pro with both entitlements |
| Memory limit of the process | 2,343 MiB | iPhone 11 with both entitlements |
| Address space available to the app | about 53 GiB | Both devices, with the entitlement; about 7 GiB without |

### Display

| Measurement | Value | Device |
| --- | --- | --- |
| Peak frame rate while scrolling a Settings list, at 60 Hz | 55 to 56 frames a second | iPhone 15 Pro |
| Typical frame rate in the same test, at 60 Hz | 30 to 46 frames a second, peak 52 | iPhone 17 Pro |
| The same at 120 Hz | 33 to 44 frames a second, peak 51 | iPhone 17 Pro |

### Responsiveness

Measured on an iPhone 15 Pro with scripted taps.

| Action | Time until the screen is shown |
| --- | --- |
| Opening the Files app | within 0.5 s |
| Opening Settings | 0.4 to 0.8 s |
| Opening a Settings sub-page | 0.15 to 0.35 s |
| Opening ScriptWidget's main screen | 0.5 to 0.9 s |

### Translated and interpreted code

| Measurement | Value | Where |
| --- | --- | --- |
| Rewritten command-line tools against the same binaries on Linux | 1.07 to 1.6 times the native time, geometric mean about 1.16 with a single base | Mac |
| The same with the chunk table used in the app, estimated | about 1.4 to 1.5 times native | Mac |
| Zygote preloading 18,596 classes | 111 ms | iPhone 15 Pro, experimental runtime |
| Recovering from a fault inside the process | 1.5 to 2.6 µs on average | iPhone 15 Pro and iPhone 11 |
| First widget preview from ScriptWidget, whose script engine is interpreted | about 2 minutes after opening the app | iPhone 15 Pro |

The last row shows the cost of interpretation. The app's own interface is Java code run by ART and the system's libraries, all of which are translated, and it opens in under a second. Its JavaScript engine is a native library inside the APK, which has no translation and is interpreted; rendering the previews took minutes.

## How well the tested apps run

From the record, on an iPhone 15 Pro. "Opens" means the app reached a usable screen without crashing.

| App | Observed |
| --- | --- |
| 2048 | Opens; goes through its introduction to the main menu. |
| Fossify Clock | Opens; shows running time; asks for the notification permission. |
| Fossify Calendar | Opens. |
| Fossify Gallery, Fossify Notes | Open; show their permission dialogs (file access, notifications). |
| Markor | Opens; with all-files access granted it shows the shared storage. It does not list `.md` files placed in `Documents` from outside; the cause is not known. |
| Tasks | Opens; a new task can be typed. |
| K-9 Mail, KeePassDX, F-Droid | Open to their first screens. |
| Wikipedia | Opens and loads its front page over the network. |
| ScriptWidget | Opens; widgets render, slowly. |
| NewPipe | Opens in about 2 seconds, lists videos with their pictures, and plays a YouTube live stream with sound at about 30 frames a second. |
| AntennaPod | Crashed at start before shared storage existed. Not tested again since. |
| DuckDuckGo | Its process started and loaded its native libraries; no window within 25 seconds. Not confirmed since. |

## Known limitations

- **Shared storage has no per-app access control.** File operations do not pass through the media provider, and the kernel layer does not check owners or modes for files under the shared storage ([kernel-layer.md](kernel-layer.md)).
- **Large files after power loss.** The content of a file of 1 MiB or more is not flushed to disk in step with the snapshot; after a power loss it can be older than the snapshot ([devices.md](devices.md)).
- **Storage limits.** 256 MiB per file and 2 GiB for `/data` in total. Larger files in the shared folder do not appear in Android.
- **SELinux decisions are captured, not computed.** Services added after the capture run under rules written by hand.
- **One scheduler thread.** System calls of all guest processes run one at a time.
- **Android thinks it is an emulator.** Settings titles the device page "About emulated device".
- **The `Documents` folder** also holds diagnostic text files and screenshots written by the app, which show up in the Files app.
- **Not verified:** F-Droid fetching content, large downloads, long-lived connections, unmounting the shared storage, the quick action on the app's home-screen icon being tapped on a phone, and fault recovery under a distribution signature (it was measured with a development signature).
