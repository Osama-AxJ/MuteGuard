param(
    [Parameter(Mandatory=$true)][int]$ProcessId,
    [ValidateRange(10,86400)][int]$Seconds = 65
)
$ErrorActionPreference = 'Stop'
$mgProcess = Get-Process -Id $ProcessId
if ($mgProcess.ProcessName -ne 'MuteGuard') { throw 'Choose the running MuteGuard process.' }
$mgSamples = @()
$mgClock = [System.Diagnostics.Stopwatch]::StartNew()
do {
    $mgProcess.Refresh()
    $mgSamples += [pscustomobject]@{
        seconds = $mgClock.Elapsed.TotalSeconds
        cpuMs = $mgProcess.TotalProcessorTime.TotalMilliseconds
        privateBytes = $mgProcess.PrivateMemorySize64
        threads = $mgProcess.Threads.Count
    }
    $mgRemaining = $Seconds - $mgClock.Elapsed.TotalSeconds
    if ($mgRemaining -gt 0) { Start-Sleep -Milliseconds ([int]([Math]::Min(30,$mgRemaining)*1000)) }
} while ($mgRemaining -gt 0)
$mgCpuMs = $mgSamples[-1].cpuMs - $mgSamples[0].cpuMs
$mgDuration = $mgSamples[-1].seconds - $mgSamples[0].seconds
[pscustomobject]@{
    pid = $ProcessId
    durationSeconds = $mgDuration
    deltaCpuMs = $mgCpuMs
    oneCoreCpuPercent = $mgCpuMs / ($mgDuration * 10)
    totalCpuPercent = $mgCpuMs / ($mgDuration * 10 * [Environment]::ProcessorCount)
    logicalProcessors = [Environment]::ProcessorCount
    samples = $mgSamples
} | ConvertTo-Json -Depth 5
