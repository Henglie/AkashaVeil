# SPDX-License-Identifier: Apache-2.0
param([Parameter(Mandatory=$true)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-common.ps1')
$root = Split-Path -Parent $PSScriptRoot
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$manifestPath = Join-Path $PSScriptRoot 'release-manifest.json'
$manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
$record = [IO.File]::ReadAllText((Join-Path $package 'package.json')) | ConvertFrom-Json
$version = Get-ReleaseVersion $root
if ($record.version -ne $version -or $record.repository -ne $manifest.repository) { throw 'Release identity mismatch.' }
if ($record.manifestSha256 -ne (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash) { throw 'Release manifest drift.' }
$checks = 0
function Assert-Set([string[]]$Expected, [string[]]$Actual, [string]$Name) {
    if (@(Compare-Object ($Expected | Sort-Object) ($Actual | Sort-Object)).Count) { throw "Release file set mismatch: $Name" }
}
function Assert-Entries([string]$Base, $Entries, [string]$Name) {
    $expected = @($Entries | ForEach-Object { $_.path })
    $actual = @(Get-ChildItem -LiteralPath $Base -File -Recurse -Force | ForEach-Object {
        [IO.Path]::GetRelativePath($Base, $_.FullName).Replace('\', '/')
    })
    if ($Name -eq 'source') { $actual = @($actual | Where-Object { $_ -notmatch '^(\.git|build)/' }) }
    Assert-Set $expected $actual $Name
    foreach ($entry in $Entries) {
        $path = Join-Path $Base $entry.path
        if ((Get-Item -LiteralPath $path).Length -ne $entry.bytes -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Release hash mismatch: $Name/$($entry.path)" }
        $script:checks++
    }
}
function Assert-Sums([string]$Base, [string[]]$Expected) {
    $names = @()
    foreach ($line in [IO.File]::ReadAllLines((Join-Path $Base 'SHA256SUMS.txt'))) {
        if ($line -notmatch '^([A-F0-9]{64})  (.+)$') { throw 'Invalid checksum line.' }
        $hash = $Matches[1]; $name = $Matches[2]
        if ($Expected -cnotcontains $name) { throw "Unexpected checksum path: $name" }
        if ($hash -ne (Get-FileHash -LiteralPath (Join-Path $Base $name) -Algorithm SHA256).Hash) { throw "Checksum mismatch: $name" }
        $names += $name; $script:checks++
    }
    Assert-Set $Expected $names 'checksums'
}
$source = Join-Path $package 'source'
$bundle = Join-Path $package "bundle/$($record.bundleName)"
$assets = Join-Path $package 'assets'
$expectedSource = @(Get-ReleaseFileSet $root $manifest.sourceRoots)
Assert-Set $expectedSource @($record.source.path) 'source manifest'
$documents = @(Get-ReleaseFileSet $root $manifest.bundleRoots)
$executables = @($manifest.architectures | ForEach-Object { $_.file })
Assert-Set @($documents + $executables + 'SHA256SUMS.txt') @($record.bundle.path) 'bundle manifest'
$zipName = "AkashaVeil-$version-windows-all.zip"
Assert-Set @($executables + $zipName + 'SHA256SUMS.txt') @($record.assets.path) 'assets manifest'
Assert-PublicReleaseSource $source $expectedSource $manifest
Assert-Entries $source $record.source 'source'
Assert-Entries $bundle $record.bundle 'bundle'
Assert-Entries $assets $record.assets 'assets'
Assert-Sums $bundle @($record.bundle.path | Where-Object { $_ -ne 'SHA256SUMS.txt' })
Assert-Sums $assets @($record.assets.path | Where-Object { $_ -ne 'SHA256SUMS.txt' })
foreach ($arch in $manifest.architectures) {
    Assert-ReleasePE (Join-Path $assets $arch.file) $arch.machine $version
    if ((Get-FileHash -LiteralPath (Join-Path $assets $arch.file) -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $root $arch.source) -Algorithm SHA256).Hash) { throw "Built EXE drift: $($arch.name)" }
    $checks++
}
$zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $assets $zipName))
try {
    $expected = @($record.bundle | ForEach-Object { "$($record.bundleName)/$($_.path)" })
    Assert-Set $expected @($zip.Entries.FullName) 'ZIP entries'
    foreach ($entry in $zip.Entries) {
        $relative = $entry.FullName.Substring($record.bundleName.Length + 1)
        $expectedHash = ($record.bundle | Where-Object { $_.path -ceq $relative }).sha256
        $stream = $entry.Open(); $sha = [Security.Cryptography.SHA256]::Create()
        try { $hash = [Convert]::ToHexString($sha.ComputeHash($stream)) } finally { $sha.Dispose(); $stream.Dispose() }
        if ($hash -ne $expectedHash) { throw "ZIP content mismatch: $relative" }
        $checks++
    }
} finally { $zip.Dispose() }
Write-ReleaseJson (Join-Path $package 'verification.json') ([ordered]@{status='PASS'; version=$version; checks=$checks; sourceFiles=$expectedSource.Count; assets=@($record.assets)})
"Release verification PASS: version=$version checks=$checks sourceFiles=$($expectedSource.Count)"
