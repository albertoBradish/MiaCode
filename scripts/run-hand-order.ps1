param(
    [string]$Executable,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ApplicationArguments
)

$workspace = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$runtimeRoot = Join-Path $workspace 'runtime'
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $workspace 'build'))
$distRoot = [System.IO.Path]::GetFullPath((Join-Path $workspace 'dist'))

if (-not $Executable) {
    $candidates = @(
        (Join-Path $buildRoot 'windows_x64\Release\MiaCode.exe'),
        (Join-Path $buildRoot 'windows_x64\bin\Release\MiaCode.exe'),
        (Join-Path $buildRoot 'windows_x64\bin\MiaCode.exe'),
        (Join-Path $distRoot 'app\MiaCode.exe')
    )
    $Executable = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}
if (-not $Executable) {
    throw 'No hand-order executable exists in this workspace build or dist directory.'
}

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable -ErrorAction Stop).Path
$inBuild = $resolvedExecutable.StartsWith($buildRoot + [System.IO.Path]::DirectorySeparatorChar,
    [System.StringComparison]::OrdinalIgnoreCase)
$inDist = $resolvedExecutable.StartsWith($distRoot + [System.IO.Path]::DirectorySeparatorChar,
    [System.StringComparison]::OrdinalIgnoreCase)
if (-not ($inBuild -or $inDist)) {
    throw 'The executable must be inside this workspace build or dist directory.'
}

$previousRuntimeRoot = $env:MIACODE_HAND_ORDER_RUNTIME_ROOT
$previousLogDir = $env:MIACODE_LOG_DIR
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
$previousTmpDir = $env:TMPDIR
try {
    New-Item -ItemType Directory -Path $runtimeRoot -Force | Out-Null
    $runtimeTemp = Join-Path $runtimeRoot 'temp'
    New-Item -ItemType Directory -Path $runtimeTemp -Force | Out-Null
    $env:MIACODE_HAND_ORDER_RUNTIME_ROOT = $runtimeRoot
    $env:MIACODE_LOG_DIR = Join-Path $runtimeRoot 'logs'
    $env:TEMP = $runtimeTemp
    $env:TMP = $runtimeTemp
    $env:TMPDIR = $runtimeTemp
    & $resolvedExecutable @ApplicationArguments
}
finally {
    $env:MIACODE_HAND_ORDER_RUNTIME_ROOT = $previousRuntimeRoot
    $env:MIACODE_LOG_DIR = $previousLogDir
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
    $env:TMPDIR = $previousTmpDir
}
