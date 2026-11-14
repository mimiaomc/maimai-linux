#include <stdarg.h>
#include <stdio.h>
static int stub_log(const char *fmt, ...) {
    FILE *f = fopen("/tmp/amd_stub.log", "a");
    if (!f) return 0;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fclose(f);
    return 0;
}
#define printf(...) stub_log(__VA_ARGS__)
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dlfcn.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <poll.h>
#include <sys/wait.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static uint64_t dummy_struct[4096] = {0};

/* ---- Credit & Coin Configuration + Unity Input Polling ---- */
static uint32_t g_coins = 0;
static uint32_t g_coins_per_credit = 1;
static uint32_t g_total_coins = 0;
static uint32_t g_total_credits = 0;
static int g_free_play = 0;
static int32_t g_coin_keycode = 53; /* UnityEngine.KeyCode.Alpha5 ('5') */
static char g_coin_key_name[64] = "5";
static char g_coin_config_path[PATH_MAX] = "";
static int g_coin_initialized = 0;
static uint32_t g_pending_coin_sound = 0; /* 0 = none, 1 = Coin, 2 = Credit */
static int32_t g_coin_player_indices[2] = {0, 0};
static uint16_t g_credit_str_u16[32] = {'0', 0};

static char *trim_ascii(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

static int32_t parse_unity_keycode(const char *s) {
    if (!s || !*s) return 53;
    if (strcasecmp(s, "none") == 0 || strcmp(s, "0") == 0) return 0;
    if (strlen(s) == 1) {
        unsigned char c = (unsigned char)s[0];
        if (c >= '0' && c <= '9') return 48 + (c - '0');
        c = (unsigned char)tolower(c);
        if (c >= 'a' && c <= 'z') return 97 + (c - 'a');
    }
    if (strncasecmp(s, "alpha", 5) == 0 && s[5] >= '0' && s[5] <= '9' && s[6] == '\0') {
        return 48 + (s[5] - '0');
    }
    if (strncasecmp(s, "keypad", 6) == 0 && s[6] >= '0' && s[6] <= '9' && s[7] == '\0') {
        return 256 + (s[6] - '0');
    }
    if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1])) {
        int fn = atoi(s + 1);
        if (fn >= 1 && fn <= 12) return 281 + fn;
    }
    if (strcasecmp(s, "space") == 0) return 32;
    if (strcasecmp(s, "return") == 0 || strcasecmp(s, "enter") == 0) return 13;
    if (strcasecmp(s, "insert") == 0 || strcasecmp(s, "ins") == 0) return 277;
    if (strcasecmp(s, "tab") == 0) return 9;
    if (strcasecmp(s, "backspace") == 0) return 8;
    if (strcasecmp(s, "keypadenter") == 0) return 271;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end && *end == '\0' && v > 0) return (int32_t)v;
    return 53;
}

static void resolve_exe_dir_file(char *out, size_t out_sz, const char *filename) {
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len > 0) {
        exe[len] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            size_t dlen = strlen(exe);
            size_t flen = strlen(filename);
            if (dlen + 1 + flen + 1 <= out_sz) {
                memcpy(out, exe, dlen);
                out[dlen] = '/';
                memcpy(out + dlen + 1, filename, flen + 1);
                return;
            }
        }
    }
    snprintf(out, out_sz, "%s", filename);
}

static void resolve_coin_config_path(void) {
    if (g_coin_config_path[0] != '\0') return;
    resolve_exe_dir_file(g_coin_config_path, sizeof(g_coin_config_path), "coin_config.ini");
}

static void save_coin_config(void) {
    resolve_coin_config_path();
    char tmp_path[PATH_MAX + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_coin_config_path);
    FILE *f = fopen(tmp_path, "w");
    if (!f) f = fopen(g_coin_config_path, "w");
    else {
        fprintf(f,
            "# maimai DX Linux 投币与点数配置文件\n"
            "# 当前持有的币数（按投币键会自动累加并保存，扣点会自动扣除）\n"
            "coins = %u\n\n"
            "# 投多少币兑换 1 点数 (Credit)\n"
            "coins_per_credit = %u\n\n"
            "# 投币按键（仅在游戏窗口聚焦时生效；支持 0-9 / A-Z / F1-F12 / Insert / Space / Return / Keypad0-9 / None）\n"
            "coin_key = %s\n\n"
            "# 是否开启免费游玩模式 (true / false)\n"
            "free_play = %s\n",
            g_coins, g_coins_per_credit, g_coin_key_name, g_free_play ? "true" : "false");
        fclose(f);
        if (rename(tmp_path, g_coin_config_path) != 0) {
            remove(tmp_path);
        }
        return;
    }
    if (!f) return;
    fprintf(f, "coins = %u\ncoins_per_credit = %u\ncoin_key = %s\nfree_play = %s\n",
            g_coins, g_coins_per_credit, g_coin_key_name, g_free_play ? "true" : "false");
    fclose(f);
}

static void load_coin_config(void) {
    resolve_coin_config_path();
    FILE *f = fopen(g_coin_config_path, "r");
    if (!f) {
        save_coin_config();
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *semi = strchr(line, ';');
        if (semi) *semi = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim_ascii(line);
        char *v = trim_ascii(eq + 1);
        if (strcmp(k, "coins") == 0) {
            long val = strtol(v, NULL, 10);
            g_coins = val > 0 ? (uint32_t)val : 0;
        } else if (strcmp(k, "coins_per_credit") == 0) {
            long val = strtol(v, NULL, 10);
            g_coins_per_credit = val >= 1 ? (uint32_t)val : 1;
        } else if (strcmp(k, "coin_key") == 0) {
            snprintf(g_coin_key_name, sizeof(g_coin_key_name), "%s", v);
            g_coin_keycode = parse_unity_keycode(v);
        } else if (strcmp(k, "free_play") == 0) {
            g_free_play = (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0 ||
                           strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0);
        }
    }
    fclose(f);
}

static void init_coin_system_once(void) {
    if (g_coin_initialized) return;
    g_coin_initialized = 1;
    load_coin_config();
    g_total_coins = g_coins;
    g_total_credits = g_coins / g_coins_per_credit;
    stub_log("[LinuxStub:amdaemon_api] coin_config=%s coins=%u coins_per_credit=%u coin_key=%s(%d) free_play=%d\n",
             g_coin_config_path, g_coins, g_coins_per_credit, g_coin_key_name, g_coin_keycode, g_free_play);
}

static void insert_one_coin(void) {
    init_coin_system_once();
    uint32_t prev_credits = g_coins / g_coins_per_credit;
    if (g_coins < 9999) g_coins++;
    if (g_total_coins < 0xFFFFFFFFu) g_total_coins++;
    uint32_t new_credits = g_coins / g_coins_per_credit;
    if (new_credits > prev_credits) {
        if (g_total_credits < 0xFFFFFFFFu) g_total_credits++;
        g_pending_coin_sound = 2; /* Credit */
    } else {
        g_pending_coin_sound = 1; /* Coin */
    }
    save_coin_config();
    stub_log("[LinuxStub:amdaemon_api] Coin inserted -> coins=%u credit=%u remain=%u\n",
             g_coins, g_coins / g_coins_per_credit, g_coins % g_coins_per_credit);
}

static void poll_coin_key(void) {
    if (g_coin_keycode <= 0) return;

    typedef void *(*fn_mono_domain_get_t)(void);
    typedef void *(*fn_mono_image_loaded_t)(const char *name);
    typedef void *(*fn_mono_domain_assembly_open_t)(void *domain, const char *name);
    typedef void *(*fn_mono_assembly_get_image_t)(void *assembly);
    typedef void *(*fn_mono_class_from_name_t)(void *image, const char *ns, const char *name);
    typedef void *(*fn_mono_class_get_method_from_name_t)(void *klass, const char *name, int param_count);
    typedef void *(*fn_mono_runtime_invoke_t)(void *method, void *obj, void **params, void **exc);
    typedef void *(*fn_mono_object_unbox_t)(void *obj);

    static int mono_resolved = 0;
    static fn_mono_domain_get_t p_domain_get = NULL;
    static fn_mono_image_loaded_t p_image_loaded = NULL;
    static fn_mono_domain_assembly_open_t p_asm_open = NULL;
    static fn_mono_assembly_get_image_t p_asm_get_image = NULL;
    static fn_mono_class_from_name_t p_class_from_name = NULL;
    static fn_mono_class_get_method_from_name_t p_get_method = NULL;
    static fn_mono_runtime_invoke_t p_invoke = NULL;
    static fn_mono_object_unbox_t p_unbox = NULL;
    static void *s_get_keydown_method = NULL;

    if (!mono_resolved) {
        mono_resolved = 1;
        p_domain_get = (fn_mono_domain_get_t)dlsym(RTLD_DEFAULT, "mono_domain_get");
        p_image_loaded = (fn_mono_image_loaded_t)dlsym(RTLD_DEFAULT, "mono_image_loaded");
        p_asm_open = (fn_mono_domain_assembly_open_t)dlsym(RTLD_DEFAULT, "mono_domain_assembly_open");
        p_asm_get_image = (fn_mono_assembly_get_image_t)dlsym(RTLD_DEFAULT, "mono_assembly_get_image");
        p_class_from_name = (fn_mono_class_from_name_t)dlsym(RTLD_DEFAULT, "mono_class_from_name");
        p_get_method = (fn_mono_class_get_method_from_name_t)dlsym(RTLD_DEFAULT, "mono_class_get_method_from_name");
        p_invoke = (fn_mono_runtime_invoke_t)dlsym(RTLD_DEFAULT, "mono_runtime_invoke");
        p_unbox = (fn_mono_object_unbox_t)dlsym(RTLD_DEFAULT, "mono_object_unbox");
    }

    if (!p_domain_get || !p_image_loaded || !p_class_from_name || !p_get_method || !p_invoke || !p_unbox) {
        return;
    }

    if (!s_get_keydown_method) {
        void *dom = p_domain_get();
        if (!dom) return;
        void *img = p_image_loaded("UnityEngine.CoreModule");
        if (!img && p_asm_open && p_asm_get_image) {
            void *asm_ptr = p_asm_open(dom, "Sinmai_Data/Managed/UnityEngine.CoreModule.dll");
            if (asm_ptr) img = p_asm_get_image(asm_ptr);
        }
        if (!img) return;
        void *cls = p_class_from_name(img, "UnityEngine", "Input");
        if (!cls) return;
        s_get_keydown_method = p_get_method(cls, "GetKeyDownInt", 1);
        if (!s_get_keydown_method) return;
    }

    int32_t kc = g_coin_keycode;
    void *args[1] = { &kc };
    void *exc = NULL;
    void *ret = p_invoke(s_get_keydown_method, NULL, args, &exc);
    if (exc || !ret) return;
    uint8_t pressed = *(uint8_t *)p_unbox(ret);
    if (pressed) {
        insert_one_coin();
    }
}

static uint32_t game_cost_for_index(int32_t index) {
    /* Index 0: 1Credit (1), Index 1: 2Credit (2), Index 2: Freedom (2) */
    if (index == 1 || index == 2) return 2;
    return 1;
}
static uint16_t u16_ip[64] = u"192.168.0.100";
static uint16_t u16_mask[64] = u"255.255.255.0";
static uint16_t u16_gw[64] = u"192.168.0.1";
static uint16_t u16_dns1[64] = u"223.5.5.5";
static uint16_t u16_dns2[64] = u"8.8.8.8";
static uint16_t u16_gameid[16] = u"SDGB";
static uint16_t u16_keychip[32] = u"A63E-01E00000000";
static uint16_t u16_short_keychip[32] = u"A63E01E0000";
static uint16_t u16_board[32] = u"A63E-01E00000000";
static uint16_t u16_short_board[32] = u"A63E01E0000";
static uint16_t u16_option_dir[32] = u"option";

static uint16_t u16_empty[2] = u"";

static char g_real_ip[64] = "192.168.0.100";

/* ---- ALL.Net Server & Cabinet Configuration + Auth Client ---- */
static uint64_t g_keychip_id_handle = 1;
static uint64_t g_board_id_handle = 2;

static char g_net_config_path[PATH_MAX] = "";
static char g_net_server[256] = "at.sys-allnet.cn";
static char g_net_protocol[32] = "auto";
static char g_keychip_str[32] = "A63E-01E00000000";
static char g_short_keychip_str[32] = "A63E01E0000";
static char g_board_str[32] = "A63E-01E00000000";
static char g_short_board_str[32] = "A63E01E0000";
static char g_game_id_str[16] = "SDGB";
static char g_title_ver_str[16] = "1.55";
static uint32_t g_app_version_u32 = (1u << 16) | (55u << 8) | 0u;
static int32_t g_region_code = 2; /* 1=Japan, 2=China, 3=Export */
static int g_ssl_verify = 0;
static int g_net_timeout_sec = 5;

static int g_net_initialized = 0;
static volatile int g_auth_in_progress = 0;
static volatile int g_auth_good = 0;
static volatile int g_wan_available = 0;
static volatile uint64_t g_auth_time_us = 0;

static uint32_t g_auth_location_id = 1;
static int32_t g_auth_region_code = 2;
static int32_t g_auth_line_type = 1; /* LineType.Broadband */
static uint16_t u16_auth_uri[512] = {0};
static uint16_t u16_auth_host[256] = {0};
static uint16_t u16_auth_name[128] = u"SEGA";
static uint16_t u16_auth_nickname[128] = {0};
static uint16_t u16_auth_country[16] = u"CHN";
static uint16_t u16_auth_region_names[4][128] = {{0}};
static uint16_t u16_ver_str[32] = u"1.55";

static pthread_mutex_t g_auth_mutex = PTHREAD_MUTEX_INITIALIZER;

static void utf8_to_u16(const char *src, uint16_t *dst, size_t max_u16) {
    if (!dst || max_u16 == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t i = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p && i + 1 < max_u16) {
        uint32_t cp = 0;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            p += 4;
            if (cp >= 0x10000 && i + 2 < max_u16) {
                cp -= 0x10000;
                dst[i++] = (uint16_t)(0xD800 + (cp >> 10));
                dst[i++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
                continue;
            }
            cp = '?';
        } else {
            cp = '?';
            p++;
        }
        dst[i++] = (uint16_t)(cp <= 0xFFFF ? cp : '?');
    }
    dst[i] = 0;
}

