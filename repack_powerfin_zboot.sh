#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="${ROOT_DIR}/kernel-6.1"
NORMAL_DTS_NAME="rk3506-powerfin-spinor"
RECOVERY_DTS_NAME="rk3506-powerfin-ramboot"
FIT_MAX_SIZE=0x7e0000
REPACK="${ROOT_DIR}/repack_powerfin_ramboot.sh"
OUTPUT="${KERNEL_DIR}/zboot.img"
BUILD_DTB=0

usage() {
	cat <<'EOF'
Usage: ./repack_powerfin_zboot.sh [options]

Repack the production PowerFin SPI-NOR multi-configuration FIT. It contains a
shared kernel, normal/recovery DTBs and the recovery initramfs.

Options:
  --amp                Use the Betaflight AMP DTBs and partition size limit.
  --build-dtb          Rebuild both PowerFin DTBs before repacking.
  --output PATH        Output FIT image path. Default: kernel-6.1/zboot.img.
  -h, --help           Show this help.
EOF
}

while (($#)); do
	case "$1" in
		--amp)
			NORMAL_DTS_NAME="rk3506-powerfin-spinor-amp"
			RECOVERY_DTS_NAME="rk3506-powerfin-ramboot-amp"
			FIT_MAX_SIZE=0x6f0000
			shift
			;;
		--build-dtb)
			BUILD_DTB=1
			shift
			;;
		--output)
			OUTPUT="${2:?missing value for --output}"
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "unknown option: $1" >&2
			usage >&2
			exit 1
			;;
	esac
done

NORMAL_DTS="${KERNEL_DIR}/arch/arm/boot/dts/${NORMAL_DTS_NAME}.dts"
RECOVERY_DTS="${KERNEL_DIR}/arch/arm/boot/dts/${RECOVERY_DTS_NAME}.dts"
NORMAL_DTB="${KERNEL_DIR}/arch/arm/boot/dts/${NORMAL_DTS_NAME}.dtb"
RECOVERY_DTB="${KERNEL_DIR}/arch/arm/boot/dts/${RECOVERY_DTS_NAME}.dtb"
OVERLAY_DTS=(
	"${KERNEL_DIR}/arch/arm/boot/dts/rk3506-powerfin-motor-pwm.dts"
	"${KERNEL_DIR}/arch/arm/boot/dts/rk3506-powerfin-spi1-pwm.dts"
)
OVERLAY_DTBO=(
	"${KERNEL_DIR}/arch/arm/boot/dts/rk3506-powerfin-motor-pwm.dtbo"
	"${KERNEL_DIR}/arch/arm/boot/dts/rk3506-powerfin-spi1-pwm.dtbo"
)

for file in "${NORMAL_DTS}" "${RECOVERY_DTS}" "${OVERLAY_DTS[@]}" \
	"${REPACK}"; do
	if [[ ! -f "${file}" ]]; then
		echo "missing required file: ${file}" >&2
		exit 1
	fi
done

if ((BUILD_DTB)); then
	"${ROOT_DIR}/build.sh" "kernel-make:${NORMAL_DTS_NAME}.dtb"
	"${ROOT_DIR}/build.sh" "kernel-make:${RECOVERY_DTS_NAME}.dtb"
	"${ROOT_DIR}/build.sh" kernel-make:rk3506-powerfin-motor-pwm.dtbo
	"${ROOT_DIR}/build.sh" kernel-make:rk3506-powerfin-spi1-pwm.dtbo
elif [[ ! -f "${NORMAL_DTB}" || ! -f "${RECOVERY_DTB}" ||
	! -f "${OVERLAY_DTBO[0]}" || ! -f "${OVERLAY_DTBO[1]}" ]]; then
	echo "missing required PowerFin DTB" >&2
	echo "rerun with --build-dtb" >&2
	exit 1
elif [[ "${NORMAL_DTS}" -nt "${NORMAL_DTB}" || \
	"${RECOVERY_DTS}" -nt "${RECOVERY_DTB}" || \
	"${OVERLAY_DTS[0]}" -nt "${OVERLAY_DTBO[0]}" || \
	"${OVERLAY_DTS[1]}" -nt "${OVERLAY_DTBO[1]}" ]]; then
	echo "a PowerFin DTS is newer than its DTB" >&2
	echo "rerun with --build-dtb" >&2
	exit 1
fi

exec "${REPACK}" \
	--kernel-dts "${NORMAL_DTS_NAME}" \
	--recovery-dts "${RECOVERY_DTS_NAME}" \
	--max-fit-size "${FIT_MAX_SIZE}" \
	--output "${OUTPUT}"
