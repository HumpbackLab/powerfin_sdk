#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="${ROOT_DIR}/kernel-6.1"
BUSYBOX_VERSION="1.36.1"
BUSYBOX_TARBALL="${ROOT_DIR}/buildroot/dl/busybox/busybox-${BUSYBOX_VERSION}.tar.bz2"
PFC_RELEASE_CONFIG="${ROOT_DIR}/buildroot/package/rockchip/penguin-flight-console/pfc-release.conf"
PFC_HASH_FILE="${ROOT_DIR}/buildroot/package/rockchip/penguin-flight-console/penguin-flight-console.hash"
TOOLCHAIN_DIR="${ROOT_DIR}/prebuilts/gcc/linux-x86/arm/gcc-arm-10.3-2021.07-x86_64-arm-none-linux-gnueabihf/bin"
CROSS_COMPILE="${TOOLCHAIN_DIR}/arm-none-linux-gnueabihf-"
TOOLS_DIR="${ROOT_DIR}/tools/powerfin_ramboot"
OUTPUT_DIR="${ROOT_DIR}/output/powerfin-ramboot"
BUILD_DIR="${OUTPUT_DIR}/build"
BUSYBOX_SRC="${BUILD_DIR}/busybox-${BUSYBOX_VERSION}"
BUSYBOX_BUILD="${BUILD_DIR}/busybox-out"
BUSYBOX_CONFIG="${TOOLS_DIR}/busybox-minimal.config"
MERGE_CONFIG="${ROOT_DIR}/buildroot/support/kconfig/merge_config.sh"
INIT="${TOOLS_DIR}/init"
UDHCPD_CONFIG="${TOOLS_DIR}/powerfin-recovery-udhcpd.conf"
INITRAMFS_TEMPLATE="${TOOLS_DIR}/initramfs.list.in"
INITRAMFS_LIST="${OUTPUT_DIR}/initramfs.list"
INITRAMFS_CPIO="${OUTPUT_DIR}/rootfs.cpio"
INITRAMFS_GZ="${OUTPUT_DIR}/rootfs.cpio.gz"
ITS="${ROOT_DIR}/device/rockchip/.chips/rk3506/powerfin-boot.its"
FIT_ITS="${OUTPUT_DIR}/powerfin-boot.its"
MKIMAGE="${ROOT_DIR}/rkbin/tools/mkimage"
ZIMAGE="${KERNEL_DIR}/arch/arm/boot/zImage"
GEN_INIT_CPIO="${KERNEL_DIR}/usr/gen_init_cpio"
OUTPUT_IMG="${OUTPUT_DIR}/powerfin-boot.itb"
KERNEL_DTS_NAME="${RK_KERNEL_DTS_NAME:-}"
RECOVERY_DTS_NAME="${RK_BOOT_FIT_RECOVERY_DTS_NAME:-}"
FIT_MAX_SIZE="${RK_BOOT_FIT_MAX_SIZE:-0}"

usage() {
	cat <<'EOF'
Usage: repack_powerfin_ramboot.sh [options]

Options:
  --kernel-dts NAME    Normal device-tree name (without .dtb).
  --recovery-dts NAME  Recovery device-tree name (without .dtb).
  --max-fit-size SIZE  Maximum FIT size in bytes; zero disables the check.
  --output PATH        Output FIT image path.
EOF
}

while (($#)); do
	case "$1" in
		--kernel-dts)
			KERNEL_DTS_NAME="${2:?missing value for --kernel-dts}"
			shift 2
			;;
		--recovery-dts)
			RECOVERY_DTS_NAME="${2:?missing value for --recovery-dts}"
			shift 2
			;;
		--max-fit-size)
			FIT_MAX_SIZE="${2:?missing value for --max-fit-size}"
			shift 2
			;;
		--output)
			OUTPUT_IMG="${2:?missing value for --output}"
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

: "${KERNEL_DTS_NAME:?missing RK_KERNEL_DTS_NAME or --kernel-dts}"
: "${RECOVERY_DTS_NAME:?missing RK_BOOT_FIT_RECOVERY_DTS_NAME or --recovery-dts}"
if [[ ! "${FIT_MAX_SIZE}" =~ ^(0|[1-9][0-9]*|0[xX][0-9a-fA-F]+)$ ]]; then
	echo "invalid FIT size limit: ${FIT_MAX_SIZE}" >&2
	exit 1
