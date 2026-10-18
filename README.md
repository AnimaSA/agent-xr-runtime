# AgentXR

AgentXR is a Windows OpenXR runtime built for deterministic, agent-driven XR testing. It supplies tracked head/controller poses, Oculus Touch-compatible actions, D3D12 stereo composition, frame evidence, image capture, and timeline control through a local MCP server.

AgentXR does not replace the machine-wide OpenXR runtime. Launch helpers set `XR_RUNTIME_JSON` only for the target process and restore caller environment.

## Current scope

- Windows x64
- OpenXR 1.0 and 1.1 runtime interface
- Extensions: `XR_KHR_D3D12_enable`, `XR_EXT_local_floor`, `XR_KHR_composition_layer_depth`, `XR_EXT_palm_pose`, `XR_EXT_active_action_set_priority`, `XR_EXT_hand_tracking`, `XR_EXT_eye_gaze_interaction`, `XR_KHR_locate_spaces`, `XR_KHR_maintenance1`
- Primary stereo view configuration
- `LOCAL`, `STAGE`, `VIEW`, and `LOCAL_FLOOR_EXT` spaces
- Binding validation for nine OpenXR 1.0 core interaction profiles; input emulation for `/interaction_profiles/oculus/touch_controller`
- Boolean, float, vector2, pose, and haptic actions
- Deterministic schema-version-1 timelines
- Local named-pipe control, MCP orchestration, frame reports, and PNG capture

`XR_EXT_hand_tracking` is exposed with hand joints always inactive (controllers are held). `XR_EXT_eye_gaze_interaction` is exposed with `supportsEyeGazeInteraction` false.

