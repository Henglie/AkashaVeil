# SPDX-License-Identifier: Apache-2.0
param([string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-common.ps1')
$root = Split-Path -Parent $PSScriptRoot
$manifestPath = Join-Path $PSScriptRoot 'release-manifest.json'
$manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
$version = Get-ReleaseVersion $root
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root "dist/release-$version" }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw "Output already exists; choose a fresh directory: $output" }
$files = @(Get-ReleaseFileSet $root $manifest.sourceRoots)
Assert-PublicReleaseSource $root $files $manifest
$documents = @(Get-ReleaseFileSet $root $manifest.bundleRoots)
foreach ($arch in $manifest.architectures) {
    Assert-ReleasePE (Join-Path $root $arch.source) $arch.machine $version
}
Ensure-ReleaseDirectory $output
$source = Join-Path $output 'source'
$bundleName = "AkashaVeil-$version"
$bundle = Join-Path $output "bundle/$bundleName"
$assets = Join-Path $output 'assets'
foreach ($dir in @($source, $bundle, $assets)) { Ensure-ReleaseDirectory $dir }
foreach ($file in $files) { Copy-ReleaseFile (Join-Path $root $file) (Join-Path $source $file) }
foreach ($file in $documents) { Copy-ReleaseFile (Join-Path $root $file) (Join-Path $bundle $file) }
$bundleFiles = @($documents)
$assetFiles = @()
foreach ($arch in $manifest.architectures) {
    $input = Join-Path $root $arch.source
    Copy-ReleaseFile $input (Join-Path $bundle $arch.file)
    Copy-ReleaseFile $input (Join-Path $assets $arch.file)
    $bundleFiles += $arch.file
    $assetFiles += $arch.file
}
Write-ReleaseSums $bundle $bundleFiles
$bundleFiles += 'SHA256SUMS.txt'
$zipName = "AkashaVeil-$version-windows-all.zip"
Add-Type -AssemblyName System.IO.Compression
$zip = [IO.Compression.ZipFile]::Open((Join-Path $assets $zipName), [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in ($bundleFiles | Sort-Object)) {
        $entry = $zip.CreateEntry("$bundleName/$file", [IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = [DateTimeOffset]::new(2000, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
        $input = [IO.File]::OpenRead((Join-Path $bundle $file))
        $target = $entry.Open()
        try { $input.CopyTo($target) } finally { $target.Dispose(); $input.Dispose() }
    }
} finally { $zip.Dispose() }
$assetFiles += $zipName
Write-ReleaseSums $assets $assetFiles
$assetFiles += 'SHA256SUMS.txt'
$record = [ordered]@{
    schema=1; version=$version; repository=$manifest.repository; bundleName=$bundleName
    manifestSha256=(Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
    source=@(Get-ReleaseEntries $source $files)
    bundle=@(Get-ReleaseEntries $bundle $bundleFiles)
    assets=@(Get-ReleaseEntries $assets $assetFiles)
}
Write-ReleaseJson (Join-Path $output 'package.json') $record
& (Join-Path $PSScriptRoot 'verify-release.ps1') -PackageDirectory $output
"Packaged $version; public source files=$($files.Count); assets=$assets"