fi
FIT_MAX_SIZE=$((FIT_MAX_SIZE))
NORMAL_DTB="${KERNEL_DIR}/arch/arm/boot/dts/${KERNEL_DTS_NAME}.dtb"
RECOVERY_DTB="${KERNEL_DIR}/arch/arm/boot/dts/${RECOVERY_DTS_NAME}.dtb"
MOTOR_PWM_DTBO="${KERNEL_DIR}/arch/arm/boot/dts/rk3506-powerfin-motor-pwm.dtbo"
SPI1_PWM_DTBO="${KERNEL_DIR}/arch/arm/boot/dts/rk3506-powerfin-spi1-pwm.dtbo"

if [[ ! -f "${PFC_RELEASE_CONFIG}" ]]; then
	echo "missing PFC release config: ${PFC_RELEASE_CONFIG}" >&2
	exit 1
fi
if [[ ! -f "${PFC_HASH_FILE}" ]]; then
	echo "missing PFC hash file: ${PFC_HASH_FILE}" >&2
	exit 1
fi

# pfc-release.conf intentionally uses syntax accepted by both shell and make.
# shellcheck source=/dev/null
source "${PFC_RELEASE_CONFIG}"
: "${PFC_RELEASE_VERSION:?missing PFC_RELEASE_VERSION}"
: "${PFC_RELEASE_SOURCE:?missing PFC_RELEASE_SOURCE}"
: "${PFC_RELEASE_SITE:?missing PFC_RELEASE_SITE}"

PFC_RELEASE_SHA256="$(awk -v source="${PFC_RELEASE_SOURCE}" \
	'$1 == "sha256" && $3 == source { print $2 }' "${PFC_HASH_FILE}")"
if [[ ! "${PFC_RELEASE_SHA256}" =~ ^[[:xdigit:]]{64}$ ]]; then
	echo "missing or invalid PFC release SHA256 in ${PFC_HASH_FILE}" >&2
	exit 1
fi

PFC_RELEASE_URL="${PFC_RELEASE_SITE}/${PFC_RELEASE_SOURCE}"
PFC_TARBALL="${ROOT_DIR}/buildroot/dl/penguin-flight-console/${PFC_RELEASE_SOURCE}"
PFC_RELEASE_DIR="${BUILD_DIR}/pfc-${PFC_RELEASE_VERSION}-${PFC_RELEASE_SHA256:0:16}"
PFC_BINARY="${PFC_RELEASE_DIR}/penguin-flight-console"
PFC_BOARD_DIR="${PFC_RELEASE_DIR}"

verify_pfc_tarball() {
	echo "${PFC_RELEASE_SHA256}  ${PFC_TARBALL}" | sha256sum -c - >/dev/null 2>&1
}

fetch_pfc_release() {
	local download_tmp

	if verify_pfc_tarball; then
		return
	fi

	if ! command -v curl >/dev/null 2>&1; then
		echo "curl is required to download the PFC release" >&2
		exit 1
	fi

	mkdir -p "$(dirname "${PFC_TARBALL}")"
	download_tmp="$(mktemp "${PFC_TARBALL}.tmp.XXXXXX")"
	if ! curl --fail --location --retry 3 \
		"${PFC_RELEASE_URL}" --output "${download_tmp}"; then
		rm -f -- "${download_tmp}"
		exit 1
	fi
	if ! echo "${PFC_RELEASE_SHA256}  ${download_tmp}" | sha256sum -c -; then
		rm -f -- "${download_tmp}"
		exit 1
	fi
	mv -f -- "${download_tmp}" "${PFC_TARBALL}"
}

extract_pfc_release() {
	local extract_tmp

	if [[ ! -d "${PFC_RELEASE_DIR}" ]]; then
		mkdir -p "${BUILD_DIR}"
		extract_tmp="$(mktemp -d "${BUILD_DIR}/.pfc-release.XXXXXX")"
		if ! tar --no-same-owner -xzf "${PFC_TARBALL}" -C "${extract_tmp}"; then
			rm -rf -- "${extract_tmp}"
			exit 1
		fi
		mv -- "${extract_tmp}" "${PFC_RELEASE_DIR}"
	fi

	(cd "${PFC_RELEASE_DIR}" && sha256sum -c SHA256SUMS)
}

mkdir -p "${BUILD_DIR}" "${OUTPUT_DIR}"
mkdir -p "$(dirname "${OUTPUT_IMG}")"
fetch_pfc_release
extract_pfc_release

require_file() {
	if [[ ! -f "$1" ]]; then
		echo "missing required file: $1" >&2
		exit 1
	fi
}

