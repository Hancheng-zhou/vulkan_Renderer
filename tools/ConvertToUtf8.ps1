# Convert GBK/ANSI source files to UTF-8 with BOM.
#
# IMPORTANT: keep this file ASCII-only.
#   Windows PowerShell 5.1 reads BOM-less files as ANSI, which can corrupt
#   non-ASCII comments and silently break later statements in the script.
#
# Safety measures:
#   1. $ErrorActionPreference = 'Stop'  -> any error aborts immediately
#   2. hard fail if the GBK encoder cannot be obtained
#   3. never write when decoded text is null/empty
#   4. verify output size after writing; auto-restore that file on mismatch
#   5. per-file backup under tools/backup2_<timestamp>/

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }

$extensions = @('.h', '.hpp', '.cpp', '.c', '.vert', '.frag', '.glsl', '.lua', '.bat')

$files = Get-ChildItem -Path $root -Recurse -File |
    Where-Object {
        $extensions -contains $_.Extension -and
        $_.FullName -notmatch '\\vendor\\' -and
        $_.FullName -notmatch '\\bin-int\\' -and
        $_.FullName -notmatch '\\bin\\' -and
        $_.FullName -notmatch '\\.vs\\' -and
        $_.FullName -notmatch '\\tools\\'
    } |
    Sort-Object FullName

# ---------------------------------------------------------------- encoder
$gbk = $null
try
{
    $gbk = [System.Text.Encoding]::GetEncoding(936)
}
catch
{
    $gbk = $null
}

if ($null -eq $gbk)
{
    # fall back to system ANSI (which is 936 on a Chinese Windows)
    $gbk = [System.Text.Encoding]::Default
}

if ($null -eq $gbk)
{
    Write-Output '[FATAL] cannot obtain GBK/ANSI encoder. Aborting without touching any file.'
    exit 1
}

Write-Output ('[info] ANSI encoder : ' + $gbk.WebName + '  (codepage ' + $gbk.CodePage + ')')

$utf8WithBom = New-Object System.Text.UTF8Encoding($true)
$strictUtf8  = New-Object System.Text.UTF8Encoding($false, $true)

$backupRoot = Join-Path $PSScriptRoot ('backup2_' + (Get-Date -Format 'yyyyMMdd_HHmmss'))

$converted = 0
$skipped   = 0
$failed    = 0

# ---------------------------------------------------------------- convert
foreach ($f in $files)
{
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    $rel   = $f.FullName.Substring($root.Length + 1)

    # ---- already valid UTF-8 ? ----
    $isUtf8 = $false
    if ($bytes.Length -ge 3 -and
        $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    {
        $isUtf8 = $true
    }
    else
    {
        try
        {
            $null = $strictUtf8.GetString($bytes)
            $isUtf8 = $true
        }
        catch
        {
            $isUtf8 = $false
        }
    }

    if ($isUtf8)
    {
        $skipped++
        continue
    }

    # ---- decode as GBK ----
    $text = $null
    try
    {
        $text = $gbk.GetString($bytes)
    }
    catch
    {
        $text = $null
    }

    # ---- guard: never write null / empty ----
    if ($null -eq $text -or $text.Length -eq 0)
    {
        Write-Output ('[FAIL] decoded to empty text, file untouched: ' + $rel)
        $failed++
        continue
    }

    # ---- backup ----
    $backupPath = Join-Path $backupRoot $rel
    $backupDir  = Split-Path -Parent $backupPath
    if (-not (Test-Path $backupDir))
    {
        New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
    }
    Copy-Item -Path $f.FullName -Destination $backupPath -Force

    # ---- write ----
    [System.IO.File]::WriteAllText($f.FullName, $text, $utf8WithBom)

    # ---- verify: size must not collapse ----
    $newBytes = [System.IO.File]::ReadAllBytes($f.FullName)
    if ($newBytes.Length -lt [int]($bytes.Length / 2))
    {
        Write-Output ('[FAIL] size shrank unexpectedly, restoring: ' + $rel)
        Copy-Item -Path $backupPath -Destination $f.FullName -Force
        $failed++
        continue
    }

    Write-Output ('converted : ' + $rel + '   ' + $bytes.Length + ' -> ' + $newBytes.Length + ' bytes')
    $converted++
}

Write-Output ''
Write-Output ('converted: ' + $converted + ' file(s)')
Write-Output ('skipped  : ' + $skipped + ' file(s) (already UTF-8 or ASCII)')
Write-Output ('failed   : ' + $failed + ' file(s)')
Write-Output ('backup   : ' + $backupRoot)
