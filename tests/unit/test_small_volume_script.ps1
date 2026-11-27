# Requirements: K-117, K-122. Only AST-selected Create blocks run; all process launches are mocked.
$ErrorActionPreference = 'Stop'
$tokens = $null
$errors = $null
$source = Join-Path (Get-Location) 'tools/test_volume/small_volume.ps1'
$ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$errors)
if (@($errors).Count) { throw 'Production script has syntax errors.' }
$guard = @($ast.EndBlock.Statements | Where-Object { $_.Extent.Text.StartsWith('if ($projectRoot -match') })
$create = @($ast.EndBlock.Statements | Where-Object { $_.Extent.Text.StartsWith('if (-not (Test-Path -LiteralPath $imagePath -PathType Leaf))') })
if ($guard.Count -ne 1 -or $create.Count -ne 1) { throw 'Expected exactly the ASCII guard and image creation block.' }
$guardCode = [scriptblock]::Create($guard[0].Extent.Text)
$createCode = [scriptblock]::Create($create[0].Extent.Text)
$area = Join-Path (Get-Location) ('.cache/small-volume-script-tests/' + [guid]::NewGuid().ToString())
[IO.Directory]::CreateDirectory($area) | Out-Null
$script:fake = $null
$script:calls = 0
function Start-Process {
    param($FilePath, $ArgumentList, $WindowStyle, [switch]$Wait, [switch]$PassThru, $RedirectStandardOutput)
    if ($FilePath -ne (Join-Path $env:SystemRoot 'System32/diskpart.exe') -or $WindowStyle -ne 'Hidden' -or -not $Wait -or -not $PassThru) { throw 'Unexpected process request.' }
    if ($ArgumentList.Count -ne 2 -or $ArgumentList[0] -ne '/s') { throw 'Unexpected DiskPart arguments.' }
    $script:calls++
    if ($null -ne $script:fake.LogBytes) { [IO.File]::WriteAllBytes($RedirectStandardOutput, $script:fake.LogBytes) }
    if ($script:fake.Image) { [IO.File]::WriteAllText($imagePath, 'mock image only; never mounted') }
    [pscustomobject]@{ ExitCode = $script:fake.Code }
}
function Check-Case($name, $code, [byte[]]$logBytes, [bool]$image, [bool]$accepted, [bool]$unicode = $false) {
    $projectRoot = (Get-Location).Path
    if ($unicode) { $projectRoot += [char]0x0430 }
    $volumeArea = Join-Path $area $name
    [IO.Directory]::CreateDirectory($volumeArea) | Out-Null
    $imagePath = Join-Path $volumeArea 'vol.vhdx'
    $mountPath = Join-Path $volumeArea 'mnt'
    $ownershipPath = Join-Path $volumeArea 'owner.json'
    $script:fake = @{ Code = $code; LogBytes = $logBytes; Image = $image }
    $script:calls = 0
    $failure = $null
    try { . $guardCode; . $createCode } catch { $failure = $_.Exception.Message }
    if (($null -eq $failure) -ne $accepted) { throw "$name unexpected result: $failure" }
    if ((Test-Path -LiteralPath $ownershipPath) -ne $accepted) { throw "$name wrote an ownership record on failure." }
    if ($unicode) {
        if ($script:calls -ne 0 -or $failure -notmatch 'ASCII project path' -or (Test-Path (Join-Path $volumeArea 'create.txt'))) { throw 'Non-ASCII guard did not fail before command creation.' }
    } else {
        if ($script:calls -ne 1) { throw "$name did not use the mock exactly once." }
        $command = Join-Path $volumeArea 'create.txt'
        $actual = [IO.File]::ReadAllBytes($command)
        if (@($actual | Where-Object { $_ -gt 127 -or $_ -eq 0 }).Count) { throw "$name command contains a BOM, NUL, or non-ASCII bytes." }
        $expected = "create vdisk file=`"$imagePath`" maximum=64 type=expandable`r`nexit`r`n"
        if ([Text.Encoding]::ASCII.GetString($actual) -cne $expected) { throw "$name command bytes differ." }
        if ($accepted -eq $false -and $failure -notmatch 'exit code:') { throw "$name diagnostic lacks the exit code." }
    }
    Write-Output "PASS $name"
}
$ascii = [Text.Encoding]::ASCII
$complete = $ascii.GetBytes("  100 percent completed`r`nDiskPart successfully created the virtual disk file.`r`n")
Check-Case 'english-success' 0 $complete $true $true
$localized = "  100 " + (-join [char[]](0x043F,0x0440,0x043E,0x0446,0x0435,0x043D,0x0442,0x043E,0x0432)) + "`r`n"
Check-Case 'oem866-success' 0 ([Text.Encoding]::GetEncoding(866).GetBytes($localized)) $true $true
Check-Case 'percent-cr-progress' 0 ($ascii.GetBytes(" 50%`r 100%`r")) $true $true
Check-Case 'nonzero-exit' 5 $complete $true $false
Check-Case 'unknown-exit' $null $complete $true $false
Check-Case 'help-without-progress' 0 ($ascii.GetBytes("CREATE      - help`r`n")) $true $false
Check-Case 'empty-log' 0 ([byte[]]@()) $true $true
Check-Case 'progress-not-a-condition' 0 ($ascii.GetBytes(" 50 percent completed`r`n")) $true $true
Check-Case 'missing-log' 0 $null $true $true
Check-Case 'missing-image' 0 $complete $false $false
Check-Case 'help-after-progress' 0 ($ascii.GetBytes(" 100 percent completed`r`nCREATE      - help`r`n")) $true $false
Check-Case 'non-ascii-project' 0 $complete $true $false $true
$actualLog = [IO.File]::ReadAllBytes((Join-Path (Get-Location) 'tests/fixtures/fault/diskpart-create-ru.cp866.log'))
$decoded = [Text.Encoding]::GetEncoding(866).GetString($actualLog)
if ($decoded -notmatch '\u0417\u0430\u0432\u0435\u0440\u0448\u0435\u043d\u043e \(\u0432 \u043f\u0440\u043e\u0446\u0435\u043d\u0442\u0430\u0445\):\s+100\b') { throw 'Fixture does not reproduce the actual Russian progress layout.' }
Check-Case 'actual-russian-cp866-success' 0 $actualLog $true $true
Check-Case 'actual-russian-nonzero-exit' 5 $actualLog $true $false
Check-Case 'actual-russian-missing-image' 0 $actualLog $false $false
Write-Output 'PASS: 15/15 DiskPart ASCII/log regression cases; no native process or disk operation executed.'
