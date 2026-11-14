#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cri_audio.h"
#include "cri_video.h"

static uint64_t dummy_mem[4096] = {0};
static pthread_mutex_t g_cri_lock = PTHREAD_MUTEX_INITIALIZER;

static void cri_log(const char *fmt, ...) {
    FILE *f = fopen("/tmp/cri_stub.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

static int64_t cri_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000LL);
}

/* ---- Callbacks passed to Unity / CriWare ---- */
static void *cri_alloc_func(void *obj, uint32_t size) {
    (void)obj;
    return calloc(1, size ? size : 1);
}

static void cri_free_func(void *obj, void *ptr) {
    (void)obj;
    free(ptr);
}

static void cri_err_cb_func(const char *errid, uint32_t p1, uint32_t p2, uint32_t *parray) {
    (void)p1; (void)p2; (void)parray;
    if (errid) cri_log("[CriErrCb] %s\n", errid);
}

/* ---- Global library init & decrypter state ---- */
static int g_atom_initialized = 0;
static int g_mana_initialized = 0;
static int g_fs_initialized = 0;
static uint64_t g_cri_decrypter_key = 0;
static int g_cri_decrypter_atom = 0;
static int g_cri_decrypter_mana = 0;
static int32_t g_mana_event_offset = 0;

/* ---- CriMana.Player state machine + GL texture clear ---- */
#define MAX_MANA_PLAYERS 32
typedef struct {
    int used;
    int status; /* 0=Stop, 1=Prep, 2=Ready, 3=WaitPrep, 4=ReadyWait, 5=Playing, 6=PlayEnd, 7=Error */
    int paused;
    int loop;
    int has_file;
    char path[512];
    int64_t start_ms;
    int64_t paused_elapsed_ms;
    uint32_t seek_frame;
    uint32_t frame_no;
    void *cue_cb;
    void *atom_player;
    uint32_t width;
    uint32_t height;
    uint32_t disp_width;
    uint32_t disp_height;
    uint32_t fps_n;
    uint32_t fps_d;
    uint32_t total_frames;
    void *tex_ptrs[4];
    int num_tex;
    int tex_dirty;
    CriVideoStream *video;
    uint32_t last_uploaded_seq;
} CriManaSlot;

static CriManaSlot g_mana[MAX_MANA_PLAYERS];

/* Match CriMana.MovieInfo sequential layout (first 48 bytes before AudioInfo[]) */
typedef struct {
    uint32_t _reserved1;
    uint32_t _hasAlpha;
    uint32_t width;
    uint32_t height;
    uint32_t dispWidth;
    uint32_t dispHeight;
    uint32_t framerateN;
    uint32_t framerateD;
    uint32_t totalFrames;
    uint32_t _codecType;      /* 1 = SofdecPrime */
    uint32_t _alphaCodecType; /* 1 = SofdecPrime alpha */
    uint32_t numAudioStreams;
} CriMovieInfoNative;

/* Match CriMana.FrameInfo sequential layout */
typedef struct {
    int32_t  frameNo;
    int32_t  frameNoPerFile;
    uint32_t width;
    uint32_t height;
    uint32_t dispWidth;
    uint32_t dispHeight;
    uint32_t framerateN;
    uint32_t framerateD;
    uint64_t time;
    uint64_t tunit;
    uint32_t cntConcatenatedMovie;
    uint32_t alphaType;
    uint32_t cntSkippedFrames;
    uint32_t totalFramesPerFile;
} CriFrameInfoNative;

static int64_t mana_elapsed_ms(CriManaSlot *m) {
    if (!m->used || m->status != 5) return 0;
    if (m->paused) return m->paused_elapsed_ms;
    int64_t dt = cri_now_ms() - m->start_ms;
    return dt < 0 ? 0 : dt;
}

static void record_mana_textures(int32_t id, int32_t num_tex, void **tex_ptrs) {
    if (id < 0 || id >= MAX_MANA_PLAYERS || !tex_ptrs || num_tex <= 0) return;
    CriManaSlot *m = &g_mana[id];
    if (!m->used) return;
    int n = num_tex > 4 ? 4 : num_tex;
    if (m->num_tex != n) {
        m->num_tex = n;
        m->tex_dirty = 1;
    }
    for (int i = 0; i < n; i++) {
        if (m->tex_ptrs[i] != tex_ptrs[i]) {
            m->tex_ptrs[i] = tex_ptrs[i];
            m->tex_dirty = 1;
        }
    }
}

/* OpenGL render thread callback invoked by GL.IssuePluginEvent */
static void cri_render_event_callback(int32_t event_id) {
    typedef unsigned int GLenum;
    typedef int GLint;
    typedef int GLsizei;
    typedef unsigned int GLuint;
    static int gl_resolved = 0;
    static void (*p_glBindTexture)(GLenum, GLuint) = NULL;
    static void (*p_glGetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *) = NULL;
    static void (*p_glGetTexParameteriv)(GLenum, GLenum, GLint *) = NULL;
    static void (*p_glTexParameteriv)(GLenum, GLenum, const GLint *) = NULL;
    static void (*p_glGetIntegerv)(GLenum, GLint *) = NULL;
    static void (*p_glPixelStorei)(GLenum, GLint) = NULL;
    static void (*p_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *) = NULL;
    static int upload_logs = 0;

    uint32_t off = (uint32_t)g_mana_event_offset;
    uint32_t raw = (uint32_t)event_id - off;
    if (raw > 0x1000U) raw = (uint32_t)event_id ^ off;
    int op = (int)(raw & 0xFF00U);
    int id = (int)(raw & 0x00FFU);
    if (op != 512 || id < 0 || id >= MAX_MANA_PLAYERS) return;

    void *local_ptrs[4] = {0};
    int local_ntex = 0;
    pthread_mutex_lock(&g_cri_lock);
    if (g_mana[id].used && g_mana[id].tex_dirty && g_mana[id].num_tex > 0) {
        local_ntex = g_mana[id].num_tex;
        for (int i = 0; i < local_ntex; i++) local_ptrs[i] = g_mana[id].tex_ptrs[i];
        g_mana[id].tex_dirty = 0;
    }
    pthread_mutex_unlock(&g_cri_lock);
    if (local_ntex <= 0) return;

    if (!gl_resolved) {
        gl_resolved = 1;
        void *handle = RTLD_DEFAULT;
        p_glBindTexture = (void (*)(GLenum, GLuint))dlsym(handle, "glBindTexture");
        p_glGetTexLevelParameteriv = (void (*)(GLenum, GLint, GLenum, GLint *))dlsym(handle, "glGetTexLevelParameteriv");
        p_glGetTexParameteriv = (void (*)(GLenum, GLenum, GLint *))dlsym(handle, "glGetTexParameteriv");
        p_glTexParameteriv = (void (*)(GLenum, GLenum, const GLint *))dlsym(handle, "glTexParameteriv");
        p_glGetIntegerv = (void (*)(GLenum, GLint *))dlsym(handle, "glGetIntegerv");
        p_glPixelStorei = (void (*)(GLenum, GLint))dlsym(handle, "glPixelStorei");
        p_glTexSubImage2D = (void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))dlsym(handle, "glTexSubImage2D");
        if (!p_glTexSubImage2D) {
            void *gl_so = dlopen("libGL.so.1", RTLD_LAZY | RTLD_NOLOAD);
            if (!gl_so) gl_so = dlopen("libGL.so.1", RTLD_LAZY);
            if (gl_so) {
                if (!p_glBindTexture) p_glBindTexture = (void (*)(GLenum, GLuint))dlsym(gl_so, "glBindTexture");
                if (!p_glGetTexLevelParameteriv) p_glGetTexLevelParameteriv = (void (*)(GLenum, GLint, GLenum, GLint *))dlsym(gl_so, "glGetTexLevelParameteriv");
                if (!p_glGetTexParameteriv) p_glGetTexParameteriv = (void (*)(GLenum, GLenum, GLint *))dlsym(gl_so, "glGetTexParameteriv");
                if (!p_glTexParameteriv) p_glTexParameteriv = (void (*)(GLenum, GLenum, const GLint *))dlsym(gl_so, "glTexParameteriv");
                if (!p_glGetIntegerv) p_glGetIntegerv = (void (*)(GLenum, GLint *))dlsym(gl_so, "glGetIntegerv");
                if (!p_glPixelStorei) p_glPixelStorei = (void (*)(GLenum, GLint))dlsym(gl_so, "glPixelStorei");
                p_glTexSubImage2D = (void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))dlsym(gl_so, "glTexSubImage2D");
            }
        }
    }
    if (!p_glBindTexture || !p_glGetTexLevelParameteriv || !p_glTexSubImage2D || !p_glPixelStorei) return;

    static int gl_diag_done = 0;
    if (!gl_diag_done && p_glGetIntegerv) {
        gl_diag_done = 1;
        typedef void (*fn_getfb)(GLenum, GLenum, GLenum, GLint *);
        typedef unsigned char (*fn_isen)(GLenum);
        fn_getfb p_getfb = (fn_getfb)dlsym(RTLD_DEFAULT, "glGetFramebufferAttachmentParameteriv");
        fn_isen p_isen = (fn_isen)dlsym(RTLD_DEFAULT, "glIsEnabled");
        GLint fb = -1, fbsbits = -1, dbits = -1, swm = -1;
        p_glGetIntegerv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &fb);
        p_glGetIntegerv(0x0B98 /* GL_STENCIL_WRITEMASK */, &swm);
        if (p_getfb) {
            p_getfb(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, fb == 0 ? 0x1802 : 0x8D20, 0x8217 /* STENCIL_SIZE */, &fbsbits);
            p_getfb(0x8CA9, fb == 0 ? 0x1801 : 0x8D00, 0x8216 /* DEPTH_SIZE */, &dbits);
        }
        cri_log("[CriGLDiag] draw_fb=%d fb_stencil_size=%d fb_depth_size=%d swm=0x%x sten_en=%d\n",
                fb, fbsbits, dbits, swm, p_isen ? (int)p_isen(0x0B90) : -1);
    }

    /* Snapshot latest decoded VP9 YUV planes under lock before GL upload */
    static uint8_t *s_y = NULL, *s_u = NULL, *s_v = NULL;
    static uint32_t s_y_cap = 0, s_uv_cap = 0;
    uint32_t vw = 0, vh = 0;
    int has_yuv = 0;

    pthread_mutex_lock(&g_cri_lock);
    if (g_mana[id].used && g_mana[id].video) {
        CriVideoStream *vs = g_mana[id].video;
        uint32_t w = cri_video_get_width(vs);
        uint32_t h = cri_video_get_height(vs);
        if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
            uint32_t yb = w * h, uvb = (w / 2) * (h / 2);
            if (yb > s_y_cap) {
                uint8_t *ny = (uint8_t *)realloc(s_y, yb);
                if (ny) { s_y = ny; s_y_cap = yb; }
            }
            if (uvb > s_uv_cap) {
                uint8_t *nu = (uint8_t *)realloc(s_u, uvb);
                uint8_t *nv = (uint8_t *)realloc(s_v, uvb);
                if (nu && nv) { s_u = nu; s_v = nv; s_uv_cap = uvb; }
                else { free(nu); free(nv); }
            }
            if (s_y && s_u && s_v && s_y_cap >= yb && s_uv_cap >= uvb) {
                uint32_t seq = 0;
                has_yuv = cri_video_read_planes(vs, s_y, s_u, s_v, &vw, &vh, &seq);
            }
        }
    }
    pthread_mutex_unlock(&g_cri_lock);

    GLint prev_tex = 0, prev_align = 4, prev_rowlen = 0;
    if (p_glGetIntegerv) {
        p_glGetIntegerv(0x8069 /* GL_TEXTURE_BINDING_2D */, &prev_tex);
        p_glGetIntegerv(0x0CF5 /* GL_UNPACK_ALIGNMENT */, &prev_align);
        p_glGetIntegerv(0x0CF2 /* GL_UNPACK_ROW_LENGTH */, &prev_rowlen);
    }
    p_glPixelStorei(0x0CF5, 1);

    /* Plane fallback values: Y=16, U=128, V=128, A=255 */
    static const uint8_t plane_fill[4] = { 16, 128, 128, 255 };
    for (int k = 0; k < local_ntex && k < 4; k++) {
        GLuint tex = (GLuint)(uintptr_t)local_ptrs[k];
        if (!tex) continue;
        p_glBindTexture(0x0DE1 /* GL_TEXTURE_2D */, tex);
        GLint tw = 0, th = 0, ifmt = 0;
        p_glGetTexLevelParameteriv(0x0DE1, 0, 0x1000 /* GL_TEXTURE_WIDTH */, &tw);
        p_glGetTexLevelParameteriv(0x0DE1, 0, 0x1001 /* GL_TEXTURE_HEIGHT */, &th);
        p_glGetTexLevelParameteriv(0x0DE1, 0, 0x1003 /* GL_TEXTURE_INTERNAL_FORMAT */, &ifmt);
        if (tw <= 0 || th <= 0 || tw > 8192 || th > 8192) continue;

        GLenum fmt = 0x1903 /* GL_RED */;
        size_t bpp = 1;
        if (ifmt == 0x803C || ifmt == 0x1906) { /* GL_ALPHA8 / GL_ALPHA */
            fmt = 0x1906;
            bpp = 1;
        } else if (ifmt == 0x8058 || ifmt == 0x1908) { /* GL_RGBA8 / GL_RGBA */
            fmt = 0x1908;
            bpp = 4;
        }

        if (ifmt == 0x8229 /* GL_R8 */ && p_glGetTexParameteriv) {
            GLint swz[4] = {0};
            p_glGetTexParameteriv(0x0DE1, 0x8E46 /* GL_TEXTURE_SWIZZLE_RGBA */, swz);
            if (swz[3] != 0x1903 /* GL_RED */ && p_glTexParameteriv) {
                static const GLint alpha_swz[4] = { 0, 0, 0, 0x1903 };
                p_glTexParameteriv(0x0DE1, 0x8E46, alpha_swz);
                if (upload_logs < 16) {
                    cri_log("[CriManaGL] id=%d plane=%d fixed swizzle [0x%x,0x%x,0x%x,0x%x] -> (0,0,0,R)\n",
                            id, k, swz[0], swz[1], swz[2], swz[3]);
                }
            } else if (upload_logs < 4) {
                cri_log("[CriManaGL] id=%d plane=%d swizzle=[0x%x,0x%x,0x%x,0x%x]\n",
                        id, k, swz[0], swz[1], swz[2], swz[3]);
            }
        }

        if (has_yuv && k < 3 && fmt == 0x1903 && vw > 0 && vh > 0) {
            const uint8_t *src = (k == 0) ? s_y : (k == 1 ? s_u : s_v);
            GLint pw = (GLint)(k == 0 ? vw : (vw / 2));
            GLint ph = (GLint)(k == 0 ? vh : (vh / 2));
            GLint uw = pw < tw ? pw : tw;
            GLint uh = ph < th ? ph : th;
            p_glPixelStorei(0x0CF2 /* GL_UNPACK_ROW_LENGTH */, pw);
            p_glTexSubImage2D(0x0DE1, 0, 0, 0, uw, uh, fmt, 0x1401 /* GL_UNSIGNED_BYTE */, src);
            if (upload_logs < 16) {
                upload_logs++;
                cri_log("[CriManaGL] id=%d plane=%d tex=%u tex=%dx%d yuv=%dx%d ifmt=0x%x\n",
                        id, k, tex, tw, th, uw, uh, ifmt);
            }
        } else {
            p_glPixelStorei(0x0CF2, 0);
            size_t bytes = (size_t)tw * (size_t)th * bpp;
            uint8_t *buf = (uint8_t *)malloc(bytes);
            if (!buf) continue;
            memset(buf, plane_fill[k], bytes);
            p_glTexSubImage2D(0x0DE1, 0, 0, 0, (GLsizei)tw, (GLsizei)th, fmt, 0x1401 /* GL_UNSIGNED_BYTE */, buf);
            free(buf);
            if (upload_logs < 16) {
                upload_logs++;
                cri_log("[CriManaGL] id=%d plane=%d tex=%u %dx%d ifmt=0x%x fmt=0x%x fill=%u\n",
                        id, k, tex, tw, th, ifmt, fmt, plane_fill[k]);
            }
        }
    }

    p_glPixelStorei(0x0CF2, prev_rowlen);
    p_glPixelStorei(0x0CF5, prev_align);
    p_glBindTexture(0x0DE1, (GLuint)prev_tex);
}