static void normalize_serial(const char *in, char *std_out, size_t std_sz, char *short_out, size_t short_sz) {
    char clean[32];
    size_t ci = 0;
    for (size_t i = 0; in[i] && ci + 1 < sizeof(clean); i++) {
        if (isalnum((unsigned char)in[i])) {
            clean[ci++] = (char)toupper((unsigned char)in[i]);
        }
    }
    clean[ci] = '\0';

    if (short_out && short_sz > 0) {
        size_t slen = ci < 11 ? ci : 11;
        if (slen >= short_sz) slen = short_sz - 1;
        memcpy(short_out, clean, slen);
        short_out[slen] = '\0';
    }

    if (std_out && std_sz > 0) {
        if (strchr(in, '-') != NULL || ci <= 11) {
            snprintf(std_out, std_sz, "%s", in);
        } else {
            char head[5] = {0}, tail[16] = {0};
            memcpy(head, clean, ci >= 4 ? 4 : ci);
            if (ci > 4) memcpy(tail, clean + 4, (ci - 4) < 15 ? (ci - 4) : 15);
            snprintf(std_out, std_sz, "%s-%s", head, tail);
        }
    }
}

static uint32_t parse_version_str(const char *ver) {
    unsigned maj = 1, min = 55, pat = 0;
    if (ver && *ver) {
        sscanf(ver, "%u.%u.%u", &maj, &min, &pat);
    }
    if (maj > 65535) maj = 65535;
    if (min > 255) min = 255;
    if (pat > 255) pat = 255;
    return ((uint32_t)maj << 16) | ((uint32_t)min << 8) | (uint32_t)pat;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static void url_decode(char *s, int plus_as_space) {
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '%' && hex_val(r[1]) >= 0 && hex_val(r[2]) >= 0) {
            *w++ = (char)((hex_val(r[1]) << 4) | hex_val(r[2]));
            r += 3;
        } else if (plus_as_space && *r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static char *base64_encode(const uint8_t *in, size_t len, size_t *out_len) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t cap = ((len + 2) / 3) * 4 + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t a = in[i++];
        uint32_t b = (i < len) ? in[i++] : 0;
        uint32_t c = (i < len) ? in[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        size_t rem = (i <= len) ? 0 : 0;
        (void)rem;
        out[j++] = tbl[(triple >> 18) & 0x3F];
        out[j++] = tbl[(triple >> 12) & 0x3F];
        out[j++] = tbl[(triple >> 6) & 0x3F];
        out[j++] = tbl[triple & 0x3F];
    }
    size_t mod = len % 3;
    if (mod == 1) { out[j - 1] = '='; out[j - 2] = '='; }
    else if (mod == 2) { out[j - 1] = '='; }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}

static uint8_t *base64_decode(const char *in, size_t len, size_t *out_len) {
    uint8_t *out = (uint8_t *)malloc((len / 4 + 1) * 3 + 4);
    if (!out) return NULL;
    uint32_t buf = 0;
    int bits = 0;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        int v = -1;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else continue;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[j++] = (uint8_t)((buf >> bits) & 0xFF);
        }
    }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} curl_buf_t;

static int append_buf(curl_buf_t *b, const void *ptr, size_t add) {
    if (!b || add == 0) return 0;
    if (b->len + add > 262144) return -1;
    if (b->len + add + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        while (ncap < b->len + add + 1) ncap *= 2;
        uint8_t *nd = (uint8_t *)realloc(b->data, ncap);
        if (!nd) return -1;
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, ptr, add);
    b->len += add;
    b->data[b->len] = '\0';
    return 0;
}

static int http_post_bytes(const char *url, const char *user_agent, const char *extra_header,
                           const void *body, size_t body_len, long *http_code, curl_buf_t *resp) {
    if (http_code) *http_code = 0;

    /* Run /usr/bin/curl out-of-process so host OpenSSL 3 never collides with Sinmai.x86_64's embedded OpenSSL symbols. */
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        for (int i = 0; i < 2; i++) {
            if (in_pipe[i] >= 0) close(in_pipe[i]);
            if (out_pipe[i] >= 0) close(out_pipe[i]);
            if (err_pipe[i] >= 0) close(err_pipe[i]);
        }
        return -1;
    }

    char timeout_buf[16];
    snprintf(timeout_buf, sizeof(timeout_buf), "%d", g_net_timeout_sec > 0 ? g_net_timeout_sec : 5);

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);

        unsetenv("LD_PRELOAD");
        unsetenv("LD_LIBRARY_PATH");

        const char *argv[32];
        int ac = 0;
        argv[ac++] = "curl";
        argv[ac++] = "-sS";
        argv[ac++] = "-L";
        argv[ac++] = "-X";
        argv[ac++] = "POST";
        argv[ac++] = "--connect-timeout";
        argv[ac++] = timeout_buf;
        argv[ac++] = "--max-time";
        argv[ac++] = timeout_buf;
        argv[ac++] = "-H";
        argv[ac++] = "Content-Type: application/x-www-form-urlencoded";
        argv[ac++] = "-H";
        argv[ac++] = "Expect:";
        if (extra_header && *extra_header) {
            argv[ac++] = "-H";
            argv[ac++] = extra_header;
        }
        if (user_agent && *user_agent) {
            argv[ac++] = "-A";
            argv[ac++] = user_agent;
        }
        if (!g_ssl_verify) {
            argv[ac++] = "-k";
        }
        argv[ac++] = "--data-binary";
        argv[ac++] = "@-";
        argv[ac++] = "-w";
        argv[ac++] = "%{http_code}";
        argv[ac++] = url;
        argv[ac++] = NULL;

        execv("/usr/bin/curl", (char *const *)argv);
        execvp("curl", (char *const *)argv);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);

    size_t sent = 0;
    while (sent < body_len) {
        ssize_t w = write(in_pipe[1], (const uint8_t *)body + sent, body_len - sent);
        if (w > 0) {
            sent += (size_t)w;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(in_pipe[1]);

    char err_msg[512] = "";
    size_t err_len = 0;
    int out_open = 1, err_open = 1;
    while (out_open || err_open) {
        struct pollfd pfds[2];
        nfds_t nfds = 0;
        if (out_open) {
            pfds[nfds].fd = out_pipe[0];
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (err_open) {
            pfds[nfds].fd = err_pipe[0];
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        int pr = poll(pfds, nfds, (g_net_timeout_sec + 5) * 1000);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) break;

        for (nfds_t i = 0; i < nfds; i++) {
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                uint8_t tmp[4096];
                ssize_t r = read(pfds[i].fd, tmp, sizeof(tmp));
                if (r > 0) {
                    if (pfds[i].fd == out_pipe[0]) {
                        append_buf(resp, tmp, (size_t)r);
                    } else {
                        size_t copy = sizeof(err_msg) - 1 - err_len;
                        if ((size_t)r < copy) copy = (size_t)r;
                        if (copy > 0) {
                            memcpy(err_msg + err_len, tmp, copy);
                            err_len += copy;
                            err_msg[err_len] = '\0';
                        }
                    }
                } else if (r == 0 || (r < 0 && errno != EINTR)) {
                    if (pfds[i].fd == out_pipe[0]) out_open = 0;
                    else err_open = 0;
                }
            }
        }
    }
    close(out_pipe[0]);
    close(err_pipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    int exit_rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (resp && resp->data && resp->len >= 3) {
        uint8_t *tail = resp->data + resp->len - 3;
        if (isdigit(tail[0]) && isdigit(tail[1]) && isdigit(tail[2])) {
            char cbuf[4] = {(char)tail[0], (char)tail[1], (char)tail[2], '\0'};
            if (http_code) *http_code = strtol(cbuf, NULL, 10);
            resp->len -= 3;
            resp->data[resp->len] = '\0';
        }
    }

    if (exit_rc != 0) {
        while (err_len > 0 && (err_msg[err_len - 1] == '\n' || err_msg[err_len - 1] == '\r')) {
            err_msg[--err_len] = '\0';
        }
        if (err_msg[0]) {
            stub_log("[LinuxStub:amdaemon_api] curl(rc=%d): %s\n", exit_rc, err_msg);
        }
    }
    return exit_rc;
}

/* ---- Self-contained FIPS-197 AES-128-CBC (zero libcrypto dependency) ---- */
static const uint8_t AES_SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t AES_RSBOX[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static const uint8_t AES_RCON[11] = {
    0x8d,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

static uint8_t aes_xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ (((x >> 7) & 1u) * 0x1bu));
}

static uint8_t aes_gf_mul(uint8_t x, uint8_t y) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (y & 1u) r ^= x;
        x = aes_xtime(x);
        y >>= 1;
    }
    return r;
}

static void aes128_key_expand(const uint8_t key[16], uint8_t rk[176]) {
    memcpy(rk, key, 16);
    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        memcpy(t, rk + (i - 1) * 4, 4);
        if ((i % 4) == 0) {
            uint8_t u = t[0];
            t[0] = (uint8_t)(AES_SBOX[t[1]] ^ AES_RCON[i / 4]);
            t[1] = AES_SBOX[t[2]];
            t[2] = AES_SBOX[t[3]];
            t[3] = AES_SBOX[u];
        }
        for (int j = 0; j < 4; j++) {
            rk[i * 4 + j] = (uint8_t)(rk[(i - 4) * 4 + j] ^ t[j]);
        }
    }
}

static void aes_add_round_key(uint8_t s[16], const uint8_t *rk) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
}

static void aes128_encrypt_block(const uint8_t rk[176], uint8_t s[16]) {
    aes_add_round_key(s, rk);
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) s[i] = AES_SBOX[s[i]];
        uint8_t t;
        t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
        t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
        if (r < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = s + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                uint8_t tmp = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                col[0] ^= (uint8_t)(tmp ^ aes_xtime((uint8_t)(a0 ^ a1)));
                col[1] ^= (uint8_t)(tmp ^ aes_xtime((uint8_t)(a1 ^ a2)));
                col[2] ^= (uint8_t)(tmp ^ aes_xtime((uint8_t)(a2 ^ a3)));
                col[3] ^= (uint8_t)(tmp ^ aes_xtime((uint8_t)(a3 ^ a0)));
            }
        }
        aes_add_round_key(s, rk + r * 16);
    }
}

static void aes128_decrypt_block(const uint8_t rk[176], uint8_t s[16]) {
    aes_add_round_key(s, rk + 160);
    for (int r = 9; r >= 0; r--) {
        uint8_t t;
        t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
        t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
        t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
        for (int i = 0; i < 16; i++) s[i] = AES_RSBOX[s[i]];
        aes_add_round_key(s, rk + r * 16);
        if (r > 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = s + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = (uint8_t)(aes_gf_mul(a0, 0x0e) ^ aes_gf_mul(a1, 0x0b) ^ aes_gf_mul(a2, 0x0d) ^ aes_gf_mul(a3, 0x09));
                col[1] = (uint8_t)(aes_gf_mul(a0, 0x09) ^ aes_gf_mul(a1, 0x0e) ^ aes_gf_mul(a2, 0x0b) ^ aes_gf_mul(a3, 0x0d));
                col[2] = (uint8_t)(aes_gf_mul(a0, 0x0d) ^ aes_gf_mul(a1, 0x09) ^ aes_gf_mul(a2, 0x0e) ^ aes_gf_mul(a3, 0x0b));
                col[3] = (uint8_t)(aes_gf_mul(a0, 0x0b) ^ aes_gf_mul(a1, 0x0d) ^ aes_gf_mul(a2, 0x09) ^ aes_gf_mul(a3, 0x0e));
            }
        }
    }
}

static const uint8_t LITE_AUTH_KEY[16] = {
    47, 63, 106, 111, 43, 34, 76, 38, 92, 67, 114, 57, 40, 61, 107, 71
};

static int lite_aes_encrypt(const uint8_t *plain, int plain_len, uint8_t *out, int *out_len) {
    if (!plain || plain_len < 0 || !out || !out_len || plain_len > 992) return -1;

    uint8_t iv[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, iv, 16);
        close(fd);
        if (r != 16) {
            for (int i = 0; i < 16; i++) iv[i] = (uint8_t)(rand() & 0xFF);
        }
    } else {
        for (int i = 0; i < 16; i++) iv[i] = (uint8_t)(rand() & 0xFF);
    }

    uint8_t rk[176];
    aes128_key_expand(LITE_AUTH_KEY, rk);
    memcpy(out, iv, 16);

    int pad = 16 - (plain_len % 16);
    int total_ct = plain_len + pad;
    const uint8_t *prev = out;
    for (int off = 0; off < total_ct; off += 16) {
        uint8_t blk[16];
        for (int j = 0; j < 16; j++) {
            uint8_t b = (off + j < plain_len) ? plain[off + j] : (uint8_t)pad;
            blk[j] = (uint8_t)(b ^ prev[j]);
        }
        aes128_encrypt_block(rk, blk);
        memcpy(out + 16 + off, blk, 16);
        prev = out + 16 + off;
    }
    *out_len = 16 + total_ct;
    return 0;
}

static int lite_aes_decrypt(const uint8_t *in, int in_len, char *plain_out, int max_out) {
    int ct_len = in_len - 16;
    if (!in || !plain_out || ct_len <= 0 || (ct_len % 16) != 0 || ct_len >= max_out) return -1;

    uint8_t rk[176];
    aes128_key_expand(LITE_AUTH_KEY, rk);

    const uint8_t *prev = in;
    for (int off = 0; off < ct_len; off += 16) {
        uint8_t blk[16];
        memcpy(blk, in + 16 + off, 16);
        aes128_decrypt_block(rk, blk);
        for (int j = 0; j < 16; j++) {
            plain_out[off + j] = (char)(blk[j] ^ prev[j]);
        }
        prev = in + 16 + off;
    }

    uint8_t pad = (uint8_t)plain_out[ct_len - 1];
    if (pad == 0 || pad > 16 || pad > ct_len) return -1;
    for (int j = 0; j < pad; j++) {
        if ((uint8_t)plain_out[ct_len - 1 - j] != pad) return -1;
    }
    int pt_len = ct_len - pad;
    plain_out[pt_len] = '\0';
    return pt_len;
}

