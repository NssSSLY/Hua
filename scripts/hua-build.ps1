param(
    [Parameter(Mandatory, Position = 0)][string]$Source,
    [Parameter(Mandatory, Position = 1)][string]$Output,
    [string]$Compiler
)

$ErrorActionPreference = 'Stop'
$sdkRoot = Split-Path -Parent $PSScriptRoot
$hua = Join-Path $PSScriptRoot 'hua.exe'
if (!$Compiler) {
    $Compiler = Join-Path $sdkRoot 'lib\bootstrap\compiler.huab'
}
foreach ($dependency in @($hua, (Join-Path $PSScriptRoot 'wasmtime.dll'), $Compiler)) {
    if (!(Test-Path -LiteralPath $dependency -PathType Leaf)) {
        throw "Hua SDK dependency missing: $dependency"
    }
}

$sourceFile = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Source)
$outputFile = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
$compilerFile = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Compiler)
$previousSdk = $env:HUA_HOME
try {
    $env:HUA_HOME = $sdkRoot
    $receiptText = & $hua run $compilerFile --execution-profile compiler '--' $sourceFile $outputFile
} finally {
    $env:HUA_HOME = $previousSdk
}
if ($LASTEXITCODE -ne 0) {
    throw "Hua compiler runtime failed (exit $LASTEXITCODE)."
}
$receipt = $receiptText | ConvertFrom-Json
if ($receipt.schema -ne 'hua.archive.result.v1' -or $receipt.status -ne 'ok') {
    throw "Hua compilation failed: $receiptText"
}
Write-Output $receiptText
