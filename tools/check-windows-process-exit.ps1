param([Parameter(Mandatory=$true)][string]$Executable)
$ErrorActionPreference = 'Stop'
$directory = Join-Path (Split-Path $Executable) ('process-exit-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $directory | Out-Null
$process = Start-Process $Executable -ArgumentList 'child literal' -PassThru -RedirectStandardOutput (Join-Path $directory 'stdout.txt') -RedirectStandardError (Join-Path $directory 'stderr.txt')
$null = $process.Handle
if (!$process.WaitForExit(10000)) { & taskkill.exe /PID $process.Id /T /F; throw 'Helper timeout' }
$process.Refresh()
if ($null -eq $process.ExitCode -or $process.ExitCode -ne 7) { throw "Incorrect captured exit: $($process.ExitCode)" }
Write-Output "PASS own helper ExitCode=$($process.ExitCode); no GPU APIs invoked"
