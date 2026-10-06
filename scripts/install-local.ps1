param(
    [string]$BuildDirectory = 'build-python-off',
    [string]$Destination = (Join-Path $env:LOCALAPPDATA 'Hua\bin'),
    [switch]$AddToUserPath
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskBuild = if ([IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory } else { Join-Path $taskRoot $BuildDirectory }
$taskBuild = [IO.Path]::GetFullPath($taskBuild)
$taskBin = [IO.Path]::GetFullPath($Destination)
if ($taskBin.TrimEnd('\') -eq $taskBuild.TrimEnd('\')) { throw 'Choose a destination separate from the build directory.' }
$taskFiles = @('hua.exe', 'wasmtime.dll', 'wasmtime-LICENSE.txt')
foreach ($taskFile in $taskFiles) {
    if (!(Test-Path -LiteralPath (Join-Path $taskBuild $taskFile) -PathType Leaf)) { throw "Missing runtime file: $taskFile. Build Hua first." }
}
New-Item -ItemType Directory -Path $taskBin -Force | Out-Null
foreach ($taskFile in $taskFiles) { Copy-Item -LiteralPath (Join-Path $taskBuild $taskFile) -Destination (Join-Path $taskBin $taskFile) -Force }
Copy-Item -LiteralPath (Join-Path $taskRoot 'LICENSE') -Destination (Join-Path $taskBin 'Hua-LICENSE.txt') -Force
$taskExe = Join-Path $taskBin 'hua.exe'
& $taskExe version
if ($LASTEXITCODE -ne 0) { throw 'Installed runtime cannot start.' }
# This PowerShell session is ready immediately. Other terminals need to be reopened.
$env:HUA_HOME = Split-Path -Parent $taskBin
$env:PATH = $taskBin + ';' + $env:PATH
if ($AddToUserPath) {
    $taskOldPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $taskEntries = @($taskOldPath -split ';' | Where-Object { $_ -ne '' })
    $taskAlreadyPresent = @($taskEntries | Where-Object { $_.TrimEnd('\') -ieq $taskBin.TrimEnd('\') }).Count -gt 0
    if (!$taskAlreadyPresent) {
        $taskNewPath = if ([string]::IsNullOrEmpty($taskOldPath)) { $taskBin } else { $taskOldPath.TrimEnd(';') + ';' + $taskBin }
        [Environment]::SetEnvironmentVariable('Path', $taskNewPath, 'User')
    }
    [Environment]::SetEnvironmentVariable('HUA_HOME', $env:HUA_HOME, 'User')
    Write-Host 'User PATH configured. Reopen terminals and VS Code to see it.'
}
Write-Host "Hua installed in $taskBin"
Write-Host 'HUA_HOME identifies the installation; it does not change module import lookup.'
