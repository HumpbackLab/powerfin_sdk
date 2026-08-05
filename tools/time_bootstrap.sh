#!/bin/sh

# Restore a monotonically increasing software clock from the newest ULog.
# This is intended to run before PX4 starts on PowerFin systems without RTC.

MIN_VALID_EPOCH=1234567890
TIME_MARGIN_SECONDS=2

log_info()
{
	echo "powerfin-time-bootstrap: $*"
}

log_error()
{
	echo "powerfin-time-bootstrap: $*" >&2
}

script_dir=$(CDPATH='' cd "$(dirname "$0")" 2>/dev/null && pwd)

if [ -n "$script_dir" ]; then
	default_px4_dir=$(CDPATH='' cd "$script_dir/../.." 2>/dev/null && pwd)
fi

px4_dir=${PX4_DIR:-${default_px4_dir:-/root/px4}}
log_dir=${PX4_LOG_DIR:-$px4_dir/log}

if [ -n "${POWERFIN_TIME_BOOTSTRAP_NOW:-}" ]; then
	now=$POWERFIN_TIME_BOOTSTRAP_NOW
else
	now=$(date +%s 2>/dev/null)
fi

case "$now" in
	''|*[!0-9]*)
		log_error "cannot read the current system time"
		exit 1
		;;
esac

# A valid RTC, GPS, NTP, or MAVLink SYSTEM_TIME always takes precedence.
if [ "$now" -ge "$MIN_VALID_EPOCH" ]; then
	exit 0
fi

if [ ! -d "$log_dir" ]; then
	exit 0
fi

# Scan names once, then select at most one candidate from each PX4 naming
# scheme. This avoids spawning one date process per ULog as the archive grows.
log_files=$(find "$log_dir" -type f -name '*.ulg*' -print 2>/dev/null)

latest_session_log=$(
	printf '%s\n' "$log_files" |
		awk -F/ '
		{
			dir = $(NF - 1)
			file = $NF

			if (dir ~ /^sess[0-9]+$/ && file ~ /^log[0-9]+.*[.]ulg/) {
				session_index = dir
				sub(/^sess/, "", session_index)

				log_index = file
				sub(/^log/, "", log_index)
				sub(/[^0-9].*$/, "", log_index)

				print session_index + 0, log_index + 0, $0
			}
		}' |
		sort -k1,1n -k2,2n |
		tail -n 1 |
		cut -d ' ' -f 3-
)

latest_dated_log=$(
	printf '%s\n' "$log_files" |
		awk -F/ '
		{
			dir = $(NF - 1)
			file = $NF

			if (dir ~ /^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$/ &&
			    file ~ /^[0-9][0-9]_[0-9][0-9]_[0-9][0-9].*[.]ulg/) {
				print dir, substr(file, 1, 8), $0
			}
		}' |
		sort -k1,1 -k2,2 |
		tail -n 1 |
		cut -d ' ' -f 3-
)

latest_log_time=

for candidate in "$latest_session_log" "$latest_dated_log"; do
	if [ -z "$candidate" ]; then
		continue
	fi

	candidate_time=$(date -r "$candidate" +%s 2>/dev/null)

	case "$candidate_time" in
		''|*[!0-9]*)
			continue
			;;
	esac

	if [ -z "$latest_log_time" ] || [ "$candidate_time" -gt "$latest_log_time" ]; then
		latest_log_time=$candidate_time
	fi
done

case "$latest_log_time" in
	''|*[!0-9]*)
		exit 0
		;;
esac

target_time=$((latest_log_time + TIME_MARGIN_SECONDS))

# Never move CLOCK_REALTIME backwards.
if [ "$target_time" -le "$now" ]; then
	exit 0
fi

if [ "${POWERFIN_TIME_BOOTSTRAP_DRY_RUN:-0}" = "1" ]; then
	log_info "would advance system time from $now to $target_time using $log_dir"
	exit 0
fi

if ! date -u -s "@$target_time" >/dev/null 2>&1; then
	log_error "failed to advance system time from $now to $target_time"
	exit 1
fi

log_info "advanced system time from $now to $target_time using $log_dir"
exit 0
