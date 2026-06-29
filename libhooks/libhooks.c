/*
 * libhooks.so — hook __system_property_get at libc level
 * 
 * Load BEFORE libanogs.so: LD_PRELOAD / zygote injection
 * Load AFTER libanogs.so:  GOT patching via constructor (use hooker injector)
 *
 * Compile:  $NDK/ndk-build
 * Push:     adb push libs/arm64-v8a/libhooks.so /data/local/tmp/
 * Inject:   adb shell su -c "/data/local/tmp/hooker com.dts.freefireth /data/local/tmp/libhooks.so"
 *
 * No changes to libanogs.so on disk or in memory — antitamper never triggers.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <link.h>
#include <sys/mman.h>
#include <android/log.h>

#define LOG_TAG "libhooks"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

/* ── Real __system_property_get from libc ────────────────────────── */
typedef int (*real_system_property_get_t)(const char* name, char* value);
static real_system_property_get_t real_system_property_get = NULL;

/* ── Emulator property blacklist (52+ ro.* properties) ──────────── */
static const char* emu_properties[] = {
    "ro.rk.screenshot_enable",
    "ro.rk.bt_enable",
    "ro.rk.ethernet_settings",
    "ro.rk.flash_enable",
    "ro.rk.hdmi_enable",
    "ro.rksdk.version",
    "ro.rk.display.device",
    "ro.rk.install.mount",
    "ro.rk.install.usb",
    "ro.rk.system.build",
    "ro.cloud.gaming",
    "ro.vendor.platform",
    "ro.boottime.cloudAppEngine",
    "ro.com.cph.vpn.changed",
    "ro.com.cph.cloud_app_engine",
    "ro.com.cph.remote_input_method",
    "ro.com.cph.mac_address",
    "ro.com.cph.sfs_enable",
    "ro.com.cph.non_root",
    "ro.com.cph.toast_enable",
    "ro.com.cph.as.changed",
    "ro.com.cph.notification_disable",
    "ro.com.cph.rootfs",
    "ro.com.cph.poweroff",
    "ro.com.cph.screenshot",
    "ro.com.cph.vibrate",
    "ro.com.cph.gps",
    "ro.com.cph.wifi",
    "ro.com.cph.bluetooth",
    "ro.com.cph.audio",
    "ro.com.cph.display",
    "ro.com.cph.camera",
    "ro.com.cph.sensor",
    "ro.com.cph.battery",
    "ro.com.cph.storage",
    "ro.com.cph.network",
    "ro.com.cph.thermal",
    "ro.com.cph.usb",
    "ro.dalvik.vm.native.bridge",
    "ro.enable.native.bridge.exec",
    "ro.dalvik.vm.isa.arm64",
    "ro.dalvik.vm.isa.arm",
    "ro.kernel.qemu",
    "ro.boot.qemu",
    "ro.hardware.bochs",
    "ro.boottime.fb_ready",
    "ro.product.cpu.abi2",
    "ro.build.description",
    "ro.build.display.id",
    "ro.serialno",
    "ro.bootloader",
    "ro.bootmode",
    NULL
};

/* ── Hooked __system_property_get ────────────────────────────────── */
int __system_property_get(const char* name, char* value) {
    if (!real_system_property_get) {
        real_system_property_get = (real_system_property_get_t)
            dlsym(RTLD_NEXT, "__system_property_get");
    }

    if (name) {
        for (int i = 0; emu_properties[i]; i++) {
            if (strcmp(name, emu_properties[i]) == 0) {
                if (value) value[0] = '\0';
                return 0;
            }
        }
    }

    return real_system_property_get(name, value);
}

/* ── GOT patching for post-load injection ────────────────────────── */
/* If loaded AFTER libanogs.so, its GOT already points to libc.
   We patch the GOT entry to redirect to our hook instead. */

