# SPDX-License-Identifier: Apache-2.0
Set-StrictMode -Version Latest

function Get-ReleaseVersion([string]$Root) {
    $value = [IO.File]::ReadAllText((Join-Path $Root 'CMakeLists.txt'))
    $match = [regex]::Match($value, 'project\s*\(\s*AkashaVeil\s+VERSION\s+(\d+\.\d+\.\d+)')
    if (-not $match.Success) { throw 'CMake project version is missing.' }
    return $match.Groups[1].Value
}

function Ensure-ReleaseDirectory([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Container) { return }
    $parent = Split-Path -Parent $Path
    if (-not $parent -or $parent -eq $Path) { throw "Invalid directory: $Path" }
    Ensure-ReleaseDirectory $parent
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) { throw "Missing parent: $parent" }
    [void](New-Item -ItemType Directory -Path $Path)
}

function Get-ReleaseFileSet([string]$Root, [string[]]$Roots) {
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $result = [Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
    foreach ($entry in $Roots) {
        $path = [IO.Path]::GetFullPath((Join-Path $fullRoot $entry))
        if (-not $path.StartsWith($fullRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Manifest path escapes source root: $entry"
        }
        if (-not (Test-Path -LiteralPath $path)) { throw "Required release input is missing: $entry" }
        $item = Get-Item -LiteralPath $path
        $files = if ($item.PSIsContainer) { @(Get-ChildItem -LiteralPath $path -File -Recurse -Force) } else { @($item) }
        foreach ($file in $files) {
            if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Release input is a link: $($file.Name)" }
            [void]$result.Add([IO.Path]::GetRelativePath($fullRoot, $file.FullName).Replace('\', '/'))
        }
    }
    return @($result)
}

function Assert-PublicReleaseSource([string]$Root, [string[]]$Files, $Manifest) {
    foreach ($relative in $Files) {
        foreach ($pattern in $Manifest.forbiddenSourcePaths) {
            if ($relative -match $pattern) { throw "Forbidden public source path: $relative" }
        }
        $path = Join-Path $Root $relative
        if ([IO.Path]::GetExtension($relative) -in @('.png', '.ico')) { continue }
        $text = [IO.File]::ReadAllText($path)
        foreach ($pattern in $Manifest.forbiddenTextPatterns) {
            if ($relative -eq 'tools/release-manifest.json') { continue }
            if ($text -match $pattern) { throw "Public source content check failed: $relative" }
        }
    }
}

function Copy-ReleaseFile([string]$Source, [string]$Target) {
    Ensure-ReleaseDirectory (Split-Path -Parent $Target)
    Copy-Item -LiteralPath $Source -Destination $Target
    if ((Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash) { throw "Copy verification failed: $Target" }
}

function Get-ReleaseEntries([string]$Root, [string[]]$Files) {
    return @($Files | ForEach-Object {
        $path = Join-Path $Root $_
        [ordered]@{path=$_; bytes=(Get-Item -LiteralPath $path).Length; sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
    })
}

function Write-ReleaseJson([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, (($Value | ConvertTo-Json -Depth 12) + "`n"), [Text.UTF8Encoding]::new($false))
}

function Write-ReleaseSums([string]$Root, [string[]]$Files) {
    $lines = @($Files | Sort-Object | ForEach-Object { "{0}  {1}" -f (Get-FileHash -LiteralPath (Join-Path $Root $_) -Algorithm SHA256).Hash, $_ })
    [IO.File]::WriteAllText((Join-Path $Root 'SHA256SUMS.txt'), (($lines -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
}

function Assert-ReleasePE([string]$Path, [int]$Machine, [string]$Version) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'Missing DOS header.' }
        $stream.Position = 0x3C
        $offset = $reader.ReadInt32()
        if ($offset -lt 64 -or $offset -gt $stream.Length - 24) { throw 'Invalid PE offset.' }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne $Machine) { throw "PE architecture mismatch: $Path" }
    } finally { $reader.Dispose(); $stream.Dispose() }
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
    if ($info.FileVersion -ne $Version -or $info.ProductVersion -ne $Version) { throw "PE version mismatch: $Path" }
}
