param(
    [int]$InstanceCountOverride = 0
)

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class WindowTools
{
    [DllImport("kernel32.dll")]
    public static extern IntPtr GetConsoleWindow();

    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(
        IntPtr hWnd,
        IntPtr hWndInsertAfter,
        int X,
        int Y,
        int cx,
        int cy,
        uint uFlags
    );
}
"@

$consoleWindow = [WindowTools]::GetConsoleWindow()


if ($consoleWindow -ne [IntPtr]::Zero) {
    [WindowTools]::SetWindowPos(
        $consoleWindow,
        [IntPtr](-1),
        0,
        0,
        850,
        300,
        0x0010
    ) | Out-Null
}
# HWND_TOPMOST = -1
# SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
[WindowTools]::SetWindowPos(
    $consoleWindow,
    [IntPtr](-1),
    0,
    0,
    0,
    0,
    0x0001 -bor 0x0002 -bor 0x0010
) | Out-Null

. (Join-Path $PSScriptRoot "bf_config.ps1")
$documentsFolder = [Environment]::GetFolderPath([Environment+SpecialFolder]::MyDocuments)
if ([string]::IsNullOrWhiteSpace($documentsFolder)) {
    $documentsFolder = Join-Path $env:USERPROFILE "Documents"
}
$scriptDir = Join-Path $documentsFolder "TMInterface\Scripts"
$resultsFile = Join-Path $scriptDir "results.txt"

if ($InstanceCountOverride -gt 0) {
    $InstanceCount = $InstanceCountOverride
}

if ($InstanceCount -lt 1 -or $InstanceCount -gt 32) {
    throw "Instance count must be between 1 and 32."
}

# Enable ANSI color support in classic cmd.exe hosts
try {
    Add-Type -Name Win -Namespace Console -MemberDefinition '
        [DllImport("kernel32.dll")] public static extern bool GetConsoleMode(IntPtr h, out int mode);
        [DllImport("kernel32.dll")] public static extern bool SetConsoleMode(IntPtr h, int mode);
        [DllImport("kernel32.dll")] public static extern IntPtr GetStdHandle(int handle);
    '
    $stdout = [Console.Win]::GetStdHandle(-11)
    $mode = 0
    [Console.Win]::GetConsoleMode($stdout, [ref]$mode) | Out-Null
    [Console.Win]::SetConsoleMode($stdout, $mode -bor 0x0004) | Out-Null
} catch {}

# ANSI color codes (use [char]27 for ESC -- `e is not recognized in PowerShell 5.1)
$Esc    = [char]27
$Reset  = "$Esc[0m"
$Green  = "$Esc[38;2;93;202;165m"
$Red    = "$Esc[38;2;226;130;127m"
$Gray   = "$Esc[38;2;120;120;115m"
$Purple = "$Esc[38;2;127;119;221m"
$White  = "$Esc[38;2;230;230;230m"