/* ---- CriAtomExAcbLoader state ---- */
typedef struct {
    int used;
    void *acb;
} CriAcbLoaderSlot;
#define MAX_ACB_LOADERS 32
static CriAcbLoaderSlot g_acb_loaders[MAX_ACB_LOADERS];
static int32_t g_next_asr_rack = 0;

/* ==== CriWare / Decrypter / ErrorHandler / Plugin Init ==== */
int64_t CRIWARE85A17F35(void) {
    /* CriWare.GetRequiredBinaryVersionNumber() == 36844032 (0x02323200) */
    return 36844032LL;
}

int64_t CRIWARE3A537A62(int32_t enable_atom, int32_t enable_mana, uint64_t (*cb)(void *), void *obj) {
    uint64_t enc = 0;
    if (cb) enc = cb(obj);
    uint64_t key = enc ^ 59812011783944165ULL;
    pthread_mutex_lock(&g_cri_lock);
    g_cri_decrypter_atom = enable_atom;
    g_cri_decrypter_mana = enable_mana;
    g_cri_decrypter_key = key;
    pthread_mutex_unlock(&g_cri_lock);
    cri_audio_set_keycode(key);
    cri_log("[CriWareDecrypter] enable_atom=%d enable_mana=%d enc=%llu key=%llu (0x%llx)\n",
            enable_atom, enable_mana, (unsigned long long)enc, (unsigned long long)key, (unsigned long long)key);
    return 0;
}

int64_t CRIWAREAA6E64CF(void) {
    /* No native error string queued */
    return 0;
}

int64_t criWareUnity_SetRenderingEventOffsetForMana(int32_t offset) {
    g_mana_event_offset = offset;
    cri_log("[CriWare] SetRenderingEventOffsetForMana(%d / 0x%x)\n", offset, (unsigned)offset);
    return 0;
}

int64_t CRIWAREA4B00AC3(void) {
    g_atom_initialized = 1;
    cri_audio_init();
    cri_log("[CriAtomPlugin] InitializeLibrary\n");
    return 0;
}

int64_t CRIWARED210699A(void) {
    cri_audio_shutdown();
    g_atom_initialized = 0;
    cri_log("[CriAtomPlugin] FinalizeLibrary\n");
    return 0;
}

int64_t CRIWARE3F266E57(void) { return g_atom_initialized ? 1 : 0; }

int64_t CRIWAREA52DC80F(void) { g_mana_initialized = 1; cri_log("[CriManaPlugin] InitializeLibrary\n"); return 0; }
int64_t CRIWARE3466830B(void) { g_mana_initialized = 0; cri_log("[CriManaPlugin] FinalizeLibrary\n"); return 0; }
int64_t CRIWAREBB847967(void) { return g_mana_initialized ? 1 : 0; }

int64_t CRIWARE5607FE7A(void) { g_fs_initialized = 1; cri_log("[CriFsPlugin] InitializeLibrary\n"); return 0; }
int64_t CRIWARE1692E4D8(void) { g_fs_initialized = 0; cri_log("[CriFsPlugin] FinalizeLibrary\n"); return 0; }
int64_t CRIWAREB2EB8B5F(void) { return g_fs_initialized ? 1 : 0; }

int64_t criWareUnity_GetRenderEventFunc(void) { return (int64_t)&cri_render_event_callback; }
int64_t criWareUnity_GetAllocateFunc(void) { return (int64_t)&cri_alloc_func; }
int64_t criWareUnity_GetDeallocateFunc(void) { return (int64_t)&cri_free_func; }
int64_t criWareUnity_GetPluginErrorCallbackFunc(void) { return (int64_t)&cri_err_cb_func; }

/* ==== CriMana.Player ==== */
static int alloc_mana_slot(void) {
    void *ap = cri_audio_player_create();
    pthread_mutex_lock(&g_cri_lock);
    int id = -1;
    for (int i = 0; i < MAX_MANA_PLAYERS; i++) {
        if (!g_mana[i].used) {
            memset(&g_mana[i], 0, sizeof(g_mana[i]));
            g_mana[i].used = 1;
            g_mana[i].width = 1080;
            g_mana[i].height = 1080;
            g_mana[i].disp_width = 1080;
            g_mana[i].disp_height = 1080;
            g_mana[i].fps_n = 30000;
            g_mana[i].fps_d = 1000;
            g_mana[i].total_frames = 9000;
            g_mana[i].atom_player = ap;
            id = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_cri_lock);
    if (id < 0 && ap) cri_audio_player_destroy(ap);
    return id;
}

int64_t CRIWAREA26BB8B2(void) { return alloc_mana_slot(); }
int64_t CRIWARE24719E0F(void) { return alloc_mana_slot(); }

int64_t CRIWARE2521A295(int32_t id) {
    void *ap = NULL;
    CriVideoStream *vs = NULL;
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS) {
        ap = g_mana[id].atom_player;
        vs = g_mana[id].video;
        g_mana[id].atom_player = NULL;
        g_mana[id].video = NULL;
        g_mana[id].used = 0;
    }
    pthread_mutex_unlock(&g_cri_lock);
    if (vs) cri_video_close(vs);
    if (ap) cri_audio_player_destroy(ap);
    return 0;
}

int64_t CRIWARE2647B0E8(int32_t id) {
    void *ap = NULL;
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used) ap = g_mana[id].atom_player;
    pthread_mutex_unlock(&g_cri_lock);
    return ap ? (int64_t)ap : (int64_t)dummy_mem;
}

int64_t CRIWARE08B3B694(int32_t id, void *cb) {
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used) g_mana[id].cue_cb = cb;
    pthread_mutex_unlock(&g_cri_lock);
    return 0;
}

