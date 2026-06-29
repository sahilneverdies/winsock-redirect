# Winsock Redirect — Garena Free Fire Bypass DLL

Interactive console-menu DLL that hooks `connect()`, `send()`, `recv()`, and `WSARecv()` from `ws2_32.dll` to redirect Game Client traffic through an external TCP bypass proxy, with per-mode packet patching, protobuf field logging, and ADB-based emulator property bypass.

```
┌──────────────────────────────────────────────────────────────────────────┐
│                          GAME PROCESS                                    │
│                                                                          │
│  connect("loginbp.ggblueshark.com:443")                                  │
│         │                                                                │
│         ▼                                                                │
│  ┌──────────────────────────────────────────────────────────────────┐    │
│  │              DetourConnect (hooked)                               │    │
│  │  • Checks if target port ≥ 6000 (game traffic)                   │    │
│  │  • Resolves PROXY_IP (e.g. "192.168.1.50")                       │    │
│  │  • Calls real connect() → PROXY_IP:19112                         │    │
│  │  • Sends 6-byte preamble: original IP (4B) + port (2B)           │    │
│  │  • Preamble obfuscated: rotl8(xor 0xA3, 3) per byte              │    │
│  └──────────────────────┬───────────────────────────────────────────┘    │
│                         │                                                │
│  ┌──────────────────────▼───────────────────────────────────────────┐    │
│  │              DetourSend (hooked)                                  │    │
│  │  • Per-mode packet patching before forwarding                     │    │
│  │  • Mode selected at injection via console menu                    │    │
│  │  • Applies obfuscated byte transforms (not XOR 0xFF)             │    │
│  │  • Forwards modified buffer to real send()                       │    │
│  │  • Protobuf field scanner logs new field tags to file/console     │    │
│  └──────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  ┌──────────────────────▼───────────────────────────────────────────┐    │
│  │              DetourRecv / DetourWSARecv (hooked)                  │    │
│  │  • Intercepts matchmaking response protobufs                     │    │
│  │  • Patches workshop_code, region, gs_proxy_secret fields         │    │
│  │  • Redirects IP:port strings in proto to PROXY_IP:PROXY_PORT     │    │
│  │  • Protobuf field scanner logs new field tags                    │    │
│  └──────────────────────────────────────────────────────────────────┘    │
│                                                                          │
└──────────────────────────┬───────────────────────────────────────────────┘
                           │ raw TCP (redirected)
                           ▼
              ┌─────────────────────────┐
              │  TCP Controller         │
              │  (port 19112)           │
              │  — decodes preamble     │
              │  — forwards to real     │
              │    target IP:port       │
              │  — traffic shaping      │
              └──────────┬──────────────┘
                         │
                         ▼
              ┌─────────────────────────┐
              │  mitmproxy (port 2020)  │
              │  — protobuf mutation    │
              │  — UID auth check       │
              └─────────────────────────┘
```

## Bypass Methods (Selectable at Runtime)

When injected, the DLL shows an interactive menu:

```
  ========================================
   XENITRONIX ELITE BYPASS v12.2
  ========================================

   [1] CSR Remove Only
       — Strips ranked/CSR flags from outbound packets
   [2] Custom Room Spoof (Method A)
       — Forces room type to custom via field A
   [3] Custom Room Spoof (Method B)
       — Forces room type to custom via field B
   [4] Ranked Pool Patch (original)
       — Standard pool bypass
   [5] Memory Patch Mode
       — Patches game functions in memory
   [6] Packet Pass-Through
       — DLL only relays; mitmproxy does all mutation
   [7] Aggressive (all methods combined)
   [8] Auto Bypass libanogs (ADB + resetprop)
       — Deletes 52 emulator properties via Magisk resetprop
       — Optionally injects libhooks.so hook if compiled
       — Requires adb.exe (BlueStacks) + rooted emulator
   [9] Packet Capture Mode
       — Dumps ALL send/recv payloads to 'dll_captured/' folder
       — PC logo bypass still active
       — No other modifications (pass-through to mitmproxy)
       — For blacklist trigger analysis in ranked matches

   Select method (1-9):
```

