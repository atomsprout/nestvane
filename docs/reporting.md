# Reporting a problem

Nestvane is early, and most of what got fixed so far was found by people who said what did not work for them.
This page says what to collect when something goes wrong, how to get Nestvane's diagnostics out of the app, and where to send them.

## Where to report

| Where | Best for |
| --- | --- |
| [GitHub issues](https://github.com/atomsprout/nestvane/issues) | Bugs: something that does not work, crashes or freezes. An issue keeps the report, the diagnostics file and the answers together, and says in which release it was fixed. A GitHub account is needed. |
| [Discord](https://discord.gg/eGzEaP6TzR) | Questions, quick feedback, and talking with other people who run Nestvane. |
| [Telegram](https://t.me/atomsprout) | The same, and following updates. |
| [Email](mailto:atomsprout@gmail.com?subject=Nestvane%20problem) | Anything you would rather not post in public, a diagnostics file included. |

If you are not sure whether something is a bug, ask on Discord or Telegram first. If it turns out to be one, it is moved to an issue so that it is not lost.

## The quickest way: Submit a Bug

In Nestvane, open **More → Submit a Bug…**. It opens a new GitHub issue in the browser with your system already filled in: Nestvane's version and build, the iOS version, the device model, its memory, whether 120 Hz is on, whether Android was running, and the phone's temperature.
Write what happened above that, and attach the diagnostics file (below) by dragging it into the issue, or with *Paste, drop, or click to add files*.

## Exporting diagnostics

**More → Export Diagnostics…**, then **Export**. After a few seconds (at most about 15) the share sheet opens with a text file named like `Nestvane-Diagnostics-20261125-103000.txt`. Save it to Files, or send it straight to Mail, Telegram, Discord or wherever you report.

The file has:

- The time, Nestvane's version, the iOS version, the device model, its memory and temperature, and whether Android was running.
- Memory: how much Nestvane uses and how much iOS still lets it use.
- What every Android thread is waiting for, the Android system server's Java stacks, and the last part of Android's own log.
- Nestvane's own records of the last start: how far it got and how long each stage took, when the home screen appeared, the network, and what a watchdog saw.

**Export it while the problem is there.** The More menu is Nestvane's own and keeps working when Android is frozen. If you close Nestvane first, the next start writes over the records of the run that went wrong. If the runtime itself is stuck, the file says so after about 15 seconds and contains what was written down before.

**Read it before you share it.** Android's log can contain the names of your apps, accounts and files. To share it privately, send it by email instead of in a public issue or chat.

The file is usually a few hundred kilobytes. GitHub accepts it as an attachment as it is.

## What makes a report useful

- **What you did, what you expected, and what you saw instead.** Step by step, if it happens again when you repeat it.
- **The app**, if it is about one: its name, its version, and where the APK came from (F-Droid, the developer's site, APKMirror…). A link to the same APK helps most.
- **Whether it happens every time**, only once, or only after Nestvane has been running a while.
- **A screenshot or a screen recording.** The iOS screen recorder (in Control Center) records Nestvane like any app.
- **The diagnostics file.**
- **If Nestvane itself closed** (back to the iOS home screen), iOS keeps a crash report: Settings → Privacy & Security → Analytics & Improvements → Analytics Data, entries whose names start with `nestvane-native`. Share the newest one (the share button is at the top right) along with the rest. This list is there only if *Share iPhone Analytics* is on.

If an app does not start at all and nothing else is wrong, its name and a link to the APK are often enough: the [compatibility list](compatibility.md) says which apps were tried.

## Before reporting

- Check that you have the [latest release](https://github.com/atomsprout/nestvane/releases): Nestvane tells you in More → Settings when there is a newer one.
- Look at [what does not work yet](status.md#what-does-not-work-yet): Google Play services, hardware video decoding, the home screen turning and KernelSU modules, among others, are known.
- Search the [existing issues](https://github.com/atomsprout/nestvane/issues?q=is%3Aissue). If yours is there, add to it rather than opening a new one: one more phone or one more app where it happens is useful too.
