#!/usr/bin/env bash
# Build the standalone OTA frontend and publish it to the production web root.
set -euo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

: "${OTA_DEPLOY_USER:?Set OTA_DEPLOY_USER to the SSH account for ota.nrlptt.com.}"

host="${OTA_DEPLOY_HOST:-ota.nrlptt.com}"
remote="${OTA_DEPLOY_USER}@${host}"
remote_dir="${OTA_DEPLOY_DIR:-/nrlota/www}"

command -v vp >/dev/null || { echo "vp was not found in PATH" >&2; exit 1; }
command -v ssh >/dev/null || { echo "ssh was not found in PATH" >&2; exit 1; }
command -v rsync >/dev/null || { echo "rsync was not found in PATH" >&2; exit 1; }

vp build
ssh "$remote" "test -d '$remote_dir'"
rsync -az --delete --chmod=D755,F644 dist/ "$remote:$remote_dir/"

echo "Published frontend to $remote:$remote_dir/"
