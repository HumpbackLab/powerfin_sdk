# Give each PowerFin a stable, locally administered RNDIS MAC pair derived
# from its SoC serial number. This keeps the Windows adapter and DHCP lease
# stable across board reboots.
powerfin_serial=$(awk '/^Serial[[:space:]]*:/ { print $3; exit }' /proc/cpuinfo)
powerfin_suffix=$(printf '%s' "$powerfin_serial" | tail -c 10 | \
	sed 's/../&:/g; s/:$//')

if [ ${#powerfin_suffix} -ne 14 ]; then
	powerfin_suffix=00:00:00:35:06
fi

rndis_prepare()
{
	host_mac="02:$powerfin_suffix"
	device_mac="06:$powerfin_suffix"

	# usbdevice also runs prepare on update events, when configfs rejects writes
	# to an already-bound function. Only initialize values that differ.
	[ "$(cat host_addr)" = "$host_mac" ] || echo "$host_mac" > host_addr
	[ "$(cat dev_addr)" = "$device_mac" ] || echo "$device_mac" > dev_addr
}
