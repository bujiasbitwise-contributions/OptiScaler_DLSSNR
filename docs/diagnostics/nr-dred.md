# NR GPU fault diagnostic

This is an evidence-gathering build, not a claimed crash fix.

## Test

Use your existing working installation and configuration. Back up the currently installed
OptiScaler DLL, then replace it with this build's OptiScaler DLL using the same filename
as the installed DLL. Keep your existing INI, NR model, forwarder, and other DLLs.
The build includes `NR-DIAGNOSTIC-BUILD.txt` with its exact commit.

Launch Cyberpunk 2077 normally with the D3D12 debug layer off and no WinDbg attached.
Use native MFG x4 and the same fixed resolution as the failed 512-slot test. Repeat the
NR on/off action that caused that crash. Stop at the first crash; if it stays stable,
stop after 20 on/off cycles or five minutes, whichever comes first.

Save the complete `OptiScaler.log` before launching again. Send that log and say whether
it crashed. No debugger commands or Windows settings changes are needed for this build's
own diagnostics. A startup line containing `NR-DIAG dred-v1` identifies the build.

## What the build records

- Enables DRED breadcrumbs, breadcrumb contexts when supported, and page-fault tracking
  before intercepted D3D12 device creation. It does not enable the D3D12 validation layer.
- Names NR's descriptor heaps, constant buffers, scratch textures, guide clones, and timing objects.
- Retains 128 CPU recording events in memory, including command-list/resource pointer values,
  thread IDs, shader mode and descriptor slot. It writes this history only after device loss.
- On observed device loss, logs DRED's fault address, matching existing/recently freed
  allocations, and bounded history around incomplete command lists' GPU breadcrumb counters.
- Uses the existing device-removal reporting paths, plus a check on entering NR dispatch.
- Keeps 512 slots and the same rendering, synchronization, and retirement behavior as the
  failed test. This is based on `beb7ca7bf32a63d3c3cfe2b2c8ac6e5df6169044`.

## How to interpret the result

CPU recording events do not prove GPU completion. Successful earlier Presents do not
exclude a later GPU fault. Breadcrumb counters help locate outstanding work but are not
guaranteed to identify the exact faulting instruction. Empty allocation matches do not
exclude invalid descriptors or lifetime errors. An unhandled CPU exception that terminates
the process before a removal-reporting path runs may produce no DRED dump in this log.

The 512-slot build crashed on the first reported attempt. That means enlargement did not
resolve this reproduction; it does not establish that descriptor reuse is correct.
The WinDbg resolution-change failure remains unresolved.

Source review at this revision also found frame-count-based retirement, fixed-depth query
and readback reuse, and exposure scanning that assumes resource states. These are investigation
targets, not proven causes. A state-restoration envelope already exists around the steady-state
NR passes; its presence and scope must be considered before proposing another restoration fix.

Reference: https://learn.microsoft.com/en-us/windows/win32/direct3d12/use-dred
