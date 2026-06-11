#include "BypassDll.h"
#include "MinHook.h"
#include <vector>
#include <winternl.h>
#include <fstream>
#include <ctime>
#include <random>
#include <conio.h>

/*
    Xenitronix - ELITE BYPASS v12 (Interactive)
    - Console menu on injection to choose bypass method
    - Each method uses different field modification patterns
    - Useful for testing which approach works with current anti-cheat
*/

#define DLL_VERSION "12.1"


// =====================================================================
//  METHOD DEFINITIONS
// =====================================================================
// Stored in a global so hooks can check which mode is active.
// The Python proxy also reads BYPASS_MODE from a side channel.

enum BYPASS_MODE {
    MODE_CSR_REMOVE       = 1,  // Strip CSR / ranked flags from packets
    MODE_CUSTOM_ROOM_A    = 2,  // Spoof room type field A
    MODE_CUSTOM_ROOM_B    = 3,  // Spoof room type field B (alternative offset)
    MODE_RANKED_POOL      = 4,  // Original ranked pool patch only
    MODE_MEMORY_PATCH     = 5,  // In-memory function patching (requires more flags)
    MODE_PACKET_PASS      = 6,  // Let mitmproxy handle everything — DLL relay only
    MODE_AGGRESSIVE       = 7,  // All methods combined
};

volatile int g_bypass_mode = MODE_RANKED_POOL;  // default

// =====================================================================
//  HOOK FUNCTION TYPES
// =====================================================================

typedef int (WINAPI* CONNECT)(SOCKET s, const struct sockaddr* name, int namelen);
typedef int (WINAPI* SEND)(SOCKET s, const char* buf, int len, int flags);
typedef int (WINAPI* RECV)(SOCKET s, char* buf, int len, int flags);
typedef int (WINAPI* WSARECV)(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);

CONNECT fpConnect = NULL;
SEND fpSend = NULL;
RECV fpRecv = NULL;
WSARECV fpWSARecv = NULL;

// =====================================================================
//  RNG
// =====================================================================

static std::mt19937 rng(std::random_device{}());
static std::normal_distribution<double> gauss_dist(4.0, 2.5);

static void stealth_sleep() {
    int ms = static_cast<int>(std::abs(gauss_dist(rng))) + 1;
    if (ms > 15) ms = 15;
    Sleep(ms);
}

static void deep_sleep() {
    int ms = static_cast<int>(std::abs(gauss_dist(rng))) + 3;
    if (ms > 25) ms = 25;
    Sleep(ms);
}

static void scope_noop() {
    volatile int x = 0;
    for (volatile int i = 0; i < 7; i++) {
        x ^= i * 3;
        x = rotl8(static_cast<unsigned char>(x), 2);
    }
}

static void scope_noop2() {
    volatile unsigned char buf[8];
    for (volatile int i = 0; i < 8; i++) {
        buf[i] = static_cast<unsigned char>(i * 7);
        buf[i] = rotr8(buf[i], 3) ^ 0x1B;
    }
}

static void memory_fence() {
    volatile int a = 1, b = 2;
    for (volatile int i = 0; i < 4; i++) {
        a ^= b;
        b = rotl8(static_cast<unsigned char>(a), i);
    }
}

// =====================================================================
//  BYTE TRANSFORMS (obfuscated patching, no XOR 0xFF fingerprints)
// =====================================================================

static unsigned char rotl8(unsigned char v, int bits) {
    return (v << bits) | (v >> (8 - bits));
}

static unsigned char rotr8(unsigned char v, int bits) {
    return (v >> bits) | (v << (8 - bits));
}

static void obfuscate_preamble(unsigned char* preamble) {
    unsigned char key = 0xA3;
    for (int i = 0; i < 6; i++) {
        preamble[i] = rotl8(preamble[i] ^ key, 3);
        key = preamble[i];
    }
}

// =====================================================================
//  CONSOLE MENU
// =====================================================================

