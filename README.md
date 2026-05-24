# Windows 10 Latency Optimizer

A command-line Windows 10 latency optimization tool. No installer, no GUI, no telemetry, no network connections. Run it from an elevated CMD, choose what to apply, restart.

All techniques implemented here are sourced from the competitive gaming and systems performance communities: [valleyofdoom/PC-Tuning](https://github.com/valleyofdoom/PC-Tuning), [djdallmann/GamingPCSetup](https://github.com/djdallmann/GamingPCSetup), and Blur Busters forums. Nothing in this tool is novel or undocumented — it automates what experienced users apply manually.


## Security and Transparency

This tool modifies Windows registry keys, service startup types, power plan settings, and bcdedit boot parameters. It does not:

- Make any network connections
- Collect or transmit any data
- Install drivers, services, or persistent agents (the timer daemon is a plain process spawned from the same binary, killable with `latency.exe timer stop` or Task Manager)
- Execute obfuscated or downloaded code
- Modify any executable files on disk

Every registry path and value written is listed in this README and visible in the source code. Every change has a corresponding restore path — running `latency.exe restore` reverts all modifications to Windows defaults. The tool has been compiled with MinGW from a single C++17 source file with no external dependencies beyond standard Win32 APIs.

If your antivirus flags this binary, it is due to heuristic detection of Win32 API calls that are also used by malware (registry writes, process spawning, service control). The source is fully auditable.


## Build

Requires either MSVC (`cl.exe`) or MinGW (`g++`) in PATH.

```
build.bat
```

Or manually:

```
g++ main.cpp -O2 -std=c++17 -Wall -static -o latency.exe -ladvapi32 -lpowrprof -lshlwapi -lole32
```


## Usage

Must be run from an elevated (Administrator) command prompt for most commands.

```
latency.exe                   Open interactive menu
latency.exe apply             Apply all 13 optimization modules
latency.exe restore           Revert all changes to Windows defaults
latency.exe status            Display current state of every tracked setting
latency.exe timer start       Start the 0.5ms timer resolution daemon
latency.exe timer stop        Kill the timer daemon
latency.exe timer status      Show current timer resolution and daemon state
latency.exe help              Full command reference

latency.exe apply --boot      Apply bcdedit boot tweaks only
latency.exe apply --vbs       Disable VBS/HVCI only
latency.exe apply --spectre   Disable Spectre/Meltdown mitigations only
latency.exe restore --boot    Undo bcdedit changes only
latency.exe restore --vbs     Re-enable VBS/HVCI only
latency.exe restore --spectre Re-enable Spectre mitigations only
```


## What It Applies

### Module 1 — Registry Tweaks

All changes are written to HKLM or HKCU and are fully reversible.

**MMCSS (Multimedia Class Scheduler Service)**

| Key | Value | Effect |
|-----|-------|--------|
| `HKLM\...\Multimedia\SystemProfile\SystemResponsiveness` | 0 | Allocates 100% of CPU time to high-priority multimedia tasks instead of the default 20% cap |
| `HKLM\...\Multimedia\SystemProfile\NoLazyMode` | 1 | Removes idle sleep chains from the MMCSS scheduler thread |
| `HKLM\...\Multimedia\SystemProfile\NetworkThrottlingIndex` | 0xFFFFFFFF | Removes the 10 packets/ms network throughput cap applied to non-MMCSS threads |
| `HKLM\...\Tasks\Games\GPU Priority` | 8 | Maximum GPU scheduling priority for the Games MMCSS task class |
| `HKLM\...\Tasks\Games\Priority` | 6 | Elevated CPU scheduling priority |
| `HKLM\...\Tasks\Games\Scheduling Category` | High | Places the task in the highest MMCSS scheduling category |
| `HKLM\...\Tasks\Games\Affinity` | 0 | Allows the scheduler to use all CPU cores |
| `HKLM\...\Tasks\Games\Background Only` | False | Enables foreground priority boosting |
| `HKLM\...\Tasks\Games\Clock Rate` | 10000 | Sets the scheduler clock resolution for this task class to 1ms (10000 x 100ns units) |
| `HKLM\...\Tasks\Games\SFIO Priority` | High | High-priority storage I/O scheduling for game asset streaming |

**Scheduler and Memory**

| Key | Value | Effect |
|-----|-------|--------|
| `HKLM\SYSTEM\CurrentControlSet\Control\PriorityControl\Win32PrioritySeparation` | 38 | Short variable quanta with maximum foreground boost (0x26: short + variable + 3 boost levels) |
| `HKLM\SYSTEM\...\Memory Management\DisablePagingExecutive` | 1 | Keeps kernel code and drivers resident in physical RAM, preventing page fault latency on kernel calls |
| `HKLM\SYSTEM\CurrentControlSet\Control\SvcHostSplitThresholdInKB` | RAM size in KB | Forces Windows to use shared svchost processes instead of per-service isolation, reducing context switch overhead |
| `HKLM\SYSTEM\...\Memory Management\LargeSystemCache` | 0 | Configures the memory manager in workstation mode, prioritizing application working sets over file system cache |

**NTFS File System**

| Key | Value | Effect |
|-----|-------|--------|
| `HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\NtfsDisableLastAccessUpdate` | 0x80000001 | Disables last-access timestamp writes on file open operations, eliminating a write I/O event on every file read. The 0x8000000x format persists across reboots on Windows 8+ |
| `HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\NtfsDisable8dot3NameCreation` | 1 | Disables generation of legacy 8.3 short filenames, reducing overhead on file and directory creation |
| `HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\NtfsMemoryUsage` | 2 | Allocates more paged pool memory to the NTFS metadata cache, reducing directory lookup latency under load |

**Timer and Interrupt Handling**

| Key | Value | Effect |
|-----|-------|--------|
| `HKLM\SYSTEM\...\Session Manager\kernel\GlobalTimerResolutionRequests` | 1 | Restores pre-Windows 10 2004 behavior: any process requesting a higher timer frequency raises the global resolution system-wide, not just for its own process. Required for games that do not call NtSetTimerResolution themselves to benefit from the daemon |
| `HKLM\SYSTEM\...\Session Manager\Power\CoalescingTimerInterval` | 0 | Disables timer coalescing, which normally groups timer expiry events together to reduce CPU wake-ups at the cost of scheduling jitter |
| `HKLM\SOFTWARE\Policies\Microsoft\Windows\Psched\TimerResolution` | 1 | Sets the packet scheduler timer resolution to 1 microsecond, stabilizing ndis.sys DPC latency |

**GPU**

| Key | Value | Effect |
|-----|-------|--------|
| `HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\HwSchMode` | 2 | Enables Hardware Accelerated GPU Scheduling (HAGS, WDDM 2.7+). The GPU manages its own command queue instead of relying on the CPU scheduler, reducing frame-time variance |
| `HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\TdrDelay` | 60 | Increases the GPU Timeout Detection and Recovery timeout from 2 to 60 seconds, preventing spurious driver resets during shader compilation or heavy compute workloads |

**Input Devices**

| Key | Value | Effect |
|-----|-------|--------|
| `HKLM\...\mouclass\Parameters\MouseDataQueueSize` | 16 | Reduces the mouse driver input queue from 100 to 16 packets. Older events are discarded sooner, reducing accumulated input lag under load |
| `HKLM\...\kbdclass\Parameters\KeyboardDataQueueSize` | 16 | Same reduction applied to the keyboard input queue |
| `HKCU\Control Panel\Mouse\MouseSpeed/Threshold1/Threshold2` | 0/0/0 | Disables pointer acceleration, producing a 1:1 linear mapping between physical and on-screen movement |

**Network**

| Key | Value | Effect |
|-----|-------|--------|
| `HKLM\...\Tcpip\Parameters\Interfaces\*\TcpAckFrequency` | 1 | Sends a TCP acknowledgment after every received segment instead of waiting to batch them, disabling the Nagle algorithm at the interface level |
| `HKLM\...\Tcpip\Parameters\Interfaces\*\TCPNoDelay` | 1 | Companion to TcpAckFrequency; disables segment coalescing on the send side |
| `HKLM\...\Services\LanmanServer\Parameters\IrpStackSize` | 30 | Increases the SMB server IRP stack depth, preventing buffer overflow errors in high-throughput local file sharing and named pipe communication |
| `HKLM\...\Services\Tcpip6\Parameters\DisabledComponents` | 0x20 | Deprioritizes IPv6 in the prefix policy table, forcing the stack to prefer IPv4. Does not disable IPv6 entirely |

**Other**

| Key | Value | Effect |
|-----|-------|--------|
| `HKCU\System\GameConfigStore\GameDVR_Enabled` | 0 | Disables Game DVR capture overlay |
| `HKLM\...\FTH\Enabled` | 0 | Disables the Fault Tolerant Heap, which introduces non-deterministic allocation delays in monitored processes |
| `HKLM\SYSTEM\...\Session Manager\Power\HiberbootEnabled` | 0 | Disables Fast Startup (hybrid shutdown), ensuring a clean driver state on every boot |
| `HKCU\...\Themes\Personalize\EnableTransparency` | 0 | Disables DWM transparency compositing |
| `HKLM\...\DataCollection\AllowTelemetry` | 0 | Caps telemetry collection at the minimum permitted level |
| `HKLM\...\Schedule\Maintenance\MaintenanceDisabled` | 1 | Disables automatic maintenance tasks that can interrupt foreground workloads |
| `HKLM\SYSTEM\...\CrashControl\CrashDumpEnabled` | 0 | Disables kernel crash dump generation |
| `HKLM\SOFTWARE\...\AppPrivacy\LetAppsRunInBackground` | 2 | Blocks UWP applications from executing background tasks |
| `HKCU\Control Panel\Desktop\AutoEndTasks` | 1 | Enables automatic termination of unresponsive applications |
| `HKCU\Control Panel\Desktop\WaitToKillAppTimeout` | 2000 | Reduces the hang detection threshold to 2 seconds |


### Module 2 — Services

The following services are set to disabled startup type and stopped if running. All can be manually started afterwards if needed, and `latency.exe restore` returns them to their original startup types.

| Service | Default | Reason for disabling |
|---------|---------|----------------------|
| SysMain | Automatic | Superfetch prefetcher competes with game workloads for memory bandwidth |
| DiagTrack | Automatic | Connected User Experiences telemetry service |
| XboxGipSvc | Manual | Xbox Game Input Protocol driver host |
| XblAuthManager | Manual | Xbox Live authentication background service |
| XblGameSave | Manual | Xbox Live cloud save synchronization |
| WerSvc | Manual | Windows Error Reporting service |
| DoSvc | Automatic | Delivery Optimization peer-to-peer update distribution |
| CDPSvc | Automatic | Connected Devices Platform host |
| WSearch | Automatic | Windows Search indexer |
| PcaSvc | Automatic | Program Compatibility Assistant |
| lfsvc | Manual | Geolocation sensor polling service |
| MapsBroker | Automatic | Downloaded Maps Manager |
| RetailDemo | Disabled | Retail store demo mode service |
| SensorService | Manual | Sensor framework manager |
| SensorDataService | Manual | Sensor data acquisition |
| SensrSvc | Manual | Sensor monitoring service |
| Fax | Manual | Fax service |
| PrintNotify | Manual | Printer notification service |
| TabletInputService | Manual | Tablet PC input service |
| RemoteRegistry | Disabled | Remote registry access |
| WMPNetworkSvc | Manual | Windows Media Player network sharing |
| SSDPSRV | Manual | SSDP Discovery (UPnP device detection) |
| upnphost | Manual | UPnP Device Host |
| fdPHost | Manual | Function Discovery Provider Host |
| FDResPub | Automatic | Function Discovery Resource Publication |
| WbioSrvc | Manual | Windows Biometric Service |
| stisvc | Manual | Windows Image Acquisition (scanner framework) |
| SharedAccess | Disabled | Internet Connection Sharing |


### Module 3 — Network Adapter Properties

Applied via PowerShell `Set-NetAdapterAdvancedProperty` to all physical adapters. Properties are adapter-specific; the command is silently skipped if the adapter does not expose the property.

| Property | Applied Value | Effect |
|----------|--------------|--------|
| `*InterruptModeration` | Disabled | Each received packet triggers an immediate CPU interrupt. Disabling interrupt moderation eliminates the hardware-side batching delay at the cost of higher CPU interrupt rate |
| `*EEE` | Disabled | Prevents the NIC from entering IEEE 802.3az low-power idle states during traffic gaps, eliminating the link wake-up latency that occurs when traffic resumes |
| `*FlowControl` | Disabled | Removes IEEE 802.3x pause-frame processing. The NIC will not halt transmission waiting for the remote end to drain its receive buffer |
| `*RSC` | Disabled | Disables Receive Segment Coalescing. TCP segments are delivered to the network stack individually rather than being merged at the hardware level, reducing per-packet latency at the cost of throughput efficiency |


### Module 4 — TCP/IP Stack

Applied via `netsh int tcp set global` and registry writes.

| Setting | Applied Value | Effect |
|---------|--------------|--------|
| autotuninglevel | disabled | Disables the TCP receive window auto-tuning algorithm, which can introduce latency spikes when the tuning heuristic adjusts the window size mid-stream |
| rss | enabled | Receive Side Scaling distributes NIC interrupt processing across multiple CPU cores |
| chimney | disabled | Disables TCP Chimney Offload, preventing synchronization overhead between the CPU network stack and NIC offload engine |
| ecncapability | disabled | Disables Explicit Congestion Notification to avoid compatibility issues with routers that drop ECN-marked packets |
| timestamps | disabled | Removes per-packet TCP timestamp overhead |
| heuristics | disabled | Disables the TCP heuristics engine that dynamically adjusts protocol behavior |
| congestionprovider | ctcp | Compound TCP congestion control algorithm, which provides better throughput characteristics than the default NewReno on lossy or high-latency paths |
| initialRto | 2000 ms | Halves the initial retransmission timeout, allowing failed connection attempts to surface and retry faster |
| minRto | 300 ms | Reduces the minimum retransmission timeout, beneficial on low-latency LAN paths |
| IPv6 DisabledComponents | 0x20 | Deprioritizes IPv6 in the prefix policy table |


### Module 5 — Power Plan

Creates and activates the Ultimate Performance power plan (`{e9a42b02-d5df-448d-aa00-03f14749eb61}`), which is identical to High Performance with processor idle allowed set to 0% and all power-saving heuristics removed. Disables USB selective suspend to prevent USB controller power gating latency.


### Module 6 — CPU Power Settings

Applied via `powercfg /setacvalueindex` on SCHEME_CURRENT after activating Ultimate Performance.

| Setting GUID | Value | Effect |
|-------------|-------|--------|
| 0cc5b647 (CPMINCORES) | 100% | Disables core parking. All logical cores remain active regardless of load, eliminating the latency spike when a parked core has to spin up to handle a burst |
| 5d76a2ca (IDLEDISABLE) | 1 | Prevents the CPU from entering C-states when idle. The processor spins at full clock instead of sleeping. Eliminates C-state exit latency entirely. Increases power consumption significantly |
| be337238 (PERFBOOSTMODE) | 3 (Aggressive) | CPU boosts immediately on any detected load increase without waiting for a utilization threshold |
| 7b224883 (IDLECHECK) | 0 ms | Sets the idle detection interval to minimum, making the scheduler check and act on idle state as frequently as possible |
| 893dee8e (PROCTHROTTLEMIN) | 100% | Prevents the processor from dropping below its maximum frequency |
| ee12f906 (PCIe LSPM) | 0 (Off) | Disables PCIe Active State Power Management. GPU and NIC will not enter L0s or L1 link power states, eliminating the link re-activation latency that occurs on traffic bursts |
| 8baa4a8a (PERFAUTONOMOUS) | 0 | Disables the processor's autonomous frequency scaling algorithm, forcing the explicit performance policy |


### Module 7 — MSI Mode

Enumerates `HKLM\SYSTEM\CurrentControlSet\Enum\PCI` and sets `MSISupported=1` in the `Device Parameters\Interrupt Management\MessageSignaledInterruptProperties` subkey for every device with Class=Display or Class=Net.

Message Signaled Interrupts route interrupt signals through the PCI bus as memory writes rather than asserting a physical IRQ line. This eliminates interrupt sharing between devices and allows the CPU to process each interrupt in isolation through its Local APIC, reducing DPC latency on both GPU and NIC. Requires a restart to take effect.


### Module 8 — NVIDIA PowerMizer

Enumerates `HKLM\SYSTEM\CurrentControlSet\Control\Video\{GUID}\0000` and identifies NVIDIA adapters by DriverDesc. Writes:

| Value | Setting | Effect |
|-------|---------|--------|
| PerfLevelSrc | 0x2222 | Forces a fixed performance level on both AC and battery power |
| PowerMizerEnable | 1 | Activates the PowerMizer override |
| PowerMizerLevel | 1 | Locks the GPU to its maximum performance state |
| PowerMizerLevelAC | 1 | Same behavior on AC power |

Without this, the GPU driver may clock down between frames when instantaneous load drops below a threshold, producing a visible clock-up latency spike at the start of the next heavy frame.


### Module 9 — Visual Effects

Disables DWM compositing effects that add GPU overhead without contributing to application rendering.

| Setting | Value | Effect |
|---------|-------|--------|
| VisualFXSetting | 2 | Sets system visual effects to custom mode, allowing individual control |
| DragFullWindows | 0 | Renders only the window frame outline during drag operations |
| MenuShowDelay | 0 | Removes the artificial menu popup delay |
| MinAnimate | 0 | Disables minimize and maximize window animations |
| TaskbarAnimations | 0 | Disables taskbar button animations |
| ListviewShadow | 0 | Removes desktop icon drop shadows |
| ListviewAlphaSelect | 0 | Removes translucent selection rectangle rendering |


### Module 10 — Boot Parameters

Applied via `bcdedit`. Requires a restart to take effect.

| Parameter | Value | Effect |
|-----------|-------|--------|
| disabledynamictick | yes | Disables dynamic tick (tickless idle). The system clock ticks at a fixed rate rather than suppressing ticks during CPU idle periods, preventing the scheduling jitter introduced when the first tick after an idle period coalesces deferred work |
| useplatformclock | false | Directs the kernel to use the TSC (Time Stamp Counter) as the primary clock source rather than HPET or ACPI PM Timer. On modern hardware with invariant TSC, this provides lower-latency timer reads |
| tscsyncpolicy | Enhanced | Enforces enhanced TSC synchronization across all logical processors. Reduces per-core timer skew on multi-socket and multi-die configurations, which can otherwise cause DPC latency spikes on thread migration |
| quietboot | yes | Suppresses the boot progress animation |


### Module 11 — VBS / HVCI

Disables Virtualization-Based Security and Hypervisor-Enforced Code Integrity by writing to `HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard` and running `bcdedit /set hypervisorlaunchtype off`.

On Intel CPUs produced before approximately 2019, VBS adds 5-15% overhead to kernel-mode transitions due to the additional VMX root/non-root mode switching. On CPUs with hardware virtualization extensions optimized for VBS (Tiger Lake and later), the impact is minimal. Requires a restart.

This is a security tradeoff. VBS protects against kernel-mode exploits by enforcing code integrity from a hypervisor context that the compromised kernel cannot reach. Disable only on machines dedicated to gaming or performance testing.


### Module 12 — Spectre/Meltdown Mitigations

Sets `FeatureSettingsOverride=3` and `FeatureSettingsOverrideMask=3` in `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management`. This disables the software mitigations for Spectre variant 2 (branch target injection) and Meltdown (rogue data cache load).

On Intel Core CPUs without silicon-level mitigations (roughly pre-2019 microarchitectures), these software patches add 4-10% overhead to system call paths. On CPUs with IBRS/IBPB implemented in microcode or silicon (Ice Lake, Tiger Lake, Alder Lake and later), the mitigations have near-zero overhead and disabling them provides no measurable benefit.

This is a security tradeoff. These vulnerabilities allow unprivileged code to read kernel memory through speculative execution side channels. Disable only on isolated gaming machines with no sensitive data and no exposure to untrusted code execution.


### Module 13 — Timer Resolution Daemon

Calls `NtSetTimerResolution(5000, TRUE)` via the undocumented but stable NT API exported from ntdll.dll. The target resolution of 5000 (in 100-nanosecond units) corresponds to 0.5ms, raising the system timer interrupt frequency to 2000Hz.

Starting with Windows 10 build 19041 (version 2004), Microsoft changed timer resolution to be per-process rather than global. A process requesting a higher resolution no longer affects other processes. The daemon is a background process spawned from the same binary that holds the resolution request alive indefinitely. `GlobalTimerResolutionRequests=1` (Module 1) restores the pre-2004 global behavior so that the daemon's resolution request benefits all processes system-wide.

The daemon is identified by a named mutex (`Global\LatencyOptimizerDaemon_v1`) and writes its PID to `daemon.pid` in the tool directory. `latency.exe timer stop` terminates it cleanly.


## Notes

MSI mode, HAGS, boot parameters, VBS, and Spectre mitigations all require a system restart to take effect.

Modules 11 and 12 involve security tradeoffs and are intended for dedicated gaming or benchmarking machines. Review the descriptions above before applying them on a machine with sensitive data or exposure to untrusted code.

Results vary by hardware. Measuring with LatencyMon before and after is recommended to confirm that DPC latency has improved on your specific configuration. Timer accuracy can be verified with Tier1Timer.

`latency.exe restore` reverts all registry changes, service startup types, and power plan settings to their Windows defaults. Boot and VBS/Spectre changes require a restart after restore to fully take effect.


## Sources

- valleyofdoom/PC-Tuning: https://github.com/valleyofdoom/PC-Tuning
- djdallmann/GamingPCSetup: https://github.com/djdallmann/GamingPCSetup
- Blur Busters Forums: https://forums.blurbusters.com
- Microsoft MMCSS documentation: https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service
- Microsoft bcdedit reference: https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/bcdedit--set


## License

MIT
