# Raw Vulkan counter rollover and calibrated profiling

This candidate descends from frozen bc9aed1. It fixes the profiling conversion
when the selected Vulkan queue's timestamp counter wraps. Previously START and
END were independently rescaled as absolute nanoseconds; a48bit wrap made END
smaller than START and could underflow the calibrated host offset.

The queue now retains timestampValidBits. Query pairs remain raw until both are
converted with one locked calibration. Signed modular deltas are rescaled to
nanoseconds around that host anchor. The1ns path preserves all64integer bits;
fractional periods round towards the floor of the final timestamp. Reversed,
half-cycle ambiguous and unrepresentable intervals fail instead of clamping or
fabricating durations. Queries and their calibration must be within half a
counter cycle (about84.8days for Turnip's48bit19.2MHz counter).

Windows calibration still consumes raw QPC ticks and the actual QPF. Public
clGetDeviceAndHostTimer keeps its existing raw-device-nanosecond API behavior;
this fix applies to event profiling. Calibration uncertainty is traced as
max_deviation_ns. It is not a correction to add to either timestamp, and no
event-ordering or semantic assertion has been relaxed to accommodate it.

DEVICE_TIMESTAMP_QUERY now records mapped host nanoseconds with clock=host and
valid_bits, alongside both original raw queries. CALIBRATED_SAMPLE records raw
DEVICE/QPC, converted host nanoseconds, uncertainty and period. This allows a
real integration failure to distinguish unit conversion, rollover, calibration
uncertainty and GPU query ordering. Cross-calibration jitter remains a hardware
validation concern; this change does not claim to resolve an unobserved failure.

The production clock test covers48/64bit wrap with anchors on either side,
unused upper bits, fractional periods, underflow/overflow, reversed and
ambiguous intervals, and20,000query pairs against an independent unwrapped
signed-rational oracle. The existing full compiled-kernel0/1/7controls and
strict batch END<=nextSTART checks remain the runtime acceptance gate.

The companion host/KMD/Mesa timestamp bridge is independently owned. No Vulkan
extension is fabricated, no registration changes are made, and a passing
software/ABI CI run does not establish target GPU or GB7 acceptance.
