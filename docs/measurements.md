# Nestvane in numbers

Nestvane starts Android 17 inside an iPhone app, without a just-in-time compiler and without a hypervisor.
This is the last report before the source is published: what the first release measures, on which phone, and
where the numbers stop. Every figure comes from the lab record, [`results/G0.md`](../results/G0.md).

## Version 1.4

Measured on 23 November 2026 with the release build of 1.4 and `tools/regress-device.sh`.

| | iPhone 14 (iOS 17.3.1), 9 apps | iPad Air 11-inch M3 (iPadOS 27.0.1), 1 app |
| --- | --- | --- |
| Boot to the Android home screen | 10.9 s; 16.5 s the first time after installing | 7.4 s; 12.1 s the first time after installing |
| Memory footprint at the home screen | 2.43 GB | 2.37 to 2.40 GB |
| Android's display | 578 × 1064 pixels | 1160 × 1578 pixels |
| A finger kept moving on a list | up to 60 frames a second | up to 60 |
| Installing an APK | 0.5 s | 0.3 s |

On the iPhone 14 a 720p video played at 30 to 31 frames a second, and `ping` to 1.1.1.1 took 12 to 35 ms. On an
iPhone 17 Pro with thirty apps, before the last two changes of this version: 7.5 s, 3.04 to 3.08 GB, up to 120
frames a second.

## Version 1.3

Measured on 23 November 2026 with the release build of 1.3 and `tools/regress-device.sh`.

| | iPhone 17 Pro (iOS 27.0.1), 30 apps | iPhone 14 (iOS 17.3.1), 5 apps |
| --- | --- | --- |
| Boot to the Android home screen | 10.1 s; 12.2 s the first time after installing | 9.4 s; 19.3 s the first time after installing |
| Memory footprint at the home screen | 3.10 to 3.17 GB | 2.28 to 2.33 GB |
| A finger kept moving on a list, display at 120 Hz | up to 120 frames a second | up to 60: its screen shows 60 |
| The same at 60 Hz | up to 60 | up to 60 |
| Installing an APK | 0.3 s | 0.3 s |

The more apps are installed, the slower Android starts and the more memory it uses: with ten apps the iPhone 17
Pro started in 6.6 to 7.8 s and used 2.48 GB (version 1.2, below). The iPhone 14 has 6 GB of memory and gives
the app about 4.3 GB. Its starts measured earlier the same day ranged from 11.5 to 20.1 s; the cause of the
spread is not known. Which apps open is in [Apps that were tried](compatibility.md).

## Version 1.2

Measured on 22 November 2026 with the release build of 1.2, on an iPhone 17 Pro (iOS 27.0.1) with ten apps
installed, after installing it over 1.1.

| | |
| --- | --- |
| Boot to the Android home screen | 6.6 to 7.8 s (four starts); 10.7 s the first time after installing |
| Memory footprint at the home screen | 2.48 GB, peak 2.57 GB |
| A finger kept moving on a list, 120 Hz on | 111 to 120 frames a second handed to iOS; the screen asked for a frame 120 times a second |
| The same, 120 Hz off | 58 to 60 |
| Processor time in that test at 120 Hz | 48 to 56% of one core; 85 to 95% the old way, in the same build |
| 720p H.264 video | 29 to 32 frames a second of 30 |
| A text message, from the request to an app receiving it | about 0.4 s |
| Installing an APK | 0.2 s |

Frames no longer pass through the processor on their way to the screen: 1.1 read each one back and handed iOS
a picture. The refresh count is iOS's callbacks, still not a measurement of the panel.

## Version 1.1

Measured on 22 November 2026 with the release build of 1.1, on an iPhone 15 Pro (iOS 26.2) and an iPhone 17 Pro
(iOS 27.0.1). The rest of this page is the report written for 1.0 and is kept as it was.

| | |
| --- | --- |
| Boot to the Android home screen, no apps added | 7.7 to 8.1 s (iPhone 15 Pro); 17.3 s the first time after installing |
| The same with eight apps kept from 1.0 | 9.8 to 11.6 s (iPhone 17 Pro, installed over 1.0) |
| Memory footprint at the home screen | 2.38 to 2.55 GB |
| A finger kept moving on a list, 120 Hz on | 113 to 121 frames a second composed, 85 to 120 shown |
| The same, 120 Hz off | 58 to 60 |
| 720p H.264 video | 29 to 32 frames a second of 30 (iPhone 17 Pro) |
| Installing an APK | 0.1 to 0.5 s |
| Download | 1.07 GB, about 3.3 GB installed |

"Shown" counts frames handed to iOS, not refreshes of the screen itself. Before 1.1 Android drew at most 60
frames a second whatever the display was set to: resource overlays of its framework were never applied, because
the program that applies them was missing from the system image.

## One run from a fresh install (1.0)

On 22 November 2026 the release build was installed on an iPhone 17 Pro (iOS 27.0.1) that did not have the app,
and everything below was done in order.

