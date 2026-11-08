param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ApplicationArguments
)

$packageRoot = $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $packageRoot 'HandOrderPackage.marker') -PathType Leaf)) {
    throw 'Hand-order package marker is missing.'
}

$executable = Join-Path $packageRoot 'app\MiaCode.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Packaged application is missing: $executable"
}
$runtimeRoot = Join-Path $packageRoot 'runtime'
$runtimeTemp = Join-Path $runtimeRoot 'temp'
New-Item -ItemType Directory -Path $runtimeTemp -Force | Out-Null

$start = [System.Diagnostics.ProcessStartInfo]::new($executable)
$start.UseShellExecute = $false
$start.WorkingDirectory = $packageRoot
$start.Environment['MIACODE_HAND_ORDER_RUNTIME_ROOT'] = $runtimeRoot
$start.Environment['MIACODE_LOG_DIR'] = Join-Path $runtimeRoot 'logs'
$start.Environment['TEMP'] = $runtimeTemp
$start.Environment['TMP'] = $runtimeTemp
$start.Environment['TMPDIR'] = $runtimeTemp
foreach ($argument in $ApplicationArguments) {
    $start.ArgumentList.Add($argument)
}
$process = [System.Diagnostics.Process]::Start($start)
$process.WaitForExit()
exit $process.ExitCode
