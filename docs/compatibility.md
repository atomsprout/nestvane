# Apps that were tried

Open-source apps installed from their APK files and opened once each. This says how far each one got, not how
well it works: "opens" means its first screen was drawn, and nothing beyond that was tried unless a note says so.

Tried on 23 November 2026 on an iPhone 17 Pro (iOS 27.0.1) with the build that became version 1.3. The APKs came
from F-Droid or the projects' own releases. Each app was opened by its package name through the app's test
channel, and a frame was saved 15 to 18 seconds later.

| App | Version | Its own native libraries | Result |
| --- | --- | --- | --- |
| 2048 (Privacy Friendly) | 1.4.2 | none | Opens |
| Aegis Authenticator | from F-Droid, build 82 | 2 | Opens, at its introduction |
| Amaze File Manager | from F-Droid, build 126 | 1 | Opens, and asks for a permission |
| AntennaPod | 3.12.3 | 1 | Opens. Before 1.3 it stopped as it started: see below |
| Fossify Calendar | 1.12.2 | 1 | Opens |
| Fossify Clock | 1.6.0 | 1 | Opens |
| Fossify Contacts | from F-Droid, build 13 | 1 | Opens, and asks for a permission |
| Fossify File Manager | from F-Droid, build 16 | 1 | Opens |
| Fossify Gallery | 1.14.2 | 12 | Opens, and asks for a permission |
| Fossify Notes | 1.7.0 | 1 | Opens |
| F-Droid | 2.0.3 | 1 | Opens, and asks for a permission |
| K-9 Mail | 25.0 | 4 | Opens, at its welcome screen |
| KeePassDX | 4.5.8 | 2 | Opens |
| Loop Habit Tracker | from F-Droid, build 20302 | none | Opens, at its introduction |
| Markor | 2026.10.0 | none | Opens, at its introduction |
| NewPipe | 0.29.3 | 2 | Opens |
| ScriptWidget | 1.1.1 | 1 | Opens. Its first widget preview takes about two minutes |
| Tasks | 15.14 | 4 | Opens |
| Termux | from F-Droid, build 1002 | 2 | Opens, and asks for a permission |
| Wikipedia | r/50616 | 4 | Opens; search and the home feed work. Articles do not: they need a WebView |
| DuckDuckGo | 5.301.1 | 10 | Does not open: it is a browser and needs a WebView. In 1.3 it stopped Android: see below |
| Feeder | from F-Droid, build 4191 | 2 | Opens. In 1.3 it stopped Android: see below |

Twenty-one of the twenty-two open.

## Thirty-two more, tried for 1.5

Tried on 23 November 2026 on an iPad Air 11-inch (M3, iPadOS 27.0.1) with builds of version 1.5 as it was being
made, each app again after the fix it led to. From F-Droid, except Telegram, Signal, Toutiao and Douyin, which
are the builds their makers offer on their own sites. A frame was saved 16 to 30 seconds after opening, and up
to two minutes for the slow ones.

| App | Version | Its own native libraries | Result |
| --- | --- | --- | --- |
| App Manager | 4.1.1 | 4 | Opens, and asks for a permission |
| Aurora Store | 4.8.4 | 1 | Opens, at its welcome screen |
| Binary Eye | 1.75.7 | 3 | Opens, at its welcome screen |
| Breezy Weather | 6.2.2 | 3 | Opens |
| Fennec (Firefox from F-Droid) | 160.0.0 | 15 | Opens, at its welcome screen, after one and a half to two minutes |
| FairEmail | 1.2341 | 1 | Opens, at its welcome screen |
| Fossify Calculator | 1.4.0 | 1 | Opens |
| Fossify Camera | 1.5.0 | 3 | Opens, and asks for a permission |
| Fossify Messages | 1.10.2 | 1 | Opens, and asks to be the SMS app |
| Fossify Music Player | 1.8.5 | 1 | Opens, and asks for a permission |
| Fossify Phone | 1.11.1 | 1 | Opens, and asks to be the phone app |
| Fossify Voice Recorder | 1.7.4 | 2 | Opens and records, from the device's microphone once iOS has been allowed to give it |
| Joplin | 3.7.14 | 26 | Opens; its list of notes shows within two minutes |
| LibreTube | 32.1 | none | Opens, and shows what is trending |
| Luanti | 5.18.0 | 2 | Opens, in landscape |
| Mastodon | 2.13.7 | none | Opens, at its welcome screen |
| Mindustry | 8, build 160.8 | 2 | Opens, at its loading screen, in landscape |
| MuPDF viewer | 1.28.5a | 1 | Opens, and asks for a file |
| Nextcloud | 35.1.0 | 6 | Opens, at its welcome screen |
| Organic Maps | 2026.11.11 | 1 | Opens, and asks for a permission |
| OsmAnd~ | 5.4.11 | 7 | Opens, at its welcome screen |
| PPSSPP | 1.19.3 | 5 | Opens, in landscape |
| Shattered Pixel Dungeon | 4.0.2 | 2 | Opens, at its title screen |
| Signal | 8.36.3 | 12 | Opens, at its welcome screen. Before 1.5 it could not make a key: see below |
| Telegram X | 0.28.11 | 8 | Opens, at its welcome screen |
| Toutiao (今日头条) | 19.2.0 | 161 | Opens, with its tabs and bars; the feed stays empty: its network library needs a WebView |
| Douyin (抖音) | 32.2.6 | 204 | Opens, with its tabs and bars; no video loads, for the same reason. Needs about 5 GB of memory: an iPad |
| AnkiDroid | 2.25.3 | 2 | Does not show a screen within a minute; it uses a WebView |
| Element Classic | 1.6.62 | 21 | Stays at its splash screen. Before 1.5 it stopped as it started: see below |
| Jellyfin | 2.7.3 | 2 | Opens only to say that a WebView is needed |
| KDE Connect | 1.37.3 | 1 | Stops as it starts: it asks for the Wi-Fi service, which this Android does not have |
| Telegram | 13.3.1 | 4 | Its window stays black. Before 1.5 it stopped as it started: see below |

