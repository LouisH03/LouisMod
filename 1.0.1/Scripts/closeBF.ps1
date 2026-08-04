$pidFile = Join-Path $PSScriptRoot "bf_pids.txt"

if (-not (Test-Path $pidFile)) {
    Write-Host "No PID file found -- nothing to close."
    exit
}

$pids = Get-Content $pidFile

foreach ($procId in $pids) {
    $proc = Get-Process -Id $procId -ErrorAction SilentlyContinue
    if ($proc) {
        Write-Host "Closing PID $procId ($($proc.ProcessName))"
        Stop-Process -Id $procId -Force
    } else {
        Write-Host "PID $procId not running (already closed?)"
    }
}

Remove-Item $pidFile
Write-Host "Done."