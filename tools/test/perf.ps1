# Performance benchmark: CPU and GPU use of the game process in three scenes.
#   menu   - main menu, idle
#   game   - Episode I campaign mission 1 (fixed map), idle (camera still)
#   scroll - the same, camera edge-scrolling left and right
# For each scene: CPU (% of one core, process total), GPU (% of the busiest
# GPU engine used by the process, from Windows' GPU Engine counters), frames
# presented per second and the presenter's own time per frame.
# Usage: perf.ps1 [-Renderer d3d11] [-Fog 0|1] [-Text 0|1] [-Runs 1] [-Tag name]
#   -Set 'Key=Value;Key=Value'  extra SWGBModern.ini overrides (e.g. GameFPS=20;Widescreen=0)
#   -Other                     another wrapper: leave the game folder's config alone,
#                              skip the intro videos, focus the window first
#   -OkX/-OkY                  screen position of the Objectives OK button
#                              (default: 1920x1080 game; 800x600 game: 2599,1810)
param([string]$Game = $(if ($env:SWGB_GAME) { $env:SWGB_GAME } else { Join-Path $PSScriptRoot '..\..\..\Game' }), [string]$Renderer = 'd3d11',
      [int]$Fog = 1, [int]$Text = 1, [int]$Runs = 1, [string]$Tag = 'perf', [int]$Seconds = 10,
      [string]$Set = '', [switch]$Other, [int]$OkX = 2297, [int]$OkY = 1486)
$i = "$env:TEMP\inp.exe"; $ini = "$Game\SWGBModern.ini"
Add-Type -Namespace Perf -Name Native -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern bool QueryProcessCycleTime(System.IntPtr h, out ulong cycles);
'@
$orig = if (Test-Path $ini) { Get-Content $ini -Raw } else { $null }

function Measure-Scene($p, [int]$sec, [scriptblock]$during) {
    $p.Refresh(); $c0 = $p.TotalProcessorTime; $t0 = Get-Date
    $cy0 = [uint64]0; [void][Perf.Native]::QueryProcessCycleTime($p.Handle, [ref]$cy0)
    $th0 = @{}; foreach ($t in $p.Threads) { try { $th0[$t.Id] = @($t.UserProcessorTime, $t.PrivilegedProcessorTime) } catch {} }
    $pidKey = "pid_$($p.Id)_"
    $job = Start-Job -ArgumentList $pidKey, $sec -ScriptBlock {
        param($k, $s)
        $all = Get-Counter '\GPU Engine(*)\Utilization Percentage' -SampleInterval 1 -MaxSamples $s -EA SilentlyContinue
        $per = @{}
        foreach ($set in $all) { foreach ($cs in $set.CounterSamples) { if ($cs.InstanceName -like "*$k*") { $e = ($cs.InstanceName -replace '.*engtype_', ''); if (-not $per[$e]) { $per[$e] = 0.0 }; $per[$e] += $cs.CookedValue } } }
        $n = [math]::Max(1, $all.Count); $best = 0.0
        foreach ($v in $per.Values) { $best = [math]::Max($best, $v / $n) }
        $best
    }
    if ($during) { & $during }
    Start-Sleep -Milliseconds ([int]($sec * 1000 - ((Get-Date) - $t0).TotalMilliseconds))
    $p.Refresh(); $cpu = ($p.TotalProcessorTime - $c0).TotalMilliseconds / ((Get-Date) - $t0).TotalMilliseconds * 100
    $el = ((Get-Date) - $t0).TotalMilliseconds
    $cy1 = [uint64]0; [void][Perf.Native]::QueryProcessCycleTime($p.Handle, [ref]$cy1); $mcps = ($cy1 - $cy0) / 1e6 / ($el / 1000)
    $th = foreach ($t in $p.Threads) { try { if ($th0.ContainsKey($t.Id)) {
        $u = ($t.UserProcessorTime - $th0[$t.Id][0]).TotalMilliseconds / $el * 100; $k = ($t.PrivilegedProcessorTime - $th0[$t.Id][1]).TotalMilliseconds / $el * 100
        [pscustomobject]@{ t = $u + $k; s = ('{0:N1}u+{1:N1}k' -f $u, $k) } } } catch {} }
    $top = ($th | Sort-Object t -Descending | Select-Object -First 3 | ForEach-Object { $_.s }) -join ' '
    $gpu = Receive-Job $job -Wait -AutoRemoveJob
    [pscustomobject]@{ mcyc = [math]::Round($mcps, 0); cpu = [math]::Round($cpu, 1); gpu = [math]::Round([double]($gpu | Select-Object -Last 1), 1); threads = $top }
}