static void ShowMenu() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hConsole, &csbi);

    SetConsoleTextAttribute(hConsole, 11); // cyan
    printf("\n");
    printf("  ========================================\n");
    printf("   XENITRONIX ELITE BYPASS v%s\n", DLL_VERSION);
    printf("  ========================================\n");
    SetConsoleTextAttribute(hConsole, 7); // white
    printf("\n");
    printf("   [1] CSR Remove Only\n");
    printf("       - Strips ranked/CSR flags from outbound packets\n");
    printf("   [2] Custom Room Spoof (Method A)\n");
    printf("       - Forces room type to custom via field A\n");
    printf("   [3] Custom Room Spoof (Method B)\n");
    printf("       - Forces room type to custom via field B\n");
    printf("   [4] Ranked Pool Patch (original)\n");
    printf("       - Standard pool bypass\n");
    printf("   [5] Memory Patch Mode\n");
    printf("       - Patches game functions in memory\n");
    printf("   [6] Packet Pass-Through\n");
    printf("       - DLL only relays; mitmproxy does all mutation\n");
    printf("   [7] Aggressive (all methods combined)\n");
    printf("\n");
    SetConsoleTextAttribute(hConsole, 14); // yellow
    printf("   Select method (1-7): ");
    SetConsoleTextAttribute(hConsole, 7);
}

// =====================================================================
//  PACKET PATCHING  —  per-mode behavior
// =====================================================================

static bool IsCustomRoomSpoofMode() {
    return g_bypass_mode == MODE_CUSTOM_ROOM_A ||
           g_bypass_mode == MODE_CUSTOM_ROOM_B ||
           g_bypass_mode == MODE_AGGRESSIVE;
}

static bool IsCsrRemoveMode() {
    return g_bypass_mode == MODE_CSR_REMOVE ||
           g_bypass_mode == MODE_AGGRESSIVE;
}

static bool IsRankedPoolMode() {
    return g_bypass_mode == MODE_RANKED_POOL ||
           g_bypass_mode == MODE_AGGRESSIVE;
}

// --------------------  CSR / RANKED FLAG STRIPPING  --------------------
// Looks for common protobuf tags that carry skill rating / ranked flags.
// Tags in the 25-40 range often hold CSR, ELO, tier, or ranked booleans.
static void PatchCsrFields(unsigned char* data, int len) {
    for (int i = 0; i < len - 3; i++) {
        // Ranked flag fields (common varint booleans)
        if ((data[i] == 0x28 || data[i] == 0x30 || data[i] == 0x38 ||
             data[i] == 0x40 || data[i] == 0x48) &&
            (data[i+1] == 0x01 || data[i+1] == 0x02)) {
            // Set to 0 (false/no/disabled)
            data[i+1] = 0x00;
        }
        // Field 25-35 are often used for CSR / division / tier
        if (data[i] >= 0xC8 && data[i] <= 0xFF) {
            if (i+2 < len && (data[i+1] & 0x80) == 0) {
                // Varint field — zero the value
                data[i+1] = 0x00;
            }
        }
    }
}

// --------------------  CUSTOM ROOM SPOOF A  --------------------
// Forces room type field to custom room value.
// Field 26 (0xD2 0x01) or 27 (0xD8 0x01) = custom room = 2, ranked = 1
static void PatchCustomRoomA(unsigned char* data, int len) {
    for (int i = 0; i < len - 4; i++) {
        // Field 26 (wire type varint = 0xD2 0x01)
        if (data[i] == 0xD2 && data[i+1] == 0x01 && i+4 < len) {
            // If value is 1 (ranked), change to 2 (custom)
            if (data[i+2] == 0x01) data[i+2] = 0x02;
            // If value is 2 (custom leave it)
            // If value is 0 (unranked leave it)
        }
        // Field 27 (0xD8 0x01)
        if (data[i] == 0xD8 && data[i+1] == 0x01 && i+4 < len) {
            if (data[i+2] == 0x01) data[i+2] = 0x02;
        }
        // Field 28 (0xE0 0x01) — game mode sub-type
        if (data[i] == 0xE0 && data[i+1] == 0x01 && i+4 < len) {
            if (data[i+2] == 0x04) data[i+2] = 0x03; // ranked clash → casual
        }
    }
}

