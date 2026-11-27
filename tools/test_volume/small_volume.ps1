[CmdletBinding(DefaultParameterSetName = 'Status')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Create')][switch]$Create,
    [Parameter(Mandatory = $true, ParameterSetName = 'Remove')][switch]$Remove,
    [Parameter(ParameterSetName = 'Status')][switch]$Status
)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$volumeArea = [IO.Path]::GetFullPath((Join-Path $projectRoot '.cache/small-volume'))
$imagePath = [IO.Path]::GetFullPath((Join-Path $volumeArea 'vol.vhdx'))
$mountPath = [IO.Path]::GetFullPath((Join-Path $volumeArea 'mnt'))
$ownershipPath = Join-Path $volumeArea 'owner.json'
$volumeSize = 64MB # hypothesis: disposable test volume, not a product limit
foreach ($targetPath in @($volumeArea, $imagePath, $mountPath, $ownershipPath)) {
    if (-not $targetPath.StartsWith($projectRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Test volume path escaped the project.'
    }
}
function Assert-Owner {
    if (-not (Test-Path -LiteralPath $ownershipPath -PathType Leaf)) { throw 'Refusing an image without the T-036 ownership record.' }
    $record = Get-Content -LiteralPath $ownershipPath -Raw | ConvertFrom-Json
    if ($record.task -ne 'T-036' -or $record.image -ne $imagePath -or $record.mount -ne $mountPath) {
        throw 'Test volume ownership record does not match the exact paths.'
    }
}
function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this command yourself in an Administrator PowerShell. This script never requests elevation.'
    }
}
function Get-OwnedImage {
    if (-not (Test-Path -LiteralPath $imagePath -PathType Leaf)) { return $null }
    Assert-Owner
    return Get-DiskImage -ImagePath $imagePath
}
function Remove-EmptyMountFolder {
    if (Test-Path -LiteralPath $mountPath) {
        $item = Get-Item -LiteralPath $mountPath -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing to remove an unresolved mount point.' }
        # No recursive deletion: a detached mount folder must be empty.
        Remove-Item -LiteralPath $mountPath
    }
}
if ($PSCmdlet.ParameterSetName -eq 'Status') {
    $exists = Test-Path -LiteralPath $imagePath -PathType Leaf
    $attached = $false
    if ($exists) { $attached = [bool](Get-OwnedImage).Attached }
    [ordered]@{ task = 'T-036'; state = $(if ($attached) { 'attached' } elseif ($exists) { 'detached' } else { 'absent' }); mount = $mountPath } | ConvertTo-Json -Compress
    exit 0
}
if ($Remove) {
    if (-not (Test-Path -LiteralPath $imagePath -PathType Leaf)) {
        if (Test-Path -LiteralPath $ownershipPath) {
            Assert-Owner
            Remove-EmptyMountFolder
            Remove-Item -LiteralPath $ownershipPath
        }
        Write-Output 'Removed T-036 test volume (already absent).'
        exit 0
    }
    Assert-Administrator
    $image = Get-OwnedImage
    if ($image.Attached) {
        $disk = $image | Get-Disk
        if ($disk.IsBoot -or $disk.IsSystem -or $disk.Size -ne $volumeSize) { throw 'Refusing a disk that is not the exact disposable test image.' }
        foreach ($partition in @($disk | Get-Partition)) {
            if (@($partition.AccessPaths) -contains ($mountPath + '\')) {
                Remove-PartitionAccessPath -DiskNumber $disk.Number -PartitionNumber $partition.PartitionNumber -AccessPath ($mountPath + '\')
            }
        }
        Dismount-DiskImage -ImagePath $imagePath
    }
    # Absolute paths were checked above; ownership is checked again before deletion.
    Assert-Owner
    Remove-Item -LiteralPath $imagePath
    Remove-EmptyMountFolder
    Remove-Item -LiteralPath $ownershipPath
    Write-Output 'Removed T-036 test volume and mount point.'
    exit 0
}
Assert-Administrator
if ($projectRoot -match '[^\x00-\x7F]') {
    throw 'DiskPart requires an ASCII project path for its command file. Use a project path containing only ASCII characters.'
}
[IO.Directory]::CreateDirectory($volumeArea) | Out-Null
if (-not (Test-Path -LiteralPath $imagePath -PathType Leaf)) {
    if (Test-Path -LiteralPath $ownershipPath) { throw 'Ownership record exists without its image; run -Remove first.' }
    # DiskPart is installed with Windows; Hyper-V/New-VHD is not required.
    $commandPath = Join-Path $volumeArea 'create.txt'
    [IO.File]::WriteAllText($commandPath, "create vdisk file=`"$imagePath`" maximum=64 type=expandable`r`nexit`r`n", [Text.Encoding]::ASCII)
    $logPath = Join-Path $volumeArea 'diskpart.log'
    $process = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32/diskpart.exe') -ArgumentList @('/s', ('"' + $commandPath + '"')) -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $logPath
    $exitCode = $process.ExitCode
    $logText = if (Test-Path -LiteralPath $logPath -PathType Leaf) { [IO.File]::ReadAllText($logPath) } else { '' }
    # Command keywords remain ASCII in localized/OEM logs; progress text is not a success condition.
    $helpOutput = $logText -match '(?:^|[\r\n])\s*CREATE\s+-'
    if ($null -eq $exitCode -or $exitCode -ne 0 -or $helpOutput -or -not (Test-Path -LiteralPath $imagePath -PathType Leaf)) {
        throw "DiskPart did not create the test image (exit code: $exitCode; help output: $helpOutput); see diskpart.log."
    }
    $record = [ordered]@{ task = 'T-036'; image = $imagePath; mount = $mountPath } | ConvertTo-Json -Compress
    [IO.File]::WriteAllText($ownershipPath, $record, (New-Object Text.UTF8Encoding($false)))
}
$image = Get-OwnedImage
if (-not $image.Attached) { $image = Mount-DiskImage -ImagePath $imagePath -StorageType VHDX -NoDriveLetter -PassThru }
$disk = $image | Get-Disk
if ($disk.IsBoot -or $disk.IsSystem -or $disk.Size -ne $volumeSize) { throw 'Refusing to initialize a disk outside the disposable image.' }
if ($disk.PartitionStyle -eq 'RAW') { $disk | Initialize-Disk -PartitionStyle MBR | Out-Null }
$partitions = @($disk | Get-Partition | Where-Object { $_.Type -ne 'Reserved' })
if ($partitions.Count -eq 0) {
    $partition = $disk | New-Partition -UseMaximumSize
    $partition | Format-Volume -FileSystem NTFS -NewFileSystemLabel 'AM_T036' -Confirm:$false | Out-Null
} elseif ($partitions.Count -eq 1) {
    $partition = $partitions[0]
    if (($partition | Get-Volume).FileSystem -ne 'NTFS') { throw 'Existing test partition is not NTFS; run -Remove before retrying.' }
} else { throw 'Unexpected partition layout in disposable image.' }
if ($partition.DriveLetter) { throw 'The test partition unexpectedly has a drive letter; no permanent letters are permitted.' }
[IO.Directory]::CreateDirectory($mountPath) | Out-Null
if (-not (@($partition.AccessPaths) -contains ($mountPath + '\'))) {
    if (Get-ChildItem -LiteralPath $mountPath -Force) { throw 'Mount directory must be empty.' }
    Add-PartitionAccessPath -DiskNumber $disk.Number -PartitionNumber $partition.PartitionNumber -AccessPath ($mountPath + '\')
}
Write-Output ('Ready. Set AM_SMALL_VOLUME to ' + $mountPath + ' in the test shell.')