static int build_dummy_movie_path(const char *path, char *out, size_t out_sz) {
    if (!path || !out || out_sz == 0) return -1;
    const char *md = strstr(path, "/MovieData/");
    if (!md) return -1;
    const char *slash = md;
    while (slash > path && slash[-1] != '/') slash--;
    if (slash > path && slash[-1] == '/') {
        size_t prefix_len = (size_t)(slash - path);
        if (prefix_len + sizeof("A000/MovieData/dummy.dat") <= out_sz) {
            snprintf(out, out_sz, "%.*sA000/MovieData/dummy.dat", (int)prefix_len, path);
            if (access(out, R_OK) == 0) return 0;
        }
    }
    const char *sa = strstr(path, "/StreamingAssets/");
    if (sa) {
        size_t sa_len = (size_t)(sa - path) + strlen("/StreamingAssets/");
        if (sa_len + sizeof("A000/MovieData/dummy.dat") <= out_sz) {
            snprintf(out, out_sz, "%.*sA000/MovieData/dummy.dat", (int)sa_len, path);
            if (access(out, R_OK) == 0) return 0;
        }
    }
    return -1;
}

static int is_empty_black_movie_stub(const char *path, CriVideoStream *vs) {
    if (!path || !vs) return 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strcmp(base, "dummy.dat") == 0) return 0;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > 16384) return 0;
    uint32_t nf = cri_video_get_frame_count(vs);
    if (nf == 0 || nf > 30) return 0;
    if (cri_video_decode_frame(vs, 0) != 0) return 0;
    uint32_t vw = cri_video_get_width(vs);
    uint32_t vh = cri_video_get_height(vs);
    if (vw < 16 || vh < 16 || vw > 4096 || vh > 4096) return 0;
    uint8_t *ybuf = (uint8_t *)malloc((size_t)vw * vh);
    if (!ybuf) return 0;
    uint32_t rw = 0, rh = 0;
    int ok = cri_video_read_planes(vs, ybuf, NULL, NULL, &rw, &rh, NULL);
    uint8_t max_y = 0;
    if (ok && rw > 0 && rh > 0) {
        uint32_t sx = rw >= 16 ? rw / 16 : 1;
        uint32_t sy = rh >= 16 ? rh / 16 : 1;
        for (uint32_t y = sy / 2; y < rh; y += sy) {
            const uint8_t *row = ybuf + (size_t)y * rw;
            for (uint32_t x = sx / 2; x < rw; x += sx) {
                if (row[x] > max_y) max_y = row[x];
            }
        }
    }
    free(ybuf);
    return (ok && max_y <= 24) ? 1 : 0;
}

int64_t CRIWAREDA7020AB(int32_t id, void *binder, const char *path) {
    (void)binder;
    char open_path[1024];
    snprintf(open_path, sizeof(open_path), "%s", path ? path : "");

    uint32_t w = 1080, h = 1080, dw = 1080, dh = 1080, fn = 30000, fd = 1000, tf = 9000;
    int parsed = open_path[0] ? cri_audio_parse_usm_video_info(open_path, &w, &h, &dw, &dh, &fn, &fd, &tf) : -1;
    uint64_t key = g_cri_decrypter_mana ? g_cri_decrypter_key : 0ULL;
    CriVideoStream *new_vs = (open_path[0] && open_path[0] != '(') ? cri_video_open(open_path, key) : NULL;

    char fallback_path[1024];
    if (new_vs && is_empty_black_movie_stub(open_path, new_vs) &&
        build_dummy_movie_path(open_path, fallback_path, sizeof(fallback_path)) == 0) {
        CriVideoStream *fb_vs = cri_video_open(fallback_path, key);
        if (fb_vs) {
            cri_video_close(new_vs);
            new_vs = fb_vs;
            snprintf(open_path, sizeof(open_path), "%s", fallback_path);
            w = 1080; h = 1080; dw = 1080; dh = 1080; fn = 30000; fd = 1000; tf = 9000;
            parsed = cri_audio_parse_usm_video_info(open_path, &w, &h, &dw, &dh, &fn, &fd, &tf);
            cri_log("[CriMana] id=%d empty stub '%s' -> fallback '%s'\n", id, path ? path : "", open_path);
        }
    }

    if (new_vs) {
        uint32_t vw = cri_video_get_width(new_vs);
        uint32_t vh = cri_video_get_height(new_vs);
        uint32_t vf = cri_video_get_frame_count(new_vs);
        if (vw > 0 && vh > 0) {
            w = vw;
            h = vh;
        }
        if (vf > 0 && parsed != 0) tf = vf;
    }
    if (w == 0) w = 1080;
    if (h == 0) h = 1080;
    if (dw == 0) dw = w;
    if (dh == 0) dh = h;
    if (fn == 0 || fd == 0) { fn = 30000; fd = 1000; }
    if (tf == 0) tf = 9000;

    CriVideoStream *old_vs = NULL;
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used) {
        old_vs = g_mana[id].video;
        g_mana[id].video = new_vs;
        g_mana[id].last_uploaded_seq = 0;
        new_vs = NULL;
        g_mana[id].has_file = 1;
        snprintf(g_mana[id].path, sizeof(g_mana[id].path), "%.511s", open_path);
        g_mana[id].width = w;
        g_mana[id].height = h;
        g_mana[id].disp_width = dw;
        g_mana[id].disp_height = dh;
        g_mana[id].fps_n = fn;
        g_mana[id].fps_d = fd;
        g_mana[id].total_frames = tf;
        g_mana[id].tex_dirty = 1;
        cri_log("[CriMana] id=%d SetFile('%s') parsed=%d vs=%p %ux%u disp=%ux%u fps=%u/%u frames=%u\n",
                id, g_mana[id].path, parsed, (void *)g_mana[id].video, w, h, dw, dh, fn, fd, tf);
    }
    pthread_mutex_unlock(&g_cri_lock);
    if (old_vs) cri_video_close(old_vs);
    if (new_vs) cri_video_close(new_vs);
    return 0;
}

int64_t CRIWAREB83D23AD(int32_t id, void *binder, const char *path, int32_t repeat) {
    (void)repeat;
    CRIWAREDA7020AB(id, binder, path);
    return 1;
}

int64_t CRIWARE63277F6F(int32_t id, void *binder, int32_t content_id) {
    (void)binder; (void)content_id;
    CRIWAREDA7020AB(id, NULL, "(content_id)");
    return 0;
}

int64_t CRIWAREE0B9EFC2(int32_t id, void *binder, int32_t content_id, int32_t repeat) {
    (void)repeat;
    CRIWARE63277F6F(id, binder, content_id);
    return 1;
}

int64_t CRIWARECB0E14E2(int32_t id, const char *path, uint64_t off, int64_t range) {
    (void)off; (void)range;
    CRIWAREDA7020AB(id, NULL, path);
    return 0;
}

int64_t CRIWARE5BAB5E33(int32_t id, const char *path, uint64_t off, int64_t range, int32_t repeat) {
    (void)repeat;
    CRIWARECB0E14E2(id, path, off, range);
    return 1;
}

int64_t CRIWAREE313321C(int32_t id, void *data, int64_t size) {
    (void)data; (void)size;
    CRIWAREDA7020AB(id, NULL, "(data)");
    return 0;
}

int64_t CRIWARE21520488(int32_t id, void *data, int64_t size, int32_t repeat) {
    (void)repeat;
    CRIWAREE313321C(id, data, size);
    return 1;
}

int64_t CRIWARE8445DC8A(int32_t id) {
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used) {
        if (g_mana[id].status == 0 || g_mana[id].status == 6) {
            g_mana[id].status = 1; /* Prep */
        }
    }
    pthread_mutex_unlock(&g_cri_lock);
    return 0;
}

int64_t CRIWARE816059CA(int32_t id) {
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used) {
        g_mana[id].status = 5; /* Playing */
        g_mana[id].start_ms = cri_now_ms();
        g_mana[id].paused_elapsed_ms = 0;
        g_mana[id].frame_no = 0;
        cri_log("[CriMana] id=%d Start paused=%d\n", id, g_mana[id].paused);
    }
    pthread_mutex_unlock(&g_cri_lock);
    return 0;
}

int64_t CRIWARE9E8B8158(int32_t id) {
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used) {
        g_mana[id].status = 0; /* Stop */
        g_mana[id].paused = 0;
        g_mana[id].paused_elapsed_ms = 0;
    }
    pthread_mutex_unlock(&g_cri_lock);
    return 0;
}

int64_t CRIWARE4909F06A(int32_t id, int32_t sw) {
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used) {
        int new_p = sw ? 1 : 0;
        if (new_p && !g_mana[id].paused) {
            g_mana[id].paused_elapsed_ms = mana_elapsed_ms(&g_mana[id]);
        } else if (!new_p && g_mana[id].paused) {
            g_mana[id].start_ms = cri_now_ms() - g_mana[id].paused_elapsed_ms;
            g_mana[id].tex_dirty = 1;
        }
        g_mana[id].paused = new_p;
        cri_log("[CriMana] id=%d Pause(%d)\n", id, new_p);
    }
    pthread_mutex_unlock(&g_cri_lock);
    return 0;
}

int64_t CRIWARE8B4A3952(int32_t id) {
    int res = 0;
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used) res = g_mana[id].paused;
    pthread_mutex_unlock(&g_cri_lock);
    return res;
}

int64_t CRIWAREB0E53BA9(int32_t id, int32_t sw) {
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used) g_mana[id].loop = sw ? 1 : 0;
    pthread_mutex_unlock(&g_cri_lock);
    return 0;
}

int64_t CRIWARE5AEC2F15(int32_t id, int32_t seek_frame) {
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used) {
        CriManaSlot *m = &g_mana[id];
        m->seek_frame = (seek_frame > 0) ? (uint32_t)seek_frame : 0;
        uint32_t fn = m->fps_n ? m->fps_n : 30000;
        uint32_t fd = m->fps_d ? m->fps_d : 1000;
        m->paused_elapsed_ms = (int64_t)m->seek_frame * 1000LL * (int64_t)fd / (int64_t)fn;
        m->start_ms = cri_now_ms() - m->paused_elapsed_ms;
    }
    pthread_mutex_unlock(&g_cri_lock);
    return 0;
}

int64_t CRIWAREEF57F36F(int32_t id, void *subtitle_buf, uint32_t *subtitle_size) {
    (void)subtitle_buf;
    if (subtitle_size) *subtitle_size = 0;
    int res = 0;
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used) {
        if (g_mana[id].status == 1) {
            g_mana[id].status = 2; /* Prep -> Ready */
        }
        res = g_mana[id].status;
    }
    pthread_mutex_unlock(&g_cri_lock);
    return res;
}

int64_t CRIWARE5E90CA71(int32_t id, void *movie_info) {
    if (movie_info) {
        CriMovieInfoNative *mi = (CriMovieInfoNative *)movie_info;
        pthread_mutex_lock(&g_cri_lock);
        uint32_t w = 1080, h = 1080, dw = 1080, dh = 1080, fn = 30000, fd = 1000, tf = 9000;
        if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used) {
            w = g_mana[id].width;
            h = g_mana[id].height;
            dw = g_mana[id].disp_width;
            dh = g_mana[id].disp_height;
            fn = g_mana[id].fps_n;
            fd = g_mana[id].fps_d;
            tf = g_mana[id].total_frames;
        }
        pthread_mutex_unlock(&g_cri_lock);
        mi->_reserved1 = 0;
        mi->_hasAlpha = 0;       /* SofdecPrimeYuv (opaque YUV, no alpha track) */
        mi->width = w;
        mi->height = h;
        mi->dispWidth = dw;
        mi->dispHeight = dh;
        mi->framerateN = fn;
        mi->framerateD = fd;
        mi->totalFrames = tf;
        mi->_codecType = 1;      /* SofdecPrime */
        mi->_alphaCodecType = 0;
        mi->numAudioStreams = 0;
    }
    return 0;
}

