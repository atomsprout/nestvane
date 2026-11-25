# Devices and host integration

The guest is an Android system built for the Android emulator's virtual hardware (the product is `sdk_phone16k_arm64`, whose virtual board is called "ranchu").
That choice means Android already contains drivers for hardware that is easy to imitate: a pipe to a renderer on the host, a simple network interface, a touch screen that reports absolute positions.
This document describes how each of those is connected to the phone.

## Graphics

**The path of a frame.**

1. An Android app draws with OpenGL ES or through Android's own renderer, as on any device.
2. The emulator's graphics libraries in the guest do not draw. They encode each graphics call and write it to a pipe device, `/dev/goldfish_pipe_dprctd`; buffers are shared through `/dev/goldfish_address_space`.
3. The kernel layer hands the pipe's bytes to **gfxstream**, the renderer the Android emulator uses on a desktop, here built for iOS and loaded into the app as `NestvaneGfxstream.framework` ([`ios/GfxstreamSession.hpp`](../ios/GfxstreamSession.hpp), [`runtime/GfxstreamPipe.hpp`](../runtime/GfxstreamPipe.hpp)).
4. gfxstream decodes the calls and issues them to **ANGLE**, a library that implements OpenGL ES on top of other graphics interfaces. It is built with only its **Metal** backend ([`tools/angle-iphoneos.gn`](../tools/angle-iphoneos.gn)).
5. SurfaceFlinger, Android's compositor, composes the windows into the display's frame, again through the same pipe.
6. The app reads the composed frame back from gfxstream as RGBA pixels (`gfxstream_guest_display_read`), wraps them in an image and shows it in a view ([`ios/App.mm`](../ios/App.mm), `StartDisplayReader`).

gfxstream is used with a set of patches kept in [`tools/patches/`](../tools/patches): a window implementation for iOS, the pipe interface the kernel layer talks to, reading back the primary display, and several fixes for running without a window of its own.
The gfxstream and ANGLE sources themselves are not in this repository; [building.md](building.md) says what is needed.

**Frame rate.**
Two settings have to agree: how often the guest composes, and how often the app looks for a new frame.
Both come from `NESTVANE_DISPLAY_HZ`, which the app sets to 60, or to 120 on a screen that shows 120 when "120 Hz" is turned on in the app's Settings; a screen with a lower maximum gets that. The guest receives it as its vsync rate through the boot configuration.
Earlier the app looked for a frame every 33 ms, which capped the display at 30 frames a second.

Measured while scrolling a list in Settings: up to about 55 frames a second at 60 Hz on an iPhone 15 Pro.
At 120 Hz, with a finger kept moving on the same list, Android composes 120 frames a second and the app shows 86 to 120 of them (iPhone 15 Pro).

Two things had kept 120 Hz from working, and neither was speed.
The framework holds drawing to 60 frames a second by default whatever the display mode is; the overlay in [`fixtures/overlay-framework`](../fixtures/overlay-framework) lifts that default to 120.
And overlays of the framework's own resources were never applied: zygote runs `/system/bin/idmap2` for them before anything else, the guest root did not have it, and zygote logged `'idmap2 create-multiple' failed` at every boot.
The earlier figures of about 50 frames a second "at 120 Hz" were Android drawing at 60.

**Shader compilation.**
SurfaceFlinger normally compiles a set of shaders at boot. Here that took 3.9 seconds, so it is switched off (`service.sf.prime_shader_cache=0`) and shaders are compiled when first used.

## Storage

Android writes all of its changeable state under `/data`. Three mechanisms hold it.

**The snapshot.**
[`runtime/ProbeData.cpp`](../runtime/ProbeData.cpp) keeps `/data` in a SQLite database, `guest-data.sqlite3`, in the app's Application Support directory (under `Nestvane/`).
While Android runs, the files live in memory in the kernel layer.
When the guest calls `fsync` or `sync`, the layer writes the changed entries to the database in one transaction; commits are at least two seconds apart, because Android syncs often and each commit has a cost.
The database is opened with full synchronous writes, so a committed snapshot survives the app being killed.
What is stored per entry: path, kind, content, owner, mode, timestamps, extended attributes and access control lists.

**Large files.**
Keeping every file in memory and in the database stops working once apps are installed: with fourteen apps the boot time rose from 6.5 to 10.8 seconds and memory from 2.05 to 2.78 GB.
So the content of a regular file of 1 MiB or more is kept in a host file of its own, `guest-data.sqlite3.files/<inode>`, and mapped into memory only when something reads it (`SharedBuffer` in [`runtime/Runtime.hpp`](../runtime/Runtime.hpp)).
The database keeps one small record for it. A file moves out of the database at the moment it grows past 1 MiB.

**Crash semantics.**
These follow from the two mechanisms having different timing.

- The database is a snapshot from the guest's last sync. Names, sizes and the existence of files are as of that snapshot.
- The content of a large file is whatever was last written to it. After the app is killed, a large file can be newer than the snapshot, never older.
- The host file of a deleted large file is removed only after a sync in which no record names it any more. Host files that no record names are removed at mount.
- Host files are not flushed with `msync` or `F_FULLFSYNC`. After a power loss, the content of a large file may be older than the snapshot. This is a known gap.

