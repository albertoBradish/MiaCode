param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ApplicationArguments
)

$packageRoot = $PSScriptRoot
$workspace = [System.IO.Path]::GetFullPath((Join-Path $packageRoot '..\..'))
if (-not (Test-Path -LiteralPath (Join-Path $workspace 'baseline.json') -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $workspace 'source\AGENTS.md') -PathType Leaf)) {
    throw 'This development package must stay inside the hand-order workspace dist directory.'
}

$executable = Join-Path $packageRoot 'app\MiaCode.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Packaged application is missing: $executable"
}
$runtimeRoot = Join-Path $workspace 'runtime'
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
