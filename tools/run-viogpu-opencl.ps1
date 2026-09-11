param(
    [Parameter(Mandatory=$true)][string]$RuntimeDir,
    [Parameter(Mandatory=$true)][string]$CompilerDir,
    [Parameter(Mandatory=$true)][string]$DriverManifest,
    [ValidateSet('arm64','x64','x86')][string]$Architecture = 'arm64',
    [int]$TimeoutSeconds = 120
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
    Get-FileHash $binary -Algorithm SHA256 | Format-List
}
if ((Get-FileHash $compiler).Hash -ne '79E236AF8FEBD67FD02ADFD93F81295C87E868E9FD861F71D03D1057E6BE1F9D') {
    throw 'Compiler identity mismatch'
}
Get-FileHash $compiler | Format-List
$variables = @('VK_DRIVER_FILES','VK_ICD_FILENAMES','CLVK_CLSPV_PATH','CLVK_COMPILER_TEMP_DIR','CLVK_LOG')
$saved = @{}
foreach ($variable in $variables) { $saved[$variable] = [Environment]::GetEnvironmentVariable($variable, 'Process') }
try {
    $env:VK_DRIVER_FILES = $manifest
    $env:VK_ICD_FILENAMES = $manifest
    $env:CLVK_CLSPV_PATH = $compiler
    # Deliberate spaces exercise compiler path quoting, inside the candidate only.
    $env:CLVK_COMPILER_TEMP_DIR = Join-Path $runtime 'compiler temporary files'
    New-Item -ItemType Directory -Force $env:CLVK_COMPILER_TEMP_DIR | Out-Null
    $env:CLVK_LOG = '3'
    $stdout = Join-Path $runtime 'opencl-check.stdout.txt'
    $stderr = Join-Path $runtime 'opencl-check.stderr.txt'
    $process = Start-Process (Join-Path $runtime 'viogpu-opencl-check.exe') -WorkingDirectory $runtime -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if (!$process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill()
        throw "OpenCL probe timed out after $TimeoutSeconds seconds"
    }
    $process.Refresh()
    Get-Content $stdout
    Get-Content $stderr
    if ($process.ExitCode -ne 0) { throw "OpenCL probe exit=$($process.ExitCode)" }
    if (!(Select-String -Path $stdout -SimpleMatch 'PASS GPU kernel + copy + readback + events + compiler error propagation')) {
        throw 'Probe did not emit GPU verification PASS'
    }
} finally {
    foreach ($variable in $variables) { [Environment]::SetEnvironmentVariable($variable, $saved[$variable], 'Process') }
}
