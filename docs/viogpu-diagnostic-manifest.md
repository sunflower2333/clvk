# Ordinary-user diagnostic startup

On the actual ARM64 target, `viogpu-opencl-dispatch-check.exe` failed before
process creation with Win32 error 740 (elevation required). The original PE had
no resources or embedded manifest. Renaming the identical SHA-256 binary to
`viogpu-cl-control.exe` let the same ordinary interactive user launch it and
pass the real GPU control: two queues, four dispatches, 512 correct words and
event/profiling checks. This isolates filename-dependent installer detection
from a GPU/runtime failure.

The build now explicitly links both diagnostic executables with
`/MANIFEST:EMBED` and `/MANIFESTUAC:level='asInvoker' uiAccess='false'`.
Original executable names and tool source remain unchanged. No elevation,
compatibility flags, global UAC changes or runtime changes are involved.

The focused workflow builds only these two tools for ARM64, x64 and x86 using
the frozen runtime/import library from clvk `7dfb36f`, CI `34693617321`. It
extracts and verifies actual manifest resource #1. A native ARM64 Windows job
creates a temporary standard account and launches the original EXEs directly
with `CreateProcessWithLogonW`, checking the child user SID, non-elevated token
and medium-or-lower integrity before resuming the child. All six launches must
exit successfully. The recorder executes its checks; the GPU control executes
only `--help`. This proves launch behavior, not a new target GPU result.

Artifacts contain only the two updated EXEs, extracted manifests and source/
hash metadata. Reuse frozen runtime dependencies. No runtime rebuild or global
installation is needed. The ordinary-user test account and its staged copies
are removed in CI cleanup.
