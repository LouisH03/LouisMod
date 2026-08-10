param(
    [ValidateRange(1, 32)]
    [int]$InstanceCount = 7,
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$ReplayPath
)

$ErrorActionPreference = "Stop"

$scriptDir = $PSScriptRoot
$productDir = Split-Path -Parent $scriptDir
$logFile = Join-Path $productDir "multibruteforce.log"
$pidFile = Join-Path $scriptDir "bf_pids.txt"
$countFile = Join-Path $scriptDir "bf_instance_count.txt"
$validationDirsFile = Join-Path $scriptDir "bf_validation_dirs.txt"
$launcherFile = Join-Path $scriptDir "bf_launcher.txt"
$cancelFile = Join-Path $scriptDir "bf_cancel.txt"
$legacyActiveFile = Join-Path $scriptDir "bf_active.txt"
$legacyNextIndexFile = Join-Path $scriptDir "bf_next_result_index.txt"
$legacyLastValidationFile = Join-Path $scriptDir "last_validation_path.txt"
$resultsViewerMutexName = "Local\LouisMod.ResultsViewer"
$launcherMutexName = "Local\LouisMod.MultiBruteforceLauncher"
$closerMutexName = "Local\LouisMod.MultiBruteforceCloser"
$launcherMutex = $null
$ownsLauncherMutex = $false
$stagedReplays = @()
$newWorkers = @()
$previousResultCount = 0
$sessionStateInitialized = $false
$launcherStartTicks = (Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks
$launcherRecord = "{0}|{1}" -f $PID, $launcherStartTicks

$documentsFolder = $null
$localAppData = $null
$loaderDirectory = $null
$loaderExecutable = $null
$resultsDirectory = $null
$replayRoots = @()

function Write-RunLog {
    param([string]$Message)

    try {
        Add-Content -LiteralPath $logFile -Value (
            "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"),
            $Message) -Encoding UTF8 -ErrorAction Stop
    }
    catch {
        # Logging must never prevent a launch or cleanup operation.
    }
}

function Get-CancellationTicks {
    $value = 0L
    if (Test-Path -LiteralPath $cancelFile -PathType Leaf) {
        [void][long]::TryParse(
            (Get-Content -LiteralPath $cancelFile -TotalCount 1),
            [ref]$value)
    }
    return $value
}

function Test-LaunchCancelled {
    (Get-CancellationTicks) -ge $launcherStartTicks
}

function Assert-LaunchNotCancelled {
    if (Test-LaunchCancelled) {
        throw [System.OperationCanceledException]::new(
            "The Multi-Bruteforce launch was cancelled.")
    }
}

function Test-CloserRunning {
    try {
        $mutex = [System.Threading.Mutex]::OpenExisting($closerMutexName)
        $mutex.Dispose()
        return $true
    }
    catch {
        return $false
    }
}

function Remove-LauncherRecordIfOwned {
    if (-not (Test-Path -LiteralPath $launcherFile -PathType Leaf)) {
        return
    }

    $current = Get-Content -LiteralPath $launcherFile -TotalCount 1 `
        -ErrorAction SilentlyContinue
    if ([string]::Equals(
            $current,
            $launcherRecord,
            [System.StringComparison]::Ordinal)) {
        Remove-Item -LiteralPath $launcherFile -Force `
            -ErrorAction SilentlyContinue
    }
}

