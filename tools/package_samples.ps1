<#
.SYNOPSIS
    Assemble the NV3DLib prebuilt-samples zip from already-built sample exes
    (Release-MD, x64). Requires the samples to have been built first; see
    tools/build.ps1.

.PARAMETER OutputDir
    Where to write the zip. Defaults to <repo>/out.

.PARAMETER Version
    Version stamp embedded into VERSION.txt. Zip filename is stable
    (NV3DLib-Samples.zip).
#>
[CmdletBinding()]
param(
    [string]$OutputDir,
    [string]$Version = 'dev'
)
$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
if (-not $OutputDir) { $OutputDir = Join-Path $root 'out' }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

# Sample exe output dir: bin\samples\x64\Release-MD\<name>.exe
$srcBin = Join-Path $root 'bin\samples\x64\Release-MD'
$samples = @('DX11Sample.exe','DX12Sample.exe','OGLSample.exe','VulkanSample.exe')
foreach ($s in $samples) {
    $p = Join-Path $srcBin $s
    if (-not (Test-Path $p)) {
        throw "Missing sample exe: $p`nBuild Release-MD x64 first."
    }
}

$staging = Join-Path $OutputDir 'samples-stage'
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force -Path $staging | Out-Null

foreach ($s in $samples) {
    Copy-Item (Join-Path $srcBin $s) (Join-Path $staging $s)
}
Copy-Item "$root\LICENSE" (Join-Path $staging 'LICENSE')

$readme = @'
NV3DLib sample binaries (x64, Release-MD)
=========================================

Each .exe is a self-contained test program. Plug in your NVIDIA 3D Vision
display, put on the shutter glasses, and run any of them. You should see
the test pattern in stereo (left eye = red gradient, right eye = green
gradient, with an animated white quad oscillating in depth).

If you only see one eye / no 3D, the 3D Vision driver + emitter setup is
the most likely culprit, not the lib. Check that:
  - You are running on an NVIDIA GPU with the legacy 3D Vision driver
  - The target display is in a 3D Vision-capable refresh mode
  - The 3D Vision USB emitter is connected and the glasses are paired

Source code: https://github.com/oneup03/NV3DLib (samples/ folder).
'@
$readme | Out-File -Encoding UTF8 (Join-Path $staging 'README.txt')

@"
NV3DLib samples
version: $Version
built:   $(Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz')
config:  Release-MD x64
"@ | Out-File -Encoding UTF8 (Join-Path $staging 'VERSION.txt')

$zipPath = Join-Path $OutputDir 'NV3DLib-Samples.zip'
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zipPath -CompressionLevel Optimal

Remove-Item -Recurse -Force $staging
Write-Host "Samples zip: $zipPath"
