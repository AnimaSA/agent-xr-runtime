# AgentXR

AgentXR is a Windows OpenXR runtime built for deterministic, agent-driven XR testing. It supplies tracked head/controller poses, Oculus Touch-compatible actions, D3D12 stereo composition, frame evidence, image capture, and timeline control through a local MCP server.

AgentXR does not replace the machine-wide OpenXR runtime. Launch helpers set `XR_RUNTIME_JSON` only for the target process and restore caller environment.

## Current scope

- Windows x64
- OpenXR 1.0 runtime interface
- `XR_KHR_D3D12_enable`
- Primary stereo view configuration
- `LOCAL`, `STAGE`, `VIEW`, and `LOCAL_FLOOR_EXT` spaces
- Core `/interaction_profiles/oculus/touch_controller` bindings
- Boolean, float, vector2, pose, and haptic actions
- Deterministic schema-version-1 timelines
- Local named-pipe control, MCP orchestration, frame reports, and PNG capture

Not implemented: headset display hardware, passthrough, hand tracking, eye tracking, audio, Vulkan, OpenGL, or non-Windows platforms.

## Requirements

- Windows 10 or 11
- Visual Studio 2022 C++ toolchain
- Windows SDK with D3D12
- CMake 3.24 or newer

OpenXR loader and JSON dependencies are vendored under `third_party`; build requires no network fetch.

## Build

