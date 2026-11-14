#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdarg.h>
#include <execinfo.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>

static void preload_log(const char *fmt, ...) {
    FILE *f = fopen("/tmp/preload_stub.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fclose(f);
}
#define printf(...) do { preload_log(__VA_ARGS__); fprintf(stderr, __VA_ARGS__); } while(0)

static struct sigaction g_old_segv;
static struct sigaction g_old_abrt;
static struct sigaction g_old_bus;

static int safe_read_mem(uint64_t addr, void *out, size_t len) {
    if (addr < 0x10000ULL || addr > 0x00007fffffffffffULL) return 0;
    int pfd[2];
    if (pipe(pfd) != 0) return 0;
    ssize_t w = write(pfd[1], (const void *)(uintptr_t)addr, len);
    int ok = 0;
    if (w == (ssize_t)len) {
        ssize_t r = read(pfd[0], out, len);
        if (r == (ssize_t)len) ok = 1;
    }
    close(pfd[0]);
    close(pfd[1]);
    return ok;
}

static void safe_read_cstr(uint64_t addr, char *out, size_t max_len) {
    out[0] = '\0';
    for (size_t i = 0; i + 1 < max_len; i++) {
        char c = 0;
        if (!safe_read_mem(addr + i, &c, 1)) {
            if (i == 0) snprintf(out, max_len, "<unreadable@0x%llx>", (unsigned long long)addr);
            else out[i] = '\0';
            return;
        }
        out[i] = (c >= 0x20 && c < 0x7f) ? c : (c == '\0' ? '\0' : '?');
        if (c == '\0') return;
    }
    out[max_len - 1] = '\0';
}

static void dump_mono_class(int fd, const char *label, uint64_t addr) {
    uint8_t raw[0xf0];
    if (!safe_read_mem(addr, raw, sizeof(raw))) {
        dprintf(fd, "[%s @ 0x%llx]: <unreadable>\n", label, (unsigned long long)addr);
        return;
    }
    uint64_t elem = 0, parent = 0, nested = 0, image = 0, name_p = 0, ns_p = 0;
    uint32_t token = 0;
    memcpy(&elem, raw + 0x00, 8);
    memcpy(&parent, raw + 0x28, 8);
    memcpy(&nested, raw + 0x30, 8);
    memcpy(&image, raw + 0x38, 8);
    memcpy(&name_p, raw + 0x40, 8);
    memcpy(&ns_p, raw + 0x48, 8);
    memcpy(&token, raw + 0x50, 4);
    char name[96], ns[96];
    safe_read_cstr(name_p, name, sizeof(name));
    safe_read_cstr(ns_p, ns, sizeof(ns));
    dprintf(fd, "[%s @ 0x%llx]: ns='%s' name='%s' kind=%u vt=%u byval_type=0x%02x token=0x%08x elem=0x%llx parent=0x%llx nested=0x%llx image=0x%llx\n",
            label, (unsigned long long)addr, ns, name,
            (unsigned)(raw[0x24] & 7), (unsigned)((raw[0x20] >> 2) & 1),
            (unsigned)raw[0xba], token,
            (unsigned long long)elem, (unsigned long long)parent,
            (unsigned long long)nested, (unsigned long long)image);
    for (int off = 0; off < 0xf0; off += 16) {
        dprintf(fd, "  +0x%02x:", off);
        for (int j = 0; j < 16; j++) dprintf(fd, " %02x", raw[off + j]);
        dprintf(fd, "\n");
    }
}

static void crash_handler(int sig, siginfo_t *info, void *ucontext) {
    int fd = open("/tmp/crash_backtrace.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        dprintf(fd, "=== CRASH SIGNAL %d (%s) CAUGHT ===\n", sig, strsignal(sig));
        dprintf(fd, "Fault address: %p\n", info->si_addr);
        ucontext_t *uc = (ucontext_t *)ucontext;
        if (uc) {
            dprintf(fd, "RIP: 0x%llx\n", (unsigned long long)uc->uc_mcontext.gregs[REG_RIP]);
            dprintf(fd, "RSP: 0x%llx\n", (unsigned long long)uc->uc_mcontext.gregs[REG_RSP]);
            dprintf(fd, "RAX: 0x%llx  RBX: 0x%llx  RCX: 0x%llx  RDX: 0x%llx\n",
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RAX],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RBX],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RCX],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RDX]);
            dprintf(fd, "RDI: 0x%llx  RSI: 0x%llx  RBP: 0x%llx\n",
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RDI],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RSI],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_RBP]);
            dprintf(fd, "R12: 0x%llx  R13: 0x%llx  R14: 0x%llx  R15: 0x%llx\n",
                    (unsigned long long)uc->uc_mcontext.gregs[REG_R12],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_R13],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_R14],
                    (unsigned long long)uc->uc_mcontext.gregs[REG_R15]);
            if (sig == SIGSEGV) {
                uint64_t rax = (uint64_t)uc->uc_mcontext.gregs[REG_RAX];
                uint64_t rdi = (uint64_t)uc->uc_mcontext.gregs[REG_RDI];
                dump_mono_class(fd, "RAX", rax);
                if (rdi > 0xffffffffULL) {
                    dump_mono_class(fd, "RDI_low32-0xb0", (rdi & 0xffffffffULL) - 0xb0ULL);
                }
            }
        }
        void *buffer[128];
        int nptrs = backtrace(buffer, 128);
        dprintf(fd, "Backtrace (%d frames):\n", nptrs);
        backtrace_symbols_fd(buffer, nptrs, fd);
        dprintf(fd, "=== END CRASH DUMP ===\n\n");
        close(fd);
    }
    
    // Also log to preload_stub.log
    preload_log("[CRASH HANDLER] Signal %d at %p! Detailed dump in /tmp/crash_backtrace.log\n", sig, info->si_addr);

    // Chain to previous handler
    struct sigaction *old_sa = (sig == SIGSEGV) ? &g_old_segv : (sig == SIGABRT ? &g_old_abrt : &g_old_bus);
    if (old_sa->sa_flags & SA_SIGINFO) {
        if (old_sa->sa_sigaction) {
            old_sa->sa_sigaction(sig, info, ucontext);
            return;
        }
    } else if (old_sa->sa_handler == SIG_DFL) {
        signal(sig, SIG_DFL);
        raise(sig);
    } else if (old_sa->sa_handler != SIG_IGN && old_sa->sa_handler) {
        old_sa->sa_handler(sig);
        return;
    }
}

