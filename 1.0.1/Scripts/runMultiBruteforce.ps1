param(
    [Parameter(Mandatory = $true)]
    [int]$GamePid,

    [int]$InstanceCount = 7
)

$ErrorActionPreference = "Stop"
$scriptDir = $PSScriptRoot
$logFile = Join-Path $scriptDir "multibruteforce.log"

if ($InstanceCount -lt 1 -or $InstanceCount -gt 32) {
    throw "Instance count must be between 1 and 32."
}

function Write-RunLog {
    param([string]$Message)

    $line = "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Message
    Add-Content -LiteralPath $logFile -Value $line
}

function Invoke-BatchFile {
    param(
        [string]$Name,
        [int]$InstanceCountOverride = 0
    )

    $batchFile = Join-Path $scriptDir $Name
    if (-not (Test-Path -LiteralPath $batchFile)) {
        throw "Required batch file was not found: $batchFile"
    }

    $command = "`"$batchFile`""
    if ($InstanceCountOverride -gt 0) {
        $command += " -InstanceCountOverride $InstanceCountOverride"
    }

    Write-RunLog "Running $Name."
    $process = Start-Process `
        -FilePath $env:ComSpec `
        -ArgumentList @("/d", "/c", $command) `
        -WorkingDirectory $scriptDir `
        -Wait `
        -PassThru

    if ($process.ExitCode -ne 0) {
        throw "$Name exited with code $($process.ExitCode)."
    }
}

try {
    Write-RunLog "MultiBruteforce requested for game PID $GamePid with $InstanceCount instances."

    $game = Get-Process -Id $GamePid -ErrorAction SilentlyContinue
    if ($game) {
        Write-RunLog "Waiting for the game to close cleanly."
        Wait-Process -Id $GamePid -Timeout 90 -ErrorAction Stop
    }

    if (Get-Process -Id $GamePid -ErrorAction SilentlyContinue) {
        throw "The game did not close within the timeout; BF scripts were not started."
    }

    $checkBatch = Join-Path $scriptDir "checkBf.bat"
    if (-not (Test-Path -LiteralPath $checkBatch)) {
        throw "Required batch file was not found: $checkBatch"
    }

    $checkCommand = "`"$checkBatch`" -InstanceCountOverride $InstanceCount"
    Write-RunLog "Starting checkBf.bat with $InstanceCount instances."
    Start-Process `
        -FilePath $env:ComSpec `
        -ArgumentList @("/d", "/c", $checkCommand) `
        -WorkingDirectory $scriptDir | Out-Null

    Invoke-BatchFile "launchBF.bat" $InstanceCount

    Write-RunLog "MultiBruteforce workflow started."
}
catch {
    Write-RunLog "ERROR: $($_.Exception.Message)"
    exit 1
}
