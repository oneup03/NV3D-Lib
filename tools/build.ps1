<#
.SYNOPSIS
    Build NV3DLib.sln (or a single project) from PowerShell without needing a
    Developer Command Prompt. Used by VSCode tasks and the GitHub Actions
    workflow.

.PARAMETER Configuration
    Solution configuration: Debug-MT | Release-MT | Debug-MD | Release-MD.

.PARAMETER Platform
    Solution platform: x86 | x64. (Maps to Win32 | x64 at the project level
    automatically via the .sln configuration mapping.)

.PARAMETER Target
    MSBuild target. Defaults to Build. Use "NV3DLib" to build just the lib,
    "DX11Sample;DX12Sample;OGLSample;VulkanSample" for just the samples, etc.

.PARAMETER Clean
    Run /t:Clean instead of /t:$Target.

.EXAMPLE
    .\tools\build.ps1 -Configuration Release-MD -Platform x64
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('Debug-MT','Release-MT','Debug-MD','Release-MD')]
    [string]$Configuration,

    [Parameter(Mandatory=$true)]
    [ValidateSet('x86','x64')]
    [string]$Platform,

    [string]$Target = 'Build',

    [switch]$Clean
)
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at '$vswhere'. Install Visual Studio 2022 (Community / Build Tools) with the C++ workload."
}

$msbuild = & $vswhere -latest -prerelease `
    -requires Microsoft.Component.MSBuild `
    -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1

if (-not $msbuild -or -not (Test-Path $msbuild)) {
    throw "MSBuild.exe not located via vswhere. Ensure the C++ build tools workload is installed."
}

$solutionDir = Split-Path -Parent $PSScriptRoot
$solution    = Join-Path $solutionDir 'NV3DLib.sln'
if (-not (Test-Path $solution)) { throw "Solution not found: $solution" }

$effectiveTarget = if ($Clean) { 'Clean' } else { $Target }

Write-Host "MSBuild   : $msbuild"
Write-Host "Solution  : $solution"
Write-Host "Target    : $effectiveTarget"
Write-Host "Config    : $Configuration|$Platform"
Write-Host ""

& $msbuild $solution `
    "/t:$effectiveTarget" `
    "/p:Configuration=$Configuration" `
    "/p:Platform=$Platform" `
    /m /nologo /v:minimal /clp:Summary

if ($LASTEXITCODE -ne 0) {
    throw "Build failed (exit $LASTEXITCODE) for $Configuration|$Platform (target=$effectiveTarget)."
}