int64_t CRIWARE597BC8D5(int32_t id, int32_t num_tex, void **tex_ptrs, void *frame_info, uint8_t *frame_drop) {
    if (frame_drop) *frame_drop = 0;
    int ok = 0;
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used && g_mana[id].status == 5) {
        CriManaSlot *m = &g_mana[id];
        record_mana_textures(id, num_tex, tex_ptrs);
        int64_t ms = mana_elapsed_ms(m);
        uint32_t fn = m->fps_n ? m->fps_n : 30000;
        uint32_t fd = m->fps_d ? m->fps_d : 1000;
        uint32_t fno = (uint32_t)(ms * (int64_t)fn / (1000LL * (int64_t)fd));
        if (m->frame_no == 0 || !m->paused) {
            if (m->frame_no == 0 || fno >= m->frame_no) {
                uint32_t target_f = fno;
                if (m->total_frames > 0) {
                    target_f = m->loop ? (fno % m->total_frames)
                                       : (fno < m->total_frames ? fno : m->total_frames - 1);
                }
                if (m->video) {
                    cri_video_decode_frame(m->video, target_f);
                }
                m->frame_no = fno + 1;
                m->tex_dirty = 1;
                if (frame_info) {
                    CriFrameInfoNative *fi = (CriFrameInfoNative *)frame_info;
                    fi->frameNo = (int32_t)fno;
                    fi->frameNoPerFile = (int32_t)(m->total_frames ? (fno % m->total_frames) : fno);
                    fi->width = m->width;
                    fi->height = m->height;
                    fi->dispWidth = m->disp_width;
                    fi->dispHeight = m->disp_height;
                    fi->framerateN = fn;
                    fi->framerateD = fd;
                    fi->time = (uint64_t)ms;
                    fi->tunit = 1000ULL;
                    fi->cntConcatenatedMovie = 0;
                    fi->alphaType = 0;
                    fi->cntSkippedFrames = 0;
                    fi->totalFramesPerFile = m->total_frames;
                }
                ok = 1;
            }
        }
    }
    pthread_mutex_unlock(&g_cri_lock);
    return ok;
}

int64_t CRIWARE09E7841E(int32_t id, int32_t num_tex, void **tex_ptrs) {
    int ok = 0;
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used && g_mana[id].status == 5) {
        record_mana_textures(id, num_tex, tex_ptrs);
        ok = 1;
    }
    pthread_mutex_unlock(&g_cri_lock);
    return ok;
}

int64_t CRIWAREB3D5B710(int32_t id) {
    int64_t ms = 0;
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used) ms = mana_elapsed_ms(&g_mana[id]);
    pthread_mutex_unlock(&g_cri_lock);
    return ms * 1000LL;
}

int64_t CRIWARE9BD23668(int32_t id) {
    uint32_t tf = 9000;
    pthread_mutex_lock(&g_cri_lock);
    if (id >= 0 && id < MAX_MANA_PLAYERS && g_mana[id].used) tf = g_mana[id].total_frames;
    pthread_mutex_unlock(&g_cri_lock);
    return tf;
}

/* ==== CriAtomExAcb / AcbLoader / Player / Playback / AsrRack (Real Audio Engine) ==== */
int64_t CRIWARE04B3A4F8(void *cfg, void *pcfg) {
    (void)cfg; (void)pcfg;
    pthread_mutex_lock(&g_cri_lock);
    int32_t r = g_next_asr_rack++;
    pthread_mutex_unlock(&g_cri_lock);
    return r;
}

int64_t criAtomEx_RegisterAcfFile(void *binder, const char *path, void *work, int32_t work_size) {
    (void)binder; (void)work; (void)work_size;
    int32_t rc = cri_audio_register_acf_file(path);
    cri_log("[CriAtomEx] RegisterAcfFile('%s') -> rc=%d\n", path ? path : "", rc);
    return 1;
}

int64_t criAtomEx_RegisterAcfData(void *data, int32_t size, void *work, int32_t work_size) {
    (void)data; (void)size; (void)work; (void)work_size;
    return 1;
}

int64_t criAtomExAcb_LoadAcbFile(void *b1, const char *acb_path, void *b2, const char *awb_path, void *work, int32_t wsz) {
    (void)b1; (void)b2; (void)work; (void)wsz;
    void *hn = cri_audio_load_acb_file(acb_path, awb_path);
    cri_log("[CriAtomExAcb] LoadAcbFile('%s', '%s') -> %p (cues=%d)\n",
            acb_path ? acb_path : "", awb_path ? awb_path : "", hn, hn ? cri_audio_acb_get_num_cues(hn) : 0);
    return (int64_t)hn;
}

int64_t criAtomExAcb_LoadAcbData(void *data, int32_t sz, void *b2, const char *awb_path, void *work, int32_t wsz) {
    (void)b2; (void)work; (void)wsz;
    void *hn = cri_audio_load_acb_data((const uint8_t *)data, sz, awb_path);
    cri_log("[CriAtomExAcb] LoadAcbData(sz=%d, '%s') -> %p (cues=%d)\n",
            sz, awb_path ? awb_path : "", hn, hn ? cri_audio_acb_get_num_cues(hn) : 0);
    return (int64_t)hn;
}

int64_t criAtomExAcb_Release(void *acb_hn) {
    cri_audio_release_acb(acb_hn);
    return 0;
}

int64_t criAtomExAcb_ExistsId(void *acb_hn, int32_t id) { return cri_audio_acb_exists_id(acb_hn, id); }
int64_t criAtomExAcb_ExistsName(void *acb_hn, const char *nm) { return cri_audio_acb_exists_name(acb_hn, nm); }
int64_t criAtomExAcb_IsReadyToRelease(void *acb_hn) { (void)acb_hn; return 1; }
int64_t criAtomExAcb_GetNumCues(void *acb_hn) { return cri_audio_acb_get_num_cues(acb_hn); }

