# SPDX-License-Identifier: Apache-2.0
param(
    [ValidateSet('x86', 'x64', 'arm64', 'all')][string]$Architecture = 'x64',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateRange(1, 8)][int]$Jobs = 2,
    [string]$ContributionAsset = '',
    [switch]$Test
)
$environmentBefore = [Environment]::GetEnvironmentVariables('Process')
try {
$ErrorActionPreference = 'Stop'
$env:VSLANG = '1033'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools not found.' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ninja = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
function Invoke-TargetBuild([string]$Arch, [string]$Build, [bool]$Gui, [string]$Config=$Configuration) {
$savedEnvironment = [Environment]::GetEnvironmentVariables('Process')
try {
$hostTarget = @{ x86 = 'x64_x86'; x64 = 'x64'; arm64 = 'x64_arm64' }[$Arch]
$process = [Diagnostics.Process]::new()
$process.StartInfo.FileName = $env:ComSpec
$process.StartInfo.Arguments = "/d /u /s /c `"`"$vcvars`" $hostTarget >nul && set`""
$process.StartInfo.WorkingDirectory = $root
$process.StartInfo.UseShellExecute = $false
$process.StartInfo.RedirectStandardOutput = $true
$process.StartInfo.StandardOutputEncoding = [Text.Encoding]::Unicode
$process.StartInfo.Environment['VSCMD_SKIP_SENDTELEMETRY'] = '1'
[void]$process.Start()
$output = $process.StandardOutput.ReadToEndAsync()
if (-not $process.WaitForExit(60000)) { $process.Kill($true); $process.Dispose(); throw 'vcvarsall timed out.' }
if (-not $output.Wait(5000)) { $process.Dispose(); throw 'vcvarsall output pipe timed out.' }
$environment = $output.Result -split "`r?`n"
if ($process.ExitCode -ne 0) { throw 'vcvarsall failed.' }
$process.Dispose()
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
$guiFlag = if ($Gui) { 'ON' } else { 'OFF' }
$testFlag = if ($Gui -and (Test-Path -LiteralPath (Join-Path $root 'tests/tests.cpp'))) { 'ON' } else { 'OFF' }
if ($Test -and $Gui -and $testFlag -eq 'OFF') { throw 'Optional test sources are unavailable in this source tree.' }
$contributionFlag = if ($ContributionAsset) { 'ON' } else { 'OFF' }
$arguments = @('-S',$root,'-B',$Build,'-G','Ninja',"-DCMAKE_BUILD_TYPE=$Config","-DCMAKE_MAKE_PROGRAM=$ninja","-DAV_BUILD_GUI=$guiFlag","-DBUILD_TESTING=$testFlag","-DAV_CONTRIBUTION_ASSET=$ContributionAsset","-DAV_REQUIRE_CONTRIBUTION_ASSET=$contributionFlag")
if ($Gui) { $arguments += "-DAV_PAYLOAD_DIR=$payloads" }
& $cmake @arguments
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& $cmake --build $Build --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if ($Test -and $Gui -and ($Arch -ne 'arm64' -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64')) {
    $ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
    & $ctest --test-dir $Build --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
} finally {
    foreach ($name in [Environment]::GetEnvironmentVariables('Process').Keys) {
        if (-not $savedEnvironment.Contains($name)) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    }
    foreach ($entry in $savedEnvironment.GetEnumerator()) { [Environment]::SetEnvironmentVariable($entry.Key, [string]$entry.Value, 'Process') }
}
}
$payloads = Join-Path $root 'build/payloads/Release'
foreach ($arch in @('x86', 'x64', 'arm64')) {
    $helperBuild = Join-Path $root "build/payload-release-$arch"
    Invoke-TargetBuild $arch $helperBuild $false 'Release'
    $destination = Join-Path $payloads $arch
    & $cmake -E make_directory $destination
    if ($LASTEXITCODE -ne 0) { throw 'Payload directory failed.' }
    & $cmake -E copy_if_different (Join-Path $helperBuild 'bin/AkashaVeilWorker.exe') (Join-Path $helperBuild "bin/AkashaVeilHook-$arch.dll") $destination
    if ($LASTEXITCODE -ne 0) { throw 'Payload staging failed.' }
}
$targets = if ($Architecture -eq 'all') { @('x86','x64','arm64') } else { @($Architecture) }
$standalone = Join-Path $root $(if ($Configuration -eq 'Release') { 'build/standalone' } else { 'build/standalone-debug' })
& $cmake -E make_directory $standalone
if ($LASTEXITCODE -ne 0) { throw 'Output directory failed.' }
foreach ($arch in $targets) {
    $build = Join-Path $root "build/$($Configuration.ToLowerInvariant())-$arch"
    Invoke-TargetBuild $arch $build $true
    $output = Join-Path $standalone "AkashaVeil-$arch.exe"
    & $cmake -E copy_if_different (Join-Path $build 'bin/AkashaVeil.exe') $output
    if ($LASTEXITCODE -ne 0) { throw 'Standalone output failed.' }
    "Standalone: $output"
}
} finally {
    # Do not accumulate vcvars paths when building several architectures.
    foreach ($name in [Environment]::GetEnvironmentVariables('Process').Keys) {
        if (-not $environmentBefore.Contains($name)) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    }
    foreach ($entry in $environmentBefore.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, [string]$entry.Value, 'Process')
    }
}
