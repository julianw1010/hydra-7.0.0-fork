#!/usr/bin/env bash
set -euo pipefail

log() { echo ">>> $*"; }

SCHEDULER="${1:-node3}"
NETNAME="${ICECC_NETNAME:-hydra}"

verify_distributed_setup() {
    local sched="$1" netname="$2"
    local conf=/etc/icecc/icecc.conf bashrc="$HOME/.bashrc"
    local host dargs problems=0
    host=$(hostname -s)
    _ok()  { echo "    [ OK ]  $*"; }
    _bad() { echo "    [FAIL]  $*"; problems=$((problems + 1)); }

    log "Verifying distributed build on $host (scheduler=$sched, netname=$netname)..."

    [[ -x /usr/lib/icecc/bin/gcc ]] \
        && _ok "icecc compiler wrappers present" \
        || _bad "icecc not installed (/usr/lib/icecc/bin/gcc missing)"

    grep -q "ICECC_NETNAME=.*${netname}" "$conf" 2>/dev/null \
        && _ok "icecc.conf netname=${netname}" \
        || _bad "icecc.conf missing ICECC_NETNAME=${netname}"

    grep -q "ICECC_SCHEDULER_HOST=.*${sched}" "$conf" 2>/dev/null \
        && _ok "icecc.conf scheduler_host=${sched}" \
        || _bad "icecc.conf missing ICECC_SCHEDULER_HOST=${sched}"

    systemctl is-enabled iceccd 2>/dev/null | grep -qw enabled \
        && _ok "iceccd enabled at boot" \
        || _bad "iceccd not enabled at boot"

    systemctl is-active iceccd >/dev/null 2>&1 \
        && _ok "iceccd active" \
        || _bad "iceccd not running"

    dargs=$(pgrep -af '/usr/sbin/iceccd' || true)
    { grep -q -- "-n ${netname}" <<<"$dargs" && grep -q -- "-s ${sched}" <<<"$dargs"; } \
        && _ok "iceccd running with -n ${netname} -s ${sched}" \
        || _bad "iceccd not running with -n ${netname} -s ${sched}"

    grep -q '/usr/lib/icecc/bin' "$bashrc" 2>/dev/null \
        && _ok "~/.bashrc puts icecc on PATH" \
        || _bad "~/.bashrc missing icecc PATH"

    grep -qE 'LD=ld[.]lld' "$bashrc" 2>/dev/null \
        && _ok "~/.bashrc has lld build alias" \
        || _bad "~/.bashrc missing lld build alias"

    timeout 5 bash -c ">/dev/tcp/${sched}/8765" 2>/dev/null \
        && _ok "scheduler ${sched}:8765 reachable" \
        || _bad "scheduler ${sched}:8765 NOT reachable"

    if command -v gcc >/dev/null 2>&1; then
        local t; t=$(mktemp --suffix=.c)
        echo 'int main(void){return 0;}' > "$t"
        /usr/lib/icecc/bin/gcc -c "$t" -o "${t}.o" >/tmp/icecc_selftest.log 2>&1 \
            && _ok "end-to-end icecc compile works" \
            || _bad "end-to-end icecc compile FAILED (see /tmp/icecc_selftest.log)"
        rm -f "$t" "${t}.o"
    else
        echo "    [skip]  end-to-end compile (gcc not installed yet)"
    fi

    if (( problems > 0 )); then
        echo ">>> ABORT: distributed build not ready on $host — $problems problem(s) above." >&2
        exit 1
    fi
    log "Distributed build cluster verified OK on $host."
}

verify_distributed_setup "$SCHEDULER" "$NETNAME"

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
