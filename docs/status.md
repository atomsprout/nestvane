# Status

Nestvane is an early release, and for now only developers can run it: the IPA must be signed with a paid Apple Developer Program membership and sideloaded. An unsigned IPA is attached to each [release](https://github.com/atomsprout/nestvane/releases) ([installing.md](installing.md)); the app is not on the App Store.
This document says what works, what does not, and what has been measured.

Every number here comes from the lab record, [`results/G0.md`](../results/G0.md), and names the device it was measured on.
Numbers were taken at different stages of the project, so each table says which stage it describes.
Unless noted, the build is a development-signed Release build.

## Requirements

- iOS 17 or later, and at least 6 GB of memory.
- Tested on iPhone 14 (iOS 17.3.1), iPhone 15 Pro, iPhone 17 Pro and iPad Air 11-inch (M3). On an iPad the display is sharp and fills the screen, laid out as on a phone; it turns with the device. Earlier stages were also run on an iPhone 11 and an iPad Air (M3).
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
- **Text messages written in the app.** The More menu's Send Text Message… takes a sender and a text; Android's Messaging app lists it as an incoming message and a notification icon appears in the status bar. Nothing is sent or received over any network: the guest has no phone process and no radio, so nothing arrives the way a message does on a phone. Apps that listen for incoming SMS (to read a code, say) are told by a broadcast the guest sends itself; an app has to have been opened once and hold the permission to receive SMS, as on a phone.
- **120 Hz**, off by default, on a screen that shows 120: see the display figures below. Frames go to the screen as surfaces copied on the graphics processor, taken when the screen is about to refresh; nothing is read back.
- **Less drawing when the phone is hot.** At iOS's "serious" thermal state Android draws at most half the display's rate, at "critical" at most 30 frames a second. Checked with a pretended state, not a hot phone.
- **Changing network.** When the phone moves between Wi-Fi and cellular the guest's TCP connections are ended, so apps connect again at once instead of waiting on connections that lead nowhere.
- **Libraries laid out for 4 KiB pages** load: the guest's pages are 16 KiB, and the linker's compatible way of loading is on for every app. See [Apps that were tried](compatibility.md).
- **Submit a Bug** in the More menu opens a new GitHub issue with the versions of Nestvane and iOS and the phone's model written in.
- **An update notice.** Once a day the app asks GitHub which release is the latest and says so when it is newer; it installs nothing itself, and the question can be turned off in the app's Settings.
- **Video and sound.** Android's `MediaPlayer` plays a 1280×720 H.264 video with AAC sound: the position advances one second a second, about 30 frames a second are composed (the video has 30), and the sound reaches the phone's output at the level it has in the file. NewPipe opens, lists videos and plays a YouTube live stream with sound. Measured on the samples that arrive at the phone's output; how it sounds has not been judged by ear, and resolutions above 720p have not been measured.
- **1080p video.** A 1920×1080 H.264 video at 6 Mbit/s plays at its 30 frames a second on an iPhone 14, and a 1080p HEVC video plays in time, at 29 to 31 frames a second once it has settled. Before 1.5 an HEVC video stopped the codec process: its decoder has routines written by hand in assembly that use registers the translated code keeps for itself.
- **Landscape.** Android's display turns with the device, and an app that asks for landscape is shown the right way up, with touches landing where they are made (checked on an iPhone 14 with Android a quarter turn round).
- **The microphone.** The sound card Android sees has a capture device, fed from the device's microphone while an Android app records. iOS asks for the microphone the first time. Seen: a voice recorder starts recording and Android reads the device. Not yet checked: what it records once the microphone is allowed.
- **Web pages inside apps (1.6).** Android System WebView 128, AOSP's prebuilt build of Chromium, is part of the system. Its library was not built with the registers the translated code keeps for itself set aside; the translator now lends other registers in their place, and the library is translated ahead of time like the rest. A Wikipedia article loads in a WebView in 4.4 seconds on an iPad Air and 8.3 on an iPhone 14. JavaScript runs in V8's interpreter, with no JIT. Toutiao's feed, AnkiDroid and Jellyfin, which needed a WebView, now open. Renderer processes run without Chromium's seccomp sandbox, and WebView's overlays are off.
- **Storage that is the device's (1.7).** Android's Storage page shows the size of the volume Nestvane is on and what iOS says is free on it, less 1 GiB kept back; Android can write until then.
- **Root, when you want it (1.7).** Off unless you turn it on in Nestvane's Settings. Then `su` exists in Android, and the first time an app runs it iOS asks whether that app may have root; the answers are kept and can be changed in Settings. An allowed app's `su -c id` gives `uid=0(root)`, with every capability and an SELinux domain that may do anything.
- **Running commands (1.7).** Java's `Runtime.exec` and `ProcessBuilder` used to hang the calling thread (`vfork`, and asking a pipe how much it holds, were missing); the usual commands (`ls`, `id`, `cp`, `ps` and about 70 more) are now in `/system/bin`.
- **Apps' own native libraries run faster (1.7).** They are still interpreted, now about twice as fast: common instructions are decoded once, and a thread that keeps interpreting moves to a core of its own. The renderers of WebView run in Chromium's seccomp sandbox again.
- **Clearing background apps (1.8).** Touch and hold Home in Nestvane's bar and choose Clear Background Apps: recent tasks other than what is on the screen go, the person's own apps in the background are ended, and cached processes with them. On an iPad Air memory went from 4.42 GB to 2.83 GB.
- **Root shells and app names (1.8).** An app can keep `su` open and write commands to it, as terminal apps do. The root prompt and the list in Settings show the app's name, not only its package.

## What does not work yet

- **Hardware video decoding and camera recording.** Video is decoded by Android's software codecs, which costs processor time that a hardware decoder would not: 1080p at 30 frames a second takes half of one core on an iPhone 14. Using the phone's decoder needs a codec component built into Android.
- **A tablet's layout on an iPad.** At a tablet's density this build of Android does not work: the launcher stops as it starts, and after it restarts, apps' windows are not drawn. An iPad gets the phone's layout. In landscape it is 920 units wide, and apps that lay themselves out by width use their wide layout.
- **The home screen does not turn.** The launcher of this Android keeps a phone's home screen the way the display was made, so it stays as it was when Nestvane started; apps turn.
- **Google apps and Play services.** The system is AOSP. Apps that need Google's services will not run.
- **IPv6.** The guest network is IPv4 only.
- **Live changes to the shared folder from iOS.** They show up only after Android restarts.
- **Apps with heavy native code are slow**, because the native libraries inside an APK are interpreted. Some apps do not start. ScriptWidget's previews do not appear: it stops a script that runs past its deadline, and interpreted, its scripts do.

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
| A finger kept moving on the same list, at 120 Hz (off by default) | 120 frames a second composed, 86 to 120 shown | iPhone 15 Pro |

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
| First widget preview from ScriptWidget, whose script engine is interpreted | about 2 minutes after opening the app (before 1.7); none within 3 minutes on an iPad Air with 1.7, the scripts run past the app's own deadline | iPhone 15 Pro, iPad Air |

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
- **Storage limits.** 1 GiB per file; `/data` as a whole up to what the device has free, less 1 GiB. Larger files in the shared folder do not appear in Android.
- **SELinux decisions are captured, not computed.** Services added after the capture run under rules written by hand.
- **A paid developer account is required**, because each Android process takes host address space of its own and the total needs the Extended Virtual Addressing entitlement. A developer of Twoyi suggested the way out, a software MMU with one pool of memory shared by all processes, with our thanks; it is planned and not started ([PLAN.md](../PLAN.md), in Chinese).
- **One scheduler thread.** System calls of all guest processes run one at a time.
- **Android thinks it is an emulator.** Settings titles the device page "About emulated device".
- **The `Documents` folder** also holds diagnostic text files and screenshots written by the app, which show up in the Files app.
- **Not verified:** F-Droid fetching content, large downloads, long-lived connections, unmounting the shared storage, the quick action on the app's home-screen icon being tapped on a phone, and fault recovery under a distribution signature (it was measured with a development signature).