function Last-Present([string]$after) {
    if ($Other -or -not (Test-Path "$Game\SWGBModern.log")) { return '' }
    $l = Get-Content "$Game\SWGBModern.log" | Where-Object { $_ -match 'present: [\d.]+ fps' } | Select-Object -Last 1
    $t = Get-Content "$Game\SWGBModern.log" | Where-Object { $_ -match 'present: time per 5s' } | Select-Object -Last 1
    $fps = if ($l -match 'present: ([\d.]+) fps') { [double]$Matches[1] } else { 0 }
    $ms = if ($t -match 'snapshot (\d+) ms, text \d+ ms, present (\d+) ms') { [math]::Round(([double]$Matches[1] + [double]$Matches[2]) / [math]::Max(1, $fps * 5), 2) } else { -1 }
    "fps $fps, presenter $ms ms/frame"
}

$results = @()
try {
    $cfg = "$orig" -replace '(?m)^Renderer=.*', "Renderer=$Renderer" -replace '(?m)^SmoothFog=.*', "SmoothFog=$Fog" -replace '(?m)^HDText=.*', "HDText=$Text" -replace '(?m)^Filter=.*', 'Filter=sharp'
    $cfg = $cfg -replace '(?m)^ThreadReport=.*', 'ThreadReport=1'
    if ($cfg -notmatch '(?m)^ThreadReport=') { $cfg = $cfg -replace '(?m)^\[Debug\]\s*$', "[Debug]`r`nThreadReport=1" }
    if ($cfg -notmatch '(?m)^\[Debug\]') { $cfg += "`r`n[Debug]`r`nThreadReport=1`r`n" }
    foreach ($kv in ($Set -split ';' | Where-Object { $_ -match '=' })) { $k, $v = $kv -split '=', 2; $cfg = $cfg -replace "(?m)^$k=.*", "$k=$v" }
    if (-not $Other) { $cfg | Set-Content $ini -NoNewline }
    for ($r = 1; $r -le $Runs; $r++) {
        Get-Process battlegrounds_x1 -EA SilentlyContinue | Stop-Process -Force; Start-Sleep 1
        $p = Start-Process "$Game\battlegrounds_x1.exe" -ArgumentList NOSTARTUP -WorkingDirectory $Game -PassThru; Start-Sleep 9
        if ($Other) { & $i key 0x1B sleep 1500 key 0x1B sleep 1500 key 0x1B sleep 4000 click 2400 1700 sleep 800 click 1790 772 sleep 800 }
        & $i move 2400 1700
        $m = Measure-Scene $p $Seconds $null; $m | Add-Member scene menu; $m | Add-Member info (Last-Present); $results += $m
        # Episode I campaign, mission 1 (the same map every run): skip the briefing, close Objectives
        & $i click 2400 1700 sleep 800 click 1790 772 sleep 2500 click 2000 1075 sleep 300 key 0x54 key 0x45 key 0x53 key 0x54 sleep 300 click 1646 1272 sleep 2500
        & $i click 2650 402 sleep 2500 click 1456 444 sleep 2500 click 884 520 sleep 3000 click 1626 1730 sleep 6000 key 0x1B sleep 4000 click $OkX $OkY sleep 3000
        & $i move 1920 700
        $m = Measure-Scene $p $Seconds $null; $m | Add-Member scene game; $m | Add-Member info (Last-Present); $results += $m
        $m = Measure-Scene $p $Seconds { & $i move 3839 700 sleep 2000 move 0 700 sleep 2000 move 3839 700 sleep 2000 move 0 700 sleep 2000 move 1920 700 }
        $m | Add-Member scene scroll; $m | Add-Member info (Last-Present); $results += $m
        $alive = [bool](Get-Process -Id $p.Id -EA SilentlyContinue)
        Stop-Process -Id $p.Id -Force -EA SilentlyContinue
        if (-not $alive) { Write-Output "run $r : game exited early" }
    }
} finally {
    Get-Process battlegrounds_x1 -EA SilentlyContinue | Stop-Process -Force
    Start-Sleep 1
    if ($orig -ne $null -and -not $Other) { Set-Content $ini $orig -NoNewline }
}
$results | Group-Object scene | ForEach-Object {
    $c = ($_.Group | Measure-Object cpu -Average).Average; $g = ($_.Group | Measure-Object gpu -Average).Average
    $mc = $_.Group | Measure-Object mcyc -Average -Minimum -Maximum
    "{0,-6} {1,-7} {2,5:N0} Mcycles/s ({3:N0}-{4:N0})  cpu {5,5:N1}%  gpu {6,5:N1}%  threads {7,-34} {8}" -f $Tag, $_.Name, $mc.Average, $mc.Minimum, $mc.Maximum, $c, $g, ($_.Group | Select-Object -Last 1).threads, ($_.Group | Select-Object -Last 1).info
}
