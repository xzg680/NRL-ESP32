[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$DeployUser,
    [string]$DeployHost = 'ota.nrlptt.com',
    [string]$RemoteBinary = '/nrlota/nrl-ota',
    [string]$Service = 'nrl-ota.service'
)

$ErrorActionPreference = 'Stop'

foreach ($command in 'go', 'ssh', 'scp') {
    if (-not (Get-Command $command -ErrorAction SilentlyContinue)) {
        throw "$command was not found in PATH."
    }
}
if ($RemoteBinary -notmatch '^/[A-Za-z0-9._/@-]+$') { throw 'RemoteBinary contains unsupported characters.' }
if ($Service -notmatch '^[A-Za-z0-9_.@-]+$') { throw 'Service contains unsupported characters.' }

$scriptDir = Split-Path -Parent $PSCommandPath
$remote = "$DeployUser@$DeployHost"
$outputDir = Join-Path $scriptDir 'dist'
$outputFile = Join-Path $outputDir 'nrl-ota-linux-amd64'
$remoteStage = "/tmp/nrl-ota-$([guid]::NewGuid().ToString('N')).new"

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
Push-Location $scriptDir
try {
    $env:GOOS = 'linux'
    $env:GOARCH = 'amd64'
    $env:CGO_ENABLED = '0'
    & go build -trimpath -ldflags='-s -w' -o $outputFile .
    if ($LASTEXITCODE -ne 0) { throw "go build failed with exit code $LASTEXITCODE." }

    & scp $outputFile "${remote}:$remoteStage"
    if ($LASTEXITCODE -ne 0) { throw "scp upload failed with exit code $LASTEXITCODE." }

    $remoteCommand = "set -e; sudo cp -p '$RemoteBinary' '$RemoteBinary.previous' 2>/dev/null || true; sudo install -m 0755 '$remoteStage' '$RemoteBinary'; rm -f '$remoteStage'; if ! sudo systemctl restart '$Service'; then sudo test -f '$RemoteBinary.previous' && sudo mv '$RemoteBinary.previous' '$RemoteBinary'; sudo systemctl restart '$Service' || true; exit 1; fi; sudo systemctl is-active --quiet '$Service'"
    & ssh $remote $remoteCommand
    if ($LASTEXITCODE -ne 0) { throw "Remote service deployment failed with exit code $LASTEXITCODE." }

    Write-Host "Published Linux backend to ${remote}:$RemoteBinary and restarted $Service."
}
finally {
    Pop-Location
}
