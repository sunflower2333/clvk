# Windows VIOGPU OpenCL candidates

The runtime executes OpenCL kernels as Vulkan compute on Turnip. The separate
x64 clspv process compiles OpenCL C into SPIR-V; it never executes the kernel.
CPU Vulkan implementations are not accepted by the verification probe.

| Application | OpenCL runtime / Vulkan loader / ICD | Compiler process |
| --- | --- | --- |
| Native ARM64 | ARM64 | x64 under Windows emulation |
| x64 emulation | actual AMD64 | x64 under Windows emulation |
| x86 emulation | actual I386, OpenCL stdcall ABI | x64 under Windows emulation |

Use the architecture-specific runtime artifact, the separate compiler artifact,
and the matching Turnip driver candidate. Keep compiler DLL dependencies in the
compiler directory: x64 compiler dependencies must not replace ARM64/x86 runtime
dependencies. The runtime build enables OpenCL C source compilation and disables
the optional SPIR-V *OpenCL IL input* frontend, which needs an additional
translator. Vulkan SPIR-V compute remains enabled. No full OpenCL conformance
claim is made by a successful build or by the bounded verification probe.

`tools/run-viogpu-opencl.ps1` checks PE machine types and compiler identity, prints
binary hashes, selects the supplied ICD only for the child process, and runs a
120-second bounded probe. Supply `-RuntimeDir`, `-CompilerDir`,
`-DriverManifest`, and `-Architecture`. It does not register an ICD globally or
install a display driver. Coordinate the VM GPU test window before invoking it.

The probe checks undecorated ICD exports, requires an Adreno/Turnip GPU, verifies
invalid compiler input fails, and validates every element after repeated kernel
execution, GPU buffer copies and CPU readback. It verifies event completion and
GPU profiling order, and exercises out-of-order queues only if advertised.
Runtime acceptance additionally requires matched host GPU and guest driver
evidence with no TDR, corruption, hang or device loss.

Compiler provenance: kpet/clvk Actions run 34487278874, artifact
`clvk-windows-2022---compiler-true-online-false`; clspv source
`c20f7c8ccf58f317972ac1ffeab68a96019cbbaa`, matching this checkout's submodule.
The pinned compiler SHA256 is
`79e236af8febd67fd02adfd93f81295c87e868e9fd861f71d03d1057e6be1f9d`.