| Method | `PatchCsrFields` | `PatchCustomRoomA` | `PatchCustomRoomB` | HWID Mask | Memory Patch | Pool Bypass | ADB Inject | Packet Dump |
|--------|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| 1 — CSR Remove | ✔ | ✘ | ✘ | ✔ | ✘ | ✘ | ✘ | ✘ |
| 2 — Room A | ✘ | ✔ | ✘ | ✔ | ✘ | ✘ | ✘ | ✘ |
| 3 — Room B | ✘ | ✘ | ✔ | ✔ | ✘ | ✘ | ✘ | ✘ |
| 4 — Ranked Pool | ✘ | ✘ | ✘ | ✔ | ✘ | ✔ | ✘ | ✘ |
| 5 — Memory Patch | ✘ | ✘ | ✘ | ✔ | ✔ | ✘ | ✘ | ✘ |
| 6 — Pass-Through | ✘ | ✘ | ✘ | ✘ | ✘ | ✘ | ✘ | ✘ |
| 7 — Aggressive | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✘ | ✘ |
| 8 — ADB Inject | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ | ✘ |
| 9 — Capture | ✘ | ✘ | ✘ | ✘ | ✘ | ✘ | ✘ | ✔ |

## Packet Patching Details

### CSR/Flag Stripping (`PatchCsrFields`)
Scans raw bytes for protobuf varint tags (field 25-40 range) and zeros out ranked/division/tier values:
- Fields `0x28`, `0x30`, `0x38`, `0x40`, `0x48` with boolean values → set to 0
- Fields `0xC8`–`0xFF` (varint encoded) → zero the value byte

### Custom Room Spoof A (`PatchCustomRoomA`)
- Field `0xD2 0x01` (field 26): if value is 1 (ranked), change to 2 (custom)
- Field `0xD8 0x01` (field 27): same pattern
- Field `0xE0 0x01` (field 28): if value is 4 (ranked clash), change to 3 (casual)

### Custom Room Spoof B (`PatchCustomRoomB`)
- Field `0x7A` (field 15 — room_type): if value 1, set to 2
- Field `0x92 0x01` (field 18 — match_mode): if value 1, set to 0
- Field `0x08 0x01`: boolean flags zeroed

### PC Logo / Emulator Detection Strip (`PatchPcDetection`)
- Field `0xB8` (field 23 — platform_type): force value 3 (Android Phone)
- Fields `0x90 0x03`, `0x98 0x03`, `0x80 0x06`, `0x88 0x06`, `0x90 0x06`, `0x98 0x06`: zero the value

### Matchmaking Response Patching (`PatchRecvBuffer`)
- Strips ranked/CSR varint flags in incoming data (fields 1-3, 90+)
- Patches workshop_code string fields with custom markers
- Redirects `gs_proxy_secret` IP:port strings to `PROXY_IP:PROXY_PORT`

### HWID Masking
Fields `0x75`, `0x76`, `0x77` (HWID bytes) are obfuscated with rotating XOR + bit rotation:
```c
data[i + j] = rotr8(data[i + j], 3) ^ key;  // key chains per byte
```
This replaces the detectable `XOR 0xFF` pattern with per-field unique scrambling.

### Ranked Pool Bypass
Searches for tag `0xB0 0x01 0x01` (pool indicator) and sets the value to `0x00`.

## New Features

### Option [8] — ADB Auto-Inject

Automates emulator property bypass via ADB:

1. **Step 1**: Pushes `reset_emu_props.sh` (or inline commands) to the emulator via ADB
2. **Step 2**: Runs Magisk `resetprop -d` to delete all 52 `ro.*` emulator properties
3. **Step 3**: If compiled `libhooks.so` + `hooker` exist alongside DLL, pushes and injects them for deeper `__system_property_get` hooking

Requires `adb.exe` (auto-detected from BlueStacks installation paths) and a rooted emulator.

### Option [9] — Packet Capture Mode

Full bidirectional TCP-level packet dump mode for blacklist trigger analysis. Unlike mitmproxy's `--capture login` (which only sees HTTP proxy traffic), the DLL hooks **all** Winsock `send`/`recv` calls at the TCP level — capturing every packet even on non-HTTP connections.

**Why this matters:** The in-game blacklist (triggered when queueing ranked matches) likely arrives on a separate TCP connection to a matchmaking server, which never goes through the HTTP proxy. Only the DLL sees this traffic.

#### How it works

1. Select **option [9]** at injection
2. DLL resolves its own directory and creates `dll_captured/` there (works even if injected into `hd-player.exe` running from Program Files)
3. For every `send()` and `recv()` on redirected sockets, the raw buffer is saved **before any modification** as a timestamped `.dat` file
4. PC logo bypass (`PatchPcDetection`) still runs — emulator detection flags are stripped
5. All other bypass modifications are disabled — pure pass-through to mitmproxy

#### Output files

Saved to `dll_captured/` (next to DLL):

