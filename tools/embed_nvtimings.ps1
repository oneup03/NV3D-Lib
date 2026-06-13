# Converts nvtimings.json into a C++ header containing a byte array. Run as
# a pre-build step on NV3DLib so consumers don't need to ship nvtimings.json
# separately -- the JSON is baked into the static lib.
#
# Skips regeneration if the output file is newer than the input.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)] [string] $InputJson,
    [Parameter(Mandatory=$true)] [string] $OutputHeader
)

if (-not (Test-Path $InputJson)) {
    Write-Error "embed_nvtimings: input not found: $InputJson"
    exit 1
}

if ((Test-Path $OutputHeader) -and
    ((Get-Item $OutputHeader).LastWriteTime -ge (Get-Item $InputJson).LastWriteTime)) {
    Write-Host "embed_nvtimings: $(Split-Path -Leaf $OutputHeader) up to date"
    exit 0
}

$bytes = [System.IO.File]::ReadAllBytes($InputJson)

$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("// Auto-generated from nvtimings.json by tools/embed_nvtimings.ps1.")
[void]$sb.AppendLine("// Do not edit -- regenerated whenever nvtimings.json changes.")
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("#include <cstddef>")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("namespace NV3D {")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("inline constexpr size_t kNvTimingsJsonSize = $($bytes.Length);")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("inline constexpr unsigned char kNvTimingsJson[] = {")

$lineBuf = [System.Text.StringBuilder]::new()
for ($i = 0; $i -lt $bytes.Length; $i++) {
    [void]$lineBuf.Append(("0x{0:x2}," -f $bytes[$i]))
    if (($i + 1) % 16 -eq 0) {
        [void]$sb.AppendLine($lineBuf.ToString())
        [void]$lineBuf.Clear()
    }
}
if ($lineBuf.Length -gt 0) {
    [void]$sb.AppendLine($lineBuf.ToString())
}

[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("}  // namespace NV3D")

$outDir = Split-Path -Parent $OutputHeader
if (-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}
[System.IO.File]::WriteAllText($OutputHeader, $sb.ToString())
Write-Host "embed_nvtimings: wrote $OutputHeader ($($bytes.Length) bytes embedded)"