Limits: 256 MiB per file and 2 GiB in total.

**The shared folder.**
Android's shared storage, which apps see as `/storage/emulated/0`, is the guest directory `/data/media/0`.
The kernel layer maps that directory onto a real folder, `Documents/Android` in the app's container.
Every file and directory there is one host file or directory of the same name and exactly the same length; creating, deleting and renaming in the guest act on the host folder directly. The database keeps only the owner, mode and label of each.
The app enables file sharing, so the iOS Files app shows the folder under On My iPhone, Nestvane, Android.

At mount the host folder is the truth: files put there from outside appear in Android, and files removed from outside are gone.
A change made from iOS while Android is running is seen only after Android is started again.

**The simplified FUSE.**
On a real device, apps reach shared storage through a FUSE file system served by the media provider, which checks each app's permission for each file.
Here the `fuse` mount is a plain view of `/data/media` and the provider's daemon is given only the request that opens the session ([kernel-layer.md](kernel-layer.md)).
The effect for the user is that shared storage works; the effect for security is that per-app access control of shared storage is not enforced.

## Network

**What the guest sees.**
One Ethernet interface, `eth0`, configured the way the Android emulator's user-mode network is: the guest is `10.0.2.15/24`, its gateway is `10.0.2.2`, and its name server is `10.0.2.3`.
The kernel layer answers the netlink queries, interface `ioctl`s and `/sys/class/net/eth0` files that make the interface real to Android ([`runtime/GuestRoute.hpp`](../runtime/GuestRoute.hpp)).
On the Android side the guest root declares the Ethernet feature so that the Ethernet service starts, and a resource overlay ([`fixtures/overlay-connectivity`](../fixtures/overlay-connectivity)) gives `eth0` its fixed configuration.
Android's connectivity service then brings the network up and validates it like any other.
The overlay's last field makes the network a Wi-Fi one for apps (`TRANSPORT_WIFI`): many apps count only Wi-Fi and cellular as being online. Android's Wi-Fi service is not started: it cost the start 0.3 s. The old `NetworkInfo.getType()` still says Ethernet, which the Ethernet service sets.

**Host sockets.**
No packets are forwarded. Instead, each guest TCP or UDP socket whose destination is not the guest itself is paired with a host socket ([`runtime/HostSocket.hpp`](../runtime/HostSocket.hpp)).
`connect`, `send`, `recv`, readiness for `poll` and `epoll`, `SO_ERROR` and `shutdown` are carried over to it, and host error numbers are converted to Linux ones.
Host sockets are non-blocking; a guest thread that waits on one is checked again after a short delay, since a host socket cannot wake the guest scheduler by itself.
IPv6 destinations are carried too where the phone's network has IPv6. The resolver asks whether there is an IPv6 route by connecting a UDP socket to port 0; for IPv6 that question goes to the host. An IPv6 socket's own address is `fd00:10:0:2::15`, a unique local address, so where a name has both kinds of address Android tries IPv4 first.

When the phone moves from Wi-Fi to cellular or back, the app ends every TCP connection the guest has open.
A connection made over the old network does not move to the new one and gives no error either: left alone it stays silent, and an app that keeps connections for reuse waits out a timeout on each request.
A phone's Android ends such connections itself when a network goes away; the guest sees one made-up network that never does, so the host does it, and the guest reads the end of the stream and connects again.

On a network that has no IPv4 of its own, as some cellular networks are, the guest's IPv4 destinations are rewritten to the IPv6 addresses the network's gateway answers at (NAT64), when the system reports such a gateway.
That part has not been tried on such a network.

**DNS.**
The address `10.0.2.3` is a name server inside the app (`runtime/DnsProxy.hpp`). It listens on the host's loopback and answers each query from the system's resolver (`DNSServiceQueryRecord`), so names resolve as they do for every other app on the phone, VPN and DNS settings included. Only queries over UDP are answered, and names under `.local` are refused.
The app never sends to the network's own name server. That server is usually the router, and iOS asks the user for local network access the moment an app sends to it, which used to happen on the first boot. Multicast and broadcast packets from the guest, and packets to the guest's own made-up subnet, are not carried out either. An Android app that connects to a device on the local network still brings the question up, at that moment.

**The iOS network permission.**
On phones where each app must be allowed onto the network, iOS shows its prompt only when the app uses the system's own networking interface; plain sockets do not trigger it and are simply refused.
So at start the app makes one `HEAD` request through `NSURLSession` to Apple's connectivity check address and writes the outcome to `network-check.txt`. This causes the prompt to appear.
The app's `Info.plist` also carries the local network usage description.

Verified: Wikipedia loads its front page. Not tested: large downloads and long-lived connections.

## Sound