function Get-Results {
    for ($i = 1; $i -le $InstanceCount; $i++) {
        $file = Join-Path $scriptDir "a$i.txt"
        if (Test-Path $file) {
            $line = Get-Content $file -TotalCount 1 -ErrorAction SilentlyContinue

            if ($line -match 'Found better trigger \d+:\s*\[time:\s*([\d.]+),\s*distance:\s*([\d.eE+-]+),\s*speed:\s*([\d.]+)km/h\].*?iterations:\s*(\d+)') {
                [PSCustomObject]@{
                    Label = "A$i"; File = $file; Mode = "Trigger"
                    Time = [double]$matches[1]; Distance = [double]$matches[2]; Speed = [double]$matches[3]
                    Angle = $null; Score = $null
                    Iterations = [int]$matches[4]
                    Status = "ok"
                }
            }
            elseif ($line -match 'distance/speed:\s*([\d.eE+-]+)\s*m,\s*([\d.]+)\s*km/h at\s*([\d.]+),\s*iterations:\s*(\d+)') {
                [PSCustomObject]@{
                    Label = "A$i"; File = $file; Mode = "Point"
                    Time = [double]$matches[3]; Distance = [double]$matches[1]; Speed = [double]$matches[2]
                    Angle = $null; Score = $null
                    Iterations = [int]$matches[4]
                    Status = "ok"
                }
            }
            elseif ($line -match 'Found lower checkpoint \d+:\s*([\d.]+)\s*\(([-\d.]+)\),\s*iterations:\s*(\d+)') {
                [PSCustomObject]@{
                    Label = "A$i"; File = $file; Mode = "Checkpoint"
                    Time = [double]$matches[1]; Distance = $null; Speed = $null
                    Angle = $null; Score = $null
                    Iterations = [int]$matches[3]
                    Status = "ok"
                }
            }
            elseif ($line -match 'Found better result:\s*([\d.eE+-]+)\s*m,\s*([\d.eE+-]+)\s*deg\s*\(score:\s*([\d.eE+-]+)\)\s*at\s*([\d.]+),\s*iterations:\s*(\d+)') {
                [PSCustomObject]@{
                    Label = "A$i"; File = $file; Mode = "CarLocation"
                    Time = [double]$matches[4]; Distance = [double]$matches[1]; Speed = $null
                    Angle = [double]$matches[2]; Score = [double]$matches[3]
                    Iterations = [int]$matches[5]
                    Status = "ok"
                }
            }
            elseif ($line -match 'Found lower precise time:\s*([\d.eE+-]+),\s*iterations:\s*(\d+)') {
                [PSCustomObject]@{
                    Label = "A$i"; File = $file; Mode = "Precise"
                    Time = [double]$matches[1]; Distance = $null; Speed = $null
                    Angle = $null; Score = $null; TimeRaw = $matches[1]
                    Iterations = [int]$matches[2]
                    Status = "ok"
                }
            }
            elseif ($line -match '^# base at (\d+): angle=([\d.eE+-]+), Distance=([\d.eE+-]+), Iteration=(\d+)') {
                [PSCustomObject]@{
                    Label = "A$i"; File = $file; Mode = "NosePos"
                    Time = [double]$matches[1] / 1000; Distance = [double]$matches[3]; Speed = $null
                    Angle = [double]$matches[2]; Score = $null
                    Iterations = [int]$matches[4]
                    Status = "ok"
                }
            }
			elseif ($line -match '^# base at (\d+): angle=([\d.eE+-]+), Speed=([\d.eE+-]+), Iteration=(\d+)') {
				[PSCustomObject]@{
					Label = "A$i"; File = $file; Mode = "NosePos+"
					Time = [double]$matches[1] / 1000; Distance = $null; Speed = [double]$matches[3]
					Angle = [double]$matches[2]; Score = $null
					Iterations = [int]$matches[4]
					Status = "ok"
				}
			}
            else {
                [PSCustomObject]@{
                    Label = "A$i"; File = $file; Mode = $null
                    Time = $null; Distance = $null; Speed = $null
                    Angle = $null; Score = $null
                    Iterations = $null
                    Status = "unparsable"
                }
            }
        } else {
            [PSCustomObject]@{
                Label = "A$i"; File = $file; Mode = $null
                Time = $null; Distance = $null; Speed = $null
                Angle = $null; Score = $null
                Iterations = $null
                Status = "notfound"
            }
        }
    }
}

function Pad($text, $width) {
    $text = "$text"
    # truncate with a trailing space guaranteed, so columns never run together
    if ($text.Length -ge $width) { return $text.Substring(0, $width - 1) + " " }
    return $text + (" " * ($width - $text.Length))
}

function Fmt($value, $decimals) {
    if ($null -eq $value) { return "" }
    return [math]::Round([double]$value, $decimals).ToString("F$decimals")
}

function Render($results, $sortMode, $status) {
    $lines = New-Object System.Collections.Generic.List[string]

    $keyHint = if ($InstanceCount -eq 1) { "[1]" } else { "[1-$InstanceCount]" }
    $lines.Add("${Purple}[D]${Reset} Dist  ${Purple}[S]${Reset} Speed  ${Purple}[A]${Reset} Angle  ${Purple}[R]${Reset} Score  ${Purple}[C]${Reset} Copy top  ${Purple}$keyHint${Reset} Copy result  ${Purple}[F]${Reset} Close BF  ${Purple}[Q]${Reset} Quit")
    $lines.Add("${Gray}Sort: ${White}$sortMode${Reset}")
    $lines.Add("")

    $header = (Pad "ID" 5) + (Pad "Mode" 12) + (Pad "Time" 16) + (Pad "Distance" 14) + (Pad "Speed" 10) + (Pad "Angle" 10) + (Pad "Score" 12) + (Pad "Iterations" 12)
    $lines.Add("${Gray}$header${Reset}")
    $lines.Add("${Gray}" + ("-" * 96) + "${Reset}")

    $okResults = @($results | Where-Object { $_.Status -eq "ok" })
    foreach ($r in $results) {
        $isBest = ($okResults.Count -gt 0 -and $r -eq $okResults[0])
        $isWorst = ($okResults.Count -gt 1 -and $r -eq $okResults[$okResults.Count - 1])

        if ($r.Status -eq "notfound") {
            $row = (Pad $r.Label 5) + "${Gray}[file not found]${Reset}"
            $lines.Add($row)
        } elseif ($r.Status -eq "unparsable") {
            $row = (Pad $r.Label 5) + "${Gray}[unparsable]${Reset}"
            $lines.Add($row)
        } else {
            $timeText = if ($r.Mode -eq "Precise") { $r.TimeRaw } else { Fmt $r.Time 2 }
            if ($r.Mode -eq "Precise") {
                $rowText = (Pad $r.Label 5) + (Pad $r.Mode 12) + (Pad $timeText 16) + (Pad $r.Iterations 12)
            } else {
                $rowText = (Pad $r.Label 5) + (Pad $r.Mode 12) + (Pad $timeText 16) + (Pad (Fmt $r.Distance 6) 14) + (Pad (Fmt $r.Speed 2) 10) + (Pad (Fmt $r.Angle 4) 10) + (Pad (Fmt $r.Score 6) 12) + (Pad $r.Iterations 12)
            }
            if ($isBest) {
                $lines.Add("${Green}$rowText${Reset}")
            } elseif ($isWorst) {
                $lines.Add("${Red}$rowText${Reset}")
            } else {
                $lines.Add($rowText)
            }
        }
    }

    $lines.Add("")
    if ($status) {
        $lines.Add("${Green}$status${Reset}")
    } else {
        $lines.Add("")
    }

    $width = [Console]::WindowWidth
    [Console]::SetCursorPosition(0, 0)
    foreach ($line in $lines) {
        # strip ansi codes to measure visible length for padding
        $visible = $line -replace "$Esc\[[0-9;]*m", ""
        $padCount = $width - 1 - $visible.Length
        if ($padCount -lt 0) { $padCount = 0 }
        Write-Host ($line + (" " * $padCount))
    }
}