function Test-SafeStagingDirectory {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $false
    }

    try {
        $fullPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
        $name = [System.IO.Path]::GetFileName($fullPath)
        $parent = [System.IO.Path]::GetDirectoryName($fullPath)
    }
    catch {
        return $false
    }

    if (-not $name.StartsWith(
            ".LouisModBF_",
            [System.StringComparison]::OrdinalIgnoreCase)) {
        return $false
    }

    foreach ($root in $replayRoots) {
        if ([string]::Equals(
                $parent,
                $root,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}

function Remove-SafeStagingDirectory {
    param([string]$Path)

    if (-not (Test-SafeStagingDirectory $Path)) {
        Write-RunLog "Ignored unsafe staging path: $Path"
        return
    }

    if (Test-Path -LiteralPath $Path -PathType Container) {
        Remove-Item -LiteralPath $Path -Recurse -Force
        Write-RunLog "Removed staged replay directory: $Path"
    }
}

function Clear-TrackedStagingDirectories {
    if (-not (Test-Path -LiteralPath $validationDirsFile -PathType Leaf)) {
        return
    }

    foreach ($directory in @(
            Get-Content -LiteralPath $validationDirsFile |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Select-Object -Unique)) {
        Remove-SafeStagingDirectory $directory
    }
    Remove-Item -LiteralPath $validationDirsFile -Force
}

function Unregister-StagingDirectory {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $validationDirsFile -PathType Leaf)) {
        return
    }

    $remaining = @(
        Get-Content -LiteralPath $validationDirsFile |
        Where-Object {
            -not [string]::Equals(
                $_,
                $Path,
                [System.StringComparison]::OrdinalIgnoreCase)
        }
    )
    if ($remaining.Count -eq 0) {
        Remove-Item -LiteralPath $validationDirsFile -Force
    }
    else {
        Set-Content -LiteralPath $validationDirsFile `
            -Value $remaining -Encoding UTF8
    }
}

function Get-ReplayRoot {
    param([string]$Path)

    foreach ($root in $replayRoots) {
        $prefix = $root + [System.IO.Path]::DirectorySeparatorChar
        if ($Path.StartsWith(
                $prefix,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            return $root
        }
    }
    throw "The selected replay is outside the TrackMania replay folders: $Path"
}

function New-StagedReplay {
    param([string]$SourcePath)

    $replayRoot = Get-ReplayRoot $SourcePath
    $fileName = [System.IO.Path]::GetFileName($SourcePath)
    $directoryName = ".LouisModBF_{0}" -f `
        [Guid]::NewGuid().ToString("N")
    $directory = Join-Path $replayRoot $directoryName
    $destination = Join-Path $directory $fileName

    [System.IO.Directory]::CreateDirectory($directory) | Out-Null
    try {
        $attributes = [System.IO.File]::GetAttributes($directory)
        [System.IO.File]::SetAttributes(
            $directory,
            $attributes -bor [System.IO.FileAttributes]::Hidden)
        [System.IO.File]::Copy($SourcePath, $destination, $false)

        $files = @(Get-ChildItem -LiteralPath $directory -File -Force)
        if ($files.Count -ne 1 -or
            -not [string]::Equals(
                $files[0].Name,
                $fileName,
                [System.StringComparison]::Ordinal)) {
            throw "The validation directory does not contain exactly the selected replay."
        }

        Add-Content -LiteralPath $validationDirsFile -Value $directory `
            -Encoding UTF8
        return [PSCustomObject]@{
            SourcePath = $SourcePath
            Directory = $directory
            ValidatePath = $directoryName
            ResultStartIndex = 0
            ResultEndIndex = 0
            LaunchRequested = $false
            WorkerCount = 0
        }
    }
    catch {
        if (Test-SafeStagingDirectory $directory) {
            Remove-Item -LiteralPath $directory -Recurse -Force `
                -ErrorAction SilentlyContinue
        }
        throw
    }
}

function ConvertFrom-WorkerRecord {
    param([string]$Line)

    if ([string]::IsNullOrWhiteSpace($Line)) {
        return
    }

    $parts = $Line.Split('|')
    $processId = 0
    if (-not [int]::TryParse($parts[0], [ref]$processId)) {
        return
    }

    $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if (-not $process -or $process.ProcessName -ine "TmForever") {
        return
    }

    $startTicks = 0L
    if ($parts.Count -ge 2 -and
        [long]::TryParse($parts[1], [ref]$startTicks) -and
        $startTicks -gt 0) {
        try {
            if ($process.StartTime.ToUniversalTime().Ticks -ne $startTicks) {
                return
            }
        }
        catch {
            return
        }
    }
    else {
        try {
            $startTicks = $process.StartTime.ToUniversalTime().Ticks
        }
        catch {
            return
        }
    }

    $resultIndex = 0
    if ($parts.Count -ge 3) {
        [void][int]::TryParse($parts[2], [ref]$resultIndex)
    }

    [PSCustomObject]@{
        ProcessId = $processId
        StartTicks = $startTicks
        ResultIndex = $resultIndex
    }
}

function Get-LiveWorkers {
    if (-not (Test-Path -LiteralPath $pidFile -PathType Leaf)) {
        return @()
    }

    @(
        foreach ($line in Get-Content -LiteralPath $pidFile) {
            ConvertFrom-WorkerRecord $line
        }
    )
}

