# IPC Protocol

The frontend is the server; every Vulkan process that loads the layer connects as a client. The frontend listens on two sockets simultaneously:

- **Filesystem path**: `$XDG_RUNTIME_DIR/game-filters-flatpak.sock`
- **Linux abstract namespace**: `@game-filters-flatpak.sock`

Both use the same name — a protocol invariant referenced as `gff::kSocketName` (`layer/src/ipc.hpp`) on the C++ side and `SOCKET_NAME` (`frontend/src/ipc.rs`) on the Rust side. The abstract socket exists so Steam pressure-vessel / bubblewrap-sandboxed processes — which cannot see the host's `$XDG_RUNTIME_DIR` — can still reach the frontend. The layer tries both; either succeeds is enough.

## Wire format

Length-prefixed JSON. Each message is:

```
| 4 bytes: payload length (little-endian uint32) | payload: UTF-8 JSON |
```

Maximum payload size: 64 KiB (reject anything larger).

## Messages

### Layer → Frontend

| `type` | Payload | Meaning |
|---|---|---|
| `game-started` | `{ "pid": int, "exe": string }` | A Vulkan application opened a `VkInstance`. |
| `game-stopped` | `{ "pid": int }` | The tracked Vulkan application exited. |
| `overlay-shown` | `{}` | The in-game ImGui overlay is now visible. |
| `overlay-hidden` | `{}` | The in-game ImGui overlay is now hidden. |

### Frontend → Layer

| `type` | Payload | Meaning |
|---|---|---|
| `show-overlay` | `{}` | Show the in-game ImGui overlay. |
| `hide-overlay` | `{}` | Hide the in-game ImGui overlay. |
| `toggle-overlay` | `{}` | Toggle overlay visibility. |
| `load-profile` | `{ "path": string }` | Hot-load a profile file from disk. |
| `param-updated` | `{ "key": string, "value": number\|bool\|[number, ...] }` | Override a single parameter live (used when the frontend mirror window drives the slider). |

All messages include a top-level `"type"` field and an optional `"seq"` integer for request/response correlation.