| Step | Result |
| --- | --- |
| Installing the app (3.2 GB on the phone) | 32 to 34 s over the cable |
| First start, to the Android home screen | 16.9 s and 17.6 s (two fresh installs) |
| Later starts | 9.5 to 10.9 s (seven starts) |
| Memory footprint at the home screen | 2.21 to 2.38 GB, with 4.1 to 4.2 GB left before the limit |
| Installing an APK (8 to 105 MB) | 0.1 to 0.4 s each |
| Icon of a new app in the drawer | 0.3 s after installing; about 11 s in Android's first minute |

Six apps were installed from their APK files and each was opened: 2048, Fossify Clock, Markor, Wikipedia, NewPipe
and ScriptWidget. All six reached their first screen. Wikipedia showed the day's front page with its pictures,
fetched over the phone's network; NewPipe listed live streams.

## How long a boot takes, and how it got there

| Device | Stage | Boot to the launcher |
| --- | --- | --- |
| iPhone 15 Pro | Interpreter only, the first design | 606 s |
| iPhone 11 | First stage with translated code | 30.7 s |
| iPad Air (M3) | First stage with translated code | 12.2 s |
| iPhone 15 Pro | Release, without the media services | about 8 s |
| iPhone 15 Pro | Release, with the media services | about 10 s |
| iPhone 17 Pro | Release | 9.5 to 10.9 s |

The step from 606 seconds to about 10 is the whole idea of the project. An interpreter runs every guest
instruction through a loop; iOS forbids the usual cure, compiling at run time. So Android is rebuilt with five
registers kept free (`x18` and `x25`–`x28`), and each of its about 1,230 ARM64 modules is rewritten ahead of time
into code that iOS signs and runs directly. Ordinary instructions are not changed; only system calls and indirect
branches leave through the runtime. Rewritten command-line tools take 1.07 to 1.6 times as long as the same
binaries on Linux.

For comparison, the same Android image on a real Linux kernel, in the reference emulator on a Mac with four cores,
shows the launcher 6.84 seconds after its kernel starts.

## Size

| Quantity | Value |
| --- | --- |
| Download (IPA) | 1.06 GB |
| Installed app | 3.2 GB: 1.8 GB of translated code and graphics, 1.5 GB of Android |
| Translated modules | about 1,230, linked into 12 frameworks |
| Runtime source | about 26,000 lines of C++; the kernel layer alone is 12,600 |
| The patch to AOSP | 23 files |

## Display, video and sound

| Measurement | Value | Device |
| --- | --- | --- |
| Scrolling a Settings list at 60 Hz | up to 55 to 56 frames a second | iPhone 15 Pro |
| The same | 30 to 46 frames a second, peak 52 | iPhone 17 Pro |
| A finger kept moving on the same list, at 60 Hz | 60 frames a second composed and shown | iPhone 15 Pro |
| The same at 120 Hz | 120 composed, 86 to 120 shown | iPhone 15 Pro |
| 1280×720 H.264 video with AAC sound | 28 to 31 frames a second of 30, position one second a second | iPhone 15 Pro |
| 1920×1080 H.264 video (High profile, 6 Mbit/s) with AAC sound | 30 frames a second composed and shown, position one second a second, half of one processor core | iPhone 14 |
| 1920×1080 HEVC video (4 Mbit/s) with AAC sound | position one second a second; 29 to 31 frames a second once settled, 14 to 25 in the first seconds after starting right at boot | iPhone 14 |
| Opening Settings | 0.4 to 0.8 s | iPhone 15 Pro |
| Opening a Settings sub-page | 0.15 to 0.35 s | iPhone 15 Pro |

Video is decoded by Android's software codecs. They are part of the system, so they run as translated code, at
the processor's own speed; that is enough for 1080p at 30 frames a second, and the phone's hardware decoder is
not used. Sound was measured on the samples that reach the phone's output, at the level they have in the file;
nobody has yet judged it by ear.

## Where it is slow

Code that came with Android is translated. Code that comes inside an APK is not: iOS will not run code made on
the device, so an app's own native libraries are interpreted, at about 40 million instructions a second.
ScriptWidget shows the cost. Its interface is Java, run by translated code, and opens in under a second; its
script engine is a native library, and the first widget preview took about two minutes on an iPhone 15 Pro.

A hundred and four apps have been installed and opened once each, and eighty-seven open to a usable screen: the
list, with what stops the others, is in [Apps that were tried](compatibility.md).

## What has not been measured

- Sound by ear, video above 1080p or 30 frames a second, and what the microphone records: Android reads its
  capture device and iOS asks for the microphone, and nobody has yet allowed it and listened.
- Sideloading tools. Signed without the two capabilities, as a free Apple ID would sign it, Android had not reached
  its home screen after five minutes on an iPhone 15 Pro.
- Long sessions: battery use, heat over an hour, memory over a day.
- Devices with less than 8 GB of memory. An iPhone 11 allows one app about 2.3 GB, which is less than Android needs.
- Opening several apps in one session from the test harness; each app above was opened after its own start.

## Where the rest is

[status.md](status.md) lists what works and what does not. [design-notes.md](design-notes.md) says what each
step of the boot time cost. [installing.md](installing.md) says how to put the release on a phone.