function Format-WorkerRecord {
    param($Worker)

    "{0}|{1}|{2}" -f `
        $Worker.ProcessId,
        $Worker.StartTicks,
        $Worker.ResultIndex
}

function Save-Workers {
    param([object[]]$Workers)

    if ($Workers.Count -eq 0) {
        Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
        return
    }

    $lines = @($Workers | ForEach-Object { Format-WorkerRecord $_ })
    Set-Content -LiteralPath $pidFile -Value $lines -Encoding ASCII
}

function Read-ResultCount {
    $value = 0
    if (Test-Path -LiteralPath $countFile -PathType Leaf) {
        [void][int]::TryParse(
            (Get-Content -LiteralPath $countFile -TotalCount 1),
            [ref]$value)
    }
    if ($value -lt 0 -or $value -gt 1024) {
        return 0
    }
    return $value
}

function Test-ResultsViewerRunning {
    try {
        $mutex = [System.Threading.Mutex]::OpenExisting(
            $resultsViewerMutexName)
        $mutex.Dispose()
        return $true
    }
    catch {}

    return @(
        Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowTitle -eq "Results Viewer" }
    ).Count -gt 0
}

function Start-ResultsViewer {
    param([int]$ResultCount)

    if (Test-ResultsViewerRunning) {
        return
    }

    $checkBatch = Join-Path $scriptDir "checkBf.bat"
    if (-not (Test-Path -LiteralPath $checkBatch -PathType Leaf)) {
        throw "Required viewer entry point was not found: $checkBatch"
    }

    $command = '"{0}" -InstanceCountOverride {1}' -f `
        $checkBatch,
        $ResultCount
    Start-Process `
        -FilePath $env:ComSpec `
        -ArgumentList @("/d", "/c", $command) `
        -WorkingDirectory $scriptDir | Out-Null
    Write-RunLog "Started the Results Viewer."
}

function Get-TmForeverProcesses {
    @(Get-Process -Name "TmForever" -ErrorAction SilentlyContinue)
}

function Wait-ForNewTmForeverProcess {
    param(
        [hashtable]$KnownProcessIds,
        [int]$TimeoutSeconds = 30
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $candidate = @(
            Get-TmForeverProcesses |
            Where-Object { -not $KnownProcessIds.ContainsKey($_.Id) } |
            Sort-Object StartTime
        ) | Select-Object -First 1
        if ($candidate) {
            return $candidate
        }
        Assert-LaunchNotCancelled
        Start-Sleep -Milliseconds 200
    }
    return $null
}

function New-WorkerRecord {
    param(
        [System.Diagnostics.Process]$Process,
        [int]$ResultIndex
    )

    $Process.Refresh()
    [PSCustomObject]@{
        ProcessId = $Process.Id
        StartTicks = $Process.StartTime.ToUniversalTime().Ticks
        ResultIndex = $ResultIndex
    }
}

function Arrange-TrackedWorkerWindows {
    param([object[]]$Workers)

    Assert-LaunchNotCancelled
    if ($Workers.Count -eq 0) {
        return
    }

    if (-not ("LouisModWindowTools" -as [type])) {
        Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public sealed class LouisModWindowInfo
{
    public long Handle;
    public string ClassName;
    public string Title;
}

public static class LouisModWindowTools
{
    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(
        EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(
        IntPtr hWnd, out uint processId);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextLengthW(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextW(
        IntPtr hWnd, StringBuilder text, int capacity);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassNameW(
        IntPtr hWnd, StringBuilder text, int capacity);

    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(
        IntPtr hWnd, IntPtr hWndInsertAfter,
        int X, int Y, int cx, int cy, uint uFlags);

    [DllImport("user32.dll")]
    private static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);

    public static LouisModWindowInfo[] GetVisibleWindows(int processId)
    {
        List<LouisModWindowInfo> windows =
            new List<LouisModWindowInfo>();
        EnumWindows(delegate(IntPtr hWnd, IntPtr lParam)
        {
            uint owner;
            GetWindowThreadProcessId(hWnd, out owner);
            if (owner != (uint)processId || !IsWindowVisible(hWnd))
                return true;

            int titleLength = GetWindowTextLengthW(hWnd);
            StringBuilder title = new StringBuilder(titleLength + 1);
            StringBuilder className = new StringBuilder(256);
            GetWindowTextW(hWnd, title, title.Capacity);
            GetClassNameW(hWnd, className, className.Capacity);
            windows.Add(new LouisModWindowInfo
            {
                Handle = hWnd.ToInt64(),
                ClassName = className.ToString(),
                Title = title.ToString()
            });
            return true;
        }, IntPtr.Zero);
        return windows.ToArray();
    }

    public static bool ParkGameWindow(long handle)
    {
        IntPtr window = new IntPtr(handle);
        ShowWindowAsync(window, 4);
        return SetWindowPos(
            window,
            new IntPtr(-2),
            0,
            0,
            1,
            1,
            0x0010u | 0x4000u);
    }

    public static bool ParkBruteforceWindow(long handle)
    {
        return SetWindowPos(
            new IntPtr(handle),
            new IntPtr(-2),
            0,
            0,
            0,
            0,
            0x0010u | 0x4000u);
    }
}
"@
    }

    $pending = @{}
    foreach ($worker in $Workers) {
        $pending[$worker.ProcessId] = $worker
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    while ($pending.Count -gt 0 -and [DateTime]::UtcNow -lt $deadline) {
        Assert-LaunchNotCancelled
        foreach ($processId in @($pending.Keys)) {
            $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
            if (-not $process) {
                Write-RunLog "Tracked worker PID $processId has exited."
                $pending.Remove($processId)
                continue
            }

            try {
                if ($process.StartTime.ToUniversalTime().Ticks -ne
                    $pending[$processId].StartTicks) {
                    Write-RunLog "Ignored reused PID $processId."
                    $pending.Remove($processId)
                    continue
                }
            }
            catch {
                $pending.Remove($processId)
                continue
            }

            $windows = @(
                [LouisModWindowTools]::GetVisibleWindows($processId))
            $gameWindows = @(
                $windows |
                Where-Object { $_.ClassName -eq "TmForever" })
            $bruteforceWindows = @(
                $windows |
                Where-Object {
                    $_.ClassName -eq "ConsoleWindowClass" -and
                    $_.Title -match
                        '(?i)(best time:|bruteforce|attempts/second:|iterations:)'
                })

            foreach ($window in $gameWindows) {
                [LouisModWindowTools]::ParkGameWindow(
                    $window.Handle) | Out-Null
            }

            if ($bruteforceWindows.Count -eq 0) {
                continue
            }

            foreach ($window in $bruteforceWindows) {
                [LouisModWindowTools]::ParkBruteforceWindow(
                    $window.Handle) | Out-Null
            }
            Write-RunLog ((
                "Parked worker PID {0} before bruteforce: " +
                "{1} game window(s), " +
                "{2} bruteforce window(s).") -f
                $processId,
                $gameWindows.Count,
                $bruteforceWindows.Count)
            $pending.Remove($processId)
        }

        if ($pending.Count -gt 0) {
            Start-Sleep -Milliseconds 250
        }
    }

    foreach ($processId in @($pending.Keys)) {
        Write-RunLog "Worker PID $processId did not expose a bruteforce window within 120 seconds; left visible."
    }

    $relevantWindowCount = 0
    foreach ($process in Get-TmForeverProcesses) {
        $relevantWindowCount += @(
            [LouisModWindowTools]::GetVisibleWindows($process.Id) |
            Where-Object {
                $_.ClassName -eq "TmForever" -or
                ($_.ClassName -eq "ConsoleWindowClass" -and
                 $_.Title -match
                    '(?i)(best time:|bruteforce|attempts/second:|iterations:)')
            }
        ).Count
    }
    Write-RunLog "Tracked $relevantWindowCount open TrackMania-related windows."
}

try {
    $documentsFolder = [Environment]::GetFolderPath(
        [Environment+SpecialFolder]::MyDocuments)
    if ([string]::IsNullOrWhiteSpace($documentsFolder)) {
        throw "The Documents directory could not be resolved."
    }

    $localAppData = [Environment]::GetFolderPath(
        [Environment+SpecialFolder]::LocalApplicationData)
    if ([string]::IsNullOrWhiteSpace($localAppData)) {
        throw "The local application-data directory could not be resolved."
    }

    $loaderDirectory = Join-Path $localAppData "TMLoader"
    $loaderExecutable = Join-Path $loaderDirectory "TMLoader.exe"
    $resultsDirectory = Join-Path $documentsFolder "TMInterface\Scripts"
    $replayRoots = @(
        (Join-Path $documentsFolder "TrackMania\Tracks\Replays"),
        (Join-Path $documentsFolder "TmForever\Tracks\Replays")
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Container } |
        ForEach-Object { [System.IO.Path]::GetFullPath($_).TrimEnd('\') }

    if (-not (Test-Path -LiteralPath $loaderExecutable -PathType Leaf)) {
        throw "TMLoader was not found: $loaderExecutable"
    }
    if (-not (Test-Path -LiteralPath $resultsDirectory -PathType Container)) {
        throw "TMInterface's Scripts directory was not found: $resultsDirectory"
    }
    if ($replayRoots.Count -eq 0) {
        throw "No TrackMania replay directory was found."
    }

    $selectedReplayPaths = @(
        foreach ($candidate in $ReplayPath.Split([char]'|')) {
            if ([string]::IsNullOrWhiteSpace($candidate)) {
                continue
            }

            $selectedReplayPath = [System.IO.Path]::GetFullPath($candidate)
            if (-not (Test-Path -LiteralPath $selectedReplayPath -PathType Leaf)) {
                throw "The selected replay does not exist: $selectedReplayPath"
            }
            if (-not $selectedReplayPath.EndsWith(
                    ".Replay.Gbx",
                    [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "The selected file is not a .Replay.Gbx: $selectedReplayPath"
            }
            [void](Get-ReplayRoot $selectedReplayPath)
            $selectedReplayPath
        }
    )
    if ($selectedReplayPaths.Count -lt 1 -or
        $selectedReplayPaths.Count -gt 32) {
        throw "Select between 1 and 32 replay files."
    }
    Write-RunLog (
        "Received {0} selected replay(s), with {1} worker(s) per replay." -f
        $selectedReplayPaths.Count,
        $InstanceCount)

    if (Test-CloserRunning) {
        throw [System.OperationCanceledException]::new(
            "Multi-Bruteforce cleanup is already running.")
    }

    $launcherMutex = [System.Threading.Mutex]::new($false, $launcherMutexName)
    try {
        $ownsLauncherMutex = $launcherMutex.WaitOne()
    }
    catch [System.Threading.AbandonedMutexException] {
        $ownsLauncherMutex = $true
    }
    if (-not $ownsLauncherMutex) {
        throw "The Multi-Bruteforce launcher mutex could not be acquired."
    }

    if (Test-CloserRunning) {
        throw [System.OperationCanceledException]::new(
            "Multi-Bruteforce cleanup is already running.")
    }
    Assert-LaunchNotCancelled
    Set-Content -LiteralPath $launcherFile -Value $launcherRecord `
        -Encoding ASCII
    Assert-LaunchNotCancelled

    $liveWorkers = @(Get-LiveWorkers)
    if ($liveWorkers.Count -eq 0) {
        Clear-TrackedStagingDirectories
        Remove-Item -LiteralPath $pidFile,$legacyActiveFile,`
            $legacyNextIndexFile,$legacyLastValidationFile `
            -Force -ErrorAction SilentlyContinue
        $previousResultCount = 0
        Write-RunLog "Started a fresh Multi-Bruteforce session."
    }
    else {
        Save-Workers $liveWorkers
        $previousResultCount = Read-ResultCount
        $highestTrackedIndex = @(
            $liveWorkers.ResultIndex |
            Where-Object { $_ -gt 0 } |
            Measure-Object -Maximum
        ).Maximum
        if ($highestTrackedIndex -gt $previousResultCount) {
            $previousResultCount = $highestTrackedIndex
        }
        Write-RunLog "Adding workers to the active Multi-Bruteforce session."
    }
    $sessionStateInitialized = $true

    $resultStartIndex = $previousResultCount + 1
    $requestedWorkerCount = $selectedReplayPaths.Count * $InstanceCount
    $resultEndIndex = $previousResultCount + $requestedWorkerCount
    if ($resultEndIndex -gt 1024) {
        throw "The Results Viewer supports at most 1024 cumulative workers."
    }

    $nextReplayResultIndex = $resultStartIndex
    for ($replayOffset = 0;
         $replayOffset -lt $selectedReplayPaths.Count;
         ++$replayOffset) {
        Assert-LaunchNotCancelled
        $stagedReplay = New-StagedReplay $selectedReplayPaths[$replayOffset]
        $stagedReplay.ResultStartIndex = $nextReplayResultIndex
        $stagedReplay.ResultEndIndex =
            $nextReplayResultIndex + $InstanceCount - 1
        $stagedReplays += $stagedReplay
        $nextReplayResultIndex = $stagedReplay.ResultEndIndex + 1

        Write-RunLog ((
            "Staged replay {0}/{1} without renaming it for " +
            "LM-{2} through LM-{3}: {4}") -f
            ($replayOffset + 1),
            $selectedReplayPaths.Count,
            $stagedReplay.ResultStartIndex,
            $stagedReplay.ResultEndIndex,
            $stagedReplay.Directory)
    }

    Set-Content -LiteralPath $countFile `
        -Value $resultEndIndex -Encoding ASCII
    Start-ResultsViewer $resultEndIndex

    $knownProcessIds = @{}
    foreach ($process in Get-TmForeverProcesses) {
        $knownProcessIds[$process.Id] = $true
    }

    if ($liveWorkers.Count -gt 0) {
        Arrange-TrackedWorkerWindows -Workers $liveWorkers
    }

    foreach ($stagedReplay in $stagedReplays) {
        for ($resultIndex = $stagedReplay.ResultStartIndex;
             $resultIndex -le $stagedReplay.ResultEndIndex;
             ++$resultIndex) {
            Assert-LaunchNotCancelled
            $arguments = `
                'run TmForever "TAS" /configstring="set bf_result_filename LM-{0}.txt" /validatepath="{1}"' -f `
                $resultIndex,
                $stagedReplay.ValidatePath

            $loaderProcess = Start-Process `
                -FilePath $loaderExecutable `
                -ArgumentList $arguments `
                -WorkingDirectory $loaderDirectory `
                -PassThru
            $stagedReplay.LaunchRequested = $true

            try {
                $process = Wait-ForNewTmForeverProcess $knownProcessIds
            }
            catch [System.OperationCanceledException] {
                try {
                    if ($loaderProcess -and -not $loaderProcess.HasExited) {
                        Stop-Process -Id $loaderProcess.Id -Force `
                            -ErrorAction SilentlyContinue
                    }
                }
                catch {}
                throw
            }
            if (-not $process) {
                throw "TMLoader did not create worker LM-$resultIndex within 30 seconds."
            }

            $worker = New-WorkerRecord $process $resultIndex
            $newWorkers += $worker
            $liveWorkers += $worker
            $stagedReplay.WorkerCount += 1
            Save-Workers $liveWorkers
            Assert-LaunchNotCancelled

            # The game window must be restored and parked before its
            # bruteforce console appears. Minimizing it here prevents
            # bruteforce startup.
            Arrange-TrackedWorkerWindows -Workers @($worker)

            foreach ($runningProcess in Get-TmForeverProcesses) {
                $knownProcessIds[$runningProcess.Id] = $true
            }
            Write-RunLog (
                "Launched LM-$resultIndex as PID $($worker.ProcessId) " +
                "for $([System.IO.Path]::GetFileName($stagedReplay.SourcePath)).")
        }
    }

    $liveWorkers = @(Get-LiveWorkers)
    Save-Workers $liveWorkers
    Write-RunLog ((
        "Multi-Bruteforce is running {0} replay(s) with result files " +
        "LM-{1}.txt through LM-{2}.txt.") -f
        $selectedReplayPaths.Count,
        $resultStartIndex,
        $resultEndIndex)
}
catch {
    $cancelled = $_.Exception -is [System.OperationCanceledException]
    try {
        if ($cancelled) {
            Write-RunLog "CANCELLED: $($_.Exception.Message)"
        }
        else {
            Write-RunLog "ERROR: $($_.Exception.Message)"
        }
    }
    catch {}

    foreach ($stagedReplay in $stagedReplays) {
        if (-not $stagedReplay.LaunchRequested) {
            Remove-SafeStagingDirectory $stagedReplay.Directory
            Unregister-StagingDirectory $stagedReplay.Directory
        }
    }

    if ($sessionStateInitialized) {
        $successfulCount = $previousResultCount + $newWorkers.Count
        if ($successfulCount -gt 0) {
            Set-Content -LiteralPath $countFile `
                -Value $successfulCount -Encoding ASCII `
                -ErrorAction SilentlyContinue
        }
        else {
            Remove-Item -LiteralPath $countFile -Force `
                -ErrorAction SilentlyContinue
        }
    }
    exit 1
}
finally {
    Remove-LauncherRecordIfOwned
    if ($launcherMutex) {
        if ($ownsLauncherMutex) {
            try {
                $launcherMutex.ReleaseMutex()
            }
            catch {}
        }
        $launcherMutex.Dispose()
    }
}
