# Nestvane 1.0 in numbers

Nestvane starts Android 17 inside an iPhone app, without a just-in-time compiler and without a hypervisor.
This is the last report before the source is published: what the first release measures, on which phone, and
where the numbers stop. Every figure comes from the lab record, [`results/G0.md`](../results/G0.md).

## One run from a fresh install

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
| The same at 120 Hz | 33 to 44, peak 51: no gain | iPhone 17 Pro |
| 1280×720 H.264 video with AAC sound | 28 to 31 frames a second of 30, position one second a second | iPhone 15 Pro |
| Opening Settings | 0.4 to 0.8 s | iPhone 15 Pro |
| Opening a Settings sub-page | 0.15 to 0.35 s | iPhone 15 Pro |

Video is decoded by Android's software codecs. Sound was measured on the samples that reach the phone's output,
at the level they have in the file; nobody has yet judged it by ear.

## Where it is slow

Code that came with Android is translated. Code that comes inside an APK is not: iOS will not run code made on
the device, so an app's own native libraries are interpreted, at about 40 million instructions a second.
ScriptWidget shows the cost. Its interface is Java, run by translated code, and opens in under a second; its
script engine is a native library, and the first widget preview took about two minutes on an iPhone 15 Pro.

Fifteen open-source apps have been installed. Most open to a usable screen. AntennaPod crashed at start at an
earlier stage and DuckDuckGo showed no window within 25 seconds; neither has been tried again.

## What has not been measured

- Sound by ear, and video above 720p.
- Sideloading tools. Signed without the two capabilities, as a free Apple ID would sign it, Android had not reached
  its home screen after five minutes on an iPhone 15 Pro.
- Long sessions: battery use, heat over an hour, memory over a day.
- Devices with less than 8 GB of memory. An iPhone 11 allows one app about 2.3 GB, which is less than Android needs.
- Opening several apps in one session from the test harness; each app above was opened after its own start.

## Where the rest is

[status.md](status.md) lists what works and what does not. [design-notes.md](design-notes.md) says what each
step of the boot time cost. [installing.md](installing.md) says how to put the release on a phone.
