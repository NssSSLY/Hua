param(
    [string]$BuildDirectory = 'build',
    [switch]$PythonBridge,
    [string]$PythonHome = ''
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskCompiler = Join-Path $taskRoot '.tools\llvm-mingw-20260922-ucrt-x86_64\bin\clang++.exe'
$taskSdkBin = Join-Path $env:LOCALAPPDATA 'Android\Sdk\cmake\3.22.1\bin'
$taskCmake = Join-Path $taskSdkBin 'cmake.exe'
$taskNinja = Join-Path $taskSdkBin 'ninja.exe'
if (!(Test-Path -LiteralPath $taskCompiler) -or !(Test-Path -LiteralPath $taskCmake) -or !(Test-Path -LiteralPath $taskNinja)) {
    throw 'Local portable tools are missing. Use a C++20 toolchain and the standard CMake commands in README.md.'
}
$taskBuild = Join-Path $taskRoot $BuildDirectory
$taskCompiler = $taskCompiler.Replace('\', '/')
$taskNinja = $taskNinja.Replace('\', '/')
$taskBridgeOption = if ($PythonBridge) { 'ON' } else { 'OFF' }
$taskPythonOptions = @()
if ($PythonHome) { $taskPythonOptions += "-DPython3_ROOT_DIR=$($PythonHome.Replace('\', '/'))" }
& $taskCmake -S $taskRoot -B $taskBuild -G Ninja "-DCMAKE_MAKE_PROGRAM=$taskNinja" "-DCMAKE_CXX_COMPILER=$taskCompiler" -DCMAKE_BUILD_TYPE=Release "-DHUA_ENABLE_PYTHON_BRIDGE=$taskBridgeOption" @taskPythonOptions
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
& $taskCmake --build $taskBuild
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& (Join-Path $taskSdkBin 'ctest.exe') --test-dir $taskBuild --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
