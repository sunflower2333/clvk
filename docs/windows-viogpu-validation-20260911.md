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

The next actual I386 run used runtime source0506ac3, CI34577372051 (all four
jobs passed), and the same Turnip4ace ICD. OpenCL SHA256
FAF744A83FEDE22641FFA27FA7D1960813D8A01FA1C34211B33C963AB4154222;
probe B5A17FD4779ED44DEBB16531DF7E350610547F806FF1CA6265B153AF6ADBDEDE;
loader79DD4A2D463965481F1FE72511612841C02B380358E891542934E719E84D1BD3.
Console run opencl-x86-0506ac3-01 completed Exit0; child pointer_bits32/PID7908
ran UTC08:23:05.8048617Z through08:23:11.6351084Z, elapsed5790ms, ExitCode0.
Directory x86/package/runs/20260911-012305-794-6f1538173d0549caa109b3ee33448978
under the separate opencl-0506ac3 candidate contains exact module paths,
Adreno830/Qualcomm identity and explicit PASS for eight4096-element kernels,
copies, complete readback and events. Invalid source returned -11 in1444314us;
valid build returned0 in331140us. Event intervals use host monotonic fallback,
not GPU hardware timing. Out-of-order remains unsupported and unclaimed.
Parent host capture is named opencl-x86-0d8e573-02 for historical reasons;
the actual tested runtime is0506ac3. Parent matched host trace11391lines with
zero loss, CLcontext41:2 submissions8/retirements8, no fault/dmesg delta.
Postdesktop Explorer passed3/3 rounds with the same DWM576/Explorer4400.
New Application record108434 was Edge Information event256 at local01:22:31,
before this OpenCL run: extension garbage collection completed, not an error.

Actual AMD64 runtime0506ac3 is staged and remote preflight passed, not yet run:
OpenCL921627B6388849124BAD1B8327ED7336B2FF4CA67F8A6B588D25E3F50E4C33C0;
probe88BA7E3DCFC39FCA0593534B1C477AE7A21F4D4038E5BBA4E5B132A3036EACC2;
loader58B802C59057CA7264184A4348F86AABE3211B9B527C1FB7E3FFB7DDF44CBA57.
Turnip build73834ddda30 (runtime baseline4ace), CI34576200645, is actual8664:
ICDB5A2E4715CE3B309EF876ECF3EF7A9D06A8174D245454665EC0725050667F370;
zlibC148477D0DB3A84EDD7B095F3C2F1D59D70B43E753E281DD8E18F661F7D8632B.
All runtime/compiler dependency hashes and architecture gates passed. No
system-wide OpenCL ICD registration or general conformance claim is made.
