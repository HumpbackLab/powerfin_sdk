#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${ROOT_DIR}/kernel-6.1/zboot.img"
EXPECTED_DTB="${ROOT_DIR}/kernel-6.1/arch/arm/boot/dts/rk3506-powerfin-spinor.dtb"
REMOTE_IMAGE="/tmp/zboot.img"
BOOT_DEV="/dev/block/by-name/boot"
KNOWN_HOSTS="/tmp/luckfox_known_hosts"

# defaults
HOST="192.168.123.100"
USER="root"
PASSWORD="luckfox"
FAST=false
REBOOT=true

usage() {
	cat <<EOF
Usage: $(basename "$0") [-f] [-n] [-i <ip>] [-u <user>] [-p <password>]

Options:
  -f    Fast mode: skip all interactive prompts
  -n    Do not reboot after a verified write
  -i    Device IP   (default: ${HOST})
  -u    Username    (default: ${USER})
  -p    Password    (default: ${PASSWORD})
EOF
	exit 0
}

while getopts "fni:u:p:h" opt; do
	case "${opt}" in
		f) FAST=true ;;
		n) REBOOT=false ;;
		i) HOST="${OPTARG}" ;;
		u) USER="${OPTARG}" ;;
		p) PASSWORD="${OPTARG}" ;;
		h) usage ;;
		*) usage ;;
	esac
done
shift $((OPTIND - 1))

read_default() {
	local prompt="$1"
	local default="$2"
	local value

	read -r -p "${prompt} [${default}]: " value
	printf '%s' "${value:-$default}"
}

require_cmd() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "missing required command: $1" >&2
		exit 1
	fi
}

require_cmd sshpass
require_cmd ssh
require_cmd scp
require_cmd sha256sum
require_cmd stat
require_cmd dumpimage
require_cmd fdtget

if [[ ! -f "${IMAGE}" ]]; then
	echo "image not found: ${IMAGE}" >&2
	exit 1
fi
if [[ ! -f "${EXPECTED_DTB}" ]]; then
	echo "expected DTB not found: ${EXPECTED_DTB}" >&2
	exit 1
fi

FIT_LIST="$(dumpimage -l "${IMAGE}")"
if grep -Eq 'Image [0-9]+ \((resource|ramdisk)\)' <<<"${FIT_LIST}"; then
	echo "refusing to flash FIT containing resource or ramdisk: ${IMAGE}" >&2
	exit 1
fi
if ! grep -Eq 'Image [0-9]+ \(fdt\)' <<<"${FIT_LIST}" ||
	! grep -Eq 'Image [0-9]+ \(kernel\)' <<<"${FIT_LIST}"; then
	echo "refusing to flash FIT without both fdt and kernel: ${IMAGE}" >&2
	exit 1
fi

FIT_DTB="$(mktemp)"
trap 'rm -f "${FIT_DTB}"' EXIT
dumpimage -T flat_dt -p 0 -o "${FIT_DTB}" "${IMAGE}" >/dev/null
FIT_BOOTARGS="$(fdtget "${FIT_DTB}" /chosen bootargs)"
EXPECTED_BOOTARGS="$(fdtget "${EXPECTED_DTB}" /chosen bootargs)"
if [[ "${FIT_BOOTARGS}" != "${EXPECTED_BOOTARGS}" ]]; then
	echo "refusing to flash FIT whose bootargs differ from the built PowerFin DTB:" >&2
	echo "  FIT: ${FIT_BOOTARGS}" >&2
	echo "  DTB: ${EXPECTED_BOOTARGS}" >&2
	exit 1
fi
echo "Bootargs  : ${FIT_BOOTARGS}"

if [[ "${FAST}" == true ]]; then
	echo "Device IP : ${HOST}"
	echo "Username  : ${USER}"
	echo "Password  : ***"
else
	HOST="$(read_default "Device IP" "${HOST}")"
	USER="$(read_default "Username" "${USER}")"
	read -r -s -p "Password [${PASSWORD}]: " pw
	echo
	PASSWORD="${pw:-${PASSWORD}}"
fi

SSH_OPTS=(
	-o StrictHostKeyChecking=no
	-o UserKnownHostsFile="${KNOWN_HOSTS}"
	-o ConnectTimeout=5
)

