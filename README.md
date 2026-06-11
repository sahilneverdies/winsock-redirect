# Winsock Redirect — Garena Free Fire Bypass DLL

Interactive console-menu DLL that hooks `connect()` and `send()` from `ws2_32.dll` to redirect Game Client traffic through an external TCP bypass proxy.

```
┌──────────────────────────────────────────────────────────────────────┐
│                        GAME PROCESS                                  │
│                                                                      │
│  connect("loginbp.ggblueshark.com:443")                              │
│         │                                                            │
│         ▼                                                            │
│  ┌──────────────────────────────────────────────────────────────┐    │
│  │              DetourConnect (hooked)                          │    │
│  │  • Checks if target port ≥ 6000 (game traffic)               │    │
│  │  • Resolves PROXY_IP (e.g. "192.168.1.50")                   │    │
│  │  • Calls real connect() → PROXY_IP:19112                     │    │
│  │  • Sends 6-byte preamble: original IP (4B) + port (2B)       │    │
│  │  • Preamble obfuscated: rotl8(xor 0xA3, 3) per byte          │    │
│  └──────────────────────┬───────────────────────────────────────┘    │
│                         │                                            │
│  ┌──────────────────────▼───────────────────────────────────────┐    │
│  │              DetourSend (hooked)                             │    │
│  │  • Per-mode packet patching before forwarding                │    │
│  │  • Mode selected at injection via console menu               │    │
│  │  • Applies obfuscated byte transforms (not XOR 0xFF)         │    │
│  │  • Forwards modified buffer to real send()                   │    │
│  └──────────────────────────────────────────────────────────────┘    │
│                                                                      │
└──────────────────────────┬───────────────────────────────────────────┘
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

## 7 Bypass Methods (Selectable at Runtime)

When injected, the DLL shows an interactive menu:

```
  ========================================
   XENITRONIX ELITE BYPASS v12.1
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

   Select method (1-7):
```

| Method | `PatchCsrFields` | `PatchCustomRoomA` | `PatchCustomRoomB` | HWID Mask | Memory Patch | Pool Bypass |
|--------|:-:|:-:|:-:|:-:|:-:|:-:|
| 1 — CSR Remove | ✔ | ✘ | ✘ | ✔ | ✘ | ✘ |
| 2 — Room A | ✘ | ✔ | ✘ | ✔ | ✘ | ✘ |
| 3 — Room B | ✘ | ✘ | ✔ | ✔ | ✘ | ✘ |
| 4 — Ranked Pool | ✘ | ✘ | ✘ | ✔ | ✘ | ✔ |
| 5 — Memory Patch | ✘ | ✘ | ✘ | ✔ | ✔ | ✘ |
| 6 — Pass-Through | ✘ | ✘ | ✘ | ✘ | ✘ | ✘ |
| 7 — Aggressive | ✔ | ✔ | ✔ | ✔ | ✔ | ✔ |

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

### HWID Masking
Fields `0x75`, `0x76`, `0x77` (HWID bytes) are obfuscated with:
```c
data[i + j] = rotr8(data[i + j], 4) ^ 0x5A;
```
This replaces the detectable `XOR 0xFF` pattern.

### Ranked Pool Bypass
Searches for tag `0xB0 0x01 0x01` (pool indicator) and sets the value to `0x00`.

## Obfuscation & Anti-Detection

- **No `HideFromDebugger`**: The `NtSetInformationThread` call was removed because anti-cheats specifically scan for it
- **Rotating XOR**: Preamble encoded with `rotl8(byte ^ 0xA3, 3)` per byte, key chained to previous output
- **Scope no-ops**: `volatile` integer XOR loops inserted between hook calls to break pattern analysis
- **Stealth sleep**: 1-7ms random `Sleep()` before connect hook to desync timing analysis
- **No `XOR 0xFF`**: All byte transforms use rotation + non-trivial XOR keys to avoid signature detection

## Configuration

In `BypassDll.h`:

```c
#define PROXY_IP "xenitronix"    // Change to your server IPv4
#define PROXY_PORT 19112         // TCP controller port
```

## Compilation

### Requirements
- **Visual Studio 2019/2022** with "Desktop development with C++"
- **MinHook** included as submodule in `minhook/`

### Steps
1. Open `BypassDll.sln` in Visual Studio
2. Select **Release** + **x64** (or **x86** for 32-bit emulators)
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
2. Select method (1-7) via keyboard
3. MinHook initializes, hooks activate
4. All subsequent game connections redirect through the proxy

The Python TCP controller (port 19112) must be running before game launch.
