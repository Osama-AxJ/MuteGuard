param(
    [ValidateRange(10,3600)][int]$Seconds = 65,
    [switch]$RestartPrevious
)
$ErrorActionPreference = 'Stop'
$mgRoot = Split-Path -Parent $PSScriptRoot
$mgExe = Join-Path $mgRoot 'bin\MuteGuard.exe'
$mgControl = Join-Path $mgRoot 'build\test_control.exe'
$mgAutomatic = Join-Path $mgRoot 'build\test_automatic.exe'
$mgAudio = Join-Path $mgRoot 'build\test_audio.exe'
$mgConfig = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'MuteGuard\settings.dat'
$mgBackup = Join-Path $mgRoot 'build\release-original-settings.dat'
$mgLockPath = Join-Path $mgRoot 'build\release-test.lock'
if (Test-Path -LiteralPath $mgBackup) { throw "A prior settings backup exists at $mgBackup. Recover it before testing." }
$mgLock = [IO.File]::Open($mgLockPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
$mgOriginal = $null
$mgChanged = $false
$mgRestart = $RestartPrevious.IsPresent
function Set-MGConfig([byte[]]$Bytes) {
    $mgTemporary = "$mgConfig.test.tmp"
    [IO.File]::WriteAllBytes($mgTemporary,$Bytes)
    if (Test-Path -LiteralPath $mgConfig) { [IO.File]::Replace($mgTemporary,$mgConfig,[NullString]::Value) }
    else { [IO.File]::Move($mgTemporary,$mgConfig) }
}
Push-Location $mgRoot
try {
    $mgRunning = @(Get-Process MuteGuard -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $mgExe })
    if ($mgRunning.Count) { $mgRestart = $true }
    & $mgControl $mgExe
    if ($LASTEXITCODE -ne 0) { throw 'The existing application could not be safely closed.' }
    if (Test-Path -LiteralPath $mgConfig) {
        $mgOriginal = [IO.File]::ReadAllBytes($mgConfig)
        [IO.File]::WriteAllBytes($mgBackup,$mgOriginal)
    }
    [IO.Directory]::CreateDirectory((Split-Path -Parent $mgConfig)) | Out-Null
    foreach ($mgMode in @('manual','automatic')) {
        $mgCandidate = Join-Path $mgRoot "build\release-$mgMode-settings.dat"
        $mgOption = if ($mgMode -eq 'manual') { '--write-release-manual-config' } else { '--write-release-config' }
        & $mgAutomatic $mgOption $mgCandidate
        if ($LASTEXITCODE -ne 0) { throw 'Could not prepare the test-only configuration.' }
        $mgChanged = $true
        Set-MGConfig ([IO.File]::ReadAllBytes($mgCandidate))
        $mgProcess = Start-Process -FilePath $mgExe -WindowStyle Hidden -PassThru
        Start-Sleep -Seconds 2
        Write-Output "Measuring $mgMode mode, Release PID $($mgProcess.Id), $Seconds seconds, Settings closed."
        & (Join-Path $PSScriptRoot 'measure.ps1') -ProcessId $mgProcess.Id -Seconds $Seconds |
            Tee-Object -FilePath (Join-Path $mgRoot "build\release-$mgMode-performance.json")
        $mgTestOption = if ($mgMode -eq 'automatic') { '--production-lifecycle' } else { '--production' }
        & $mgAudio $mgTestOption $mgProcess.Id 2>&1 |
            Tee-Object -FilePath (Join-Path $mgRoot "build\release-$mgMode-audio.log")
        if ($LASTEXITCODE -ne 0) { throw "$mgMode Release audio checks failed." }
        & $mgControl $mgExe
        if ($LASTEXITCODE -ne 0) { throw 'Could not stop the test instance cleanly.' }
        Remove-Item -LiteralPath $mgCandidate
    }
}
finally {
    try {
        if ($mgChanged) {
            & $mgControl $mgExe
            if ($LASTEXITCODE -ne 0) { throw 'A test instance is still running; the original backup is retained.' }
            if ($null -ne $mgOriginal) {
                Set-MGConfig $mgOriginal
                if ((Get-FileHash -LiteralPath $mgConfig).Hash -ne (Get-FileHash -LiteralPath $mgBackup).Hash) { throw 'Settings restoration verification failed; backup retained.' }
            } else { [IO.File]::Delete($mgConfig) }
            Write-Output 'Original settings restored byte-for-byte (or removed if originally absent).'
        }
        if (Test-Path -LiteralPath $mgBackup) { Remove-Item -LiteralPath $mgBackup }
        if ($mgRestart) { Start-Process -FilePath $mgExe -WindowStyle Hidden }
    }
    finally {
        Pop-Location
        $mgLock.Dispose()
        Remove-Item -LiteralPath $mgLockPath
    }
}
