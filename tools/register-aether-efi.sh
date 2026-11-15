#!/bin/bash
# register-aether-efi.sh — register all aether rescue images under /efi/aether/ as EFI entries in NVRAM
#
# Usage:
#   sudo ./tools/register-aether-efi.sh             # register every /efi/aether/*.img as an EFISTUB boot entry
#   sudo ./tools/register-aether-efi.sh --dry-run   # print the commands that would be run, touch nothing
#   ./tools/register-aether-efi.sh --list           # list registered aether rescue boot entries
#   sudo ./tools/register-aether-efi.sh --uninstall # delete all aether rescue entries (safe: matches by label prefix)
#   sudo ./tools/register-aether-efi.sh -d /dev/nvme0n1 -p 1   # manually specify ESP disk and partition number
#   sudo ./tools/register-aether-efi.sh -u '<kernel cmdline>'  # override kernel cmdline (advanced; initrd= is still replaced per image)
#   sudo ./tools/register-aether-efi.sh --timeout 5            # after registering, set NVRAM menu timeout to 5s (avoids menu flashing by)
#   sudo ./tools/register-aether-efi.sh --add-rescue-entry   # register ONE always-rescue entry named `aether-rescue`
#                           (or directly: sudo efibootmgr -t 5)
#
# Terminology (EFI device path format, same as UEFI, NOT Linux path format):
#   The root of an EFI device path is the ESP root (\), not the Linux mount-point name.
#   e.g. ESP mounted at /efi, image at /efi/aether/x.img -> EFI path \aether\x.img (NOT \EFI\aether\x.img).
#   loader  = \EFI\arch\vmlinuz-linux            (backslashes, no leading /)
#   initrd  = \aether\aether-rescue.img          (backslashes, computed from the ESP mount point)
#
# Behavior:
#   - each *.img gets one BootXXXX entry, loader fixed to \EFI\arch\vmlinuz-linux,
#     cmdline is taken verbatim from /proc/cmdline (root= rootflags= rw/ro loglevel= nouveau.* all kept,
#     only initrd= is stripped and replaced with the per-image initrd=\aether\<file>).
#   - Idempotent: existing entries with matching labels (prefix aether-rescue*) are deleted then recreated.
#   - Never touches entries not registered by this script (Arch/Windows/GRUB etc.).
#   - NVRAM and BootOrder are left untouched (pick the rescue entry from the F9/F10/F12 menu when needed);
#     to change order, use efibootmgr -o (out of scope for this script).
#   - efibootmgr needs root (NVRAM write protection) -- non-root auto-reexecs with sudo.
set -euo pipefail

IMG_DIR=/efi/aether
ESP_MOUNT=${ESP_MOUNT:-/efi}
LABEL_PREFIX=aether-rescue
ESP_DISK=""          # e.g. /dev/nvme0n1 (whole disk)
ESP_PART=""          # e.g. 1
CMDLINE_OVERRIDE=""  # e.g. 'root=UUID=... rw' (initrd= is replaced per image)
ACTION=install
DRY_RUN=0
TIMEOUT=""      # if set, set NVRAM Timeout (seconds) after registering

usage() { sed -n '2,22p' "$0"; exit 0; }
log()  { printf '%s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
run()  { if [[ $DRY_RUN == 1 ]]; then printf '+ %s\n' "$*"; else "$@"; fi; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)   DRY_RUN=1 ;;
        --list)      ACTION=list ;;
        --uninstall) ACTION=uninstall ;;
        -d|--disk)   ESP_DISK="$2"; shift ;;
        -p|--part)   ESP_PART="$2"; shift ;;
        -u|--cmdline) CMDLINE_OVERRIDE="$2"; shift ;;
        --timeout)   TIMEOUT="$2"; shift ;;
        --add-rescue-entry) ACTION=add-rescue-entry ;;
        -h|--help)   usage ;;
        *) die "unknown argument: $1 (see --help)" ;;
    esac
    shift
done

# ── privilege check: NVRAM needs root (list / dry-run are exempt) ──
# dry-run only prints commands and touches nothing; normal users may run it
if [[ $ACTION != list && $DRY_RUN != 1 && $EUID != 0 ]]; then
    command -v sudo >/dev/null 2>&1 || die "root required (efibootmgr writes NVRAM); run with sudo"
    exec sudo "$0" "$@"
fi

command -v efibootmgr >/dev/null 2>&1 || die "efibootmgr not found (pacman -S efibootmgr)"

# ── UEFI environment check ──────────────────────────────────────────
[[ -d /sys/firmware/efi ]] || die "not a UEFI boot environment (/sys/firmware/efi missing)"

