# RotationEngine and group behavior

A group joins conditioners for display and, optionally, coordinated control. A MANUAL group only organizes cards. An AUTO group is managed by RotationEngine when the separate global automation switch is enabled.

## N and M

- **N — required active count.** The number of healthy, operating conditioners the engine tries to maintain during normal operation.
- **M — maximum active count.** The ceiling for simultaneous working units, including a powered conditioner temporarily assigned to per-unit Manual control.

If N=1 and M=2, one healthy unit normally runs. A second may be started after fresh T1 remains above the configured high-temperature threshold for its delay, or to replace a unit treated as failed. After T1 remains below the recovery threshold for its delay, the engine returns to N. If M=N, temperature demand cannot add another active unit.

The group temperature used for the high and recovery thresholds is the highest fresh, confirmed T1 among group members. Missing or stale T1 is not treated as a temperature of zero.

## Reserve and failover

If fewer than N healthy units remain, the engine tries an eligible reserve. A brief missed response is not enough to declare a unit offline: the bus needs three consecutive failed polls by default. Until the state is sufficiently clear, the engine pauses a reserve start to reduce the chance of running an unobserved unit concurrently. A known faulted unit is stopped and its OFF state is verified before the reserve is started.

The start-verification interval controls how long the engine waits for observable confirmation after a start command. Minimum on/off times prevent rapid cycling. A requested command is not considered successful just because it was queued.

## Scheduled rotation and handover

When enabled, scheduled rotation selects a different eligible member after the configured interval and local time.

- With a positive handover delay, the engine starts the replacement, verifies it, waits for the overlap interval, then stops the previous unit. This requires a free slot under M.
- With a handover delay of zero, it confirms that the old unit has stopped before starting the replacement. This avoids intentional overlap but can cause a short cooling pause.

If a replacement fails to start during break-before-make rotation, the engine attempts to restore the old unit. No software schedule can guarantee that a physical conditioner accepted a command if the unit or bus is faulty.

## Ventilation reserve

When «Ventilation for reserves» is enabled for a compatible AUTO group, eligible inactive members stay powered in Fan mode at the group's fan setting. These fan-only reserves do not count toward N or M. The engine promotes one to the group operating mode when it is needed. When the option is off, inactive reserves are powered off.

## Per-conditioner Manual control in an AUTO group

The per-unit Manual control option lets an operator temporarily control one member without removing it from the group. While enabled, the group does not reconcile that unit's settings. A powered manual-control member consumes an M slot but does not satisfy N; if room remains under M, the engine can start another eligible member to maintain N.

Clear the option to return the conditioner to group control. Changing settings from the air-conditioner remote while the member remains under AUTO control may cause the engine to restore the group settings after a poll.

## Practical setup

Begin with global automation off. Validate each model and its C0 readings, create a MANUAL group first, and check the group's T1 value. Then configure N/M and thresholds conservatively, confirm that reserve selection and handover behavior match the installation, and enable AUTO control only after those checks.

The current firmware limits the registry to 16 conditioners and 16 groups. Group runtime, the next scheduled rotation, and countdown diagnostics are available in /json.
