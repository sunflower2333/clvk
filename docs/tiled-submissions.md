# Opt-in NDRange submission tiling

`CLVK_MAX_DISPATCH_WORKGROUPS` bounds the number of complete workgroups in each
independently submitted and retired tile. Default `0` preserves the old path.
Only whole NDRanges exceeding the budget use independent tiles. The original
group count is the product of `ceil(GWS[d]/LWS[d])`, including all nonuniform
tails. An exact fit or smaller NDRange retains ordinary batching and the full
command duration key. This avoids serializing thousands of small kernels into
single-tile submit/wait calls. Division-based admission avoids product overflow.
`CLVK_MAX_BATCH_DURATION_US` admits separate ordinary OpenCL commands into
batches. Neither setting guarantees execution time or preemption.

The runtime uses one public kernel command/event, one retained argument snapshot
and descriptor set, the original buffers/images/offsets, and one profiling query
pair. It records the first tile before returning from enqueue and streams later
tiles after previous submission retirement, with O(1) live command buffers.
No public event completes between tiles. A failed record/submit stops immediately;
partially executed atomic work is never replayed. START is recorded/reset once in
the first tile and END once in the last; failed events do not wait an unwritten
END query. The elapsed interval includes inter-tile gaps.

Eligibility is explicitly captured from successful source-to-executable clspv
generation with the known nonuniform region/global-offset mode and published
only after executable validation. Imports, IR/link paths, unknown flags,
uniform-only mode, empty NDRanges and shared printf-buffer programs use the
unchanged execution path. Missing reflection constants are not treated as proof
of imported ABI compatibility. CLVK v2 binaries lose uniform-only build options;
supporting their import safely needs versioned ABI provenance or executable
validation in a separate change. No capability reporting is altered.

Each tile preserves original global IDs, global offset/size, enqueued LWS, total
group count, group IDs and linear IDs through the existing compiler region ABI.
Whole workgroups and existing reduced nonuniform tail sizes are preserved.
The planner checks offset/product overflow and Vulkan per-axis limits. A single
workgroup may still take too long; cross-workgroup spin coordination or assumed
simultaneous residency is not made safe by tiling.

For diagnostics, set `CLVK_DISPATCH_TRACE=1` and `CLVK_LOG=3`. Each
`NDRANGE_TILE_SUBMIT` carries the same public command/event, sequential tile
ordinal, budget and first/last markers. Existing DISPATCH records give actual
Vulkan submit/retirement IDs. Standalone excluded commands log
`NDRANGE_TILE_BYPASS`. An eligible over-budget kernel is excluded from batching;
duration admission does not learn per-tile time as a full-command sample.

`viogpu-opencl-tile-check-<arch>.exe --groups 7` is a private candidate-ICD control
and requires its sibling runtime/compiler DLLs. It defaults to Adreno/Turnip;
on Windows it relaunches with the chosen budget inherited before the ICD's
static CRT initializes if the existing process setting differs. The non-GPU
`--check-environment --groups 7` verifies that actual relaunch path in CI.
`--any-device` explicitly permits a software Vulkan implementation for CI.
It executes seven actual compiled-kernel cases: 1D/2D/3D nonuniform tails,
ready-to-run 3D tiling, uniform source, uniform CLVK-container import and uniform
raw SPIR-V import. It also verifies that an empty NDRange does no work.
Three additional ready-command controls use 5/7/8-group nonuniform grids: a
completed warmup trains the duration key, then twelve commands enqueue before
flush. Exact integer recurrence and canaries check ordering/barriers/scalar
snapshot and each public event has its own callback and profiling interval.
At budget7 the 5/7-group grids must share actual submissions with homogeneous
duration training, while 8groups must retain independent retired tiles.
Run the trace oracle with process-inherited `CLVK_MAX_BATCH_DURATION_US=1000000`,
`CLVK_MAX_CMD_BATCH_SIZE=10000`, `CLVK_MAX_FIRST_CMD_BATCH_SIZE=10000`, and
`CLVK_DYNAMIC_BATCHES=0`; these are test controls, not recommended device policy.
It checks all ID/size/offset builtins, exact atomic histograms,
local barrier reduction, subbuffer canaries, argument mutation/reference release,
user-event dependencies, one callback and monotonic profiling, plus negative
dependencies without waiting nonexistent timestamps. The frozen compiler has an
assertion for `get_enqueued_local_size` in uniform-only mode; only those negative
controls use the equal uniform `get_local_size` value. Source tiled controls use
the real enqueued-size builtin.

`tools/ndrange-tiles-check.cpp` uses the production planner, generation guard and
first-failure loop under ASan/UBSan, plus whole-grid admission against an
independent group-origin oracle. `tools/check-tile-trace.py` verifies actual
runtime logs: one dispatch per distinct retired tile, bounded workgroups, one
public event, real shared trained batches and unchanged uniform/import
execution. CI repeats real kernel
controls for budgets 0/1/7 on lavapipe and builds ARM64/x64/x86 Windows runtimes
and semantic controls; architecture loader/compiler checks remain enabled.

Local lavapipe tests are software execution evidence, not target Adreno/Windows
acceptance. Root owns target controls, original Hough comparison, driver package
integration and a valid complete Geekbench score. No benchmark fix or watchdog
safety is claimed by these local checks.

A group count is not a duration estimate: an expensive small grid can exceed a
watchdog interval and a cheap large grid can suffer avoidable submission
overhead. Preserve the opt-in policy, measure original application work and
actual host retirement, and assess a complete valid benchmark separately.
Increasing a global group threshold without that evidence is not acceptance.

## Calibrated timing requirement on Windows

The control now requires actual calibrated per-command timing and exits3 with
`TIMER_UNSUPPORTED` before kernel work if the driver cannot provide it. The old
host fallback copies each batch's START/END envelope to all its public events;
that fallback is preserved but is not accepted as device timing. The strict
cross-command END<=nextSTART assertion is unchanged.

On Windows, CLVK selects `VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT` paired
with DEVICE, with an actual callable calibrated timestamp function. Host values
are raw QPC ticks per the Vulkan contract and are converted with the actual
`QueryPerformanceFrequency` to nanoseconds in the MSVCsteady_clock epoch.
Exact integer scaling checks overflow and avoids floating-point uptime loss.
Linux retains its CLOCK_MONOTONIC domain. Extension/domain absence is not
overridden; an explicit timestamp-query setting cannot call a null calibrated
function. No Vulkan capabilities are fabricated by this CLVK change.

`--check-timers` performs eight real host calibration calls checked against
the surrounding steady-clock interval, then compiles and executes the5-group
recurrence warmup and twelve ready commands. It requires all per-event callback,
output and ordered profiling checks and prints`TIMER_QUERY_PASS` on success.
It is a limited timer/batch diagnostic, not the full7-case semantic test.
With dispatch tracing, `DEVICE_TIMESTAMP_QUERY` records actual retrieved raw
device query pairs and their nanosecond values. The full trace oracle requires
46such pairs for the46nonempty semantic/batch commands.

The Windows clock regression executes1000actual QPC samples against the local
steady_clock epoch for each native/emulated architecture. It verifies host
arithmetic/platform integration only; the Adreno calibrated bridge remains a
separate required dependency owned by the Mesa/KMD/host worker.
