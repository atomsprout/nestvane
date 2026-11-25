# Using Nestvane

This page is for using Nestvane once it is installed ([installing.md](installing.md) says how to install it).
It goes through installing apps, files, the menu, root and KernelSU Manager, backups, and what to do when Android seems stuck.
What works and what does not, with numbers, is in [status.md](status.md).

## The screen and the bar

Nestvane starts Android when you open it; the first start after installing takes longer than the next ones.
Below Android's screen is Nestvane's bar:

| Key | What it does |
| --- | --- |
| Back | Android's Back. |
| Home | Android's Home. Touch and hold it for **Clear Background Apps**, which closes the apps in the background and frees their memory. |
| Recent Apps | Android's recent apps. |
| More (…) | Nestvane's menu: the commands below. |

The More menu has **Install APK…**, **Send Text Message…**, **Settings**, **Back Up Android…**, **Restore Android…**, **Submit a Bug…**, **Export Diagnostics…** and **Stop Android…** (Start Android when it is stopped).

## Installing apps

Android apps come as APK files. Any of these installs one:

- **More → Install APK…**, then choose the file.
- In another iOS app (Files, Safari's downloads, AirDrop, a chat), share the `.apk` file or use *Open in…* and choose Nestvane. The file is judged by its content, so an APK with another name works too.
- Inside Android, open the APK in Android's own Files app.

Apps that need Google Play services do not run: the system is AOSP, without Google's apps.

## Files between iOS and Android

Android's shared storage is a folder in the iOS Files app: **On My iPhone → Nestvane → Android**.
Its folders are Android's `Download`, `Pictures`, `Movies` and so on.
A file you put there from iOS appears in Android within a few seconds, and a file an Android app saves there can be opened from iOS.
Pictures and videos open in Android with Nestvane's small viewer.

## Settings

More → Settings has Nestvane's own settings: its version, where the shared files are, **120 Hz** on a ProMotion screen (smoother, uses more battery; it applies from the next start of Android), **Allow root** and the apps that asked for it, and **Check for updates**.

## Root

Root is off until you turn it on.

1. More → Settings → **Allow root**.
2. Close Nestvane (swipe it away in the app switcher) and open it again. The switch is read when Nestvane starts.
3. When an Android app runs `su`, iOS asks whether that app may have root. Your answer is kept.

The apps that have asked are listed under Allow root in Settings, by name; turn one off to refuse it from then on.
An app with root can read and change every app's data in Android, so allow only apps you trust.

## KernelSU Manager

Nestvane answers KernelSU's own interface, so the official KernelSU Manager can manage root instead of (and together with) the iOS prompt.
There is nothing to flash and no kernel to patch: the part of KernelSU that lives in the kernel is answered by Nestvane's kernel layer.

1. Turn on root as above.
2. Download the Manager from [KernelSU's releases on GitHub](https://github.com/tiann/KernelSU/releases): the file named like `KernelSU_v3.3.1_32680-release.apk`. Only the Manager signed by KernelSU is recognized; builds from other projects or forks are not.
3. Install it (any of the ways above) and open it. The first time, it sets up its tools for about a minute before its first screen; after that it opens normally.
4. Its home screen says **Working**, with the version. It also says GKI mode is meant for testing; that note is about real phones' kernels and does not apply here.
5. The **Superuser** tab lists your apps. Open one and turn on **Superuser** to give it root, or off to take it away.

The answers are shared: an app you allow in the Manager is allowed in Nestvane's Settings, and an app you allowed in the iOS prompt shows as allowed in the Manager.
An app with root can use KernelSU's way (as KernelSU's own tools do) or `su`; both follow the same answers.

What does not work:

- **Modules.** The Module tab lists none and installing one does nothing useful: nothing runs KernelSU's `ksud` while Android starts, and there are no overlay mounts for a module's files.
- **Installing or patching a kernel or boot image** from the Manager. There is no kernel image to change, and nothing needs one.
- App Profiles beyond on and off (a custom user, groups or SELinux domain): the Manager shows the default profile, and root is always full root.

## Backing up and restoring Android

A backup is one file with Android's apps, their data, its settings and your root answers.
The shared folder is not in it: it is in the Files app already.
An iPhone backup (iCloud or a computer) already includes Android's data. A Nestvane backup is for going back to a moment, before trying something with root for example, or for moving only Android to another device.

**To back up:** More → **Back Up Android…** → Back Up. Android stops while the file is written, then you choose where to keep it (On My iPhone, iCloud Drive, or another app), and Android starts again. The file is named like `Nestvane-Backup-2026-11-25-1030.aar`. Make sure there is about as much free space as Android's data takes; the menu says how much.

**To restore:** More → **Restore Android…** → Choose Backup…, and pick the file. Android stops, its apps and data are replaced by the backup's, and it starts from the backup.
What Android had before is kept until Android reaches its home screen from the backup; if it stops before that, the previous Android is put back.
The apps' root answers come back with the backup at once. Whether root is on comes back too, and like the switch it applies after Nestvane is closed and opened again.

A backup made by one version of Nestvane is meant to be restored by the same or a later version.

## When Android seems stuck

- Wait a little: apps' own native code is interpreted and can be slow, and the first start of a large app takes time.
- **More → Export Diagnostics…** writes one text file with what every Android thread is waiting for, memory and Android's recent log, and opens the share sheet. Attach it to a bug report. It can name your apps and accounts, so read it before you share it.
- **More → Submit a Bug…** opens a new GitHub issue with the versions filled in.
- **More → Stop Android…** and then Start Android starts Android again without losing anything.

## Text messages

Android here has no phone number and no radio. **More → Send Text Message…** takes a sender and a text and hands them to Android as a message it received: Messaging shows it, and an app that waits for a code by SMS receives it. Nothing is sent to anyone.