```
20260627_231110_123_SEND_original.dat       # Game → Server request
20260627_231110_456_RECV_original.dat       # Server → Game response
```

**File format:**
```
# DLL Packet Capture
# Direction: RECV
# Label: original
# Time: 2026-06-27 23:11:10.123
# Length: 1428
# Hex:
<lowercase hex dump, 64 bytes per line>
```

#### Analyzing captures for blacklist trigger

1. Run **normal mode** (`python main.py`) for full bypass stack + DLL [9] for capture
2. Queue into a **ranked match** (CS Ranked or BR Ranked)
3. When blacklist hits, the last few `RECV` files before the kick contain the ban signal
4. Compare protobuf fields between normal and ranked queue captures to identify the trigger message

#### Differences from `--capture login`

| Aspect | `--capture login` (mitmproxy) | Option [9] (DLL) |
|--------|------------------------------|-------------------|
| Scope | HTTP proxy traffic only | All TCP sockets (send/recv) |
| See non-HTTP connections? | No | Yes |
| Decryption | Full (AES decrypt via Python) | Raw encrypted bytes only |
| Traffic modification | None (pass-through) | PatchPcDetection still runs |
| Timing | After TLS termination | Raw TCP layer |

### Protobuf Field Logging (`LogProtobufFields`)

Every SEND and RECV packet is scanned for unique protobuf field tags. When a new (previously unseen) field tag is discovered, it's logged to both:
- Console output
- `proto_fields.log` (in DLL directory)

This helps identify which protobuf fields carry emulator detection data, blacklist triggers, or other game server signals. The log tracks cumulative unique fields across the session.

```
[12:34:56.789] [SEND] New fields: 23, 99, 100 (total 42 unique)
[12:34:57.012] [RECV] New fields: 1, 2, 3 (total 45 unique)
```

### ARM64 Hook Library (`dll_src/libhooks/`)

When compiled with Android NDK, provides a `libhooks.so` that intercepts `__system_property_get` at the libc level:

| File | Purpose |
|------|---------|
| `libhooks.c` | Hook library — intercepts 52+ `ro.*` emulator properties, returns "not found" |
| `hooker.c` | Ptrace injector — loads libhooks.so into game process, constructor patches GOT |
| `Android.mk` | NDK build rules |
| `build.bat` | Windows build script |

To compile:
```bash
cd dll_src/libhooks/
build.bat C:\path\to\android-ndk-r26b
```

Push to emulator → place alongside DLL → option [8] auto-injects.

## Obfuscation & Anti-Detection

- **No `HideFromDebugger`**: The `NtSetInformationThread` call was removed because anti-cheats specifically scan for it
- **Rotating XOR**: Preamble encoded with `rotl8(byte ^ 0xA3, 3)` per byte, key chained to previous output
- **Scope no-ops**: `volatile` integer XOR loops inserted between hook calls to break pattern analysis
- **Stealth sleep**: 1-7ms random `Sleep()` before connect hook to desync timing analysis
- **No `XOR 0xFF`**: All byte transforms use rotation + non-trivial XOR keys to avoid signature detection
- **Memory fence**: Volatile read/write sequences between hook calls to prevent optimization-based detection
- **Deep sleep**: 1-25ms gaussian delay after connect to randomize timing further

## Configuration

In `BypassDll.h`:

```c
#define PROXY_IP "192.168.1.9"    // Change to your server IPv4
#define PROXY_PORT 19112           // TCP controller port
```

## Compilation

### Requirements
- **Visual Studio 2019/2022** with "Desktop development with C++"
- **MinHook** included as submodule in `minhook/`

### Steps
1. Open `BypassDll.sln` in Visual Studio
2. Select **Release** + **x64**
3. Build → `BypassDll.dll` is output to `x64/Release/`

### Manual Project Setup
1. Create new **Dynamic-Link Library (DLL)** project
2. Add `BypassDll.cpp` and `BypassDll.h`
3. Set **Additional Include Directories** → `minhook/include`
4. Add `minhook/src/*.c` to project sources
5. Link: `ws2_32.lib`

## Injection

Use any DLL injector targeting the emulator process (e.g. `HD-Player.exe` for BlueStacks).

On injection:
1. Console window appears with the interactive menu
2. Select method (1-8) via keyboard
3. If option [8]: ADB push + resetprop runs, then optionally injects hook library
4. MinHook initializes, hooks activate
5. All subsequent game connections redirect through the proxy

The Python TCP controller (port 19112) must be running before game launch.
