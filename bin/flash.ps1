<#
.SYNOPSIS
  Flash the firmware that is ALREADY built, without going through PlatformIO.

.DESCRIPTION
  `pio run -t upload` spends a minute or two re-scanning the project and the
  library dependency graph before it reaches esptool. That is wasted whenever
  the binary has not changed -- most often when a long build finished while the
  device was asleep, the upload failed for want of a port, and all you want is
  to power the device on and push the same image again.

  This calls esptool directly on the artifacts already in .pio\build\<env>, so
  it starts writing immediately.

  NOTE: it writes the same four images at the same four offsets as a PlatformIO
  upload, which deliberately leaves the gap at 0x9000 alone -- that is the NVS
  partition, where settings live. Do NOT "simplify" this by flashing
  firmware.factory.bin at 0x0: that image spans 0x0-0x10000 and pads the NVS
  window with 0xFF, so it would wipe the user's settings on every flash.

.EXAMPLE
  .\bin\flash.ps1
.EXAMPLE
  .\bin\flash.ps1 -Environment x4pro -Port COM6
.EXAMPLE
  .\bin\flash.ps1 -AppOnly -Wait 0
.EXAMPLE
  .\bin\flash.ps1 -f C:\tmp\firmware-run3.bin
  Flash that file as the app image instead of .pio\build\<env>\firmware.bin; the bootloader and
  partition table still come from the environment's build dir if it has one, else the app alone.
#>
[CmdletBinding()]
param(
  [Alias('e')][string]$Environment = 'default',
  # Falls back to $env:FLASH_PORT, then to esptool's own auto-detection.
  [Alias('p')][string]$Port = $env:FLASH_PORT,
  # An app image to flash instead of .pio\build\<env>\firmware.bin.
  [Alias('f')][string]$Firmware = '',
  # Seconds to keep retrying while the device is still asleep. 0 = fail at once.
  [Alias('w')][int]$Wait = 120,
  [Alias('a')][switch]$AppOnly,
  [Alias('n')][switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot ".pio\build\$Environment"

# PlatformIO's core directory holds both esptool and the framework blob. The
# local config pins it to C:\pio on Windows to stay under the command-line
# length limit, so honour that before falling back to the default location.
if ($env:PLATFORMIO_CORE_DIR)   { $pioCore = $env:PLATFORMIO_CORE_DIR }
elseif (Test-Path 'C:\pio')     { $pioCore = 'C:\pio' }
else                            { $pioCore = Join-Path $HOME '.platformio' }

$esptool = Join-Path $pioCore 'penv\Scripts\esptool.exe'
if (-not (Test-Path $esptool)) {
  throw "flash: no esptool at $esptool -- is PlatformIO installed?"
}

if ($Firmware) {
  if (-not (Test-Path $Firmware)) {
    Write-Error "flash: $Firmware does not exist."
    exit 1
  }
  $app = (Resolve-Path $Firmware).Path
  # The app image is the caller's; the bootloader and partition table still have to come from a
  # build of the same environment. Without one, write the app alone rather than guess.
  if (-not $AppOnly -and -not ((Test-Path (Join-Path $buildDir 'bootloader.bin')) -and (Test-Path (Join-Path $buildDir 'partitions.bin')))) {
    Write-Warning "flash: no bootloader/partitions for environment '$Environment' in .pio\build; flashing the app only."
    $AppOnly = $true
  }
} else {
  $app = Join-Path $buildDir 'firmware.bin'
  if (-not (Test-Path $app)) {
    Write-Error "flash: $app does not exist. Nothing has been built for environment '$Environment' yet; run a normal build first."
    exit 1
  }
}

# This tool exists to skip the build, so it has to say plainly what it is about
# to write -- otherwise a stale image gets flashed and debugged as a live one.
$appItem = Get-Item $app
$ageMin = [int]((Get-Date) - $appItem.LastWriteTime).TotalMinutes
$appLabel = if ($Firmware) { $app } else { 'firmware.bin' }
Write-Host "flash: $Environment $appLabel, built $ageMin min ago ($($appItem.LastWriteTime))"
$newer = Get-ChildItem -Path (Join-Path $repoRoot 'src'), (Join-Path $repoRoot 'lib') -Recurse -Include *.cpp, *.h -ErrorAction SilentlyContinue |
         Where-Object { $_.LastWriteTime -gt $appItem.LastWriteTime } | Select-Object -First 1
if ($newer) {
  Write-Warning 'flash: sources under src/ or lib/ are newer than this binary.'
  Write-Warning 'flash: you are about to flash a build that predates your edits.'
}

# --chip auto plus keep/keep: write exactly the image that was built. Hardcoding
# esp32c3 / dio would be wrong for the S3 boards and for the env at
# platformio.ini:394, which builds qio -- and esptool REWRITES the bootloader
# header from these flags when it writes offset 0x0, so a wrong value here
# corrupts the image rather than merely being ignored. esptool still refuses an
# image whose header does not match the chip it finds, so auto cannot mis-flash.
# --flash-freq is omitted deliberately: esptool already defaults it to keep.
$espArgs = @('--chip', 'auto', '--baud', '921600', '--before', 'default-reset', '--after', 'hard-reset')
if ($Port) { $espArgs += @('--port', $Port) }

# Same offsets a PlatformIO upload uses, verified against firmware.factory.bin:
# bootloader 0x0, partition table 0x8000, otadata 0xe000, app 0x10000.
$images = @('0x10000', $app)
if (-not $AppOnly) {
  $bootApp0 = Join-Path $pioCore 'packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'
  if (-not (Test-Path $bootApp0)) {
    Write-Warning 'flash: boot_app0.bin not found in the framework package; flashing the app only.'
  } else {
    $images = @('0x0',     (Join-Path $buildDir 'bootloader.bin'),
                '0x8000',  (Join-Path $buildDir 'partitions.bin'),
                '0xe000',  $bootApp0,
                '0x10000', $app)
  }
}

$espArgs += @('write-flash', '--flash-mode', 'keep', '--flash-size', 'keep') + $images

if ($DryRun) {
  Write-Host "$esptool $($espArgs -join ' ')"
  exit 0
}

# Retry rather than fail: the whole point is that the device is usually asleep
# when you get here. Start the script, then wake the device -- it goes as soon
# as the port appears.
$deadline = (Get-Date).AddSeconds($Wait)
$attempt = 0
while ($true) {
  $attempt++
  & $esptool @espArgs
  if ($LASTEXITCODE -eq 0) {
    Write-Host 'flash: done.'
    exit 0
  }
  if ((Get-Date) -ge $deadline) {
    Write-Error "flash: giving up after $attempt attempt(s). If the device is awake and still not found, check the port with 'pio device list'."
    exit 1
  }
  $left = [int]($deadline - (Get-Date)).TotalSeconds
  Write-Host "flash: device not ready -- power it on; retrying (attempt $attempt, ${left}s left)..."
  Start-Sleep -Seconds 3
}
