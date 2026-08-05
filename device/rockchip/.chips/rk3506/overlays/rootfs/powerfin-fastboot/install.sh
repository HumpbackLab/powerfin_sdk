#!/bin/bash -e

TARGET_DIR="$1"
[ "$TARGET_DIR" ] || exit 1

case "$RK_DEFCONFIG" in
powerfin_*) ;;
*) exit 0 ;;
esac

OVERLAY_DIR="$(dirname "$(realpath "$0")")"
SDK_DIR="$(realpath "$OVERLAY_DIR/../../../../../../..")"
AT7456_DIR="$SDK_DIR/tools/at7456"
AT7456_CROSS_COMPILE="$SDK_DIR/prebuilts/gcc/linux-x86/arm/"\
"gcc-arm-10.3-2021.07-x86_64-arm-none-linux-gnueabihf/bin/"\
"arm-none-linux-gnueabihf-"

message "Installing PowerFin fastboot rootfs policy to $TARGET_DIR..."

install -m 0755 "$OVERLAY_DIR/rcS" "$TARGET_DIR/etc/init.d/rcS"
install -m 0755 "$OVERLAY_DIR/S00mountall.sh" \
	"$TARGET_DIR/etc/init.d/S00mountall.sh"
rm -f "$TARGET_DIR/etc/init.d/S00grow-root-partition" \
	"$TARGET_DIR/etc/init.d/S04powerfin-grow-rootfs"
install -m 0755 "$OVERLAY_DIR/S10udev" "$TARGET_DIR/etc/init.d/S10udev"
install -m 0755 "$OVERLAY_DIR/S11powerfin-parallel" \
	"$TARGET_DIR/etc/init.d/S11powerfin-parallel"
install -m 0755 "$OVERLAY_DIR/S06powerfin-usb-gadget" \
	"$TARGET_DIR/etc/init.d/S06powerfin-usb-gadget"
install -m 0755 "$OVERLAY_DIR/S51powerfin-rndis" \
	"$TARGET_DIR/etc/init.d/S51powerfin-rndis"
rm -f "$TARGET_DIR/etc/init.d/S52powerfin-wifi-ap"
install -m 0755 "$OVERLAY_DIR/S52powerfin-wifi" \
	"$TARGET_DIR/etc/init.d/S52powerfin-wifi"
install -m 0755 "$OVERLAY_DIR/S60powerfin-netdevice" \
	"$TARGET_DIR/etc/init.d/S60netdevice"
install -m 0755 "$OVERLAY_DIR/S99z-bootmark" \
	"$TARGET_DIR/etc/init.d/S99z-bootmark"
install -m 0644 -D "$OVERLAY_DIR/powerfin-fastboot.conf" \
	"$TARGET_DIR/etc/modprobe.d/powerfin-fastboot.conf"
install -m 0644 "$OVERLAY_DIR/powerfin-rndis-dnsmasq.conf" \
	"$TARGET_DIR/etc/powerfin-rndis-dnsmasq.conf"
install -m 0644 "$OVERLAY_DIR/powerfin-hostapd.conf" \
	"$TARGET_DIR/etc/powerfin-hostapd.conf"
install -m 0644 "$OVERLAY_DIR/powerfin-ap-dnsmasq.conf" \
	"$TARGET_DIR/etc/powerfin-ap-dnsmasq.conf"
install -m 0644 -D "$OVERLAY_DIR/powerfin-rndis-usbdevice.sh" \
	"$TARGET_DIR/etc/usbdevice.d/powerfin-rndis.sh"
install -m 0644 "$OVERLAY_DIR/powerfin-release" \
	"$TARGET_DIR/etc/powerfin-release"
make -C "$AT7456_DIR" \
	CROSS_COMPILE="$AT7456_CROSS_COMPILE" \
	BUILD_DIR="$TARGET_DIR/usr/bin"
install -m 0644 -D "$AT7456_DIR/betaflight.mcm" \
	"$TARGET_DIR/usr/share/powerfin/betaflight.mcm"
install -m 0755 -D "$SDK_DIR/tools/time_bootstrap.sh" \
	"$TARGET_DIR/root/px4/posix-configs/powerfin/time_bootstrap.sh"

# Run independent post-udev services concurrently. Keep their original
# scripts outside /etc/init.d so rcS/rcK only invoke the coordinator.
PARALLEL_DIR="$TARGET_DIR/usr/libexec/powerfin-init"
install -d "$PARALLEL_DIR"
for script in \
	S11alsa-utils \
	S30dbus \
	S35es8311 \
	S35wifibt-poweron.sh \
	S40network \
	S45usbconfig \
	S50sshd \
	S50usbdevice.sh \
	S51powerfin-rndis \
	S60netdevice \
	S99-auto-reboot
do
	if [ -f "$TARGET_DIR/etc/init.d/$script" ]; then
		mv "$TARGET_DIR/etc/init.d/$script" "$PARALLEL_DIR/$script"
	fi
done

# RNDIS uses its own isolated dnsmasq configuration and pid file. The generic
# service has no /etc/dnsmasq.conf on PowerFin and would only add a no-op fork.
rm -f "$TARGET_DIR/etc/init.d/S80dnsmasq"

# Loading every kernel module serially probes hardware that is not needed
# during early boot. PowerFin loads optional modules on demand.
rm -f "$TARGET_DIR/etc/init.d/S03modules_init.sh"

# PowerFin has a fixed device tree. Keep the utility for manual debugging, but
# do not reapply generic Luckfox board configuration on every boot.
rm -f "$TARGET_DIR/etc/init.d/S99luckfoxconfigload"

# Bluetooth is not used on PowerFin. Wi-Fi remains enabled and its power-on,
# network, kernel modules and AIC8800 firmware are intentionally preserved.
rm -f "$TARGET_DIR/etc/init.d/S40bluetoothd"