static void extract_host_from_uri(const char *uri, char *host_out, size_t max_sz) {
    const char *p = strstr(uri, "://");
    p = p ? (p + 3) : uri;
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    if (len >= max_sz) len = max_sz - 1;
    memcpy(host_out, p, len);
    host_out[len] = '\0';
}

static int apply_allnet_response(const char *raw_kv, int is_lite) {
    if (!raw_kv || !*raw_kv) return 0;
    char *copy = strdup(raw_kv);
    if (!copy) return 0;
    size_t clen = strlen(copy);
    while (clen > 0 && (copy[clen - 1] == '\r' || copy[clen - 1] == '\n' || copy[clen - 1] == ' ')) {
        copy[--clen] = '\0';
    }

    int stat_ok = -999;
    char uri[512] = "";
    char host[256] = "";
    char name[256] = "SEGA";
    char nickname[256] = "";
    char country[16] = "CHN";
    char region_names[4][256] = {{0}};
    uint32_t place_id = 1;
    int32_t region0 = g_region_code;

    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, "&", &saveptr); tok; tok = strtok_r(NULL, "&", &saveptr)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = tok;
        char *v = eq + 1;
        int is_uri_field = (strcmp(k, "uri") == 0 || strcmp(k, "uri1") == 0 || strcmp(k, "uri2") == 0);
        url_decode(v, !is_uri_field);

        if ((is_lite && strcmp(k, "result") == 0) || (!is_lite && strcmp(k, "stat") == 0) ||
            strcmp(k, "stat") == 0 || strcmp(k, "result") == 0) {
            stat_ok = atoi(v);
        } else if (strcmp(k, "uri1") == 0 || (strcmp(k, "uri") == 0 && uri[0] == '\0')) {
            snprintf(uri, sizeof(uri), "%s", v);
        } else if ((is_lite && strcmp(k, "uri2") == 0 && v[0] != '\0') || strcmp(k, "host") == 0) {
            /* ALL.Net Lite exposes uri2 as amdaemon's HOST field (encrypted server cert hashes). */
            snprintf(host, sizeof(host), "%s", v);
        } else if (strcmp(k, "place_id") == 0) {
            /* ALL.Net returns place_id as 4-digit hex (e.g. 07b3 -> 0x07b3). */
            unsigned long pid = strtoul(v, NULL, 16);
            if (pid > 0) place_id = (uint32_t)pid;
        } else if (strcmp(k, "name") == 0) {
            if (*v) snprintf(name, sizeof(name), "%s", v);
        } else if (strcmp(k, "nickname") == 0) {
            snprintf(nickname, sizeof(nickname), "%s", v);
        } else if (strcmp(k, "region0") == 0) {
            if (*v) region0 = (int32_t)atoi(v);
        } else if (strcmp(k, "region_name0") == 0) {
            snprintf(region_names[0], sizeof(region_names[0]), "%s", v);
        } else if (strcmp(k, "region_name1") == 0) {
            snprintf(region_names[1], sizeof(region_names[1]), "%s", v);
        } else if (strcmp(k, "region_name2") == 0) {
            snprintf(region_names[2], sizeof(region_names[2]), "%s", v);
        } else if (strcmp(k, "region_name3") == 0) {
            snprintf(region_names[3], sizeof(region_names[3]), "%s", v);
        } else if (strcmp(k, "country") == 0) {
            if (*v) snprintf(country, sizeof(country), "%s", v);
        }
    }
    free(copy);

    if (stat_ok != 1 || uri[0] == '\0') {
        stub_log("[LinuxStub:amdaemon_api] ALL.Net auth rejected (stat/result=%d uri='%s')\n", stat_ok, uri);
        return 0;
    }

    if (host[0] == '\0') {
        extract_host_from_uri(uri, host, sizeof(host));
    }

    pthread_mutex_lock(&g_auth_mutex);
    utf8_to_u16(uri, u16_auth_uri, 512);
    utf8_to_u16(host, u16_auth_host, 256);
    utf8_to_u16(name, u16_auth_name, 128);
    utf8_to_u16(nickname, u16_auth_nickname, 128);
    utf8_to_u16(country, u16_auth_country, 16);
    for (int i = 0; i < 4; i++) {
        utf8_to_u16(region_names[i], u16_auth_region_names[i], 128);
    }
    g_auth_location_id = place_id;
    g_auth_region_code = region0;
    g_auth_line_type = 1;
    g_auth_time_us = (uint64_t)time(NULL) * 1000000ULL;
    __atomic_store_n(&g_auth_good, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wan_available, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_auth_mutex);

    stub_log("[LinuxStub:amdaemon_api] ALL.Net auth OK! uri='%s' host='%s' place_id=%u name='%s'\n",
             uri, host, place_id, name);
    return 1;
}

static void build_endpoint_url(const char *server, const char *path, const char *default_scheme, char *out, size_t out_sz) {
    if (strncasecmp(server, "http://", 7) == 0 || strncasecmp(server, "https://", 8) == 0) {
        if (strstr(server + 8, "/") != NULL) {
            snprintf(out, out_sz, "%s", server);
        } else {
            snprintf(out, out_sz, "%s%s", server, path);
        }
        return;
    }
    const char *slash = strchr(server, '/');
    if (slash) {
        snprintf(out, out_sz, "%s://%s", default_scheme, server);
    } else {
        snprintf(out, out_sz, "%s://%s%s", default_scheme, server, path);
    }
}

static int try_allnet_lite(void) {
    char url[512];
    build_endpoint_url(g_net_server, "/net/initialize", "https", url, sizeof(url));

    uint32_t tok = ((uint32_t)time(NULL) ^ (uint32_t)getpid()) | 1u;
    char plain[512];
    int plen = snprintf(plain, sizeof(plain), "title_id=%s&title_ver=%s&client_id=%s&token=%u\r\n",
                        g_game_id_str, g_title_ver_str, g_short_keychip_str, tok);
    if (plen <= 0 || plen >= (int)sizeof(plain)) return 0;

    uint8_t enc_buf[1024];
    int enc_len = 0;
    if (lite_aes_encrypt((const uint8_t *)plain, plen, enc_buf, &enc_len) != 0) {
        stub_log("[LinuxStub:amdaemon_api] lite AES encrypt failed\n");
        return 0;
    }

    char ua[64];
    snprintf(ua, sizeof(ua), "%s;Windows/Lite", g_game_id_str[0] ? g_game_id_str : "SDGB");

    curl_buf_t resp = {0};
    long http_code = 0;
    stub_log("[LinuxStub:amdaemon_api] Trying ALL.Net Lite: %s (ua=%s client_id=%s title=%s/%s)\n",
             url, ua, g_short_keychip_str, g_game_id_str, g_title_ver_str);
    int rc = http_post_bytes(url, ua, NULL, enc_buf, (size_t)enc_len, &http_code, &resp);
    int ok = 0;
    if (rc == 0 && http_code == 200 && resp.data && resp.len > 16) {
        char dec[4096];
        int dlen = lite_aes_decrypt(resp.data, (int)resp.len, dec, (int)sizeof(dec));
        if (dlen > 0) {
            ok = apply_allnet_response(dec, 1);
        } else {
            resp.data[resp.len < 4095 ? resp.len : 4095] = '\0';
            if (strstr((const char *)resp.data, "result=") || strstr((const char *)resp.data, "stat=")) {
                ok = apply_allnet_response((const char *)resp.data, 1);
            } else {
                stub_log("[LinuxStub:amdaemon_api] Lite response decrypt failed (len=%zu)\n", resp.len);
            }
        }
    } else {
        stub_log("[LinuxStub:amdaemon_api] Lite HTTP failed (curl_rc=%d http=%ld len=%zu)\n", rc, http_code, resp.len);
    }
    free(resp.data);
    return ok;
}

static int try_allnet_poweron(void) {
    char url[512];
    build_endpoint_url(g_net_server, "/sys/servlet/PowerOn", "http", url, sizeof(url));

    uint32_t tok = ((uint32_t)time(NULL) ^ (uint32_t)getpid()) | 1u;
    char plain[512];
    int plen = snprintf(plain, sizeof(plain),
                        "game_id=%s&ver=%s&serial=%s&ip=%s&firm_ver=51332&boot_ver=0&encode=UTF-8&format_ver=3&hops=1&token=%u",
                        g_game_id_str, g_title_ver_str, g_short_keychip_str, g_real_ip, tok);
    if (plen <= 0 || plen >= (int)sizeof(plain)) return 0;

    typedef int (*fn_compress2_t)(uint8_t *, unsigned long *, const uint8_t *, unsigned long, int);
    typedef int (*fn_uncompress_t)(uint8_t *, unsigned long *, const uint8_t *, unsigned long);
    static void *h_z = NULL;
    static fn_compress2_t p_comp = NULL;
    static fn_uncompress_t p_uncomp = NULL;
    if (!h_z) {
        h_z = dlopen("libz.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (h_z) {
            p_comp = (fn_compress2_t)dlsym(h_z, "compress2");
            p_uncomp = (fn_uncompress_t)dlsym(h_z, "uncompress");
        }
    }

    const void *send_ptr = plain;
    size_t send_len = (size_t)plen;
    char *b64_req = NULL;
    const char *extra_hdr = NULL;

    if (p_comp) {
        unsigned long zcap = (unsigned long)plen + 64;
        uint8_t *zbuf = (uint8_t *)malloc(zcap);
        if (zbuf && p_comp(zbuf, &zcap, (const uint8_t *)plain, (unsigned long)plen, 6) == 0) {
            b64_req = base64_encode(zbuf, (size_t)zcap, &send_len);
            if (b64_req) {
                send_ptr = b64_req;
                extra_hdr = "Pragma: DFI";
            }
        }
        free(zbuf);
    }

    curl_buf_t resp = {0};
    long http_code = 0;
    stub_log("[LinuxStub:amdaemon_api] Trying ALL.Net PowerOn: %s (serial=%s ver=%s)\n",
             url, g_short_keychip_str, g_title_ver_str);
    int rc = http_post_bytes(url, "ALL.Net/Linux", extra_hdr, send_ptr, send_len, &http_code, &resp);
    free(b64_req);

    int ok = 0;
    if (rc == 0 && http_code == 200 && resp.data && resp.len > 0) {
        if (strstr((const char *)resp.data, "stat=") || strstr((const char *)resp.data, "result=")) {
            ok = apply_allnet_response((const char *)resp.data, 0);
        } else if (p_uncomp) {
            size_t zlen = 0;
            uint8_t *zraw = base64_decode((const char *)resp.data, resp.len, &zlen);
            if (zraw && zlen > 0) {
                unsigned long ulen = 8192;
                char *ubuf = (char *)malloc(ulen + 1);
                if (ubuf && p_uncomp((uint8_t *)ubuf, &ulen, zraw, (unsigned long)zlen) == 0) {
                    ubuf[ulen] = '\0';
                    ok = apply_allnet_response(ubuf, 0);
                }
                free(ubuf);
            }
            free(zraw);
        }
    } else {
        stub_log("[LinuxStub:amdaemon_api] PowerOn HTTP failed (curl_rc=%d http=%ld len=%zu)\n", rc, http_code, resp.len);
    }
    free(resp.data);
    return ok;
}

static void *allnet_auth_thread(void *arg) {
    (void)arg;
    int ok = 0;
    if (strcasecmp(g_net_protocol, "lite") == 0) {
        ok = try_allnet_lite();
    } else if (strcasecmp(g_net_protocol, "poweron") == 0) {
        ok = try_allnet_poweron();
    } else {
        if (strstr(g_net_server, "sys-allnet") != NULL ||
            strstr(g_net_server, "sys-all.cn") != NULL ||
            strstr(g_net_server, "/net/initialize") != NULL) {
            ok = try_allnet_lite();
        } else if (strstr(g_net_server, "/sys/servlet/PowerOn") != NULL) {
            ok = try_allnet_poweron();
        } else {
            ok = try_allnet_poweron();
            if (!ok) ok = try_allnet_lite();
        }
    }

    if (!ok) {
        __atomic_store_n(&g_auth_good, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_wan_available, 0, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_auth_in_progress, 0, __ATOMIC_RELEASE);
    return NULL;
}

static void start_allnet_auth_async(void) {
    if (__atomic_exchange_n(&g_auth_in_progress, 1, __ATOMIC_ACQ_REL) != 0) {
        return;
    }
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, allnet_auth_thread, NULL) != 0) {
        __atomic_store_n(&g_auth_in_progress, 0, __ATOMIC_RELEASE);
    }
    pthread_attr_destroy(&attr);
}

static void save_net_config(void) {
    if (g_net_config_path[0] == '\0') {
        resolve_exe_dir_file(g_net_config_path, sizeof(g_net_config_path), "net_config.ini");
    }
    FILE *f = fopen(g_net_config_path, "w");
    if (!f) return;
    fprintf(f,
        "# maimai DX Linux 联网与机台配置文件\n"
        "server = %s\n"
        "protocol = %s\n"
        "keychip_id = %s\n"
        "board_id = %s\n"
        "game_id = %s\n"
        "title_ver = %s\n"
        "region_code = %d\n"
        "ssl_verify = %s\n"
        "timeout_sec = %d\n",
        g_net_server, g_net_protocol, g_keychip_str, g_board_str,
        g_game_id_str, g_title_ver_str, g_region_code,
        g_ssl_verify ? "true" : "false", g_net_timeout_sec);
    fclose(f);
}

