# OpenCL candidate validation, 2026-09-11

Selected runtime/compiler artifacts: CI
https://github.com/sunflower2333/clvk/actions/runs/34576118762,
source 0d8e573 (all four jobs successful). Runtime source is unchanged from
83cd21b; subsequent commits improve packaging and the launcher only.
Launcher source: 36c2654. SHA256SUMS files in the deployed candidates are a
lossless conversion of CI's sha256.txt records; every converted entry was
checked locally using sha256sum -c before upload.

| Runtime | OpenCL.dll SHA256 | PE / compile / link | GPU proof |
| --- | --- | --- | --- |
| ARM64 | CC2410DB88838EAF8332BF3EE67F02364765C0C201C0EEEF2E5E81FF7A7AF3CC | Native AArch64, passed | 8 kernels/copies/readbacks correct; original launcher failed collecting exit code |
| x64 | E790B904FD5F6872AF5EC95D8EA4AB6BE8CB5FFDB24DCADA4E7D2ACDC67A2A80 | Actual AMD64, passed | pending |
| x86 | 973D2964C98A24D8FF0A3D83E65BB26BE93547DFFCE0BA40CAE45F167FBC50F8 | Actual I386, passed | first run failed Vulkan instance loading; no kernel executed |

ARM64 loader SHA256:
8286C6AEEFB4D53691CE6394089B817A2AD56D34DD536C609139EBA93491067F.
ARM64 verification executable SHA256:
F8834458E4C2807E94F45998569750E171900FABB87DC587019E41B978D7F406.
External x64 clspv SHA256:
79E236AF8FEBD67FD02ADFD93F81295C87E868E9FD861F71D03D1057E6BE1F9D.

The runtime directories contain architecture-matching MSVC redistributables;
the compiler directory contains its own x64 redistributables. Undecorated ICD
exports were checked. x64 and x86 CI execute the subprocess regression check:
literal spaces/ampersand/percent/trailing backslash, stdout and stderr capture,
nonzero child exit propagation, and launch failure all passed.

Deployment is app-local under
C:/Users/Public/VioGpuCandidates/opencl-0d8e573/.
No global OpenCL registration was changed. Initial test will use the parent
agent's verified native Turnip 4ace9df9 ICD (SHA256
7B4107D451A111A7079B65D75EECA9604E511E43F117E5097868305F5BFCD652).

Build failures repaired during implementation: missing x86 stdcall on six
semaphore entrypoints; Windows compiler subprocess quoting and failure
propagation; hash manifest reading its own output during CI packaging.

First ARM64 GPU test ran at guest UTC 08:00:48, probe PID1404, directory
arm64/package/runs/20260911-010050-811-8c507e90e5294407995294ecb0e2927e.
It verified every element after eight 4096-element kernels, copies and readback;
invalid compiler source returned CL_BUILD_PROGRAM_FAILURE, valid source built,
and all events completed. Out-of-order queues were not advertised or tested.
The probe emitted its explicit PASS, but the original launcher/SSH exited1:
PowerShell lost Process.ExitCode and the wrapper rejected the null value.
This original wrapper failure is preserved, not reclassified as exit0.
Launcher1f6e973 caches the process Handle; a separate non-GPU helper on the same
Windows machine subsequently verified its expected ExitCode7 was captured.

The first probe's GPU_ns label was wrong: this clvk configuration falls back to
host monotonic event profiling when calibrated timestamps are unavailable.
Those numbers are event intervals, not hardware GPU time. Probe1f6e973 renames
the field event_ns and adds independent compiler wall time. First-run compiler
durations cannot be reconstructed separately from the captured logs.

Parent matched host trace opencl-arm64-0d8e573-01 recorded 7340 lines, zero loss,
CL context41:2 with 8 submissions and 8 retirements. Submit-to-retirement mean
0.353ms/max0.436ms is host trace latency, not GPU hardware busy time. Parent
postdesktop check passed three Explorer rounds with DWM576/Explorer4400
unchanged, no added GPU errors, KGSL fault or TDR. x64/x86 GPU windows remain
pending; no full OpenCL conformance or application compatibility claim is made.

First x86 run used exact Mesa4ace9df ICD20E322743456FFFC6AC12B35AA26169D3A473CF08E729E80740FFA92D3D46FE4
from successful CI34574005601. Candidate files and I386 ABI passed remote
preflight. Run x86/package/runs/20260911-010744-914-ed05d2df2d0e405cb0db67bd36269d0c,
PID5036, UTC08:07:44.9241919Z through08:07:46.0272375Z, elapsed1095ms,
captured ExitCode1. The runtime loaded x86 OpenCL and Vulkan loader but reported
only the four loader-owned instance extensions, then VK_ERROR_INCOMPATIBLE_DRIVER.
No physical device, compiler process or GPU kernel was reached. Investigation
is at the Windows ICD loader/dependency boundary, not GPU compute correctness.

The elevated SSH instance-only diagnostic showed Vulkan Loader ignoring
VK_DRIVER_FILES/VK_ICD_FILENAMES and no registered WOW ICD. Direct LoadLibrary
returned error193; LoadLibraryEx with DLL_LOAD_DIR and DEFAULT_DIRS succeeded
with interface7. An interactive scheduled-task control using the existing
console launcher and ICD-directory PATH resolved both: normal LoadLibrary,
negotiation and vkCreateInstance all passed, with the exact candidate manifest
in loader logs. Run opencl-x86-instance-interactive-01, PID4668,
UTC08:19:04.1971041Z, elapsed8398ms, Exit0; no physical-device enumeration,
device creation or submissions were performed. No global registration or
loader-security change is necessary for this app-local launch path.