# ── list registered entries ───────────────────────────────────────────
list_aether_entries() {
    efibootmgr | awk -v pfx="$LABEL_PREFIX" '
        $1 ~ /^Boot[0-9A-Fa-f]+\*/ || $1 ~ /^Boot[0-9A-Fa-f]+$/ {
            num=$1; sub(/^Boot/, "", num); sub(/\*$/, "", num)
            label=$2                        # the label is a single token
            if (index(label, pfx) == 1) printf "%s %s\n", num, label
        }'
}

if [[ $ACTION == list ]]; then
    list_aether_entries
    exit 0
fi
# ── delete all aether rescue entries (by label prefix; never touches others) ──
if [[ $ACTION == uninstall ]]; then
    if ! entries="$(list_aether_entries)" || [[ -z "$entries" ]]; then
        log "no registered $LABEL_PREFIX* entries found, nothing to clean up"
        exit 0
    fi
    while read -r num label; do
        log "deleting Boot$num  ($label)"
        run efibootmgr -b "$num" -B
    done <<< "$entries"
    log "done: all aether rescue entries deleted"
    exit 0
fi

# ── auto-detect ESP (disk + partition number) ──────────────────────────
if [[ -z "$ESP_DISK" || -z "$ESP_PART" ]]; then
    esp_src="$(findmnt -n -o SOURCE --target "$ESP_MOUNT" 2>/dev/null || true)"
    if [[ -z "$esp_src" ]]; then
        for cand in /efi /boot/efi /boot; do
            src="$(findmnt -n -o SOURCE --target "$cand" 2>/dev/null || true)"
            if [[ -n "$src" && -d "$cand/EFI" ]]; then ESP_MOUNT="$cand"; esp_src="$src"; break; fi
        done
    fi
    [[ -n "$esp_src" ]] || die "ESP mount point not found (tried /efi, /boot/efi, /boot; or pass -d/-p)"
    [[ -d "$ESP_MOUNT/EFI" ]] || die "no EFI/ dir under $ESP_MOUNT, is this really the ESP?"
    disk="$(lsblk -no PKNAME "$esp_src" 2>/dev/null || true)"
    [[ -n "$disk" ]] || disk="$(lsblk -no pkname "$esp_src" 2>/dev/null || true)"
    part="$(lsblk -no PARTN "$esp_src" 2>/dev/null || true)"
    [[ -z "$ESP_PART" ]] && ESP_PART="$part"
    [[ -z "$ESP_DISK" ]] && ESP_DISK="/dev/$disk"
    [[ -n "$ESP_DISK" && -n "$ESP_PART" ]] || die "cannot infer ESP disk/partition from $esp_src; pass -d/-p manually"
fi
log "ESP = $ESP_MOUNT ($ESP_DISK partition $ESP_PART)"

# ── check rescue images ────────────────────────────────────────────────
shopt -s nullglob
IMAGES=("$IMG_DIR"/*.img)
(( ${#IMAGES[@]} > 0 )) || die "no .img files under $IMG_DIR"
[[ -f "$ESP_MOUNT/EFI/arch/vmlinuz-linux" ]] || die "ESP is missing \EFI\arch\vmlinuz-linux (loader; check a kernel exists in the ESP)"
log "found ${#IMAGES[@]} rescue images:"
printf '  %s\n' "${IMAGES[@]##*/}"

if [[ $ACTION == add-rescue-entry ]]; then
    [[ -r /proc/cmdline ]] || die "cannot read /proc/cmdline"
    cl="$(cat /proc/cmdline)"
    # strip initrd= and append aether_rescue (always-rescue); keep everything else verbatim
    out=(); for tok in $cl; do case "$tok" in initrd=*|aether_rescue|aether_rescue=*) ;; *) out+=("$tok");; esac; done
    rescue_cmdline="${out[*]} aether_rescue"
    img="${IMAGES[0]:-}"
    if [[ -z "$img" ]]; then
        # fallback: smallest image is the most convenient for a rescue entry
        img="$(ls -S "$IMG_DIR"/*.img 2>/dev/null | tail -1)"
    fi
    [[ -n "$img" && -f "$img" ]] || die "no rescue image found in $IMG_DIR"
    efi_initrd="\\aether\\${img##*/}"
    cmdline="${rescue_cmdline} initrd=${efi_initrd}"
    log "always-rescue entry: label=aether-rescue, initrd=${efi_initrd}"
    log "  cmdline   ${cmdline}"
    if [[ $DRY_RUN == 1 ]]; then
        printf '+ efibootmgr -c -d %s -p %s -L %q -l %q -u %q\n' "$ESP_DISK" "$ESP_PART" 'aether-rescue' '\\EFI\\arch\\vmlinuz-linux' "$cmdline"
    else
        # idempotent: delete same-named entry, then re-create
        while read -r num label; do
            [[ "$label" == aether-rescue ]] || continue
            log "deleting existing always-rescue entry Boot$num ($label)"
            efibootmgr -b "$num" -B
        done < <(list_aether_entries || true)
        efibootmgr -c -d "$ESP_DISK" -p "$ESP_PART" -L 'aether-rescue' -l '\EFI\arch\vmlinuz-linux' -u "$cmdline"
    fi
    exit 0