// --------------------  CUSTOM ROOM SPOOF B  --------------------
// Alternative: patches room ID and match type fields.
// Fields 15-20 often hold room/match identifiers.
static void PatchCustomRoomB(unsigned char* data, int len) {
    for (int i = 0; i < len - 4; i++) {
        // Field 15 (0x7A) — room_type
        if (data[i] == 0x7A && data[i+2] == 0x01 && i+4 < len) {
            if (data[i+3] == 0x01) data[i+3] = 0x02;
        }
        // Field 18 (0x92 0x01) — match_mode
        if (data[i] == 0x92 && data[i+1] == 0x01 && i+4 < len) {
            if (data[i+2] == 0x01) data[i+2] = 0x00; // ranked → normal
        }
        // Zero out any is_csr / has_ranked fields at common positions
        if (data[i] == 0x08 && i+2 < len) {
            if (data[i+1] == 0x01) data[i+1] = 0x00;
        }
    }
}

// --------------------  PC LOGO / EMULATOR DETECTION STRIP  -----------
// Forces platform_type to Android phone and zeros emulator-indicating fields.
static void PatchPcDetection(unsigned char* data, int len) {
    // Field 23 (0xB8) = platform_type → force value 3 (Android Phone)
    for (int i = 0; i < len - 2; i++) {
        if (data[i] == 0xB8 && data[i+1] != 0x03) {
            data[i+1] = 0x03;
        }
    }
    // Field 50 (0x90 0x03) = arch/platform flag → zero
    // Field 51 (0x98 0x03) = arch/platform flag → zero
    // Field 104 (0x80 0x06) = emulator flag → zero
    // Field 105 (0x88 0x06) = emulator flag → zero
    // Field 106 (0x90 0x06) = emulator flag → zero
    // Field 107 (0x98 0x06) = emulator flag → zero
    unsigned char patterns[][3] = {
        {0x90, 0x03, 0x00}, {0x98, 0x03, 0x00},
        {0x80, 0x06, 0x00}, {0x88, 0x06, 0x00},
        {0x90, 0x06, 0x00}, {0x98, 0x06, 0x00},
    };
    for (int p = 0; p < 6; p++) {
        for (int i = 0; i < len - 2; i++) {
            if (data[i] == patterns[p][0] && data[i+1] == patterns[p][1]) {
                data[i+2] = 0x00;
            }
        }
    }
}

// =====================================================================
//  MATCHMAKING RESPONSE PATCHING  —  recv/WSARecv hooks
// =====================================================================
// Intercepts incoming matchmaking protobuf responses and modifies
// workshop_code / ranked / region fields for custom room spoofing.
//
// Target messages (from FFTcp strings):
//   MatchmakingSussNtf   — workshop_code, original_workshop_code, gs_proxy_secret
//   StartSoloRoomMatchmakingReq — workshop_code, pre_workshop_code
//   GroupStartRoomMatchmakingReq — workshop_code, topic_workshop_codes
//   GroupCreateReq       — workshop_code, language
//   GroupJoinReq         — self_region, language, country_code
//   GameOpeningInfoReq   — region, language
//   AccountIDCPingInfo   — id, clock, region
// =====================================================================

// Protobuf string field tags for common matchmaking fields:
//   field 4 string = 0x22, field 5 = 0x2A, field 6 = 0x32,
//   field 7 = 0x3A, field 8 = 0x42, field 9 = 0x4A
// Varint tags: field 1 = 0x08, field 2 = 0x10, field 3 = 0x18

