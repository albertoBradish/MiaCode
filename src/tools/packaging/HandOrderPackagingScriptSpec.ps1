param([string]$ScratchRoot = '')

$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$workspaceRoot = Split-Path -Parent $sourceRoot
$allowedScratchRoot = [IO.Path]::GetFullPath((Join-Path $workspaceRoot 'runtime\temp'))
if ([string]::IsNullOrWhiteSpace($ScratchRoot)) {
    $ScratchRoot = Join-Path $allowedScratchRoot 'packaging_script_spec'
}
$ScratchRoot = [IO.Path]::GetFullPath($ScratchRoot)
if (!$ScratchRoot.StartsWith($allowedScratchRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Synthetic packaging fixtures must remain inside the workspace runtime/temp directory.'
}
$testRoot = Join-Path $ScratchRoot ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null

function Assert-Condition {
    param([bool]$Condition, [string]$Label)
    if (!$Condition) { throw "FAIL: $Label" }
    Write-Output "PASS: $Label"
}

function Import-TestedFunctions {
    param([string]$Path, [string[]]$Names)
    $tokens = $null
    $parseErrors = $null
    $tree = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count -gt 0) { throw "PowerShell syntax errors in $Path : $parseErrors" }
    foreach ($name in $Names) {
        $definition = $tree.FindAll({
            param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst]
        }, $true) | Where-Object Name -EQ $name | Select-Object -First 1
        if ($null -eq $definition) { throw "Missing function $name" }
        # Return declarations only; never execute either packaging entry point.
        $definition.Extent.Text
    }
}

$buildScripts = Join-Path $sourceRoot 'scripts\build'
$definitions = Import-TestedFunctions (Join-Path $buildScripts 'Start_HandOrder.ps1') @(
    'ConvertTo-WindowsCommandLineArgument')
foreach ($definition in $definitions) { . ([scriptblock]::Create($definition)) }