Twenty-seven of the thirty-two open; of all fifty-four, forty-eight.

Not tried: TikTok and YouTube. Neither maker offers an APK to download, and YouTube needs Google Play services.

## What changed in 1.5

**Keys.** An app could not make a key in Android's keystore: the keystore asked whether the app was allowed, and
the answer was no for every app, because the table of answers had been taken for the system's own processes
only. Apps now get what Android's policy gives every app. Signal and Element stopped on this as they started,
and so would any app that keeps its settings encrypted.

**Named pipes.** `mkfifo` was refused. Element's database library makes one in the app's own folder.

**Instructions.** Telegram stopped on an instruction the interpreter did not have. The native libraries of all
these apps were searched for more (`tools/scan-interpreter-coverage.py`), and what ordinary code among them
uses was added: about seventy vector and floating-point instructions, the conversion to and from half
precision, and the cache instructions a program uses after writing code of its own. Each vector instruction is
run on a Mac's processor and in the interpreter with the same operands (557 cases).

**Large apps.** Douyin's package is 271 MB and it starts several hundred threads. The limits it met were raised:
the size of a package (to 1 GiB), the threads Android may have (to 4,096), the memory an app's Java code may use
(to 256 MB), and how many requests may wait for one thread of a system service.

**Names of hosts.** A lookup whose answer went through several aliases came back cut short, and the app was told
the host did not exist. Answers are now packed the way the protocol allows.

**Landscape.** An app that asks for landscape is shown the right way up, and turning the device turns Android.

## What changed after 1.3

**Apps that stopped Android.** DuckDuckGo and Feeder each bring a library written in Go, and in 1.3 opening
either one stopped Android: nothing was drawn any more. The interpreter that runs apps' own libraries had met an
instruction it does not have, and that ended the whole run. Two things changed. An instruction the interpreter
does not have now ends only the app that met it, with the signal a processor without the instruction would
raise. And the instructions themselves were added: the ones Go's libraries use, and about forty more vector
instructions that the libraries of the apps in this list contain. Each is run on a Mac's processor and in the
interpreter with the same operands, and the results compared (`tools/validate-vector-instructions.cpp`).
Tried on an iPhone 14 with iOS 17.3.1: Feeder opens and stays open; DuckDuckGo ends by itself, for want of a
WebView, and Android goes on.

## What changed in 1.3

**Libraries laid out for 4 KiB pages.** Android here runs with the iPhone's 16 KiB pages, and its linker refuses a
library laid out for 4 KiB pages. Many apps still ship such libraries. AntennaPod is one: it stopped as it
started with `dlopen failed: … program alignment (4096) cannot be smaller than system page size`. Android has a
compatible way of loading them, which is now on for every app (`bionic.linker.16kb.app_compat.enabled`), and
the kernel layer no longer refuses memory that gains execute permission, which that way of loading needs.

## What stands in the way of more

- **No WebView.** An app that shows a web page inside itself says that Android System WebView is being updated.
- **A limit on how many files Android's data may hold** was reached with some thirty apps installed: no further app
  would start. It was 4,096 files and 2,048 directories and is now sixteen times that.
- **Native libraries are interpreted,** at about forty million instructions a second. An app that does real work
  in its own library is slow.
- **No Google Play services.** Apps that need them were not tried.
- **No Wi-Fi service.** Android here reaches the network through a wired interface. An app that asks for the
  Wi-Fi service and does not expect to be told there is none stops (KDE Connect).
- **Large apps start slowly.** Their Java code is not compiled ahead of time here. Firefox takes up to two
  minutes to its first screen.

Not in the list: VLC, because the APK at hand was built for x86 only and cannot run on a phone.