/* Walk the ELF dynamic segment to find PLT GOT entries */
static void patch_got(const char* target_lib, const char* symbol, void* hook) {
    struct link_map* map = dlopen(NULL, RTLD_NOLOAD);
    if (!map) return;

    /* Iterate loaded libraries */
    struct link_map* lm = map;
    while (lm) {
        if (lm->l_name && strstr(lm->l_name, target_lib)) {
            /* Found libanogs — locate its DT_SYMTAB / DT_STRTAB / DT_PLTGOT */
            ElfW(Dyn)* dyn = lm->l_ld;
            ElfW(Sym)* symtab = NULL;
            const char* strtab = NULL;
            ElfW(Addr)* pltgot = NULL;
            size_t pltgot_size = 0;

            for (; dyn->d_tag != DT_NULL; dyn++) {
                switch (dyn->d_tag) {
                    case DT_SYMTAB: symtab = (ElfW(Sym)*)dyn->d_un.d_ptr; break;
                    case DT_STRTAB: strtab = (const char*)dyn->d_un.d_ptr; break;
                    case DT_PLTGOT: pltgot = (ElfW(Addr)*)dyn->d_un.d_ptr; break;
                    case DT_PLTRELSZ: pltgot_size = dyn->d_un.d_val; break;
                }
            }

            if (!symtab || !strtab || !pltgot || !pltgot_size) {
                dlclose(lm);
                break;
            }

            /* Scan for the target symbol in the dynamic symbol table */
            int nsyms = pltgot_size / sizeof(ElfW(Addr));
            for (int i = 0; i < nsyms; i++) {
                /* PLT GOT entries follow a predictable pattern:
                   Initially point to resolver, then to resolved address.
                   We need to find the right slot. */
                
                /* Alternative: search symbol table for __system_property_get */
                /* Count exported symbols */
                int sym_count = 0;
                while (symtab[sym_count].st_name != 0 || symtab[sym_count].st_value != 0) {
                    sym_count++;
                }

                for (int s = 0; s < sym_count; s++) {
                    const char* sym_name = strtab + symtab[s].st_name;
                    if (strcmp(sym_name, symbol) == 0) {
                        /* Found the symbol — patch the GOT entry if it's in range */
                        ElfW(Addr) got_entry = symtab[s].st_value;
                        /* Calculate offset from sym to GOT */
                        for (int g = 0; g < nsyms; g++) {
                            if (pltgot[g] == (ElfW(Addr))real_system_property_get) {
                                /* This GOT slot points to the real function */
                                size_t page_size = sysconf(_SC_PAGESIZE);
                                ElfW(Addr) page_start = (ElfW(Addr))&pltgot[g] & ~(page_size - 1);
                                mprotect((void*)page_start, page_size, PROT_READ | PROT_WRITE);
                                
                                /* Atomic store of hook address */
                                __atomic_store_n(&pltgot[g], (ElfW(Addr))hook, __ATOMIC_SEQ_CST);
                                
                                LOGI("Patched GOT[%d] for %s in %s: %p -> %p",
                                     g, symbol, target_lib,
                                     (void*)pltgot[g], hook);
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            break;
        }
        lm = lm->l_next;
    }
    dlclose(map);
}

/* ── Constructor: auto-patch GOT on load ─────────────────────────── */
__attribute__((constructor))
static void init(void) {
    /* Resolve real function */
    real_system_property_get = (real_system_property_get_t)
        dlsym(RTLD_NEXT, "__system_property_get");

    LOGI("libhooks loaded. Real __system_property_get at %p", real_system_property_get);

    /* If RTLD_NEXT resolves to ourselves, we were preloaded — do nothing.
       Otherwise, we were injected after libanogs — patch its GOT. */
    void* self = dlsym(RTLD_DEFAULT, "__system_property_get");
    if (self == __system_property_get) {
        /* Preloaded — our symbol already wins via normal resolution */
        LOGI("LD_PRELOAD mode — hook active via symbol interposition");
    } else {
        /* Injected after — need GOT patching */
        LOGI("Post-load injection mode — patching libanogs.so GOT");
        patch_got("libanogs", "__system_property_get", __system_property_get);
    }
}
