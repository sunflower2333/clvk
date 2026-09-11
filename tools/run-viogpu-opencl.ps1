param(
    [Parameter(Mandatory=$true)][string]$RuntimeDir,
    [Parameter(Mandatory=$true)][string]$CompilerDir,
    [Parameter(Mandatory=$true)][string]$DriverManifest,
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string]$ExpectedDriverSHA256,
    [switch]$VerifyOnly,
    [ValidateSet('arm64','x64','x86')][string]$Architecture = 'arm64',
    [ValidateRange(1,300)][int]$TimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
$runtime = (Resolve-Path $RuntimeDir).Path
$compiler = (Resolve-Path (Join-Path $CompilerDir 'clspv.exe')).Path
$manifest = (Resolve-Path $DriverManifest).Path
$json = Get-Content $manifest -Raw | ConvertFrom-Json
$icd = $json.ICD.library_path
if (![IO.Path]::IsPathRooted($icd)) { $icd = Join-Path (Split-Path $manifest) $icd }
$icd = (Resolve-Path $icd).Path
$expectedMachine = @{arm64=0xAA64; x64=0x8664; x86=0x14C}[$Architecture]
$expectedHashes = @{}
foreach ($line in (Get-Content (Join-Path $runtime 'SHA256SUMS'))) {
    if ($line -notmatch '^([0-9A-Fa-f]{64})  (.+)$') { throw 'Invalid SHA256SUMS entry' }
    if ($expectedHashes.ContainsKey($Matches[2])) { throw 'Duplicate SHA256SUMS entry' }
    $expectedHashes[$Matches[2]] = $Matches[1]
}
function Get-Machine([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        $stream.Position = 0x3c
        $offset = $reader.ReadInt32()
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550) { throw "Not PE: $Path" }
        return $reader.ReadUInt16()
    } finally { $stream.Dispose() }
}
$binaries = @((Join-Path $runtime 'OpenCL.dll'), (Join-Path $runtime 'vulkan-1.dll'),
              (Join-Path $runtime 'viogpu-opencl-check.exe'), $icd)
foreach ($binary in $binaries) {
    $machine = Get-Machine $binary
    if ($machine -ne $expectedMachine) { throw "ABI mismatch: $binary machine=$machine" }
    $hash = Get-FileHash $binary -Algorithm SHA256
    $expected = if ($binary -eq $icd) { $ExpectedDriverSHA256 } else { $expectedHashes[[IO.Path]::GetFileName($binary)] }
    if (!$expected -or $hash.Hash -ne $expected) { throw "Binary identity mismatch: $binary" }
    $hash | Format-List
}
foreach ($dll in (Get-ChildItem $runtime -Filter '*.dll' -File)) {
    if (!$expectedHashes.ContainsKey($dll.Name) -or
        (Get-FileHash $dll.FullName).Hash -ne $expectedHashes[$dll.Name]) {
        throw "Dependency identity mismatch: $($dll.Name)"
    }
}
if ((Get-FileHash $compiler).Hash -ne '79E236AF8FEBD67FD02ADFD93F81295C87E868E9FD861F71D03D1057E6BE1F9D') {
    throw 'Compiler identity mismatch'
}
Get-FileHash $compiler | Format-List
$compilerHashes = @{}
foreach ($line in (Get-Content (Join-Path (Split-Path $compiler) 'SHA256SUMS'))) {
    if ($line -notmatch '^([0-9A-Fa-f]{64})  (.+)$') { throw 'Invalid compiler SHA256SUMS entry' }
    $compilerHashes[$Matches[2]] = $Matches[1]
}
foreach ($dll in (Get-ChildItem (Split-Path $compiler) -Filter '*.dll' -File)) {
    if (!$compilerHashes.ContainsKey($dll.Name) -or
        (Get-FileHash $dll.FullName).Hash -ne $compilerHashes[$dll.Name]) {
        throw "Compiler dependency identity mismatch: $($dll.Name)"
    }
}
if ($VerifyOnly) { Write-Output 'PASS candidate hashes and ABI; GPU workload not run'; return }
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Elevated Vulkan loader ignores ICD overrides; use run-viogpu-opencl-interactive.ps1'
}
$variables = @('VK_DRIVER_FILES','VK_ICD_FILENAMES','CLVK_CLSPV_PATH','CLVK_COMPILER_TEMP_DIR','CLVK_LOG','PATH')
$saved = @{}
foreach ($variable in $variables) { $saved[$variable] = [Environment]::GetEnvironmentVariable($variable, 'Process') }
try {
    $env:VK_DRIVER_FILES = $manifest
    $env:VK_ICD_FILENAMES = $manifest
    $env:PATH = (Split-Path $icd) + ';' + $env:PATH
    $env:CLVK_CLSPV_PATH = $compiler
    # Deliberate spaces exercise compiler path quoting, inside the candidate only.
    $runName = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N')
    $runDir = Join-Path (Join-Path $runtime 'runs') $runName
    New-Item -ItemType Directory $runDir | Out-Null
    Write-Output "RunDirectory=$runDir"
    $env:CLVK_COMPILER_TEMP_DIR = Join-Path $runDir 'compiler temporary files'
    New-Item -ItemType Directory -Force $env:CLVK_COMPILER_TEMP_DIR | Out-Null
    $env:CLVK_LOG = '3'
    $stdout = Join-Path $runDir 'opencl-check.stdout.txt'
    $stderr = Join-Path $runDir 'opencl-check.stderr.txt'
    $startedUtc = [DateTime]::UtcNow.ToString('o')
    $elapsed = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process (Join-Path $runtime 'viogpu-opencl-check.exe') -WorkingDirectory $runtime -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    # Cache the native handle while alive; Windows PowerShell otherwise loses
    # ExitCode when its Start-Process object notices the child has already exited.
    $null = $process.Handle
    if (!$process.WaitForExit($TimeoutSeconds * 1000)) {
        # Only this freshly launched probe and its compiler descendants.
        & taskkill.exe /PID $process.Id /T /F
        if ($LASTEXITCODE -ne 0 -and !$process.HasExited) { throw 'Failed to stop owned probe process tree' }
        $process.WaitForExit()
        throw "OpenCL probe timed out after $TimeoutSeconds seconds"
    }
    $process.Refresh()
    $elapsed.Stop()
    $exitCode = $process.ExitCode
    [PSCustomObject]@{StartedUtc=$startedUtc; FinishedUtc=[DateTime]::UtcNow.ToString('o');
        ProcessId=$process.Id; ElapsedMilliseconds=$elapsed.ElapsedMilliseconds;
        ExitCode=$exitCode; Architecture=$Architecture} |
        ConvertTo-Json | Set-Content (Join-Path $runDir 'result.json')
    Get-Content $stdout
    Get-Content $stderr
    if ($null -eq $exitCode -or $exitCode -ne 0) { throw "OpenCL probe exit=$exitCode" }
    if (!(Select-String -Path $stdout -SimpleMatch 'PASS GPU kernel + copy + readback + events + compiler error propagation')) {
        throw 'Probe did not emit GPU verification PASS'
    }
} finally {
    foreach ($variable in $variables) { [Environment]::SetEnvironmentVariable($variable, $saved[$variable], 'Process') }
}
