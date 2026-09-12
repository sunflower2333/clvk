param([Parameter(Mandatory)][string]$ToolsRoot, [Parameter(Mandatory)][string]$FrozenRoot)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'Arm64') { throw 'Native ARM64 CI runner required' }
Add-Type -Path (Join-Path $PSScriptRoot diagnostic-standard-user.cs)
function Test-Sums([string]$Root) {
    foreach ($line in Get-Content (Join-Path $Root SHA256SUMS)) {
        $parts = $line -split '  ', 2
        if ($parts.Count -ne 2 -or [IO.Path]::GetFileName($parts[1]) -ne $parts[1]) { throw 'Invalid hash record' }
        if ((Get-FileHash (Join-Path $Root $parts[1]) -Algorithm SHA256).Hash -ne $parts[0]) { throw "Input hash mismatch: $($parts[1])" }
    }
}
$userName = 'clvkdiag' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$password = 'Clvk!' + [Guid]::NewGuid().ToString('N')
$user = $null
$stage = Join-Path $env:PUBLIC ('clvk-tools-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $stage | Out-Null
try {
    $user = New-LocalUser -Name $userName -Password (ConvertTo-SecureString $password -AsPlainText -Force) -AccountNeverExpires
    $usersGroup = Get-LocalGroup -SID 'S-1-5-32-545'
    if (@(Get-LocalGroupMember -Group $usersGroup | ForEach-Object {$_.SID.Value}) -notcontains $user.SID.Value) {
        Add-LocalGroupMember -Group $usersGroup -Member $user
    }
    $adminGroup = Get-LocalGroup -SID 'S-1-5-32-544'
    if (@(Get-LocalGroupMember -Group $adminGroup | ForEach-Object {$_.SID.Value}) -contains $user.SID.Value) { throw 'Test account must not be an administrator' }
    foreach ($arch in @('arm64','x64','x86')) {
        $tools = (Resolve-Path (Join-Path $ToolsRoot "clvk-manifest-tools-$arch")).Path
        $frozen = (Resolve-Path (Join-Path $FrozenRoot "$arch/package")).Path
        Test-Sums $tools
        Test-Sums $frozen
        if ((Get-Content (Join-Path $tools source.txt)) -notcontains "tools_source=$env:GITHUB_SHA") { throw 'Tool source mismatch' }
        if ((Get-Content (Join-Path $frozen source.txt) -First 1).Trim() -ne '7dfb36f67f47f152b93032fe9488cfe7e07eb486') { throw 'Runtime source mismatch' }
        $directory = New-Item -ItemType Directory (Join-Path $stage $arch)
        Copy-Item (Join-Path $frozen '*') $directory.FullName -Recurse
        foreach ($name in @('dispatch-trace-check.exe','viogpu-opencl-dispatch-check.exe')) {
            $path = Join-Path $directory.FullName $name
            Copy-Item (Join-Path $tools $name) $path -Force
            $bytes = [IO.File]::ReadAllBytes($path)
            $pe = [BitConverter]::ToInt32($bytes, 0x3c)
            $expected = (@{arm64=0xaa64; x64=0x8664; x86=0x14c})[$arch]
            if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne $expected) { throw 'Wrong tool PE architecture' }
            [xml]$manifest = [DiagnosticStandardUser]::Manifest($path)
            $levels = @($manifest.SelectNodes("//*[local-name()='requestedExecutionLevel']"))
            if ($levels.Count -ne 1 -or $levels[0].level -ne 'asInvoker' -or $levels[0].uiAccess -ne 'false') { throw 'Wrong embedded manifest' }
            Get-FileHash $path -Algorithm SHA256 | Format-List
            $arguments = if ($name -eq 'viogpu-opencl-dispatch-check.exe') {'--help'} else {''}
            [DiagnosticStandardUser]::Launch($userName, $password, $user.SID.Value, $path, $arguments)
        }
        if ((Get-FileHash (Join-Path $directory.FullName OpenCL.dll)).Hash -ne (Get-FileHash (Join-Path $frozen OpenCL.dll)).Hash) { throw 'Frozen runtime changed' }
    }
    'PASS six original-name executable launches: standard user, non-elevated, medium-or-lower integrity, correct manifests and architecture; no GPU execution'
} finally {
    if ($user) { Remove-LocalUser -SID $user.SID }
    # Windows can briefly retain emulated DLL image mappings after process
    # exit. Retry this exact disposable directory; never suppress final failure.
    for ($attempt = 0; $attempt -lt 6; $attempt++) {
        try { Remove-Item -LiteralPath $stage -Recurse -Force; break }
        catch {
            if ($attempt -eq 5) { throw }
            Start-Sleep -Milliseconds 500
        }
    }
}
