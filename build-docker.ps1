<#
.SYNOPSIS
    Build the Hiworld canbox firmware in a container. Windows launcher.

.DESCRIPTION
    Needs nothing on the machine but Docker Desktop -- no cross toolchain, no
    Python, no compiler. The builder image is built once from docker\Dockerfile
    and reused; this repo is bind-mounted into it, so the packed .iap lands in
    build\ exactly where a native 'python tools\build.py --pack' would leave it.

    Flashing deliberately stays OUT of the container -- it needs the USB serial
    port and an SWD probe. Do that on the host, against the .iap this produced:

        python tools\iap_upload.py build\canupdate<version>.iap `
               --port COM17 --reset powercycle --verify

.PARAMETER Protocol
    Which head-unit dialect to build: jeep (stock-equivalent), benz (the
    Mercedes emulation), or both. Default both.

.PARAMETER NoTests
    Skip the host unit tests and go straight to the firmware.

.PARAMETER TestsOnly
    Run the host unit tests and stop.

.PARAMETER Clean
    Remove build\ and stop.

.PARAMETER Shell
    Open an interactive shell inside the builder instead of building.

.PARAMETER RebuildImage
    Force a fresh image build even if one is already cached.

.PARAMETER ShowCommands
    Echo the real compiler command lines.

.EXAMPLE
    .\build-docker.ps1

.EXAMPLE
    .\build-docker.ps1 -Protocol jeep -NoTests
#>
param(
    [ValidateSet('jeep', 'benz', 'both')]
    [string]$Protocol = 'both',
    [switch]$NoTests,
    [switch]$TestsOnly,
    [switch]$Clean,
    [switch]$Shell,
    [switch]$RebuildImage,
    [switch]$ShowCommands,
    [string]$Image = $(if ($env:CANBOX_IMAGE) { $env:CANBOX_IMAGE } else { 'canbox-builder' }),
    [string]$Tag   = $(if ($env:CANBOX_TAG)   { $env:CANBOX_TAG }   else { '15.2.1-1.1' })
)

# Windows PowerShell 5.1 turns anything a native command writes to stderr into
# ErrorRecords, and under $ErrorActionPreference='Stop' that aborts the script --
# on a compiler warning, on docker's own progress chatter, on `docker info`
# failing. A launcher must not be that fragile, so native results are judged by
# $LASTEXITCODE alone and failures are reported explicitly.
$ErrorActionPreference = 'Continue'

function Fail([string]$Message) {
    Write-Host "error: $Message" -ForegroundColor Red
    exit 1
}

$root = $PSScriptRoot
$ref  = "${Image}:${Tag}"

# Probe a native command for its exit code, discarding whatever it prints.
function Test-NativeCommand {
    param([string]$Exe, [string[]]$CmdArgs)
    try   { & $Exe @CmdArgs 2>$null | Out-Null }
    catch { return $false }
    return ($LASTEXITCODE -eq 0)
}

# --- container runtime ----------------------------------------------------
$docker = $env:DOCKER
if (-not $docker) {
    if (Get-Command docker -ErrorAction SilentlyContinue) {
        $docker = 'docker'
    } elseif (Get-Command podman -ErrorAction SilentlyContinue) {
        $docker = 'podman'
    } else {
        Fail "Neither docker nor podman is on PATH. Install Docker Desktop (https://docs.docker.com/desktop/install/windows-install/) or set `$env:DOCKER."
    }
}

# `docker info` is the only reliable 'is the daemon actually up' check -- the CLI
# exists whether or not Docker Desktop is running, and the failure otherwise
# surfaces as an unreadable named-pipe error.
if (-not (Test-NativeCommand $docker @('info'))) {
    Fail "'$docker' is installed but its daemon is not reachable. Start Docker Desktop and try again."
}

# --- image ----------------------------------------------------------------
$haveImage = Test-NativeCommand $docker @('image', 'inspect', $ref)

# The image is rebuilt on every run, not just when it is missing: the entrypoint
# lives inside it, so an edit to docker/build-all.sh would otherwise go unnoticed
# and silently run the stale copy. A cached rebuild costs about a second.
if ($RebuildImage -or -not $haveImage) {
    Write-Host ">>> building image $ref (one time, a few minutes)" -ForegroundColor Cyan
    & $docker build -t $ref -t "${Image}:latest" (Join-Path $root 'docker')
    if ($LASTEXITCODE -ne 0) { Fail "image build failed" }
    Write-Host ""
} else {
    & $docker build -q -t $ref -t "${Image}:latest" (Join-Path $root 'docker') 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "refreshing the builder image failed -- rerun with -RebuildImage to see why" }
}

# --- run ------------------------------------------------------------------
# Docker Desktop accepts a Windows path on the left of the bind mount. No --user
# here: the entrypoint reads the owner off the mounted tree and becomes them,
# which is what keeps a tree shared with WSL usable from both sides. See the
# comment at the top of docker/build-all.sh.
$mount = "${root}:/work"

if ($Shell) {
    & $docker run --rm -it -v $mount -w /work --entrypoint /bin/bash $ref
    exit $LASTEXITCODE
}

$containerArgs = @('--protocol', $Protocol)
if ($NoTests)      { $containerArgs += '--no-tests' }
if ($TestsOnly)    { $containerArgs += '--tests-only' }
if ($ShowCommands) { $containerArgs += '--verbose' }
if ($Clean)        { $containerArgs = @('--clean') }

& $docker run --rm -v $mount -w /work $ref @containerArgs
exit $LASTEXITCODE
