#!/usr/bin/env bash
# Cross-compile the Go API for Linux amd64, upload it, then restart systemd.
set -euo pipefail

: "${OTA_DEPLOY_USER:?Set OTA_DEPLOY_USER to the SSH account for ota.nrlptt.com.}"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
host="${OTA_DEPLOY_HOST:-ota.nrlptt.com}"
remote_binary="${OTA_BACKEND_BINARY:-/nrlota/nrl-ota}"
service="${OTA_BACKEND_SERVICE:-nrl-ota.service}"
remote="${OTA_DEPLOY_USER}@${host}"
output_dir="$script_dir/dist"
output_file="$output_dir/nrl-ota-linux-amd64"
remote_stage="/tmp/nrl-ota-$(date +%s)-$$.new"

for command in go ssh scp; do
  command -v "$command" >/dev/null || { echo "$command was not found in PATH" >&2; exit 1; }
done

mkdir -p "$output_dir"
cd "$script_dir"
GOOS=linux GOARCH=amd64 CGO_ENABLED=0 go build -trimpath -ldflags='-s -w' -o "$output_file" .
scp "$output_file" "$remote:$remote_stage"
ssh "$remote" "set -e; sudo cp -p '$remote_binary' '$remote_binary.previous' 2>/dev/null || true; sudo install -m 0755 '$remote_stage' '$remote_binary'; rm -f '$remote_stage'; if ! sudo systemctl restart '$service'; then sudo test -f '$remote_binary.previous' && sudo mv '$remote_binary.previous' '$remote_binary'; sudo systemctl restart '$service' || true; exit 1; fi; sudo systemctl is-active --quiet '$service'"

echo "Published Linux backend to $remote:$remote_binary and restarted $service."
