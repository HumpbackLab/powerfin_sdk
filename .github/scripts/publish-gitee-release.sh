#!/usr/bin/env bash
set -euo pipefail

: "${GITEE_ACCESS_TOKEN:?missing GITEE_ACCESS_TOKEN}"
: "${GITEE_OWNER:?missing GITEE_OWNER}"
: "${GITEE_REPO:?missing GITEE_REPO}"
: "${GITEE_RELEASE_TAG:?missing GITEE_RELEASE_TAG}"
: "${GITEE_RELEASE_TITLE:?missing GITEE_RELEASE_TITLE}"
: "${GITHUB_TRIGGERED_AT:?missing GITHUB_TRIGGERED_AT}"
: "${GITHUB_REPOSITORY:?missing GITHUB_REPOSITORY}"
: "${GITHUB_RUN_ID:?missing GITHUB_RUN_ID}"
: "${GITHUB_SHA:?missing GITHUB_SHA}"

release_dir="${1:?usage: publish-gitee-release.sh RELEASE_DIR}"
release_api="https://gitee.com/api/v5/repos/${GITEE_OWNER}/${GITEE_REPO}/releases"
release_files=(
	"${release_dir}/powerfin-sdcard.img.gz"
	"${release_dir}/update.img"
	"${release_dir}/zboot.img"
	"${release_dir}/SHA256SUMS"
)

for release_file in "${release_files[@]}"; do
	if [[ ! -s "${release_file}" ]]; then
		echo "missing release file: ${release_file}" >&2
		exit 1
	fi
done

release_body="PowerFin SPI NOR full build

GitHub commit: ${GITHUB_SHA}
GitHub run: https://github.com/${GITHUB_REPOSITORY}/actions/runs/${GITHUB_RUN_ID}
Triggered at: ${GITHUB_TRIGGERED_AT} (UTC)
Release title timezone: Asia/Shanghai

The SD-card image is gzip-compressed. Decompress powerfin-sdcard.img.gz before writing it to an SD card.
update.img is the complete SPI NOR upgrade image."

release_json="$(curl --fail-with-body --silent --show-error --retry 3 \
	--request POST \
	--header "Authorization: Bearer ${GITEE_ACCESS_TOKEN}" \
	--data-urlencode "tag_name=${GITEE_RELEASE_TAG}" \
	--data-urlencode "name=${GITEE_RELEASE_TITLE}" \
	--data-urlencode "body=${release_body}" \
	--data-urlencode "target_commitish=master" \
	"${release_api}")"
release_id="$(jq --exit-status --raw-output '.id' <<<"${release_json}")"
release_url="https://gitee.com/${GITEE_OWNER}/${GITEE_REPO}/releases/tag/${GITEE_RELEASE_TAG}"

for release_file in "${release_files[@]}"; do
	echo "Uploading ${release_file}"
	curl --fail-with-body --silent --show-error --retry 3 \
		--request POST \
		--header "Authorization: Bearer ${GITEE_ACCESS_TOKEN}" \
		--form "file=@${release_file}" \
		"${release_api}/${release_id}/attach_files" >/dev/null
done

echo "Published Gitee release ${GITEE_RELEASE_TAG}: ${release_url}"
