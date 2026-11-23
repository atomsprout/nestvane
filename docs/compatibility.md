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
| DuckDuckGo | 5.301.1 | 10 | **Stops Android.** Nothing is drawn any more until Android is restarted |
| Feeder | from F-Droid, build 4191 | 2 | **Stops Android**, a moment after its first screen is drawn |

Twenty of the twenty-two open. The two that stop Android both bring a library written in Go (`libwg-go.so`,
`libgojni.so`); no other app in the list does. That is the only thing known about the cause. Until it is found,
an app with a Go library should be expected to do the same. *Stop Android* in the More menu, then *Start Android*,
brings Android back; the app that caused it stays installed.

## What changed in 1.3

**Libraries laid out for 4 KiB pages.** Android here runs with the iPhone's 16 KiB pages, and its linker refuses a
library laid out for 4 KiB pages. Many apps still ship such libraries. AntennaPod is one: it stopped as it
started with `dlopen failed: … program alignment (4096) cannot be smaller than system page size`. Android has a
compatible way of loading them, which is now on for every app (`bionic.linker.16kb.app_compat.enabled`), and
the kernel layer no longer refuses memory that gains execute permission, which that way of loading needs.

## What stands in the way of more

- **No WebView.** An app that shows a web page inside itself says that Android System WebView is being updated.
- **Native libraries are interpreted,** at about forty million instructions a second. An app that does real work
  in its own library is slow.
- **Go libraries stop Android,** as above.
- **No Google Play services.** Apps that need them were not tried.

Not in the list: VLC, because the APK at hand was built for x86 only and cannot run on a phone.
