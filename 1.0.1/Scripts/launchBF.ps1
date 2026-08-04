param(
    [int]$InstanceCountOverride = 0
)

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class GameWindowTools
{
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SetWindowPos(
        IntPtr hWnd,
        IntPtr hWndInsertAfter,
        int X,
        int Y,
        int cx,
        int cy,
        uint uFlags
    );

    [DllImport("user32.dll")]
    public static extern bool ShowWindowAsync(
        IntPtr hWnd,
        int nCmdShow
    );
}
"@

. (Join-Path $PSScriptRoot "bf_config.ps1")

if ($InstanceCountOverride -gt 0) {
    $InstanceCount = $InstanceCountOverride
}

if ($InstanceCount -lt 1 -or $InstanceCount -gt 32) {
    throw "Instance count must be between 1 and 32."
}

$path_to_tm = Join-Path $env:USERPROFILE "AppData\Local\TMLoader"
$pidFile = Join-Path $PSScriptRoot "bf_pids.txt"

if (Test-Path $pidFile) {
    Remove-Item $pidFile
}

$capturedPids = @()

for ($i = 1; $i -le $InstanceCount; $i++) {
    $before = @(
        Get-Process -Name "TmForever" -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Id
    )

    Push-Location $path_to_tm

    $exe = Join-Path $path_to_tm "TMLoader.exe"
    $args = "run TmForever `"TAS`" /configstring=`"set bf_result_filename a$i.txt`" /validatepath=Bruteforce"

    Start-Process -FilePath $exe -ArgumentList $args

    Pop-Location

    # Wait for the new TmForever process to appear
    $newPid = $null
    $timeout = 15
    $elapsed = 0

    while (-not $newPid -and $elapsed -lt $timeout) {
        Start-Sleep -Milliseconds 500
        $elapsed += 0.5

        $after = @(
            Get-Process -Name "TmForever" -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty Id
        )

        $diff = $after | Where-Object {
            $before -notcontains $_
        }

        if ($diff) {
            $newPid = $diff | Select-Object -First 1
        }
    }

    if ($newPid) {
        Write-Host "Instance $i launched with PID $newPid"
        $capturedPids += $newPid
    }
    else {
        Write-Host "Instance $i -- could not detect new PID within timeout"
    }

    Start-Sleep -Milliseconds 50
}


# ============================================================
# Allow normal instances to initialise, then move them to the tiny worker
# position. These are the first $InstanceCount instances; the extra results
# window below keeps its own size and position.
# ============================================================
$initialisationDelaySeconds = 12

Write-Host "Waiting $initialisationDelaySeconds seconds for brute-force instances to initialise..."
Start-Sleep -Seconds $initialisationDelaySeconds

foreach ($tmPid in $capturedPids) {
    $tmProcess = Get-Process -Id $tmPid -ErrorAction SilentlyContinue

    if (-not $tmProcess) {
        Write-Host "TmForever PID $tmPid is no longer running."
        continue
    }

    $tmProcess.Refresh()
    $windowHandle = $tmProcess.MainWindowHandle

    if ($windowHandle -ne [IntPtr]::Zero) {
        # HWND_NOTOPMOST = -2; SWP_NOACTIVATE = 0x0010
        [GameWindowTools]::SetWindowPos(
            $windowHandle,
            [IntPtr](-2),
            0,
            0,
            1,
            1,
            0x0010
        ) | Out-Null

        Write-Host "Moved normal TmForever instance PID $tmPid to X=0, Y=0, size 1x1."
    }
    else {
        Write-Host "Could not find the window for TmForever PID $tmPid."
    }
}


# ============================================================
# Extra instance window settings
# Change these four values whenever you want
# ============================================================
$extraWindowX      = 0
$extraWindowY      = 250
$extraWindowWidth  = 1500
$extraWindowHeight = 780

# Set this to $true to keep the extra instance always on top
$extraAlwaysOnTop = $false


# Record existing processes before launching the extra instance
$beforeExtra = @(
    Get-Process -Name "TmForever" -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty Id
)

Push-Location $path_to_tm

$exe = Join-Path $path_to_tm "TMLoader.exe"
$args = "run TmForever `"TAS`" /configstring=`"set bf_result_filename extra.txt; load results.txt`""

Start-Process -FilePath $exe -ArgumentList $args

Pop-Location


# Wait for the extra process and its window to become available
$extraProcess = $null
$timeoutSeconds = 20
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

while (
    -not $extraProcess -and
    $stopwatch.Elapsed.TotalSeconds -lt $timeoutSeconds
) {
    Start-Sleep -Milliseconds 250

    $newProcesses = @(
        Get-Process -Name "TmForever" -ErrorAction SilentlyContinue |
        Where-Object {
            $beforeExtra -notcontains $_.Id
        }
    )

    foreach ($process in $newProcesses) {
        $process.Refresh()

        if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
            $extraProcess = $process
            break
        }
    }
}

$stopwatch.Stop()

if ($extraProcess) {
    if ($extraAlwaysOnTop) {
        $insertAfter = [IntPtr](-1) # HWND_TOPMOST
    }
    else {
        $insertAfter = [IntPtr](-2) # HWND_NOTOPMOST
    }

    # SWP_NOACTIVATE: do not steal keyboard focus
    $flags = 0x0010

    [GameWindowTools]::SetWindowPos(
        $extraProcess.MainWindowHandle,
        $insertAfter,
        $extraWindowX,
        $extraWindowY,
        $extraWindowWidth,
        $extraWindowHeight,
        $flags
    ) | Out-Null

    Write-Host "Extra instance launched with results.txt loaded."
    Write-Host "Extra window positioned at X=$extraWindowX, Y=$extraWindowY, size ${extraWindowWidth}x${extraWindowHeight}."
}
else {
    Write-Host "Extra instance launched, but its window could not be detected within $timeoutSeconds seconds."
}


$capturedPids | Out-File -FilePath $pidFile
Write-Host "Saved PIDs to $pidFile"
