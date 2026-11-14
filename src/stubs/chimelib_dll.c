#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <limits.h>

/* ============================================================================
 * Logging Helper (writes to /tmp/amd_stub.log + stdout)
 * ============================================================================ */

static pthread_mutex_t g_chime_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void chime_log(const char *fmt, ...) {
    pthread_mutex_lock(&g_chime_log_mutex);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_val;
    localtime_r(&ts.tv_sec, &tm_val);

    FILE *fp = fopen("/tmp/amd_stub.log", "a");
    if (fp) {
        fprintf(fp, "[%02d:%02d:%02d.%03ld] [chimelib_dll] ",
                tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec, ts.tv_nsec / 1000000);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(fp, fmt, ap);
        va_end(ap);
        fprintf(fp, "\n");
        fclose(fp);
    }

    printf("[LinuxStub:chimelib_dll] ");
    va_list ap2;
    va_start(ap2, fmt);
    vprintf(fmt, ap2);
    va_end(ap2);
    printf("\n");
    fflush(stdout);
    pthread_mutex_unlock(&g_chime_log_mutex);
}

/* ============================================================================
 * Self-contained FIPS-180-4 SHA-256 (avoids OpenSSL symbol collisions)
 * ============================================================================ */

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t buffer[64];
} chime_sha256_ctx;

static const uint32_t k_sha256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static inline uint32_t rotr32(uint32_t x, unsigned int n) {
    return (x >> n) | (x << (32u - n));
}

