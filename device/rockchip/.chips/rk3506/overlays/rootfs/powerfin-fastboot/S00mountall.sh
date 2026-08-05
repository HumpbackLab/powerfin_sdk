#!/bin/sh

### BEGIN INIT INFO
# Provides:       mount-all
# Required-Start:
# Required-Stop:
# Default-Start:  S
# Default-Stop:
# Description:    Grow PowerFin root partition and mount internal partitions
### END INIT INFO

DISK=/dev/mmcblk0
PART=/dev/mmcblk0p2
SYS_DISK=/sys/class/block/mmcblk0
SYS_PART=/sys/class/block/mmcblk0p2
RESIZED_MARKER=/.resized

grow_root_partition()
{
	[ -b "$DISK" ] && [ -b "$PART" ] || return 0
	[ -r "$SYS_DISK/size" ] && [ -r "$SYS_PART/start" ] &&
		[ -r "$SYS_PART/size" ] || return 0

	disk_sectors="$(cat "$SYS_DISK/size")"
	part_start="$(cat "$SYS_PART/start")"
	part_sectors="$(cat "$SYS_PART/size")"
	part_end=$((part_start + part_sectors))

	# Allow a 1 MiB tail gap so an already expanded card is a no-op.
	[ $((disk_sectors - part_end)) -le 2048 ] && return 0

	echo "PowerFin: expanding SD root partition to the end of the card"

	# disk-helper's marker only describes the filesystem size. Force its
	# resize check after changing the partition boundary.
	rm -f "$RESIZED_MARKER"

	if ! parted -s "$DISK" resizepart 2 100%; then
		echo "PowerFin: failed to resize partition 2; will retry next boot" >&2
		return 0
	fi

	# Wait until BLKPG_RESIZE_PARTITION is visible before mount-helper invokes
	# disk-helper's existing resize2fs path.
	tries=10
	while [ "$tries" -gt 0 ]; do
		part_sectors="$(cat "$SYS_PART/size")"
		part_end=$((part_start + part_sectors))
		[ $((disk_sectors - part_end)) -le 2048 ] && return 0
		sleep 0.1
		tries=$((tries - 1))
	done

	echo "PowerFin: kernel did not accept the new partition size; will retry next boot" >&2
}

case "$1" in
	start|"")
		grow_root_partition
		mount-helper
		;;
	restart|reload|force-reload)
		echo "Error: argument '$1' not supported" >&2
		exit 3
		;;
	stop|status)
		# No-op
		;;
	*)
		echo "Usage: start" >&2
		exit 3
		;;
esac

:
