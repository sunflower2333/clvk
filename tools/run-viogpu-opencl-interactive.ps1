param(
    [Parameter(Mandatory=$true)][string]$RuntimeDir,
    [Parameter(Mandatory=$true)][string]$CompilerDir,
    [Parameter(Mandatory=$true)][string]$DriverManifest,
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$ExpectedDriverSHA256,
    [Parameter(Mandatory=$true)][ValidateSet('arm64','x64','x86')][string]$Architecture,
    [Parameter(Mandatory=$true)][ValidatePattern('^[a-zA-Z0-9-]+$')][string]$RunId,
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'run-viogpu-opencl.ps1'
& $runner -RuntimeDir $RuntimeDir -CompilerDir $CompilerDir -DriverManifest $DriverManifest `
    -ExpectedDriverSHA256 $ExpectedDriverSHA256 -Architecture $Architecture -VerifyOnly
if ($VerifyOnly) { return }
foreach ($path in @($runner,$RuntimeDir,$CompilerDir,$DriverManifest)) {
    if ($path.Contains('"')) { throw 'Quote in path' }
}
$arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $runner + '" -RuntimeDir "' +
    $RuntimeDir + '" -CompilerDir "' + $CompilerDir + '" -DriverManifest "' +
    $DriverManifest + '" -ExpectedDriverSHA256 ' + $ExpectedDriverSHA256 +
    ' -Architecture ' + $Architecture + ' -TimeoutSeconds 120'
$powershell = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
# The existing console worker runs as the logged-in user. Elevated SSH ignores
# Vulkan ICD environment overrides; the worker also adds the ICD directory to
# its process PATH so the architecture-matching driver dependencies resolve.
& C:\Users\Public\run-viogpu-console.ps1 -Action Start -RunId $RunId `
    -ProbePath $powershell -ProbeArguments $arguments -ExpectedArchitecture ARM64 `
    -ExpectedSHA256 (Get-FileHash $powershell).Hash -TimeoutSeconds 150 `
    -VulkanIcd $DriverManifest -ExpectedIcdSHA256 $ExpectedDriverSHA256
