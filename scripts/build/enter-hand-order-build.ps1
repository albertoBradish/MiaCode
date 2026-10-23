# Dot-source this script from PowerShell before cmake --preset hand_order.
$workspace = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsInstall = & $vswhere -latest -products * -property installationPath | Select-Object -First 1
if (-not $vsInstall) { throw 'Visual Studio 2022 was not found.' }

Import-Module (Join-Path $vsInstall 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

# This machine has the SDK files, but VsDevShell does not register them.
$sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkVersion = '10.0.22621.0'
$sdkBin = Join-Path $sdk "bin\$sdkVersion\x64"
$sdkIncludes = @('ucrt', 'shared', 'um', 'winrt', 'cppwinrt') |
    ForEach-Object { Join-Path $sdk "Include\$sdkVersion\$_" }
$sdkLibraries = @('ucrt', 'um') |
    ForEach-Object { Join-Path $sdk "Lib\$sdkVersion\$_\x64" }
$requiredSdkPaths = @($sdkBin) + $sdkIncludes + $sdkLibraries + @(
    (Join-Path $sdkBin 'rc.exe'), (Join-Path $sdkBin 'mt.exe'))
foreach ($required in $requiredSdkPaths) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Windows SDK component is missing: $required"
    }
}
$env:INCLUDE = ($sdkIncludes -join ';') + ';' + $env:INCLUDE
$env:LIB = ($sdkLibraries -join ';') + ';' + $env:LIB
$env:PATH = $sdkBin + ';' + $env:PATH

$qtRoot = Join-Path $workspace 'toolchains\qt\6.11.1\msvc2022_64'
if (-not (Test-Path -LiteralPath (Join-Path $qtRoot 'lib\cmake\Qt6\Qt6Config.cmake'))) {
    throw "Qt SDK is missing: $qtRoot"
}
$runtimeRoot = Join-Path $workspace 'runtime'
$runtimeTemp = Join-Path $runtimeRoot 'temp'
New-Item -ItemType Directory -Path $runtimeTemp -Force | Out-Null
$env:MIACODE_HAND_ORDER_QT_ROOT = $qtRoot
$env:MIACODE_HAND_ORDER_RUNTIME_ROOT = $runtimeRoot
$env:TEMP = $runtimeTemp
$env:TMP = $runtimeTemp
$env:TMPDIR = $runtimeTemp
