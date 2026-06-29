#define _CRT_SECURE_NO_WARNINGS

#include "BypassDll.h"
#include "MinHook.h"
#include <vector>
#include <set>
#include <map>
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

#define DLL_VERSION "12.2"

// Forward declarations for byte rotation helpers
static unsigned char rotl8(unsigned char v, int bits);
static unsigned char rotr8(unsigned char v, int bits);

// Track which sockets are redirected (to avoid corrupting non-game connections)
static std::set<SOCKET> g_redirected_socks;
static CRITICAL_SECTION g_sock_lock;


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
    MODE_CAPTURE          = 9,  // Packet capture dump mode
};

volatile int g_bypass_mode = MODE_RANKED_POOL;  // default

// Packet capture mode
static bool g_capture_active = false;
static int g_capture_count = 0;
static CRITICAL_SECTION g_capture_lock;
static HMODULE g_dll_module = NULL;

// Shared DLL directory helpers
static char g_dll_dir[MAX_PATH] = { 0 };
static char g_capture_dir[MAX_PATH] = { 0 };

#define CAPTURE_DIR "dll_captured"

static void ResolveDllDir() {
    if (g_dll_dir[0]) return;
    GetModuleFileNameA(g_dll_module, g_dll_dir, MAX_PATH);
    char* last_slash = strrchr(g_dll_dir, '\\');
    if (last_slash) *last_slash = '\0';
    strcat(g_dll_dir, "\\");
}

static void ResolveCaptureDir() {
    if (g_capture_dir[0]) return;
    ResolveDllDir();
    snprintf(g_capture_dir, MAX_PATH, "%s%s", g_dll_dir, CAPTURE_DIR);
}

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

static void deobfuscate_preamble(unsigned char* preamble) {
    unsigned char key = 0xA3;
    for (int i = 0; i < 6; i++) {
        preamble[i] = rotr8(preamble[i], 3) ^ key;
        key = preamble[i];
    }
}

// =====================================================================
//  CONSOLE MENU
// =====================================================================

