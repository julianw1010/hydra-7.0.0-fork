#!/usr/bin/env bash
set -euo pipefail

log() { echo ">>> $*"; }

log "Fixing ownership of source tree..."
sudo chown -R "$USER:$(id -gn)" .

PACKAGES=(
    build-essential
    fakeroot
    libncurses-dev
    bison
    flex
    libssl-dev
    libelf-dev
    dwarves
    pahole
    bc
    zstd
    kmod
    cpio
    rsync
    git
)

log "Updating package lists..."
sudo apt-get update -y

log "Installing ${#PACKAGES[@]} kernel build dependencies..."
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${PACKAGES[@]}"

NUMA_NODES=$(ls -d /sys/devices/system/node/node[0-9]* 2>/dev/null | wc -l)
if [[ "$NUMA_NODES" -lt 1 ]]; then
    NUMA_NODES=1
fi
log "Detected $NUMA_NODES NUMA node(s)."

log "Generating baseline defconfig..."
make defconfig

log "Trimming config to currently loaded modules (localmodconfig)..."
# 'yes' keeps writing blank lines until localmodconfig stops reading stdin,
# at which point it gets SIGPIPE and exits 141. That's expected, not a real
# failure — so pipefail is disabled just for this one pipeline, otherwise
# set -e would kill the script here every single run.
set +o pipefail
yes '' | make localmodconfig 2>&1 | tee /tmp/lmc.log
set -o pipefail

# Pull every CONFIG_* token out of "module X did not have configs CONFIG_Y ..."
# lines and strip the CONFIG_ prefix, using awk alone (no sed needed).
mapfile -t MISSING_CONFIGS < <(
    awk '/did not have configs/ {
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^CONFIG_/) {
                sub(/^CONFIG_/, "", $i)
                print $i
            }
        }
    }' /tmp/lmc.log
)

if [[ "${#MISSING_CONFIGS[@]}" -gt 0 ]]; then
    log "localmodconfig missed ${#MISSING_CONFIGS[@]} module config(s); enabling as modules:"
    printf '      %s\n' "${MISSING_CONFIGS[@]}"

    CONFIG_ARGS=()
    for c in "${MISSING_CONFIGS[@]}"; do
        CONFIG_ARGS+=(--module "$c")
    done
    ./scripts/config "${CONFIG_ARGS[@]}"
else
    log "No missing module configs to patch in."
fi

log "Resolving dependencies (olddefconfig, pass 1)..."
make olddefconfig

log "Detecting NUMA node-count config symbol..."
# Different kernel variants call this option different things.
# Only one of MITOSIS_NUMA_NODE_COUNT / HYDRA_NUMA_NODE_COUNT should exist
# in a given tree, so we detect whichever is actually defined in the
# Kconfig sources and set that one.
NUMA_CONFIG_NAME=""
for candidate in MITOSIS_NUMA_NODE_COUNT HYDRA_NUMA_NODE_COUNT; do
    if grep -rqsE "^[[:space:]]*config[[:space:]]+${candidate}([[:space:]]|\$)" --include='Kconfig*' .; then
        NUMA_CONFIG_NAME="$candidate"
        break
    fi
done

if [[ -z "$NUMA_CONFIG_NAME" ]]; then
    echo "ERROR: neither MITOSIS_NUMA_NODE_COUNT nor HYDRA_NUMA_NODE_COUNT" >&2
    echo "       was found in this tree's Kconfig sources. Aborting." >&2
    exit 1
fi
log "  -> found CONFIG_${NUMA_CONFIG_NAME}"

log "Applying custom NUMA / virtualization / mitigation settings..."
log "  CONFIG_${NUMA_CONFIG_NAME} = $NUMA_NODES"
./scripts/config --set-val "$NUMA_CONFIG_NAME" "$NUMA_NODES"
./scripts/config --enable  NUMA
./scripts/config --enable  PARAVIRT
./scripts/config --enable  PARAVIRT_XXL
./scripts/config --enable  TRANSPARENT_HUGEPAGE
./scripts/config --enable  NUMA_BALANCING
./scripts/config --enable  MITIGATION_PAGE_TABLE_ISOLATION
./scripts/config --disable LOCALVERSION_AUTO

log "Resolving dependencies (olddefconfig, pass 2)..."
make olddefconfig

log "Done. Your system is ready to build the Linux kernel."