static void install_crash_handler(void) {
    stack_t ss;
    ss.ss_sp = malloc(SIGSTKSZ * 4);
    ss.ss_size = SIGSTKSZ * 4;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGABRT, &sa, &g_old_abrt);
    sigaction(SIGBUS, &sa, &g_old_bus);
    printf("[PreLoad] Native crash handlers installed.\n");
}

static void patch_code(void *addr, const uint8_t *code, size_t size) {
    long pagesize = sysconf(_SC_PAGESIZE);
    void *page = (void *)((uintptr_t)addr & ~(pagesize - 1));
    if (mprotect(page, pagesize * 2, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("[PreLoad] mprotect RWX failed");
        return;
    }
    memcpy(addr, code, size);
    __builtin___clear_cache((char*)addr, (char*)addr + size);
    printf("[PreLoad] Successfully patched code at %p\n", addr);
}

typedef void* (*mono_class_from_mono_type_t)(void *type);
static mono_class_from_mono_type_t g_real_mono_class_from_mono_type = NULL;

static void* hook_mono_class_from_mono_type(void *type) {
    if (((uintptr_t)type >> 32) == 0x100) {
        void *cleaned = (void*)((uintptr_t)type & 0xffffffffULL);
        printf("[PreLoad Hook] mono_class_from_mono_type: %p -> %p\n", type, cleaned);
        type = cleaned;
    }
    return g_real_mono_class_from_mono_type(type);
}

typedef void* (*fn_1f816a_t)(void *domain, void *type, void *error);
static fn_1f816a_t g_real_1f816a = NULL;

static void* hook_1f816a(void *domain, void *type, void *error) {
    if (((uintptr_t)type >> 32) == 0x100) {
        void *cleaned = (void*)((uintptr_t)type & 0xffffffffULL);
        printf("[PreLoad Hook] 0x1f816a: %p -> %p\n", type, cleaned);
        type = cleaned;
    }
    return g_real_1f816a(domain, type, error);
}

static void install_mono_type_sanitizer(void *mono) {
    // 1. Get base address of libmonobdwgc-2.0.so and patch GetBaseType callsites
    void *sym = dlsym(mono, "mono_domain_get");
    Dl_info info;
    if (dladdr(sym, &info) && info.dli_fbase) {
        uint8_t *base = (uint8_t*)info.dli_fbase;
        uint8_t *p1 = base + 0x187c3d;
        uint8_t *p2 = base + 0x187cfe;

        uint8_t patch_add_esi[7] = { 0x81, 0xc6, 0xb0, 0x00, 0x00, 0x00, 0x90 };
        if (memcmp(p1, "\x48\x81\xc6\xb0\x00\x00\x00", 7) == 0) {
            patch_code(p1, patch_add_esi, 7);
            printf("[PreLoad] Patched GetBaseType (+0x187c3d) with add $0xb0, %%esi!\n");
        }
        if (memcmp(p2, "\x48\x81\xc6\xb0\x00\x00\x00", 7) == 0) {
            patch_code(p2, patch_add_esi, 7);
            printf("[PreLoad] Patched (+0x187cfe) with add $0xb0, %%esi!\n");
        }

        // 2. Install trampoline hook on 0x1f816a (15 bytes)
        uint8_t *target_1f816a = base + 0x1f816a;
        uint8_t *tramp_1f = (uint8_t*)mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (tramp_1f != MAP_FAILED) {
            memcpy(tramp_1f, target_1f816a, 15);
            uint8_t *jmp_back = tramp_1f + 15;
            uintptr_t ret_1f = (uintptr_t)target_1f816a + 15;
            jmp_back[0] = 0x49; jmp_back[1] = 0xbb; // movabs <ret>, %r11
            memcpy(&jmp_back[2], &ret_1f, 8);
            jmp_back[10] = 0x41; jmp_back[11] = 0xff; jmp_back[12] = 0xe3; // jmp *%r11
            g_real_1f816a = (fn_1f816a_t)tramp_1f;

            uint8_t patch_1f[15];
            patch_1f[0] = 0x49; patch_1f[1] = 0xbb;
            uintptr_t hook_1f_addr = (uintptr_t)hook_1f816a;
            memcpy(&patch_1f[2], &hook_1f_addr, 8);
            patch_1f[10] = 0x41; patch_1f[11] = 0xff; patch_1f[12] = 0xe3;
            patch_1f[13] = 0x90; patch_1f[14] = 0x90;
            patch_code(target_1f816a, patch_1f, 15);
            __builtin___clear_cache((char*)tramp_1f, (char*)tramp_1f + 28);
            printf("[PreLoad] Installed hook on 0x1f816a!\n");
        }
    }

    // 3. Install hook on mono_class_from_mono_type (13 bytes)
    void *target = dlsym(mono, "mono_class_from_mono_type");
    if (target) {
        uint8_t *tramp = (uint8_t*)mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (tramp != MAP_FAILED) {
            memcpy(tramp, target, 13);
            uint8_t *tramp_jmp = tramp + 13;
            uintptr_t ret_addr = (uintptr_t)target + 13;
            tramp_jmp[0] = 0x49; tramp_jmp[1] = 0xbb;
            memcpy(&tramp_jmp[2], &ret_addr, 8);
            tramp_jmp[10] = 0x41; tramp_jmp[11] = 0xff; tramp_jmp[12] = 0xe3;
            g_real_mono_class_from_mono_type = (mono_class_from_mono_type_t)tramp;

            uint8_t patch[13];
            patch[0] = 0x49; patch[1] = 0xbb;
            uintptr_t hook_addr = (uintptr_t)hook_mono_class_from_mono_type;
            memcpy(&patch[2], &hook_addr, 8);
            patch[10] = 0x41; patch[11] = 0xff; patch[12] = 0xe3;
            patch_code(target, patch, 13);
            __builtin___clear_cache((char*)tramp, (char*)tramp + 25);
            printf("[PreLoad] mono_class_from_mono_type hook installed on %p!\n", target);
        }
    }
}

typedef void* (*mono_domain_get_t)(void);
typedef void* (*mono_domain_assembly_open_t)(void *domain, const char *name);
typedef void* (*mono_assembly_get_image_t)(void *assembly);
typedef void* (*mono_class_from_name_t)(void *image, const char* name_space, const char *name);
typedef void* (*mono_class_get_method_from_name_t)(void *klass, const char *name, int param_count);
typedef void* (*mono_compile_method_t)(void *method);
typedef void (*mono_add_internal_call_t)(const char *name, const void *method);
typedef void* (*mono_runtime_invoke_t)(void *method, void *obj, void **params, void **exc);
typedef void* (*mono_assembly_get_object_t)(void *domain, void *assembly);
typedef void* (*mono_object_to_string_t)(void *obj, void **exc);
typedef char* (*mono_string_to_utf8_t)(void *mono_str);
typedef void* (*mono_string_new_t)(void *domain, const char *text);

static void *g_mono = NULL;
static mono_domain_get_t g_mono_domain_get = NULL;
static mono_assembly_get_object_t g_mono_assembly_get_object = NULL;
static mono_string_new_t g_mono_string_new = NULL;

static int melon_is_game_32bit(void) {
    return 0; // x86_64 Linux Native
}

static void* melon_get_lib_ptr(void) {
    return g_mono;
}

static void* melon_get_root_domain_ptr(void) {
    return g_mono_domain_get ? g_mono_domain_get() : NULL;
}

static void* melon_cast_managed_assembly_ptr(void *assembly) {
    if (!assembly || !g_mono_assembly_get_object || !g_mono_domain_get) return NULL;
    return g_mono_assembly_get_object(g_mono_domain_get(), assembly);
}

static void melon_install_hooks(void) {
    printf("[MelonLoader:Native] AssemblyManager::InstallHooks called.\n");
}

static void melon_native_hook_attach(void **target, void *detour) {
    printf("[MelonLoader:Native] NativeHookAttach called (%p -> %p)\n", target ? *target : NULL, detour);
}

static void melon_native_hook_detach(void **target, void *detour) {
    printf("[MelonLoader:Native] NativeHookDetach called\n");
}

static void* melon_get_managed_directory(void) {
    if (!g_mono_string_new || !g_mono_domain_get) return NULL;
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        char managed_path[2048];
        snprintf(managed_path, sizeof(managed_path), "%s/Sinmai_Data/Managed", cwd);
        return g_mono_string_new(g_mono_domain_get(), managed_path);
    }
    return g_mono_string_new(g_mono_domain_get(), "Sinmai_Data/Managed");
}