static void init_net_system_once(void) {
    if (__atomic_load_n(&g_net_initialized, __ATOMIC_ACQUIRE)) return;
    if (__atomic_exchange_n(&g_net_initialized, 1, __ATOMIC_ACQ_REL) != 0) return;

    resolve_exe_dir_file(g_net_config_path, sizeof(g_net_config_path), "net_config.ini");
    FILE *f = fopen(g_net_config_path, "r");
    if (!f) {
        save_net_config();
    } else {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *hash = strchr(line, '#');
            if (hash) *hash = '\0';
            char *semi = strchr(line, ';');
            if (semi) *semi = '\0';
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char *k = trim_ascii(line);
            char *v = trim_ascii(eq + 1);
            if (strcmp(k, "server") == 0 && *v) {
                snprintf(g_net_server, sizeof(g_net_server), "%s", v);
            } else if (strcmp(k, "protocol") == 0 && *v) {
                snprintf(g_net_protocol, sizeof(g_net_protocol), "%s", v);
            } else if (strcmp(k, "keychip_id") == 0 && *v) {
                normalize_serial(v, g_keychip_str, sizeof(g_keychip_str),
                                 g_short_keychip_str, sizeof(g_short_keychip_str));
            } else if (strcmp(k, "board_id") == 0 && *v) {
                normalize_serial(v, g_board_str, sizeof(g_board_str),
                                 g_short_board_str, sizeof(g_short_board_str));
            } else if (strcmp(k, "game_id") == 0 && *v) {
                snprintf(g_game_id_str, sizeof(g_game_id_str), "%.4s", v);
            } else if (strcmp(k, "title_ver") == 0 && *v) {
                snprintf(g_title_ver_str, sizeof(g_title_ver_str), "%s", v);
            } else if (strcmp(k, "region_code") == 0 && *v) {
                g_region_code = (int32_t)atoi(v);
            } else if (strcmp(k, "ssl_verify") == 0) {
                g_ssl_verify = (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0 ||
                                strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0);
            } else if (strcmp(k, "timeout_sec") == 0 && *v) {
                int t = atoi(v);
                if (t >= 1 && t <= 30) g_net_timeout_sec = t;
            }
        }
        fclose(f);
    }

    g_app_version_u32 = parse_version_str(g_title_ver_str);
    g_auth_region_code = g_region_code;
    utf8_to_u16(g_game_id_str, u16_gameid, 16);
    utf8_to_u16(g_keychip_str, u16_keychip, 32);
    utf8_to_u16(g_short_keychip_str, u16_short_keychip, 32);
    utf8_to_u16(g_board_str, u16_board, 32);
    utf8_to_u16(g_short_board_str, u16_short_board, 32);

    stub_log("[LinuxStub:amdaemon_api] net_config=%s server='%s' proto='%s' keychip='%s'(%s) game=%s ver=%s(0x%08x)\n",
             g_net_config_path, g_net_server, g_net_protocol, g_keychip_str, g_short_keychip_str,
             g_game_id_str, g_title_ver_str, g_app_version_u32);

    start_allnet_auth_async();
}

static uint16_t u16_item_ip[16] = u"IP Address";
static uint16_t u16_item_gw[16] = u"Gateway";
static uint16_t u16_item_dns[16] = u"LAN DNS";
static uint16_t u16_item_hops[16] = u"Hops";
static uint16_t u16_item_line[16] = u"Line Type";
static uint16_t u16_item_auth[16] = u"Allnet Auth";
static uint16_t u16_item_aime[16] = u"Aime";
static uint16_t u16_item_emoney[16] = u"EMoney";
static uint16_t u16_item_aimepay[16] = u"AimePay";
static uint16_t u16_item_allnetair[16] = u"AllnetAir";

static int ip_initialized = 0;

static void init_real_ip(void) {
    if (ip_initialized) return;
    ip_initialized = 1;
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;
        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
            int i;
            for (i = 0; buf[i] && i < 60; i++) u16_ip[i] = (uint16_t)buf[i];
            u16_ip[i] = 0;
            snprintf(g_real_ip, sizeof(g_real_ip), "%s", buf);
            char gw_buf[INET_ADDRSTRLEN];
            strncpy(gw_buf, buf, sizeof(gw_buf));
            char *last_dot = strrchr(gw_buf, '.');
            if (last_dot) {
                strcpy(last_dot, ".1");
                for (i = 0; gw_buf[i] && i < 60; i++) u16_gw[i] = (uint16_t)gw_buf[i];
                u16_gw[i] = 0;
            }
        }
        if (ifa->ifa_netmask) {
            struct sockaddr_in *sm = (struct sockaddr_in *)ifa->ifa_netmask;
            char mbuf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &sm->sin_addr, mbuf, sizeof(mbuf))) {
                int i;
                for (i = 0; mbuf[i] && i < 60; i++) u16_mask[i] = (uint16_t)mbuf[i];
                u16_mask[i] = 0;
            }
        }
        break;
    }
    freeifaddrs(ifaddr);
}