The kernel layer provides a sound card of the kind Android's audio service plays through: a control device, `/dev/snd/controlC0`, with no controls, and one playback device, `/dev/snd/pcmC0D0p`. The audio service will not use a playback device whose card has no control device to open.
The playback device answers the stream's parameters with what it settled on, takes samples, can be polled for room, and reports the play position.
Its buffer is as long as it tells the guest, four periods, and it takes as much of a write as there is room for. This matters: the audio service fills the card by its own timing and fills in silence whenever the card has room and it has nothing to write. A card that swallowed more than it said, or that made a long write wait, had silence filled in five times a second and real sound thrown away to make up for it.

What the guest writes goes into a ring buffer ([`runtime/GuestAudio.hpp`](../runtime/GuestAudio.hpp)).
On the host side the app runs an audio output unit in the guest's sample format. It starts taking samples once about 43 ms of them are there, and keeps that much in hand so that a late write from the guest is not heard.
Three seconds after Android last played anything the app gives the phone's audio output back, so that other apps' sound can go on; it takes it again when Android plays.
If nothing takes the samples out, the card plays at the pace of the clock, into nothing.

Android's media volume is at its highest step in the image, so that the phone's own volume buttons decide how loud it is.

**Current state.** Measured on an iPhone 15 Pro with a 1280×720 H.264 video with AAC sound: Android plays 48,000 frames a second into the card, the phone's output takes them at the same rate, and the loudest sample is what it is in the file. After the first seconds of a stream the audio service fills in no more silence.
Measured on the samples; the sound has not been judged by ear.
There is no sound input.

### Video

Video is decoded by Android's software codecs (`media.swcodec`), into buffers of the graphics allocator, which are YUV buffers on the host's GPU.
Two things in the host's renderer had to be there for that: the guest reads such a buffer back before a decoder writes into it and stops the process when the host cannot (the `YuvCache` feature), and the guest's writes into YUV buffers have to reach the host through the same path as its other buffers ([`tools/patches/gfxstream-yuv-buffers.patch`](../tools/patches/gfxstream-yuv-buffers.patch)).

## Battery, time zone and touch

**Battery.**
The app watches the phone's battery level and charging state through UIKit and writes them into [`runtime/GuestBattery.hpp`](../runtime/GuestBattery.hpp).
The kernel layer presents them as `/sys/class/power_supply/battery` and `ac`, which Android's health service reads about once a minute, so the figure Android shows can lag by that much.

**Time.**
The guest keeps its own wall clock, started from the host's current time.
The app passes the phone's time zone name in the boot configuration, and `init` sets `persist.sys.timezone` from it.

**Touch.**
[`runtime/GuestTouch.hpp`](../runtime/GuestTouch.hpp) is a queue of Linux input events.
The app's view converts touches to absolute positions on a 0 to 32767 scale and queues them; the kernel layer delivers them through `/dev/input/event0`, which Android's input system reads like any touch screen.
The Back and Home buttons in the app's bar are sent as key events through the same device.

**Text messages.**
The guest has no phone process and no radio service, so no message can arrive the way it does on a phone.
The app's *Send Text Message…* queues a sender and a text ([`runtime/GuestApkInbox.hpp`](../runtime/GuestApkInbox.hpp)); a guest service started after boot ([`fixtures/android17-text.sh`](../fixtures/android17-text.sh)) reads them from `/dev/nestvane-text`, which waits while there is none, writes a row into the SMS provider with Android's `content` tool, and posts a notification.
The image carries the telephony provider and the Messaging app for this; the provider runs in a process named `com.android.phone`, which is not the phone app.
A second service, running as root because only the system may send it ([`fixtures/android17-sms.sh`](../fixtures/android17-sms.sh)), sends the `SMS_RECEIVED` broadcast for the same message, so apps that wait for an incoming SMS receive it.
The message travels in it as the PDUs a network would have delivered, built by [`tools/sms-broadcast/SmsBroadcast.java`](../tools/sms-broadcast/SmsBroadcast.java): UCS-2, in parts of 67 characters when it is longer than 70.
A sender written as a number arrives as its digits alone, without spaces or hyphens.

**Installing apps.**
An `.apk` file opened with the app (from Files, AirDrop or a share sheet), or picked with Install APK… in the app's menu or on its home-screen icon, is copied into an inbox ([`runtime/GuestApkInbox.hpp`](../runtime/GuestApkInbox.hpp)).
Once Android has booted, a small guest program ([`fixtures/android17-apk-install.c`](../fixtures/android17-apk-install.c)) hands it to Android's own package manager with the stock `cmd package install` command.
The inbox accepts files up to 256 MiB.

**Device identity.**
The model, brand and manufacturer properties are set to Nestvane, and the build number shown in Settings is the build ID.

## Hardware the guest does not have

[`tools/prepare-boot-image.py`](../tools/prepare-boot-image.py) removes the declarations of hardware that is absent, so that Android does not wait for services that never start: the Wi-Fi and fingerprint hardware services are taken out, and the Bluetooth, location, Wi-Fi, ultra-wideband and fingerprint features are marked unavailable.
The motion sensor service exists with an empty list of sensors.
Code for sharing the phone's rear camera with Android exists ([`runtime/GuestCamera.hpp`](../runtime/GuestCamera.hpp), [`ios/IOSCamera.mm`](../ios/IOSCamera.mm)); its state in the current build is not verified in the record these documents draw on.
