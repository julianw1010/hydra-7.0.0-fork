#!/usr/bin/env bash
set -euo pipefail

log() { echo ">>> $*"; }

log "Pulling latest source..."
git pull

export LOCALVERSION=

log "Fixing ownership of source tree..."
sudo chown -R "$USER:$(id -gn)" .

KVER=$(make -s kernelrelease)
log "Target kernel version: $KVER"

BOOT_FILES=(
    "/boot/vmlinuz-$KVER"
    "/boot/initrd.img-$KVER"
    "/boot/initramfs-$KVER.img"
    "/boot/System.map-$KVER"
    "/boot/config-$KVER"
)

log "Removing old /boot files for $KVER..."
for f in "${BOOT_FILES[@]}"; do
    if [[ -e "$f" ]]; then
        log "  removing $f"
    else
        log "  skipping $f (not present)"
    fi
done
sudo rm -f "${BOOT_FILES[@]}"

JOBS=$(( $(nproc) * 4 ))
log "Building kernel with $JOBS job(s)..."
make LD=ld.lld -j"$JOBS"

log "Installing modules..."
sudo make modules_install

log "Installing kernel image..."
sudo make install

log "Built and installed kernel $KVER"
