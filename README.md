# Voyager OS

*Voyager OS is a from-scratch, bootable 64-bit operating system written
entirely in C and C++ — no host OS at runtime. It boots via Limine on
BIOS/UEFI and ships a full GUI desktop: file manager, web browser,
media player, editors, networking, and sound. Built completely by AI.
QEMU recommended; VirtualBox is unstable. Experimental — it has bugs.*

**Voyager OS** is a from-scratch, bootable 64-bit operating system with a
full graphical desktop — built entirely in C and C++ as a freestanding
kernel with no host OS dependencies at runtime.

It boots on both BIOS and UEFI through the Limine bootloader, runs on
real hardware (USB) or QEMU, and ships with a complete desktop
environment: file manager, web browser, media player, editors, games,
persistent settings, networking, and sound.

> **VM note:** Voyager OS is developed and tested on **QEMU** — that's
> the best and most stable option (`RUN VOYAGER.bat` uses it
> automatically). **VirtualBox is unstable** with this OS: expect
> display glitches, input problems, and audio issues. Real hardware via
> the USB ISO works well.

> **Made entirely by AI.** Every line of code in this repository —
> kernel, drivers, GUI, apps, web engine — was written by an AI coding
> agent (Devin, by Cognition) working interactively with a human who
> supplied the idea and feedback. No human wrote the source.
>
> **This is a hobby/experimental project and it has bugs.** Expect
> crashes, rough edges, and incomplete features. Known issues include:
> occasional audio crackle on some hardware, web pages rendering
> approximately rather than pixel-perfectly, and edge-case crashes
> under heavy memory pressure. Issue reports and pull requests are
> welcome but please be patient — fixes depend on available compute
> time.

---

## Quick start

### Try it instantly — pick your OS

**Windows** — double-click **`RUN VOYAGER.bat`**

The script does everything:

1. Checks for WSL — installs it if missing
2. Checks for QEMU — installs it if missing
3. Checks the compiler toolchain inside WSL — installs it if missing
4. Builds the ISO (`build/voyager.iso`)
5. Boots Voyager OS in QEMU with **networking + sound enabled**

Already installed? It skips straight to building and booting.
Close the QEMU window to exit.

**macOS** — double-click **`RUN VOYAGER.command`**

> If double-clicking does nothing (macOS Gatekeeper may block it),
> open **Terminal**, `cd` into this folder, and run:
> `bash run-voyager.sh` — the `.sh` scripts are the fallback for
> every platform and do exactly the same thing.

The script auto-installs QEMU, nasm, xorriso, and llvm via Homebrew,
detects an ELF-capable toolchain (osxcross, or clang+ld.lld), builds
the ISO, creates the settings disk, and boots QEMU.

**Linux** — open a terminal in this folder and run:

```bash
./run-voyager.sh        # or: bash run-voyager.sh
```

It auto-installs missing packages via apt, builds, and boots QEMU.

### Make a bootable USB (real computer)

Same idea, build-only version:

- **Windows** — double-click **`MAKE ISO.bat`**
- **macOS** — double-click **`MAKE ISO.command`** (fallback:
  `bash make-iso.sh` in Terminal)
- **Linux** — `./make-iso.sh` (or `bash make-iso.sh`)

All three produce **`Your ISO is here/voyager.iso`** and print the
next steps.

Then flash the ISO to a USB stick (4 GB+) with **balenaEtcher**
(etcher.balena.io) or **Rufus** (rufus.ie, Windows): select the ISO,
select the drive, flash. Boot the PC from the USB.

### Manual build

```bash
make -j4          # produces build/voyager.iso
```

---

## What's inside

### Desktop environment

- Boot splash, animated window spin-in / spin-out transitions
- Draggable, resizable windows with traffic-light controls
- Zoom button: boing resize to fullscreen and back, live relayout
- Animated dock with magnification, scroll arrows, window lists,
  auto-hide in fullscreen
- Desktop icons with drag-and-drop, marquee selection, context menus
- Menu bar: Voyager menu, network popup, volume slider, calendar,
  live clock
- Full cursor system: arrow, hand, I-beam, busy, resize grips —
  leases release instantly when leaving their owner
- Custom 8x8 bitmap font, 9 free-licensed photographic wallpapers
  (CC0 / public domain — see `assets/WALLPAPER-CREDITS.txt`)
- Persistent settings across reboots (wallpaper, username, icons)

### Applications

| App | Description |
|-----|-------------|
| **Files** | Finder-style file manager — System HD, user files, detected disks, drag & drop, thumbnails, rename/delete, disk eject/refresh/properties, unified animated sidebar |
| **Voyage Web** | Web browser — dynamic tabs, back/forward/reload, URL bar with Frogfind search (`frogfind.com/?q=...`), HTML/CSS/JS rendering engine, images, animated GIFs, downloads |
| **Voyage Player** | Music player — MP3/WAV/OGG, playlist, scrolling song title, keeps playing while minimized or in background |
| **Quill** | Plain-text editor with Save As picker |
| **Memo** | Sticky notes |
| **Photos** | Image viewer for PNG/JPEG/GIF |
| **Calc** | Calculator |
| **Console** | Command-line shell |
| **Settings** | Wallpaper picker, Date & Time (timezone, auto-DST, SNTP sync, manual set), Task Manager with End Task |

### System

- **Kernel**: freestanding x86-64, Limine protocol, GDT/IDT, heap,
  framebuffer abstraction, PS/2 keyboard + mouse
- **Storage**: custom Voyager volume format (VFS) user disk, System HD
  catalog from ISO modules, ATA driver with hot-plug media polling
  (USB, optical, VM drives)
- **Networking**: e1000 driver, ARP, ICMP, UDP, DNS, TCP, HTTP client,
  SNTP time sync (like Windows, retries on failure)
- **Audio**: AC97 driver with PCM ring buffer, MP3 via minimp3,
  WAV/OGG support, software volume
- **Web engine**: custom HTML parser + layout + JS interpreter subset,
  CSS-ish styling, forms, links, images, scrolling
- **Reliability**: OOM watchdog (warns then auto-closes apps),
  disk-space guard on saves/downloads, styled modal dialogs

---

## Repository layout

```
Application/   Voyager apps (VoyageWeb, VoyagePlayer, Quill, ...)
assets/        Icons, cursors, wallpapers (+ WALLPAPER-CREDITS.txt)
boot/          Limine config
drivers/       AC97, ATA, e1000, PS/2
kernel/        Core kernel, framebuffer, VFS, sysvol, PNG/JPEG/GIF
gui/           Window manager, widgets, app registration
lib/           libc subset, font, embedded assets, minimp3
net/           TCP/IP stack, HTTP, HTML engine, JS engine
tools/         sysvol.sh, png2h, limine binaries
RUN VOYAGER.bat One-click build + QEMU boot (try it out)
MAKE ISO.bat   Build voyager.iso for USB/real hardware
Makefile       Build system (WSL/Linux)
```

---

## Credits

- **Bootloader**: Limine (BSD-2-Clause)
- **MP3 decoding**: minimp3 (CC0 / public domain)
- **JPEG decoding**: picojpeg (public domain)
- **Wallpapers**: free-licensed photography via Openverse — see
  `assets/WALLPAPER-CREDITS.txt`
- **Test music**: "Straight Fuse" by French Fuse

Voyager OS is an independent project — not affiliated with any
commercial OS vendor.
