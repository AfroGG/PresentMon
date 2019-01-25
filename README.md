# PresentMon

PresentMon is a tool to trace
[ETW](https://msdn.microsoft.com/en-us/library/windows/desktop/bb968803%28v=vs.85%29.aspx?f=255&MSPPError=-2147217396)
events related to swap chain presentation on Windows.  It can be used to
capture and analyze key performance metrics for graphics applications (e.g.,
CPU and Display frame durations and latencies) and works across all graphics
APIs, including UWP applications.

## License

Copyright 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Releases

Binaries for main release versions of PresentMon are provided on GitHub:
- [Latest release](https://github.com/GameTechDev/PresentMon/releases/latest)
- [List of all releases](https://github.com/GameTechDev/PresentMon/releases)

Please see
[CONTRIBUTING](https://github.com/GameTechDev/PresentMon/blob/master/CONTRIBUTING.md)
for information on how to request features, report issues, or contribute code
changes.

## Command line options

```html
Capture target options:
    -captureall                Record all processes (default).
    -process_name [exe name]   Record only the named process. If multiple processes with
                               the same image name are running, it is undefined which one
                               will be recorded. This argument can be repeated to capture
                               more than one process at the same time.
    -process_id [integer]      Record only the process specified by ID.
    -etl_file [path]           Consume events from an ETL file instead of running processes.

Output options:
    -no_csv                    Do not create any output file.
    -output_file [path]        Write CSV output to specified path. See below for defaults.
    -multi_csv                 Create a separate CSV file for each captured process.

Control and filtering options:
    -exclude [exe name]        Don't record specific process specified by name; this argument can be
                               repeated to exclude multiple processes.
    -scroll_toggle             Only record events while scroll lock is enabled.
    -scroll_indicator          Set scroll lock while recording events.
    -hotkey [key]              Use specified key to start and stop recording, writing to a
                               unique file each time (default is F11).
    -delay [seconds]           Wait for specified time before starting to record. When using
                               -hotkey, delay occurs each time recording is started.
    -timed [seconds]           Stop recording after the specified amount of time.  PresentMon will exit
                               timer expires.
    -exclude_dropped           Exclude dropped presents from the csv output.
    -terminate_on_proc_exit    Terminate PresentMon when all instances of the specified process exit.
    -terminate_after_timed     Terminate PresentMon after the timed trace, specified using -timed, completes.
    -simple                    Disable advanced tracking (try this if you encounter crashes).
    -verbose                   Adds additional data to output not relevant to normal usage.
    -dont_restart_as_admin     Don't try to elevate privilege.
    -no_top                    Don't display active swap chains in the console window.
    -session_name [name]       Use the specified name to start a new realtime ETW session, instead
                               of the default "PresentMon". This can be used to start multiple
                               realtime capture process at the same time (using distinct names).
                               A realtime PresentMon capture cannot start if there are any
                               existing sessions with the same name.
    -stop_existing_session     If a trace session with the same name is already running, stop
                               the existing session (to allow this one to proceed).
    -include_mixed_reality     [Beta] Include Windows Mixed Reality data. If enabled, writes csv output
                               to a separate file (with "_WMR" suffix).
```

## Comma-separated value (CSV) file output

### CSV File Names

The default CSV file name is `PresentMon-TIME.csv`, where `TIME` is the capture
time in ISO 8601 format.

If the CSV file contains a single target process (`-process_id` or one
`-process_name` or `-multi_csv`) then the CSV filename is
`PresentMon-PROCESSNAME-TIME.csv`.

If an output file name is provided with `-output_file PATH.EXT` then that name
will be used, but `-PROCESSNAME` is appended to `PATH` if `-multi_csv`, and
`-INDEX` is appended if `-hotkey` where `INDEX` increases each time the hotkey
is used.

In all cases, `_WMR` is added to the file name if `-include_mixed_reality` is
used.

### Simple Columns (-simple command line argument)

| CSV Column Header | CSV Data Description |
|---|---|
| Application            | Process name (if known) |
| ProcessID              | Process ID |
| SwapChainAddress       | Swap chain address |
| Runtime                | Swap chain runtime (e.g., D3D9 or DXGI) |
| SyncInterval           | Sync interval used |
| PresentFlags           | Present flags used |
| Dropped                | Whether the present was dropped (1) or displayed (0) |
| TimeInSeconds          | Time since PresentMon recording started |
| MsBetweenPresents      | Time between this Present() API call and the previous one |
| MsInPresentAPI         | Time spent inside the Present() API call |

### Default Columns

All of the above columns, plus:

| CSV Column Header | CSV Data Description |
|---|---|
| AllowsTearing          | Whether tearing possible (1) or not (0) |
| PresentMode            | Present mode |
| MsBetweenDisplayChange | Time between when this frame was displayed, and previous was displayed |
| MsUntilRenderComplete  | Time between present start and GPU work completion |
| MsUntilDisplayed       | Time between present start and frame display |

### Verbose Columns (-verbose command line argument)

All of the above columns above, plus:

| CSV Column Header | CSV Data Description |
|---|---|
| WasBatched  | The frame was submitted by the driver on a different thread than the app |
| DwmNotified | The desktop compositor was notified about the frame. |


## Windows Mixed Reality comma-separated value (CSV) file output

### Simple Columns (-simple command line argument)

| CSV Column Header | CSV Data Description |
|---|---|
| Application               | Process name (if known) |
| ProcessID                 | Process ID |
| DwmProcessID              | Compositor Process ID |
| TimeInSeconds             | Time since PresentMon recording started |
| MsBetweenLsrs             | Time between this Lsr CPU start and the previous one |
| AppMissed                 | Whether Lsr is reprojecting a new (0) or old (1) App frame (App GPU work must complete before Lsr CPU start) |
| LsrMissed                 | Whether Lsr displayed a new frame (0) or not (1+) at the intended V-Sync (Count V-Syncs with no display change) |
| MsAppPoseLatency          | Time between App's pose sample and the intended mid-photon frame display |
| MsLsrPoseLatency          | Time between Lsr's pose sample and the intended mid-photon frame display |
| MsActualLsrPoseLatency    | Time between Lsr's pose sample and mid-photon frame display |
| MsTimeUntilVsync          | Time between Lsr CPU start and the intended V-Sync |
| MsLsrThreadWakeupToGpuEnd | Time between Lsr CPU start and GPU work completion |
| MsLsrThreadWakeupError    | Time between intended Lsr CPU start and Lsr CPU start |
| MsLsrPreemption           | Time spent preempting the GPU with Lsr GPU work |
| MsLsrExecution            | Time spent executing the Lsr GPU work |
| MsCopyPreemption          | Time spent preempting the GPU with Lsr GPU cross-adapter copy work (if required) |
| MsCopyExecution           | Time spent executing the Lsr GPU cross-adapter copy work (if required) |
| MsGpuEndToVsync           | Time between Lsr GPU work completion and V-Sync |

### Default Columns

All of the above columns, plus:

| CSV Column Header | CSV Data Description |
|---|---|
| MsBetweenAppPresents   | Time between App's present and the previous one |
| MsAppPresentToLsr      | Time between App's present and Lsr CPU start |

### Verbose Columns (-verbose command line argument)

All of the above columns above, plus:

| CSV Column Header | CSV Data Description |
|---|---|
| HolographicFrameID                           | App's Holographic Frame ID |
| MsSourceReleaseFromRenderingToLsrAcquire     | Time between composition end and Lsr acquire |
| MsAppCpuRenderFrame                          | Time between App's CreateNextFrame() API call and PresentWithCurrentPrediction() API call |
| MsAppMisprediction                           | Time between App's intended pose time and the intended mid-photon frame display |
| MsLsrCpuRenderFrame                          | Time between Lsr CPU render start and GPU work submit |
| MsLsrThreadWakeupToCpuRenderFrameStart       | Time between Lsr CPU start and CPU render start |
| MsCpuRenderFrameStartToHeadPoseCallbackStart | Time between Lsr CPU render start and pose sample |
| MsGetHeadPose                                | Time between Lsr pose sample start and pose sample end |
| MsHeadPoseCallbackStopToInputLatch           | Time between Lsr pose sample end and input latch |
| MsInputLatchToGpuSubmission                  | Time between Lsr input latch and GPU work submit |

