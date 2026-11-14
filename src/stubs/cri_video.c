#define _GNU_SOURCE
#include "cri_video.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned int fmt;
    unsigned int cs;
    unsigned int range;
    unsigned int w, h;
    unsigned int bit_depth;
    unsigned int d_w, d_h;
    unsigned int r_w, r_h;
    unsigned int x_chroma_shift, y_chroma_shift;
    unsigned char *planes[4];
    int stride[4];
} vpx_img_t;

typedef struct {
    unsigned int threads;
    unsigned int w;
    unsigned int h;
} vpx_dec_cfg_t;

typedef struct {
    uint64_t file_off;
    uint32_t plen;
    uint8_t is_key;
    uint8_t has_dkif;
} UsmVideoChunk;

struct CriVideoStream {
    FILE *fp;
    int has_key;
    uint8_t vkey[64];
    uint32_t width;
    uint32_t height;
    uint32_t alpha_type;
    UsmVideoChunk *chunks;
    uint32_t num_chunks;
    uint32_t cap_chunks;

    uint8_t vpx_ctx[512];
    int vpx_inited;
    int32_t cur_frame;

    uint8_t *pkt_buf;
    uint32_t pkt_cap;

    pthread_mutex_t lock;
    uint8_t *y_buf;
    uint8_t *u_buf;
    uint8_t *v_buf;
    uint32_t y_cap;
    uint32_t uv_cap;
    uint32_t decoded_seq;
    uint32_t frame_w;
    uint32_t frame_h;
    int has_decoded;
};