# The Buildroot image generates its SSH host key on first boot. Reflashing the
# SD rootfs therefore legitimately changes the key; this file is private to
# this one-shot flashing helper and must not retain a previous image's key.
rm -f "${KNOWN_HOSTS}"

LOCAL_SIZE="$(stat -c '%s' "${IMAGE}")"
LOCAL_SHA="$(sha256sum "${IMAGE}" | awk '{print $1}')"

echo "Checking target..."
REMOTE_SIZE="$(
	sshpass -p "${PASSWORD}" ssh "${SSH_OPTS[@]}" "${USER}@${HOST}" \
		"if command -v blockdev >/dev/null 2>&1; then
			blockdev --getsize64 '${BOOT_DEV}'
		else
			dev=\$(readlink -f '${BOOT_DEV}') || exit 1
			name=\${dev##*/}
			sectors=\$(cat \"/sys/class/block/\${name}/size\") || exit 1
			echo \$((sectors * 512))
		fi"
)"

if [[ ! "${REMOTE_SIZE}" =~ ^[0-9]+$ ]]; then
	echo "failed to read boot partition size: ${BOOT_DEV}" >&2
	exit 1
fi

if (( LOCAL_SIZE > REMOTE_SIZE )); then
	echo "image is larger than boot partition: ${LOCAL_SIZE} > ${REMOTE_SIZE}" >&2
	exit 1
fi

REMOTE_MTD="$(
	sshpass -p "${PASSWORD}" ssh "${SSH_OPTS[@]}" "${USER}@${HOST}" \
		"block=\$(readlink -f '${BOOT_DEV}') || exit 1
		name=\${block##*/}
		case \"\${name}\" in
			mtdblock[0-9]*) mtd=/dev/mtd\${name#mtdblock} ;;
			*) exit 1 ;;
		esac
		[ -c \"\${mtd}\" ] || exit 1
		command -v flashcp >/dev/null 2>&1 || exit 1
		echo \"\${mtd}\""
)"

if [[ ! "${REMOTE_MTD}" =~ ^/dev/mtd[0-9]+$ ]]; then
	echo "failed to resolve character MTD for ${BOOT_DEV}" >&2
	exit 1
fi

echo "Uploading ${IMAGE}..."
sshpass -p "${PASSWORD}" scp "${SSH_OPTS[@]}" "${IMAGE}" "${USER}@${HOST}:${REMOTE_IMAGE}"

echo "Verifying uploaded image..."
REMOTE_SHA="$(
	sshpass -p "${PASSWORD}" ssh "${SSH_OPTS[@]}" "${USER}@${HOST}" \
		"sha256sum '${REMOTE_IMAGE}' | awk '{print \$1}'"
)"

if [[ "${LOCAL_SHA}" != "${REMOTE_SHA}" ]]; then
	echo "sha256 mismatch after upload:" >&2
	echo "  local : ${LOCAL_SHA}" >&2
	echo "  remote: ${REMOTE_SHA}" >&2
	exit 1
fi

echo "Writing ${BOOT_DEV}..."
sshpass -p "${PASSWORD}" ssh "${SSH_OPTS[@]}" "${USER}@${HOST}" \
	"flashcp -v '${REMOTE_IMAGE}' '${REMOTE_MTD}'"

echo "Verifying flash readback..."
READBACK_SHA="$(
	sshpass -p "${PASSWORD}" ssh "${SSH_OPTS[@]}" "${USER}@${HOST}" \
		"dd if='${REMOTE_MTD}' bs=${LOCAL_SIZE} count=1 2>/dev/null | sha256sum | awk '{print \$1}'"
)"

if [[ "${LOCAL_SHA}" != "${READBACK_SHA}" ]]; then
	echo "sha256 mismatch after write:" >&2
	echo "  local   : ${LOCAL_SHA}" >&2
	echo "  readback: ${READBACK_SHA}" >&2
	exit 1
fi

echo "Write verified: ${LOCAL_SHA}"
if [[ "${REBOOT}" == true ]]; then
	echo "Rebooting target..."
	sshpass -p "${PASSWORD}" ssh "${SSH_OPTS[@]}" "${USER}@${HOST}" "sync; reboot" || true
else
	echo "Leaving target running (-n)."
fi

echo "Done."
