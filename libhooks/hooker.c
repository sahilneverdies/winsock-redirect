/*
 * hooker — ptrace-based injector for ARM64 Android
 * Injects libhooks.so into the target process and triggers its constructor.
 *
 * Compile:  $NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android21-clang hooker.c -o hooker -static
 * Push:     adb push hooker /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/hooker
 * Usage:    adb shell su -c "/data/local/tmp/hooker com.dts.freefireth /data/local/tmp/libhooks.so"
 *
 * Technique:
 *   1. Ptrace attach to target process
 *   2. Allocate memory in remote process via mmap call
 *   3. Write library path + call dlopen using ARM64 shellcode
 *   4. Detach — constructor auto-patches GOT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/user.h>
#include <dlfcn.h>
#include <errno.h>

/* ARM64 syscall calling convention:
   x8 = syscall number
   x0-x5 = arguments
   svc #0
   Return in x0 */

#define SYS_MMAP 222

/* Shellcode to call dlopen(library_path, RTLD_NOW)
   We write this into the remote process's allocated memory.
   library_path pointer is passed in x0. */
static const uint32_t shellcode[] = {
    0xa9bf7bfd,  /* stp x29, x30, [sp, #-16]! */
    0x910003fd,  /* mov x29, sp */
    /* Load RTLD_NOW (0) into x1 */
    0xd2800001,  /* mov x1, #0 */
    /* x0 already contains library path pointer from caller */
    /* Call dlopen — we'll patch the address at runtime */
    0x00000000,  /* placeholder: bl dlopen (offset patched at runtime) */
    /* Store return value */
    0xaa0003e0,  /* mov x0, x0 — dlopen result */
    /* Restore and return */
    0xa8c17bfd,  /* ldp x29, x30, [sp], #16 */
    0xd65f03c0,  /* ret */
};

/* ──── Find process PID by name ──────────────────────────────────── */
static pid_t find_pid(const char* pname) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "pidof %s", pname);
    FILE* f = popen(cmd, "r");
    if (!f) return -1;
    pid_t pid = -1;
    fscanf(f, "%d", &pid);
    pclose(f);
    return pid;
}

/* ──── Get remote function address via /proc/pid/maps ────────────── */
static uintptr_t get_remote_addr(pid_t pid, const char* lib, const char* sym) {
    /* Find library base from /proc/pid/maps */
    char path[512];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r");
    if (!f) return 0;

    uintptr_t base = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, lib) && strstr(line, "r-xp")) {
            sscanf(line, "%lx", &base);
            break;
        }
    }
    fclose(f);
    if (!base) return 0;

    /* Get symbol offset from local info */
    void* handle = dlopen(lib, RTLD_NOLOAD | RTLD_LAZY);
    if (!handle) return 0;
    void* local_addr = dlsym(handle, sym);
    if (!local_addr) { dlclose(handle); return 0; }

    /* Get local library base */
    Dl_info info;
    dladdr(local_addr, &info);
    uintptr_t local_base = 0;
    FILE* fl = fopen("/proc/self/maps", "r");
    if (fl) {
        while (fgets(line, sizeof(line), fl)) {
            if (strstr(line, info.dli_fname) && strstr(line, "r-xp")) {
                sscanf(line, "%lx", &local_base);
                break;
            }
        }
        fclose(fl);
    }
    dlclose(handle);

    if (!local_base) return 0;
    uintptr_t offset = (uintptr_t)local_addr - local_base;
    return base + offset;
}