function Sort-Results($results, $sortMode) {
    if ($sortMode -eq "Speed") {
        $results | Sort-Object Speed -Descending
    } elseif ($sortMode -eq "Angle") {
        $results | Sort-Object Angle
    } elseif ($sortMode -eq "Score") {
        $results | Sort-Object Score
    } else {
        $results | Sort-Object Time, Distance
    }
}

function Copy-Best($sortedResults) {
    $top = $sortedResults | Where-Object { $_.Status -eq "ok" } | Select-Object -First 1
    if ($top) {
        Copy-Item -Path $top.File -Destination $resultsFile -Force
        return "Copied $($top.Label) ($($top.File)) into results.txt at $(Get-Date -Format 'HH:mm:ss')"
    } else {
        return "No valid top result found -- results.txt not updated."
    }
}

function Copy-Specific($results, $index) {
    $target = $results | Where-Object { $_.Label -eq "A$index" } | Select-Object -First 1
    if ($target -and (Test-Path $target.File)) {
        Copy-Item -Path $target.File -Destination $resultsFile -Force
        return "Copied $($target.Label) ($($target.File)) into results.txt at $(Get-Date -Format 'HH:mm:ss')"
    } elseif ($target) {
        return "A$index file not found -- nothing to copy."
    } else {
        return "A$index not found."
    }
}

function Close-BF {
    $closeScript = Join-Path $PSScriptRoot "closeBF.ps1"
    if (-not (Test-Path -LiteralPath $closeScript)) {
        return "closeBF.ps1 was not found."
    }

    $null = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $closeScript 2>&1
    if ($LASTEXITCODE -eq 0) {
        return "closeBF completed at $(Get-Date -Format 'HH:mm:ss')."
    }

    return "closeBF failed with exit code $LASTEXITCODE."
}

$sortMode = "Distance"
$status = $null

[Console]::CursorVisible = $false
Clear-Host

$watcher = New-Object System.IO.FileSystemWatcher
$watcher.Path = $scriptDir
$watcher.Filter = "a*.txt"
$watcher.NotifyFilter = [System.IO.NotifyFilters]::LastWrite -bor [System.IO.NotifyFilters]::Size
$watcher.EnableRaisingEvents = $true

$needsRedraw = $true

$onChange = {
    $global:needsRedraw = $true
}
Register-ObjectEvent -InputObject $watcher -EventName Changed -Action $onChange | Out-Null
Register-ObjectEvent -InputObject $watcher -EventName Created -Action $onChange | Out-Null

try {
    while ($true) {
        if ($needsRedraw) {
            $results = Get-Results
            $sorted = Sort-Results $results $sortMode
            Render $sorted $sortMode $status
            $needsRedraw = $false
        }

        if ([Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true)
            $char = $key.KeyChar.ToString().ToLower()

            if ($char -match '^[1-9]$' -and [int]$char -le $InstanceCount) {
                $status = Copy-Specific $results ([int]$char)
                $needsRedraw = $true
            } else {
                switch ($char) {
                    "d" { $sortMode = "Distance"; $needsRedraw = $true }
                    "s" { $sortMode = "Speed"; $needsRedraw = $true }
                    "a" { $sortMode = "Angle"; $needsRedraw = $true }
                    "r" { $sortMode = "Score"; $needsRedraw = $true }
                    "c" {
                        $status = Copy-Best $sorted
                        $needsRedraw = $true
                    }
                    "f" {
                        $status = Close-BF
                        $needsRedraw = $true
                    }
                    "q" {
                        [Console]::CursorVisible = $true
                        exit
                    }
                }
            }
        }

        Start-Sleep -Milliseconds 100
    }
} finally {
    [Console]::CursorVisible = $true
    Get-EventSubscriber | Unregister-Event
    $watcher.Dispose()
}