static void PatchRecvBuffer(unsigned char* data, int len) {
    if (len < 16) return;  // Too small to be a matchmaking message

    // ── Phase 1: Strip ranked/CSR varint flags in incoming data ──
    // Targets field 1-3 varints that often hold status/rank indicators
    for (int i = 0; i < len - 2; i++) {
        // Field 1 (0x08), 2 (0x10), 3 (0x18) — ranked/status flags
        if ((data[i] == 0x08 || data[i] == 0x10 || data[i] == 0x18) &&
            (data[i+1] == 0x01 || data[i+1] == 0x02)) {
            // Only zero if the next byte is small — avoid corrupting lengths
            if ((i + 2 >= len) || (data[i+1] <= 5 && data[i+2] <= 0x7F)) {
                data[i+1] = 0x00;
            }
        }
        // Field 90+ varint — often matchmaking pool / tier flags
        if (data[i] == 0x90 && i + 3 < len &&
            (data[i+1] == 0x06 || data[i+1] == 0x07) &&
            (data[i+2] == 0x01 || data[i+2] == 0x02)) {
            data[i+2] = 0x00;
        }
    }

    // ── Phase 2: Patch string fields with workshop_code / room identifiers ──
    // Scan for string field tags (wire type 2) and modify known patterns
    int workshop_field_tags[] = { 0x22, 0x2A, 0x32, 0x3A, 0x42, 0x4A, 0x52, 0x5A };
    int num_tags = sizeof(workshop_field_tags) / sizeof(workshop_field_tags[0]);

    for (int i = 0; i < len - 4; i++) {
        for (int t = 0; t < num_tags; t++) {
            if (data[i] != workshop_field_tags[t]) continue;

            // Read varint length
            int len_pos = i + 1;
            int str_len = 0;
            int shift = 0;
            int varint_bytes = 0;
            while (len_pos < len && varint_bytes < 5) {
                str_len |= (data[len_pos] & 0x7F) << shift;
                shift += 7;
                varint_bytes++;
                if (!(data[len_pos++] & 0x80)) break;
            }
            if (str_len <= 0 || str_len > 64) continue;
            if (len_pos + str_len > len) continue;

            // Check if this string looks like a workshop code or identifier
            char* str_val = (char*)&data[len_pos];
            bool is_workshop = false;
            int scan_max = (str_len < 20) ? str_len : 20;
            for (int s = 0; s < scan_max; s++) {
                if (str_val[s] == '_' || str_val[s] == '-' || str_val[s] == ':') {
                    is_workshop = true;
                    break;
                }
            }

            if (is_workshop && str_len > 3) {
                // Replace with a custom marker: "cstm"
                const char* custom_mark = "cstm";
                int mark_len = 4;
                int new_len = (str_len < 4) ? str_len : mark_len;
                for (int c = 0; c < new_len; c++) {
                    str_val[c] = custom_mark[c];
                }
                // Null out the rest
                for (int c = new_len; c < str_len && c < 8; c++) {
                    str_val[c] = 0x00;
                }
            }

            // Skip past this string field
            i = len_pos + str_len - 1;
            break;
        }
    }

    // ── Phase 3: Look for "region" or "language" string fields ──
    // (field tags 0x62=field12, 0x6A=field13, 0x72=field14 — common for region/lang)
    unsigned char region_tags[] = { 0x62, 0x6A, 0x72, 0x7A, 0x82, 0x01, 0x8A, 0x01 };
    for (int t = 0; t < 8; t++) {
        unsigned char tag = region_tags[t];
        for (int i = 0; i < len - 4; i++) {
            if (data[i] != tag) continue;
            int len_pos = i + 1;
            int str_len = 0;
            int shift = 0;
            int varint_bytes = 0;
            while (len_pos < len && varint_bytes < 5) {
                str_len |= (data[len_pos] & 0x7F) << shift;
                shift += 7;
                varint_bytes++;
                if (!(data[len_pos++] & 0x80)) break;
            }
            if (str_len < 2 || str_len > 16) continue;
            if (len_pos + str_len > len) continue;

            // If region looks wrong (too short, non-standard) don't touch
            // If it contains known region codes, leave it
            char* str_val = (char*)&data[len_pos];
            bool has_alpha = false;
            for (int s = 0; s < str_len; s++) {
                if (isalpha((unsigned char)str_val[s])) { has_alpha = true; break; }
            }
            if (!has_alpha) continue;

            i = len_pos + str_len - 1;
            break;
        }
    }
}

