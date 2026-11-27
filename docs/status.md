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
- **Shared storage.** The folder `Documents/Android` is Android's shared storage and is visible in the iOS Files app. A file put there from iOS appears in Android within a few seconds (1.10).
- **Persistence.** Installed apps, home-screen widgets and files under `/data` are still there after Android is restarted.
- **Battery level and time zone** of the phone are shown by Android.
- **Text messages written in the app.** The More menu's Send Text Message… takes a sender and a text; Android's Messaging app lists it as an incoming message and a notification icon appears in the status bar. Nothing is sent or received over any network: the guest has no phone process and no radio, so nothing arrives the way a message does on a phone. Apps that listen for incoming SMS (to read a code, say) are told by a broadcast the guest sends itself; an app has to have been opened once and hold the permission to receive SMS, as on a phone.
- **120 Hz**, off by default, on a screen that shows 120: see the display figures below. Frames go to the screen as surfaces copied on the graphics processor, taken when the screen is about to refresh; nothing is read back.
- **Less drawing when the phone is hot.** At iOS's "serious" thermal state Android draws at most half the display's rate, at "critical" at most 30 frames a second. Checked with a pretended state, not a hot phone.
- **Changing network.** When the phone moves between Wi-Fi and cellular the guest's TCP connections are ended, so apps connect again at once instead of waiting on connections that lead nowhere.
- **Libraries laid out for 4 KiB pages** load: the guest's pages are 16 KiB, and the linker's compatible way of loading is on for every app. See [Apps that were tried](compatibility.md).
- **Submit a Bug** in the More menu opens a new GitHub issue with the versions of Nestvane and iOS and the phone's model written in.
- **An update notice.** Once a day the app asks GitHub which release is the latest and says so when it is newer; it installs nothing itself, and the question can be turned off in the app's Settings. Since 1.11 the notice at the bottom of the screen comes once for each new version; after that only the More menu and Settings show it.
- **Video and sound.** Android's `MediaPlayer` plays a 1280×720 H.264 video with AAC sound: the position advances one second a second, about 30 frames a second are composed (the video has 30), and the sound reaches the phone's output at the level it has in the file. NewPipe opens, lists videos and plays a YouTube live stream with sound. Measured on the samples that arrive at the phone's output; how it sounds has not been judged by ear, and resolutions above 720p have not been measured.
- **1080p video.** A 1920×1080 H.264 video at 6 Mbit/s plays at its 30 frames a second on an iPhone 14, and a 1080p HEVC video plays in time, at 29 to 31 frames a second once it has settled. Before 1.5 an HEVC video stopped the codec process: its decoder has routines written by hand in assembly that use registers the translated code keeps for itself.
- **Landscape.** Android's display turns with the device, and an app that asks for landscape is shown the right way up, with touches landing where they are made (checked on an iPhone 14 with Android a quarter turn round).
- **The microphone.** The sound card Android sees has a capture device, fed from the device's microphone while an Android app records. iOS asks for the microphone the first time. With the microphone allowed, a test app on an iPad Air recorded the room: Android received 16 kHz mono at about 31 KB a second, with peaks around 800 to 950 where silence is 0. Not yet judged by ear.
- **Web pages inside apps (1.6).** Android System WebView 128, AOSP's prebuilt build of Chromium, is part of the system. Its library was not built with the registers the translated code keeps for itself set aside; the translator now lends other registers in their place, and the library is translated ahead of time like the rest. A Wikipedia article loads in a WebView in 4.4 seconds on an iPad Air and 8.3 on an iPhone 14. JavaScript runs in V8's interpreter, with no JIT. Toutiao's feed, AnkiDroid and Jellyfin, which needed a WebView, now open. Renderer processes run without Chromium's seccomp sandbox, and WebView's overlays are off.
- **Storage that is the device's (1.7).** Android's Storage page shows the size of the volume Nestvane is on and what iOS says is free on it, less 1 GiB kept back; Android can write until then.
- **Root, when you want it (1.7).** Off unless you turn it on in Nestvane's Settings. Then `su` exists in Android, and the first time an app runs it iOS asks whether that app may have root; the answers are kept and can be changed in Settings. An allowed app's `su -c id` gives `uid=0(root)`, with every capability and an SELinux domain that may do anything.
- **Running commands (1.7).** Java's `Runtime.exec` and `ProcessBuilder` used to hang the calling thread (`vfork`, and asking a pipe how much it holds, were missing); the usual commands (`ls`, `id`, `cp`, `ps` and about 70 more) are now in `/system/bin`.
- **Apps' own native libraries run faster (1.7).** They are still interpreted, now about twice as fast: common instructions are decoded once, and a thread that keeps interpreting moves to a core of its own. The renderers of WebView run in Chromium's seccomp sandbox again.
- **Apps' own native libraries keep running beside Android (1.9).** A thread that interprets an app's library on a core of its own used to hand itself back at every floating-point or SIMD instruction and at every return from a system library; it now runs all of those itself. On a Mac benchmark the interpreter runs 195 million instructions a second on the main loop instead of 168 million, and 242 million instead of 210 million on a worker.
- **Clearing background apps (1.8).** Touch and hold Home in Nestvane's bar and choose Clear Background Apps: recent tasks other than what is on the screen go, the person's own apps in the background are ended, and cached processes with them. On an iPad Air memory went from 4.42 GB to 2.83 GB.
- **Root shells and app names (1.8).** An app can keep `su` open and write commands to it, as terminal apps do. The root prompt and the list in Settings show the app's name, not only its package.
- **Installing an APK from Android's own file manager (1.10).** Tapping an APK in Files opens Android's installer, which asks to confirm and installs it. Before, nothing happened: the media library, through which apps open shared files, waited forever on a kind of file lock (`F_OFD_GETLK`) that Nestvane did not answer. Opening any shared file through it was affected.
- **A shared folder that follows the Files app (1.10).** Files and folders put into Nestvane's shared folder, or removed from it, from the Files app or a computer show up or go in Android within a few seconds, without a restart, and the media library is told. Nestvane looks every three seconds once the home screen is up.
- **Pictures, videos and sounds open (1.10).** The system had no app for them, so tapping one did nothing. A small built-in viewer, not on the home screen, opens them from a file manager: a picture can be pinched and moved, and a 1080p HEVC video plays.
- **Recent apps (1.10).** A key in Nestvane's bar opens Android's recent apps. An APK handed over from iOS is judged by its content, so one shared under another name installs too.
- **Wi-Fi, as apps see it (1.11).** Android's network is reported to apps as Wi-Fi, connected and validated: many apps count only Wi-Fi and cellular as being online and took the Ethernet network it was before for none. Android's older way of asking (`NetworkInfo.getType()`) still says Ethernet: that needs the connectivity module rebuilt.
- **IPv6 (1.11).** Where the phone's network has IPv6, Android looks up IPv6 addresses and connects to them, with IPv4 still tried first where a name has both: on an iPhone 14 on Wi-Fi, an app got `www.google.com`'s IPv6 addresses and connected to one in 15 ms. On a network with no IPv4 at all, IPv4 destinations go through the network's NAT64 gateway, as since 1.4; that has still not been tried on such a network.
- **A faster start (1.11).** On an iPhone 14 Android reaches its home screen in 8.4 s (median of five starts, 8388 ms; 1.10: 8747 ms). Closing a file or reading a socket no longer wakes every waiting thread, the table of translated code is not written one system call per page, and file and mount look-ups no longer walk long lists.
- **Export Diagnostics (1.11).** In the More menu. For when Android seems frozen: Nestvane writes one text file with what every guest thread is waiting for, memory, the system server's Java stacks and Android's recent log, with the app's own records of the start and the network, and opens the share sheet. If the runtime itself is stuck it says so and gives what was written down before.
- **A low-memory killer (1.11).** Android's `lmkd` stopped at its first system call (`mlockall`) in every version before, so Android never knew which apps it could end. It now runs, from when the home screen is up: started with Android, it cost the start a few hundred milliseconds. When a new process finds no room left in the address space iOS gives the app, Nestvane ends the cached app Android ranks lowest, as Linux's low-memory killer would, and the process starts. Before, the start failed, and the app that was tapped did not open; with a few large apps started at boot this happened within a minute on an iPhone 14.
- **Screen recordings and saving into the media library (1.11).** The system's screen recorder records and saves (a 10-second recording on an iPhone 14: 64 frames, 668 KB, listed in the media library); a recording copied into Movies, or recorded straight into it as a camera app does, keeps its size. The recordings that came out empty (0 KB) were most likely the same media-library wait that 1.10 fixed.
- **Backing up and restoring Android (1.12).** Back Up Android… in the More menu stops Android, saves its apps, their data, its settings and the root answers in one file (an Apple Archive), lets you choose where to keep it, and starts Android again. The shared folder is not in it: it is in the Files app already. Restore Android… puts a chosen backup in place and starts Android from it; what Android had before is kept until it reaches its home screen from the backup, and put back if it stops before that. On an iPhone 14, 507 MB of data made a 331 MB backup; restoring it took under a second and brought back an app uninstalled after the backup and a file changed after it. A whole-iPhone backup already holds Android's data: this one is for going back to a moment, or for moving only Android.
- **KernelSU Manager (1.12).** With root on, the official KernelSU Manager (3.3.1, from KernelSU's releases) shows KernelSU as working, version 32680, and lists the apps. Its switch for an app allows or refuses root, and these are the same answers as the iOS prompt and Nestvane's Settings. The kernel layer answers KernelSU's driver (its ioctls, UAPI 5) itself; only the Manager signed by KernelSU is taken for the Manager. Tested on an iPhone 14: allowed, an app became root through KernelSU's driver; refused, it was turned away.
- **Programs in an app's own files run (1.12).** A program in `/data`, such as one in an app's native libraries directory, could never be started; now an executable one runs. A process that catches SIGTERM is no longer left hanging when another sends it one.
- **Many apps opened one after another (1.13).** Fifty more apps were tried, a hundred and four in all ([the list](compatibility.md)). Opening several in one start of Android ended Nestvane or stopped Android in ways a single app never did, and these are fixed: when little memory is left Nestvane ends the apps Android ranks lowest before iOS ends Nestvane (on an iPhone 14 iOS allows about 4.3 GB); a process that faults or gets no host memory is ended alone, not all of Android; WebView's zygote no longer waits for ever after its first renderer ends, which had stopped every later app from starting; a 400 MB APK is handed to Android without a copy.
- **Apps that use JNA or Rust run natively (1.13).** Code that libffi writes into a file mapped executable (Bitwarden, Element X, Firefox Focus, Proton VPN), and libraries that apps load with their own loader (WeChat), made the whole app interpreted. Now only that code is. Firefox opens; its helper processes stop.
- **The front camera (1.13).** Android has two cameras: the iPhone's back camera and now its front one, each 640×480, with the front one not mirrored. On an iPhone 14, Camera2 lists both (facing back and facing front). Photos and videos save: on an iPhone 14 (after the 1.13 release) a test app took a 640×480 JPEG with each camera and a five-second video with sound with the back one (H.264, 145 frames, AAC), all saved into DCIM/Camera, in the media library and in the shared folder in Files. iOS must be allowed to give Nestvane the camera: if Camera is off for Nestvane in iOS Settings, Android's camera fails to start. A video that fails to record leaves an empty hidden `.pending-` file. In 1.13, apps that use the older camera interface (Camera API 1), such as Open Camera, cannot open a camera: Android had no media profiles, its built-in ones stop video at 352×288, and API 1 then offers no preview size for a 640×480 camera. Since 1.14 Nestvane gives Android media profiles with 640×480; Open Camera (from F-Droid) then took a photo and a 6-second video with sound on an iPhone 14.
- **WebView no longer crashes after a few seconds (1.14).** Chromium's renderer locks and unlocks its discardable memory with ashmem's pin and unpin requests, which the kernel layer refused; Chromium then freed memory it still used, and the renderer crashed about 11 seconds after a WebView opened, taking the app with it (1.12 and 1.13 had this). The kernel layer now answers them. On an iPhone 14 the WebView test app and eight apps, each opened three times in one start of Android, ran without a crash.
- **Android's data stays compact (1.14).** Android's `/data` is kept in one database that every start reads in full. Installing and removing many apps left it mostly free pages (209 MB for 45 MB of records); it is now compacted when it opens like that, which took about 170 ms off the start on an iPhone 14.
- **Contacts, calendar, blocked numbers and the user dictionary (1.15).** The system had none of these providers, so an app that read contacts or the calendar, or Settings' personal dictionary, found nothing to ask, and some stopped. They are now part of Android, taken from the same build; on an iPhone 14 the contacts and dictionary providers answer queries.
- **Safety and emergency in Settings opens (1.15).** It stopped Settings at once: the page asks the emergency information app, which the system did not have.
- **KernelSU Manager keeps what you set for an app (1.15).** The Manager saves a whole profile for each app (default or custom: the user, groups, capabilities and SELinux domain root gets). Nestvane kept only whether root was allowed, so the Manager read back the default and the setting seemed to undo itself. The profile is now kept, in backups too, and a custom one is what the app gets when it becomes root. Not yet tried with the Manager on a phone.
- **Network and internet, Connected devices, Sound and vibration and Accessibility in Settings open (1.16).** They stopped Settings at once: these pages use the Wi-Fi and Bluetooth services without checking, and the system had neither. Both are there now and say they are off: there is no Wi-Fi or Bluetooth hardware here, and Bluetooth cannot be turned on. The Wi-Fi service goes through its start-up once the boot has completed, so it takes no time from the boot.
- **The home screen turns with the device (1.16).** In landscape the home screen and the app drawer used to stay a small upright picture; they now turn into the landscape layout as apps do.
- **H.264 and HEVC are decoded by the phone's decoders (1.16; up to 4K since 1.18).** Video up to 3840x2160 goes to the iPhone's hardware decoders instead of Android's software ones. On an iPhone 14, at 30 frames a second: 1080p H.264 (High, 6 Mbit/s) shows 29.4 frames with the app at 42% of one core, where the same clip decoded in software takes 166%; 1080p HEVC 29.3 frames at 42%; 4K H.264 (20 Mbit/s) 28.6 frames at 75% and 4K HEVC 29.0 frames at 71%, where 4K in software reached 8.8.
- **Steadier with many apps installed (1.18).** Every Android process and Nestvane itself share one address space that the iPhone gives (about 63 GB), and each app process takes 1.1 to 1.3 GB of it. They used to take it until none was left: a new process could not start, and the whole app closed when a 4K video began. Now 2 GB are always kept free, and when they are not, the least needed app in the background is ended first, as Android itself does. On an iPhone 14, ten apps opened in turn for two rounds came to the front every time.
- **Turning works from the start (1.19).** Android used to run a program of its own only to report which way up the display is; it started 15 seconds after the start and held nearly 1 GB of address space. The rotation is now read from the system's own log, and the program is no longer started.
- **Fewer apps in the background are ended (1.20).** Every app process starts as a copy of one parent, and each copy used to take its own 1.1 to 1.3 GB of address space. Now the megabytes that hold only read-only pages of files stay shared between parent and copies, and a process gets its own only when it changes something there: each app process takes about 300 MB less. On an iPhone 14, 40 seconds after the start, Android's processes take 31.6 GB where they took 37.7 GB; with seven apps opened in turn for three rounds, 2 apps in the background were ended where 1.19 ended 11.
- **A file replaced from outside no longer closes the app (1.21).** The files of the shared storage are shared with Android page for page. When one was replaced or shortened from the Files app or from a Mac while an Android app had it open, touching a part the file no longer had closed the whole app; now that part reads as zeros, and when Android next opens the file it gets what is there now. On an iPhone 14, a 12 MB file read in a loop while it was replaced by a 5 MB and then a 15 MB one gave the new content each time.
- **A faster start (1.22).** The table that tells translated code where the code for an Android address is was built as a start went along, on the one thread everything waits for: a tenth of that thread's time. The parts a start needed are now remembered, and at the next start another thread builds them ahead. On an iPhone 14 the home screen is up in 7.7 s where it took 8.7.
- **Apps that ask for the Wi-Fi service get one (1.16).** It says Wi-Fi is off, but it is no longer missing; the old `NetworkInfo.getType()` reports Wi-Fi (the type's name is still "Ethernet"). KDE Connect and Session, which stopped as they started for that reason, both open now (tried on 1.17).
- **Airplane mode cuts the network (1.17).** Android's network is a wired interface, which airplane mode leaves on; now, while airplane mode is on, Nestvane carries nothing for Android and ends the connections there are, and carries again when it is turned off. Android's status bar still shows the network as connected.
- **The built-in keyboard types digits (1.17).** Holding a letter of the top row gave no digit, and the digits of the ?123 page did nothing: the system lacked the virtual keyboard's key map.

## What does not work yet


- **Bluetooth and Wi-Fi themselves.** Both services are there, but there is no hardware: Bluetooth cannot be turned on, and Wi-Fi shows as off and finds no networks. Android still reaches the network through a wired interface, over the phone's Wi-Fi or cellular connection.
- **Hardware decoding of VP9 and AV1, and hardware encoding.** VP8, VP9, AV1 and all encoding are still software.
- **A tablet's layout on an iPad is an experiment.** The app's Settings have a Tablet layout switch (on an iPad only), off by default. 1.16 fixes the launcher stopping as it starts at a tablet's density, and at such a density on an iPhone 14 the home screen, Settings and Files show. It has not been tried on an iPad since the fix. With 1.18 it was tried on an iPhone 14 at an iPad Air's display size (1160x1578): the home screen, Settings and Wikipedia show. With the switch off an iPad gets the phone's layout.
- **Google apps and Play services.** The system is AOSP. Apps that need Google's services will not run.
- **KernelSU modules.** The Manager lists none and cannot install one: nothing runs KernelSU's `ksud` as Android starts, and there are no overlay mounts for a module's files.
- **Apps with heavy native code are slow**, because the native libraries inside an APK are interpreted. Some apps do not start. ScriptWidget's previews do not appear: it stops a script that runs past its deadline, and interpreted, its scripts do.

## Measured numbers

### Boot to the launcher

Time from starting Android to the launcher being displayed, not counting the first start after installation, which is longer.

| Device | Stage | Time |
| --- | --- | --- |
| iPhone 14 (iOS 17.3.1) | 1.22, the release check: median of five starts | 7.7 s (7.4 to 8.3) |
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
- **A paid developer account is required**, because each Android process takes host address space of its own and the total needs the Extended Virtual Addressing entitlement. A developer of Twoyi suggested the way out, a software MMU with one pool of memory shared by all processes, with our thanks; it was measured in 1.9 and is not planned for now: without the entitlement iOS gives an app 5 to 6 GiB of addresses, and Android uses about 34 GiB by the home screen ([PLAN.md](../PLAN.md), in Chinese).
- **One scheduler thread.** System calls of all guest processes run one at a time.
- **Android thinks it is an emulator.** Settings titles the device page "About emulated device".
- **The `Documents` folder** also holds diagnostic text files and screenshots written by the app, which show up in the Files app.
- **Not verified:** F-Droid fetching content, large downloads, long-lived connections, unmounting the shared storage, the quick action on the app's home-screen icon being tapped on a phone, and fault recovery under a distribution signature (it was measured with a development signature).
