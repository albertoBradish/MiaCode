param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDir
)

$ErrorActionPreference = 'Stop'
$workspace = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$distRoot = [System.IO.Path]::GetFullPath((Join-Path $workspace 'dist'))
$exportsRoot = [System.IO.Path]::GetFullPath((Join-Path $workspace 'exports'))
$testRoot = [System.IO.Path]::GetFullPath('D:\Maimai\MiaCode_HandOrder_Test')
$packageRoot = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $PackageDir).Path)

function Assert-ChildPath {
    param([string]$Path, [string]$Parent, [string]$Label)
    if (!$Path.StartsWith($Parent + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must be inside $Parent : $Path"
    }
}

function Assert-PlainDirectory {
    param([string]$Path)
    if (!(Test-Path -LiteralPath $Path -PathType Container)) { throw "Directory missing: $Path" }
    $entry = Get-Item -LiteralPath $Path -Force
    if ($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        throw "Junctions and symlinks are not accepted: $Path"
    }
    $links = @(Get-ChildItem -LiteralPath $Path -Recurse -Force -Attributes ReparsePoint)
    if ($links.Count -gt 0) { throw "Package or test directory contains a junction or symlink: $($links[0].FullName)" }
}

Assert-ChildPath -Path $packageRoot -Parent $distRoot -Label 'PackageDir'
Assert-PlainDirectory -Path $packageRoot
foreach ($required in @('HandOrderPackage.marker', 'MiaCode.exe', 'app\MiaCode.exe', 'README_HAND_ORDER_ZH.md')) {
    if (!(Test-Path -LiteralPath (Join-Path $packageRoot $required) -PathType Leaf)) {
        throw "Incomplete hand-order package: $required"
    }
}
$identity = Get-Content -LiteralPath (Join-Path $packageRoot 'README_HAND_ORDER_ZH.md') -Raw
if ($identity -notmatch '(?m)^- Git dirty: 0\s*$' -or $identity -notmatch '(?m)^- Git revision: [0-9a-f]{12}\s*$') {
    throw 'Package lacks a clean, recorded Git build identity.'
}

if (Test-Path -LiteralPath $testRoot) { Assert-PlainDirectory -Path $testRoot }
$running = @(Get-CimInstance Win32_Process -Filter "name = 'MiaCode.exe'" | Where-Object {
    $_.ExecutablePath -and $_.ExecutablePath.StartsWith(
        $testRoot + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)
})
if ($running.Count -gt 0) { throw "Close the test installation before updating it (PID $($running[0].ProcessId))." }

New-Item -ItemType Directory -Path $exportsRoot -Force | Out-Null
$stamp = (Get-Date).ToString('yyyyMMdd_HHmmss')
$stage = [System.IO.Path]::GetFullPath((Join-Path $exportsRoot ("hand_order_test_stage_${stamp}_" + [guid]::NewGuid().ToString('N'))))
Assert-ChildPath -Path $stage -Parent $exportsRoot -Label 'Stage'
New-Item -ItemType Directory -Path $stage | Out-Null
foreach ($entry in Get-ChildItem -LiteralPath $packageRoot -Force) {
    Copy-Item -LiteralPath $entry.FullName -Destination $stage -Recurse -Force
}

# Once the test installation has a stable root, retain its isolated data on updates.
$hasStableInstall = (Test-Path -LiteralPath (Join-Path $testRoot 'HandOrderPackage.marker') -PathType Leaf)
if ($hasStableInstall) {
    foreach ($name in @('runtime', 'exports', 'logs')) {
        $oldData = Join-Path $testRoot $name
        $newData = Join-Path $stage $name
        if (!(Test-Path -LiteralPath $oldData -PathType Container)) { continue }
        New-Item -ItemType Directory -Path $newData -Force | Out-Null
        foreach ($entry in Get-ChildItem -LiteralPath $oldData -Force) {
            Copy-Item -LiteralPath $entry.FullName -Destination $newData -Recurse -Force
        }
    }
}

$sourceHash = (Get-FileHash -LiteralPath (Join-Path $packageRoot 'app\MiaCode.exe') -Algorithm SHA256).Hash
$stageHash = (Get-FileHash -LiteralPath (Join-Path $stage 'app\MiaCode.exe') -Algorithm SHA256).Hash
if ($sourceHash -ne $stageHash) { throw 'Staged executable differs from the verified package.' }

$backupRoot = [System.IO.Path]::GetFullPath((Join-Path $exportsRoot 'hand_order_test_install_backups'))
Assert-ChildPath -Path $backupRoot -Parent $exportsRoot -Label 'Backup root'
New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
$backup = [System.IO.Path]::GetFullPath((Join-Path $backupRoot ("MiaCode_HandOrder_Test_${stamp}_" + [guid]::NewGuid().ToString('N'))))
Assert-ChildPath -Path $backup -Parent $backupRoot -Label 'Backup'
$movedOld = $false
if (Test-Path -LiteralPath $testRoot) {
    Move-Item -LiteralPath $testRoot -Destination $backup
    $movedOld = $true
}
try {
    Move-Item -LiteralPath $stage -Destination $testRoot
} catch {
    if ($movedOld -and !(Test-Path -LiteralPath $testRoot)) {
        Move-Item -LiteralPath $backup -Destination $testRoot
    }
    throw
}

Write-Output "Test launcher: $(Join-Path $testRoot 'MiaCode.exe')"
Write-Output "Package revision: $([regex]::Match($identity, '(?m)^- Git revision: ([0-9a-f]{12})').Groups[1].Value)"
if ($movedOld) { Write-Output "Previous installation backup: $backup" }
