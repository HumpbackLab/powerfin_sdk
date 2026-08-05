#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="${ROOT_DIR}/kernel-6.1"
BUSYBOX_VERSION="1.36.1"
BUSYBOX_TARBALL="${ROOT_DIR}/buildroot/dl/busybox/busybox-${BUSYBOX_VERSION}.tar.bz2"
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
NORMAL_DTB="${KERNEL_DIR}/arch/arm/boot/dts/rk3506-powerfin-spinor.dtb"
RECOVERY_DTB="${KERNEL_DIR}/arch/arm/boot/dts/rk3506-powerfin-ramboot.dtb"
GEN_INIT_CPIO="${KERNEL_DIR}/usr/gen_init_cpio"
OUTPUT_IMG="${OUTPUT_DIR}/powerfin-boot.itb"
BOOT_PARTITION_SIZE=$((0x007f0000))
PFC_RECOVERY_DIR="${ROOT_DIR}/tools/penguin-flight-console/dist/recovery"
PFC_BINARY="${PFC_RECOVERY_DIR}/penguin-flight-console"
PFC_BOARD_DIR="${PFC_RECOVERY_DIR}/boards/powerfin"
PFC_BUILD_SCRIPT="${ROOT_DIR}/tools/penguin-flight-console/build-cross.sh"

if [[ ${1:-} == "--output" ]]; then
	OUTPUT_IMG="${2:?missing output path}"
	shift 2
fi

if (($#)); then
	echo "Usage: $0 [--output PATH]" >&2
	exit 1
fi

if [[ ! -x "${PFC_BUILD_SCRIPT}" ]]; then
	echo "missing executable: ${PFC_BUILD_SCRIPT}" >&2
	exit 1
fi

"${PFC_BUILD_SCRIPT}"

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
	"${GEN_INIT_CPIO}" "${PFC_BINARY}" \
	"${PFC_BOARD_DIR}/board.conf" \
	"${PFC_BOARD_DIR}/sdcard-flash.sh"; do
	require_file "${file}"
done

if ! grep -qx 'CONFIG_RD_GZIP=y' "${KERNEL_DIR}/.config"; then
	echo "kernel must enable CONFIG_RD_GZIP=y" >&2
	exit 1
fi

mkdir -p "${BUILD_DIR}" "${OUTPUT_DIR}"
mkdir -p "$(dirname "${OUTPUT_IMG}")"

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
	-e "s~@KERNEL_IMG@~${ZIMAGE}~" \
	-e "s~@RAMDISK_IMG@~${INITRAMFS_GZ}~" \
	"${ITS}" >"${FIT_ITS}"

"${MKIMAGE}" -f "${FIT_ITS}" -E -p 0x800 "${OUTPUT_IMG}"

fit_size="$(stat -c %s "${OUTPUT_IMG}")"
if ((fit_size > BOOT_PARTITION_SIZE)); then
	echo "FIT image is too large for the NOR boot partition:" >&2
	printf '  image: %d bytes\n  limit: %d bytes (0x007f0000)\n' \
		"${fit_size}" "${BOOT_PARTITION_SIZE}" >&2
	exit 1
fi

echo
file "${BUSYBOX_BUILD}/busybox"
sha256sum "${INITRAMFS_GZ}" "${OUTPUT_IMG}"
ls -lh "${BUSYBOX_BUILD}/busybox" "${INITRAMFS_CPIO}" \
	"${INITRAMFS_GZ}" "${OUTPUT_IMG}"
