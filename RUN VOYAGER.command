#!/bin/sh
# Voyager OS - macOS launcher (double-clickable)
# Boots Your ISO is here/voyager.iso in QEMU - no build required.
# Auto-finds any QEMU install, auto-installs via Homebrew if missing.
# Fallback: bash run-voyager.sh does the same thing.

cd "$(dirname "$0")" || exit 1

ISO="Your ISO is here/voyager.iso"
VDI="voyager-hd.vdi"

if [ ! -f "$ISO" ]; then
    echo "ERROR: voyager.iso not found in 'Your ISO is here' folder."
    exit 1
fi

# --- find QEMU: PATH plus every common macOS location, any version ---
QEMU=""
for d in /opt/homebrew/bin /opt/homebrew/sbin /usr/local/bin /usr/local/sbin \
         "$HOME/.local/bin" /usr/bin /usr/sbin /bin /sbin /opt/local/bin; do
    if [ -z "$QEMU" ] && [ -x "$d/qemu-system-x86_64" ]; then
        QEMU="$d/qemu-system-x86_64"
    fi
done
if [ -z "$QEMU" ]; then
    QEMU=$(command -v qemu-system-x86_64 2>/dev/null)
fi

# --- not found: auto-install via Homebrew ---
if [ -z "$QEMU" ]; then
    echo "QEMU not found - installing automatically via Homebrew..."
    if command -v brew >/dev/null 2>&1; then
        brew install qemu || { echo "Install failed - get QEMU from https://www.qemu.org/download/"; exit 1; }
    else
        echo "Homebrew not found. Install QEMU from https://www.qemu.org/download/"
        exit 1
    fi
    for d in /opt/homebrew/bin /usr/local/bin /usr/bin /usr/sbin /bin; do
        if [ -z "$QEMU" ] && [ -x "$d/qemu-system-x86_64" ]; then
            QEMU="$d/qemu-system-x86_64"
        fi
    done
    [ -z "$QEMU" ] && QEMU=$(command -v qemu-system-x86_64 2>/dev/null)
fi

if [ -z "$QEMU" ] || [ ! -x "$QEMU" ]; then
    echo "Could not find QEMU after install attempt."
    echo "Get it from https://www.qemu.org/download/"
    exit 1
fi
echo "Using QEMU: $QEMU"

# qemu-img lives in the same folder as qemu-system-x86_64
QIMGF="$(dirname "$QEMU")/qemu-img"
[ -x "$QIMGF" ] || QIMGF=$(command -v qemu-img 2>/dev/null)

# --- create the persistent settings disk if missing ---
if [ ! -f "$VDI" ]; then
    echo "Creating Voyager settings disk voyager-hd.vdi..."
    if [ -n "$QIMGF" ]; then
        "$QIMGF" create -f vdi "$VDI" 16M || echo "qemu-img failed - continuing without settings disk."
    else
        echo "qemu-img not found - continuing without settings disk."
    fi
fi

DISKARGS=""
[ -f "$VDI" ] && DISKARGS="-drive file=$VDI,format=vdi,if=ide,index=0,media=disk"

echo "Booting Voyager OS in QEMU - networking and sound enabled. Close the window to exit."
exec "$QEMU" \
    -machine pc -m 1024 \
    $DISKARGS \
    -cdrom "$ISO" -boot d \
    -display sdl \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -audiodev sdl,id=snd0 -device AC97,audiodev=snd0 \
    -serial file:qemu-serial.log \
    -monitor none -no-reboot