/* ──── Inject library via ptrace ─────────────────────────────────── */
static int inject_library(pid_t pid, const char* lib_path) {
    int ret;
    struct user_regs_struct regs;
    struct user_regs_struct orig_regs;

    /* Step 1: Attach */
    ret = ptrace(PTRACE_ATTACH, pid, NULL, NULL);
    if (ret < 0) {
        perror("PTRACE_ATTACH");
        return -1;
    }
    waitpid(pid, NULL, 0);

    /* Step 2: Get current registers */
    ret = ptrace(PTRACE_GETREGS, pid, NULL, &orig_regs);
    if (ret < 0) { perror("PTRACE_GETREGS"); goto detach; }
    memcpy(&regs, &orig_regs, sizeof(regs));

    /* Step 3: Find dlopen in remote process */
    uintptr_t dlopen_addr = get_remote_addr(pid, "libdl.so", "dlopen");
    if (!dlopen_addr) {
        fprintf(stderr, "Failed to find dlopen in remote process\n");
        goto detach;
    }

    /* Step 4: Allocate memory in remote process for library path + shellcode */
    size_t path_len = strlen(lib_path) + 1;
    size_t total_size = path_len + sizeof(shellcode) + 4096;
    total_size = (total_size + 4095) & ~4095;

    /* Prepare mmap call: mmap(0, total_size, PROT_RWX, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) */
    regs.regs[0] = 0;                         /* addr = NULL */
    regs.regs[1] = total_size;                 /* length */
    regs.regs[2] = PROT_READ | PROT_WRITE | PROT_EXEC;  /* prot */
    regs.regs[3] = MAP_PRIVATE | MAP_ANONYMOUS;  /* flags */
    regs.regs[4] = -1;                         /* fd = -1 */
    regs.regs[5] = 0;                          /* offset = 0 */
    regs.regs[8] = SYS_MMAP;                   /* syscall number */
    regs.pc = orig_regs.pc;                    /* Use current PC as scratch */

    ret = ptrace(PTRACE_SETREGS, pid, NULL, &regs);
    if (ret < 0) { perror("PTRACE_SETREGS (mmap)"); goto detach; }

    /* Execute syscall */
    ret = ptrace(PTRACE_SYSCALL, pid, NULL, NULL);
    if (ret < 0) { perror("PTRACE_SYSCALL (mmap enter)"); goto restore; }
    waitpid(pid, NULL, 0);

    /* syscall entry — we need to catch the exit */
    ret = ptrace(PTRACE_SYSCALL, pid, NULL, NULL);
    if (ret < 0) { perror("PTRACE_SYSCALL (mmap exit)"); goto restore; }
    waitpid(pid, NULL, 0);

    /* Read mmap result */
    ret = ptrace(PTRACE_GETREGS, pid, NULL, &regs);
    if (ret < 0) { perror("PTRACE_GETREGS (mmap result)"); goto restore; }

    uintptr_t remote_mem = (uintptr_t)regs.regs[0];
    if (remote_mem == (uintptr_t)-1) {
        fprintf(stderr, "Remote mmap failed\n");
        goto restore;
    }

    /* Step 5: Write library path to remote memory */
    for (size_t i = 0; i < path_len; i += 8) {
        uint64_t val = 0;
        memcpy(&val, lib_path + i, (path_len - i < 8) ? (path_len - i) : 8);
        ptrace(PTRACE_POKETEXT, pid, remote_mem + i, (void*)val);
    }

    /* Step 6: Write shellcode after the path */
    uintptr_t sc_addr = remote_mem + path_len;
    for (size_t i = 0; i < sizeof(shellcode); i += 8) {
        uint64_t val = 0;
        memcpy(&val, (uint8_t*)shellcode + i, (sizeof(shellcode) - i < 8) ? (sizeof(shellcode) - i) : 8);
        ptrace(PTRACE_POKETEXT, pid, sc_addr + i, (void*)val);
    }

    /* Patch the BL instruction in shellcode to point to dlopen */
    /* BL instruction format: 0x14000000 | ((offset >> 2) & 0x3ffffff) */
    /* offset = dlopen_addr - (sc_addr + 8)  (BL is at shellcode + 16, PC+8 at execute) */
    int32_t bl_offset = (int32_t)(dlopen_addr - (sc_addr + 16));
    uint32_t bl_instr = 0x94000000 | ((bl_offset >> 2) & 0x03ffffff);
    memcpy((uint8_t*)shellcode + 16, &bl_instr, 4);
    ptrace(PTRACE_POKETEXT, pid, sc_addr + 16, (void*)(uint64_t)bl_instr);

    /* Step 7: Set up call to shellcode */
    /* Set SP to a safe location (use the end of our allocated memory) */
    regs.sp = remote_mem + total_size - 8;

    /* x0 = library path pointer */
    regs.regs[0] = remote_mem;

    /* Set PC to shellcode */
    regs.pc = sc_addr;

    ret = ptrace(PTRACE_SETREGS, pid, NULL, &regs);
    if (ret < 0) { perror("PTRACE_SETREGS (call dlopen)"); goto restore; }

    /* Step 8: Execute the shellcode (single step to dlopen) */
    ret = ptrace(PTRACE_CONT, pid, NULL, NULL);
    if (ret < 0) { perror("PTRACE_CONT"); goto restore; }

    /* Wait for dlopen to complete (it will execute normally) */
    int status;
    waitpid(pid, &status, 0);

    if (WIFSTOPPED(status)) {
        printf("Library injected successfully: %s\n", lib_path);
        goto restore;
    }

restore:
    /* Restore original registers */
    ptrace(PTRACE_SETREGS, pid, NULL, &orig_regs);

detach:
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <process_name|pid> <library_path>\n", argv[0]);
        return 1;
    }

    pid_t pid;
    if (sscanf(argv[1], "%d", &pid) != 1) {
        pid = find_pid(argv[1]);
        if (pid <= 0) {
            fprintf(stderr, "Could not find process: %s\n", argv[1]);
            return 1;
        }
    }

    printf("Injecting %s into PID %d (%s)...\n", argv[2], pid, argv[1]);
    return inject_library(pid, argv[2]);
}