$quoteCases = @(
    @{ Input = ''; Expected = '""' },
    @{ Input = 'plain'; Expected = '"plain"' },
    @{ Input = 'with space'; Expected = '"with space"' },
    @{ Input = 'C:\normal\file.txt'; Expected = '"C:\normal\file.txt"' },
    @{ Input = 'C:\with space\'; Expected = '"C:\with space\\"' },
    @{ Input = '\'; Expected = '"\\"' },
    @{ Input = 'a"b'; Expected = '"a\"b"' },
    @{ Input = '"'; Expected = '"\""' },
    @{ Input = 'two\\"quotes'; Expected = '"two\\\\\"quotes"' },
    @{ Input = '& | %PATH% $(literal)'; Expected = '"& | %PATH% $(literal)"' }
)
for ($index = 0; $index -lt $quoteCases.Count; ++$index) {
    $case = $quoteCases[$index]
    $actual = ConvertTo-WindowsCommandLineArgument -Argument $case.Input
    Assert-Condition ($actual -ceq $case.Expected) "Windows argv quoting case $index"
}
$unicode = [string][char]0x4E2D + [char]0x6587 + ' path\'
Assert-Condition ((ConvertTo-WindowsCommandLineArgument $unicode) -ceq ('"' + $unicode + '\"')) `
    'Unicode, spaces and a final backslash survive quoting'
$forwarded = @('', 'a b', 'C:\tail\', 'x"y') | ForEach-Object {
    ConvertTo-WindowsCommandLineArgument -Argument $_
}
Assert-Condition (($forwarded -join ' ') -ceq '"" "a b" "C:\tail\\" "x\"y"') `
    'Joined command line preserves separate empty and escaped arguments'

# Python's native Windows startup is the independent argv oracle. This runs
# only a standard-library echo process, never MiaCode, a shell, or a compiler.
$python = Get-Command python.exe -ErrorAction Stop
$shellMarker = Join-Path $testRoot 'shell-must-not-run.txt'
$roundtripArguments = [string[]]@(
    '', 'C:\chart folder\maidata.txt', 'embedded"quote', 'C:\tail\', '\',
    'two\\"quotes', ('tab' + [char]9 + 'value'), $unicode,
    '& | %PATH% $(literal)', ('& echo should_not_run > "' + $shellMarker + '"'))
$echoSource = 'import json,sys; print(json.dumps(sys.argv[1:],ensure_ascii=True))'
$echoArguments = @('-c', $echoSource) + $roundtripArguments
$echoStart = [Diagnostics.ProcessStartInfo]::new($python.Source)
$echoStart.UseShellExecute = $false
$echoStart.RedirectStandardOutput = $true
$echoStart.RedirectStandardError = $true
$echoStart.CreateNoWindow = $true
$echoStart.WorkingDirectory = $testRoot
$echoStart.EnvironmentVariables['TEMP'] = $testRoot
$echoStart.EnvironmentVariables['TMP'] = $testRoot
$echoStart.EnvironmentVariables['TMPDIR'] = $testRoot
$echoStart.EnvironmentVariables['PYTHONDONTWRITEBYTECODE'] = '1'
$echoStart.Arguments = (@($echoArguments | ForEach-Object {
    ConvertTo-WindowsCommandLineArgument -Argument $_
}) -join ' ')
$echoProcess = [Diagnostics.Process]::new()
try {
    $echoProcess.StartInfo = $echoStart
    if (!$echoProcess.Start()) { throw 'Native argv echo process did not start.' }
    if (!$echoProcess.WaitForExit(30000)) {
        $echoProcess.Kill()
        throw 'Native argv echo process exceeded 30 seconds.'
    }
    $echoOutput = $echoProcess.StandardOutput.ReadToEnd()
    $echoError = $echoProcess.StandardError.ReadToEnd()
    Assert-Condition ($echoProcess.ExitCode -eq 0) "Native argv echo process succeeds: $echoError"
    # Windows PowerShell 5.1 emits a JSON array as one pipeline object.
    # Assign it directly so an additional @() does not wrap that native array.
    $actualArguments = ConvertFrom-Json -InputObject $echoOutput
    Write-Output ("Native argv JSON: {0}" -f $echoOutput.Trim())
    Write-Output ("Native argv decoded: type={0}, count={1}, firstType={2}, expected={3}" -f
        $actualArguments.GetType().FullName, $actualArguments.Count,
        $actualArguments[0].GetType().FullName, $roundtripArguments.Count)
    Assert-Condition ($actualArguments.Count -eq $roundtripArguments.Count) 'Native child receives the exact argument count'
    for ($index = 0; $index -lt $roundtripArguments.Count; ++$index) {
        Assert-Condition ($actualArguments[$index] -ceq $roundtripArguments[$index]) "Native argv round-trip case $index"
    }
    Assert-Condition (!(Test-Path -LiteralPath $shellMarker)) 'Shell metacharacters remain literal data and execute no command'
} finally {
    $echoProcess.Dispose()
}

$definitions = Import-TestedFunctions (Join-Path $buildScripts 'package-win.ps1') @(
    'Read-VersionInfoFromGeneratedHeader', 'Resolve-BuildOutputDir', 'Ensure-PackageBuildReady')
foreach ($definition in $definitions) { . ([scriptblock]::Create($definition)) }

# No compiler or product executable is invoked. The real precheck runs against
# small synthetic files with configure/build replaced by observable mocks.
$script:buildCalls = 0
$script:configureCalls = 0
$script:buildJobsSeen = @()
$script:versionInfo = [pscustomobject]@{ PackageVersion = '2.0.0-alpha'; DisplayVersion = '2.0.0-alpha' }
function Write-TestVersionHeader {
    param([string]$BuildDir)
    New-Item -ItemType Directory -Path (Join-Path $BuildDir 'generated') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $BuildDir 'generated\AppVersion.h') -Encoding Ascii -Value @(
        '#define MIACODE_VERSION_STRING "2.0.0-alpha"',
        '#define MIACODE_DISPLAY_VERSION_STRING "2.0.0-alpha"')
}
function Invoke-MiaCodeConfigure {
    param([string]$RepoRoot, [string]$BuildDir)
    ++$script:configureCalls
    Write-TestVersionHeader $BuildDir
}
function Invoke-MiaCodeBuild {
    param([string]$BuildDir, [string]$Config, [int]$BuildJobs)
    ++$script:buildCalls
    $script:buildJobsSeen += $BuildJobs
    $outputDir = Join-Path $BuildDir $Config
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
    foreach ($name in @('MiaCode.exe', 'MiaCodeLauncher.exe')) {
        Set-Content -LiteralPath (Join-Path $outputDir $name) -Value 'synthetic fixture only' -Encoding Ascii
    }
}

$repoFixture = Join-Path $testRoot 'source'
$buildFixture = Join-Path $testRoot 'build'
New-Item -ItemType Directory -Path (Join-Path $repoFixture 'src\app') -Force | Out-Null
New-Item -ItemType Directory -Path $buildFixture -Force | Out-Null
Set-Content -LiteralPath (Join-Path $repoFixture 'CMakeLists.txt') -Value '# synthetic' -Encoding Ascii
Set-Content -LiteralPath (Join-Path $repoFixture 'src\app\AppVersion.h.in') -Value '# synthetic' -Encoding Ascii
Set-Content -LiteralPath (Join-Path $buildFixture 'CMakeCache.txt') -Value '# synthetic' -Encoding Ascii
Write-TestVersionHeader $buildFixture
$oldStamp = [DateTime]::UtcNow.AddMinutes(-5)
Get-ChildItem -LiteralPath $testRoot -Recurse -File | ForEach-Object { $_.LastWriteTimeUtc = $oldStamp }
Invoke-MiaCodeBuild $buildFixture Release 4
$script:buildCalls = 0
$script:buildJobsSeen = @()

$actual = Ensure-PackageBuildReady $repoFixture $buildFixture Release 4 $script:versionInfo
Assert-Condition ($script:buildCalls -eq 1 -and $script:configureCalls -eq 0) `
    'Matching version metadata still runs the incremental product/launcher build'
Set-Content -LiteralPath (Join-Path $repoFixture 'src\new_source.cpp') -Value '// newer input' -Encoding Ascii
$actual = Ensure-PackageBuildReady $repoFixture $buildFixture Release 4 $script:versionInfo
Assert-Condition ($script:buildCalls -eq 2 -and $script:configureCalls -eq 0) `
    'Same-version source edits cannot bypass the build freshness check'
Assert-Condition (($script:buildJobsSeen -join ',') -eq '4,4') 'Incremental build keeps the requested parallel limit'

Set-Content -LiteralPath (Join-Path $buildFixture 'generated\AppVersion.h') -Value @(
    '#define MIACODE_VERSION_STRING "old"', '#define MIACODE_DISPLAY_VERSION_STRING "old"') -Encoding Ascii
$actual = Ensure-PackageBuildReady $repoFixture $buildFixture Release 2 $script:versionInfo
Assert-Condition ($script:configureCalls -eq 1 -and $script:buildCalls -eq 3) `
    'Version mismatch configures then builds exactly once'

$freshBuild = Join-Path $testRoot 'new_build'
New-Item -ItemType Directory -Path $freshBuild -Force | Out-Null
Set-Content -LiteralPath (Join-Path $freshBuild 'CMakeCache.txt') -Value '# synthetic' -Encoding Ascii
Write-TestVersionHeader $freshBuild
$actual = Ensure-PackageBuildReady $repoFixture $freshBuild Release 1 $script:versionInfo
Assert-Condition ($actual -eq (Join-Path $freshBuild 'Release\MiaCode.exe')) `
    'Missing multi-config binaries are resolved again after the incremental build'

$rejected = $false
try { $null = Ensure-PackageBuildReady $repoFixture $buildFixture Release 5 $script:versionInfo }
catch { $rejected = $_.Exception.Message -like '*between 1 and 4*' }
Assert-Condition $rejected 'Packaging refuses parallelism above four before any build'
Write-Output "Synthetic fixtures: $testRoot"