// --------------------  MEMORY PATCH MODE (stub)  --------------------
// In-memory patches require knowing function offsets per game version.
// This is a placeholder — you supply the addresses from your decompilation.
static void ApplyMemoryPatches() {
    // TODO: Add game function addresses here when you find them
    // Example pattern:
    // DWORD old;
    // VirtualProtect((LPVOID)0xADDRESS, 4, PAGE_EXECUTE_READWRITE, &old);
    // *(BYTE*)0xADDRESS = 0x90; // NOP out ranked check
    // VirtualProtect((LPVOID)0xADDRESS, 4, old, &old);
}

// --------------------  MAIN PATCH DISPATCH  ---------------------------
static void PatchBuffer(char* buf, int len) {
    unsigned char* data = (unsigned char*)buf;

    // Always strip PC/emulator detection flags regardless of mode
    PatchPcDetection(data, len);

    if (g_bypass_mode == MODE_PACKET_PASS) {
        return;  // Do nothing — mitmproxy handles everything
    }

    // 1. CSR removal (modes 1, 7)
    if (IsCsrRemoveMode()) {
        PatchCsrFields(data, len);
    }

    // 2. Custom room spoof A (modes 2, 7)
    if (g_bypass_mode == MODE_CUSTOM_ROOM_A || g_bypass_mode == MODE_AGGRESSIVE) {
        PatchCustomRoomA(data, len);
    }

    // 3. Custom room spoof B (modes 3, 7)
    if (g_bypass_mode == MODE_CUSTOM_ROOM_B || g_bypass_mode == MODE_AGGRESSIVE) {
        PatchCustomRoomB(data, len);
    }

    // 4. Standard ranked pool bypass (modes 4, 7)
    if (IsRankedPoolMode()) {
        for (int i = 0; i < len - 2; i++) {
            if (data[i] == 0xB0 && data[i+1] == 0x01 && data[i+2] == 0x01) {
                data[i+2] = 0x00;  // Leave ranked pool
            }
        }
    }

    // 5. Enhanced HWID masking (all modes except pass-through)
    if (g_bypass_mode != MODE_PACKET_PASS) {
        for (int i = 0; i < len - 2; i++) {
            if (data[i] == 0x75 && (data[i+1] > 5)) {
                unsigned char key = 0x5A;
                for (int j = 2; j < 6 && (i + j < len); j++) {
                    data[i + j] = rotr8(data[i + j], 3) ^ key;
                    key = rotl8(key, 1) ^ 0x33;
                }
            }
            if (data[i] == 0x76 && (data[i+1] > 5)) {
                unsigned char key = 0x7B;
                for (int j = 2; j < 6 && (i + j < len); j++) {
                    data[i + j] = rotl8(data[i + j], 2) ^ key;
                    key = rotr8(key, 1) ^ 0x4C;
                }
            }
            if (data[i] == 0x77 && (data[i+1] > 5)) {
                unsigned char key = 0x9D;
                for (int j = 2; j < 6 && (i + j < len); j++) {
                    data[i + j] = rotr8(data[i + j], 5) ^ key;
                    key = rotl8(key, 2) ^ 0x17;
                }
            }
        }
    }

    // 6. Memory patches (mode 5, 7)
    if (g_bypass_mode == MODE_MEMORY_PATCH || g_bypass_mode == MODE_AGGRESSIVE) {
        ApplyMemoryPatches();
    }
}

// =====================================================================
//  HOOKED FUNCTIONS
// =====================================================================

int WINAPI DetourSend(SOCKET s, const char* buf, int len, int flags) {
    scope_noop2();
    if (len > 5) {
        std::vector<char> writable_buf(buf, buf + len);
        PatchBuffer(writable_buf.data(), len);
        scope_noop();
        memory_fence();
        int result = fpSend(s, writable_buf.data(), len, flags);
        scope_noop2();
        return result;
    }
    scope_noop();
    memory_fence();
    int result = fpSend(s, buf, len, flags);
    scope_noop2();
    return result;
}