static void chime_sha256_transform(chime_sha256_ctx *ctx, const uint8_t data[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i * 4 + 0] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               ((uint32_t)data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + k_sha256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void chime_sha256_upper_hex(const char *input, char out_hex[65]) {
    chime_sha256_ctx ctx;
    ctx.state[0] = 0x6a09e667u;
    ctx.state[1] = 0xbb67ae85u;
    ctx.state[2] = 0x3c6ef372u;
    ctx.state[3] = 0xa54ff53au;
    ctx.state[4] = 0x510e527fu;
    ctx.state[5] = 0x9b05688cu;
    ctx.state[6] = 0x1f83d9abu;
    ctx.state[7] = 0x5be0cd19u;
    ctx.bitcount = 0;

    const uint8_t *data = (const uint8_t *)(input ? input : "");
    size_t len = strlen((const char *)data);
    size_t idx = 0;

    while (len >= 64) {
        chime_sha256_transform(&ctx, data + idx);
        ctx.bitcount += 512;
        idx += 64;
        len -= 64;
    }

    memset(ctx.buffer, 0, sizeof(ctx.buffer));
    if (len > 0) {
        memcpy(ctx.buffer, data + idx, len);
        ctx.bitcount += (uint64_t)len * 8u;
    }
    ctx.buffer[len] = 0x80u;
    if (len >= 56) {
        chime_sha256_transform(&ctx, ctx.buffer);
        memset(ctx.buffer, 0, sizeof(ctx.buffer));
    }
    for (int i = 0; i < 8; i++) {
        ctx.buffer[63 - i] = (uint8_t)((ctx.bitcount >> (i * 8)) & 0xFFu);
    }
    chime_sha256_transform(&ctx, ctx.buffer);

    static const char hex_tbl[] = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) {
        uint32_t v = ctx.state[i];
        out_hex[i * 8 + 0] = hex_tbl[(v >> 28) & 0xFu];
        out_hex[i * 8 + 1] = hex_tbl[(v >> 24) & 0xFu];
        out_hex[i * 8 + 2] = hex_tbl[(v >> 20) & 0xFu];
        out_hex[i * 8 + 3] = hex_tbl[(v >> 16) & 0xFu];
        out_hex[i * 8 + 4] = hex_tbl[(v >> 12) & 0xFu];
        out_hex[i * 8 + 5] = hex_tbl[(v >> 8) & 0xFu];
        out_hex[i * 8 + 6] = hex_tbl[(v >> 4) & 0xFu];
        out_hex[i * 8 + 7] = hex_tbl[v & 0xFu];
    }
    out_hex[64] = '\0';
}

/* ============================================================================
 * UTF-16LE (LPWStr) to UTF-8 conversion
 * ============================================================================ */

static void chime_utf16_to_utf8(const uint16_t *src, char *dst, size_t cap) {
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t out = 0;
    for (size_t i = 0; src[i] != 0; i++) {
        uint32_t cp = src[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu && src[i + 1] >= 0xDC00u && src[i + 1] <= 0xDFFFu) {
            cp = 0x10000u + (((cp - 0xD800u) << 10) | (src[i + 1] - 0xDC00u));
            i++;
        }
        if (cp < 0x80u) {
            if (out + 1 >= cap) break;
            dst[out++] = (char)cp;
        } else if (cp < 0x800u) {
            if (out + 2 >= cap) break;
            dst[out++] = (char)(0xC0u | (cp >> 6));
            dst[out++] = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            if (out + 3 >= cap) break;
            dst[out++] = (char)(0xE0u | (cp >> 12));
            dst[out++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            dst[out++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            if (out + 4 >= cap) break;
            dst[out++] = (char)(0xF0u | (cp >> 18));
            dst[out++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            dst[out++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            dst[out++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    dst[out] = '\0';
}

/* ============================================================================
 * Out-of-process curl helper (avoids OpenSSL symbol collisions with Unity)
 * ============================================================================ */

static const char *const k_server_urls[2] = {
    "https://ai.sys-allnet.cn/",
    "https://ai.sys-all.cn/"
};

static const char k_common_key[] = "XcW5FW4cPArBXEk4vzKz3CIrMuA5EVVW";

static int chime_http_post(const char *url, const char *post_body, size_t body_len,
                           int timeout_sec, char *resp_buf, size_t resp_cap) {
    if (resp_buf && resp_cap > 0) resp_buf[0] = '\0';

    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0) return -1;
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        unsetenv("LD_PRELOAD");
        unsetenv("LD_LIBRARY_PATH");
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        int devnull = open("/dev/null", 2);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        char t_str[16], ct_str[16];
        snprintf(t_str, sizeof(t_str), "%d", timeout_sec > 0 ? timeout_sec : 10);
        snprintf(ct_str, sizeof(ct_str), "%d", 5);

        const char *argv[24];
        int ac = 0;
        argv[ac++] = "/usr/bin/curl";
        argv[ac++] = "-s";
        argv[ac++] = "-S";
        argv[ac++] = "-k";
        argv[ac++] = "--http1.1";
        argv[ac++] = "--connect-timeout";
        argv[ac++] = ct_str;
        argv[ac++] = "-m";
        argv[ac++] = t_str;
        argv[ac++] = "-X";
        argv[ac++] = "POST";
        argv[ac++] = "-A";
        argv[ac++] = "WC_AIME_LIB";
        argv[ac++] = "-H";
        argv[ac++] = "Content-Type: application/json";
        argv[ac++] = "-H";
        argv[ac++] = "Expect:";
        argv[ac++] = "--data-binary";
        argv[ac++] = "@-";
        argv[ac++] = url;
        argv[ac++] = NULL;

        execv("/usr/bin/curl", (char *const *)argv);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);

    size_t written = 0;
    while (written < body_len) {
        ssize_t w = write(in_pipe[1], post_body + written, body_len - written);
        if (w <= 0) break;
        written += (size_t)w;
    }
    close(in_pipe[1]);

    size_t total = 0;
    if (resp_buf && resp_cap > 1) {
        while (total + 1 < resp_cap) {
            ssize_t r = read(out_pipe[0], resp_buf + total, resp_cap - 1 - total);
            if (r <= 0) break;
            total += (size_t)r;
        }
        resp_buf[total] = '\0';
    }
    close(out_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return (int)total;
}

/* ============================================================================
 * Minimal JSON Field Extractors for wc_aime/api/get_data response
 * ============================================================================ */

static const char *find_json_key(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t plen = strlen(pat);
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        const char *after = p + plen;
        while (*after && isspace((unsigned char)*after)) after++;
        if (*after == ':') {
            after++;
            while (*after && isspace((unsigned char)*after)) after++;
            return after;
        }
        p += plen;
    }
    return NULL;
}

static int json_get_int64(const char *json, const char *key, int64_t *out_val) {
    const char *v = find_json_key(json, key);
    if (!v) return 0;
    if (*v == '"') v++;
    char *endp = NULL;
    long long parsed = strtoll(v, &endp, 10);
    if (endp == v) return 0;
    if (out_val) *out_val = (int64_t)parsed;
    return 1;
}

static int json_get_string(const char *json, const char *key, char *out_buf, size_t cap) {
    if (out_buf && cap > 0) out_buf[0] = '\0';
    const char *v = find_json_key(json, key);
    if (!v || *v != '"') return 0;
    v++;
    size_t i = 0;
    while (*v && *v != '"') {
        if (*v == '\\' && v[1] != '\0') {
            v++;
        }
        if (out_buf && i + 1 < cap) {
            out_buf[i++] = *v;
        }
        v++;
    }
    if (out_buf && cap > 0) out_buf[i] = '\0';
    return (*v == '"') ? 1 : 0;
}

/* ============================================================================
 * Server ErrorID -> ChimeErrorId mapping (matches chimelib_dll.dll 0x180003948)
 * ============================================================================ */

static int32_t map_server_error_id(int64_t srv_err) {
    switch (srv_err) {
        case 0:  return 0;  /* None */
        case 1:  return 3;  /* ReaderIncompatible */
        case 2:  return 7;  /* AimeIdInvalid */
        case 3:  return 9;  /* LockBanSystemUser */
        case 4:  return 10; /* LockBanSystem */
        case 5:  return 11; /* LockBanUser */
        case 6:  return 12; /* LockBan */
        case 7:  return 13; /* LockSystemUser */
        case 8:  return 14; /* LockSystem */
        case 9:  return 15; /* LockUser */
        case 50: return 8;  /* NoBoardInfo */
        default: return 6;  /* DBAccessFail */
    }
}

/* ============================================================================
 * CCommServAliveCheck Implementation
 * ============================================================================ */

typedef struct {
    volatile int in_use;
    volatile int is_alive;
    volatile int is_busy;
    volatile int in_play;
    volatile int connect_permission;
    volatile uint64_t server_index;
} CCommServAliveCheckInst;

#define MAX_ALIVE_INSTANCES 16
static CCommServAliveCheckInst g_alive_pool[MAX_ALIVE_INSTANCES];
static pthread_mutex_t g_alive_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *alive_check_thread(void *arg) {
    CCommServAliveCheckInst *inst = (CCommServAliveCheckInst *)arg;
    char resp[512];
    int found_alive = 0;

    for (uint64_t idx = 0; idx < 2; idx++) {
        char url[256];
        snprintf(url, sizeof(url), "%swc_aime/api/alive_check", k_server_urls[idx]);
        int n = chime_http_post(url, "", 0, 4, resp, sizeof(resp));
        if (n >= 5 && strstr(resp, "alive") != NULL) {
            inst->server_index = idx;
            inst->is_alive = 1;
            found_alive = 1;
            chime_log("AliveCheck OK on index %llu (%s)", (unsigned long long)idx, url);
            break;
        }
    }

    if (!found_alive) {
        /* Default to index 0 and mark alive so transient ping hiccups never disable the QR reader */
        inst->server_index = 0;
        inst->is_alive = 1;
        chime_log("AliveCheck fallback: marking alive on index 0");
    }

    inst->is_busy = 0;
    return NULL;
}

void *CCommServAliveCheck_Create(void) {
    pthread_mutex_lock(&g_alive_mutex);
    CCommServAliveCheckInst *inst = NULL;
    for (int i = 0; i < MAX_ALIVE_INSTANCES; i++) {
        if (!g_alive_pool[i].in_use) {
            inst = &g_alive_pool[i];
            break;
        }
    }
    if (!inst) inst = &g_alive_pool[0];
    inst->in_use = 1;
    inst->is_alive = 1;
    inst->is_busy = 0;
    inst->in_play = 0;
    inst->connect_permission = 1;
    inst->server_index = 0;
    pthread_mutex_unlock(&g_alive_mutex);

    chime_log("CCommServAliveCheck_Create() -> %p", (void *)inst);
    return (void *)inst;
}

int32_t CCommServAliveCheck_Destroy(void *ptr) {
    CCommServAliveCheckInst *inst = (CCommServAliveCheckInst *)ptr;
    chime_log("CCommServAliveCheck_Destroy(%p)", ptr);
    if (inst) {
        inst->in_use = 0;
    }
    return 1;
}

int32_t CCommServAliveCheck_isAlive(void *ptr) {
    CCommServAliveCheckInst *inst = (CCommServAliveCheckInst *)ptr;
    return inst ? inst->is_alive : 1;
}

void CCommServAliveCheck_beginPlay(void *ptr) {
    CCommServAliveCheckInst *inst = (CCommServAliveCheckInst *)ptr;
    if (inst) inst->in_play = 1;
}

void CCommServAliveCheck_endPlay(void *ptr) {
    CCommServAliveCheckInst *inst = (CCommServAliveCheckInst *)ptr;
    if (inst) inst->in_play = 0;
}

void CCommServAliveCheck_startCheck(void *ptr) {
    CCommServAliveCheckInst *inst = (CCommServAliveCheckInst *)ptr;
    if (!inst) return;
    if (!inst->connect_permission || inst->in_play || inst->is_busy) {
        return;
    }
    inst->is_busy = 1;
    pthread_t tid;
    if (pthread_create(&tid, NULL, alive_check_thread, inst) == 0) {
        pthread_detach(tid);
    } else {
        inst->is_alive = 1;
        inst->is_busy = 0;
    }
}

void CCommServAliveCheck_cancelCheck(void *ptr) {
    CCommServAliveCheckInst *inst = (CCommServAliveCheckInst *)ptr;
    if (inst) inst->is_busy = 0;
}

void CCommServAliveCheck_setCheckResult(void *ptr, int32_t flag) {
    CCommServAliveCheckInst *inst = (CCommServAliveCheckInst *)ptr;
    if (inst) inst->is_alive = (flag != 0) ? 1 : 0;
}

int32_t CCommServAliveCheck_isBusy(void *ptr) {
    CCommServAliveCheckInst *inst = (CCommServAliveCheckInst *)ptr;
    return inst ? inst->is_busy : 0;
}

void CCommServAliveCheck_setConnectPermission(void *ptr, int32_t flag) {
    CCommServAliveCheckInst *inst = (CCommServAliveCheckInst *)ptr;
    if (inst) inst->connect_permission = (flag != 0) ? 1 : 0;
}

void CCommServAliveCheck_execute(void *ptr) {
    (void)ptr;
}

uint64_t CCommServAliveCheck_getIndex(void *ptr) {
    CCommServAliveCheckInst *inst = (CCommServAliveCheckInst *)ptr;
    return inst ? inst->server_index : 0;
}

/* ============================================================================
 * CCommGetUserData Implementation
 * ============================================================================ */

typedef struct {
    volatile int state;        /* 0 = init, 1 = busy, 2 = ended */
    volatile int32_t error_id; /* ChimeErrorId */
    volatile uint32_t user_id; /* AimeId / UserID */
    uint64_t server_index;
    time_t start_time;
    char game_id[64];
    char chip_id[64];
    char common_key[128];
    char qr_data[256];
    char title_key[64];
    char token[512];
} CCommGetUserDataInst;

#define MAX_USERDATA_INSTANCES 32
static CCommGetUserDataInst g_userdata_pool[MAX_USERDATA_INSTANCES];
static uint32_t g_userdata_next = 0;
static pthread_mutex_t g_userdata_mutex = PTHREAD_MUTEX_INITIALIZER;

static void make_short_chip_id(const char *in, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < cap && j < 11; i++) {
        if (isalnum((unsigned char)in[i])) {
            out[j++] = (char)toupper((unsigned char)in[i]);
        }
    }
    out[j] = '\0';
}

static int perform_get_data_request(const char *base_url,
                                    const char *chip_id,
                                    const char *game_id,
                                    const char *qr_data,
                                    const char *title_key,
                                    const char *common_key,
                                    char *resp_buf,
                                    size_t resp_cap) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_val;
    localtime_r(&ts.tv_sec, &tm_val);

    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%02d%02d%02d%02d%02d%02d",
             (tm_val.tm_year % 100),
             tm_val.tm_mon + 1,
             tm_val.tm_mday,
             tm_val.tm_hour,
             tm_val.tm_min,
             tm_val.tm_sec);

    char sign_input[512];
    snprintf(sign_input, sizeof(sign_input), "%s%s%s", chip_id, timestamp, common_key);

    char key_hex[65];
    chime_sha256_upper_hex(sign_input, key_hex);

    char post_json[1024];
    int json_len = snprintf(post_json, sizeof(post_json),
                            "{\"chipID\":\"%s\",\"openGameID\":\"%s\",\"key\":\"%s\","
                            "\"qrCode\":\"%s\",\"timestamp\":\"%s\",\"titlekey\":\"%s\"}",
                            chip_id, game_id, key_hex, qr_data, timestamp, title_key);
    if (json_len < 0 || (size_t)json_len >= sizeof(post_json)) {
        return -1;
    }

    char url[256];
    snprintf(url, sizeof(url), "%swc_aime/api/get_data", base_url);

    chime_log("POST %s body=%s", url, post_json);
    int n = chime_http_post(url, post_json, (size_t)json_len, 10, resp_buf, resp_cap);
    if (n <= 0) {
        chime_log("POST %s failed (n=%d)", url, n);
        return -1;
    }
    chime_log("RESP (%d bytes): %s", n, resp_buf);
    return n;
}

static void *get_user_data_thread(void *arg) {
    CCommGetUserDataInst *inst = (CCommGetUserDataInst *)arg;

    uint64_t primary_idx = (inst->server_index < 2) ? inst->server_index : 0;
    uint64_t fallback_idx = (primary_idx == 0) ? 1 : 0;

    char resp[4096];
    const char *used_chip_id = inst->chip_id;

    /* Note: AMDaemon SerialId.ShortValue is 11 chars (e.g. A63E01E0000) while SerialId.Value is 16 chars
     * (e.g. A63E-01E00000000). Let's prepare the 11-char short chipID as well in case the server expects 11 chars. */
    char short_chip[32];
    make_short_chip_id(inst->chip_id, short_chip, sizeof(short_chip));

    /* Wait: What does Wahlap's wc_aime/api/get_data expect in chipID?
     * On real Windows cabinets / segatools, SerialId.Value in segatools or amdaemon is passed.
     * Wait: in segatools, keychip id in [keychip] id=A63E-01E00000000 has '-' stripped by segatools into
     * 11-char shortId A63E01E0000! Let's check: if server returns errorID=50 (NoBoardInfo) or errorID=1,
     * we can try short_chip (or try short_chip first if chip_id has '-').
     * Wait: why not check what errorID 50 means? Error 50 is literally "NoBoardInfo" (chipID not found),
     * which does NOT consume the QR code! */
    int n = perform_get_data_request(k_server_urls[primary_idx],
                                     used_chip_id,
                                     inst->game_id,
                                     inst->qr_data,
                                     inst->title_key,
                                     inst->common_key,
                                     resp, sizeof(resp));
    if (n <= 0) {
        n = perform_get_data_request(k_server_urls[fallback_idx],
                                     used_chip_id,
                                     inst->game_id,
                                     inst->qr_data,
                                     inst->title_key,
                                     inst->common_key,
                                     resp, sizeof(resp));
    }

    if (n <= 0) {
        inst->user_id = 0xFFFFFFFFu;
        inst->token[0] = '\0';
        inst->error_id = 6; /* DBAccessFail */
        __sync_synchronize();
        inst->state = 2;
        return NULL;
    }

    int64_t srv_err = -1;
    int64_t srv_uid = -1;
    char srv_key[128] = {0};
    char srv_ts[64] = {0};
    char srv_token[512] = {0};

    int has_err = json_get_int64(resp, "errorID", &srv_err);
    int has_uid = json_get_int64(resp, "userID", &srv_uid);
    json_get_string(resp, "key", srv_key, sizeof(srv_key));
    json_get_string(resp, "timestamp", srv_ts, sizeof(srv_ts));
    json_get_string(resp, "token", srv_token, sizeof(srv_token));

    /* If server rejected chipID with NoBoardInfo (50) and chip_id had '-', retry with 11-char short_chip! */
    if (has_err && srv_err == 50 && strcmp(inst->chip_id, short_chip) != 0 && short_chip[0] != '\0') {
        chime_log("Server returned errorID=50 (NoBoardInfo) for chipID='%s'; retrying with short chipID='%s'",
                  inst->chip_id, short_chip);
        n = perform_get_data_request(k_server_urls[primary_idx],
                                     short_chip,
                                     inst->game_id,
                                     inst->qr_data,
                                     inst->title_key,
                                     inst->common_key,
                                     resp, sizeof(resp));
        if (n > 0) {
            has_err = json_get_int64(resp, "errorID", &srv_err);
            has_uid = json_get_int64(resp, "userID", &srv_uid);
            json_get_string(resp, "key", srv_key, sizeof(srv_key));
            json_get_string(resp, "timestamp", srv_ts, sizeof(srv_ts));
            json_get_string(resp, "token", srv_token, sizeof(srv_token));
        }
    }

    if (!has_err || !has_uid) {
        chime_log("Invalid JSON response (missing errorID or userID): %s", resp);
        inst->user_id = 0xFFFFFFFFu;
        inst->token[0] = '\0';
        inst->error_id = 6; /* DBAccessFail */
        __sync_synchronize();
        inst->state = 2;
        return NULL;
    }

    if (srv_err == 0) {
        uint32_t uid_u32 = (uint32_t)srv_uid;
        char verify_in[256];
        snprintf(verify_in, sizeof(verify_in), "%u%s%s", uid_u32, srv_ts, inst->common_key);
        char expected_key[65];
        chime_sha256_upper_hex(verify_in, expected_key);

        if (srv_key[0] != '\0' && strcasecmp(srv_key, expected_key) != 0) {
            chime_log("WARNING: response key mismatch! srv_key='%s' expected='%s' (proceeding anyway)",
                      srv_key, expected_key);
        }

        inst->user_id = uid_u32;
        snprintf(inst->token, sizeof(inst->token), "%s", srv_token);
        inst->error_id = 0;
        chime_log("QR Login SUCCESS! userID=%u token_len=%zu", uid_u32, strlen(inst->token));
    } else {
        inst->user_id = 0xFFFFFFFFu;
        inst->token[0] = '\0';
        inst->error_id = map_server_error_id(srv_err);
        chime_log("QR Login server error: srv_errorID=%lld -> mapped ChimeErrorId=%d",
                  (long long)srv_err, inst->error_id);
    }

    __sync_synchronize();
    inst->state = 2;
    return NULL;
}

void *CCommGetUserData_Create(const uint16_t *strGameID,
                              const uint16_t *strChipID,
                              const uint16_t *strCommonKey,
                              const uint16_t *strQRData,
                              const uint16_t *strTitleKey,
                              uint64_t serverURLIndex) {
    pthread_mutex_lock(&g_userdata_mutex);
    CCommGetUserDataInst *inst = &g_userdata_pool[g_userdata_next % MAX_USERDATA_INSTANCES];
    g_userdata_next++;
    memset(inst, 0, sizeof(*inst));
    inst->state = 0;
    inst->error_id = 0;
    inst->user_id = 0xFFFFFFFFu;
    inst->server_index = serverURLIndex;
    inst->start_time = time(NULL);

    chime_utf16_to_utf8(strGameID, inst->game_id, sizeof(inst->game_id));
    chime_utf16_to_utf8(strChipID, inst->chip_id, sizeof(inst->chip_id));
    chime_utf16_to_utf8(strCommonKey, inst->common_key, sizeof(inst->common_key));
    chime_utf16_to_utf8(strQRData, inst->qr_data, sizeof(inst->qr_data));
    chime_utf16_to_utf8(strTitleKey, inst->title_key, sizeof(inst->title_key));

    /* Just like chimelib_dll.dll (0x18000307d), overwrite common_key with the hardcoded Chime key */
    snprintf(inst->common_key, sizeof(inst->common_key), "%s", k_common_key);
    pthread_mutex_unlock(&g_userdata_mutex);

    chime_log("CCommGetUserData_Create(gameID='%s', chipID='%s', qrLen=%zu, titleKey='%s', urlIdx=%llu) -> %p",
              inst->game_id, inst->chip_id, strlen(inst->qr_data), inst->title_key,
              (unsigned long long)serverURLIndex, (void *)inst);
    return (void *)inst;
}

int32_t CCommGetUserData_Destroy(void *ptr) {
    chime_log("CCommGetUserData_Destroy(%p)", ptr);
    return 1;
}

void CCommGetUserData_execute(void *ptr) {
    CCommGetUserDataInst *inst = (CCommGetUserDataInst *)ptr;
    if (!inst) return;

    if (__sync_bool_compare_and_swap(&inst->state, 0, 1)) {
        inst->start_time = time(NULL);
        pthread_t tid;
        if (pthread_create(&tid, NULL, get_user_data_thread, inst) == 0) {
            pthread_detach(tid);
        } else {
            inst->user_id = 0xFFFFFFFFu;
            inst->error_id = 6; /* DBAccessFail */
            inst->state = 2;
        }
    } else if (inst->state == 1) {
        time_t now = time(NULL);
        if (now - inst->start_time > 15) {
            chime_log("CCommGetUserData_execute(%p) timed out after 15s", ptr);
            inst->user_id = 0xFFFFFFFFu;
            inst->token[0] = '\0';
            inst->error_id = 5; /* DBAccessTimeout */
            __sync_synchronize();
            inst->state = 2;
        }
    }
}

int32_t CCommGetUserData_isEnd(void *ptr) {
    CCommGetUserDataInst *inst = (CCommGetUserDataInst *)ptr;
    if (!inst) return 1;
    return (inst->state == 2) ? 1 : 0;
}

int32_t CCommGetUserData_getErrorID(void *ptr) {
    CCommGetUserDataInst *inst = (CCommGetUserDataInst *)ptr;
    if (!inst) return 6;
    return inst->error_id;
}

uint32_t CCommGetUserData_getUserID(void *ptr) {
    CCommGetUserDataInst *inst = (CCommGetUserDataInst *)ptr;
    if (!inst) return 0xFFFFFFFFu;
    return inst->user_id;
}

const char *CCommGetUserData_getToken(void *ptr) {
    CCommGetUserDataInst *inst = (CCommGetUserDataInst *)ptr;
    if (!inst) return "";
    return inst->token;
}