for file in "${BUSYBOX_TARBALL}" "${CROSS_COMPILE}gcc" \
	"${BUSYBOX_CONFIG}" "${MERGE_CONFIG}" "${INIT}" \
	"${UDHCPD_CONFIG}" "${INITRAMFS_TEMPLATE}" "${ITS}" \
	"${MKIMAGE}" "${ZIMAGE}" "${NORMAL_DTB}" "${RECOVERY_DTB}" \
	"${MOTOR_PWM_DTBO}" "${SPI1_PWM_DTBO}" \
	"${GEN_INIT_CPIO}" "${PFC_BINARY}" \
	"${PFC_BOARD_DIR}/board.conf" \
	"${PFC_BOARD_DIR}/scripts/kernel-flash.sh" \
	"${PFC_BOARD_DIR}/scripts/sdcard-flash.sh"; do
	require_file "${file}"
done

if ! grep -qx 'CONFIG_RD_GZIP=y' "${KERNEL_DIR}/.config"; then
	echo "kernel must enable CONFIG_RD_GZIP=y" >&2
	exit 1
fi

if [[ ! -d "${BUSYBOX_SRC}" ]]; then
	tar -xf "${BUSYBOX_TARBALL}" -C "${BUILD_DIR}"
fi

if [[ ! -x "${BUSYBOX_BUILD}/busybox" || \
	"${BUSYBOX_CONFIG}" -nt "${BUSYBOX_BUILD}/busybox" ]]; then
	mkdir -p "${BUSYBOX_BUILD}"
	make -C "${BUSYBOX_SRC}" O="${BUSYBOX_BUILD}" \
		ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" allnoconfig >/dev/null
	KCONFIG_CONFIG="${BUSYBOX_BUILD}/.config" \
		"${MERGE_CONFIG}" -m "${BUSYBOX_BUILD}/.config" \
		"${BUSYBOX_CONFIG}"
	set +o pipefail
	yes "" | make -C "${BUSYBOX_SRC}" O="${BUSYBOX_BUILD}" \
		ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" oldconfig >/dev/null
	oldconfig_status="${PIPESTATUS[1]}"
	set -o pipefail
	if [[ "${oldconfig_status}" -ne 0 ]]; then
		echo "BusyBox oldconfig failed" >&2
		exit "${oldconfig_status}"
	fi
	make -C "${BUSYBOX_SRC}" O="${BUSYBOX_BUILD}" \
		ARCH=arm CROSS_COMPILE="${CROSS_COMPILE}" -j"$(nproc)"
fi

if ! file "${BUSYBOX_BUILD}/busybox" | grep -q "statically linked"; then
	echo "BusyBox is not statically linked" >&2
	exit 1
fi

sed -e "s~@INIT@~${INIT}~" \
	-e "s~@BUSYBOX@~${BUSYBOX_BUILD}/busybox~" \
	-e "s~@UDHCPD_CONFIG@~${UDHCPD_CONFIG}~" \
	-e "s~@PFC_BINARY@~${PFC_BINARY}~" \
	-e "s~@PFC_BOARD_DIR@~${PFC_BOARD_DIR}~" \
	"${INITRAMFS_TEMPLATE}" >"${INITRAMFS_LIST}"

"${GEN_INIT_CPIO}" "${INITRAMFS_LIST}" >"${INITRAMFS_CPIO}"
gzip -n -9 -c "${INITRAMFS_CPIO}" >"${INITRAMFS_GZ}"

sed -e "s~@KERNEL_DTB@~${NORMAL_DTB}~" \
	-e "s~@RECOVERY_KERNEL_DTB@~${RECOVERY_DTB}~" \
	-e "s~@MOTOR_PWM_DTBO@~${MOTOR_PWM_DTBO}~" \
	-e "s~@SPI1_PWM_DTBO@~${SPI1_PWM_DTBO}~" \
	-e "s~@KERNEL_IMG@~${ZIMAGE}~" \
	-e "s~@RAMDISK_IMG@~${INITRAMFS_GZ}~" \
	"${ITS}" >"${FIT_ITS}"

"${MKIMAGE}" -f "${FIT_ITS}" -E -p 0x800 "${OUTPUT_IMG}"

fit_size="$(stat -c %s "${OUTPUT_IMG}")"
if ((FIT_MAX_SIZE > 0 && fit_size > FIT_MAX_SIZE)); then
	echo "FIT image is too large for the NOR boot partition:" >&2
	printf '  image: %d bytes\n  limit: %d bytes (0x%08x)\n' \
		"${fit_size}" "${FIT_MAX_SIZE}" "${FIT_MAX_SIZE}" >&2
	exit 1
fi

echo
file "${BUSYBOX_BUILD}/busybox"
sha256sum "${INITRAMFS_GZ}" "${OUTPUT_IMG}"
ls -lh "${BUSYBOX_BUILD}/busybox" "${INITRAMFS_CPIO}" \
	"${INITRAMFS_GZ}" "${OUTPUT_IMG}"
