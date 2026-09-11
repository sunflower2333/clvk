$ErrorActionPreference = 'Stop'
$candidate = 'C:\Users\Public\VioGpuCandidates\opencl-0d8e573'
$arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $candidate + '\inspect-x86-vulkan-loader.ps1" -RuntimeDir "' + $candidate + '\x86\package" -DriverManifest "' + $candidate + '\turnip-4ace-x86\freedreno_icd.x86.json"'
& C:\Users\Public\run-viogpu-console.ps1 -Action Start -RunId opencl-x86-instance-interactive-01 `
    -ProbePath C:\Windows\SysWOW64\WindowsPowerShell\v1.0\powershell.exe `
    -ProbeArguments $arguments -ExpectedArchitecture x86 -TimeoutSeconds 30 `
    -ExpectedSHA256 6A73F3DDA06163BB6253E4F82A283E184D70755C067633C4190FBFF64F0BAECD `
    -VulkanIcd ($candidate + '\turnip-4ace-x86\freedreno_icd.x86.json') `
    -ExpectedIcdSHA256 20E322743456FFFC6AC12B35AA26169D3A473CF08E729E80740FFA92D3D46FE4