int WINAPI DetourConnect(SOCKET s, const struct sockaddr* name, int namelen) {
    scope_noop2();
    if (name != NULL && name->sa_family == AF_INET) {
        sockaddr_in* addr = (sockaddr_in*)name;
        unsigned short port = ntohs(addr->sin_port);
        if (port >= 6000) {
            sockaddr_in proxy_addr = { 0 };
            proxy_addr.sin_family = AF_INET;
            proxy_addr.sin_port = htons(PROXY_PORT);
            inet_pton(AF_INET, PROXY_IP, &proxy_addr.sin_addr);
            stealth_sleep();
            memory_fence();
            int result = fpConnect(s, (const struct sockaddr*)&proxy_addr, sizeof(proxy_addr));
            if (result == 0) {
                unsigned char preamble[6];
                memcpy(preamble, &addr->sin_addr.s_addr, 4);
                memcpy(preamble + 4, &addr->sin_port, 2);
                obfuscate_preamble(preamble);
                deep_sleep();
                memory_fence();
                send(s, (const char*)preamble, 6, 0);
            }
            scope_noop2();
            return result;
        }
    }
    scope_noop();
    memory_fence();
    int result = fpConnect(s, name, namelen);
    scope_noop2();
    return result;
}

// =====================================================================
//  RECV HOOK FUNCTIONS
// =====================================================================

int WINAPI DetourRecv(SOCKET s, char* buf, int len, int flags) {
    scope_noop2();
    int result = fpRecv(s, buf, len, flags);
    if (result > 4 && g_bypass_mode != MODE_PACKET_PASS) {
        memory_fence();
        PatchRecvBuffer((unsigned char*)buf, result);
        scope_noop();
    }
    memory_fence();
    scope_noop2();
    return result;
}

int WINAPI DetourWSARecv(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags,
    LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
    scope_noop2();
    int result = fpWSARecv(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, lpOverlapped, lpCompletionRoutine);
    if (result == 0 && lpNumberOfBytesRecvd && *lpNumberOfBytesRecvd > 4) {
        if (g_bypass_mode != MODE_PACKET_PASS) {
            memory_fence();
            for (DWORD i = 0; i < dwBufferCount; i++) {
                if (lpBuffers[i].buf && lpBuffers[i].len > 4) {
                    PatchRecvBuffer((unsigned char*)lpBuffers[i].buf, lpBuffers[i].len > *lpNumberOfBytesRecvd ? *lpNumberOfBytesRecvd : lpBuffers[i].len);
                }
            }
            scope_noop();
        }
    }
    memory_fence();
    scope_noop2();
    return result;
}

// =====================================================================
//  INIT — console menu + hooks
// =====================================================================

DWORD WINAPI InitThread(LPVOID lpParam) {
    // Show console menu and let user choose
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONIN$", "r", stdin);
    SetConsoleTitleA("Xenitronix Bypass - Select Method");

    ShowMenu();
    char choice = _getche();
    int selected = choice - '0';
    if (selected >= 1 && selected <= 7) {
        g_bypass_mode = selected;
    } else {
        g_bypass_mode = MODE_RANKED_POOL; // fallback
    }

    printf("\n  -> Selected: Method %d\n", g_bypass_mode);
    printf("  -> Hooks initializing...\n");

    // Apply memory patches immediately if selected
    if (g_bypass_mode == MODE_MEMORY_PATCH || g_bypass_mode == MODE_AGGRESSIVE) {
        ApplyMemoryPatches();
    }

    MH_Initialize();
    MH_CreateHookApi(L"ws2_32.dll", "connect", &DetourConnect, (LPVOID*)&fpConnect);
    MH_CreateHookApi(L"ws2_32.dll", "send", &DetourSend, (LPVOID*)&fpSend);
    MH_CreateHookApi(L"ws2_32.dll", "recv", &DetourRecv, (LPVOID*)&fpRecv);
    MH_CreateHookApi(L"ws2_32.dll", "WSARecv", &DetourWSARecv, (LPVOID*)&fpWSARecv);
    MH_EnableHook(MH_ALL_HOOKS);

    printf("  -> Hooks active. Mode %d enabled.\n", g_bypass_mode);
    printf("  ========================================\n");

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hModule, DWORD dwReason, LPVOID lpReserved) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    }
    return TRUE;
}
