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

AgentXR creates its compositor preview window when an XR session begins. Explicit session end disposes it immediately. One second without newly released swapchain content disposes the window and transitions a focused session to `XR_SESSION_STATE_STOPPING`; resubmitting stale layers does not keep a preview alive. After application ends that session, it can begin again from `XR_SESSION_STATE_READY`.

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
| `snapshot` | `processId`, `instanceId` | Read coherent runtime/session/frame/action state. |
| `submit_timeline` | `processId`, `instanceId`, `sessionGeneration`, `timeline` | Validate and start complete timeline. |
| `get_report` | `processId`, `instanceId`, `sessionGeneration` | Read bounded report pages. Optional `timelineId`, `cursor`, `limit`. |
| `cancel_timeline` | `processId`, `instanceId`, `sessionGeneration`, `timelineId` | Cancel named active timeline. |
| `capture` | `processId`, `instanceId`, `sessionGeneration` | Capture fresh composited stereo PNG. Optional `afterFrameId`. |

Always call `list_processes`, then `snapshot`. Carry exact `processId`, `instanceId`, and nonzero `sessionGeneration` into mutating calls. Refresh snapshot after application restarts XR session; stale generations are rejected.

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
```

Smoke runner deliberately probes unsupported OpenXR 1.1 instance creation before falling back to 1.0; loader may print expected failed-create diagnostics while scenario still exits `0`.

## Evidence model

Runtime completion does not prove application behavior. Use all relevant surfaces:

- AgentXR snapshot/report for timing, tracking, action sync, haptics, frame IDs, and API/GPU errors
- application state/logs for behavior
- fresh `capture` after known frame ID for rendered output

Treat runtime error, stale capture, missed required input edge, or graphics-device failure as failed XR run even when application state appears correct.

## Licenses

Dependency licenses and revisions are under `third_party`.