// ── ADB auto-injection for emulator property bypass ────────────────
// Step 1: Push + run resetprop script (deletes 52+ emulator props via Magisk).
// Step 2: If compiled libhooks.so + hooker exist, push + inject those too.
static void AutoInjectHook() {
    // Common BlueStacks ADB paths
    const char* adb_paths[] = {
        "C:\\Program Files\\BlueStacks_nxt\\adb.exe",
        "C:\\Program Files\\BlueStacks\\adb.exe",
        "C:\\ProgramData\\BlueStacks_nxt\\adb.exe",
        "C:\\ProgramData\\BlueStacks\\adb.exe",
        NULL
    };

    char adb[MAX_PATH] = { 0 };
    char dll_dir[MAX_PATH] = { 0 };
    GetModuleFileNameA(GetModuleHandle(NULL), dll_dir, MAX_PATH);
    char* last_slash = strrchr(dll_dir, '\\');
    if (last_slash) *last_slash = '\0';

    for (int i = 0; adb_paths[i]; i++) {
        if (GetFileAttributesA(adb_paths[i]) != INVALID_FILE_ATTRIBUTES) {
            strcpy(adb, adb_paths[i]);
            break;
        }
    }
    if (!adb[0]) {
        printf("  [!] adb.exe not found. Install BlueStacks or place adb.exe alongside DLL.\n");
        return;
    }

    char cmd[2048];

    // ── Step 1: Reset emulator properties via Magisk resetprop ──
    // This deletes all 52+ ro.* emulator properties that libanogs checks.
    // No compilation needed — works on any rooted BlueStacks with Magisk.
    printf("  [1/2] Deleting emulator properties via resetprop...\n");
    
    // Build one long ADB shell command to delete all properties
    const char* props[] = {
        "ro.rk.screenshot_enable", "ro.rk.bt_enable", "ro.rk.ethernet_settings",
        "ro.rk.flash_enable", "ro.rk.hdmi_enable", "ro.rksdk.version",
        "ro.rk.display.device", "ro.rk.install.mount", "ro.rk.install.usb",
        "ro.rk.system.build", "ro.cloud.gaming", "ro.vendor.platform",
        "ro.boottime.cloudAppEngine", "ro.boot.qemu",
        "ro.com.cph.vpn.changed", "ro.com.cph.cloud_app_engine",
        "ro.com.cph.remote_input_method", "ro.com.cph.mac_address",
        "ro.com.cph.sfs_enable", "ro.com.cph.non_root", "ro.com.cph.toast_enable",
        "ro.com.cph.as.changed", "ro.com.cph.notification_disable",
        "ro.com.cph.rootfs", "ro.com.cph.poweroff", "ro.com.cph.screenshot",
        "ro.com.cph.vibrate", "ro.com.cph.gps", "ro.com.cph.wifi",
        "ro.com.cph.bluetooth", "ro.com.cph.audio", "ro.com.cph.display",
        "ro.com.cph.camera", "ro.com.cph.sensor", "ro.com.cph.battery",
        "ro.com.cph.storage", "ro.com.cph.network", "ro.com.cph.thermal",
        "ro.com.cph.usb",
        "ro.dalvik.vm.native.bridge", "ro.enable.native.bridge.exec",
        "ro.dalvik.vm.isa.arm64", "ro.dalvik.vm.isa.arm",
        "ro.kernel.qemu", "ro.hardware.bochs", "ro.boottime.fb_ready",
        "ro.product.cpu.abi2", "ro.build.description", "ro.build.display.id",
        "ro.serialno", "ro.bootloader", "ro.bootmode",
        NULL
    };

    // Build the shell command: delete all props in one su -c call
    char prop_cmds[2048] = { 0 };
    strcat(prop_cmds, "su -c \"");
    for (int i = 0; props[i]; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "resetprop -d '%s' 2>/dev/null; ", props[i]);
        strcat(prop_cmds, buf);
    }
    strcat(prop_cmds, "echo done\"");
    
    snprintf(cmd, sizeof(cmd), "\"%s\" shell %s", adb, prop_cmds);
    system(cmd);
    printf("  [OK] Properties cleared.\n");

    // ── Step 2: If compiled libhooks.so + hooker exist, inject those too ──
    char libhooks_path[MAX_PATH];
    char hooker_path[MAX_PATH];
    snprintf(libhooks_path, MAX_PATH, "%s\\libhooks.so", dll_dir);
    snprintf(hooker_path, MAX_PATH, "%s\\hooker", dll_dir);

    if (GetFileAttributesA(libhooks_path) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesA(hooker_path) != INVALID_FILE_ATTRIBUTES) {
        printf("  [2/2] libhooks.so found — injecting hook library...\n");

        snprintf(cmd, sizeof(cmd), "\"%s\" push \"%s\" /data/local/tmp/libhooks.so", adb, libhooks_path);
        system(cmd);

        snprintf(cmd, sizeof(cmd), "\"%s\" push \"%s\" /data/local/tmp/hooker", adb, hooker_path);
        system(cmd);

        snprintf(cmd, sizeof(cmd), "\"%s\" shell su -c \"chmod 755 /data/local/tmp/hooker\"", adb);
        system(cmd);

        snprintf(cmd, sizeof(cmd),
            "\"%s\" shell su -c \"/data/local/tmp/hooker com.dts.freefireth /data/local/tmp/libhooks.so\"",
            adb);
        system(cmd);

        printf("  [OK] Hook library injected.\n");
    } else {
        printf("  [2/2] libhooks.so not found — resetprop only. "
               "Compile from dll_src/libhooks/ with NDK for deeper hook.\n");
    }

    printf("  -> Emulator bypass prep complete. Launch Free Fire now.\n");
}

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
    printf("   [8] Auto Bypass libanogs (ADB + resetprop)\n");
    printf("       - Deletes 52 emulator properties via Magisk resetprop\n");
    printf("       - Optionally injects libhooks.so hook if compiled\n");
    printf("   [9] Packet Capture Mode\n");
    printf("       - Dumps ALL send/recv payloads to 'dll_captured/' folder\n");
    printf("       - PC logo bypass still active\n");
    printf("       - No other modifications (pass-through to mitmproxy)\n");
    printf("\n");
    SetConsoleTextAttribute(hConsole, 14); // yellow
    printf("   Select method (1-9): ");
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
        // Field 26 (wire type varint = 0xD0 0x01)
        if (data[i] == 0xD0 && data[i+1] == 0x01 && i+4 < len) {
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

// =====================================================================
//  PROTOBUF-AWARE CSR → CUSTOM ROOM SPOOFING
//  Uses field number decoding instead of brittle byte patterns.
//  Survives game updates that change encoding but keep field numbers.
// =====================================================================

// Read a protobuf varint from data[pos:]; store result in *value.
// Returns bytes consumed, or 0 on error.
static int read_varint(const unsigned char* data, int len, int pos, unsigned int* value) {
    if (pos >= len) return 0;
    unsigned int result = 0;
    int shift = 0;
    int start = pos;
    while (pos < len && shift < 28) {
        result |= (data[pos] & 0x7F) << shift;
        shift += 7;
        pos++;
        if (!(data[pos - 1] & 0x80)) break;
    }
    *value = result;
    return pos - start;
}

// Write a protobuf varint to buf; returns bytes written.
static int write_varint(unsigned char* buf, unsigned int value) {
    int written = 0;
    do {
        buf[written] = (value & 0x7F) | (value > 0x7F ? 0x80 : 0);
        value >>= 7;
        written++;
    } while (value > 0);
    return written;
}

// Protobuf-aware matchmaking request modifier.
// Walks the protobuf structure using proper varint tag/field decoding.
// For each field in the target list, modifies the value to convert
// a ranked/CSR request into a custom room request.
static void PatchMatchmakingRequest(unsigned char* data, int len) {
    if (len < 8) return;
    int i = 0;
    while (i < len - 2) {
        // Decode field tag
        unsigned int tag = 0;
        int tag_size = read_varint(data, len, i, &tag);
        if (tag_size == 0) break;
        int field_num = tag >> 3;
        int wire_type = tag & 0x07;
        i += tag_size;
        if (i >= len) break;

        switch (wire_type) {
            case 0: { // Varint
                unsigned int value = 0;
                int val_size = read_varint(data, len, i, &value);
                if (val_size == 0) break;

                // Field numbers for CSR → custom room conversion
                switch (field_num) {
                    case 15: // room_type: 1=ranked, 2=custom
                        if (value == 1) data[i] = 2;
                        break;
                    case 18: // match_mode: 1=ranked, 0=normal
                        if (value == 1) data[i] = 0;
                        break;
                    case 26: // room type flag
                    case 27: // room type alt
                        if (value == 1) data[i] = 2;
                        break;
                    case 28: // game mode sub-type
                        if (value == 4) data[i] = 3;
                        break;
                    case 90: // matchmaking pool / tier flag
                    case 91:
                    case 92:
                        data[i] = 0;
                        break;
                }
                i += val_size;
                break;
            }
            case 2: { // Length-delimited (string / submessage)
                unsigned int str_len = 0;
                int len_size = read_varint(data, len, i, &str_len);
                if (len_size == 0) break;
                if (str_len > 256 || i + len_size + str_len > (unsigned int)len) {
                    i += len_size + str_len;
                    break;
                }
                // Check string fields for workshop_code patterns
                if (str_len > 3 && str_len < 64) {
                    char* str_val = (char*)&data[i + len_size];
                    bool has_underscore = false;
                    bool has_ranked = false;
                    for (int s = 0; s < (int)str_len; s++) {
                        if (str_val[s] == '_') { has_underscore = true; break; }
                    }
                    if (!has_underscore && str_len >= 6) {
                        for (int s = 0; s <= (int)str_len - 6; s++) {
                            if ((str_val[s] == 'r' || str_val[s] == 'R') &&
                                (str_val[s+1] == 'a' || str_val[s+1] == 'A') &&
                                (str_val[s+2] == 'n' || str_val[s+2] == 'N') &&
                                (str_val[s+3] == 'k' || str_val[s+3] == 'K') &&
                                (str_val[s+4] == 'e' || str_val[s+4] == 'E') &&
                                (str_val[s+5] == 'd' || str_val[s+5] == 'D')) {
                                has_ranked = true;
                                break;
                            }
                        }
                    }
                    if (has_underscore || has_ranked) {
                        const char* custom_mark = "cstm";
                        int mark_len = 4;
                        for (int c = 0; c < mark_len && c < (int)str_len; c++)
                            str_val[c] = custom_mark[c];
                        for (int c = mark_len; c < (int)str_len && c < 8; c++)
                            str_val[c] = 0x00;

                        // Update protobuf length varint to new_len
                        int new_len = 4;
                        int old_len_size = len_size;
                        int new_len_size = 0;
                        unsigned char len_buf[5];
                        unsigned int tmp = new_len;
                        do {
                            len_buf[new_len_size] = (tmp & 0x7F) | (tmp > 0x7F ? 0x80 : 0);
                            tmp >>= 7;
                            new_len_size++;
                        } while (tmp > 0);
                        if (new_len_size <= old_len_size) {
                            for (int l = 0; l < old_len_size; l++) {
                                data[i + l] = (l < new_len_size) ? len_buf[l] : 0;
                            }
                        }
                    }
                }
                i += len_size + str_len;
                break;
            }
            case 1: i += 8; break; // Fixed64
            case 5: i += 4; break; // Fixed32
            default:
                i++;
                break;
        }
    }
}

// Protobuf-aware matchmaking response modifier.
// Scans incoming response for ranked/CSR varint fields and zeroes them.
// Properly decodes field numbers so it survives encoding changes.
static void PatchMatchmakingResponse(unsigned char* data, int len) {
    if (len < 8) return;
    int i = 0;
    while (i < len - 2) {
        unsigned int tag = 0;
        int tag_size = read_varint(data, len, i, &tag);
        if (tag_size == 0) break;
        int field_num = tag >> 3;
        int wire_type = tag & 0x07;
        i += tag_size;
        if (i >= len) break;

        switch (wire_type) {
            case 0: { // Varint — zero known ranked/status flags
                unsigned int value = 0;
                int val_size = read_varint(data, len, i, &value);
                if (val_size == 0) break;
                if (value <= 5) {
                    switch (field_num) {
                        case 1: case 2: case 3:  // status/rank flags
                        case 14:                  // is_in_special_pool
                        case 22:                  // is_in_emulator_pool
                        case 58:                  // is_cs_special_pool
                            data[i] = 0;
                            break;
                    }
                }
                i += val_size;
                break;
            }
            case 2: { // Length-delimited
                unsigned int str_len = 0;
                int len_size = read_varint(data, len, i, &str_len);
                if (len_size == 0) break;
                if (str_len > 256 || i + len_size + str_len > (unsigned int)len) {
                    i += len_size + str_len;
                    break;
                }
                i += len_size + str_len;
                break;
            }
            case 1: i += 8; break;
            case 5: i += 4; break;
            default: i++; break;
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
    // Fields 99/100 platform_id — randomize to mobile values matching mitmproxy weights
    // Field 99 varint (wire 0): tag = 99<<3|0 = 792 → varint {0x98, 0x06}
    // Field 100 varint (wire 0): tag = 100<<3|0 = 800 → varint {0xA0, 0x06}
    {   int r = std::uniform_int_distribution<int>(0, 99)(rng);
        int plat_val;
        if (r < 85)      plat_val = 3;   // 85% Android
        else if (r < 93) plat_val = 8;   // 8%
        else if (r < 97) plat_val = 4;   // 4%
        else             plat_val = 6;   // 3%
        unsigned char p_id = static_cast<unsigned char>(plat_val);
        unsigned char plat_patterns[][3] = {
            {0x98, 0x06, p_id},
            {0xA0, 0x06, p_id},
        };
        for (int p = 0; p < 2; p++) {
            for (int i = 0; i < len - 2; i++) {
                if (data[i] == plat_patterns[p][0] && data[i+1] == plat_patterns[p][1]) {
                    data[i+2] = plat_patterns[p][2];
                }
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

    // ── Phase 0: Protobuf-aware ranked/CSR field zeroing ──
    PatchMatchmakingResponse(data, len);

    // ── Phase 1: Strip ranked/CSR varint flags in incoming data ──
    // Fallback byte-pattern approach (catches fields the proto walker might miss)

    // ── Phase 2: Patch string fields with workshop_code / room identifiers ──
    // Scan for string field tags (wire type 2) and modify known patterns
    int workshop_field_tags[] = {
        0x0A, 0x12, 0x1A,           // fields 1-3
        0x22, 0x2A, 0x32, 0x3A,     // fields 4-7
        0x42, 0x4A, 0x52, 0x5A,     // fields 8-11
        0x62, 0x6A, 0x72, 0x7A      // fields 12-15
    };
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

            // Sanity check: the string data must be >= 80% printable ASCII
            // to avoid false positives from tag-like bytes in string payloads
            char* str_val = (char*)&data[len_pos];
            int printable = 0;
            int check_count = (str_len < 20) ? str_len : 20;
            for (int s = 0; s < check_count; s++) {
                unsigned char c = (unsigned char)str_val[s];
                if (c >= 0x20 && c < 0x7F) printable++;
            }
            bool is_plausible_string = (printable >= check_count * 9 / 10);

            // If not printable, this was a false positive tag (data byte
            // inside another field's payload). Don't skip — just continue.
            if (!is_plausible_string) break;

            // Workshop codes contain underscores (e.g. "ranked_asia_br")
            // or literal "ranked" text (workshop_name, workshop_desc).
            // Either pattern → replace with custom marker.
            bool has_ranked = false;
            bool has_underscore = false;
            for (int s = 0; s < check_count; s++) {
                unsigned char c = (unsigned char)str_val[s];
                if (c == '_') { has_underscore = true; break; }
            }
            if (!has_underscore && str_len >= 6) {
                // Case-insensitive check for "ranked" at start or standalone
                for (int s = 0; s <= check_count - 6; s++) {
                    if ((str_val[s] == 'r' || str_val[s] == 'R') &&
                        (str_val[s+1] == 'a' || str_val[s+1] == 'A') &&
                        (str_val[s+2] == 'n' || str_val[s+2] == 'N') &&
                        (str_val[s+3] == 'k' || str_val[s+3] == 'K') &&
                        (str_val[s+4] == 'e' || str_val[s+4] == 'E') &&
                        (str_val[s+5] == 'd' || str_val[s+5] == 'D')) {
                        has_ranked = true;
                        break;
                    }
                }
            }

            if ((has_underscore || has_ranked) && str_len > 3) {
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

                // Update protobuf length varint from str_len → new_len
                int old_len_start = i + 1;
                int old_len_size = len_pos - old_len_start;
                int new_len_size = 0;
                unsigned char len_buf[5];
                unsigned int tmp = new_len;
                do {
                    len_buf[new_len_size] = (tmp & 0x7F) | (tmp > 0x7F ? 0x80 : 0);
                    tmp >>= 7;
                    new_len_size++;
                } while (tmp > 0);
                if (new_len_size <= old_len_size) {
                    for (int l = 0; l < old_len_size; l++) {
                        data[old_len_start + l] = (l < new_len_size) ? len_buf[l] : 0;
                    }
                }

                // Skip past this matched field
                i = len_pos + str_len - 1;
                break;
            }
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

    // ── Phase 4: Redirect game server addresses to our proxy ──
    // MatchmakingSussNtf contains address fields like:
    //   gs_proxy_secret  → "ip:port:secret"  (IP:port prefix)
    //   gs_proxy_addr    → "ip:port" or "hostname:port" or hostname
    //   server_proxy_addr→ same as above
    //   server_addr      → "ip:port", bare IP "1.2.3.4", or hostname "gs.example.com"
    // We scan ANY string field for these patterns and replace with
    // PROXY_IP:PROXY_PORT so all game server traffic routes through us.
    const char* proxy_ip = PROXY_IP;
    const char* proxy_port_str = "19112";
    int proxy_ip_len = (int)strlen(proxy_ip);
    int proxy_port_len = (int)strlen(proxy_port_str);
    
    for (int i = 0; i < len - 12; i++) {
        // Decode the field tag to check if wire type is length-delimited (2)
        int tag_reader = i;
        unsigned int field_tag = 0;
        int tag_shift = 0;
        while (tag_reader < len && tag_shift < 28) {
            field_tag |= (data[tag_reader] & 0x7F) << tag_shift;
            tag_shift += 7;
            if (!(data[tag_reader++] & 0x80)) break;
        }
        if ((field_tag & 0x07) != 2) continue; // Not a string field
        
        // Read varint string length
        int len_pos = tag_reader;
        int str_len = 0;
        int shift = 0;
        int vb = 0;
        while (len_pos < len && vb < 5) {
            str_len |= (data[len_pos] & 0x7F) << shift;
            shift += 7;
            vb++;
            if (!(data[len_pos++] & 0x80)) break;
        }
        if (str_len < 7 || str_len > 40) continue; // hostnames/bare IPs can be shorter
        if (len_pos + str_len > len) continue;
        
        // Check string content for address patterns
        char* str_val = (char*)&data[len_pos];
        int dots = 0;
        bool has_colon = false;
        int colon_at = -1;
        bool valid_hostname = true;
        for (int s = 0; s < str_len; s++) {
            unsigned char c = (unsigned char)str_val[s];
            if (c == '.') dots++;
            else if (c == ':') { has_colon = true; if (colon_at < 0) colon_at = s; }
            else if (!isalnum(c) && c != '-') { valid_hostname = false; }
        }
        
        bool should_redirect = false;
        
        // Case A: IP:port (3+ dots, colon before numeric port)
        if (dots >= 3 && has_colon && colon_at > 0) {
            bool all_port_digits = true;
            for (int s = colon_at + 1; s < str_len && all_port_digits; s++) {
                if (!isdigit((unsigned char)str_val[s])) all_port_digits = false;
            }
            if (all_port_digits) should_redirect = true;
        }
        
        // Case B: Bare IP (3+ dots, no colon, only digits+dots = "1.2.3.4")
        if (!should_redirect && dots >= 3 && !has_colon) {
            bool all_ip_chars = true;
            for (int s = 0; s < str_len && all_ip_chars; s++) {
                unsigned char c = (unsigned char)str_val[s];
                if (!isdigit(c) && c != '.') all_ip_chars = false;
            }
            if (all_ip_chars && str_len >= 7) should_redirect = true;
        }
        
        // Case C: Hostname (2+ dots, no colon, valid hostname chars)
        // Catches server_addr as "gs.node.example.com" etc.
        if (!should_redirect && dots >= 2 && !has_colon && valid_hostname && str_len >= 8) {
            should_redirect = true;
        }
        
        if (!should_redirect) continue;
        
        // Build replacement: PROXY_IP:PROXY_PORT
        // If replacement is same length or shorter, do in-place
        int new_len = proxy_ip_len + 1 + proxy_port_len; // ip + colon + port
        if (new_len <= str_len) {
            memcpy(str_val, proxy_ip, proxy_ip_len);
            str_val[proxy_ip_len] = ':';
            memcpy(str_val + proxy_ip_len + 1, proxy_port_str, proxy_port_len);
            // Null-pad the remainder
            if (new_len < str_len) {
                memset(str_val + new_len, 0, str_len - new_len);
            }
            // Update the protobuf length varint to new_len
            int old_len_size = len_pos - tag_reader;
            int new_len_size = 0;
            unsigned char len_buf[5];
            unsigned int tmp = new_len;
            do {
                len_buf[new_len_size] = (tmp & 0x7F) | (tmp > 0x7F ? 0x80 : 0);
                tmp >>= 7;
                new_len_size++;
            } while (tmp > 0);
            if (new_len_size <= old_len_size) {
                // Write new varint length in-place
                for (int l = 0; l < old_len_size; l++) {
                    data[tag_reader + l] = (l < new_len_size) ? len_buf[l] : 0;
                }
            }
            // Skip past this field
            i = len_pos + str_len - 1;
        }
        // If replacement is longer, skip (would need buffer expansion)
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

// ── Protobuf field logger ───────────────────────────────────────────
// Dumps unique field tags from every outbound protobuf to help
// identify the in-game blacklist trigger pattern.
// Enable by setting PROTO_LOG_FILE env var or uncomment below.

#define PROTO_LOG_FILE "proto_fields.log"

static std::set<int> g_logged_fields;
static CRITICAL_SECTION g_log_lock;

static void LogProtobufFields(unsigned char* data, int len, const char* direction) {
    if (len < 3) return;
    
    // Simple protobuf field tag scanner — finds varint field tags
    // Tags are varint-encoded: (field_number << 3) | wire_type
    std::set<int> fields_found;
    for (int i = 0; i < len - 1; i++) {
        // Read varint tag
        int tag = 0;
        int shift = 0;
        int pos = i;
        while (pos < len && shift < 28) {
            tag |= (data[pos] & 0x7F) << shift;
            shift += 7;
            if (!(data[pos++] & 0x80)) break;
        }
        if (tag == 0 || pos == i) continue;
        
        int field_num = tag >> 3;
        int wire_type = tag & 0x07;
        
        if (field_num > 0 && field_num <= 200) {
            fields_found.insert(field_num);
            i = pos - 1; // advance past this tag
        }
    }
    
    if (fields_found.empty()) return;
    
    // Check for new fields
    EnterCriticalSection(&g_log_lock);
    bool has_new = false;
    std::string new_fields;
    for (int f : fields_found) {
        if (g_logged_fields.find(f) == g_logged_fields.end()) {
            g_logged_fields.insert(f);
            has_new = true;
            if (!new_fields.empty()) new_fields += ", ";
            new_fields += std::to_string(f);
        }
    }
    LeaveCriticalSection(&g_log_lock);
    
    if (has_new) {
        // Log to file in DLL directory
        ResolveDllDir();
        char proto_log_path[MAX_PATH];
        snprintf(proto_log_path, MAX_PATH, "%s%s", g_dll_dir, PROTO_LOG_FILE);
        FILE* fl = fopen(proto_log_path, "a");
        if (fl) {
            char ts[64];
            SYSTEMTIME st;
            GetLocalTime(&st);
            sprintf(ts, "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            fprintf(fl, "[%s] [%s] New fields: %s (total %zu unique)\n",
                    ts, direction, new_fields.c_str(), g_logged_fields.size());
            fclose(fl);
        }
        // Also print to console
        printf("  [PROTO] [%s] New fields: %s (total %zu)\n",
               direction, new_fields.c_str(), g_logged_fields.size());
    }
}

// ── Packet capture dump (mode 9) ────────────────────────────────────
// Saves raw send/recv payloads to dll_captured/ for blacklist analysis.
static void DumpPacketCapture(const unsigned char* data, int len, const char* direction, const char* label) {
    if (!g_capture_active || len < 4) return;

    ResolveCaptureDir();
    CreateDirectoryA(g_capture_dir, NULL);

    // Build timestamped filename
    char fname[MAX_PATH];
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(fname, MAX_PATH, "%s\\%04d%02d%02d_%02d%02d%02d_%03d_%s_%s.dat",
             g_capture_dir,
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             direction, label);

    FILE* fp = fopen(fname, "wb");
    if (!fp) return;

    // Write header
    fprintf(fp, "# DLL Packet Capture\n");
    fprintf(fp, "# Direction: %s\n", direction);
    fprintf(fp, "# Label: %s\n", label);
    fprintf(fp, "# Time: %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    fprintf(fp, "# Length: %d\n", len);
    fprintf(fp, "# Hex:\n");

    // Write hex dump
    for (int i = 0; i < len; i++) {
        fprintf(fp, "%02x", data[i]);
        if ((i + 1) % 64 == 0) fprintf(fp, "\n");
    }
    if (len % 64 != 0) fprintf(fp, "\n");
    fclose(fp);

    EnterCriticalSection(&g_capture_lock);
    g_capture_count++;
    int count = g_capture_count;
    LeaveCriticalSection(&g_capture_lock);

    printf("  [CAPTURE] [%s] %s: %d bytes (total: %d)\n", direction, label, len, count);
}

// ── FFTcp field extractor ─────────────────────────────────────────
// Walks protobuf packets and prints/logs all field values to help
// identify which field number corresponds to which FFTcp field name
// (secret, prepare_token, workshop_code, gs_proxy_secret, server_addr, etc.)
// Logs to console + fftcp_secrets.log in the DLL directory.

static FILE* g_fftcp_log = NULL;
static bool g_fftcp_log_opened = false;
static CRITICAL_SECTION g_fftcp_lock;
static std::map<std::string, DWORD> g_fftcp_last_logged;

static void ExtractFFTcpSecrets(unsigned char* data, int len, const char* direction) {
    if (len < 4) return;

    // Open log file on first call
    if (!g_fftcp_log_opened) {
        g_fftcp_log_opened = true;
        InitializeCriticalSection(&g_fftcp_lock);
        ResolveDllDir();
        char log_path[MAX_PATH];
        snprintf(log_path, MAX_PATH, "%sfftcp_secrets.log", g_dll_dir);
        g_fftcp_log = fopen(log_path, "a");
        if (g_fftcp_log) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(g_fftcp_log, "\n=== Session started %02d/%02d/%04d %02d:%02d:%02d ===\n",
                    st.wMonth, st.wDay, st.wYear, st.wHour, st.wMinute, st.wSecond);
        }
    }

    EnterCriticalSection(&g_fftcp_lock);
    int i = 0;
    while (i < len - 2) {
        unsigned int tag = 0;
        int tag_size = read_varint(data, len, i, &tag);
        if (tag_size == 0) break;
        int field_num = tag >> 3;
        int wire_type = tag & 0x07;
        i += tag_size;
        if (i >= len) break;

        char line[512] = {0};
        bool should_log = false;

        // Build a key for rate-limiting (field_num + direction)
        char key[32];
        snprintf(key, sizeof(key), "%s_f%d", direction, field_num);

        switch (wire_type) {
            case 0: { // Varint
                unsigned int value = 0;
                int val_size = read_varint(data, len, i, &value);
                if (val_size == 0) break;
                if (field_num <= 100 && value <= 10) {
                    snprintf(line, sizeof(line), "[FFTCP] [%s] field#%d varint = %u", direction, field_num, value);
                    should_log = true;
                }
                i += val_size;
                break;
            }
            case 2: { // Length-delimited (string)
                unsigned int str_len = 0;
                int len_size = read_varint(data, len, i, &str_len);
                if (len_size == 0) break;
                if (str_len > 1024 || i + len_size + str_len > (unsigned int)len) {
                    i += len_size + str_len;
                    break;
                }
                if (str_len >= 2 && str_len <= 256 && field_num <= 120) {
                    char* str_val = (char*)&data[i + len_size];
                    char display[128];
                    int display_len = str_len < 100 ? (int)str_len : 100;
                    memcpy(display, str_val, display_len);
                    display[display_len] = '\0';
                    for (int s = 0; s < display_len; s++) {
                        if (display[s] < 0x20 || display[s] > 0x7E) display[s] = '.';
                    }
                    snprintf(line, sizeof(line), "[FFTCP] [%s] field#%d = \"%s\"", direction, field_num, display);
                    should_log = true;
                }
                i += len_size + str_len;
                break;
            }
            case 1: i += 8; break;
            case 5: i += 4; break;
            default: i++; break;
        }

        // Rate-limit: log each (field, direction) at most once per 3 seconds
        if (should_log) {
            DWORD now = GetTickCount();
            auto it = g_fftcp_last_logged.find(key);
            if (it == g_fftcp_last_logged.end() || (now - it->second) >= 3000) {
                g_fftcp_last_logged[key] = now;
                printf("%s\n", line);
                if (g_fftcp_log) {
                    fprintf(g_fftcp_log, "%s\n", line);
                    fflush(g_fftcp_log);
                }
            }
        }
    }
    LeaveCriticalSection(&g_fftcp_lock);
}

// ── Built-in ProxyServer ──────────────────────────────────────────
// When the game connects to 127.0.0.1:19112 (PROXY_IP:PROXY_PORT),
// this accepts the connection, reads the 6-byte preamble, deobfuscates
// to find the original server address, then relays to REMOTE_HOST:REMOTE_PORT.
// Eliminates dependency on external Python TCP controller.

struct RelayCtx {
    SOCKET from_sock;
    SOCKET to_sock;
    char direction[8];
};

static DWORD WINAPI ProxyRelayThread(LPVOID lpParam) {
    RelayCtx* ctx = (RelayCtx*)lpParam;
    SOCKET from = ctx->from_sock;
    SOCKET to = ctx->to_sock;
    char dir[8];
    memcpy(dir, ctx->direction, 8);
    delete ctx;

    char buf[65536];
    int n;
    while (true) {
        n = recv(from, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (n > 4) {
            ExtractFFTcpSecrets((unsigned char*)buf, n, dir);
        }
        if (send(to, buf, n, 0) <= 0) break;
    }

    closesocket(from);
    closesocket(to);
    return 0;
}

static DWORD WINAPI ProxyListenThread(LPVOID lpParam) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == INVALID_SOCKET) {
        printf("[PROXY] Failed to create listen socket\n");
        return 1;
    }

    // Allow port reuse
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, PROXY_IP, &addr.sin_addr);
    addr.sin_port = htons(PROXY_PORT);

    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("[PROXY] Bind failed to %s:%d (err %d)\n", PROXY_IP, PROXY_PORT, WSAGetLastError());
        closesocket(listen_sock);
        return 1;
    }

    if (listen(listen_sock, 10) != 0) {
        printf("[PROXY] Listen failed (err %d)\n", WSAGetLastError());
        closesocket(listen_sock);
        return 1;
    }

    printf("[PROXY] Built-in proxy listening on %s:%d\n", PROXY_IP, PROXY_PORT);
    printf("[PROXY] Relay target: %s:%d\n", REMOTE_HOST, REMOTE_PORT);

    while (true) {
        sockaddr_in client_addr;
        int client_addr_len = sizeof(client_addr);
        SOCKET client = accept(listen_sock, (sockaddr*)&client_addr, &client_addr_len);
        if (client == INVALID_SOCKET) continue;

        // Read 6-byte preamble
        unsigned char preamble[6];
        int n = recv(client, (char*)preamble, 6, 0);
        if (n != 6) {
            printf("[PROXY] Preamble read failed (got %d)\n", n);
            closesocket(client);
            continue;
        }

        // Deobfuscate a copy to log the original server
        unsigned char decoded[6];
        memcpy(decoded, preamble, 6);
        deobfuscate_preamble(decoded);
        char orig_ip[32];
        snprintf(orig_ip, sizeof(orig_ip), "%d.%d.%d.%d",
                 decoded[0], decoded[1], decoded[2], decoded[3]);
        unsigned short orig_port = (unsigned short)((decoded[4] << 8) | decoded[5]);
        orig_port = ntohs(orig_port); // preamble stores port in network byte order

        // Resolve REMOTE_HOST
        sockaddr_in remote_addr;
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_port = htons(REMOTE_PORT);
        struct addrinfo hints = { 0 };
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* ai = NULL;
        if (getaddrinfo(REMOTE_HOST, NULL, &hints, &ai) != 0 || !ai) {
            printf("[PROXY] Failed to resolve %s\n", REMOTE_HOST);
            closesocket(client);
            continue;
        }
        remote_addr.sin_addr = ((struct sockaddr_in*)ai->ai_addr)->sin_addr;
        freeaddrinfo(ai);

        SOCKET remote = socket(AF_INET, SOCK_STREAM, 0);
        if (remote == INVALID_SOCKET) {
            closesocket(client);
            continue;
        }

        if (connect(remote, (sockaddr*)&remote_addr, sizeof(remote_addr)) != 0) {
            printf("[PROXY] Connect to %s:%d failed (err %d)\n",
                   REMOTE_HOST, REMOTE_PORT, WSAGetLastError());
            closesocket(client);
            closesocket(remote);
            continue;
        }

        // Forward the RAW preamble (still obfuscated) to remote
        if (send(remote, (char*)preamble, 6, 0) <= 0) {
            closesocket(client);
            closesocket(remote);
            continue;
        }

        char client_ip[32];
        InetNtopA(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("[PROXY] Relay: %s -> %s:%d -> %s:%d (orig: %s:%d)\n",
               client_ip, PROXY_IP, PROXY_PORT,
               REMOTE_HOST, REMOTE_PORT, orig_ip, orig_port);

        // Create two relay threads (one per direction)
        RelayCtx* ctx1 = new RelayCtx{ client, remote, "SEND" };
        RelayCtx* ctx2 = new RelayCtx{ remote, client, "RECV" };
        CreateThread(NULL, 0, ProxyRelayThread, ctx1, 0, NULL);
        CreateThread(NULL, 0, ProxyRelayThread, ctx2, 0, NULL);
    }

    closesocket(listen_sock);
    return 0;
}

// --------------------  MAIN PATCH DISPATCH  ---------------------------
static void PatchBuffer(char* buf, int len) {
    unsigned char* data = (unsigned char*)buf;

    // In capture mode, dump raw packet before any modification
    if (g_capture_active) {
        DumpPacketCapture(data, len, "SEND", "original");
    }

    // Log protobuf fields from every outbound packet
    LogProtobufFields(data, len, "SEND");

    // Extract and display FFTcp field values
    ExtractFFTcpSecrets(data, len, "SEND");

    // Always strip PC/emulator detection flags regardless of mode
    PatchPcDetection(data, len);

    if (g_bypass_mode == MODE_PACKET_PASS || g_bypass_mode == MODE_CAPTURE) {
        return;  // Do nothing — mitmproxy handles everything
    }

    // 1. CSR→custom room spoofing (all modes except pass-through/capture)
    //    Uses protobuf-aware field number decoding — survives game updates.
    if (g_bypass_mode != MODE_PACKET_PASS && g_bypass_mode != MODE_CAPTURE) {
        PatchMatchmakingRequest(data, len);
    }

    // 2. Enhanced HWID masking (all modes except pass-through/capture)
    if (g_bypass_mode != MODE_PACKET_PASS && g_bypass_mode != MODE_CAPTURE) {
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
        EnterCriticalSection(&g_sock_lock);
        bool is_redirected = (g_redirected_socks.find(s) != g_redirected_socks.end());
        LeaveCriticalSection(&g_sock_lock);

        if (is_redirected) {
            std::vector<char> writable_buf(buf, buf + len);
            PatchBuffer(writable_buf.data(), len);
            scope_noop();
            memory_fence();
            int result = fpSend(s, writable_buf.data(), len, flags);
            scope_noop2();
            return result;
        }
    }
    scope_noop();
    memory_fence();
    int result = fpSend(s, buf, len, flags);
    scope_noop2();
    return result;
}

static bool resolve_proxy_ip(struct in_addr* out) {
    struct addrinfo hints = { 0 };
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo* result = NULL;
    int rc = getaddrinfo(PROXY_IP, NULL, &hints, &result);
    if (rc != 0 || !result) return false;
    *out = ((struct sockaddr_in*)result->ai_addr)->sin_addr;
    freeaddrinfo(result);
    return true;
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
            if (!resolve_proxy_ip(&proxy_addr.sin_addr)) {
                // fallback: try inet_pton in case PROXY_IP is a literal IP
                inet_pton(AF_INET, PROXY_IP, &proxy_addr.sin_addr);
            }
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
                EnterCriticalSection(&g_sock_lock);
                g_redirected_socks.insert(s);
                LeaveCriticalSection(&g_sock_lock);
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
    if (result > 4) {
        // Log incoming protobuf fields
        LogProtobufFields((unsigned char*)buf, result, "RECV");

        // Extract and display FFTcp field values
        ExtractFFTcpSecrets((unsigned char*)buf, result, "RECV");

        EnterCriticalSection(&g_sock_lock);
        bool is_redirected = (g_redirected_socks.find(s) != g_redirected_socks.end());
        LeaveCriticalSection(&g_sock_lock);

        if (is_redirected) {
            if (g_capture_active) {
                DumpPacketCapture((unsigned char*)buf, result, "RECV", "original");
            }
            if (g_bypass_mode != MODE_PACKET_PASS && g_bypass_mode != MODE_CAPTURE) {
                memory_fence();
                PatchRecvBuffer((unsigned char*)buf, result);
                scope_noop();
            }
        }
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
        // Log incoming protobuf fields (WSARecv variant)
        for (DWORD i = 0; i < dwBufferCount; i++) {
            if (lpBuffers[i].buf && lpBuffers[i].len > 4) {
                DWORD pkt_len = lpBuffers[i].len > *lpNumberOfBytesRecvd ? *lpNumberOfBytesRecvd : lpBuffers[i].len;
                LogProtobufFields((unsigned char*)lpBuffers[i].buf, pkt_len, "RECV");
                ExtractFFTcpSecrets((unsigned char*)lpBuffers[i].buf, pkt_len, "RECV");
            }
        }

        EnterCriticalSection(&g_sock_lock);
        bool is_redirected = (g_redirected_socks.find(s) != g_redirected_socks.end());
        LeaveCriticalSection(&g_sock_lock);

        if (is_redirected) {
            if (g_capture_active) {
                for (DWORD i = 0; i < dwBufferCount; i++) {
                    if (lpBuffers[i].buf && lpBuffers[i].len > 4) {
                        DumpPacketCapture((unsigned char*)lpBuffers[i].buf,
                            lpBuffers[i].len > *lpNumberOfBytesRecvd ? *lpNumberOfBytesRecvd : lpBuffers[i].len,
                            "RECV", "original");
                    }
                }
            }
            if (g_bypass_mode != MODE_PACKET_PASS && g_bypass_mode != MODE_CAPTURE) {
                memory_fence();
                for (DWORD i = 0; i < dwBufferCount; i++) {
                    if (lpBuffers[i].buf && lpBuffers[i].len > 4) {
                        PatchRecvBuffer((unsigned char*)lpBuffers[i].buf, lpBuffers[i].len > *lpNumberOfBytesRecvd ? *lpNumberOfBytesRecvd : lpBuffers[i].len);
                    }
                }
                scope_noop();
            }
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
    // Initialize socket tracking
    InitializeCriticalSection(&g_sock_lock);
    InitializeCriticalSection(&g_log_lock);

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
    } else if (selected == 8) {
        g_bypass_mode = MODE_AGGRESSIVE; // Use aggressive patching + ADB hook
        printf("\n  -> Selected: Method 8 (ADB Inject + Aggressive)\n");
        printf("  -> Injecting libhooks.so into emulator...\n");
        AutoInjectHook();
    } else if (selected == 9) {
        g_bypass_mode = MODE_CAPTURE; // Packet capture, pass-through, no extra modifications
        g_capture_active = true;
        InitializeCriticalSection(&g_capture_lock);
        ResolveCaptureDir();
        CreateDirectoryA(g_capture_dir, NULL);
        printf("\n  -> Selected: Method 9 (Packet Capture Mode)\n");
        printf("  -> Dumping ALL send/recv payloads to: %s\n", g_capture_dir);
        printf("  -> PC logo bypass still active\n");
    } else {
        g_bypass_mode = MODE_RANKED_POOL; // fallback
    }

    printf("\n  -> Selected: Method %d%s\n", g_bypass_mode,
           g_capture_active ? " (capture active)" : "");
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

    // Start built-in proxy server (listens on 127.0.0.1:PROXY_PORT,
    // relays to REMOTE_HOST:REMOTE_PORT after preamble decode)
    CreateThread(NULL, 0, ProxyListenThread, NULL, 0, NULL);

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hModule, DWORD dwReason, LPVOID lpReserved) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_dll_module = hModule;
        DisableThreadLibraryCalls(hModule);
        // Start built-in proxy immediately (before InitThread) so it's
        // listening by the time the game connects to 127.0.0.1:19112
        CreateThread(NULL, 0, ProxyListenThread, NULL, 0, NULL);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    }
    return TRUE;
}