fi

# ── build kernel cmdline ────────────────────────────────────────────────
# take the local /proc/cmdline as-is (root/rootflags/loglevel/nouveau etc. all kept,
# only initrd= is stripped and re-injected per image as initrd=\aether\<file>).
build_cmdline() {
    if [[ -n "$CMDLINE_OVERRIDE" ]]; then
        local tok out=()
        for tok in $CMDLINE_OVERRIDE; do
            case "$tok" in initrd=*) ;; *) out+=("$tok") ;; esac
        done
        printf '%s' "${out[*]}"
        return 0
    fi
    local cl tok out=()
    cl="$(cat /proc/cmdline)"
    for tok in $cl; do
        case "$tok" in initrd=*) ;; *) out+=("$tok") ;; esac
    done
    # note: entries without root= will land directly in rescue mode
    local has_root=0
    for tok in "${out[@]}"; do case "$tok" in root=*) has_root=1 ;; esac; done
    (( has_root )) || printf 'warning: no root= in /proc/cmdline; registered rescue entries will only land in rescue mode\n' >&2
    printf '%s' "${out[*]}"
}
CMDLINE_BASE="$(build_cmdline)"
log "cmdline base template: ${CMDLINE_BASE} (+ initrd=<EFI path> computed dynamically)"

# ── compute EFI device path of the image directory ────────────────────
# The root of an EFI device path is the ESP root (\), not a Linux mount-point name.
# The image directory relative to the ESP mount point, converted to backslashes.
img_dir_abs="$(readlink -f "$IMG_DIR")"
esp_mount_norm="${ESP_MOUNT%/}"
case "$img_dir_abs" in
    "$esp_mount_norm")   rel="" ;;
    "$esp_mount_norm"/*) rel="${img_dir_abs#$esp_mount_norm/}" ;;
    *) die "image directory $IMG_DIR (-> $img_dir_abs) is not under ESP mount point ($esp_mount_norm) -- cannot determine EFI path" ;;
esac
rel_bs="${rel//\//\\}"          # all forward slashes -> backslashes
if [[ -n "$rel_bs" ]]; then efi_dir="\\${rel_bs}"; else efi_dir=""; fi
log "image dir maps to EFI path: ${efi_dir:-\\}\\<file>"

# ── register (idempotent: delete by label then recreate) ───────────────
n_added=0
while read -r num label; do
    log "deleting existing entry Boot$num  ($label)"
    run efibootmgr -b "$num" -B
done < <(list_aether_entries || true)

for img in "${IMAGES[@]}"; do
    file="${img##*/}"
    label="${file%.img}"              # aether-rescue-xxx (prefix is aether-rescue)
    efi_initrd="${efi_dir}\\${file}"   # EFI path: \\ <rel-dir-with-backslashes> \\ <file>
    cmdline="${CMDLINE_BASE} initrd=${efi_initrd}"
    log "registering $label:"
    log "  loader    \\EFI\\arch\\vmlinuz-linux"
    log "  cmdline   ${cmdline}"
    if [[ $DRY_RUN == 1 ]]; then
        printf '+ efibootmgr -c -d %s -p %s -L %q -l %q -u %q\n' "$ESP_DISK" "$ESP_PART" "$label" '\EFI\arch\vmlinuz-linux' "$cmdline"
    else
        efibootmgr -c -d "$ESP_DISK" -p "$ESP_PART" -L "$label" -l '\EFI\arch\vmlinuz-linux' -u "$cmdline"
    fi
    n_added=$((n_added + 1))
done

log ""
log "done: registered $n_added boot entries (labels start with \`${LABEL_PREFIX}\`):"
list_aether_entries | sed 's/^/  Boot/'
log ""
log "After reboot, you will see these in the firmware boot menu (F9/F10/F12): ${LABEL_PREFIX}*."
log "note: BootOrder untouched; initrd paths use EFI device path format (backslashes)."

# ── NVRAM menu timeout: prevents "menu flash-by" (only with explicit --timeout) ──
if [[ -n "$TIMEOUT" ]]; then
    [[ "$TIMEOUT" =~ ^[0-9]+$ ]] || die "--timeout requires a non-negative integer (seconds)"
    log ""
    log "setting NVRAM Timeout = ${TIMEOUT}s..."
    run efibootmgr -t "$TIMEOUT"
else
    cur_tmo="$(efibootmgr 2>/dev/null | awk -F': ' '/^Timeout:/ {print $2}' || true)"
    if [[ -n "$cur_tmo" ]]; then
        log ""
        log "hint: NVRAM Timeout is currently **${cur_tmo}** -- the menu will flash by instantly."
        log "to see the menu at boot: sudo efibootmgr -t 5 (or re-run this script with --timeout 5)"
    fi
fi