int64_t AccessCode_DigitCount_get(void) {
    static int count = 0;
    int64_t res = 20;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AccessCode_DigitCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AccessCode_Size_get(void) {
    static int count = 0;
    int64_t res = 10;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AccessCode_Size_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AccessCode_canMake(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AccessCode_canMake() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AccessCode_getDigit(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AccessCode_getDigit() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AccessCode_make(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AccessCode_make() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AccessCode_makeInvalid(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AccessCode_makeInvalid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AccessCode_operator_equals(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AccessCode_operator_equals() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AccessCode_operator_less(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AccessCode_operator_less() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AccessCode_toString(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AccessCode_toString() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AccessCode_valid(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AccessCode_valid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeCampaignInfo_MaxNameLength_get(void) {
    static int count = 0;
    int64_t res = 127;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeCampaignInfo_MaxNameLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeCampaignInfo_id_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeCampaignInfo_id_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeCampaignInfo_name_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeCampaignInfo_name_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeCampaignInfo_noticeTime_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeCampaignInfo_noticeTime_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeCampaignInfo_openTimeRange_begin_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeCampaignInfo_openTimeRange_begin_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeCampaignInfo_openTimeRange_end_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeCampaignInfo_openTimeRange_end_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeCampaignInfo_rewardTimeRange_begin_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeCampaignInfo_rewardTimeRange_begin_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeCampaignInfo_rewardTimeRange_end_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeCampaignInfo_rewardTimeRange_end_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeCampaignProgress_bits_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeCampaignProgress_bits_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeCampaignProgress_entry_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeCampaignProgress_entry_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeCampaignProgress_id_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeCampaignProgress_id_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeErrorId_getCategory(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeErrorId_getCategory() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeErrorInfo_base(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeErrorInfo_base() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeErrorInfo_getCategory(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeErrorInfo_getCategory() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeErrorInfo_getId(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeErrorInfo_getId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeId_makeInvalid(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeId_makeInvalid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeId_valid(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeId_valid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeOfflineId_Size_get(void) {
    static int count = 0;
    int64_t res = 17;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeOfflineId_Size_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeOfflineId_getData_accessCode_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeOfflineId_getData_accessCode_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeOfflineId_getData_feliCaId_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeOfflineId_getData_feliCaId_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeOfflineId_getType(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeOfflineId_getType() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeOfflineId_make_AccessCode(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeOfflineId_make_AccessCode() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeOfflineId_make_FeliCaId(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeOfflineId_make_FeliCaId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeOfflineId_operator_equals(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeOfflineId_operator_equals() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeOfflineId_operator_less(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeOfflineId_operator_less() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeOfflineId_toString(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeOfflineId_toString() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeOfflineId_valid(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeOfflineId_valid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayDealResult_MaxErrorCodeLength_get(void) {
    static int count = 0;
    int64_t res = 18;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayDealResult_MaxErrorCodeLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayDealResult_MaxItemIdLength_get(void) {
    static int count = 0;
    int64_t res = 13;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayDealResult_MaxItemIdLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayDealResult_getAccessCode(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayDealResult_getAccessCode() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayDealResult_getAmount(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayDealResult_getAmount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayDealResult_getErrorCode(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayDealResult_getErrorCode() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayDealResult_getItemCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayDealResult_getItemCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayDealResult_getItemId(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayDealResult_getItemId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayDealResult_getReceiptId(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayDealResult_getReceiptId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayDealResult_getStatus(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayDealResult_getStatus() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayDealResult_getTime(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayDealResult_getTime() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayDealResult_valid(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayDealResult_valid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayDealSummary_getAmount(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayDealSummary_getAmount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayDealSummary_getCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayDealSummary_getCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayDealSummary_getDate(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayDealSummary_getDate() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayLocationInfo_MaxCompanyNameLength_get(void) {
    static int count = 0;
    int64_t res = 40;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayLocationInfo_MaxCompanyNameLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayLocationInfo_MaxNameLength_get(void) {
    static int count = 0;
    int64_t res = 20;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayLocationInfo_MaxNameLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayLocationInfo_getCompanyName(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayLocationInfo_getCompanyName() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayLocationInfo_getName(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayLocationInfo_getName() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayLocationInfo_valid(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayLocationInfo_valid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_AmountCountLimit_get(void) {
    static int count = 0;
    int64_t res = 99999;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_AmountCountLimit_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_CheckInPinCodeLength_get(void) {
    static int count = 0;
    int64_t res = 4;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_CheckInPinCodeLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_CoinLimit_get(void) {
    static int count = 0;
    int64_t res = 99;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_CoinLimit_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_MaxItemIdLength_get(void) {
    static int count = 0;
    int64_t res = 13;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_MaxItemIdLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_acceptConfirm(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_acceptConfirm() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_activate(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_activate() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_canAddCoin(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_canAddCoin() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_canOperateDeal(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_canOperateDeal() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_cancel(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_cancel() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_checkDisplay(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_checkDisplay() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_deactivate(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_deactivate() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_getConfirm(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_getConfirm() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_getDealResult(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_getDealResult() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_getUserSetting(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_getUserSetting() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_hasConfirm(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_hasConfirm() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_hasDealResult(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_hasDealResult() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_isBusy(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_isBusy() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_isCancellable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_isCancellable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_isDealAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_isDealAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_isErrorOccurred(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_isErrorOccurred() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_payAmount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_payAmount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_payToCoin(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_payToCoin() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayOperation_requestUserSetting(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayOperation_requestUserSetting() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayUserSetting_canSkipAimeHold(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayUserSetting_canSkipAimeHold() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayUserSetting_getAimeId(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayUserSetting_getAimeId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayUserSetting_isCreditCardAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayUserSetting_isCreditCardAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePayUserSetting_valid(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePayUserSetting_valid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePay_MaxDealResultCount_get(void) {
    static int count = 0;
    int64_t res = 5;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePay_MaxDealResultCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePay_MaxDealSummaryCount_get(void) {
    static int count = 0;
    int64_t res = 7;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePay_MaxDealSummaryCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePay_getActivatedLocationInfo(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePay_getActivatedLocationInfo() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePay_getCurrentLocationInfo(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePay_getCurrentLocationInfo() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePay_getDealResult(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePay_getDealResult() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePay_getDealResultCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePay_getDealResultCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePay_getDealSummary(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePay_getDealSummary() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePay_getDealSummaryCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePay_getDealSummaryCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePay_getOperation(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePay_getOperation() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePay_isActivated(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePay_isActivated() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimePay_isAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimePay_isAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeResult_getAccessCode(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeResult_getAccessCode() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeResult_getAimeId(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeResult_getAimeId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeResult_getFirmVersion(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeResult_getFirmVersion() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeResult_getHardVersion(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeResult_getHardVersion() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeResult_getOfflineId(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeResult_getOfflineId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeResult_getRelatedAimeId(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeResult_getRelatedAimeId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeResult_getRelatedAimeIdCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeResult_getRelatedAimeIdCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeResult_getSegaIdAuthKey(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeResult_getSegaIdAuthKey() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeResult_isMobile(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeResult_isMobile() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeResult_isReaderDetected(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeResult_isReaderDetected() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeResult_isSegaIdRegistered(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeResult_isSegaIdRegistered() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeResult_valid(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeResult_valid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeUnit_acceptConfirm(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeUnit_acceptConfirm() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeUnit_canStart(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeUnit_canStart() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeUnit_cancel(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeUnit_cancel() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeUnit_getBusyCommand(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeUnit_getBusyCommand() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeUnit_getConfirm(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeUnit_getConfirm() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeUnit_getErrorInfo(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeUnit_getErrorInfo() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeUnit_getLedStatus(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeUnit_getLedStatus() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeUnit_getResult(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeUnit_getResult() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeUnit_hasConfirm(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeUnit_hasConfirm() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeUnit_hasError(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeUnit_hasError() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeUnit_hasResult(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeUnit_hasResult() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeUnit_isBusy(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeUnit_isBusy() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeUnit_setLed(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeUnit_setLed() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeUnit_setLedStatus(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeUnit_setLedStatus() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AimeUnit_start(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AimeUnit_start() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Aime_MaxCampaignCount_get(void) {
    static int count = 0;
    int64_t res = 3;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Aime_MaxCampaignCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Aime_getCampaignInfo(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Aime_getCampaignInfo() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Aime_getCampaignInfoCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Aime_getCampaignInfoCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Aime_getCampaignProgress(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Aime_getCampaignProgress() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Aime_getCampaignProgressCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Aime_getCampaignProgressCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Aime_getFirmUpdateProgress(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Aime_getFirmUpdateProgress() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Aime_getUnit(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Aime_getUnit() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Aime_getUnitCount(void) {
    static int count = 0;
    int64_t res = 1;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Aime_getUnitCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Aime_isAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Aime_isAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Aime_isDBAlive(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Aime_isDBAlive() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Aime_isFirmUpdating(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Aime_isFirmUpdating() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Aime_requestCampaignProgress(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Aime_requestCampaignProgress() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Aime_requestCampaignProgress_lastResult(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Aime_requestCampaignProgress_lastResult() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Aime_sendLog(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Aime_sendLog() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Aime_sendLogWithCredit(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Aime_sendLogWithCredit() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AnalogInput_getDelta(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AnalogInput_getDelta() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AnalogInput_getValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AnalogInput_getValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AnalogOutput_getCurrentValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AnalogOutput_getCurrentValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AnalogOutput_setValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AnalogOutput_setValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Apm_exitGame(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Apm_exitGame() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Apm_getLastGameExitReason(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Apm_getLastGameExitReason() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Apm_startGame(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Apm_startGame() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AppImage_existsOption(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AppImage_existsOption() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AppImage_findOptionInfo(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AppImage_findOptionInfo() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AppImage_getCreationTime(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AppImage_getCreationTime() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AppImage_getCurrentVersion(void) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (int64_t)g_app_version_u32;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AppImage_getCurrentVersion() -> 0x%llx\n", (unsigned long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AppImage_getOptionCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AppImage_getOptionCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AppImage_getOptionInfo(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AppImage_getOptionInfo() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AppImage_getOptionMountRootPath(void) {
    static int count = 0;
    int64_t res = (int64_t)u16_option_dir;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AppImage_getOptionMountRootPath() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t AppImage_makeOptionMountPath(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] AppImage_makeOptionMountPath() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Backup_MaxRecordCount_get(void) {
    static int count = 0;
    int64_t res = 80;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Backup_MaxRecordCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Backup_executeSave(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Backup_executeSave() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Backup_getRecordCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Backup_getRecordCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Backup_getRecordStatus(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Backup_getRecordStatus() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Backup_isAsync(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Backup_isAsync() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Backup_isBusy(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Backup_isBusy() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Backup_isSetupSucceeded(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Backup_isSetupSucceeded() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Backup_saveAllRecords(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Backup_saveAllRecords() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Backup_saveRecord(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Backup_saveRecord() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Backup_setAsync(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Backup_setAsync() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Backup_setupRecords(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Backup_setupRecords() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t BoardIO_DipSwitchCount_get(void) {
    static int count = 0;
    int64_t res = 8;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] BoardIO_DipSwitchCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t BoardIO_LedCount_get(void) {
    static int count = 0;
    int64_t res = 2;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] BoardIO_LedCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t BoardIO_PushSwitchCount_get(void) {
    static int count = 0;
    int64_t res = 2;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] BoardIO_PushSwitchCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t BoardIO_getCurrentLedStateBits(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] BoardIO_getCurrentLedStateBits() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t BoardIO_getDipSwitchAppValue(void) {
    return 1;
}

int64_t BoardIO_getDipSwitchBits(void) {
    return 1;
}

int64_t BoardIO_getPushSwitchBits(void) {
    return 0;
}

int64_t BoardIO_isCurrentLedStateOn(void) {
    return 0;
}

int64_t BoardIO_isDipSwitchOn(int64_t index) {
    return (index == 0) ? 1 : 0;
}

int64_t BoardIO_isPushSwitchOn(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] BoardIO_isPushSwitchOn() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t BoardIO_setLedState(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] BoardIO_setLedState() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanAck_exists(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanAck_exists() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanAck_getCommand(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanAck_getCommand() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanAck_getData(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanAck_getData() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanAck_getDataSize(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanAck_getDataSize() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanAck_getErrorReport(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanAck_getErrorReport() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanAck_isError(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanAck_isError() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanPort_getTarget(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanPort_getTarget() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanPort_getTargetCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanPort_getTargetCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanTargetProperty_getCustomChip(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanTargetProperty_getCustomChip() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanTargetProperty_getFirmRevision(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanTargetProperty_getFirmRevision() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanTargetProperty_getFirmSum(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanTargetProperty_getFirmSum() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanTargetProperty_getId(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanTargetProperty_getId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanTargetProperty_getProduct(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanTargetProperty_getProduct() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanTargetProperty_getType(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanTargetProperty_getType() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanTargetProperty_valid(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanTargetProperty_valid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanTarget_DefaultPriority_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanTarget_DefaultPriority_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanTarget_ErrorReportCommand_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanTarget_ErrorReportCommand_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanTarget_MaxDataSize_get(void) {
    static int count = 0;
    int64_t res = 8;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanTarget_MaxDataSize_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanTarget_MaxPriority_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanTarget_MaxPriority_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanTarget_getAck(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanTarget_getAck() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanTarget_getProperty(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanTarget_getProperty() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CanTarget_send(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CanTarget_send() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Can_getPort(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Can_getPort() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Can_getPortCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Can_getPortCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Can_isAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Can_isAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CoinSlotCondition_busy_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CoinSlotCondition_busy_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CoinSlotCondition_disconnected_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CoinSlotCondition_disconnected_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CoinSlotCondition_jam_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CoinSlotCondition_jam_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Core_changeLanguage(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Core_changeLanguage() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Core_execute(void) {
    static int count = 0;
    int64_t res = 0;
    init_coin_system_once();
    init_net_system_once();
    poll_coin_key();
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Core_execute() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Core_getLanguage(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Core_getLanguage() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Core_getLibraryVersion(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Core_getLibraryVersion() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Core_getPlayerCount(void) {
    static int count = 0;
    int64_t res = 2;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Core_getPlayerCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Core_isExited(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Core_isExited() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Core_isReady(void) {
    static int count = 0;
    int64_t res = 1;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Core_isReady() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Core_kill(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Core_kill() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Core_preloadDataSection(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Core_preloadDataSection() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Core_reboot(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Core_reboot() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Core_resetExceptionHook(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Core_resetExceptionHook() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Core_setExceptionHook_check(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Core_setExceptionHook_check() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Core_setExceptionHook_count(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Core_setExceptionHook_count() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Core_setExceptionHook_done(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Core_setExceptionHook_done() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Core_setExceptionHook_register(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Core_setExceptionHook_register() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CreditBookkeeping_CoinChuteCount_get(void) {
    static int count = 0;
    int64_t res = 4;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CreditBookkeeping_CoinChuteCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CreditBookkeeping_coinCredit_get(void *ptr) {
    (void)ptr;
    init_coin_system_once();
    return (int64_t)g_total_credits;
}

int64_t CreditBookkeeping_coins_get(void *ptr, int32_t chute_index) {
    (void)ptr;
    init_coin_system_once();
    return (chute_index == 0) ? (int64_t)g_total_coins : 0;
}

int64_t CreditBookkeeping_eMoneyCoin_get(void *ptr) {
    (void)ptr;
    return 0;
}

int64_t CreditBookkeeping_eMoneyCredit_get(void *ptr) {
    (void)ptr;
    return 0;
}

int64_t CreditBookkeeping_serviceCredit_get(void *ptr) {
    (void)ptr;
    return 0;
}

int64_t CreditBookkeeping_totalCoin_get(void *ptr) {
    (void)ptr;
    init_coin_system_once();
    return (int64_t)g_total_coins;
}

int64_t CreditBookkeeping_totalCredit_get(void *ptr) {
    (void)ptr;
    init_coin_system_once();
    return (int64_t)g_total_credits;
}

int64_t CreditConfig_MaxCoinChuteCount_get(void) {
    static int count = 0;
    int64_t res = 2;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CreditConfig_MaxCoinChuteCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CreditConfig_MaxGameCostCount_get(void) {
    static int count = 0;
    int64_t res = 8;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CreditConfig_MaxGameCostCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CreditConfig_bonusAdder_get(void *ptr) {
    (void)ptr;
    return 0;
}

int64_t CreditConfig_coinAmount_get(void *ptr) {
    (void)ptr;
    return 100;
}

int64_t CreditConfig_coinChuteCommon_get(void *ptr) {
    (void)ptr;
    return 1;
}

int64_t CreditConfig_coinMultipliers_get(void *ptr, int32_t chute_index) {
    (void)ptr;
    (void)chute_index;
    return 1;
}

int64_t CreditConfig_coinToCredit_get(void *ptr) {
    (void)ptr;
    init_coin_system_once();
    return (int64_t)g_coins_per_credit;
}

int64_t CreditConfig_freePlay_get(void *ptr) {
    (void)ptr;
    init_coin_system_once();
    return g_free_play ? 1 : 0;
}

int64_t CreditConfig_gameCosts_get(void *ptr, int32_t index) {
    (void)ptr;
    return (int64_t)game_cost_for_index(index);
}

int64_t CreditConfig_serviceCommon_get(void *ptr) {
    (void)ptr;
    return 1;
}

int64_t CreditSpecialDevice_isAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CreditSpecialDevice_isAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CreditSpecialDevice_isLockoutOn(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CreditSpecialDevice_isLockoutOn() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CreditSpecialDevice_lockout(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CreditSpecialDevice_lockout() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CreditUnit_GameCostConfigCount_get(void) {
    static int count = 0;
    int64_t res = 8;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CreditUnit_GameCostConfigCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CreditUnit_PayCountLimit_get(void) {
    static int count = 0;
    int64_t res = 99;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] CreditUnit_PayCountLimit_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t CreditUnit_getAddableCoin(void *ptr) {
    (void)ptr;
    init_coin_system_once();
    return (int64_t)(g_coins < 9999 ? (9999 - g_coins) : 0);
}

int64_t CreditUnit_getCoinToCredit(void *ptr) {
    (void)ptr;
    init_coin_system_once();
    return (int64_t)g_coins_per_credit;
}

int64_t CreditUnit_getCredit(void *ptr) {
    (void)ptr;
    init_coin_system_once();
    return (int64_t)(g_coins / g_coins_per_credit);
}

int64_t CreditUnit_getGameCost(void *ptr, int32_t game_cost_index) {
    (void)ptr;
    return (int64_t)game_cost_for_index(game_cost_index);
}

int64_t CreditUnit_getRemain(void *ptr) {
    (void)ptr;
    init_coin_system_once();
    return (int64_t)(g_coins % g_coins_per_credit);
}

int64_t CreditUnit_isFreePlay(void *ptr) {
    (void)ptr;
    init_coin_system_once();
    return g_free_play ? 1 : 0;
}

int64_t CreditUnit_isGameCostEnough(void *ptr, int32_t game_cost_index, int32_t count) {
    (void)ptr;
    init_coin_system_once();
    if (g_free_play) return 1;
    if (count <= 0) return 1;
    uint32_t need_credits = game_cost_for_index(game_cost_index) * (uint32_t)count;
    return ((g_coins / g_coins_per_credit) >= need_credits) ? 1 : 0;
}

int64_t CreditUnit_isZero(void *ptr) {
    (void)ptr;
    init_coin_system_once();
    return (g_coins == 0) ? 1 : 0;
}

int64_t CreditUnit_payGameCost(void *ptr, int32_t game_cost_index, int32_t count) {
    (void)ptr;
    init_coin_system_once();
    if (g_free_play) return 1;
    if (count <= 0) return 1;
    uint32_t need_coins = game_cost_for_index(game_cost_index) * (uint32_t)count * g_coins_per_credit;
    if (g_coins < need_coins) return 0;
    g_coins -= need_coins;
    save_coin_config();
    stub_log("[LinuxStub:amdaemon_api] payGameCost(idx=%d, count=%d) -> deducted %u coins, left coins=%u\n",
             game_cost_index, count, need_coins, g_coins);
    return 1;
}

int64_t CreditUnit_toString(void *ptr) {
    (void)ptr;
    init_coin_system_once();
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", g_coins / g_coins_per_credit);
    size_t i = 0;
    for (; buf[i] && i + 1 < 32; i++) {
        g_credit_str_u16[i] = (uint16_t)(unsigned char)buf[i];
    }
    g_credit_str_u16[i] = 0;
    return (int64_t)g_credit_str_u16;
}

int64_t Credit_clearBackup(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Credit_clearBackup() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Credit_getBookkeeping(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Credit_getBookkeeping() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Credit_getConfig(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Credit_getConfig() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Credit_getPlayer(int32_t index) {
    if (index < 0 || index > 1) index = 0;
    return (int64_t)&dummy_struct[index * 16];
}

int64_t Credit_getPlayerCount(void) {
    static int count = 0;
    int64_t res = 2;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Credit_getPlayerCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Credit_getSpecialDevice(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Credit_getSpecialDevice() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Credit_isAvailable(void) {
    init_coin_system_once();
    return 1;
}

int64_t Credit_isCoinInIgnored(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Credit_isCoinInIgnored() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Credit_resetCoinInHook(void) {
    g_pending_coin_sound = 0;
    return 0;
}

int64_t Credit_setCoinInHook_check(uint32_t *sound, void **player_indices_ptr, int32_t *player_count) {
    if (g_pending_coin_sound == 0) return 0;
    if (sound) *sound = g_pending_coin_sound;
    if (player_indices_ptr) *player_indices_ptr = (void *)g_coin_player_indices;
    if (player_count) *player_count = 1;
    return 1;
}

int64_t Credit_setCoinInHook_done(void) {
    g_pending_coin_sound = 0;
    return 0;
}

int64_t Credit_setCoinInHook_register(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Credit_setCoinInHook_register() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Credit_setCoinInIgnored(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Credit_setCoinInIgnored() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyBrandIdCount_get(void) {
    static int count = 0;
    int64_t res = 7;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyBrandIdCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyBrand_MaxNameLength_get(void) {
    static int count = 0;
    int64_t res = 15;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyBrand_MaxNameLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyBrand_getIconFilePath(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyBrand_getIconFilePath() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyBrand_getId(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyBrand_getId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyBrand_getName(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyBrand_getName() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyBrand_hasBalance(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyBrand_hasBalance() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_AmountCountLimit_get(void) {
    static int count = 0;
    int64_t res = 99999;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_AmountCountLimit_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_CoinLimit_get(void) {
    static int count = 0;
    int64_t res = 99;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_CoinLimit_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_MaxItemIdLength_get(void) {
    static int count = 0;
    int64_t res = 13;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_MaxItemIdLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_authTerminal(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_authTerminal() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_canAddCoin(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_canAddCoin() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_canOperateDeal(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_canOperateDeal() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_cancel(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_cancel() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_checkDisplay(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_checkDisplay() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_getResult(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_getResult() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_hasResult(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_hasResult() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_isBusy(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_isBusy() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_isCancellable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_isCancellable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_isDealAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_isDealAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_isErrorOccurred(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_isErrorOccurred() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_isHeldOver(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_isHeldOver() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_payAmount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_payAmount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_payAmount_check(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_payAmount_check() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_payAmount_done(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_payAmount_done() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_payToCoin(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_payToCoin() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_removeTerminal(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_removeTerminal() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyOperation_requestBalance(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyOperation_requestBalance() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyResult_MaxCardNumberLength_get(void) {
    static int count = 0;
    int64_t res = 20;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyResult_MaxCardNumberLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyResult_MaxDealNumberLength_get(void) {
    static int count = 0;
    int64_t res = 20;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyResult_MaxDealNumberLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyResult_getAmount(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyResult_getAmount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyResult_getBalanceAfter(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyResult_getBalanceAfter() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyResult_getBalanceBefore(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyResult_getBalanceBefore() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyResult_getBrand(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyResult_getBrand() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyResult_getCardNumber(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyResult_getCardNumber() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyResult_getDealNumber(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyResult_getDealNumber() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyResult_getStatus(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyResult_getStatus() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyResult_getTime(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyResult_getTime() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoneyResult_valid(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoneyResult_valid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_MaxDealResultCount_get(void) {
    static int count = 0;
    int64_t res = 20;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_MaxDealResultCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_MaxTerminalIdLength_get(void) {
    static int count = 0;
    int64_t res = 30;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_MaxTerminalIdLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_MaxTerminalSerialLength_get(void) {
    static int count = 0;
    int64_t res = 20;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_MaxTerminalSerialLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_getAvailableBrand(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_getAvailableBrand() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_getAvailableBrandCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_getAvailableBrandCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_getBrand(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_getBrand() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_getDealResult(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_getDealResult() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_getDealResultCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_getDealResultCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_getOperation(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_getOperation() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_getTerminalId(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_getTerminalId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_getTerminalSerial(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_getTerminalSerial() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_isAuthCompleted(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_isAuthCompleted() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_isAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_isAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_isBrandAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_isBrandAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_isServiceAlive(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_isServiceAlive() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_resetSoundHook(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_resetSoundHook() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_setSoundHook_check(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_setSoundHook_check() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_setSoundHook_done(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_setSoundHook_done() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t EMoney_setSoundHook_register(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] EMoney_setSoundHook_register() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorInfo_getMessage(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorInfo_getMessage() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorInfo_getNumber(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorInfo_getNumber() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorInfo_getResetType(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorInfo_getResetType() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorInfo_getSubNumber(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorInfo_getSubNumber() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorInfo_getTime(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorInfo_getTime() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorInfo_isOccurred(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorInfo_isOccurred() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorInfo_makeNumberString(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorInfo_makeNumberString() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorInfo_toString(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorInfo_toString() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorLog_MaxHistoryItemCount_get(void) {
    static int count = 0;
    int64_t res = 15;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorLog_MaxHistoryItemCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorLog_MaxTimesItemCount_get(void) {
    static int count = 0;
    int64_t res = 222;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorLog_MaxTimesItemCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorLog_findTimesItem(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorLog_findTimesItem() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorLog_getHistoryItem(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorLog_getHistoryItem() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorLog_getHistoryItemCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorLog_getHistoryItemCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorLog_getSystemHistoryItem(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorLog_getSystemHistoryItem() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorLog_getSystemHistoryItemCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorLog_getSystemHistoryItemCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorLog_getTimesItem(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorLog_getTimesItem() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorLog_getTimesItemCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorLog_getTimesItemCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorTimesItem_number_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorTimesItem_number_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorTimesItem_times_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorTimesItem_times_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t ErrorTimesItem_valid(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] ErrorTimesItem_valid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Error_NumberLimit_get(void) {
    static int count = 0;
    int64_t res = 65535;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Error_NumberLimit_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Error_SubNumberLimit_get(void) {
    static int count = 0;
    int64_t res = 999;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Error_SubNumberLimit_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Error_canReset(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Error_canReset() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Error_clearLog(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Error_clearLog() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Error_getInfo(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Error_getInfo() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Error_getLog(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Error_getLog() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Error_reset(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Error_reset() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Error_set(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Error_set() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Exception_getCategory(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Exception_getCategory() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Exception_getFile(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Exception_getFile() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Exception_getFunction(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Exception_getFunction() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Exception_getLine(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Exception_getLine() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Exception_getMessage(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Exception_getMessage() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Exception_getStackTrace(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Exception_getStackTrace() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Exception_toString(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Exception_toString() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t FeliCaId_Size_get(void) {
    static int count = 0;
    int64_t res = 16;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] FeliCaId_Size_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t FeliCaId_operator_equals(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] FeliCaId_operator_equals() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t FeliCaId_operator_less(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] FeliCaId_operator_less() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t FeliCaId_toString(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] FeliCaId_toString() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t GameIdLength_get(void) {
    static int count = 0;
    int64_t res = 4;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] GameIdLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t InputId_MaxLength_get(void) {
    static int count = 0;
    int64_t res = 23;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] InputId_MaxLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t InputUnit_exists(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] InputUnit_exists() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t InputUnit_getDelta(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] InputUnit_getDelta() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t InputUnit_getDirectValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] InputUnit_getDirectValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t InputUnit_getMaxValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] InputUnit_getMaxValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t InputUnit_getMinValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] InputUnit_getMinValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t InputUnit_getSwitchFlipDelta(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] InputUnit_getSwitchFlipDelta() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t InputUnit_getValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] InputUnit_getValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

static uint64_t g_input_system_unit[64] = {0};
static uint64_t g_input_player_units[2][64] = {{0}, {0}};

int64_t Input_getPlayer(int32_t index) {
    static int count = 0;
    int32_t idx = (index >= 0 && index < 2) ? index : 0;
    int64_t res = (int64_t)g_input_player_units[idx];
    if (count < 4) { printf("[LinuxStub:amdaemon_api] Input_getPlayer(%d) -> %lld\n", index, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Input_getPlayerCount(void) {
    static int count = 0;
    int64_t res = 2;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Input_getPlayerCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Input_getSystem(void) {
    static int count = 0;
    int64_t res = (int64_t)g_input_system_unit;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Input_getSystem() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsAnalogInput_getChannelCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsAnalogInput_getChannelCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsAnalogInput_getValidBitCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsAnalogInput_getValidBitCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsAnalogInput_getValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsAnalogInput_getValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsGeneralOutput_MaxBitCount_get(void) {
    static int count = 0;
    int64_t res = 64;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsGeneralOutput_MaxBitCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsGeneralOutput_getCurrentBits(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsGeneralOutput_getCurrentBits() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsGeneralOutput_resetBits(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsGeneralOutput_resetBits() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsGeneralOutput_setBit(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsGeneralOutput_setBit() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsGeneralOutput_setBits(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsGeneralOutput_setBits() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsNode_getAnalogInput(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsNode_getAnalogInput() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsNode_getGeneralOutput(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsNode_getGeneralOutput() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsNode_getRotaryInput(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsNode_getRotaryInput() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsNode_getSwitchInput(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsNode_getSwitchInput() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsRotaryInput_getChannelCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsRotaryInput_getChannelCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsRotaryInput_getValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsRotaryInput_getValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsSwitchInput_MaxPlayerBitCount_get(void) {
    static int count = 0;
    int64_t res = 64;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsSwitchInput_MaxPlayerBitCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsSwitchInput_MaxSystemBitCount_get(void) {
    static int count = 0;
    int64_t res = 8;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsSwitchInput_MaxSystemBitCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsSwitchInput_getPlayerBits(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsSwitchInput_getPlayerBits() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsSwitchInput_getPlayerCount(void) {
    static int count = 0;
    int64_t res = 2;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsSwitchInput_getPlayerCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsSwitchInput_getPlayerFlipCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsSwitchInput_getPlayerFlipCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsSwitchInput_getSystemBits(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsSwitchInput_getSystemBits() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsSwitchInput_getSystemFlipCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsSwitchInput_getSystemFlipCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsSwitchInput_isPlayerOn(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsSwitchInput_isPlayerOn() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t JvsSwitchInput_isSystemOn(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] JvsSwitchInput_isSystemOn() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Jvs_getNode(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Jvs_getNode() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Jvs_getNodeCount(void) {
    static int count = 0;
    int64_t res = 1;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Jvs_getNodeCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Jvs_isAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Jvs_isAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t LanInstall_getServerCount(void) {
    return 1;
}

int64_t LanInstall_isAvailable(void) {
    return 1;
}

int64_t LanInstall_isClient(void) {
    return 0;
}

int64_t LanInstall_isExitNeeded(void) {
    return 0;
}

int64_t LanInstall_isServer(void) {
    return 1;
}

int64_t MaxLanguageNameLength_get(void) {
    static int count = 0;
    int64_t res = 15;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] MaxLanguageNameLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDeliveryAppImageInfo_creationTime_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDeliveryAppImageInfo_creationTime_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDeliveryAppImageInfo_progress_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDeliveryAppImageInfo_progress_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDeliveryAppImageInfo_status_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDeliveryAppImageInfo_status_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDeliveryAppImageInfo_version_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDeliveryAppImageInfo_version_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDeliveryOptionImageInfo_calcTotalProgress(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDeliveryOptionImageInfo_calcTotalProgress() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDeliveryOptionImageInfo_optionalProgress_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDeliveryOptionImageInfo_optionalProgress_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDeliveryOptionImageInfo_progress_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDeliveryOptionImageInfo_progress_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDeliveryOptionImageInfo_status_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDeliveryOptionImageInfo_status_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDeliveryProgress_current_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDeliveryProgress_current_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDeliveryProgress_toPercentage(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDeliveryProgress_toPercentage() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDeliveryProgress_total_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDeliveryProgress_total_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDeliveryStatus_toString(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDeliveryStatus_toString() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDeliveryTimeInfo_existsImage(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDeliveryTimeInfo_existsImage() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDeliveryTimeInfo_order_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDeliveryTimeInfo_order_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDeliveryTimeInfo_release_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDeliveryTimeInfo_release_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDelivery_MaxDownloadInstructionUriLength_get(void) {
    static int count = 0;
    int64_t res = 128;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDelivery_MaxDownloadInstructionUriLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDelivery_existsApp(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDelivery_existsApp() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDelivery_existsOption(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDelivery_existsOption() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDelivery_getAppImageInfo(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDelivery_getAppImageInfo() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDelivery_getAppTimeInfo(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDelivery_getAppTimeInfo() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDelivery_getOptionImageInfo(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDelivery_getOptionImageInfo() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDelivery_getOptionTimeInfo(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDelivery_getOptionTimeInfo() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDelivery_isAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDelivery_isAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDelivery_isExitNeeded(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDelivery_isExitNeeded() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetDelivery_setDownloadInstructionUri(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetDelivery_setDownloadInstructionUri() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkProperty_getAddress(void) {
    static int count = 0;
    init_real_ip();
    int64_t res = (int64_t)u16_ip;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetworkProperty_getAddress() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkProperty_getGateway(void) {
    static int count = 0;
    int64_t res = (int64_t)u16_gw;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetworkProperty_getGateway() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkProperty_getPrimaryDns(void) {
    static int count = 0;
    int64_t res = (int64_t)u16_dns1;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetworkProperty_getPrimaryDns() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkProperty_getSecondaryDns(void) {
    static int count = 0;
    int64_t res = (int64_t)u16_dns2;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetworkProperty_getSecondaryDns() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkProperty_getSubnetMask(void) {
    static int count = 0;
    init_real_ip();
    int64_t res = (int64_t)u16_mask;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetworkProperty_getSubnetMask() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkProperty_isDhcpEnabled(void) {
    static int count = 0;
    int64_t res = 1;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetworkProperty_isDhcpEnabled() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkProperty_valid(void) {
    static int count = 0;
    int64_t res = 1;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetworkProperty_valid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

static const int s_net_items[] = { 0, 1, 2, 3, 4, 5 }; // IpAddress, Gateway, LanDns, Hops, LineType, AllnetAuth

int64_t NetworkTestInfo_getAvailableItem(void* ptr, int32_t index) {
    static int count = 0;
    int64_t res = 0;
    if (index >= 0 && index < 6) res = s_net_items[index];
    if (count < 6) { printf("[LinuxStub:amdaemon_api] NetworkTestInfo_getAvailableItem(%d) -> %lld\n", index, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkTestInfo_getAvailableItemCount(void* ptr) {
    static int count = 0;
    int64_t res = 6;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetworkTestInfo_getAvailableItemCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkTestInfo_getBusyStatusText(void* ptr, int32_t item) {
    static int count = 0;
    int64_t res = (int64_t)u16_empty;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetworkTestInfo_getBusyStatusText() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkTestInfo_getErrorInfo(void* ptr) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetworkTestInfo_getErrorInfo() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkTestInfo_getHops(void* ptr) {
    static int count = 0;
    int64_t res = 1;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetworkTestInfo_getHops() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkTestInfo_getResult(void* ptr, int32_t item) {
    static int count = 0;
    init_net_system_once();
    // TestResult: None=0, NA=1, Good=2, Bad=3
    int64_t res;
    if (item >= 0 && item <= 4) {
        res = 2; // IpAddress..LineType -> Good
    } else if (item == 5) {
        if (__atomic_load_n(&g_auth_in_progress, __ATOMIC_ACQUIRE)) {
            res = 0; // None while running
        } else {
            res = __atomic_load_n(&g_auth_good, __ATOMIC_ACQUIRE) ? 2 : 3;
        }
    } else {
        res = 1; // NA for unused items
    }
    if (count < 16) { printf("[LinuxStub:amdaemon_api] NetworkTestInfo_getResult(%d) -> %lld\n", item, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkTestInfo_getState(void* ptr, int32_t item) {
    static int count = 0;
    init_net_system_once();
    // NetworkTestState: None=0, Busy=1, Done=2
    int64_t res = (item == 5 && __atomic_load_n(&g_auth_in_progress, __ATOMIC_ACQUIRE)) ? 1 : 2;
    if (count < 16) { printf("[LinuxStub:amdaemon_api] NetworkTestInfo_getState(%d) -> %lld\n", item, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkTestInfo_getStatusText(void* ptr, int32_t item, int32_t flag) {
    static int count = 0;
    int64_t res = (int64_t)u16_empty;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] NetworkTestInfo_getStatusText() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkTestInfo_isAvailableItem(void* ptr, int32_t item) {
    static int count = 0;
    int64_t res = (item >= 0 && item <= 5) ? 1 : 0;
    if (count < 10) { printf("[LinuxStub:amdaemon_api] NetworkTestInfo_isAvailableItem(%d) -> %lld\n", item, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkTestInfo_isBusy(void* ptr, int32_t item) {
    return (item == 5 && __atomic_load_n(&g_auth_in_progress, __ATOMIC_ACQUIRE)) ? 1 : 0;
}

int64_t NetworkTestInfo_isCompleted(void* ptr) {
    static int count = 0;
    init_net_system_once();
    int64_t res = __atomic_load_n(&g_auth_in_progress, __ATOMIC_ACQUIRE) ? 0 : 1;
    if (count < 5) { printf("[LinuxStub:amdaemon_api] NetworkTestInfo_isCompleted() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkTestInfo_isDone(void* ptr, int32_t item) {
    static int count = 0;
    int64_t res = (item == 5 && __atomic_load_n(&g_auth_in_progress, __ATOMIC_ACQUIRE)) ? 0 : 1;
    if (count < 10) { printf("[LinuxStub:amdaemon_api] NetworkTestInfo_isDone(%d) -> %lld\n", item, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t NetworkTestInfo_isRunning(void* ptr) {
    return __atomic_load_n(&g_auth_in_progress, __ATOMIC_ACQUIRE) ? 1 : 0;
}

int64_t NetworkTestItem_toString(int32_t item) {
    switch (item) {
        case 0: return (int64_t)u16_item_ip;
        case 1: return (int64_t)u16_item_gw;
        case 2: return (int64_t)u16_item_dns;
        case 3: return (int64_t)u16_item_hops;
        case 4: return (int64_t)u16_item_line;
        case 5: return (int64_t)u16_item_auth;
        case 6: return (int64_t)u16_item_aime;
        case 7: return (int64_t)u16_item_emoney;
        case 8: return (int64_t)u16_item_aimepay;
        case 9: return (int64_t)u16_item_allnetair;
        default: return (int64_t)u16_empty;
    }
}

int64_t Network_canStartTest(void) {
    static int count = 0;
    int64_t res = __atomic_load_n(&g_auth_in_progress, __ATOMIC_ACQUIRE) ? 0 : 1;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Network_canStartTest() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Network_getLinkupSpeed(void) {
    static int count = 0;
    int64_t res = 1000000000LL;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Network_getLinkupSpeed() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Network_getLocationAddress(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Network_getLocationAddress() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Network_getPowerOnTestInfo(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Network_getPowerOnTestInfo() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Network_getProperty(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Network_getProperty() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Network_getTestInfo(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Network_getTestInfo() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Network_isAvailable(void) {
    static int count = 0;
    int64_t res = 1;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Network_isAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Network_isLanAvailable(void) {
    static int count = 0;
    int64_t res = 1;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Network_isLanAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Network_isLocationLanAvailable(void) {
    static int count = 0;
    int64_t res = 1;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Network_isLocationLanAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Network_isWanAvailable(void) {
    static int count = 0;
    init_net_system_once();
    int64_t res = __atomic_load_n(&g_wan_available, __ATOMIC_ACQUIRE) ? 1 : 0;
    if (count < 5) { printf("[LinuxStub:amdaemon_api] Network_isWanAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Network_startTest(void) {
    static int count = 0;
    init_net_system_once();
    start_allnet_auth_async();
    int64_t res = 1;
    if (count < 5) { printf("[LinuxStub:amdaemon_api] Network_startTest() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t OptionImageInfo_NameLength_get(void) {
    static int count = 0;
    int64_t res = 4;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] OptionImageInfo_NameLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t OptionImageInfo_creationTime_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] OptionImageInfo_creationTime_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t OptionImageInfo_name_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] OptionImageInfo_name_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t OutputId_MaxLength_get(void) {
    static int count = 0;
    int64_t res = 23;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] OutputId_MaxLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t OutputUnit_exists(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] OutputUnit_exists() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t OutputUnit_getCurrentValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] OutputUnit_getCurrentValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t OutputUnit_getMaxValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] OutputUnit_getMaxValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t OutputUnit_getMinValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] OutputUnit_getMinValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t OutputUnit_setValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] OutputUnit_setValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Output_getPlayer(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Output_getPlayer() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Output_getPlayerCount(void) {
    static int count = 0;
    int64_t res = 2;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Output_getPlayerCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Output_getSystem(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Output_getSystem() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t RequestState_isDone(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] RequestState_isDone() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t RequestState_isSent(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] RequestState_isSent() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t RequestState_isSucceeded(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] RequestState_isSucceeded() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Resolution_height_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Resolution_height_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Resolution_width_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Resolution_width_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SequenceBookkeeping_MaxTimeHistogramCount_get(void) {
    static int count = 0;
    int64_t res = 32;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SequenceBookkeeping_MaxTimeHistogramCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SequenceBookkeeping_averagePlayTime_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SequenceBookkeeping_averagePlayTime_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SequenceBookkeeping_longestPlayTime_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SequenceBookkeeping_longestPlayTime_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SequenceBookkeeping_numberOfGames_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SequenceBookkeeping_numberOfGames_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SequenceBookkeeping_playTime_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SequenceBookkeeping_playTime_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SequenceBookkeeping_shortestPlayTime_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SequenceBookkeeping_shortestPlayTime_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SequenceBookkeeping_timeHistogramCount_get(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SequenceBookkeeping_timeHistogramCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SequenceBookkeeping_timeHistogram_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SequenceBookkeeping_timeHistogram_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SequenceBookkeeping_totalTime_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SequenceBookkeeping_totalTime_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Sequence_beginPlay(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Sequence_beginPlay() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Sequence_beginTest(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Sequence_beginTest() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Sequence_clearBackup(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Sequence_clearBackup() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Sequence_continuePlay(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Sequence_continuePlay() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Sequence_endPlay(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Sequence_endPlay() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Sequence_endTest(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Sequence_endTest() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Sequence_getBookkeeping(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Sequence_getBookkeeping() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Sequence_getPlayAccountingTiming(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Sequence_getPlayAccountingTiming() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Sequence_getPlayerCount(void) {
    static int count = 0;
    int64_t res = 2;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Sequence_getPlayerCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Sequence_getPlayingAimeId(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Sequence_getPlayingAimeId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Sequence_isAccountingPlaying(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Sequence_isAccountingPlaying() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Sequence_isPlaying(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Sequence_isPlaying() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Sequence_isPlayingAny(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Sequence_isPlayingAny() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Sequence_isTest(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Sequence_isTest() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SerialId_ShortLength_get(void) {
    static int count = 0;
    int64_t res = 11;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SerialId_ShortLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SerialId_StandardLength_get(void) {
    static int count = 0;
    int64_t res = 16;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SerialId_StandardLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SerialId_id_value(void *ptr) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (ptr == &g_board_id_handle) ? (int64_t)u16_board : (int64_t)u16_keychip;
    if (count < 5) { printf("[LinuxStub:amdaemon_api] SerialId_id_value(%p) -> %lld\n", ptr, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SerialId_shortId_value(void *ptr) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (ptr == &g_board_id_handle) ? (int64_t)u16_short_board : (int64_t)u16_short_keychip;
    if (count < 5) { printf("[LinuxStub:amdaemon_api] SerialId_shortId_value(%p) -> %lld\n", ptr, (long long)res); fflush(stdout); count++; }
    return res;
}

static void amd_id_to_ascii(const void *raw, char *out, size_t cap) {
    if (!raw || cap == 0) { if (cap) out[0] = '\0'; return; }
    const uint16_t *w = (const uint16_t *)raw;
    const uint8_t *b = (const uint8_t *)raw;
    /* AMDaemon.NET passes LPWSTR (UTF-16LE), e.g. 'b',0,'u',0,'t',0... */
    if (b[0] >= 0x20 && b[0] < 0x7f && b[1] == 0) {
        size_t i = 0;
        while (i + 1 < cap && w[i] != 0) {
            out[i] = (w[i] < 0x80) ? (char)w[i] : '?';
            i++;
        }
        out[i] = '\0';
    } else {
        size_t i = 0;
        while (i + 1 < cap && b[i] != 0) {
            out[i] = (char)b[i];
            i++;
        }
        out[i] = '\0';
    }
}

int64_t SwitchInput_getFlipDelta(void *unit, const void *id_value, int32_t to_on) {
    (void)unit; (void)to_on;
    char id[64]; amd_id_to_ascii(id_value, id, sizeof(id));
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SwitchInput_getFlipDelta(%s, %d) -> %lld\n", id, to_on, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SwitchInput_hasFlipNow(void *unit, const void *id_value) {
    (void)unit;
    char id[64]; amd_id_to_ascii(id_value, id, sizeof(id));
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SwitchInput_hasFlipNow(%s) -> %lld\n", id, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SwitchInput_hasOffNow(void *unit, const void *id_value) {
    (void)unit;
    char id[64]; amd_id_to_ascii(id_value, id, sizeof(id));
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SwitchInput_hasOffNow(%s) -> %lld\n", id, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SwitchInput_hasOnNow(void *unit, const void *id_value) {
    (void)unit;
    char id[64]; amd_id_to_ascii(id_value, id, sizeof(id));
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SwitchInput_hasOnNow(%s) -> %lld\n", id, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SwitchInput_isFlipNow(void *unit, const void *id_value) {
    (void)unit;
    char id[64]; amd_id_to_ascii(id_value, id, sizeof(id));
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SwitchInput_isFlipNow(%s) -> %lld\n", id, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SwitchInput_isOffNow(void *unit, const void *id_value) {
    (void)unit;
    char id[64]; amd_id_to_ascii(id_value, id, sizeof(id));
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SwitchInput_isOffNow(%s) -> %lld\n", id, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SwitchInput_isOn(void *unit, const void *id_value) {
    (void)unit;
    char id[64]; amd_id_to_ascii(id_value, id, sizeof(id));
    static int count = 0;
    /* JVS player buttons (button_01..button_08) are active-low (_invert = true): idle HIGH = 1 */
    int64_t res = (strncmp(id, "button_", 7) == 0) ? 1 : 0;
    if (count < 24) { printf("[LinuxStub:amdaemon_api] SwitchInput_isOn(%p, %s) -> %lld\n", unit, id, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SwitchInput_isOnNow(void *unit, const void *id_value) {
    (void)unit;
    char id[64]; amd_id_to_ascii(id_value, id, sizeof(id));
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SwitchInput_isOnNow(%s) -> %lld\n", id, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SwitchOutput_isCurrentOn(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SwitchOutput_isCurrentOn() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SwitchOutput_reset(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SwitchOutput_reset() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t SwitchOutput_set(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] SwitchOutput_set() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t System_getAppRootPath(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] System_getAppRootPath() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t System_getBoardId(void) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (int64_t)&g_board_id_handle;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] System_getBoardId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t System_getGameId(void) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (int64_t)u16_gameid;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] System_getGameId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t System_getKeychipId(void) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (int64_t)&g_keychip_id_handle;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] System_getKeychipId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t System_getModelType(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] System_getModelType() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t System_getRamDiskPath(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] System_getRamDiskPath() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t System_getRegionCode(void) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (int64_t)g_region_code;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] System_getRegionCode() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t System_getResolution(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] System_getResolution() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t System_isDevelop(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] System_isDevelop() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t System_isMonitorConnected(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] System_isMonitorConnected() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t TestResult_toString(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] TestResult_toString() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t TimeHistogramItem_count_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] TimeHistogramItem_count_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t TimeHistogramItem_timeRangeMax_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] TimeHistogramItem_timeRangeMax_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t TimeHistogramItem_timeRangeMin_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] TimeHistogramItem_timeRangeMin_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbDevice_getLastReconnectResult(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbDevice_getLastReconnectResult() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbDevice_isReconnectBusy(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbDevice_isReconnectBusy() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbDevice_startReconnect(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbDevice_startReconnect() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOAnalogInput_getChannelCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOAnalogInput_getChannelCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOAnalogInput_getValidBitCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOAnalogInput_getValidBitCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOAnalogInput_getValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOAnalogInput_getValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOCoinInput_getCondition(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOCoinInput_getCondition() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOCoinInput_getSlotCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOCoinInput_getSlotCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOCoinInput_getValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOCoinInput_getValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOGeneralOutput_MaxBitCount_get(void) {
    static int count = 0;
    int64_t res = 64;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOGeneralOutput_MaxBitCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOGeneralOutput_getCurrentBits(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOGeneralOutput_getCurrentBits() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOGeneralOutput_resetBits(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOGeneralOutput_resetBits() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOGeneralOutput_setBit(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOGeneralOutput_setBit() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOGeneralOutput_setBits(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOGeneralOutput_setBits() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIONode_MaxBoardNumberLength_get(void) {
    static int count = 0;
    int64_t res = 8;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIONode_MaxBoardNumberLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIONode_MaxCustomChipNumberLength_get(void) {
    static int count = 0;
    int64_t res = 5;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIONode_MaxCustomChipNumberLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIONode_getAnalogInput(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIONode_getAnalogInput() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIONode_getBoardNumber(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIONode_getBoardNumber() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIONode_getCoinInput(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIONode_getCoinInput() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIONode_getCustomChipNumber(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIONode_getCustomChipNumber() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIONode_getFirmRevision(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIONode_getFirmRevision() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIONode_getGeneralOutput(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIONode_getGeneralOutput() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIONode_getPwmOutput(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIONode_getPwmOutput() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIONode_getRotaryInput(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIONode_getRotaryInput() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIONode_getSwitchInput(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIONode_getSwitchInput() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIONode_getUniqueInput(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIONode_getUniqueInput() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIONode_getUniqueOutput(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIONode_getUniqueOutput() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOPwmOutput_getCurrentDuties(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOPwmOutput_getCurrentDuties() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOPwmOutput_getCurrentDuty(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOPwmOutput_getCurrentDuty() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOPwmOutput_getSlotCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOPwmOutput_getSlotCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOPwmOutput_resetDuties(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOPwmOutput_resetDuties() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOPwmOutput_setDuties(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOPwmOutput_setDuties() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOPwmOutput_setDuty(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOPwmOutput_setDuty() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIORotaryInput_getChannelCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIORotaryInput_getChannelCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIORotaryInput_getValue(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIORotaryInput_getValue() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOSwitchInput_MaxPlayerBitCount_get(void) {
    static int count = 0;
    int64_t res = 64;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOSwitchInput_MaxPlayerBitCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOSwitchInput_getPlayerBits(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOSwitchInput_getPlayerBits() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOSwitchInput_getPlayerCount(void) {
    static int count = 0;
    int64_t res = 2;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOSwitchInput_getPlayerCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOSwitchInput_getPlayerFlipCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOSwitchInput_getPlayerFlipCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOSwitchInput_getTestFlipCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOSwitchInput_getTestFlipCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOSwitchInput_getTiltFlipCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOSwitchInput_getTiltFlipCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOSwitchInput_isPlayerOn(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOSwitchInput_isPlayerOn() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOSwitchInput_isTestOn(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOSwitchInput_isTestOn() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOSwitchInput_isTiltOn(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOSwitchInput_isTiltOn() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOUniqueInput_RawDataSize_get(void) {
    static int count = 0;
    int64_t res = 29;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOUniqueInput_RawDataSize_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOUniqueInput_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOUniqueInput_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOUniqueInput_getRaw(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOUniqueInput_getRaw() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOUniqueInput_get_dataSize(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOUniqueInput_get_dataSize() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOUniqueInput_isSupported(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOUniqueInput_isSupported() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOUniqueOutput_MaxDataSize_get(void) {
    static int count = 0;
    int64_t res = 62;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOUniqueOutput_MaxDataSize_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOUniqueOutput_getCurrent(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOUniqueOutput_getCurrent() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOUniqueOutput_isSupported(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOUniqueOutput_isSupported() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIOUniqueOutput_set(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIOUniqueOutput_set() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIO_getFirmUpdateProgress(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIO_getFirmUpdateProgress() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIO_getNode(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIO_getNode() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIO_getNodeCount(void) {
    static int count = 0;
    int64_t res = 2;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIO_getNodeCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIO_isAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIO_isAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t UsbIO_isFirmUpdating(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] UsbIO_isFirmUpdating() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Version_MajorLimit_get(void) {
    static int count = 0;
    int64_t res = 65535;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Version_MajorLimit_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Version_MinorLimit_get(void) {
    static int count = 0;
    int64_t res = 255;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Version_MinorLimit_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Version_PatchLimit_get(void) {
    static int count = 0;
    int64_t res = 255;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] Version_PatchLimit_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t Version_canMake_fromParts(uint32_t maj, uint32_t min, uint32_t pat) {
    return (maj <= 65535u && min <= 255u && pat <= 255u) ? 1 : 0;
}

int64_t Version_canMake_fromString(const uint16_t *s) {
    return (s != NULL && s[0] != 0) ? 1 : 0;
}

int64_t Version_major(uint32_t ver) {
    return (int64_t)((ver >> 16) & 0xFFFFu);
}

int64_t Version_make_fromParts(uint32_t maj, uint32_t min, uint32_t pat) {
    return (int64_t)(((maj & 0xFFFFu) << 16) | ((min & 0xFFu) << 8) | (pat & 0xFFu));
}

int64_t Version_make_fromString(const uint16_t *s) {
    if (!s) return 0;
    char buf[64];
    int i;
    for (i = 0; i < 63 && s[i]; i++) {
        buf[i] = (s[i] < 128) ? (char)s[i] : '\0';
    }
    buf[i] = '\0';
    unsigned int maj = 0, min = 0, pat = 0;
    if (sscanf(buf, "%u.%u.%u", &maj, &min, &pat) < 2) {
        sscanf(buf, "%u.%u", &maj, &min);
        pat = 0;
    }
    return (int64_t)(((maj & 0xFFFFu) << 16) | ((min & 0xFFu) << 8) | (pat & 0xFFu));
}

int64_t Version_minor(uint32_t ver) {
    return (int64_t)((ver >> 8) & 0xFFu);
}

int64_t Version_patch(uint32_t ver) {
    return (int64_t)(ver & 0xFFu);
}

int64_t Version_toString(uint32_t ver, int32_t show_patch) {
    unsigned int maj = (ver >> 16) & 0xFFFFu;
    unsigned int min = (ver >> 8) & 0xFFu;
    unsigned int pat = ver & 0xFFu;
    char buf[32];
    if (show_patch) {
        snprintf(buf, sizeof(buf), "%u.%02u.%u", maj, min, pat);
    } else {
        snprintf(buf, sizeof(buf), "%u.%02u", maj, min);
    }
    utf8_to_u16(buf, u16_ver_str, 32);
    return (int64_t)u16_ver_str;
}

int64_t abaas_Log_MaxErrorMessageUtf8Size_get(void) {
    static int count = 0;
    int64_t res = 1024;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] abaas_Log_MaxErrorMessageUtf8Size_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t abaas_Log_getRootPath(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] abaas_Log_getRootPath() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t abaas_Log_getUploadingFileCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] abaas_Log_getUploadingFileCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t abaas_Log_isAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] abaas_Log_isAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t abaas_Log_isServerAlive(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] abaas_Log_isServerAlive() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t abaas_Log_isUploading(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] abaas_Log_isUploading() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t abaas_Log_putErrorMessage(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] abaas_Log_putErrorMessage() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingPlayCountItem_count_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingPlayCountItem_count_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingPlayCountItem_month_get(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingPlayCountItem_month_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingPlayCountItem_toString(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingPlayCountItem_toString() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingPlayCountItem_valid(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingPlayCountItem_valid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingUnit_ItemCountLimit_get(void) {
    static int count = 0;
    int64_t res = 255;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingUnit_ItemCountLimit_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingUnit_KindCodeLimit_get(void) {
    static int count = 0;
    int64_t res = 654;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingUnit_KindCodeLimit_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingUnit_MaxGeneralIdLength_get(void) {
    static int count = 0;
    int64_t res = 127;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingUnit_MaxGeneralIdLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingUnit_QuantityLimit_get(void) {
    static int count = 0;
    int64_t res = 7679;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingUnit_QuantityLimit_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingUnit_StatusCodeLimit_get(void) {
    static int count = 0;
    int64_t res = 99;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingUnit_StatusCodeLimit_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingUnit_accountItem(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingUnit_accountItem() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingUnit_beginPlay(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingUnit_beginPlay() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingUnit_canBeginPlay(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingUnit_canBeginPlay() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingUnit_continuePlay(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingUnit_continuePlay() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingUnit_endPlay(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingUnit_endPlay() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingUnit_putGeneralId(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingUnit_putGeneralId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_AccountingUnit_putQuantity(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_AccountingUnit_putQuantity() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Accounting_getBackgroundReportTime(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Accounting_getBackgroundReportTime() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Accounting_getMode(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Accounting_getMode() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Accounting_getPlayCountItem(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Accounting_getPlayCountItem() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Accounting_getPlayer(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Accounting_getPlayer() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Accounting_getPlayerCount(void) {
    static int count = 0;
    int64_t res = 2;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Accounting_getPlayerCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Accounting_getReportTime(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Accounting_getReportTime() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Accounting_getSpanUntilReport(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Accounting_getSpanUntilReport() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Accounting_isAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Accounting_isAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Accounting_isInfoAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Accounting_isInfoAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Accounting_isLogFull(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Accounting_isLogFull() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Accounting_isNearFullEnabled(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Accounting_isNearFullEnabled() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Accounting_isPlayable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Accounting_isPlayable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Accounting_isReporting(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Accounting_isReporting() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Accounting_setNearFullEnabled(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Accounting_setNearFullEnabled() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Air_MaxSignalLevel_get(void) {
    static int count = 0;
    int64_t res = 4;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Air_MaxSignalLevel_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Air_MaxSsidLength_get(void) {
    static int count = 0;
    int64_t res = 32;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Air_MaxSsidLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Air_clearSetting(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Air_clearSetting() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Air_getSignalLevel(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Air_getSignalLevel() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Air_getSsid(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Air_getSsid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Air_getStatus(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Air_getStatus() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Air_getValidatingLocationAddress(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Air_getValidatingLocationAddress() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Air_isAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Air_isAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Air_isConnected(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Air_isConnected() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Air_isValidationNeeded(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Air_isValidationNeeded() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Air_notifyValidationResult(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Air_notifyValidationResult() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_LocationNicknamePartCount_get(void) {
    static int count = 0;
    int64_t res = 2;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_LocationNicknamePartCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_MaxLocationNameLength_get(void) {
    static int count = 0;
    int64_t res = 60;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_MaxLocationNameLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_MaxLocationNicknamePartLength_get(void) {
    static int count = 0;
    int64_t res = 40;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_MaxLocationNicknamePartLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_MaxRegionNamePartLength_get(void) {
    static int count = 0;
    int64_t res = 48;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_MaxRegionNamePartLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_RegionNamePartCount_get(void) {
    static int count = 0;
    int64_t res = 4;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_RegionNamePartCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_getAuthData(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_getAuthData() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_getAuthTime(void) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (int64_t)__atomic_load_n(&g_auth_time_us, __ATOMIC_ACQUIRE);
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_getAuthTime() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_getCountryCode(void) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (int64_t)u16_auth_country;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_getCountryCode() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_getGameServerHost(void) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (int64_t)u16_auth_host;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_getGameServerHost() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_getGameServerUri(void) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (int64_t)u16_auth_uri;
    if (count < 5) { printf("[LinuxStub:amdaemon_api] allnet_Auth_getGameServerUri() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_getLineType(void) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (int64_t)g_auth_line_type;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_getLineType() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_getLocationId(void) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (int64_t)g_auth_location_id;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_getLocationId() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_getLocationName(void) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (int64_t)u16_auth_name;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_getLocationName() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_getLocationNickname(int32_t index) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (index == 0) ? (int64_t)u16_auth_nickname : (int64_t)u16_empty;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_getLocationNickname(%d) -> %lld\n", index, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_getRegionCode(void) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (int64_t)g_auth_region_code;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_getRegionCode() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_getRegionName(int32_t index) {
    static int count = 0;
    init_net_system_once();
    int64_t res = (index >= 0 && index < 4) ? (int64_t)u16_auth_region_names[index] : (int64_t)u16_empty;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_getRegionName(%d) -> %lld\n", index, (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_isAvailable(void) {
    static int count = 0;
    int64_t res = 1;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_isAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_isDevelop(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_Auth_isDevelop() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_Auth_isGood(void) {
    static int count = 0;
    init_net_system_once();
    int64_t res = __atomic_load_n(&g_auth_good, __ATOMIC_ACQUIRE) ? 1 : 0;
    if (count < 5) { printf("[LinuxStub:amdaemon_api] allnet_Auth_isGood() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFiUnit_MaxFirmVersionLength_get(void) {
    static int count = 0;
    int64_t res = 20;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFiUnit_MaxFirmVersionLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFiUnit_SerialLength_get(void) {
    static int count = 0;
    int64_t res = 12;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFiUnit_SerialLength_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFiUnit_getAccessCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFiUnit_getAccessCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFiUnit_getAuthGoodText(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFiUnit_getAuthGoodText() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFiUnit_getFirmVersion(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFiUnit_getFirmVersion() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFiUnit_getMasterSerial(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFiUnit_getMasterSerial() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFiUnit_getSerial(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFiUnit_getSerial() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFiUnit_getServerAliveText(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFiUnit_getServerAliveText() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFiUnit_isAuthGood(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFiUnit_isAuthGood() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFiUnit_isCache(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFiUnit_isCache() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFiUnit_isServerAlive(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFiUnit_isServerAlive() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFiUnit_valid(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFiUnit_valid() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFi_MaxUnitCount_get(void) {
    static int count = 0;
    int64_t res = 10;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFi_MaxUnitCount_get() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFi_clearUnitCache(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFi_clearUnitCache() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFi_getUnit(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFi_getUnit() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFi_getUnitCount(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFi_getUnitCount() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFi_isAvailable(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFi_isAvailable() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFi_isUnitCacheSaved(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFi_isUnitCacheSaved() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t allnet_WiFi_saveUnitCache(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] allnet_WiFi_saveUnitCache() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpAbaasLog(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpAbaasLog() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpAime(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpAime() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpAimePay(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpAimePay() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpAll(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpAll() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpAllFromBinary(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpAllFromBinary() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpAllnetAccounting(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpAllnetAccounting() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpAllnetAir(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpAllnetAir() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpAllnetAuth(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpAllnetAuth() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpAllnetWiFi(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpAllnetWiFi() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpAppImage(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpAppImage() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpBackup(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpBackup() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpBinary(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpBinary() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpBoardIO(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpBoardIO() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpCan(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpCan() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpCommon(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpCommon() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpCredit(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpCredit() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpEMoney(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpEMoney() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpError(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpError() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpInput(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpInput() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpJvs(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpJvs() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpLanInstall(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpLanInstall() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpNetDelivery(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpNetDelivery() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpNetwork(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpNetwork() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpOutput(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpOutput() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpProcess(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpProcess() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpSequence(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpSequence() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpSystem(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpSystem() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpUsbDevice(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpUsbDevice() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_dumpUsbIO(void) {
    static int count = 0;
    int64_t res = 0;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_dumpUsbIO() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

int64_t dump_getDumpBinarySize(void) {
    static int count = 0;
    int64_t res = (int64_t)dummy_struct;
    if (count < 3) { printf("[LinuxStub:amdaemon_api] dump_getDumpBinarySize() -> %lld\n", (long long)res); fflush(stdout); count++; }
    return res;
}

