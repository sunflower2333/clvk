# Flat Windows duration/dispatch integration

This candidate combines the exact production duration/metadata/dispatch changes
from bff99c17d2758e761f4265ca97a6179236665ca4 with flat runtime
0f436fe7110813ddf01dfaebc44b9de7cd39b3b8. Both descend from ee0ea93; production
source files affected by diagnostics do not overlap the flat naming/compiler
implementation. Duration defaults to zero and dispatch tracing defaults off.
No capability, dispatch geometry, compiler algorithm, barrier or API completion
semantics change beyond the previously verified opt-in batching candidate.

All architecture runtimes retain viogpucl_<arch>.dll and
viogpucl_vk_<arch>.dll names and static CRT linkage. Compiler default remains the
absolute sibling viogpu_clspv_x64.exe. See duration-aware-batches.md for option
semantics, argument limits and why a duration budget is not a hard timeout or
single-dispatch preemption. This build is a packageable candidate, not proof
that GB7 Hough is repaired.

CI builds all three architectures and executes the production82 duration and
198 recorder checks under Linux sanitizers and on native ARM64/emulated x64/x86
Windows. Existing actual flat runtime/private-loader/compiler SPIR-V checks
remain in place. New tools use static CRT and explicit asInvoker manifests:

- dispatch-trace-check-<arch>.exe
- batch-duration-check-<arch>.exe
- viogpu-opencl-dispatch-check-<arch>.exe

The dispatch control intentionally links the matching private candidate ICD
and runs with its flat sibling DLLs. It is not an ordinary system-loader app.
The parent producer owns ordinary application probes linked against public
OpenCL.dll; consume its corrected public-loader probes, not the legacy runtime
artifact's private-linked viogpu-opencl-check EXEs. Additional diagnostic tools
remain disposable probes, outside the signed driver inventory unless explicitly
selected later.

The matching compiler source/hash and private Vulkan loader1.4.341 pins remain
unchanged. Runtime artifact package/source.txt, SHA256SUMS and CI PAYLOAD_SHA256
output identify the exact candidate. Parent pin changes and target testing are
owned by root; do not rewrite an installed package manifest to swap DLLs.