int64_t criAtomExAcbLoader_Create(void *cfg) {
    (void)cfg;
    pthread_mutex_lock(&g_cri_lock);
    CriAcbLoaderSlot *l = NULL;
    for (int i = 0; i < MAX_ACB_LOADERS; i++) {
        if (!g_acb_loaders[i].used) {
            memset(&g_acb_loaders[i], 0, sizeof(g_acb_loaders[i]));
            g_acb_loaders[i].used = 1;
            l = &g_acb_loaders[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_cri_lock);
    return (int64_t)l;
}

int64_t criAtomExAcbLoader_Destroy(void *loader) {
    void *unmoved = NULL;
    pthread_mutex_lock(&g_cri_lock);
    for (int i = 0; i < MAX_ACB_LOADERS; i++) {
        if (&g_acb_loaders[i] == loader) {
            unmoved = g_acb_loaders[i].acb;
            g_acb_loaders[i].acb = NULL;
            g_acb_loaders[i].used = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_cri_lock);
    if (unmoved) cri_audio_release_acb(unmoved);
    return 0;
}

int64_t criAtomExAcbLoader_LoadAcbFileAsync(void *loader, void *b1, const char *acb, void *b2, const char *awb) {
    (void)b1; (void)b2;
    void *hn = cri_audio_load_acb_file(acb, awb);
    cri_log("[CriAtomExAcbLoader] LoadAcbFileAsync('%s', '%s') -> %p (cues=%d)\n",
            acb ? acb : "", awb ? awb : "", hn, hn ? cri_audio_acb_get_num_cues(hn) : 0);
    pthread_mutex_lock(&g_cri_lock);
    for (int i = 0; i < MAX_ACB_LOADERS; i++) {
        if (&g_acb_loaders[i] == loader) { g_acb_loaders[i].acb = hn; break; }
    }
    pthread_mutex_unlock(&g_cri_lock);
    return hn ? 1 : 0;
}

int64_t criAtomExAcbLoader_LoadAcbDataAsync(void *loader, void *d, int32_t sz, void *b2, const char *awb) {
    (void)b2;
    void *hn = cri_audio_load_acb_data((const uint8_t *)d, sz, awb);
    pthread_mutex_lock(&g_cri_lock);
    for (int i = 0; i < MAX_ACB_LOADERS; i++) {
        if (&g_acb_loaders[i] == loader) { g_acb_loaders[i].acb = hn; break; }
    }
    pthread_mutex_unlock(&g_cri_lock);
    return hn ? 1 : 0;
}

int64_t criAtomExAcbLoader_GetStatus(void *loader) { (void)loader; return 2; /* Complete */ }
int64_t criAtomExAcbLoader_WaitForCompletion(void *loader) { (void)loader; return 1; }
int64_t criAtomExAcbLoader_MoveAcbHandle(void *loader) {
    void *res = NULL;
    pthread_mutex_lock(&g_cri_lock);
    for (int i = 0; i < MAX_ACB_LOADERS; i++) {
        if (&g_acb_loaders[i] == loader) { res = g_acb_loaders[i].acb; g_acb_loaders[i].acb = NULL; break; }
    }
    pthread_mutex_unlock(&g_cri_lock);
    return (int64_t)res;
}

int64_t criAtomExPlayer_Create(void *cfg, void *work, int32_t work_size) {
    (void)cfg; (void)work; (void)work_size;
    return (int64_t)cri_audio_player_create();
}

int64_t criAtomExPlayer_Destroy(void *player) { cri_audio_player_destroy(player); return 0; }
int64_t criAtomExPlayer_ResetParameters(void *player) { cri_audio_player_reset_params(player); return 0; }
int64_t criAtomExPlayer_SetCueId(void *player, void *acb, int32_t id) { cri_audio_player_set_cue_id(player, acb, id); return 0; }
int64_t criAtomExPlayer_SetCueIndex(void *player, void *acb, int32_t cidx) { cri_audio_player_set_cue_index(player, acb, cidx); return 0; }
int64_t criAtomExPlayer_SetCueName(void *player, void *acb, const char *name) { cri_audio_player_set_cue_name(player, acb, name); return 0; }
int64_t criAtomExPlayer_SetStartTime(void *player, int64_t ms) { cri_audio_player_set_start_time(player, ms); return 0; }
int64_t criAtomExPlayer_SetVolume(void *player, float vol) { cri_audio_player_set_volume(player, vol); return 0; }
int64_t criAtomExPlayer_SetAisacControlById(void *player, uint32_t ctrl_id, float val) { cri_audio_player_set_aisac_by_id(player, (uint16_t)ctrl_id, val); return 0; }
int64_t criAtomExPlayer_SetFadeInTime(void *player, int32_t ms) { cri_audio_player_set_fade_in_time(player, ms); return 0; }
int64_t criAtomExPlayer_SetFadeOutTime(void *player, int32_t ms) { cri_audio_player_set_fade_out_time(player, ms); return 0; }
int64_t criAtomExPlayer_Pause(void *player, int32_t sw) { cri_audio_player_pause(player, sw); return 0; }
int64_t criAtomExPlayer_Resume(void *player, int32_t mode) { (void)mode; cri_audio_player_pause(player, 0); return 0; }
int64_t criAtomExPlayer_IsPaused(void *player) { return cri_audio_player_is_paused(player); }
int64_t criAtomExPlayer_UpdateAll(void *player) { cri_audio_player_update_all(player); return 0; }
int64_t criAtomExPlayer_Update(void *player, uint32_t pb_id) { (void)pb_id; cri_audio_player_update_all(player); return 0; }
int64_t criAtomExPlayer_Stop(void *player) { cri_audio_player_stop(player, 0); return 0; }
int64_t criAtomExPlayer_StopWithoutReleaseTime(void *player) { cri_audio_player_stop(player, 1); return 0; }
int64_t criAtomExPlayer_GetStatus(void *player) { return cri_audio_player_get_status(player); }
int64_t criAtomExPlayer_GetTime(void *player) { return cri_audio_player_get_time(player); }

int64_t criAtomExPlayer_Start(void *player) {
    uint32_t pb_id = cri_audio_player_start(player);
    static int log_cnt = 0;
    if (log_cnt < 80) {
        log_cnt++;
        cri_log("[CriAtomExPlayer] Start player=%p -> pb=%u\n", player, pb_id);
    }
    return (int64_t)pb_id;
}

int64_t criAtomExPlayer_Prepare(void *player) {
    uint32_t pb_id = cri_audio_player_prepare(player);
    cri_log("[CriAtomExPlayer] Prepare player=%p -> pb=%u\n", player, pb_id);
    return (int64_t)pb_id;
}

int64_t criAtomExPlayback_Stop(uint32_t pb_id) { cri_audio_pb_stop(pb_id, 0); return 0; }
int64_t criAtomExPlayback_StopWithoutReleaseTime(uint32_t pb_id) { cri_audio_pb_stop(pb_id, 1); return 0; }
int64_t criAtomExPlayback_Pause(uint32_t pb_id, int32_t sw) { cri_audio_pb_pause(pb_id, sw); return 0; }
int64_t criAtomExPlayback_Resume(uint32_t pb_id, int32_t mode) { (void)mode; cri_audio_pb_pause(pb_id, 0); return 0; }
int64_t criAtomExPlayback_IsPaused(uint32_t pb_id) { return cri_audio_pb_is_paused(pb_id); }
int64_t criAtomExPlayback_GetStatus(uint32_t pb_id) { return cri_audio_pb_get_status(pb_id); }
int64_t criAtomExPlayback_GetTime(uint32_t pb_id) { return cri_audio_pb_get_time(pb_id); }
int64_t criAtomExPlayback_GetTimeSyncedWithAudio(uint32_t pb_id) { return cri_audio_pb_get_time(pb_id); }
int64_t criAtomExPlayback_GetNumPlayedSamples(uint32_t pb_id, int64_t *num_samples, int32_t *sampling_rate) {
    return cri_audio_pb_get_num_played_samples(pb_id, num_samples, sampling_rate);
}


/* ==== Remaining CRIWARE / Unity Stubs ==== */

int64_t CRIWARE01C091D5(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE01C091D5()\n"); count++; }
    return 0;
}

int64_t CRIWARE02EE7DD3(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE02EE7DD3()\n"); count++; }
    return 0;
}

int64_t CRIWARE096C6782(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE096C6782()\n"); count++; }
    return 0;
}

int64_t CRIWARE0CF3A524(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE0CF3A524()\n"); count++; }
    return 0;
}

int64_t CRIWARE0ECC3362(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE0ECC3362()\n"); count++; }
    return 0;
}

int64_t CRIWARE0FA8D35C(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE0FA8D35C()\n"); count++; }
    return 0;
}

int64_t CRIWARE1A8362EB(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE1A8362EB()\n"); count++; }
    return 0;
}

int64_t CRIWARE1AFDC46B(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE1AFDC46B()\n"); count++; }
    return 0;
}

int64_t CRIWARE20C11B9A(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE20C11B9A()\n"); count++; }
    return 0;
}

int64_t CRIWARE21C5A467(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE21C5A467()\n"); count++; }
    return 0;
}

int64_t CRIWARE24B8FE60(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE24B8FE60()\n"); count++; }
    return 0;
}

int64_t CRIWARE25219A74(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE25219A74()\n"); count++; }
    return 0;
}

int64_t CRIWARE3747B7E5(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE3747B7E5()\n"); count++; }
    return 0;
}

int64_t CRIWARE3A32F733(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE3A32F733()\n"); count++; }
    return 0;
}

int64_t CRIWARE3BA75EA4(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE3BA75EA4()\n"); count++; }
    return 0;
}

int64_t CRIWARE412ABF9D(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE412ABF9D()\n"); count++; }
    return 0;
}

int64_t CRIWARE41EF14E3(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE41EF14E3()\n"); count++; }
    return 0;
}

int64_t CRIWARE4334E792(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE4334E792()\n"); count++; }
    return 0;
}

int64_t CRIWARE48587738(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE48587738()\n"); count++; }
    return 0;
}

int64_t CRIWARE488451A3(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE488451A3()\n"); count++; }
    return 0;
}

int64_t CRIWARE49F2A268(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE49F2A268()\n"); count++; }
    return 0;
}

int64_t CRIWARE4E36730F(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE4E36730F()\n"); count++; }
    return 0;
}

int64_t CRIWARE4EAFEB7C(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE4EAFEB7C()\n"); count++; }
    return 0;
}

int64_t CRIWARE54E2D24F(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE54E2D24F()\n"); count++; }
    return 0;
}

int64_t CRIWARE567E547E(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE567E547E()\n"); count++; }
    return 0;
}

int64_t CRIWARE5BB97642(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE5BB97642()\n"); count++; }
    return 0;
}

int64_t CRIWARE5FA753A4(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE5FA753A4()\n"); count++; }
    return 0;
}

int64_t CRIWARE5FCB5D7D(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE5FCB5D7D()\n"); count++; }
    return 0;
}

int64_t CRIWARE64013CA1(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE64013CA1()\n"); count++; }
    return 0;
}

int64_t CRIWARE64C0F341(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE64C0F341()\n"); count++; }
    return 0;
}

int64_t CRIWARE6872EBF9(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE6872EBF9()\n"); count++; }
    return 0;
}

int64_t CRIWARE6ED0E9C8(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE6ED0E9C8()\n"); count++; }
    return 0;
}

int64_t CRIWARE70236B44(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE70236B44()\n"); count++; }
    return 0;
}

int64_t CRIWARE7CD70E82(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE7CD70E82()\n"); count++; }
    return 0;
}

int64_t CRIWARE86280B26(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE86280B26()\n"); count++; }
    return 0;
}

int64_t CRIWARE88FC8CD0(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE88FC8CD0()\n"); count++; }
    return 0;
}

int64_t CRIWARE8E5602ED(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE8E5602ED()\n"); count++; }
    return 0;
}

int64_t CRIWARE90471FC2(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE90471FC2()\n"); count++; }
    return 0;
}

int64_t CRIWARE90CD2D75(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE90CD2D75()\n"); count++; }
    return 0;
}

int64_t CRIWARE9289E5B2(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE9289E5B2()\n"); count++; }
    return 0;
}

int64_t CRIWARE9652D27A(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE9652D27A()\n"); count++; }
    return 0;
}

int64_t CRIWARE967C97CC(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE967C97CC()\n"); count++; }
    return 0;
}

int64_t CRIWARE9A240609(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE9A240609()\n"); count++; }
    return 0;
}

int64_t CRIWARE9D9FC302(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE9D9FC302()\n"); count++; }
    return 0;
}

int64_t CRIWARE9E7A5F99(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARE9E7A5F99()\n"); count++; }
    return 0;
}

int64_t CRIWAREA00567D1(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREA00567D1()\n"); count++; }
    return 0;
}

int64_t CRIWAREA1C88AE8(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREA1C88AE8()\n"); count++; }
    return 0;
}

int64_t CRIWAREA28E36F5(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREA28E36F5()\n"); count++; }
    return 0;
}

int64_t CRIWAREA3ECA824(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREA3ECA824()\n"); count++; }
    return 0;
}

int64_t CRIWAREA488071D(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREA488071D()\n"); count++; }
    return 0;
}

int64_t CRIWAREA5BB5FA6(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREA5BB5FA6()\n"); count++; }
    return 0;
}

int64_t CRIWAREA6F66CFB(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREA6F66CFB()\n"); count++; }
    return 0;
}

int64_t CRIWAREA77B4237(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREA77B4237()\n"); count++; }
    return 0;
}

int64_t CRIWAREA85D9DF7(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREA85D9DF7()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t CRIWAREAE7A75A8(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREAE7A75A8()\n"); count++; }
    return 0;
}

int64_t CRIWAREB37DBD37(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREB37DBD37()\n"); count++; }
    return 0;
}

int64_t CRIWAREB50A1D83(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREB50A1D83()\n"); count++; }
    return 0;
}

int64_t CRIWAREB7275AD5(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREB7275AD5()\n"); count++; }
    return 0;
}

int64_t CRIWAREBBD8C621(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREBBD8C621()\n"); count++; }
    return 0;
}

int64_t CRIWAREBCACA7C0(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREBCACA7C0()\n"); count++; }
    return 0;
}

int64_t CRIWAREBEE6E504(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREBEE6E504()\n"); count++; }
    return 0;
}

int64_t CRIWAREC4593793(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREC4593793()\n"); count++; }
    return 0;
}

int64_t CRIWAREC5C5B426(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREC5C5B426()\n"); count++; }
    return 0;
}

int64_t CRIWAREC7716020(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREC7716020()\n"); count++; }
    return 0;
}

int64_t CRIWAREC92FD665(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREC92FD665()\n"); count++; }
    return 0;
}

int64_t CRIWARECA140BA3(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARECA140BA3()\n"); count++; }
    return 0;
}

int64_t CRIWARECF844BF2(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARECF844BF2()\n"); count++; }
    return 0;
}

int64_t CRIWARECF8D1B27(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARECF8D1B27()\n"); count++; }
    return 0;
}

int64_t CRIWARECF9A6273(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARECF9A6273()\n"); count++; }
    return 0;
}

int64_t CRIWARED125A570(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARED125A570()\n"); count++; }
    return 0;
}

int64_t CRIWARED1F899CC(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARED1F899CC()\n"); count++; }
    return 0;
}

int64_t CRIWARED336541F(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARED336541F()\n"); count++; }
    return 0;
}

int64_t CRIWARED5BA6462(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARED5BA6462()\n"); count++; }
    return 0;
}

int64_t CRIWARED5BABCC9(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARED5BABCC9()\n"); count++; }
    return 0;
}

int64_t CRIWARED5F9F6BA(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARED5F9F6BA()\n"); count++; }
    return 0;
}

int64_t CRIWARED811A639(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARED811A639()\n"); count++; }
    return 0;
}

int64_t CRIWARED81F3597(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARED81F3597()\n"); count++; }
    return 0;
}

int64_t CRIWARED8274724(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARED8274724()\n"); count++; }
    return 0;
}

int64_t CRIWARED9FBC8B6(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWARED9FBC8B6()\n"); count++; }
    return 0;
}

int64_t CRIWAREDCB9402A(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREDCB9402A()\n"); count++; }
    return 0;
}

int64_t CRIWAREDF48E484(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREDF48E484()\n"); count++; }
    return 0;
}

int64_t CRIWAREE3C232C9(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREE3C232C9()\n"); count++; }
    return 0;
}

int64_t CRIWAREE47F5909(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREE47F5909()\n"); count++; }
    return 0;
}

int64_t CRIWAREF1FC946E(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREF1FC946E()\n"); count++; }
    return 0;
}

int64_t CRIWAREF498208E(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREF498208E()\n"); count++; }
    return 0;
}

int64_t CRIWAREF7A5FA03(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] CRIWAREF7A5FA03()\n"); count++; }
    return 0;
}

int64_t UnityPluginLoad(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] UnityPluginLoad()\n"); count++; }
    return 0;
}

int64_t UnityPluginUnload(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] UnityPluginUnload()\n"); count++; }
    return 0;
}

int64_t UnityRenderEvent(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] UnityRenderEvent()\n"); count++; }
    return 0;
}

