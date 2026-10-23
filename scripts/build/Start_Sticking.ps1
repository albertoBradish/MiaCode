param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [AllowEmptyString()]
    [AllowEmptyCollection()]
    [string[]]$ApplicationArguments
)

function ConvertTo-WindowsCommandLineArgument {
    param([AllowEmptyString()][string]$Argument)

    # ProcessStartInfo.Arguments is available in Windows PowerShell 5.1.
    # Quote each argument using the Windows CRT rules, without a shell.
    $quoted = [System.Text.StringBuilder]::new()
    [void]$quoted.Append([char]34)
    $backslashes = 0
    foreach ($character in $Argument.ToCharArray()) {
        if ($character -eq [char]92) {
            ++$backslashes
            continue
        }
        if ($character -eq [char]34) {
            [void]$quoted.Append([char]92, 2 * $backslashes + 1)
        } else {
            [void]$quoted.Append([char]92, $backslashes)
        }
        [void]$quoted.Append($character)
        $backslashes = 0
    }
    # Backslashes immediately before the closing quote must be doubled.
    [void]$quoted.Append([char]92, 2 * $backslashes)
    [void]$quoted.Append([char]34)
    return $quoted.ToString()
}

$packageRoot = $PSScriptRoot
if (-not (Test-Path -LiteralPath (Join-Path $packageRoot 'StickingPackage.marker') -PathType Leaf)) {
    throw 'Sticking package marker is missing.'
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
$start.EnvironmentVariables['MIACODE_HAND_ORDER_RUNTIME_ROOT'] = $runtimeRoot
$start.EnvironmentVariables['MIACODE_LOG_DIR'] = Join-Path $runtimeRoot 'logs'
$start.EnvironmentVariables['TEMP'] = $runtimeTemp
$start.EnvironmentVariables['TMP'] = $runtimeTemp
$start.EnvironmentVariables['TMPDIR'] = $runtimeTemp
$quotedArguments = foreach ($argument in $ApplicationArguments) {
    ConvertTo-WindowsCommandLineArgument -Argument $argument
}
$start.Arguments = $quotedArguments -join ' '
$process = [System.Diagnostics.Process]::Start($start)
$process.WaitForExit()
exit $process.ExitCode
