<#
.SYNOPSIS
    Assemble the NV3DLib SDK zip from already-built outputs.

    Layout produced inside the zip:

        include/NV3D.hpp
        lib/x86/Release-MT/NV3DLib-mt.lib
        lib/x86/Release-MD/NV3DLib-md.lib
        lib/x64/Release-MT/NV3DLib-mt.lib
        lib/x64/Release-MD/NV3DLib-md.lib
        external/nvapi/*.h
        external/nvapi/License.txt
        external/nvapi/amd64/nvapi64.lib
        external/nvapi/x86/nvapi.lib
        LICENSE
        README.md
        VERSION.txt

    Requires all 4 Release configs to have been built first (Release-MT and
    Release-MD, x86 and x64). Run tools/build.ps1 -Configuration Release-MD
    -Platform x64 etc., or trigger the GitHub workflow.

.PARAMETER OutputDir
    Where to write the zip. Defaults to <repo>/out.

.PARAMETER Version
    Version stamp embedded into VERSION.txt. Zip filename stays stable
    (NV3DLib-SDK.zip) so the rolling pre-release replaces old assets cleanly.
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

# NV3DLib outputs land in NV3DLib\lib\x{32,64}\<Config>\NV3DLib-<rt>.lib
# (PlatformArchitecture = 32 for Win32, 64 for x64; that's the directory name
# the .vcxproj uses).
$libs = @(
    @{ from = "$root\NV3DLib\lib\x32\Release-MT\NV3DLib-mt.lib"; to = 'lib\x86\Release-MT\NV3DLib-mt.lib' },
    @{ from = "$root\NV3DLib\lib\x32\Release-MD\NV3DLib-md.lib"; to = 'lib\x86\Release-MD\NV3DLib-md.lib' },
    @{ from = "$root\NV3DLib\lib\x64\Release-MT\NV3DLib-mt.lib"; to = 'lib\x64\Release-MT\NV3DLib-mt.lib' },
    @{ from = "$root\NV3DLib\lib\x64\Release-MD\NV3DLib-md.lib"; to = 'lib\x64\Release-MD\NV3DLib-md.lib' }
)
foreach ($l in $libs) {
    if (-not (Test-Path $l.from)) {
        throw "Missing build output: $($l.from)`nBuild all 4 Release configs first."
    }
}

$staging = Join-Path $OutputDir 'sdk-stage'
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force -Path $staging | Out-Null

# Public header
New-Item -ItemType Directory -Force -Path (Join-Path $staging 'include') | Out-Null
Copy-Item "$root\NV3DLib\include\NV3D.hpp" (Join-Path $staging 'include\NV3D.hpp')

# Libs
foreach ($l in $libs) {
    $dest = Join-Path $staging $l.to
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
    Copy-Item $l.from $dest
}

# NVAPI (headers + per-arch import libs + license). We only ship the
# top-level *.h files NV3DLib's public header chain needs — not the
# Sample_Code or docs subtrees.
$nvapiSrc = "$root\external\nvapi"
$nvapiDst = Join-Path $staging 'external\nvapi'
New-Item -ItemType Directory -Force -Path (Join-Path $nvapiDst 'amd64') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $nvapiDst 'x86')   | Out-Null
Copy-Item (Join-Path $nvapiSrc '*.h')            -Destination $nvapiDst
Copy-Item (Join-Path $nvapiSrc 'License.txt')    -Destination $nvapiDst
Copy-Item (Join-Path $nvapiSrc 'amd64\nvapi64.lib') -Destination (Join-Path $nvapiDst 'amd64\nvapi64.lib')
Copy-Item (Join-Path $nvapiSrc 'x86\nvapi.lib')     -Destination (Join-Path $nvapiDst 'x86\nvapi.lib')

# Top-level docs
Copy-Item "$root\LICENSE"   (Join-Path $staging 'LICENSE')
Copy-Item "$root\README.md" (Join-Path $staging 'README.md')

# Version stamp so a downloaded zip can be traced back to a commit
@"
NV3DLib SDK
version: $Version
built:   $(Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz')
"@ | Out-File -Encoding UTF8 (Join-Path $staging 'VERSION.txt')

$zipPath = Join-Path $OutputDir 'NV3DLib-SDK.zip'
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $zipPath -CompressionLevel Optimal

Remove-Item -Recurse -Force $staging
Write-Host "SDK zip: $zipPath"