From repository root:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
cmake --install build --config RelWithDebInfo --prefix dist
```

Installed files:

- `dist/AgentXRRuntime.dll` — OpenXR runtime
- `dist/agent-xr.json` — portable runtime manifest template
- `dist/agent-xr-mcp.exe` — MCP server
- `dist/agent-xr-smoke.exe` — native smoke runner
- `dist/launch-agentxr-editor.ps1` — process-scoped shortcut launcher
- `dist/examples/*.json` — generic timeline examples

## Launch application with AgentXR

Use launcher with shortcut whose target starts XR application:

```powershell
./dist/launch-agentxr-editor.ps1 `
  -ShortcutPath "C:/path/to/Editor.lnk" `
  -ExpectedProjectPath "C:/path/to/Project.uproject"
```

`ExpectedProjectPath` is optional. When supplied, launcher rejects shortcut whose arguments do not contain expected path.

Launcher:

1. Resolves `AgentXRRuntime.dll` to absolute path in temporary runtime manifest. This supports OpenXR loaders that do not resolve manifest-relative library paths.
2. Sets `XR_RUNTIME_JSON` only for child process.
3. Removes inherited explicit XR API-layer selection from child.
4. Disables Virtual Desktop Oculus compatibility implicit layer for child.
5. Restores caller environment after process starts.

No registry writes or machine-wide runtime changes occur.

AgentXR creates its compositor preview window when an XR session begins. Explicit session end disposes it immediately. One second without newly released swapchain content disposes the window; resubmitting stale layers does not keep a preview alive.

Editor integrations that stop presentation without continuing to poll OpenXR events can resolve `agentxrRequestExitActiveSession` from already-loaded runtime DLL. Calling this optional C ABI export stops presentation immediately and returns active session to restartable `XR_SESSION_STATE_READY`; standard `xrRequestExitSession` keeps normal `XR_SESSION_STATE_STOPPING`/`xrEndSession` semantics.

## Configure MCP

Point MCP client at installed executable:

```json
{
  "mcpServers": {
    "agent-xr": {
      "type": "stdio",
      "command": "C:/path/to/agent-xr/dist/agent-xr-mcp.exe",
      "args": [],
      "cwd": "C:/path/to/workspace"
    }
  }
}
```

Server exposes one `xr` tool with actions:

| Action | Required fields | Purpose |
|---|---|---|
| `list_processes` | none | Discover AgentXR runtime endpoints. |
| `launch_editor` | `shortcutPath` | Launch shortcut target with AgentXR. Optional `expectedProjectPath`, `runtimeManifestPath`. |
| `snapshot` | `processId`, `instanceId`, `sessionGeneration` | Read coherent runtime/session/frame/action state. |
| `submit_timeline` | `processId`, `instanceId`, `sessionGeneration`, `timeline` | Validate and start complete timeline. |
| `get_report` | `processId`, `instanceId`, `sessionGeneration`, `timelineId` | Read bounded report pages. Optional `cursor`, `limit`. |
| `cancel_timeline` | `processId`, `instanceId`, `sessionGeneration`, `timelineId` | Cancel named active timeline. |
| `capture` | `processId`, `instanceId`, `sessionGeneration` | Capture fresh composited stereo PNG. Optional `afterFrameId`. |

`list_processes` returns `sessionGeneration`, `sessionRunning`, and `sessionState` for each runtime. With no running session, `sessionGeneration` is `0` and `sessionRunning` is `false`; `sessionState` remains present. Session-bound actions require a nonzero active generation. Every successful `xrBeginSession` allocates a new generation, including restarts on the same session handle. Bind calls to the exact `processId`, `instanceId`, and active generation. Refresh discovery and snapshot after a restart; stale generations are rejected.

Submitted timelines continue after an MCP disconnect, which releases the mutation lease but does not cancel the run. Only `get_report` retries a transport disconnect, once, with the unchanged request and only after confirming the same bound PID, process creation time, instance ID, and active session generation. Mutating actions and structured runtime errors are not retried. Session/runtime teardown still neutralizes inputs.

`launch_editor` materializes temporary manifest with absolute runtime DLL path. Prefer it over setting environment manually when MCP owns launch.

## Timeline format

Timeline uses right-handed OpenXR floor coordinates:

- meters
- `+Y` up
- `-Z` forward
- quaternion `[x, y, z, w]`

Root fields:

```json
{
  "schemaVersion": 1,
  "durationSeconds": 2.0,
  "sampleRateHz": 90,
  "interpolation": "linear",
  "initial": {},
  "keyframes": []
}
```

`initial` must completely define head, left controller, and right controller. Keyframes use strictly increasing `timeSeconds` and partial state patches. Omitted fields retain prior values.

Supported interpolation modes:

- `step`
- `linear`
- `ease_in`
- `ease_out`
- `ease_in_out`

Pose validity and tracking default to `true`. Tracked state requires corresponding valid state. Digital input and tracking changes occur discretely at keyframe time.

Controller inputs follow core Oculus Touch profile:

- Left buttons: `x`, `y`, `menu`, `thumbstick`
- Right buttons: `a`, `b`, `thumbstick`
- Touches: face buttons, `thumbstick`, `trigger`
- Analog: `trigger`, `squeeze`, `thumbstick`
- Poses: `grip`, `aim`

See `examples/neutral.json`, `examples/motion-and-input.json`, and `examples/tracking-recovery.json`.

## Native smoke checks

```powershell
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario lifecycle
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario stereo-composition
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario timeline-actions
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario tracking-recovery
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario invalid-input
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario session-restart
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario pipelined-swapchain
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario pipelined-frame
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario unreal-action-setup
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario report-lifetime
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario capture-wait
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario idle-client-shutdown
```

Smoke runner deliberately probes unsupported OpenXR 1.1 instance creation before falling back to 1.0; loader may print expected failed-create diagnostics while scenario still exits `0`.

The smoke scenarios check:

- `stereo-composition` decodes the captured PNG with WIC, checks red left-eye and blue right-eye pixels, and verifies that a capture taken while a newer frame is waited but unended still matches the last completed frame's pixels and metadata.
- `session-restart` checks inactive discovery (`sessionGeneration: 0`, `sessionRunning: false`, and `sessionState` present) and confirms a prior-generation binding is rejected after restarting the same session handle.
- `report-lifetime` inserts a 60 ms stall during the run, then checks exact report equality after another 60 ms pause and ten live frames. Snapshot IDs and fresh captures advance; the later capture has `timelineId: 0`.
- `capture-wait` starts capture after a known frame and confirms it waits for a fresh completed frame's finalized record, then decodes the expected red/blue eye pixels without an early timeout or stale-frame fallback.
- `idle-client-shutdown` leaves a Control connection idle during session and instance teardown. Server-side control reads observe shutdown cancellation, so `xrDestroyInstance` completes without requiring the client to disconnect.
- Nine fault-injected cases exercise the installed MCP reconnect path's single `get_report` retry and identity guards.

## Evidence model

Runtime completion does not prove application behavior. Pair AgentXR snapshot/report with application state and logs, then capture rendered output after a known frame ID.

`capture` waits for a completed frame newer than `afterFrameId` and the finalized record for that exact frame under one timeout deadline. If either remains pending through that deadline, it returns a timeout; it never substitutes an older frame. The PNG and metadata come from the same completed frame: `frameId`, `sessionGeneration`, `timelineId`, `displayTime`, `layerCount`, `ended`, `composed`, `composeResult`, `presentAttempted`, `presented`, `presentResult`, `presentOccluded`, and `presentStillDrawing`. Metadata also includes `captureTimestamp`, `width`, `height`, and `binaryLength`.

`composeResult` is the OpenXR composition status. `presentResult` records the actual signed DXGI HRESULT and is meaningful only when `presentAttempted` is `true`. `composed` marks successful compositor output; `presented` is true only when DXGI `Present` returns `S_OK`. PNG encoding uses the pixel format returned by WIC; AgentXR converts RGBA readback when WIC selects another format.

Each `RunReport` owns bounded per-run `frames`, `actionSyncs`, and `haptics`. The first frame at or beyond the authored end marks the report terminal; the final action observation settles its run records. Later idle frames cannot grow the report, though matching frames already in flight may finish and update the current or retained report. Snapshot frame IDs and capture IDs keep advancing; post-run frames use `timelineId: 0`.

`missedFrameCount` sums periods only for frames assigned to the run, saturates rather than wrapping, and sets `overflow` if the total exceeds its representable range.

Treat runtime error, stale capture, missed required input edge, or graphics-device failure as failed XR run even when application state appears correct.

## Licenses

Dependency licenses and revisions are under `third_party`.
