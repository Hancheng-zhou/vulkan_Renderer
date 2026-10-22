# 逐个检测源文件的实际编码（BOM 类型 / 是否为合法 UTF-8 / 是否为 ANSI-GBK）
# 用法： powershell -NoProfile -ExecutionPolicy Bypass -File tools\DetectEncoding.ps1

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

$summary = @{}

foreach ($f in $files)
{
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    $kind  = 'UNKNOWN'

    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    {
        $kind = 'UTF8-BOM'
    }
    elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE)
    {
        $kind = 'UTF16-LE'
    }
    elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF)
    {
        $kind = 'UTF16-BE'
    }
    else
    {
        $hasHigh = $false
        foreach ($b in $bytes)
        {
            if ($b -ge 0x80) { $hasHigh = $true; break }
        }

        if (-not $hasHigh)
        {
            $kind = 'ASCII'
        }
        else
        {
            try
            {
                $strict = New-Object System.Text.UTF8Encoding($false, $true)
                $null = $strict.GetString($bytes)
                $kind = 'UTF8-noBOM'
            }
            catch
            {
                $kind = 'ANSI/GBK'
            }
        }
    }

    if (-not $summary.ContainsKey($kind)) { $summary[$kind] = 0 }
    $summary[$kind]++

    $rel = $f.FullName.Substring($root.Length + 1)
    Write-Output ("{0,-12} {1}" -f $kind, $rel)
}

Write-Output ''
Write-Output '----- summary -----'
foreach ($k in $summary.Keys | Sort-Object)
{
    Write-Output ("{0,-12} {1} file(s)" -f $k, $summary[$k])
}
