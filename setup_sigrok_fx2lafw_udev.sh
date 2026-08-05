#!/usr/bin/env bash
set -euo pipefail

RULE_FILE=/etc/udev/rules.d/60-sigrok-fx2lafw.rules

sudo tee "$RULE_FILE" >/dev/null <<'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="1d50", ATTR{idProduct}=="608c", MODE="0666", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="0925", ATTR{idProduct}=="3881", MODE="0666", TAG+="uaccess"
SUBSYSTEM=="usb", ATTR{idVendor}=="04b4", ATTR{idProduct}=="8613", MODE="0666", TAG+="uaccess"
EOF

sudo udevadm control --reload-rules
sudo udevadm trigger

echo "Installed $RULE_FILE"
echo "Unplug and replug the FX2LA device, then test with:"
echo "/home/ncer/sigrok-local/bin/sigrok-cli --driver fx2lafw --scan"
