$ErrorActionPreference = "Stop"

$scriptDir = $PSScriptRoot
$productDir = Split-Path -Parent $scriptDir
$logFile = Join-Path $productDir "multibruteforce.log"
$pidFile = Join-Path $scriptDir "bf_pids.txt"
$validationDirsFile = Join-Path $scriptDir "bf_validation_dirs.txt"
$launcherFile = Join-Path $scriptDir "bf_launcher.txt"
$cancelFile = Join-Path $scriptDir "bf_cancel.txt"
$legacyFiles = @(
    (Join-Path $scriptDir "bf_active.txt"),
    (Join-Path $scriptDir "bf_next_result_index.txt"),
    (Join-Path $scriptDir "last_validation_path.txt")
)
$launcherMutexName = "Local\LouisMod.MultiBruteforceLauncher"
$closerMutexName = "Local\LouisMod.MultiBruteforceCloser"
$launcherMutex = $null
$ownsLauncherMutex = $false
$closerMutex = $null
$ownsCloserMutex = $false

$documentsFolder = $null
$replayRoots = @()

function Write-RunLog {
    param([string]$Message)

    try {
        Add-Content -LiteralPath $logFile -Value (
            "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"),
            $Message) -Encoding UTF8 -ErrorAction Stop
    }
    catch {
        # Cleanup must continue even if logging is unavailable.
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

function Get-TrackedWorker {
    param([string]$Line)

    if ([string]::IsNullOrWhiteSpace($Line)) {
        return
    }

    $parts = $Line.Split('|')
    if ($parts.Count -lt 2) {
        return
    }

    $processId = 0
    $expectedStartTicks = 0L
    if (-not [int]::TryParse($parts[0], [ref]$processId) -or
        -not [long]::TryParse($parts[1], [ref]$expectedStartTicks) -or
        $expectedStartTicks -le 0) {
        return
    }

    $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if (-not $process -or $process.ProcessName -ine "TmForever") {
        return
    }

    try {
        if ($process.StartTime.ToUniversalTime().Ticks -ne
            $expectedStartTicks) {
            return
        }
    }
    catch {
        return
    }
    return $process
}

function Get-TrackedLauncher {
    if (-not (Test-Path -LiteralPath $launcherFile -PathType Leaf)) {
        return
    }

    $line = Get-Content -LiteralPath $launcherFile -TotalCount 1 `
        -ErrorAction SilentlyContinue
    if ([string]::IsNullOrWhiteSpace($line)) {
        return
    }

    $parts = $line.Split('|')
    $processId = 0
    $expectedStartTicks = 0L
    if ($parts.Count -lt 2 -or
        -not [int]::TryParse($parts[0], [ref]$processId) -or
        -not [long]::TryParse($parts[1], [ref]$expectedStartTicks) -or
        $expectedStartTicks -le 0) {
        return
    }

    $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if (-not $process -or $process.ProcessName -ine "powershell") {
        return
    }

    try {
        if ($process.StartTime.ToUniversalTime().Ticks -ne
            $expectedStartTicks) {
            return
        }
    }
    catch {
        return
    }
    return $process
}

try {
    $closerCreatedNew = $false
    $closerMutex = [System.Threading.Mutex]::new(
        $true,
        $closerMutexName,
        [ref]$closerCreatedNew)
    if (-not $closerCreatedNew) {
        Write-RunLog "Ignored a repeated close request; cleanup is already running."
        exit 0
    }
    $ownsCloserMutex = $true

    $documentsFolder = [Environment]::GetFolderPath(
        [Environment+SpecialFolder]::MyDocuments)
    if ([string]::IsNullOrWhiteSpace($documentsFolder)) {
        throw "The Documents directory could not be resolved."
    }
    $replayRoots = @(
        (Join-Path $documentsFolder "TrackMania\Tracks\Replays"),
        (Join-Path $documentsFolder "TmForever\Tracks\Replays")
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Container } |
        ForEach-Object { [System.IO.Path]::GetFullPath($_).TrimEnd('\') }

    Write-RunLog "Close requested; cancelling active and queued launches."
    Set-Content -LiteralPath $cancelFile `
        -Value ([DateTime]::UtcNow.Ticks) -Encoding ASCII

    $launcher = Get-TrackedLauncher
    if ($launcher) {
        Write-RunLog "Requested cancellation of launcher PID $($launcher.Id)."
        $launcher | Wait-Process -Timeout 3 -ErrorAction SilentlyContinue
        $launcher = Get-TrackedLauncher
        if ($launcher) {
            Write-RunLog "Force-stopping launcher PID $($launcher.Id)."
            Stop-Process -Id $launcher.Id -Force -ErrorAction SilentlyContinue
            $launcher | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
        }
    }

    $launcherMutex = [System.Threading.Mutex]::new(
        $false,
        $launcherMutexName)
    try {
        $ownsLauncherMutex = $launcherMutex.WaitOne(10000)
    }
    catch [System.Threading.AbandonedMutexException] {
        $ownsLauncherMutex = $true
    }
    if (-not $ownsLauncherMutex) {
        throw "Cleanup could not take ownership of the launcher within 10 seconds."
    }

    Remove-Item -LiteralPath $launcherFile -Force `
        -ErrorAction SilentlyContinue

    $workers = @()
    if (Test-Path -LiteralPath $pidFile -PathType Leaf) {
        $workers = @(
            foreach ($line in Get-Content -LiteralPath $pidFile) {
                Get-TrackedWorker $line
            }
        )
    }

    foreach ($worker in $workers) {
        Write-RunLog "Closing worker PID $($worker.Id)."
        Stop-Process -Id $worker.Id -Force -ErrorAction SilentlyContinue
    }

    if ($workers.Count -gt 0) {
        $workers | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
    }

    if (Test-Path -LiteralPath $validationDirsFile -PathType Leaf) {
        foreach ($directory in @(
                Get-Content -LiteralPath $validationDirsFile |
                Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
                Select-Object -Unique)) {
            if (-not (Test-SafeStagingDirectory $directory)) {
                Write-RunLog "Ignored unsafe staging path: $directory"
                continue
            }
            if (Test-Path -LiteralPath $directory -PathType Container) {
                try {
                    Remove-Item -LiteralPath $directory -Recurse -Force
                    Write-RunLog "Removed staged replay directory: $directory"
                }
                catch {
                    Write-RunLog (
                        "ERROR removing staged replay directory " +
                        "${directory}: $($_.Exception.Message)")
                }
            }
        }
    }

    Remove-Item -LiteralPath $pidFile,$validationDirsFile `
        -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $legacyFiles `
        -Force -ErrorAction SilentlyContinue
    Write-RunLog "Multi-Bruteforce cleanup completed."
}
catch {
    Write-RunLog "CLEANUP ERROR: $($_.Exception.Message)"
    exit 1
}
finally {
    if ($launcherMutex) {
        if ($ownsLauncherMutex) {
            try {
                $launcherMutex.ReleaseMutex()
            }
            catch {}
        }
        $launcherMutex.Dispose()
    }
    if ($closerMutex) {
        if ($ownsCloserMutex) {
            try {
                $closerMutex.ReleaseMutex()
            }
            catch {}
        }
        $closerMutex.Dispose()
    }
}