Not implemented: headset display hardware, passthrough, audio, Vulkan, OpenGL, or non-Windows platforms.

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
- `dist/agent-xr-calibrate.exe` — device calibration tool
- `dist/profiles/oculus-touch-controller.json` — measured device profile; installed only when `profiles/oculus-touch-controller.json` exists in the source tree (see [Device calibration](#device-calibration))
- `dist/test-fixtures/oculus-touch-controller.json` — non-identity test profile used by `agent-xr-smoke` only

Manifest uses `runtime.library_path: "./AgentXRRuntime.dll"` so vendored loader resolves DLL beside manifest. Bare DLL names use DLL search order. Use absolute `library_path` when comparing separate runtime builds; launch helpers already generate absolute paths.

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

## Action contracts

`xrSuggestInteractionProfileBindings` validates profile, component, and hand. Uninterned path handles return `XR_ERROR_PATH_INVALID`; unsupported profile/component paths return `XR_ERROR_PATH_UNSUPPORTED`. Failed suggestions leave previous bindings intact. Valid componentless paths resolve to compatible components; valid but incompatible bindings are ignored.

ponytail: Non-Touch core profiles validate but have no emulated input, add emulation when a test requires another controller profile.

Current interaction profile remains `XR_NULL_PATH` without attached Touch bindings for that user path. Once an action set has been attached, later binding suggestions for it return `XR_ERROR_ACTIONSETS_ALREADY_ATTACHED`, even after session destruction.

Inactive Boolean, float, and vector2 actions return neutral values, `isActive: false`, `changedSinceLastSync: false`, and `lastChangeTime: 0`. Activity changes alone do not create input edges. Boolean aggregates compare combined hand state, so one hand releasing while another holds does not create a false release.

Unfocused `xrSyncActions` returns positive `XR_SESSION_NOT_FOCUSED`. Getters mask input while unfocused even without a sync; haptic requests produce no feedback record. Focus loss preserves previous synchronized snapshots. Recovery after an actual unfocused sync has inactive history; recovery without that sync compares against previous active values and can report a genuine falling edge. Unchanged values retain previous timestamps. See [OpenXR action state](https://registry.khronos.org/OpenXR/specs/1.0-khr/html/xrspec.html#_action_state).

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

Pose validity and tracking default to `true`. Tracked state requires corresponding valid state. Poses follow the interpolation mode between keyframes. Digital input, analog input (trigger, squeeze, thumbstick), activity, and tracking changes occur discretely at keyframe time; author intermediate keyframes for an analog ramp.

`initial.focused` and `keyframes[].state.focused` accept optional Boolean values, defaulting to `true`. Focus changes are discrete under every interpolation mode. `false` changes session from `FOCUSED` to `VISIBLE` during `xrWaitFrame`; frames and composition continue. `true` restores `FOCUSED` without changing session generation. No `xrSyncActions` call is needed to apply focus transitions. Cancellation or replacement resets focus and neutralizes input before the next timeline applies.

`left.active` and `right.active` default to `true`. Setting either to `false` makes that hand's input actions inactive even when authored values are nonzero. Pose validity/tracking flags are separate; tracking loss does not deactivate buttons.

Controller inputs follow core Oculus Touch profile:

- Left buttons: `x`, `y`, `menu`, `thumbstick`
- Right buttons: `a`, `b`, `thumbstick`
- Touches: face buttons, `thumbstick`, `trigger`, `thumbrest`
- Analog: `trigger`, `squeeze`, `thumbstick`
- Poses: `grip` only. Aim and palm poses are derived from grip with the device profile (`/user/hand/{left,right}/input/aim/pose`, `palm_ext/pose`, `grip_surface/pose`); the timeline has no `aim` key.

Pose model:

- Pose at any `XrTime` is the exact evaluation of the authored curve at that time. There is no sample-rate quantization and no `sampleRateHz` field.
- `xrLocateSpace` reports real linear and angular velocity of the located space relative to the base space (central difference of the exact evaluator over ±0.5 ms). Velocity validity flags are set only when velocity is computed.
- Oculus Touch cannot press, pull, or deflect a control without touching it. Effective touch is authored touch OR implied touch: trigger value above zero implies trigger touch; an `a`, `b`, `x`, or `y` click implies that button's touch; a thumbstick click or nonzero deflection implies thumbstick touch.

See `examples/neutral.json`, `examples/motion-and-input.json`, `examples/tracking-recovery.json`, and `examples/focus-recovery.json`.

## Device calibration

On a real runtime, aim and palm poses are rigid, device-specific offsets of the grip pose. AgentXR derives both from grip using a measured profile and refuses to create an instance without it.

`agent-xr-calibrate` measures those offsets for physical Oculus Touch controllers: per hand it locates aim and palm (`grip_surface` on OpenXR 1.1, `palm_ext` with `XR_EXT_palm_pose` on 1.0) in grip space, collects samples while both controllers are fully tracked, rejects the run if any sample deviates from the mean by more than 1 mm or 0.5°, and writes the averaged offsets.

```powershell
./dist/agent-xr-calibrate.exe --out ./profiles/oculus-touch-controller.json [--samples 300]
```

Run it once, with the physical headset worn and the physical runtime (SteamVR, Meta, Virtual Desktop, ...) active; hold both controllers naturally and keep them tracked. It refuses to run against AgentXR, requires `/interaction_profiles/oculus/touch_controller` on both hands, and writes no file on failure. Exit codes: `0` success, `1` measurement or runtime failure, `2` usage error.

Runtime loads the profile in `xrCreateInstance` from the file named by the `AGENTXR_DEVICE_PROFILE` environment variable when it is set and non-empty, otherwise from `<runtime DLL dir>/profiles/oculus-touch-controller.json`. A missing or invalid file fails `xrCreateInstance` with `XR_ERROR_RUNTIME_FAILURE`; stderr and the debugger output name the resolved absolute path, say whether it came from `AGENTXR_DEVICE_PROFILE`, and give the calibration command. There are no fallback values.

`dist/profiles/` is populated only from calibration: save the calibration output to `profiles/oculus-touch-controller.json` in the source tree and re-run `cmake --install`. The installed `dist/test-fixtures/` profile is not a device profile; `agent-xr-smoke` sets `AGENTXR_DEVICE_PROFILE` to it (process-local) unless the variable is already set or `--device-profile <path>` is given.

## Native smoke checks

```powershell
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario focus-recovery
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario binding-contract
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario inactive-controller
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario lifecycle
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario stereo-composition
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario timeline-actions
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario tracking-recovery
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario pose-model
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario invalid-input
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario session-restart
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario pipelined-swapchain
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario pipelined-frame
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario unreal-action-setup
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario report-lifetime
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario capture-wait
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario idle-client-shutdown
./dist/agent-xr-smoke.exe --runtime ./dist/agent-xr.json --scenario openxr-11
```

Smoke runner prints actual loaded runtime DLL and rejects a module that differs from manifest selection. Use absolute executable and manifest paths when comparing builds.

The smoke scenarios check:

- `focus-recovery` checks VISIBLE rendering, unfocused input/haptic suppression, event-driven recovery with and without action sync, exact edge/timestamp semantics, cancellation, and replacement.
- `binding-contract` checks strict paths, atomic suggestions, componentless conversion, ignored incompatible bindings, current profiles, ever-attached errors, and combined-hand Boolean edges.
- `inactive-controller` checks neutral inactive values, recovery, and native pose validity/tracking independent from input activity.
- `pose-model` checks that pose at an off-frame time is the exact timeline evaluation, that grip velocity is nonzero and matches the authored motion, that the aim pose equals grip composed with the device profile's `aimFromGrip`, that a trigger value without authored touch reports touch, and that `aim` and `sampleRateHz` timeline keys are rejected.
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

`focusTransitions`, `validityTransitions`, and `trackingTransitions` count applied frame-state changes, not authored keyframes that never reach a frame. Validity and tracking counters compare position/orientation flags across head and both hands' grip poses. Digital edges occurring while unfocused or on an inactive controller remain unobserved rather than falsely delivered.

Treat runtime error, stale capture, missed required input edge, or graphics-device failure as failed XR run even when application state appears correct.

Native smoke proves AgentXR runtime contracts, not physical Quest tracking, OS focus/API-layer behavior, or Unreal input and gameplay integration. Haptics are feedback records, not hardware vibration. Pair native evidence with application-specific checks before attributing a hardware failure to gameplay.

## Licenses

Dependency licenses and revisions are under `third_party`.