typedef void* (*mono_image_loaded_t)(const char *name);

static void log_unity_class_parents(void *mono, const char *stage) {
    mono_image_loaded_t p_mono_image_loaded = (mono_image_loaded_t)dlsym(mono, "mono_image_loaded");
    mono_class_from_name_t p_mono_class_from_name = (mono_class_from_name_t)dlsym(mono, "mono_class_from_name");
    if (!p_mono_image_loaded || !p_mono_class_from_name) return;
    void *img = p_mono_image_loaded("UnityEngine.CoreModule");
    if (!img) {
        printf("[PreLoad][%s] UnityEngine.CoreModule not loaded yet\n", stage);
        return;
    }
    const char *names[] = {"Color", "Vector2", "Vector3", "Rect", "Object", "MonoBehaviour"};
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
        void *k = p_mono_class_from_name(img, "UnityEngine", names[i]);
        if (!k) {
            printf("[PreLoad][%s] UnityEngine.%s = NULL\n", stage, names[i]);
            continue;
        }
        uint64_t parent = 0;
        memcpy(&parent, (const uint8_t *)k + 0x28, 8);
        printf("[PreLoad][%s] UnityEngine.%s @ %p parent=0x%llx (+0x2d=0x%02x)\n",
               stage, names[i], k, (unsigned long long)parent,
               (unsigned)((const uint8_t *)k)[0x2d]);
    }
}

