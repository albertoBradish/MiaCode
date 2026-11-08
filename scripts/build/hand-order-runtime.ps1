function Initialize-HandOrderDevelopmentRuntime([string]$Workspace) {
    $runtimeBase = Join-Path $Workspace 'runtime'
    $runtimeRoot = Join-Path $runtimeBase 'handorder_public_dev_20261108'
    $runtimeMarker = Join-Path $runtimeRoot '.hand-order-runtime'
    if (-not (Test-Path -LiteralPath $runtimeBase -PathType Container) -or
        ((Get-Item -LiteralPath $runtimeBase -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
        throw "Hand-order runtime parent is missing or redirected: $runtimeBase"
    }
    if (-not (Test-Path -LiteralPath $runtimeRoot)) {
        New-Item -ItemType Directory -Path $runtimeRoot | Out-Null
        [System.IO.File]::WriteAllText($runtimeMarker, "MiaCodeHandOrder.runtime.v1`n")
    }
    if (-not (Test-Path -LiteralPath $runtimeRoot -PathType Container) -or
        ((Get-Item -LiteralPath $runtimeRoot -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
        throw "Hand-order runtime directory is redirected: $runtimeRoot"
    }
    if (-not (Test-Path -LiteralPath $runtimeMarker -PathType Leaf) -or
        ((Get-Item -LiteralPath $runtimeMarker -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint) -or
        (Get-Content -LiteralPath $runtimeMarker -Raw).Trim() -ne 'MiaCodeHandOrder.runtime.v1') {
        throw "Hand-order runtime marker is missing or invalid: $runtimeMarker"
    }
    $runtimeTemp = Join-Path $runtimeRoot 'temp'
    if ((Test-Path -LiteralPath $runtimeTemp) -and
        (-not (Test-Path -LiteralPath $runtimeTemp -PathType Container) -or
        ((Get-Item -LiteralPath $runtimeTemp -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint))) {
        throw "Hand-order temporary directory is redirected or invalid: $runtimeTemp"
    }
    return $runtimeRoot
}
