# Renderer test matrix (one fresh game session per renderer).
# For each renderer: start Clone Campaigns, enter a standard game, pause it,
# then switch filters live via SWGBModern.ini and compare the screen against
# the game's own frame dump (exact 2x nearest upscale = ground truth).
# Also tests windowed mode, an odd window size and Alt+Enter, and measures CPU.
param([string]$Game = 'C:\Users\user\Desktop\claude\swgb\Game', [string]$Out = "$env:TEMP\matrix",
      [string[]]$Renderers = @('d3d11', 'd3d10', 'd3d12', 'opengl', 'vulkan', 'software'),
      [string[]]$Filters = @('nearest', 'sharp', 'bilinear', 'xbr'))
$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$inp = "$env:TEMP\inp.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
$ini = "$Game\SWGBModern.ini"
Add-Type @'
using System; using System.Runtime.InteropServices;
public class TW { [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RC r); [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
public struct RC { public int L,T,R,B; } }
'@
[TW]::SetProcessDPIAware() | Out-Null

function Set-Ini([hashtable]$kv) {
    $t = Get-Content $ini -Raw
    foreach ($k in $kv.Keys) {
        if ($t -match "(?m)^$k=") { $t = $t -replace "(?m)^$k=.*", "$k=$($kv[$k])" }
        else { $t = $t -replace '\[Display\]', "[Display]`r`n$k=$($kv[$k])" }
    }
    Set-Content $ini $t -NoNewline
}
function Shot([string]$name) { & "$here\shotbmp.ps1" "$Out\$name.bmp"; Copy-Item "$Game\SWGBModern_frame.bmp" "$Out\$name-dump.bmp" -Force }

$results = @()
foreach ($r in $Renderers) {
    Get-Process battlegrounds_x1 -EA SilentlyContinue | Stop-Process -Force
    Start-Sleep 1
    Set-Ini @{ Renderer = $r; Filter = 'nearest'; Mode = 'borderless'; ScaleMode = 'aspect'; GameWidth = 1920; GameHeight = 1080; VSync = 1; MaxFPS = 0 }
    (Get-Content $ini -Raw) -replace '(?m)^Log=.*', 'Log=1' -replace '(?m)^DumpFrameSeconds=.*', 'DumpFrameSeconds=1' | Set-Content $ini -NoNewline
    $p = Start-Process "$Game\battlegrounds_x1.exe" -ArgumentList NOSTARTUP -WorkingDirectory $Game -PassThru
    Start-Sleep 8
    & $inp click 2400 1700 sleep 800 click 1790 772 sleep 2500 click 2000 1075 sleep 300 key 0x54 key 0x45 key 0x53 key 0x54 sleep 300 click 1646 1272 sleep 2500
    & $inp click 2652 822 sleep 3000 click 908 2046 sleep 15000
    # performance sample (unpaused, edge-scrolling right)
    $p.Refresh(); $c0 = $p.TotalProcessorTime
    & $inp move 3839 1000 sleep 5000 move 1920 600
    $p.Refresh(); $cpu = [math]::Round(($p.TotalProcessorTime - $c0).TotalMilliseconds / 50.0, 1)  # % of one core
    $perf = (Get-Content "$Game\SWGBModern.log" | Select-String 'present:' | Select -Last 1).Line -replace '^\[\d+\] present: ', ''
    $backend = ((Get-Content "$Game\SWGBModern.log" | Select-String 'Renderer: using (.*)$' | Select -Last 1).Matches[0].Groups[1].Value)
    & $inp key 0x72 sleep 2500   # F3 pause
    foreach ($f in $Filters) {
        Set-Ini @{ Filter = $f }
        Start-Sleep -Milliseconds 2500
        Shot "$r-$f"
        $cmp = python -I "$here\cmp.py" "$Out\$r-$f-dump.bmp" "$Out\$r-$f.bmp" --scale 2
        $results += [pscustomobject]@{ Renderer = $r; Backend = $backend; Test = "fullscreen 2x $f"; Result = $cmp; CPU = "$cpu%"; Perf = $perf }
        Write-Host "$r [$backend] $f : $cmp"
    }
    # windowed, odd size (non-integer scale) with sharp filter
    Set-Ini @{ Mode = 'windowed'; Filter = 'sharp' }
    Start-Sleep 2
    $h = $p.MainWindowHandle
    [TW]::SetWindowPos($h, [IntPtr]::Zero, 200, 150, 1617, 1013, 0x14) | Out-Null   # NOZORDER|NOACTIVATE
    Start-Sleep 2
    $rc = New-Object TW+RC; [TW]::GetClientRect($h, [ref]$rc) | Out-Null
    Shot "$r-windowed"
    $alive1 = !$p.HasExited
    # Alt+Enter back to borderless
    & $inp click 1000 600 sleep 300 altenter sleep 2500
    Shot "$r-altenter"
    $cmpAlt = python -I "$here\cmp.py" "$Out\$r-altenter-dump.bmp" "$Out\$r-altenter.bmp" --scale 2
    $errs = (Get-Content "$Game\SWGBModern.log" | Select-String -Pattern 'failed|error|lost|CRASH' | % Line) -join ' | '
    $results += [pscustomobject]@{ Renderer = $r; Backend = $backend; Test = "windowed $($rc.R)x$($rc.B) sharp"; Result = "alive=$alive1"; CPU = ''; Perf = '' }
    $results += [pscustomobject]@{ Renderer = $r; Backend = $backend; Test = "alt+enter -> borderless (sharp)"; Result = "$cmpAlt"; CPU = ''; Perf = $errs }
    Write-Host "$r windowed client $($rc.R)x$($rc.B) alive=$alive1; alt+enter: $cmpAlt; errors: $errs"
    Set-Ini @{ Mode = 'borderless' }
    Stop-Process -Id $p.Id -Force -EA SilentlyContinue
}
$results | Export-Csv "$Out\results.csv" -NoTypeInformation
