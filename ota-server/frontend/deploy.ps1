[CmdletBinding()]
param(
    [string]$DeployUser = $env:OTA_DEPLOY_USER,
    [string]$DeployHost = $(if ($env:OTA_DEPLOY_HOST) { $env:OTA_DEPLOY_HOST } else { 'ota.nrlptt.com' }),
    [string]$DeployDir = $(if ($env:OTA_DEPLOY_DIR) { $env:OTA_DEPLOY_DIR } else { '/nrlota/www' })
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($DeployUser)) {
    throw 'Set OTA_DEPLOY_USER or pass -DeployUser with the SSH account for ota.nrlptt.com.'
}

foreach ($command in 'vp', 'ssh', 'scp') {
    if (-not (Get-Command $command -ErrorAction SilentlyContinue)) {
        throw "$command was not found in PATH."
    }
}

$scriptDir = Split-Path -Parent $PSCommandPath
$remote = "$DeployUser@$DeployHost"
$distDir = Join-Path $scriptDir 'dist'

Push-Location $scriptDir
try {
    & vp build
    if ($LASTEXITCODE -ne 0) { throw "vp build failed with exit code $LASTEXITCODE." }

    & ssh $remote "test -d '$DeployDir'"
    if ($LASTEXITCODE -ne 0) { throw "Remote directory does not exist: $DeployDir" }

    # Windows OpenSSH provides scp but not rsync. Vite asset names are content-
    # hashed, so retained old assets are harmless; use deploy.sh when pruning is
    # required on a host that has rsync.
    & scp -r (Join-Path $distDir '*') "${remote}:$DeployDir/"
    if ($LASTEXITCODE -ne 0) { throw "scp upload failed with exit code $LASTEXITCODE." }

    Write-Host "Published frontend to ${remote}:$DeployDir/"
}
finally {
    Pop-Location
}
