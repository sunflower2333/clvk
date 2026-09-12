# VIOGPU OpenCL dispatch attribution

This opt-in recorder distinguishes the kernel being waited on from the Vulkan
submission that stopped retiring. It changes no dispatch, synchronization,
completion, timeout or device capability. Keep it disabled in ordinary use.

For an isolated diagnostic process, set:

```
CLVK_DISPATCH_TRACE=1
CLVK_DISPATCH_TRACE_KERNEL=hough_scores
CLVK_LOG=3
CLVK_LOG_GROUPS=none,cfg
CLVK_LOG_COLOUR=0
CLVK_KEEP_TEMPORARIES=1
CLVK_COMPILER_TEMP_DIR=<new per-run directory>
CLVK_LOG_DEST=file:<new per-run log file>
```

The compiler temp directory must already exist. Keep the source and compiled
SPIR-V on the device; the program-pointer/path/build-option record associates
them with dispatch records. An empty kernel filter captures all kernel names.
The recorder retains the last32 matching dispatches in each command buffer,
with exact unfiltered command/dispatch counts and matching-record count so
truncation is explicit. It copies names and geometry into bounded metadata and
does not retain caller pointers. Records are flushed before vkQueueSubmit.

SUBMIT_BEGIN maps its unique process-local ID to the OpenCL queue, shared
Vulkan queue, command buffer, monotonic and UTC timestamps. DISPATCH_RECORD
includes the command/event/kernel/program identity, original global/local/
offset and actual dispatched region. SUBMIT_RETURN and WAIT_RETURN retain
real Vulkan results and elapsed time. The existing wait is vkQueueWaitIdle;
WAIT_RETURN explicitly marks its scope as the entire shared Vulkan queue,
not an individual dispatch fence. A missing WAIT_RETURN does not prove which
kernel within a batch is stuck.

An isolated A/B can set both CLVK_MAX_CMD_BATCH_SIZE and
CLVK_MAX_FIRST_CMD_BATCH_SIZE to1. This changes only that process's batching;
it does not change advertised capabilities or prove a fix. Splitting dispatch
regions inside one DMA packet alone does not prove WDDM can preempt the packet.

`viogpu-opencl-dispatch-check.exe` is a small deterministic control using an
actual Adreno/Turnip GPU, an integer kernel with local size16, one or two
OpenCL queues, all-texel CPU reference checking, actual events and profiling.
Defaults are64 iterations,4 dispatches,16 groups and2 queues. It never alters
environment/registration. Its explicit iteration limits bound only this test
workload. Run identical controls with default and batch-size1 policy and use
the trace to compare actual batching/queue behavior.

This control is not Geekbench hough_scores. An exact GB7 replay additionally
requires the actual source/SPIR-V, build options, global/local sizes, scalar
arguments, buffer/image shapes and initial input data from the failing run.
No production scheduling policy should be changed solely because a generic
control passes. Parent owns hardware execution and the exact replay decision.
