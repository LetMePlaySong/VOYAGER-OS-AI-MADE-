# Voyager OS

<p align="center">
<img width="1282" height="802" alt="Voyager OS preview" src="https://github.com/user-attachments/assets/323a9028-15a2-40c9-b699-ebb99d8d32fd" />
</p>

*Voyager OS is a from-scratch, bootable 64-bit operating system written
entirely in C and C++ — no host OS at runtime. It boots via Limine on
BIOS/UEFI and ships a full GUI desktop: file manager, web browser,
media player, editors, networking, and sound. Built completely by AI.
Experimental — it has bugs. Not a daily driver.*

**Voyager OS** is a from-scratch, bootable 64-bit operating system with a
full graphical desktop — built entirely in C and C++ as a freestanding
kernel with no host OS dependencies at runtime.

It boots on both BIOS and UEFI through the Limine bootloader, runs in
QEMU or on real hardware (USB), and ships with a complete desktop
environment: file manager, web browser, media player, editors, games,
persistent settings, networking, and sound.

> **Not a daily driver.** Voyager OS is a hobby/experimental project.
> Do **not** use it as your main operating system — expect crashes,
> data loss, and rough edges. It's for trying out, learning, and fun.

> **VM note:** Use **QEMU** — it's the best and most stable option, and
> every run script uses it automatically. **VirtualBox is unstable**
> with this OS: expect display glitches, input problems, and audio
> issues. Booting the ISO on real hardware via USB works too.

> **Made entirely by AI.** Every line of code in Voyager OS —
kernel, drivers, GUI, apps, web engine — was written by an AI coding
agent (Devin, by Cognition) working interactively with a human who
supplied the idea and feedback. No human wrote the source.

---

## Quick start — no building required

This repository ships a ready-to-boot ISO. Just pick your OS and
double-click (or run) the matching launcher — it boots Voyager OS in
QEMU with **networking + sound enabled** automatically. No compilers,
no dependencies to install beyond QEMU itself.

### Windows

Double-click **`RUN VOYAGER.bat`**

The script finds QEMU (or tells you how to install it), creates the
persistent settings disk (`voyager-hd.vdi`) on first run, and boots
the ISO. Close the QEMU window to exit.

### macOS

Double-click **`RUN VOYAGER.command`**

> If macOS Gatekeeper blocks the double-click, open **Terminal**,
> `cd` into this folder, and run `bash run-voyager.sh` — the `.sh`
> script is the universal fallback on every platform.

The script auto-installs QEMU via Homebrew if needed, creates the
settings disk, and boots the ISO.

### Linux

Open a terminal in this folder and run:

```bash
./run-voyager.sh        # or: bash run-voyager.sh
```

It auto-installs QEMU via apt/dnf/pacman if needed, creates the
settings disk, and boots the ISO.

### Boot on a real computer (USB)

Flash **`Your ISO is here/voyager.iso`** to a USB stick (4 GB+) with
**balenaEtcher** (etcher.balena.io) or **Rufus** (rufus.ie, Windows):
select the ISO, select the drive, flash. Boot the PC from the USB.

---

## What's inside

### Desktop environment

- Boot splash, animated window spin-in / spin-out transitions
- Draggable, resizable windows with traffic-light controls
- Zoom button: boing resize to fullscreen and back, live relayout
- Animated dock with magnification, scroll arrows, window lists,
  auto-hide in fullscreen, drop shadows on all chrome
- Desktop icons with drag-and-drop, marquee selection, context menus
- Menu bar: Voyager menu, network popup, volume slider, calendar,
  live clock
- Full cursor system: arrow, hand, I-beam, busy, resize grips
- 9 free-licensed photographic wallpapers (CC0 / public domain)
- Persistent settings across reboots (wallpaper, username, files)

### Applications

| App | Description |
|-----|-------------|
| **Files** | Finder-style file manager — System HD, user files, detected disks, drag & drop, thumbnails, rename/delete, disk eject/refresh/properties |
| **Voyage Web** | Web browser — dynamic tabs, back/forward/reload, URL bar with Frogfind search, HTML/CSS/JS rendering engine, images, animated GIFs, downloads |
| **Voyage Player** | Music player — MP3/WAV/OGG, playlist, scrolling song title, elapsed/remaining LCD, keeps playing while minimized |
| **Quill** | Plain-text editor with Save As picker |
| **Memo** | Sticky notes |
| **Photos** | Image viewer for PNG/JPEG/GIF |
| **Calc** | Calculator |
| **Console** | Command-line shell (disabled — doesn't work yet) |
| **Settings** | Wallpaper picker, Date & Time (timezone, auto-DST, SNTP sync), Task Manager with End Task |

### System

- **Kernel**: freestanding x86-64, Limine boot protocol, GDT/IDT, heap,
  framebuffer abstraction, PS/2 keyboard + mouse
- **Storage**: custom Voyager volume format (VFS) user disk, System HD
  catalog from ISO modules, ATA driver with hot-plug media polling
- **Networking**: e1000 driver, ARP, ICMP, UDP, DNS, TCP, HTTP client,
  SNTP time sync
- **Audio**: AC97 driver with PCM ring buffer, MP3 via minimp3,
  WAV/OGG support, software volume
- **Web engine**: custom HTML parser + layout + JS interpreter subset,
  CSS-ish styling, forms, links, images, scrolling
- **Reliability**: OOM watchdog, disk-space guard on saves/downloads,
  styled modal dialogs

---

## Repository layout

```
Your ISO is here/
    voyager.iso         Ready-to-boot Voyager OS image (54 MB)
RUN VOYAGER.bat        Windows launcher — boots the ISO in QEMU
RUN VOYAGER.command    macOS launcher (double-click)
run-voyager.sh         Linux/macOS launcher — boots the ISO in QEMU
LICENSE                Project license
```

All three launchers do the same thing: locate/install QEMU, create the
16 MB `voyager-hd.vdi` settings disk on first run, then boot
`voyager.iso` with e1000 networking, AC97 sound, and a serial log
(`qemu-serial.log`).

---

## Credits

- **Bootloader**: Limine (BSD-2-Clause)
- **MP3 decoding**: minimp3 (CC0 / public domain)
- **JPEG decoding**: picojpeg (public domain)
- **Wallpapers**: free-licensed photography via Openverse
- **Test music**: "Straight Fuse" by French Fuse

Voyager OS is an independent project — not affiliated with any
commercial OS vendor.