int64_t UnitySetGraphicsDevice(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] UnitySetGraphicsDevice()\n"); count++; }
    return 0;
}

int64_t criAfx_GetLipsAfxBridgeInterface(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAfx_GetLipsAfxBridgeInterface()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomAsr_SetOutputDataCallback(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomAsr_SetOutputDataCallback()\n"); count++; }
    return 0;
}

int64_t criAtomAuxIn_Create(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomAuxIn_Create()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomAuxIn_Destroy(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomAuxIn_Destroy()\n"); count++; }
    return 0;
}

int64_t criAtomAuxIn_GetFormat(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomAuxIn_GetFormat()\n"); count++; }
    return 0;
}

int64_t criAtomAuxIn_SetBusSendLevelByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomAuxIn_SetBusSendLevelByName()\n"); count++; }
    return 0;
}

int64_t criAtomAuxIn_SetFormat(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomAuxIn_SetFormat()\n"); count++; }
    return 0;
}

int64_t criAtomAuxIn_SetFrequencyRatio(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomAuxIn_SetFrequencyRatio()\n"); count++; }
    return 0;
}

int64_t criAtomAuxIn_SetInputReadStream(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomAuxIn_SetInputReadStream()\n"); count++; }
    return 0;
}

int64_t criAtomAuxIn_SetVolume(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomAuxIn_SetVolume()\n"); count++; }
    return 0;
}

int64_t criAtomAuxIn_Start(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomAuxIn_Start()\n"); count++; }
    return 0;
}

int64_t criAtomAuxIn_Stop(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomAuxIn_Stop()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dListener_Create(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dListener_Create()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomEx3dListener_Destroy(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dListener_Destroy()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dListener_ResetParameters(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dListener_ResetParameters()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dListener_SetDirectionFocusLevel(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dListener_SetDirectionFocusLevel()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dListener_SetDistanceFactor(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dListener_SetDistanceFactor()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dListener_SetDistanceFocusLevel(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dListener_SetDistanceFocusLevel()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dListener_SetDopplerMultiplier(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dListener_SetDopplerMultiplier()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dListener_SetFocusPoint(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dListener_SetFocusPoint()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dListener_SetOrientation(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dListener_SetOrientation()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dListener_SetPosition(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dListener_SetPosition()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dListener_SetVelocity(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dListener_SetVelocity()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dListener_Update(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dListener_Update()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dSource_Create(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_Create()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomEx3dSource_Destroy(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_Destroy()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dSource_GetAttenuationDistanceSetting(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_GetAttenuationDistanceSetting()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dSource_ResetParameters(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_ResetParameters()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dSource_SetAttenuationDistanceSetting(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_SetAttenuationDistanceSetting()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dSource_SetConeOrientation(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_SetConeOrientation()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dSource_SetConeParameter(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_SetConeParameter()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dSource_SetDopplerFactor(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_SetDopplerFactor()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dSource_SetInteriorPanField(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_SetInteriorPanField()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dSource_SetMaxAngleAisacDelta(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_SetMaxAngleAisacDelta()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dSource_SetMinMaxAttenuationDistance(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_SetMinMaxAttenuationDistance()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dSource_SetOrientation(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_SetOrientation()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dSource_SetPosition(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_SetPosition()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dSource_SetVelocity(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_SetVelocity()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dSource_SetVolume(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_SetVolume()\n"); count++; }
    return 0;
}

int64_t criAtomEx3dSource_Update(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx3dSource_Update()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_AttachAwbFile(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_AttachAwbFile()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_DetachAwbFile(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_DetachAwbFile()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_GetAcbInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_GetAcbInfo()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_GetBlockIndexById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_GetBlockIndexById()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_GetBlockIndexByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_GetBlockIndexByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_GetCueInfoById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_GetCueInfoById()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_GetCueInfoByIndex(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_GetCueInfoByIndex()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_GetCueInfoByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_GetCueInfoByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_GetNumCuePlayingCountById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_GetNumCuePlayingCountById()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_GetNumCuePlayingCountByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_GetNumCuePlayingCountByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_GetNumUsableAisacControlsById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_GetNumUsableAisacControlsById()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_GetNumUsableAisacControlsByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_GetNumUsableAisacControlsByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_GetUsableAisacControlById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_GetUsableAisacControlById()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_GetUsableAisacControlByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_GetUsableAisacControlByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_GetWaveformInfoById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_GetWaveformInfoById()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_GetWaveformInfoByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_GetWaveformInfoByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_ResetCueTypeStateById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_ResetCueTypeStateById()\n"); count++; }
    return 0;
}

int64_t criAtomExAcb_ResetCueTypeStateByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcb_ResetCueTypeStateByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_FindBusName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_FindBusName()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetAcfInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetAcfInfo()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetAcfInfoFromAcfData(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetAcfInfoFromAcfData()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetAisacControlIdByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetAisacControlIdByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetAisacControlInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetAisacControlInfo()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetAisacControlNameById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetAisacControlNameById()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomExAcf_GetCategoryInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetCategoryInfo()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetCategoryInfoById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetCategoryInfoById()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetCategoryInfoByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetCategoryInfoByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetDspBusInformation(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetDspBusInformation()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetDspBusLinkInformation(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetDspBusLinkInformation()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetDspFxName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetDspFxName()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetDspFxParameters(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetDspFxParameters()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetDspFxType(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetDspFxType()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetDspSettingInformation(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetDspSettingInformation()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetDspSettingNameByIndex(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetDspSettingNameByIndex()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetDspSettingNameByIndexFromAcfData(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetDspSettingNameByIndexFromAcfData()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetDspSettingSnapshotInformation(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetDspSettingSnapshotInformation()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetGlobalAisacGraphInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetGlobalAisacGraphInfo()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetGlobalAisacInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetGlobalAisacInfo()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetGlobalAisacInfoByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetGlobalAisacInfoByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetGlobalAisacValue(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetGlobalAisacValue()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetMaxBusesOfDspBusSettings(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetMaxBusesOfDspBusSettings()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetMaxBusesOfDspBusSettingsFromAcfData(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetMaxBusesOfDspBusSettingsFromAcfData()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetNumAisacControls(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetNumAisacControls()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetNumBuses(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetNumBuses()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetNumBusesFromAcfData(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetNumBusesFromAcfData()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetNumCategories(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetNumCategories()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetNumCategoriesFromAcfData(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetNumCategoriesFromAcfData()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetNumCategoriesPerPlayback(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetNumCategoriesPerPlayback()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetNumCategoriesPerPlaybackFromAcfData(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetNumCategoriesPerPlaybackFromAcfData()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetNumDspSettings(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetNumDspSettings()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetNumDspSettingsFromAcfData(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetNumDspSettingsFromAcfData()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetNumGlobalAisacs(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetNumGlobalAisacs()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetNumSelectors(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetNumSelectors()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetSelectorInfoByIndex(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetSelectorInfoByIndex()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetSelectorInfoByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetSelectorInfoByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_GetSelectorLabelInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_GetSelectorLabelInfo()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_SetGlobalLabelToSelectorByIndex(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_SetGlobalLabelToSelectorByIndex()\n"); count++; }
    return 0;
}

int64_t criAtomExAcf_SetGlobalLabelToSelectorByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAcf_SetGlobalLabelToSelectorByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAsrRack_ApplyDspBusSnapshot(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsrRack_ApplyDspBusSnapshot()\n"); count++; }
    return 0;
}

int64_t criAtomExAsrRack_AttachDspBusSetting(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsrRack_AttachDspBusSetting()\n"); count++; }
    return 0;
}

int64_t criAtomExAsrRack_Destroy(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsrRack_Destroy()\n"); count++; }
    return 0;
}

int64_t criAtomExAsrRack_DetachDspBusSetting(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsrRack_DetachDspBusSetting()\n"); count++; }
    return 0;
}

int64_t criAtomExAsrRack_GetNumBuses(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsrRack_GetNumBuses()\n"); count++; }
    return 0;
}

int64_t criAtomExAsrRack_SetAlternateRackId(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsrRack_SetAlternateRackId()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_AttachBusAnalyzer(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_AttachBusAnalyzer()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_AttachBusAnalyzerByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_AttachBusAnalyzerByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_DetachBusAnalyzer(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_DetachBusAnalyzer()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_DetachBusAnalyzerByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_DetachBusAnalyzerByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_GetBusAnalyzerInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_GetBusAnalyzerInfo()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_GetBusAnalyzerInfoByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_GetBusAnalyzerInfoByName()\n"); count++; }
    return 0;
}

float criAtomExAsr_GetEffectParameter(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_GetEffectParameter()\n"); count++; }
    return 0.0f;
}

int64_t criAtomExAsr_RegisterEffectInterface(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_RegisterEffectInterface()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_SetBusMatrix(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_SetBusMatrix()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_SetBusMatrixByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_SetBusMatrixByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_SetBusSendLevel(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_SetBusSendLevel()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_SetBusSendLevelByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_SetBusSendLevelByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_SetBusVolume(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_SetBusVolume()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_SetBusVolumeByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_SetBusVolumeByName()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_SetEffectBypass(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_SetEffectBypass()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_SetEffectParameter(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_SetEffectParameter()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_UnregisterEffectInterface(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_UnregisterEffectInterface()\n"); count++; }
    return 0;
}

int64_t criAtomExAsr_UpdateEffectParameters(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExAsr_UpdateEffectParameters()\n"); count++; }
    return 0;
}

int64_t criAtomExCategory_GetAttachedAisacInfoById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_GetAttachedAisacInfoById()\n"); count++; }
    return 0;
}

int64_t criAtomExCategory_GetAttachedAisacInfoByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_GetAttachedAisacInfoByName()\n"); count++; }
    return 0;
}

int64_t criAtomExCategory_GetReactParameter(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_GetReactParameter()\n"); count++; }
    return 0;
}

float criAtomExCategory_GetVolumeById(uint32_t id) {
    return cri_audio_category_get_volume_by_id(id);
}

float criAtomExCategory_GetVolumeByName(const char *name) {
    return cri_audio_category_get_volume_by_name(name);
}

int64_t criAtomExCategory_IsMutedById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_IsMutedById()\n"); count++; }
    return 0;
}

int64_t criAtomExCategory_IsMutedByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_IsMutedByName()\n"); count++; }
    return 0;
}

int64_t criAtomExCategory_IsPausedById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_IsPausedById()\n"); count++; }
    return 0;
}

int64_t criAtomExCategory_IsPausedByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_IsPausedByName()\n"); count++; }
    return 0;
}

int64_t criAtomExCategory_IsSoloedById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_IsSoloedById()\n"); count++; }
    return 0;
}

int64_t criAtomExCategory_IsSoloedByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_IsSoloedByName()\n"); count++; }
    return 0;
}

int64_t criAtomExCategory_MuteById(uint32_t id, int32_t mute) {
    cri_audio_category_mute_by_id(id, mute);
    return 0;
}

int64_t criAtomExCategory_MuteByName(const char *name, int32_t mute) {
    cri_audio_category_mute_by_name(name, mute);
    return 0;
}

int64_t criAtomExCategory_PauseById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_PauseById()\n"); count++; }
    return 0;
}

int64_t criAtomExCategory_PauseByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_PauseByName()\n"); count++; }
    return 0;
}

int64_t criAtomExCategory_SetAisacControlById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_SetAisacControlById()\n"); count++; }
    return 0;
}

int64_t criAtomExCategory_SetAisacControlByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_SetAisacControlByName()\n"); count++; }
    return 0;
}

int64_t criAtomExCategory_SetReactParameter(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_SetReactParameter()\n"); count++; }
    return 0;
}

int64_t criAtomExCategory_SetVolumeById(uint32_t id, float vol) {
    cri_audio_category_set_volume_by_id(id, vol);
    return 0;
}

int64_t criAtomExCategory_SetVolumeByName(const char *name, float vol) {
    cri_audio_category_set_volume_by_name(name, vol);
    return 0;
}

int64_t criAtomExCategory_SoloById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_SoloById()\n"); count++; }
    return 0;
}

int64_t criAtomExCategory_SoloByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExCategory_SoloByName()\n"); count++; }
    return 0;
}

int64_t criAtomExDebug_GetResourcesInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExDebug_GetResourcesInfo()\n"); count++; }
    return 0;
}

int64_t criAtomExOutputAnalyzer_AttachDspBusByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExOutputAnalyzer_AttachDspBusByName()\n"); count++; }
    return 0;
}

int64_t criAtomExOutputAnalyzer_AttachExPlayer(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExOutputAnalyzer_AttachExPlayer()\n"); count++; }
    return 0;
}

int64_t criAtomExOutputAnalyzer_Create(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExOutputAnalyzer_Create()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomExOutputAnalyzer_Destroy(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExOutputAnalyzer_Destroy()\n"); count++; }
    return 0;
}

int64_t criAtomExOutputAnalyzer_DetachDspBusByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExOutputAnalyzer_DetachDspBusByName()\n"); count++; }
    return 0;
}

int64_t criAtomExOutputAnalyzer_DetachExPlayer(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExOutputAnalyzer_DetachExPlayer()\n"); count++; }
    return 0;
}

int64_t criAtomExOutputAnalyzer_ExecuteQueuedPcmCapturerCallbacks(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExOutputAnalyzer_ExecuteQueuedPcmCapturerCallbacks()\n"); count++; }
    return 0;
}

int64_t criAtomExOutputAnalyzer_GetPcmData(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExOutputAnalyzer_GetPcmData()\n"); count++; }
    return (int64_t)dummy_mem;
}

float criAtomExOutputAnalyzer_GetRms(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExOutputAnalyzer_GetRms()\n"); count++; }
    return 0.0f;
}

int64_t criAtomExOutputAnalyzer_GetSpectrumLevels(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExOutputAnalyzer_GetSpectrumLevels()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomExPlayback_GetAisacControlById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayback_GetAisacControlById()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayback_GetAisacControlByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayback_GetAisacControlByName()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayback_GetBeatSyncInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayback_GetBeatSyncInfo()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayback_GetCurrentBlockIndex(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayback_GetCurrentBlockIndex()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayback_GetFormatInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayback_GetFormatInfo()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayback_GetParameterFloat32(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayback_GetParameterFloat32()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayback_GetParameterSint32(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayback_GetParameterSint32()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayback_GetParameterUint32(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayback_GetParameterUint32()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayback_GetPlaybackTrackInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayback_GetPlaybackTrackInfo()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayback_GetSequencePosition(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayback_GetSequencePosition()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayback_SetNextBlockIndex(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayback_SetNextBlockIndex()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayerParameter_RemoveParameter(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayerParameter_RemoveParameter()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_AttachAisac(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_AttachAisac()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_AttachFader(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_AttachFader()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_ClearSelectorLabels(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_ClearSelectorLabels()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_DetachAisac(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_DetachAisac()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_DetachFader(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_DetachFader()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_GetAttachedAisacInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_GetAttachedAisacInfo()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_GetFadeInStartOffset(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_GetFadeInStartOffset()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_GetFadeInTime(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_GetFadeInTime()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_GetFadeOutEndDelay(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_GetFadeOutEndDelay()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_GetFadeOutTime(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_GetFadeOutTime()\n"); count++; }
    return 0;
}

float criAtomExPlayer_GetParameterFloat32(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_GetParameterFloat32()\n"); count++; }
    return 0.0f;
}

int64_t criAtomExPlayer_GetParameterSint32(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_GetParameterSint32()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_GetParameterUint32(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_GetParameterUint32()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_GetPlayerParameter(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_GetPlayerParameter()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomExPlayer_IsFading(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_IsFading()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_LimitLoopCount(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_LimitLoopCount()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_ResetFaderParameters(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_ResetFaderParameters()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_Set3dListenerHn(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_Set3dListenerHn()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_Set3dSourceHn(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_Set3dSourceHn()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetAisacControlByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetAisacControlByName()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetAsrRackId(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetAsrRackId()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetBandpassFilterParameters(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetBandpassFilterParameters()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetBiquadFilterParameters(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetBiquadFilterParameters()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetBusSendLevel(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetBusSendLevel()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetBusSendLevelByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetBusSendLevelByName()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetBusSendLevelOffset(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetBusSendLevelOffset()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetBusSendLevelOffsetByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetBusSendLevelOffsetByName()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetCategoryById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetCategoryById()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetCategoryByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetCategoryByName()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetContentId(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetContentId()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetCuePriority(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetCuePriority()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetData(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetData()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetDspParameter(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetDspParameter()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetEnvelopeAttackTime(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetEnvelopeAttackTime()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetEnvelopeDecayTime(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetEnvelopeDecayTime()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetEnvelopeHoldTime(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetEnvelopeHoldTime()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetEnvelopeReleaseTime(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetEnvelopeReleaseTime()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetEnvelopeSustainLevel(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetEnvelopeSustainLevel()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetFadeInStartOffset(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetFadeInStartOffset()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetFadeOutEndDelay(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetFadeOutEndDelay()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetFile(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetFile()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetFirstBlockIndex(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetFirstBlockIndex()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetFormat(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetFormat()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetGroupNumber(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetGroupNumber()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetNumChannels(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetNumChannels()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetPan3dAngle(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetPan3dAngle()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetPan3dInteriorDistance(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetPan3dInteriorDistance()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetPan3dVolume(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetPan3dVolume()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetPanType(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetPanType()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetPitch(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetPitch()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetPreDelayTime(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetPreDelayTime()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetRandomSeed(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetRandomSeed()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetSamplingRate(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetSamplingRate()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetSelectorLabel(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetSelectorLabel()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetSendLevel(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetSendLevel()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetSequencePrepareTime(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetSequencePrepareTime()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetSoundRendererType(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetSoundRendererType()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetVoiceControlMethod(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetVoiceControlMethod()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetVoicePoolIdentifier(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetVoicePoolIdentifier()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_SetVoicePriority(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_SetVoicePriority()\n"); count++; }
    return 0;
}

int64_t criAtomExPlayer_UnsetCategory(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExPlayer_UnsetCategory()\n"); count++; }
    return 0;
}

int64_t criAtomExSoundObject_AddPlayer(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExSoundObject_AddPlayer()\n"); count++; }
    return 0;
}

int64_t criAtomExSoundObject_Create(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExSoundObject_Create()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomExSoundObject_DeleteAllPlayers(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExSoundObject_DeleteAllPlayers()\n"); count++; }
    return 0;
}

int64_t criAtomExSoundObject_DeletePlayer(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExSoundObject_DeletePlayer()\n"); count++; }
    return 0;
}

int64_t criAtomExSoundObject_Destroy(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExSoundObject_Destroy()\n"); count++; }
    return 0;
}

int64_t criAtomExVoicePool_AllocateAdxVoicePool(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExVoicePool_AllocateAdxVoicePool()\n"); count++; }
    return 0;
}

int64_t criAtomExVoicePool_AllocateHcaMxVoicePool(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExVoicePool_AllocateHcaMxVoicePool()\n"); count++; }
    return 0;
}

int64_t criAtomExVoicePool_AllocateHcaVoicePool(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExVoicePool_AllocateHcaVoicePool()\n"); count++; }
    return 0;
}

int64_t criAtomExVoicePool_AllocateRawPcmVoicePool(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExVoicePool_AllocateRawPcmVoicePool()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomExVoicePool_AllocateStandardVoicePool(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExVoicePool_AllocateStandardVoicePool()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomExVoicePool_AllocateWaveVoicePool(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExVoicePool_AllocateWaveVoicePool()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomExVoicePool_AttachDspPitchShifter(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExVoicePool_AttachDspPitchShifter()\n"); count++; }
    return 0;
}

int64_t criAtomExVoicePool_AttachDspTimeStretch(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExVoicePool_AttachDspTimeStretch()\n"); count++; }
    return 0;
}

int64_t criAtomExVoicePool_DetachDsp(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExVoicePool_DetachDsp()\n"); count++; }
    return 0;
}

int64_t criAtomExVoicePool_Free(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExVoicePool_Free()\n"); count++; }
    return 0;
}

int64_t criAtomExVoicePool_FreeAll(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExVoicePool_FreeAll()\n"); count++; }
    return 0;
}

int64_t criAtomExVoicePool_GetNumUsedVoices(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomExVoicePool_GetNumUsedVoices()\n"); count++; }
    return 0;
}

int64_t criAtomEx_ApplyDspBusSnapshot(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx_ApplyDspBusSnapshot()\n"); count++; }
    return 0;
}

int64_t criAtomEx_AttachDspBusSetting(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx_AttachDspBusSetting()\n"); count++; }
    return 0;
}

int64_t criAtomEx_DetachDspBusSetting(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx_DetachDspBusSetting()\n"); count++; }
    return 0;
}

float criAtomEx_GetGameVariableById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx_GetGameVariableById()\n"); count++; }
    return 0.0f;
}

float criAtomEx_GetGameVariableByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx_GetGameVariableByName()\n"); count++; }
    return 0.0f;
}

int64_t criAtomEx_GetGameVariableInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx_GetGameVariableInfo()\n"); count++; }
    return 0;
}

int64_t criAtomEx_GetLipsAtomBridgeInterface(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx_GetLipsAtomBridgeInterface()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomEx_GetNumGameVariables(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx_GetNumGameVariables()\n"); count++; }
    return 0;
}

int64_t criAtomEx_RegisterAcfFileById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx_RegisterAcfFileById()\n"); count++; }
    return 0;
}

int64_t criAtomEx_SetGameVariableById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx_SetGameVariableById()\n"); count++; }
    return 0;
}

int64_t criAtomEx_SetGameVariableByName(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx_SetGameVariableByName()\n"); count++; }
    return 0;
}

int64_t criAtomEx_SetRandomSeed(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx_SetRandomSeed()\n"); count++; }
    return 0;
}

int64_t criAtomEx_UnregisterAcf(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomEx_UnregisterAcf()\n"); count++; }
    return 0;
}

int64_t criAtomMicUnity_Finalize(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMicUnity_Finalize()\n"); count++; }
    return 0;
}

int64_t criAtomMicUnity_Initialize(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMicUnity_Initialize()\n"); count++; }
    return 0;
}

int64_t criAtomMic_AttachEffect(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_AttachEffect()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomMic_CalculateWorkSizeForEffect(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_CalculateWorkSizeForEffect()\n"); count++; }
    return 0;
}

int64_t criAtomMic_Create(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_Create()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomMic_Destroy(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_Destroy()\n"); count++; }
    return 0;
}

int64_t criAtomMic_DetachEffect(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_DetachEffect()\n"); count++; }
    return 0;
}

int64_t criAtomMic_GetDefaultDevice(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_GetDefaultDevice()\n"); count++; }
    return 0;
}

int64_t criAtomMic_GetDevice(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_GetDevice()\n"); count++; }
    return 0;
}

int64_t criAtomMic_GetEffectInstance(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_GetEffectInstance()\n"); count++; }
    return (int64_t)dummy_mem;
}

float criAtomMic_GetEffectParameter(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_GetEffectParameter()\n"); count++; }
    return 0.0f;
}

int64_t criAtomMic_GetNumBufferredSamples(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_GetNumBufferredSamples()\n"); count++; }
    return 0;
}

int64_t criAtomMic_GetNumChannels(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_GetNumChannels()\n"); count++; }
    return 0;
}

int64_t criAtomMic_GetNumDevices(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_GetNumDevices()\n"); count++; }
    return 0;
}

int64_t criAtomMic_GetOutputReadStream(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_GetOutputReadStream()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criAtomMic_GetSamplingRate(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_GetSamplingRate()\n"); count++; }
    return 0;
}

int64_t criAtomMic_IsFormatSupported(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_IsFormatSupported()\n"); count++; }
    return 0;
}

int64_t criAtomMic_ReadData(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_ReadData()\n"); count++; }
    return 0;
}

int64_t criAtomMic_SetEffectBypass(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_SetEffectBypass()\n"); count++; }
    return 0;
}

int64_t criAtomMic_SetEffectParameter(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_SetEffectParameter()\n"); count++; }
    return 0;
}

int64_t criAtomMic_SetOutputWriteStream(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_SetOutputWriteStream()\n"); count++; }
    return 0;
}

int64_t criAtomMic_Start(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_Start()\n"); count++; }
    return 0;
}

int64_t criAtomMic_Stop(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_Stop()\n"); count++; }
    return 0;
}

int64_t criAtomMic_UpdateEffectParameters(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtomMic_UpdateEffectParameters()\n"); count++; }
    return 0;
}

int64_t criAtom_GetPerformanceInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtom_GetPerformanceInfo()\n"); count++; }
    return 0;
}

int64_t criAtom_ResetPerformanceMonitor(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtom_ResetPerformanceMonitor()\n"); count++; }
    return 0;
}

int64_t criAtom_SetAudioClientBufferDuration_WASAPI(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtom_SetAudioClientBufferDuration_WASAPI()\n"); count++; }
    return 0;
}

int64_t criAtom_SetAudioClientFormat_WASAPI(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtom_SetAudioClientFormat_WASAPI()\n"); count++; }
    return 0;
}

int64_t criAtom_SetAudioClientShareMode_WASAPI(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criAtom_SetAudioClientShareMode_WASAPI()\n"); count++; }
    return 0;
}

int64_t criBase_GetLipsBaseBridgeInterface(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criBase_GetLipsBaseBridgeInterface()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criDsp_GetLipsDspBridgeInterface(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criDsp_GetLipsDspBridgeInterface()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criFsBinder_BindCpk(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsBinder_BindCpk()\n"); count++; }
    return 0;
}

int64_t criFsBinder_BindDirectory(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsBinder_BindDirectory()\n"); count++; }
    return 0;
}

int64_t criFsBinder_BindFile(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsBinder_BindFile()\n"); count++; }
    return 0;
}

int64_t criFsBinder_BindFileSection(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsBinder_BindFileSection()\n"); count++; }
    return 0;
}

int64_t criFsBinder_Create(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsBinder_Create()\n"); count++; }
    return 0;
}

int64_t criFsBinder_Destroy(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsBinder_Destroy()\n"); count++; }
    return 0;
}

int64_t criFsBinder_GetContentsFileInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsBinder_GetContentsFileInfo()\n"); count++; }
    return 0;
}

int64_t criFsBinder_GetContentsFileInfoById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsBinder_GetContentsFileInfoById()\n"); count++; }
    return 0;
}

int64_t criFsBinder_GetContentsFileInfoByIndex(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsBinder_GetContentsFileInfoByIndex()\n"); count++; }
    return 0;
}

int64_t criFsBinder_GetFileSize(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsBinder_GetFileSize()\n"); count++; }
    return 0;
}

int64_t criFsBinder_GetFileSizeById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsBinder_GetFileSizeById()\n"); count++; }
    return 0;
}

int64_t criFsBinder_GetStatus(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsBinder_GetStatus()\n"); count++; }
    return 0;
}

int64_t criFsBinder_SetPriority(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsBinder_SetPriority()\n"); count++; }
    return 0;
}

int64_t criFsBinder_Unbind(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsBinder_Unbind()\n"); count++; }
    return 0;
}

int64_t criFsInstaller_Copy(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsInstaller_Copy()\n"); count++; }
    return 0;
}

int64_t criFsInstaller_Create(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsInstaller_Create()\n"); count++; }
    return 0;
}

int64_t criFsInstaller_Destroy(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsInstaller_Destroy()\n"); count++; }
    return 0;
}

int64_t criFsInstaller_ExecuteMain(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsInstaller_ExecuteMain()\n"); count++; }
    return 0;
}

int64_t criFsInstaller_GetProgress(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsInstaller_GetProgress()\n"); count++; }
    return 0;
}

int64_t criFsInstaller_GetStatus(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsInstaller_GetStatus()\n"); count++; }
    return 0;
}

int64_t criFsInstaller_Stop(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsInstaller_Stop()\n"); count++; }
    return 0;
}

int64_t criFsLoader_Create(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsLoader_Create()\n"); count++; }
    return 0;
}

int64_t criFsLoader_DecompressData(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsLoader_DecompressData()\n"); count++; }
    return 0;
}

int64_t criFsLoader_Destroy(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsLoader_Destroy()\n"); count++; }
    return 0;
}

int64_t criFsLoader_GetRetryCount(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsLoader_GetRetryCount()\n"); count++; }
    return 0;
}

int64_t criFsLoader_GetStatus(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsLoader_GetStatus()\n"); count++; }
    return 0;
}

int64_t criFsLoader_Load(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsLoader_Load()\n"); count++; }
    return 0;
}

int64_t criFsLoader_LoadById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsLoader_LoadById()\n"); count++; }
    return 0;
}

int64_t criFsLoader_LoadWithoutDecompression(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsLoader_LoadWithoutDecompression()\n"); count++; }
    return 0;
}

int64_t criFsLoader_LoadWithoutDecompressionById(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsLoader_LoadWithoutDecompressionById()\n"); count++; }
    return 0;
}

int64_t criFsLoader_SetReadUnitSize(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsLoader_SetReadUnitSize()\n"); count++; }
    return 0;
}

int64_t criFsLoader_Stop(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsLoader_Stop()\n"); count++; }
    return 0;
}

int64_t criFsWebInstaller_Copy(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsWebInstaller_Copy()\n"); count++; }
    return 0;
}

int64_t criFsWebInstaller_Create(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsWebInstaller_Create()\n"); count++; }
    return 0;
}

int64_t criFsWebInstaller_Destroy(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsWebInstaller_Destroy()\n"); count++; }
    return 0;
}

int64_t criFsWebInstaller_ExecuteMain(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsWebInstaller_ExecuteMain()\n"); count++; }
    return 0;
}

int64_t criFsWebInstaller_Finalize(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsWebInstaller_Finalize()\n"); count++; }
    return 0;
}

int64_t criFsWebInstaller_GetCRC32(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsWebInstaller_GetCRC32()\n"); count++; }
    return 0;
}

int64_t criFsWebInstaller_GetStatusInfo(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsWebInstaller_GetStatusInfo()\n"); count++; }
    return 0;
}

int64_t criFsWebInstaller_Initialize(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsWebInstaller_Initialize()\n"); count++; }
    return 0;
}

int64_t criFsWebInstaller_SetRequestHeader(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsWebInstaller_SetRequestHeader()\n"); count++; }
    return 0;
}

int64_t criFsWebInstaller_Stop(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFsWebInstaller_Stop()\n"); count++; }
    return 0;
}

int64_t criFs_GetNumBinds(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFs_GetNumBinds()\n"); count++; }
    return 0;
}

int64_t criFs_GetNumUsedInstallers(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFs_GetNumUsedInstallers()\n"); count++; }
    return 0;
}

int64_t criFs_GetNumUsedLoaders(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criFs_GetNumUsedLoaders()\n"); count++; }
    return 0;
}

int64_t criManaUnityPlayer_AdvanceManualTimer(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criManaUnityPlayer_AdvanceManualTimer()\n"); count++; }
    return 0;
}

int64_t criManaUnityPlayer_SetManualTimerUnit(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criManaUnityPlayer_SetManualTimerUnit()\n"); count++; }
    return 0;
}

int64_t criManaUnityPlayer_SetMasterTimerType(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criManaUnityPlayer_SetMasterTimerType()\n"); count++; }
    return 0;
}

int64_t criManaUnityPlayer_SetUserTime(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criManaUnityPlayer_SetUserTime()\n"); count++; }
    return 0;
}

int64_t criManaUnity_GetAllocatorManager(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criManaUnity_GetAllocatorManager()\n"); count++; }
    return (int64_t)dummy_mem;
}

int64_t criMana_IsStreamerManagerUsed(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criMana_IsStreamerManagerUsed()\n"); count++; }
    return 0;
}

int64_t criMana_UseStreamerManager(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criMana_UseStreamerManager()\n"); count++; }
    return 0;
}

int64_t criMvPly_AttachCodecInterface(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criMvPly_AttachCodecInterface()\n"); count++; }
    return 0;
}

int64_t criWareUnity_SetErrorCallback(void) {
    static int count = 0;
    if (count < 2) { cri_log("[CriStub] criWareUnity_SetErrorCallback()\n"); count++; }
    return 0;
}
