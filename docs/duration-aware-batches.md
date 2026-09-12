# Opt-in duration-aware command batches

`CLVK_MAX_BATCH_DURATION_US` is a conservative command-batch admission estimate.
It defaults to zero (disabled). It does not implement preemption or a timeout,
and does not guarantee that a single dispatch finishes before a GPU watchdog.

With a nonzero budget, a previously unseen kernel/argument/geometry combination
is submitted alone. This also applies when several unseen commands are enqueued
before the first completion. Eligible successful observations establish a
monotonic maximum cost with a factor-of-two margin. A batch accepts another
known command only when the sum of estimates fits its budget. Existing command
count limits, group flushing, barriers, event dependencies and NDRange geometry
still apply. Nonkernel batchable commands remain unknown and are isolated.

The cost key belongs to the kernel object and contains dimensions, global/local
sizes, offsets, all POD argument bytes, resource size/type/flags/subbuffer offset,
image dimensions and pitches, local allocation sizes, and sampler identity.
Each kernel retains at most 16 full byte keys of at most 1024 bytes. Oversized
keys stay unknown. Commands retain their state if its cache entry is evicted;
the same evicted key is cold if encountered again. Input buffer contents are
neither read nor included, so data-dependent duration changes remain possible.

Timing spans actual submission and the existing whole-Vulkan-queue idle wait.
It can include logging, lock contention and another OpenCL queue's work. An API
error or whole-batch elapsed time at least equal to the budget permanently
isolates that retained cost state. Such a sample is rejected before dividing by
the command count: the observed 2.019-second false-success reset must not train
a shorter per-command estimate. Successful shorter homogeneous batches divide
elapsed time by command count, rounded up; mixed batches conservatively use the
whole duration for each command. The actual Vulkan results remain authoritative
to the existing error-return path; this estimator never synthesizes completion.

The estimate is useful only within a correctly functioning completion path.
A host/KMD false-success fix and real retirement evidence are separate
requirements. Cache eviction is bounded-memory behavior, not durable quarantine.

## Bounded argument diagnostics

```text
CLVK_DISPATCH_TRACE=1
CLVK_DISPATCH_TRACE_KERNEL=
CLVK_DISPATCH_TRACE_ARGUMENTS=1
CLVK_DISPATCH_TRACE_ARGUMENTS_KERNEL=hough_scores
CLVK_MAX_BATCH_DURATION_US=100000
CLVK_LOG=3
CLVK_LOG_GROUPS=none,cfg
CLVK_LOG_COLOUR=0
```

The 100,000-microsecond budget is an initial opt-in test setting, not a production
default. The argument-name filter affects diagnostics only; no kernel-name
execution policy exists. Keep dispatch attribution unfiltered for the first
run so work preceding the failing kernel remains visible.

`DISPATCH_ARGUMENTS` reports total and retained argument counts per dispatch.
`DISPATCH_ARGUMENT` copies at most 16 argument records and 32 scalar bytes per
argument. It includes position, compiler kind/name/type, descriptor set/binding,
declared POD offset/size, scalar validity/truncation/hex, resource pointer
identity/bytes/type/flags/subbuffer offset, local allocation size, and image
width/height/depth/array-size/row-pitch/slice-pitch. Names/types are capped at 63
characters. Raw scalar hex uses the process's native byte order.

The compiler kind numbering is: buffer=0, buffer_ubo=1, pod=2, pod_ubo=3,
pod_pushconstant=4, pointer_ubo=5, pointer_pushconstant=6, sampled_image=7,
storage_image=8, storage_texel_buffer=9, uniform_texel_buffer=10, sampler=11,
local=12, unused=13. Physical pointer kinds 5/6 expose only copied POD address
bytes: current clvk does not retain a `cvk_mem` mapping for those argument kinds,
so zero resource metadata does not establish an empty buffer. Argument capture
never reads application buffer/image contents. Input density is still unknown.

Retained dispatch records share one metadata copy per command. Trace capacity
remains 32 records per command buffer. `BATCH_DURATION` links to the command
buffer trace ID and reports whole elapsed time, budget, API success and sample
eligibility. IDs are process-local and are not host fence/KGSL timestamps.

Windows candidate ICD loading is additive. Verify the selected platform/device,
loaded module paths and exact package identity. Multiple clvk vendors can
truncate a shared `CLVK_LOG_DEST=file:...` destination, so use compact process
stderr redirection for an additive candidate run. Reuse the verified installed
matching compiler through explicit `CLVK_CLSPV_PATH`.

## Required target evidence

1. Run the integer GPU control with two queues, complete readback and events;
   verify exact candidate modules and no reset. Repeat with the opt-in budget.
2. Run Horizon with compact dispatch and bounded argument diagnostics. Verify
   the first unseen Hough commands become separate accepted submissions, and
   collect actual scalar values and available resource bounds.
3. Correlate each accepted packet with normal host retirement and reset counts.
   A queue-idle API success alone is insufficient evidence after the observed
   false completion. Check benchmark validity and output, not process exit alone.
4. If an isolated Hough dispatch still exceeds the watchdog, retain that evidence
   for a separate geometry/preemption design. Existing within-command-buffer
   Vulkan-limit region splitting does not create a WDDM submission boundary.

`tools/batch-duration-check.cpp` executes the production admission/cache and
metadata helper code; it does not execute a GPU. CI also builds the complete
runtime on ARM64/x64/x86 and runs those helpers on native ARM64/x64/x86. A
successful generic control or CI run does not establish a repaired GB7 kernel.