void datasec_preload(void) {
    install_crash_handler();  // chains to Mono handler -> full managed stack in stdout
    printf("[Linux Native Stub] === NEW RUN === datasec_preload() called!\n");

    // Find libmonobdwgc path from /proc/self/maps
    char line[1024];
    char mono_path[1024] = {0};
    FILE *f = fopen("/proc/self/maps", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "libmonobdwgc-2.0.so")) {
                char *p = strchr(line, '/');
                if (p) {
                    char *nl = strchr(p, '\n');
                    if (nl) *nl = 0;
                    strncpy(mono_path, p, sizeof(mono_path) - 1);
                    break;
                }
            }
        }
        fclose(f);
    }

    void *mono = dlopen(mono_path[0] ? mono_path : "libmonobdwgc-2.0.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!mono) mono = dlopen("libmonobdwgc-2.0.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!mono) {
        printf("[PreLoad] ERROR: Failed to dlopen libmonobdwgc-2.0.so!\n");
        return;
    }
    g_mono = mono;
    // install_mono_type_sanitizer(mono);  // reverted: wrong diagnosis, broke GetBaseType

    mono_domain_get_t mono_domain_get = (mono_domain_get_t)dlsym(mono, "mono_domain_get");
    mono_domain_assembly_open_t mono_domain_assembly_open = (mono_domain_assembly_open_t)dlsym(mono, "mono_domain_assembly_open");
    mono_assembly_get_image_t mono_assembly_get_image = (mono_assembly_get_image_t)dlsym(mono, "mono_assembly_get_image");
    mono_class_from_name_t mono_class_from_name = (mono_class_from_name_t)dlsym(mono, "mono_class_from_name");
    mono_class_get_method_from_name_t mono_class_get_method_from_name = (mono_class_get_method_from_name_t)dlsym(mono, "mono_class_get_method_from_name");
    mono_compile_method_t mono_compile_method = (mono_compile_method_t)dlsym(mono, "mono_compile_method");
    mono_add_internal_call_t mono_add_internal_call = (mono_add_internal_call_t)dlsym(mono, "mono_add_internal_call");
    mono_runtime_invoke_t mono_runtime_invoke = (mono_runtime_invoke_t)dlsym(mono, "mono_runtime_invoke");
    mono_assembly_get_object_t mono_assembly_get_object = (mono_assembly_get_object_t)dlsym(mono, "mono_assembly_get_object");
    mono_object_to_string_t mono_object_to_string = (mono_object_to_string_t)dlsym(mono, "mono_object_to_string");
    mono_string_to_utf8_t mono_string_to_utf8 = (mono_string_to_utf8_t)dlsym(mono, "mono_string_to_utf8");
    mono_string_new_t mono_string_new = (mono_string_new_t)dlsym(mono, "mono_string_new");

    g_mono_domain_get = mono_domain_get;
    g_mono_assembly_get_object = mono_assembly_get_object;
    g_mono_string_new = mono_string_new;

    if (!mono_domain_get || !mono_domain_assembly_open) {
        printf("[PreLoad] ERROR: mono symbols missing!\n");
        return;
    }
    void *domain = mono_domain_get();
    if (!domain) {
        printf("[PreLoad] ERROR: mono_domain_get returned NULL!\n");
        return;
    }
    log_unity_class_parents(mono, "entry");

    // 1. Neutralize IniFile.clear()
    void *assembly = mono_domain_assembly_open(domain, "Assembly-CSharp");
    if (!assembly) assembly = mono_domain_assembly_open(domain, "Sinmai_Data/Managed/Assembly-CSharp.dll");
    if (assembly && mono_assembly_get_image && mono_class_from_name && mono_class_get_method_from_name && mono_compile_method) {
        void *image = mono_assembly_get_image(assembly);
        if (image) {
            void *ini_class = mono_class_from_name(image, "MAI2System", "IniFile");
            if (ini_class) {
                void *clear_method = mono_class_get_method_from_name(ini_class, "clear", 0);
                if (clear_method) {
                    void *native_code = mono_compile_method(clear_method);
                    if (native_code) {
                        uint8_t ret_opcode = 0xC3;
                        patch_code(native_code, &ret_opcode, 1);
                        printf("[PreLoad] *** IniFile.clear() successfully neutralized with RET! ***\n");
                    }
                }
            }
        }
    }

    // 2. Register MelonLoader native internal calls
    if (mono_add_internal_call) {
        printf("[PreLoad] Registering MelonLoader internal calls...\n");
        mono_add_internal_call("MelonLoader.MelonUtils::IsGame32Bit", (const void*)melon_is_game_32bit);
        mono_add_internal_call("MelonLoader.BootstrapInterop::NativeHookAttach", (const void*)melon_native_hook_attach);
        mono_add_internal_call("MelonLoader.BootstrapInterop::NativeHookDetach", (const void*)melon_native_hook_detach);
        mono_add_internal_call("MelonLoader.MonoInternals.MonoLibrary::GetLibPtr", (const void*)melon_get_lib_ptr);
        mono_add_internal_call("MelonLoader.MonoInternals.MonoLibrary::GetRootDomainPtr", (const void*)melon_get_root_domain_ptr);
        mono_add_internal_call("MelonLoader.MonoInternals.MonoLibrary::CastManagedAssemblyPtr", (const void*)melon_cast_managed_assembly_ptr);
        mono_add_internal_call("MelonLoader.MonoInternals.ResolveInternals.AssemblyManager::InstallHooks", (const void*)melon_install_hooks);
        mono_add_internal_call("MelonLoader.Support.Preload::GetManagedDirectory", (const void*)melon_get_managed_directory);
    }

    // 3. Preload support assemblies
    printf("[PreLoad] Preloading MelonLoader dependencies...\n");
    mono_domain_assembly_open(domain, "MelonLoader/net35/0Harmony.dll");
    mono_domain_assembly_open(domain, "MelonLoader/net35/MonoMod.Utils.dll");
    mono_domain_assembly_open(domain, "MelonLoader/net35/MonoMod.RuntimeDetour.dll");
    mono_domain_assembly_open(domain, "MelonLoader/net35/Tomlet.dll");

    // 4. Open MelonLoader/net35/MelonLoader.dll
    void *melon_assembly = mono_domain_assembly_open(domain, "MelonLoader/net35/MelonLoader.dll");
    if (!melon_assembly) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd))) {
            char melon_path[2048];
            snprintf(melon_path, sizeof(melon_path), "%s/MelonLoader/net35/MelonLoader.dll", cwd);
            melon_assembly = mono_domain_assembly_open(domain, melon_path);
        }
    }

    if (!melon_assembly) {
        printf("[PreLoad] Failed to open MelonLoader/net35/MelonLoader.dll!\n");
        return;
    }
    printf("[PreLoad] MelonLoader/net35/MelonLoader.dll opened successfully!\n");

    void *melon_image = mono_assembly_get_image(melon_assembly);
    if (!melon_image) {
        printf("[PreLoad] Failed to get image for MelonLoader.dll!\n");
        return;
    }

    void *core_class = mono_class_from_name(melon_image, "MelonLoader", "Core");
    if (!core_class) {
        printf("[PreLoad] Failed to locate class MelonLoader.Core!\n");
        return;
    }
    printf("[PreLoad] Found MelonLoader.Core class at %p\n", core_class);

    void *init_method = mono_class_get_method_from_name(core_class, "Initialize", 0);
    void *prestart_method = mono_class_get_method_from_name(core_class, "PreStart", 0);
    void *start_method = mono_class_get_method_from_name(core_class, "Start", 0);

    void *exc = NULL;
    if (init_method && mono_runtime_invoke) {
        printf("[PreLoad] Invoking MelonLoader.Core.Initialize()...\n");
        mono_runtime_invoke(init_method, NULL, NULL, &exc);
        if (exc && mono_object_to_string && mono_string_to_utf8) {
            void *msg_obj = mono_object_to_string(exc, NULL);
            char *msg = mono_string_to_utf8(msg_obj);
            printf("[PreLoad] Core.Initialize() threw exception: %s\n", msg ? msg : "unknown");
            exc = NULL;
        } else {
            printf("[PreLoad] Core.Initialize() succeeded!\n");
        }
    }
    log_unity_class_parents(mono, "after_Initialize");

    if (prestart_method && mono_runtime_invoke) {
        printf("[PreLoad] Invoking MelonLoader.Core.PreStart()...\n");
        mono_runtime_invoke(prestart_method, NULL, NULL, &exc);
        if (exc && mono_object_to_string && mono_string_to_utf8) {
            void *msg_obj = mono_object_to_string(exc, NULL);
            char *msg = mono_string_to_utf8(msg_obj);
            printf("[PreLoad] Core.PreStart() threw exception: %s\n", msg ? msg : "unknown");
            exc = NULL;
        } else {
            printf("[PreLoad] Core.PreStart() succeeded!\n");
        }
    }
    log_unity_class_parents(mono, "after_PreStart");

    if (start_method && mono_runtime_invoke) {
        printf("[PreLoad] Invoking MelonLoader.Core.Start()...\n");
        mono_runtime_invoke(start_method, NULL, NULL, &exc);
        if (exc && mono_object_to_string && mono_string_to_utf8) {
            void *msg_obj = mono_object_to_string(exc, NULL);
            char *msg = mono_string_to_utf8(msg_obj);
            printf("[PreLoad] Core.Start() threw exception: %s\n", msg ? msg : "unknown");
            exc = NULL;
        } else {
            printf("[PreLoad] Core.Start() succeeded!\n");
        }
    }

    printf("[PreLoad] MelonLoader native bootstrap completed.\n");
}