static pthread_mutex_t g_vpx_init_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_vpx_loaded = 0;
static void *(*p_vp9_dx)(void) = NULL;
static int (*p_dec_init_ver)(void *, void *, const void *, long, int) = NULL;
static int (*p_dec_decode)(void *, const uint8_t *, unsigned int, void *, long) = NULL;
static vpx_img_t *(*p_get_frame)(void *, const void **) = NULL;
static int (*p_dec_destroy)(void *) = NULL;

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t rd_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t rd_le32(const uint8_t *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

int cri_video_global_init(void) {
    pthread_mutex_lock(&g_vpx_init_lock);
    if (!g_vpx_loaded) {
        static const char *const vpx_candidates[] = {
            "libvpx.so.9",
            "/usr/lib64/libvpx.so.9",
            "libvpx.so.10",
            "libvpx.so.8",
            "libvpx.so.7",
            "libvpx.so",
            NULL
        };
        void *lib = NULL;
        for (int i = 0; vpx_candidates[i]; i++) {
            lib = dlopen(vpx_candidates[i], RTLD_LAZY);
            if (lib) break;
        }
        if (lib) {
            p_vp9_dx = dlsym(lib, "vpx_codec_vp9_dx");
            p_dec_init_ver = dlsym(lib, "vpx_codec_dec_init_ver");
            p_dec_decode = dlsym(lib, "vpx_codec_decode");
            p_get_frame = dlsym(lib, "vpx_codec_get_frame");
            p_dec_destroy = dlsym(lib, "vpx_codec_destroy");
            if (p_vp9_dx && p_dec_init_ver && p_dec_decode && p_get_frame && p_dec_destroy) {
                g_vpx_loaded = 1;
            }
        }
    }
    pthread_mutex_unlock(&g_vpx_init_lock);
    return g_vpx_loaded ? 0 : -1;
}

static void derive_usm_vkey(uint64_t key, uint8_t vkey[64]) {
    uint8_t k[8];
    for (int i = 0; i < 8; i++) k[i] = (uint8_t)((key >> (i * 8)) & 0xff);
    uint8_t *t = vkey;
    t[0x00] = k[0];
    t[0x01] = k[1];
    t[0x02] = k[2];
    t[0x03] = (uint8_t)(k[3] - 0x34);
    t[0x04] = (uint8_t)(k[4] + 0xf9);
    t[0x05] = (uint8_t)(k[5] ^ 0x13);
    t[0x06] = (uint8_t)(k[6] + 0x61);
    t[0x07] = (uint8_t)(t[0x00] ^ 0xff);
    t[0x08] = (uint8_t)(t[0x02] + t[0x01]);
    t[0x09] = (uint8_t)(t[0x01] - t[0x07]);
    t[0x0a] = (uint8_t)(t[0x02] ^ 0xff);
    t[0x0b] = (uint8_t)(t[0x01] ^ 0xff);
    t[0x0c] = (uint8_t)(t[0x0b] + t[0x09]);
    t[0x0d] = (uint8_t)(t[0x08] - t[0x03]);
    t[0x0e] = (uint8_t)(t[0x0d] ^ 0xff);
    t[0x0f] = (uint8_t)(t[0x0a] - t[0x0b]);
    t[0x10] = (uint8_t)(t[0x08] - t[0x0f]);
    t[0x11] = (uint8_t)(t[0x10] ^ t[0x07]);
    t[0x12] = (uint8_t)(t[0x0f] ^ 0xff);
    t[0x13] = (uint8_t)(t[0x03] ^ 0x10);
    t[0x14] = (uint8_t)(t[0x04] - 0x32);
    t[0x15] = (uint8_t)(t[0x05] + 0xed);
    t[0x16] = (uint8_t)(t[0x06] ^ 0xf3);
    t[0x17] = (uint8_t)(t[0x13] - t[0x0f]);
    t[0x18] = (uint8_t)(t[0x15] + t[0x07]);
    t[0x19] = (uint8_t)(0x21 - t[0x13]);
    t[0x1a] = (uint8_t)(t[0x14] ^ t[0x17]);
    t[0x1b] = (uint8_t)(t[0x16] + t[0x16]);
    t[0x1c] = (uint8_t)(t[0x17] + 0x44);
    t[0x1d] = (uint8_t)(t[0x03] + t[0x04]);
    t[0x1e] = (uint8_t)(t[0x05] - t[0x16]);
    t[0x1f] = (uint8_t)(t[0x1d] ^ t[0x13]);
    for (int i = 0; i < 32; i++) vkey[32 + i] = (uint8_t)(t[i] ^ 0xff);
}

static void decrypt_usm_video_chunk(uint8_t *buf, uint32_t len, const uint8_t vkey[64]) {
    if (len <= 0x40) return;
    uint32_t dsize = len - 0x40;
    if (dsize < 0x200) return;
    uint8_t roll[64];
    memcpy(roll, vkey, 64);
    for (uint32_t i = 0x100; i < dsize; i++) {
        buf[0x40 + i] ^= roll[0x20 + (i & 0x1f)];
        roll[0x20 + (i & 0x1f)] = (uint8_t)(buf[0x40 + i] ^ vkey[0x20 + (i & 0x1f)]);
    }
    for (uint32_t i = 0; i < 0x100; i++) {
        roll[i & 0x1f] ^= buf[0x140 + i];
        buf[0x40 + i] ^= roll[i & 0x1f];
    }
}

static int reset_vpx_decoder(CriVideoStream *vs) {
    if (vs->vpx_inited) {
        p_dec_destroy(vs->vpx_ctx);
        vs->vpx_inited = 0;
    }
    memset(vs->vpx_ctx, 0, sizeof(vs->vpx_ctx));
    vpx_dec_cfg_t cfg = { 2, 0, 0 };
    if (p_dec_init_ver(vs->vpx_ctx, p_vp9_dx(), &cfg, 0, 12) != 0) {
        return -1;
    }
    vs->vpx_inited = 1;
    vs->cur_frame = -1;
    return 0;
}

CriVideoStream *cri_video_open(const char *path, uint64_t cipher_key) {
    if (!path || !path[0] || cri_video_global_init() != 0) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    CriVideoStream *vs = (CriVideoStream *)calloc(1, sizeof(CriVideoStream));
    if (!vs) { fclose(fp); return NULL; }
    pthread_mutex_init(&vs->lock, NULL);
    vs->fp = fp;
    vs->cur_frame = -1;
    if (cipher_key != 0) {
        vs->has_key = 1;
        derive_usm_vkey(cipher_key, vs->vkey);
    }

    if (fseeko(fp, 0, SEEK_END) != 0) { cri_video_close(vs); return NULL; }
    off_t fsz_off = ftello(fp);
    if (fsz_off <= 32) { cri_video_close(vs); return NULL; }
    uint64_t fsz = (uint64_t)fsz_off;
    fseeko(fp, 0, SEEK_SET);

    uint64_t pos = 0;
    uint8_t hdr[32];
    while (pos + 32 <= fsz) {
        if (fseeko(fp, (off_t)pos, SEEK_SET) != 0 || fread(hdr, 1, 32, fp) != 32) break;
        uint32_t sz = rd_be32(hdr + 4);
        if (sz < 24 || pos + 8ULL + sz > fsz) break;
        uint16_t doff = rd_be16(hdr + 8);
        uint16_t pad = rd_be16(hdr + 10);
        uint8_t dtype = hdr[15];
        if (memcmp(hdr, "@SFV", 4) == 0 && dtype == 0 && sz >= doff + pad) {
            uint32_t plen = sz - doff - pad;
            uint64_t poff = pos + 8ULL + doff;
            if (plen >= 13) {
                uint8_t peek[48];
                uint32_t npeek = plen < sizeof(peek) ? plen : (uint32_t)sizeof(peek);
                if (fseeko(fp, (off_t)poff, SEEK_SET) == 0 && fread(peek, 1, npeek, fp) == npeek) {
                    int has_dkif = (npeek >= 44 && memcmp(peek, "DKIF", 4) == 0);
                    uint32_t ivf_off = has_dkif ? 32 : 0;
                    if (has_dkif && vs->width == 0) {
                        vs->width = (uint32_t)(peek[12] | ((uint32_t)peek[13] << 8));
                        vs->height = (uint32_t)(peek[14] | ((uint32_t)peek[15] << 8));
                    }
                    uint8_t b0 = (ivf_off + 12 < npeek) ? peek[ivf_off + 12] : 0;
                    uint8_t is_key = (has_dkif || (((b0 & 0xC0) == 0x80) && ((b0 & 0x04) == 0))) ? 1 : 0;

                    if (vs->num_chunks == vs->cap_chunks) {
                        uint32_t ncap = vs->cap_chunks ? vs->cap_chunks * 2 : 64;
                        UsmVideoChunk *nc = (UsmVideoChunk *)realloc(vs->chunks, ncap * sizeof(UsmVideoChunk));
                        if (!nc) break;
                        vs->chunks = nc;
                        vs->cap_chunks = ncap;
                    }
                    vs->chunks[vs->num_chunks++] = (UsmVideoChunk){ poff, plen, is_key, (uint8_t)has_dkif };
                }
            }
        }
        pos += 8ULL + sz;
    }

    if (vs->num_chunks == 0 || reset_vpx_decoder(vs) != 0) {
        cri_video_close(vs);
        return NULL;
    }
    return vs;
}

void cri_video_close(CriVideoStream *vs) {
    if (!vs) return;
    if (vs->vpx_inited) p_dec_destroy(vs->vpx_ctx);
    if (vs->fp) fclose(vs->fp);
    free(vs->chunks);
    free(vs->pkt_buf);
    free(vs->y_buf);
    free(vs->u_buf);
    free(vs->v_buf);
    pthread_mutex_destroy(&vs->lock);
    free(vs);
}

uint32_t cri_video_get_width(const CriVideoStream *vs) { return vs ? vs->width : 0; }
uint32_t cri_video_get_height(const CriVideoStream *vs) { return vs ? vs->height : 0; }
uint32_t cri_video_get_frame_count(const CriVideoStream *vs) { return vs ? vs->num_chunks : 0; }
uint32_t cri_video_get_alpha_type(const CriVideoStream *vs) { return vs ? vs->alpha_type : 0; }

static void store_decoded_image(CriVideoStream *vs, const vpx_img_t *img) {
    uint32_t w = img->d_w & ~1U;
    uint32_t h = img->d_h & ~1U;
    if (!w || !h || !img->planes[0] || !img->planes[1] || !img->planes[2]) return;
    uint32_t y_bytes = w * h;
    uint32_t cw = w / 2, ch = h / 2;
    uint32_t uv_bytes = cw * ch;

    pthread_mutex_lock(&vs->lock);
    if (y_bytes > vs->y_cap) {
        uint8_t *ny = (uint8_t *)realloc(vs->y_buf, y_bytes);
        if (ny) { vs->y_buf = ny; vs->y_cap = y_bytes; }
    }
    if (uv_bytes > vs->uv_cap) {
        uint8_t *nu = (uint8_t *)realloc(vs->u_buf, uv_bytes);
        uint8_t *nv = (uint8_t *)realloc(vs->v_buf, uv_bytes);
        if (nu && nv) { vs->u_buf = nu; vs->v_buf = nv; vs->uv_cap = uv_bytes; }
        else { free(nu); free(nv); }
    }
    if (vs->y_buf && vs->u_buf && vs->v_buf && vs->y_cap >= y_bytes && vs->uv_cap >= uv_bytes) {
        for (uint32_t r = 0; r < h; r++) {
            memcpy(vs->y_buf + (size_t)r * w, img->planes[0] + (size_t)r * (size_t)img->stride[0], w);
        }
        for (uint32_t r = 0; r < ch; r++) {
            memcpy(vs->u_buf + (size_t)r * cw, img->planes[1] + (size_t)r * (size_t)img->stride[1], cw);
            memcpy(vs->v_buf + (size_t)r * cw, img->planes[2] + (size_t)r * (size_t)img->stride[2], cw);
        }
        vs->frame_w = w;
        vs->frame_h = h;
        vs->has_decoded = 1;
        vs->decoded_seq++;
    }
    pthread_mutex_unlock(&vs->lock);
}

static int decode_single_chunk(CriVideoStream *vs, uint32_t idx) {
    const UsmVideoChunk *c = &vs->chunks[idx];
    if (c->plen > vs->pkt_cap) {
        uint8_t *np = (uint8_t *)realloc(vs->pkt_buf, c->plen);
        if (!np) return -1;
        vs->pkt_buf = np;
        vs->pkt_cap = c->plen;
    }
    if (fseeko(vs->fp, (off_t)c->file_off, SEEK_SET) != 0 ||
        fread(vs->pkt_buf, 1, c->plen, vs->fp) != c->plen) {
        return -1;
    }
    if (vs->has_key) {
        decrypt_usm_video_chunk(vs->pkt_buf, c->plen, vs->vkey);
    }

    uint32_t off = (c->has_dkif && c->plen >= 32 && memcmp(vs->pkt_buf, "DKIF", 4) == 0) ? 32 : 0;
    int decoded_any = 0;
    while (off + 12 <= c->plen) {
        uint32_t fbytes = rd_le32(vs->pkt_buf + off);
        off += 12;
        if (off + fbytes > c->plen) break;
        if (p_dec_decode(vs->vpx_ctx, vs->pkt_buf + off, fbytes, NULL, 0) == 0) {
            decoded_any = 1;
        }
        off += fbytes;
    }
    const void *iter = NULL;
    vpx_img_t *img = NULL;
    vpx_img_t *last_img = NULL;
    while ((img = p_get_frame(vs->vpx_ctx, &iter)) != NULL) {
        last_img = img;
    }
    if (last_img) {
        store_decoded_image(vs, last_img);
    }
    return decoded_any ? 0 : -1;
}

int cri_video_decode_frame(CriVideoStream *vs, uint32_t target_frame) {
    if (!vs || vs->num_chunks == 0) return -1;
    if (target_frame >= vs->num_chunks) target_frame = vs->num_chunks - 1;
    if ((int32_t)target_frame == vs->cur_frame && vs->has_decoded) return 0;

    uint32_t start = (uint32_t)(vs->cur_frame + 1);
    if (vs->cur_frame < 0 || (int32_t)target_frame < vs->cur_frame) {
        if (reset_vpx_decoder(vs) != 0) return -1;
        start = 0;
    }
    /* If jumping forward or restarting, skip directly to nearest keyframe <= target_frame */
    for (int32_t k = (int32_t)target_frame; k >= (int32_t)start; k--) {
        if (vs->chunks[k].is_key) {
            start = (uint32_t)k;
            break;
        }
    }

    for (uint32_t f = start; f <= target_frame; f++) {
        decode_single_chunk(vs, f);
        vs->cur_frame = (int32_t)f;
    }
    return vs->has_decoded ? 0 : -1;
}

int cri_video_read_planes(CriVideoStream *vs, uint8_t *y_out, uint8_t *u_out, uint8_t *v_out,
                          uint32_t *out_w, uint32_t *out_h, uint32_t *out_decoded_seq) {
    if (!vs) return 0;
    int ok = 0;
    pthread_mutex_lock(&vs->lock);
    if (vs->has_decoded && vs->y_buf && vs->u_buf && vs->v_buf) {
        uint32_t w = vs->frame_w, h = vs->frame_h;
        if (y_out) memcpy(y_out, vs->y_buf, (size_t)w * h);
        if (u_out) memcpy(u_out, vs->u_buf, (size_t)(w / 2) * (h / 2));
        if (v_out) memcpy(v_out, vs->v_buf, (size_t)(w / 2) * (h / 2));
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        if (out_decoded_seq) *out_decoded_seq = vs->decoded_seq;
        ok = 1;
    }
    pthread_mutex_unlock(&vs->lock);
    return ok;
}
