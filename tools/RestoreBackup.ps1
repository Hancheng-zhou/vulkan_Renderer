# 从备份目录恢复文件
# 用法： powershell -NoProfile -ExecutionPolicy Bypass -File tools\RestoreBackup.ps1 -BackupName backup_20261021_212243

param(
    [string]$BackupName = 'backup_20261021_212243'
)

$root   = Split-Path -Parent $PSScriptRoot
$backup = Join-Path $PSScriptRoot $BackupName

if (-not (Test-Path $backup))
{
    Write-Output ('[ERROR] backup not found: ' + $backup)
    exit 1
}

$count = 0

Get-ChildItem -Path $backup -Recurse -File | ForEach-Object {
    $rel     = $_.FullName.Substring($backup.Length + 1)
    $dest    = Join-Path $root $rel
    $destDir = Split-Path -Parent $dest

    if (-not (Test-Path $destDir))
    {
        New-Item -ItemType Directory -Path $destDir -Force | Out-Null
    }

    Copy-Item -Path $_.FullName -Destination $dest -Force
    Write-Output ('restored : ' + $rel + '  (' + $_.Length + ' bytes)')
    $count++
}

Write-Output ''
Write-Output ('total restored: ' + $count + ' file(s)')
