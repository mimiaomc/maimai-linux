#define _GNU_SOURCE
#include "cri_audio.h"
#include "clhca.h"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

/* =========================================================================
 * Big-Endian / Little-Endian Helpers & @UTF Table Parser
 * ========================================================================= */
static inline uint16_t rd_u16be(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static inline int16_t  rd_s16be(const uint8_t *p) { return (int16_t)rd_u16be(p); }
static inline uint32_t rd_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t rd_u64be(const uint8_t *p) {
    return ((uint64_t)rd_u32be(p) << 32) | rd_u32be(p + 4);
}
static inline uint16_t rd_u16le(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define UTF_MAX_COLS 96
typedef struct {
    uint8_t stor;
    uint8_t type;
    const char *name;
    uint32_t const_off; /* offset in table buf for stor == 0x30 */
    uint32_t row_off;   /* byte offset inside row for stor == 0x50 */
} utf_col_t;

typedef struct {
    const uint8_t *buf;
    size_t size;
    uint32_t rows_off;
    uint32_t strs_off;
    uint32_t data_off;
    uint16_t num_cols;
    uint16_t row_width;
    uint32_t num_rows;
    utf_col_t cols[UTF_MAX_COLS];
} utf_table_t;

static uint32_t utf_type_size(uint8_t t) {
    switch (t) {
        case 0: case 1: return 1;
        case 2: case 3: return 2;
        case 4: case 5: case 8: case 10: return 4;
        case 6: case 7: case 11: return 8;
        default: return 0;
    }
}

static const char *utf_str(const utf_table_t *t, uint32_t off) {
    uint32_t abs_off = t->strs_off + off;
    if (abs_off >= t->size) return "";
    return (const char *)(t->buf + abs_off);
}

static int utf_parse(const uint8_t *buf, size_t size, utf_table_t *t) {
    memset(t, 0, sizeof(*t));
    if (!buf || size < 32 || memcmp(buf, "@UTF", 4) != 0) return -1;
    uint32_t tbl_sz = rd_u32be(buf + 4);
    if ((size_t)tbl_sz + 8 > size) tbl_sz = (uint32_t)(size - 8);
    t->buf = buf;
    t->size = size;
    t->rows_off = 8 + rd_u16be(buf + 10);
    t->strs_off = 8 + rd_u32be(buf + 12);
    t->data_off = 8 + rd_u32be(buf + 16);
    t->num_cols = rd_u16be(buf + 24);
    t->row_width = rd_u16be(buf + 26);
    t->num_rows = rd_u32be(buf + 28);
    if (t->num_cols > UTF_MAX_COLS) return -1;
    if (t->rows_off > size || t->strs_off > size || t->data_off > size) return -1;

    uint32_t p = 32;
    uint32_t ro = 0;
    for (uint16_t c = 0; c < t->num_cols; c++) {
        if (p + 5 > size) return -1;
        uint8_t f = buf[p++];
        uint32_t name_off = rd_u32be(buf + p); p += 4;
        uint8_t stor = f & 0xF0;
        uint8_t type = f & 0x0F;
        uint32_t sz = utf_type_size(type);
        t->cols[c].stor = stor;
        t->cols[c].type = type;
        t->cols[c].name = utf_str(t, name_off);
        if (stor == 0x30) {
            if (p + sz > size) return -1;
            t->cols[c].const_off = p;
            p += sz;
        } else if (stor == 0x50) {
            t->cols[c].row_off = ro;
            ro += sz;
        }
    }
    return 0;
}

static const utf_col_t *utf_find_col(const utf_table_t *t, const char *name) {
    for (uint16_t c = 0; c < t->num_cols; c++) {
        if (strcmp(t->cols[c].name, name) == 0) return &t->cols[c];
    }
    return NULL;
}

static uint64_t utf_get_u64_col(const utf_table_t *t, uint32_t row, const utf_col_t *col, uint64_t defv) {
    if (!col || row >= t->num_rows || col->stor == 0x10) return defv;
    uint32_t off = (col->stor == 0x30) ? col->const_off : (t->rows_off + row * t->row_width + col->row_off);
    uint32_t sz = utf_type_size(col->type);
    if (off + sz > t->size) return defv;
    switch (col->type) {
        case 0: return t->buf[off];
        case 1: return (int64_t)(int8_t)t->buf[off];
        case 2: return rd_u16be(t->buf + off);
        case 3: return (int64_t)rd_s16be(t->buf + off);
        case 4: return rd_u32be(t->buf + off);
        case 5: return (int64_t)(int32_t)rd_u32be(t->buf + off);
        case 6: case 7: return rd_u64be(t->buf + off);
        default: return defv;
    }
}

static uint64_t utf_get_u64(const utf_table_t *t, uint32_t row, const char *name, uint64_t defv) {
    return utf_get_u64_col(t, row, utf_find_col(t, name), defv);
}

static const char *utf_get_str(const utf_table_t *t, uint32_t row, const char *name) {
    const utf_col_t *col = utf_find_col(t, name);
    if (!col || col->type != 0x0A || row >= t->num_rows || col->stor == 0x10) return "";
    uint32_t off = (col->stor == 0x30) ? col->const_off : (t->rows_off + row * t->row_width + col->row_off);
    if (off + 4 > t->size) return "";
    return utf_str(t, rd_u32be(t->buf + off));
}

static const uint8_t *utf_get_data(const utf_table_t *t, uint32_t row, const char *name, uint32_t *out_len) {
    *out_len = 0;
    const utf_col_t *col = utf_find_col(t, name);
    if (!col || col->type != 0x0B || row >= t->num_rows || col->stor == 0x10) return NULL;
    uint32_t off = (col->stor == 0x30) ? col->const_off : (t->rows_off + row * t->row_width + col->row_off);
    if (off + 8 > t->size) return NULL;
    uint32_t doff = rd_u32be(t->buf + off);
    uint32_t dlen = rd_u32be(t->buf + off + 4);
    if (dlen == 0 || t->data_off + doff + dlen > t->size) return NULL;
    *out_len = dlen;
    return t->buf + t->data_off + doff;
}

/* =========================================================================
 * AFS2 (.awb / embedded AwbFile) Index
 * ========================================================================= */
typedef struct {
    int valid;
    uint8_t off_sz;
    uint8_t id_sz;
    uint32_t count;
    uint16_t align;
    uint32_t *ids;
    uint64_t *offsets; /* count + 1 raw offsets */
    uint64_t file_size;
} afs2_index_t;

static int afs2_parse_header(const uint8_t *hdr, size_t hdr_len, uint64_t total_size, afs2_index_t *out) {
    memset(out, 0, sizeof(*out));
    if (!hdr || hdr_len < 16 || memcmp(hdr, "AFS2", 4) != 0) return -1;
    out->off_sz = hdr[5];
    out->id_sz  = hdr[6];
    out->count  = rd_u32le(hdr + 8);
    out->align  = rd_u16le(hdr + 12);
    if (out->align == 0) out->align = 1;
    if ((out->off_sz != 2 && out->off_sz != 4) || (out->id_sz != 2 && out->id_sz != 4)) return -1;
    size_t need = 16 + (size_t)out->count * out->id_sz + ((size_t)out->count + 1) * out->off_sz;
    if (need > hdr_len) return -1;

    out->ids = (uint32_t *)malloc(sizeof(uint32_t) * (out->count ? out->count : 1));
    out->offsets = (uint64_t *)malloc(sizeof(uint64_t) * (out->count + 1));
    if (!out->ids || !out->offsets) { free(out->ids); free(out->offsets); return -1; }

    const uint8_t *pid = hdr + 16;
    for (uint32_t i = 0; i < out->count; i++) {
        out->ids[i] = (out->id_sz == 4) ? rd_u32le(pid + i * 4) : rd_u16le(pid + i * 2);
    }
    const uint8_t *poff = pid + (size_t)out->count * out->id_sz;
    for (uint32_t i = 0; i <= out->count; i++) {
        out->offsets[i] = (out->off_sz == 4) ? rd_u32le(poff + i * 4) : rd_u16le(poff + i * 2);
    }
    out->file_size = total_size;
    out->valid = 1;
    return 0;
}

static void afs2_free(afs2_index_t *idx) {
    free(idx->ids);
    free(idx->offsets);
    memset(idx, 0, sizeof(*idx));
}

static int afs2_get_span(const afs2_index_t *idx, uint32_t awb_id, uint64_t *out_off, uint64_t *out_len) {
    if (!idx->valid || idx->count == 0) return -1;
    uint32_t slot = UINT32_MAX;
    if (awb_id < idx->count && idx->ids[awb_id] == awb_id) {
        slot = awb_id;
    } else {
        for (uint32_t i = 0; i < idx->count; i++) {
            if (idx->ids[i] == awb_id) { slot = i; break; }
        }
    }
    if (slot == UINT32_MAX) return -1;
    uint64_t o0 = idx->offsets[slot];
    uint64_t rem = o0 % idx->align;
    if (rem) o0 += (idx->align - rem);
    uint64_t o1 = idx->offsets[slot + 1];
    if (o1 > idx->file_size) o1 = idx->file_size;
    if (o1 <= o0) return -1;
    *out_off = o0;
    *out_len = o1 - o0;
    return 0;
}

/* =========================================================================
 * ACB Object & Cue-to-Waveform Resolution
 * ========================================================================= */
#define MAX_ACBS 64

typedef struct {
    int used;
    uint8_t *acb_buf;
    size_t acb_size;
    utf_table_t header_tbl;
    utf_table_t cue_tbl;
    utf_table_t name_tbl;
    utf_table_t seq_tbl;
    utf_table_t trk_tbl;
    utf_table_t trk_ev_tbl;
    utf_table_t syn_tbl;
    utf_table_t wav_tbl;
    utf_table_t seq_cmd_tbl;
    utf_table_t trk_cmd_tbl;
    utf_table_t syn_cmd_tbl;
    int has_cue, has_name, has_seq, has_trk, has_trk_ev, has_syn, has_wav;
    int has_seq_cmd, has_trk_cmd, has_syn_cmd;

    /* Embedded AWB (from AwbFile column) */
    const uint8_t *emb_awb_ptr;
    size_t emb_awb_size;
    afs2_index_t emb_afs2;

    /* External .awb file path & parsed AFS2 TOC */
    char awb_path[1024];
    afs2_index_t ext_afs2;
} cri_acb_t;

static cri_acb_t g_acbs[MAX_ACBS];

static uint8_t *read_entire_file(const char *path, size_t *out_sz) {
    *out_sz = 0;
    if (!path || !path[0]) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_sz = (size_t)sz;
    return buf;
}

static int acb_parse_subtable(const utf_table_t *hdr, const char *col_name, utf_table_t *out) {
    uint32_t len = 0;
    const uint8_t *ptr = utf_get_data(hdr, 0, col_name, &len);
    if (!ptr || len < 32) return 0;
    return utf_parse(ptr, len, out) == 0;
}

static void acb_init_ext_awb(cri_acb_t *a, const char *awb_path) {
    if (!awb_path || !awb_path[0]) return;
    FILE *f = fopen(awb_path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t peek[16];
    if (fsz >= 16 && fread(peek, 1, 16, f) == 16 && memcmp(peek, "AFS2", 4) == 0) {
        uint8_t off_sz = peek[5], id_sz = peek[6];
        uint32_t cnt = rd_u32le(peek + 8);
        size_t need = 16 + (size_t)cnt * id_sz + ((size_t)cnt + 1) * off_sz;
        if (need <= 16 * 1024 * 1024 && need <= (size_t)fsz) {
            uint8_t *hdr = (uint8_t *)malloc(need);
            if (hdr) {
                fseek(f, 0, SEEK_SET);
                if (fread(hdr, 1, need, f) == need) {
                    if (afs2_parse_header(hdr, need, (uint64_t)fsz, &a->ext_afs2) == 0) {
                        snprintf(a->awb_path, sizeof(a->awb_path), "%s", awb_path);
                    }
                }
                free(hdr);
            }
        }
    }
    fclose(f);
}

static void acb_free_slot(cri_acb_t *a) {
    afs2_free(&a->emb_afs2);
    afs2_free(&a->ext_afs2);
    free(a->acb_buf);
    memset(a, 0, sizeof(*a));
}

#define MAX_CATEGORIES 32
typedef struct {
    int used;
    uint32_t id;
    char name[64];
    float acf_vol;
    float runtime_vol;
    int muted;
} cri_category_t;

static cri_category_t g_cats[MAX_CATEGORIES];

static void parse_cmd_vol_and_cats(const uint8_t *cmd, uint32_t cmd_len,
                                   float *io_vol, uint32_t *out_cats, int *io_ncats, int max_cats) {
    if (!cmd || cmd_len < 3) return;
    uint32_t pos = 0;
    while (pos + 3 <= cmd_len) {
        uint16_t op = rd_u16be(cmd + pos);
        uint8_t plen = cmd[pos + 2];
        pos += 3;
        if (pos + plen > cmd_len) break;
        const uint8_t *pl = cmd + pos;
        if (op == 0x0057 && plen >= 2 && io_vol) {
            *io_vol *= (float)rd_u16be(pl) / 100.0f;
        } else if (op == 0x0041 && plen >= 4 && out_cats && io_ncats) {
            for (uint32_t j = 0; j + 4 <= plen && *io_ncats < max_cats; j += 4) {
                out_cats[(*io_ncats)++] = rd_u32be(pl + j);
            }
        }
        pos += plen;
    }
}

static float read_row_cmd_vol_and_cats(const utf_table_t *row_tbl, uint32_t row_idx,
                                       int has_cmd_tbl, const utf_table_t *cmd_tbl,
                                       uint32_t *out_cats, int *io_ncats, int max_cats) {
    float vol = 1.0f;
    if (!row_tbl || row_idx >= row_tbl->num_rows || !has_cmd_tbl || !cmd_tbl) return vol;
    uint32_t cidx = (uint32_t)utf_get_u64(row_tbl, row_idx, "CommandIndex", 0xFFFF);
    if (cidx == 0xFFFF || cidx >= cmd_tbl->num_rows) return vol;
    uint32_t cmd_len = 0;
    const uint8_t *cmd = utf_get_data(cmd_tbl, cidx, "Command", &cmd_len);
    parse_cmd_vol_and_cats(cmd, cmd_len, &vol, out_cats, io_ncats, max_cats);
    return vol;
}

typedef struct {
    int valid;
    int32_t streaming;
    int32_t encode_type;
    uint32_t awb_id;
    int32_t loop_flag;
    int32_t ev_has_start;
    int64_t ev_start_ms;
    int32_t ev_has_end;
    int64_t ev_end_ms;
    int32_t ev_loop;
    float cue_gain;
    int n_cats;
    uint32_t cat_ids[4];
} resolved_wave_t;

static void emit_waveform_row(const cri_acb_t *a, uint32_t rx, resolved_wave_t *out, int *n_out, int max_out) {
    if (!a->has_wav || rx >= a->wav_tbl.num_rows || *n_out >= max_out) return;
    resolved_wave_t *w = &out[(*n_out)++];
    memset(w, 0, sizeof(*w));
    w->valid = 1;
    w->cue_gain = 1.0f;
    w->streaming   = (int32_t)utf_get_u64(&a->wav_tbl, rx, "Streaming", 0);
    w->encode_type = (int32_t)utf_get_u64(&a->wav_tbl, rx, "EncodeType", 0);
    w->loop_flag   = (int32_t)utf_get_u64(&a->wav_tbl, rx, "LoopFlag", 0);
    w->awb_id = w->streaming
        ? (uint32_t)utf_get_u64(&a->wav_tbl, rx, "StreamAwbId", 0xFFFF)
        : (uint32_t)utf_get_u64(&a->wav_tbl, rx, "MemoryAwbId", 0xFFFF);
}

static void resolve_synth(const cri_acb_t *a, uint32_t syn_idx, int depth,
                          resolved_wave_t *out, int *n_out, int max_out) {
    if (!a->has_syn || syn_idx >= a->syn_tbl.num_rows || depth > 4 || *n_out >= max_out) return;
    int before = *n_out;
    uint32_t ri_len = 0;
    const uint8_t *ri = utf_get_data(&a->syn_tbl, syn_idx, "ReferenceItems", &ri_len);
    if (!ri || ri_len < 4) return;
    uint64_t stype = utf_get_u64(&a->syn_tbl, syn_idx, "Type", 0);
    uint32_t nitems = ri_len / 4;
    uint32_t start_i = 0, end_i = nitems;
    if (stype != 0 && nitems > 1) {
        start_i = (uint32_t)rand() % nitems;
        end_i = start_i + 1;
    }
    for (uint32_t i = start_i; i < end_i && *n_out < max_out; i++) {
        uint16_t rt = rd_u16be(ri + i * 4);
        uint16_t rx = rd_u16be(ri + i * 4 + 2);
        /* rt==1 is Waveform, rt==7 is Track/Stream Waveform ref (used by Preview cues) */
        if (rt == 1 || rt == 7) {
            emit_waveform_row(a, rx, out, n_out, max_out);
        } else if (rt == 2) {
            resolve_synth(a, rx, depth + 1, out, n_out, max_out);
        }
    }
    float syn_vol = read_row_cmd_vol_and_cats(&a->syn_tbl, syn_idx, a->has_syn_cmd, &a->syn_cmd_tbl, NULL, NULL, 0);
    for (int k = before; k < *n_out; k++) {
        out[k].cue_gain *= syn_vol;
    }
}

static void apply_track_event_cmd(const cri_acb_t *a, uint32_t ev_idx,
                                  resolved_wave_t *out, int *n_out, int max_out) {
    int before = *n_out;
    if (!a->has_trk_ev || ev_idx >= a->trk_ev_tbl.num_rows) {
        resolve_synth(a, ev_idx, 0, out, n_out, max_out);
        return;
    }
    uint32_t cmd_len = 0;
    const uint8_t *cmd = utf_get_data(&a->trk_ev_tbl, ev_idx, "Command", &cmd_len);
    if (!cmd || cmd_len < 3) {
        resolve_synth(a, ev_idx, 0, out, n_out, max_out);
        return;
    }

    int32_t has_start = 0, has_end = 0, ev_loop = 0, seen_note_on = 0;
    int64_t start_ms = 0, end_ms = 0;
    uint32_t pos = 0;
    while (pos + 3 <= cmd_len) {
        uint16_t op = rd_u16be(cmd + pos);
        uint8_t plen = cmd[pos + 2];
        pos += 3;
        if (pos + plen > cmd_len) break;
        const uint8_t *pl = cmd + pos;
        if (op == 0x03e7 && plen >= 4) {
            has_start = 1;
            start_ms = (int64_t)rd_u32be(pl);
        } else if (op == 0x07d0 && plen >= 4) {
            seen_note_on = 1;
            uint16_t rt = rd_u16be(pl);
            uint16_t rx = rd_u16be(pl + 2);
            if (rt == 1 || rt == 7) emit_waveform_row(a, rx, out, n_out, max_out);
            else if (rt == 2) resolve_synth(a, rx, 0, out, n_out, max_out);
        } else if (op == 0x07d1 && plen >= 4 && seen_note_on) {
            has_end = 1;
            end_ms = (int64_t)rd_u32be(pl);
        } else if (op == 0x0fa0 && seen_note_on) {
            ev_loop = 1;
        }
        pos += plen;
    }
    if (!seen_note_on) {
        resolve_synth(a, ev_idx, 0, out, n_out, max_out);
    }
    if (has_start) {
        for (int i = before; i < *n_out; i++) {
            out[i].ev_has_start = has_start;
            out[i].ev_start_ms = start_ms;
            out[i].ev_has_end = has_end;
            out[i].ev_end_ms = end_ms;
            out[i].ev_loop = ev_loop;
        }
    }
}

static int acb_resolve_cue_index(const cri_acb_t *a, int32_t cue_idx, resolved_wave_t *out, int max_out) {
    if (!a->has_cue || cue_idx < 0 || (uint32_t)cue_idx >= a->cue_tbl.num_rows) return 0;
    int n_out = 0;
    uint64_t rtype = utf_get_u64(&a->cue_tbl, (uint32_t)cue_idx, "ReferenceType", 0);
    uint32_t ridx  = (uint32_t)utf_get_u64(&a->cue_tbl, (uint32_t)cue_idx, "ReferenceIndex", 0);

    if (rtype == 1 || rtype == 7) {
        emit_waveform_row(a, ridx, out, &n_out, max_out);
    } else if (rtype == 2) {
        resolve_synth(a, ridx, 0, out, &n_out, max_out);
    } else if (rtype == 3 && a->has_seq && ridx < a->seq_tbl.num_rows) {
        uint32_t seq_cats[4] = {0};
        int n_seq_cats = 0;
        float seq_vol = read_row_cmd_vol_and_cats(&a->seq_tbl, ridx, a->has_seq_cmd, &a->seq_cmd_tbl,
                                                  seq_cats, &n_seq_cats, 4);
        uint64_t seq_type = utf_get_u64(&a->seq_tbl, ridx, "Type", 0);
        uint16_t ntracks = (uint16_t)utf_get_u64(&a->seq_tbl, ridx, "NumTracks", 0);
        uint32_t ti_len = 0;
        const uint8_t *ti_bytes = utf_get_data(&a->seq_tbl, ridx, "TrackIndex", &ti_len);
        if (!ti_bytes || ntracks == 0 || ti_len < (uint32_t)ntracks * 2) return 0;

        uint16_t t_start = 0, t_end = ntracks;
        if (seq_type != 0 && ntracks > 1) {
            t_start = (uint16_t)((uint32_t)rand() % ntracks);
            t_end = t_start + 1;
        }
        for (uint16_t t = t_start; t < t_end && n_out < max_out; t++) {
            uint16_t trk_idx = rd_u16be(ti_bytes + t * 2);
            if (!a->has_trk || trk_idx >= a->trk_tbl.num_rows) continue;
            int trk_before = n_out;
            float trk_vol = read_row_cmd_vol_and_cats(&a->trk_tbl, trk_idx, a->has_trk_cmd, &a->trk_cmd_tbl,
                                                      NULL, NULL, 0);
            uint32_t ev_idx = (uint32_t)utf_get_u64(&a->trk_tbl, trk_idx, "EventIndex", 0xFFFF);
            apply_track_event_cmd(a, ev_idx, out, &n_out, max_out);
            for (int k = trk_before; k < n_out; k++) {
                out[k].cue_gain *= seq_vol * trk_vol;
                out[k].n_cats = n_seq_cats;
                for (int c = 0; c < n_seq_cats; c++) out[k].cat_ids[c] = seq_cats[c];
            }
        }
    }
    return n_out;
}

static int32_t acb_find_cue_index_by_id(const cri_acb_t *a, int32_t id) {
    if (!a->has_cue) return -1;
    const utf_col_t *col = utf_find_col(&a->cue_tbl, "CueId");
    for (uint32_t r = 0; r < a->cue_tbl.num_rows; r++) {
        if ((int32_t)utf_get_u64_col(&a->cue_tbl, r, col, 0xFFFFFFFFULL) == id) return (int32_t)r;
    }
    return -1;
}

static int32_t acb_find_cue_index_by_name(const cri_acb_t *a, const char *name) {
    if (!a->has_name || !name) return -1;
    for (uint32_t r = 0; r < a->name_tbl.num_rows; r++) {
        if (strcmp(utf_get_str(&a->name_tbl, r, "CueName"), name) == 0) {
            return (int32_t)utf_get_u64(&a->name_tbl, r, "CueIndex", 0xFFFFFFFFULL);
        }
    }
    return -1;
}

/* =========================================================================
 * Active Voice Decoders (HCA & ADX -> Resampled 48kHz Stereo Float)
 * ========================================================================= */
#define MIX_SR         48000
#define MIX_BLOCK      512
#define MAX_VOICES     64
#define MAX_PLAYERS    256

enum {
    VOICE_NONE = 0,
    VOICE_HCA  = 1,
    VOICE_ADX  = 2
};

enum {
    ATOM_STATUS_STOP    = 0,
    ATOM_STATUS_PREP    = 1,
    ATOM_STATUS_PLAYING = 2,
    ATOM_STATUS_PLAYEND = 3,
    ATOM_STATUS_ERROR   = 4
};

typedef struct {
    int active;
    uint32_t pb_id;
    int32_t player_idx;
    int paused;
    int fading_out;

    uint8_t *owned_data;
    const uint8_t *data;
    size_t data_len;

    int codec;
    uint32_t src_sr;
    uint32_t channels;
    int loop_enabled;

    /* Gain & envelope */
    float bus_gain[4];      /* 0=Spk1P, 1=Spk2P, 2=Hp1P, 3=Hp2P */
    float cue_gain;
    int n_cats;
    uint32_t cat_ids[4];
    float env_gain;
    float fade_in_step;
    float fade_out_step;

    /* Timing in source & output sample domains */
    int64_t start_time_ms;
    uint64_t rendered_out_frames;
    double src_frac_pos;
    double src_step;

    /* HCA state */
    clHCA *hca;
    clHCA_stInfo hca_info;
    uint32_t hca_cur_block;
    uint32_t hca_discard;
    uint64_t hca_abs_sample;
    int hca_ev_loop;
    uint64_t hca_ev_start_sample;
    uint64_t hca_ev_end_sample;
    float *hca_blk_pcm;     /* samplesPerBlock * ch */
    uint32_t hca_blk_pos;
    uint32_t hca_blk_len;

    /* ADX state */
    uint32_t adx_data_off;
    uint32_t adx_total_samples;
    uint32_t adx_cur_sample;
    uint32_t adx_cur_block;
    int adx_enc;
    int adx_v3;
    int adx_coef1, adx_coef2;
    int32_t adx_h1[8], adx_h2[8];
    uint16_t adx_xor[8], adx_mul, adx_add;
    float adx_blk_pcm[32 * 8];
    uint32_t adx_blk_pos;
    uint32_t adx_blk_len;
    int adx_loop_enabled;
    uint32_t adx_loop_start_sample;
    uint32_t adx_loop_end_sample;

    /* 2-tap linear resampling state (prev & cur stereo frame) */
    float prev_l, prev_r;
    float cur_l, cur_r;
    int have_pair;
} cri_voice_t;

typedef struct {
    int used;
    cri_acb_t *acb;
    int cue_mode; /* 0=none, 1=id, 2=name, 3=index */
    int32_t cue_id;
    int32_t cue_index;
    char cue_name[256];

    float volume;
    float aisac[8];
    int aisac_set[8];
    int64_t start_time_ms;
    int32_t fade_in_ms;
    int32_t fade_out_ms;
    int paused;

    int32_t status;
    uint32_t last_pb_id;
    int64_t last_time_ms;
    uint64_t last_samples;
    int32_t last_sr;
} cri_player_t;

static pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_keycode = 0;
static cri_voice_t g_voices[MAX_VOICES];
static cri_player_t g_players[MAX_PLAYERS];
static uint32_t g_next_pb_id = 1;

static pthread_t g_mix_thread;
static int g_mix_running = 0;

#define AUDIO_BUS_SPK_1P 0
#define AUDIO_BUS_SPK_2P 1
#define AUDIO_BUS_HP_1P  2
#define AUDIO_BUS_HP_2P  3
#define MAX_AUDIO_SINKS  4

static int g_audio_cfg_loaded = 0;
static int g_bus_sink_idx[4] = { 0, 0, -1, -1 };
static char g_sink_devs[MAX_AUDIO_SINKS][256];
static int g_num_sinks = 1;

static void trim_ws(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

static int resolve_sink_slot(const char *val) {
    if (!val || !val[0] || strcasecmp(val, "default") == 0) {
        val = "";
    } else if (strcasecmp(val, "none") == 0 || strcasecmp(val, "off") == 0 || strcasecmp(val, "disabled") == 0) {
        return -1;
    }
    for (int i = 0; i < g_num_sinks; i++) {
        if (strcmp(g_sink_devs[i], val) == 0) return i;
    }
    if (g_num_sinks >= MAX_AUDIO_SINKS) return -1;
    snprintf(g_sink_devs[g_num_sinks], sizeof(g_sink_devs[0]), "%.255s", val);
    return g_num_sinks++;
}

static void load_audio_config_once(void) {
    if (g_audio_cfg_loaded) return;
    g_audio_cfg_loaded = 1;

    char bus_vals[4][256];
    snprintf(bus_vals[AUDIO_BUS_SPK_1P], sizeof(bus_vals[0]), "default");
    snprintf(bus_vals[AUDIO_BUS_SPK_2P], sizeof(bus_vals[0]), "default");
    snprintf(bus_vals[AUDIO_BUS_HP_1P],  sizeof(bus_vals[0]), "none");
    snprintf(bus_vals[AUDIO_BUS_HP_2P],  sizeof(bus_vals[0]), "none");

    char cfg_path[1056] = "audio_config.ini";
    char exe_path[1024];
    ssize_t elen = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (elen > 0) {
        exe_path[elen] = '\0';
        char *slash = strrchr(exe_path, '/');
        if (slash) {
            *slash = '\0';
            snprintf(cfg_path, sizeof(cfg_path), "%s/audio_config.ini", exe_path);
        }
    }

    FILE *fp = fopen(cfg_path, "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            trim_ws(line);
            if (!line[0] || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char *key = line;
            char *val = eq + 1;
            trim_ws(key);
            trim_ws(val);
            if (strcasecmp(key, "speaker_1p") == 0) snprintf(bus_vals[0], sizeof(bus_vals[0]), "%s", val);
            else if (strcasecmp(key, "speaker_2p") == 0) snprintf(bus_vals[1], sizeof(bus_vals[1]), "%s", val);
            else if (strcasecmp(key, "headphone_1p") == 0) snprintf(bus_vals[2], sizeof(bus_vals[2]), "%s", val);
            else if (strcasecmp(key, "headphone_2p") == 0) snprintf(bus_vals[3], sizeof(bus_vals[3]), "%s", val);
        }
        fclose(fp);
        fprintf(stderr, "[CriAudio] Loaded %s: spk1p=%s spk2p=%s hp1p=%s hp2p=%s\n",
                cfg_path, bus_vals[0], bus_vals[1], bus_vals[2], bus_vals[3]);
    }

    g_num_sinks = 0;
    memset(g_sink_devs, 0, sizeof(g_sink_devs));
    for (int b = 0; b < 4; b++) {
        g_bus_sink_idx[b] = resolve_sink_slot(bus_vals[b]);
    }
}

static float acf_get_cat_effective_gain(uint32_t id) {
    for (int i = 0; i < MAX_CATEGORIES; i++) {
        if (g_cats[i].used && g_cats[i].id == id) {
            if (g_cats[i].muted) return 0.0f;
            return g_cats[i].acf_vol * g_cats[i].runtime_vol;
        }
    }
    /* Fallback default from Mai2.acf if RegisterAcfFile was not called yet */
    if (id == 4) return 0.71f; /* SE */
    if (id == 5) return 0.75f; /* Voice */
    if (id == 6) return 0.70f; /* Credit */
    return 1.0f;
}

static void compute_player_bus_gains(const cri_player_t *p, float out_bus[4]) {
    float g4 = p->aisac_set[4] ? p->aisac[4] : 1.0f;
    float g5 = p->aisac_set[5] ? p->aisac[5] : 1.0f;
    float g2 = p->aisac_set[2] ? p->aisac[2] : 0.0f;
    float g3 = p->aisac_set[3] ? p->aisac[3] : 0.0f;
    float raw[4] = { g4, g5, g2, g3 };
    for (int b = 0; b < 4; b++) {
        float v = p->volume * raw[b];
        if (v < 0.0f) v = 0.0f;
        else if (v > 2.0f) v = 2.0f;
        out_bus[b] = v;
    }
}

static void apply_voice_gains(const cri_player_t *p, cri_voice_t *v) {
    float pb[4];
    compute_player_bus_gains(p, pb);
    float cg = (v->cue_gain > 0.0f) ? v->cue_gain : 1.0f;
    for (int i = 0; i < v->n_cats; i++) {
        cg *= acf_get_cat_effective_gain(v->cat_ids[i]);
    }
    if (cg < 0.0f) cg = 0.0f; else if (cg > 4.0f) cg = 4.0f;
    for (int b = 0; b < 4; b++) {
        v->bus_gain[b] = pb[b] * cg;
    }
}

static void free_voice(cri_voice_t *v) {
    if (v->hca) { clHCA_delete(v->hca); v->hca = NULL; }
    free(v->hca_blk_pcm);
    free(v->owned_data);
    memset(v, 0, sizeof(*v));
}

static int hca_seek_to_sample(cri_voice_t *v, uint64_t tgt_samp) {
    uint64_t abs_samp = tgt_samp + v->hca_info.encoderDelay;
    uint32_t tgt_blk = (uint32_t)(abs_samp / v->hca_info.samplesPerBlock);
    uint32_t rem = (uint32_t)(abs_samp % v->hca_info.samplesPerBlock);
    if (tgt_blk >= v->hca_info.blockCount) return -1;
    clHCA_DecodeReset(v->hca);
    uint32_t warm_blk = (tgt_blk >= 2) ? (tgt_blk - 2) : 0;
    uint8_t tmp[4096];
    size_t bsz = v->hca_info.blockSize;
    if (bsz <= sizeof(tmp)) {
        for (uint32_t b = warm_blk; b < tgt_blk; b++) {
            size_t off = v->hca_info.headerSize + (size_t)b * bsz;
            if (off + bsz <= v->data_len) {
                memcpy(tmp, v->data + off, bsz);
                clHCA_DecodeBlock(v->hca, tmp, (unsigned int)bsz);
            }
        }
    }
    v->hca_cur_block = tgt_blk;
    v->hca_discard = rem;
    v->hca_abs_sample = tgt_samp;
    v->hca_blk_pos = 0;
    v->hca_blk_len = 0;
    return 0;
}

static int voice_decode_hca_block(cri_voice_t *v) {
    uint64_t end_limit = v->hca_info.sampleCount;
    if (v->hca_ev_loop && v->hca_ev_end_sample > v->hca_ev_start_sample &&
        v->hca_ev_end_sample < end_limit) {
        end_limit = v->hca_ev_end_sample;
    }

    if (v->hca_ev_loop && (v->hca_abs_sample >= end_limit || v->hca_cur_block >= v->hca_info.blockCount)) {
        if (hca_seek_to_sample(v, v->hca_ev_start_sample) != 0) return 0;
    }

    if (v->hca_cur_block >= v->hca_info.blockCount) {
        if (v->loop_enabled && v->hca_info.loopEnabled) {
            v->hca_cur_block = v->hca_info.loopStartBlock;
            v->hca_discard = v->hca_info.loopStartDelay;
            v->hca_abs_sample = v->hca_info.loopStartSample;
        } else {
            return 0;
        }
    }
    if (v->loop_enabled && v->hca_info.loopEnabled && v->hca_cur_block > v->hca_info.loopEndBlock) {
        v->hca_cur_block = v->hca_info.loopStartBlock;
        v->hca_discard = v->hca_info.loopStartDelay;
        v->hca_abs_sample = v->hca_info.loopStartSample;
    }

    size_t bsz = v->hca_info.blockSize;
    size_t off = v->hca_info.headerSize + (size_t)v->hca_cur_block * bsz;
    if (off + bsz > v->data_len || bsz > 4096) return 0;

    uint8_t tmp[4096];
    memcpy(tmp, v->data + off, bsz);
    if (clHCA_DecodeBlock(v->hca, tmp, (unsigned int)bsz) < 0) return 0;
    clHCA_ReadSamples(v->hca, v->hca_blk_pcm);

    uint32_t start = 0;
    uint32_t end = v->hca_info.samplesPerBlock;
    if (v->hca_discard > 0) {
        if (v->hca_discard >= end) {
            v->hca_discard -= end;
            v->hca_cur_block++;
            return voice_decode_hca_block(v);
        }
        start = v->hca_discard;
        v->hca_discard = 0;
    }
    if (v->loop_enabled && v->hca_info.loopEnabled) {
        if (v->hca_cur_block == v->hca_info.loopEndBlock && v->hca_info.loopEndPadding < end) {
            end -= v->hca_info.loopEndPadding;
        }
        if (v->hca_cur_block == v->hca_info.loopEndBlock) {
            v->hca_cur_block = v->hca_info.loopStartBlock;
            v->hca_discard = v->hca_info.loopStartDelay;
        } else {
            v->hca_cur_block++;
        }
    } else {
        uint64_t remaining = (v->hca_abs_sample < end_limit)
            ? (end_limit - v->hca_abs_sample) : 0;
        if (remaining == 0) {
            if (v->hca_ev_loop && hca_seek_to_sample(v, v->hca_ev_start_sample) == 0) {
                return voice_decode_hca_block(v);
            }
            return 0;
        }
        if (end - start > remaining) end = start + (uint32_t)remaining;
        v->hca_abs_sample += (end - start);
        v->hca_cur_block++;
    }
    if (end <= start) return 0;
    v->hca_blk_pos = start;
    v->hca_blk_len = end;
    return 1;
}

static int init_hca_voice(cri_voice_t *v, int64_t seek_ms) {
    int hsz = clHCA_isOurFile(v->data, (unsigned int)v->data_len);
    if (hsz < 0 || (size_t)hsz > v->data_len) return -1;
    v->hca = clHCA_new();
    if (!v->hca) return -1;
    clHCA_SetKey(v->hca, g_keycode);
    if (clHCA_DecodeHeader(v->hca, v->data, (unsigned int)hsz) < 0) return -1;
    clHCA_getInfo(v->hca, &v->hca_info);
    if (v->hca_info.channelCount == 0 || v->hca_info.channelCount > 8 || v->hca_info.samplingRate == 0) return -1;

    v->codec = VOICE_HCA;
    v->src_sr = v->hca_info.samplingRate;
    v->channels = v->hca_info.channelCount;
    v->loop_enabled = (v->hca_info.loopEnabled != 0);
    v->hca_blk_pcm = (float *)malloc(sizeof(float) * v->hca_info.samplesPerBlock * v->channels);
    if (!v->hca_blk_pcm) return -1;

    v->hca_cur_block = 0;
    v->hca_discard = v->hca_info.encoderDelay;
    v->hca_abs_sample = 0;

    if (seek_ms > 0) {
        uint64_t tgt_samp = (uint64_t)seek_ms * v->src_sr / 1000ULL;
        if (hca_seek_to_sample(v, tgt_samp) != 0) return -1;
    }
    return 0;
}

static void adx_reset_key(cri_voice_t *v) {
    if (!v->adx_enc || g_keycode == 0) return;
    uint64_t k = g_keycode - 1;
    uint16_t x0 = (uint16_t)((k >> 27) & 0x7FFF);
    v->adx_mul = (uint16_t)(((k >> 12) & 0x7FFC) | 1);
    v->adx_add = (uint16_t)(((k << 1) & 0x7FFF) | 1);
    for (uint32_t c = 0; c < v->channels; c++) {
        v->adx_xor[c] = x0;
        x0 = (uint16_t)((x0 * v->adx_mul + v->adx_add) & 0x7FFF);
    }
}

static int init_adx_voice(cri_voice_t *v, int64_t seek_ms) {
    if (v->data_len < 32 || rd_u16be(v->data) != 0x8000) return -1;
    uint32_t doff = rd_u16be(v->data + 2) + 4;
    if (doff >= v->data_len || v->data[4] != 3 || v->data[5] != 18 || v->data[6] != 4) return -1;
    uint8_t ch = v->data[7];
    if (ch == 0 || ch > 8) return -1;
    uint32_t sr = rd_u32be(v->data + 8);
    uint32_t nsamp = rd_u32be(v->data + 12);
    uint16_t cutoff = rd_u16be(v->data + 16);
    uint16_t ver = rd_u16be(v->data + 18);
    if (sr == 0) return -1;

    v->codec = VOICE_ADX;
    v->src_sr = sr;
    v->channels = ch;
    v->adx_data_off = doff;
    v->adx_total_samples = nsamp;
    v->adx_v3 = ((ver & 0xFF00) == 0x0300);
    v->adx_enc = (ver == 0x0409 || ver == 0x0408);

    double z = cos(2.0 * M_PI * cutoff / sr);
    double a = M_SQRT2 - z, b = M_SQRT2 - 1.0;
    double c = (a - sqrt((a + b) * (a - b))) / b;
    v->adx_coef1 = (int16_t)(c * 8192.0);
    v->adx_coef2 = (int16_t)(c * c * -4096.0);

    if ((ver & 0xFF00) == 0x0400) {
        size_t hist_off = 0x18;
        size_t hist_sz = (ch > 1) ? (size_t)(4 * ch) : 8;
        for (uint8_t i = 0; i < ch; i++) {
            if (hist_off + i * 4 + 4 <= doff) {
                v->adx_h1[i] = rd_s16be(v->data + hist_off + i * 4);
                v->adx_h2[i] = rd_s16be(v->data + hist_off + i * 4 + 2);
            }
        }
        size_t ainf_off = hist_off + hist_sz + 4;
        if (doff >= ainf_off + 0x20 && memcmp(v->data + ainf_off, "AINF", 4) == 0) {
            ainf_off += rd_u32be(v->data + ainf_off + 4);
        } else {
            ainf_off = hist_off + hist_sz + 4;
        }
        if (doff >= ainf_off + 0x18) {
            uint32_t lflg = rd_u32be(v->data + ainf_off + 4);
            if (lflg) {
                v->adx_loop_enabled = 1;
                v->adx_loop_start_sample = rd_u32be(v->data + ainf_off + 8);
                v->adx_loop_end_sample   = rd_u32be(v->data + ainf_off + 16);
                if (v->adx_loop_end_sample > v->adx_total_samples || v->adx_loop_end_sample == 0)
                    v->adx_loop_end_sample = v->adx_total_samples;
            }
        }
    }
    adx_reset_key(v);
    v->loop_enabled = v->adx_loop_enabled;

    if (seek_ms > 0) {
        uint64_t tgt = (uint64_t)seek_ms * sr / 1000ULL;
        if (tgt >= nsamp) return -1;
        /* Advance key & history accurately up to tgt block */
        uint32_t tgt_blk = (uint32_t)(tgt / 32);
        for (uint32_t b = 0; b < tgt_blk; b++) {
            for (uint8_t ci = 0; ci < ch; ci++) {
                size_t off = v->adx_data_off + ((size_t)b * ch + ci) * 18;
                if (off + 18 <= v->data_len) {
                    const uint8_t *fr = v->data + off;
                    int rscale = rd_s16be(fr);
                    int scale = v->adx_enc ? (((rscale ^ v->adx_xor[ci]) & 0x1fff) + 1) : (rscale + 1);
                    for (int s = 0; s < 32; s++) {
                        int nib = (s & 1) ? (fr[2 + s / 2] & 0x0f) : ((fr[2 + s / 2] >> 4) & 0x0f);
                        if (nib >= 8) nib -= 16;
                        int samp = nib * scale + ((v->adx_coef1 * v->adx_h1[ci] + v->adx_coef2 * v->adx_h2[ci]) >> 12);
                        if (samp > 32767) samp = 32767; else if (samp < -32768) samp = -32768;
                        v->adx_h2[ci] = v->adx_h1[ci];
                        v->adx_h1[ci] = samp;
                    }
                    for (uint8_t st = 0; st < ch; st++) v->adx_xor[ci] = (uint16_t)((v->adx_xor[ci] * v->adx_mul + v->adx_add) & 0x7FFF);
                }
            }
        }
        v->adx_cur_block = tgt_blk;
        v->adx_cur_sample = (uint32_t)tgt;
    }
    return 0;
}

static int voice_decode_adx_block(cri_voice_t *v) {
    uint32_t limit = v->loop_enabled ? v->adx_loop_end_sample : v->adx_total_samples;
    if (v->adx_cur_sample >= limit) {
        if (!v->loop_enabled) return 0;
        /* Loop back to start_sample by fast-stepping from block 0 */
        adx_reset_key(v);
        memset(v->adx_h1, 0, sizeof(v->adx_h1));
        memset(v->adx_h2, 0, sizeof(v->adx_h2));
        uint32_t lblk = v->adx_loop_start_sample / 32;
        uint32_t ch = v->channels;
        for (uint32_t b = 0; b < lblk; b++) {
            for (uint32_t ci = 0; ci < ch; ci++) {
                size_t off = v->adx_data_off + ((size_t)b * ch + ci) * 18;
                if (off + 18 <= v->data_len) {
                    const uint8_t *fr = v->data + off;
                    int rscale = rd_s16be(fr);
                    int scale = v->adx_enc ? (((rscale ^ v->adx_xor[ci]) & 0x1fff) + 1) : (rscale + 1);
                    for (int s = 0; s < 32; s++) {
                        int nib = (s & 1) ? (fr[2 + s / 2] & 0x0f) : ((fr[2 + s / 2] >> 4) & 0x0f);
                        if (nib >= 8) nib -= 16;
                        int samp = nib * scale + ((v->adx_coef1 * v->adx_h1[ci] + v->adx_coef2 * v->adx_h2[ci]) >> 12);
                        if (samp > 32767) samp = 32767; else if (samp < -32768) samp = -32768;
                        v->adx_h2[ci] = v->adx_h1[ci]; v->adx_h1[ci] = samp;
                    }
                    for (uint32_t st = 0; st < ch; st++) v->adx_xor[ci] = (uint16_t)((v->adx_xor[ci] * v->adx_mul + v->adx_add) & 0x7FFF);
                }
            }
        }
        v->adx_cur_block = lblk;
        v->adx_cur_sample = v->adx_loop_start_sample;
    }

    uint32_t ch = v->channels;
    uint32_t skip = (v->adx_cur_sample % 32);
    for (uint32_t ci = 0; ci < ch; ci++) {
        size_t off = v->adx_data_off + ((size_t)v->adx_cur_block * ch + ci) * 18;
        if (off + 18 > v->data_len) return 0;
        const uint8_t *fr = v->data + off;
        if (!v->adx_enc && fr[0] == 0x80 && fr[1] == 0x01) return 0;
        int rscale = rd_s16be(fr);
        int scale = v->adx_enc ? (((rscale ^ v->adx_xor[ci]) & 0x1fff) + 1) : (rscale + 1);
        for (int s = 0; s < 32; s++) {
            int nib = (s & 1) ? (fr[2 + s / 2] & 0x0f) : ((fr[2 + s / 2] >> 4) & 0x0f);
            if (nib >= 8) nib -= 16;
            int samp = nib * scale + (v->adx_v3
                ? (((v->adx_coef1 * v->adx_h1[ci]) >> 12) + ((v->adx_coef2 * v->adx_h2[ci]) >> 12))
                : ((v->adx_coef1 * v->adx_h1[ci] + v->adx_coef2 * v->adx_h2[ci]) >> 12));
            if (samp > 32767) samp = 32767; else if (samp < -32768) samp = -32768;
            v->adx_h2[ci] = v->adx_h1[ci];
            v->adx_h1[ci] = samp;
            v->adx_blk_pcm[s * ch + ci] = (float)samp / 32768.0f;
        }
        if (v->adx_enc) {
            for (uint32_t st = 0; st < ch; st++) {
                v->adx_xor[ci] = (uint16_t)((v->adx_xor[ci] * v->adx_mul + v->adx_add) & 0x7FFF);
            }
        }
    }

    uint32_t avail = 32 - skip;
    uint32_t rem = limit - v->adx_cur_sample;
    if (avail > rem) avail = rem;
    v->adx_blk_pos = skip;
    v->adx_blk_len = skip + avail;
    v->adx_cur_block++;
    v->adx_cur_sample += avail;
    return avail > 0 ? 1 : 0;
}

static int voice_next_src_frame(cri_voice_t *v, float *out_l, float *out_r) {
    const float *pcm = NULL;
    uint32_t idx = 0;
    if (v->codec == VOICE_HCA) {
        if (v->hca_blk_pos >= v->hca_blk_len) {
            if (!voice_decode_hca_block(v)) return 0;
        }
        pcm = v->hca_blk_pcm;
        idx = v->hca_blk_pos++;
    } else if (v->codec == VOICE_ADX) {
        if (v->adx_blk_pos >= v->adx_blk_len) {
            if (!voice_decode_adx_block(v)) return 0;
        }
        pcm = v->adx_blk_pcm;
        idx = v->adx_blk_pos++;
    } else {
        return 0;
    }

    uint32_t base = idx * v->channels;
    if (v->channels == 1) {
        *out_l = *out_r = pcm[base];
    } else {
        *out_l = pcm[base];
        *out_r = pcm[base + 1];
    }
    return 1;
}

static int voice_mix_block(cri_voice_t *v, float mix_bufs[MAX_AUDIO_SINKS][MIX_BLOCK * 2], int num_sinks, int nframes) {
    if (!v->have_pair) {
        if (!voice_next_src_frame(v, &v->prev_l, &v->prev_r)) return 0;
        if (!voice_next_src_frame(v, &v->cur_l, &v->cur_r)) {
            v->cur_l = v->prev_l;
            v->cur_r = v->prev_r;
        }
        v->have_pair = 1;
        v->src_frac_pos = 0.0;
    }

    float sink_gain[MAX_AUDIO_SINKS] = {0};
    for (int b = 0; b < 4; b++) {
        int s = g_bus_sink_idx[b];
        if (s >= 0 && s < num_sinks && v->bus_gain[b] > sink_gain[s]) {
            sink_gain[s] = v->bus_gain[b];
        }
    }

    int finished = 0;
    for (int f = 0; f < nframes; f++) {
        while (v->src_frac_pos >= 1.0) {
            v->prev_l = v->cur_l;
            v->prev_r = v->cur_r;
            if (!voice_next_src_frame(v, &v->cur_l, &v->cur_r)) {
                finished = 1;
                break;
            }
            v->src_frac_pos -= 1.0;
        }
        if (finished) break;

        float t = (float)v->src_frac_pos;
        float sl = v->prev_l + (v->cur_l - v->prev_l) * t;
        float sr = v->prev_r + (v->cur_r - v->prev_r) * t;

        if (!v->fading_out && v->env_gain < 1.0f) {
            v->env_gain += v->fade_in_step;
            if (v->env_gain > 1.0f) v->env_gain = 1.0f;
        } else if (v->fading_out) {
            v->env_gain -= v->fade_out_step;
            if (v->env_gain <= 0.0f) {
                v->env_gain = 0.0f;
                finished = 1;
                break;
            }
        }

        for (int s = 0; s < num_sinks; s++) {
            float g = sink_gain[s] * v->env_gain;
            if (g > 0.0f) {
                mix_bufs[s][f * 2 + 0] += sl * g;
                mix_bufs[s][f * 2 + 1] += sr * g;
            }
        }
        v->src_frac_pos += v->src_step;
        v->rendered_out_frames++;
    }
    return !finished;
}

/* =========================================================================
 * PulseAudio / PipeWire Output Mixer Thread (dlopen libpulse-simple.so.0)
 * ========================================================================= */
typedef struct pa_simple pa_simple;
typedef struct { uint32_t format; uint32_t rate; uint8_t channels; } cri_pa_sample_spec;
typedef struct { uint32_t maxlength, tlength, prebuf, minreq, fragsize; } cri_pa_buffer_attr;

typedef pa_simple *(*pfn_pa_simple_new)(const char*, const char*, int, const char*,
                                        const char*, const cri_pa_sample_spec*, const void*,
                                        const cri_pa_buffer_attr*, int*);
typedef int (*pfn_pa_simple_write)(pa_simple*, const void*, size_t, int*);
typedef void (*pfn_pa_simple_free)(pa_simple*);

static void *mixer_thread_main(void *arg) {
    (void)arg;
    load_audio_config_once();

    void *pa_lib = dlopen("libpulse-simple.so.0", RTLD_LAZY);
    pfn_pa_simple_new fn_new = pa_lib ? (pfn_pa_simple_new)dlsym(pa_lib, "pa_simple_new") : NULL;
    pfn_pa_simple_write fn_write = pa_lib ? (pfn_pa_simple_write)dlsym(pa_lib, "pa_simple_write") : NULL;
    pfn_pa_simple_free fn_free = pa_lib ? (pfn_pa_simple_free)dlsym(pa_lib, "pa_simple_free") : NULL;

    pa_simple *pa[MAX_AUDIO_SINKS] = {0};
    int any_pa = 0;
    if (fn_new && fn_write) {
        cri_pa_sample_spec ss = { 3 /* PA_SAMPLE_S16LE */, MIX_SR, 2 };
        cri_pa_buffer_attr attr = {
            (uint32_t)-1,
            MIX_BLOCK * 4 * 4, /* ~42ms target buffer */
            (uint32_t)-1,
            MIX_BLOCK * 4,     /* ~10.6ms minreq */
            (uint32_t)-1
        };
        for (int s = 0; s < g_num_sinks; s++) {
            const char *dev = g_sink_devs[s][0] ? g_sink_devs[s] : NULL;
            int err = 0;
            pa[s] = fn_new(NULL, "Sinmai", 1 /* PA_STREAM_PLAYBACK */, dev, "maimaiDX Audio", &ss, NULL, &attr, &err);
            if (!pa[s]) {
                fprintf(stderr, "[CriAudio] pa_simple_new(dev=%s) failed err=%d\n", dev ? dev : "default", err);
            } else {
                any_pa = 1;
                fprintf(stderr, "[CriAudio] PulseAudio/PipeWire sink[%d] opened: %s (48000Hz stereo)\n",
                        s, dev ? dev : "default");
            }
        }
    } else {
        fprintf(stderr, "[CriAudio] libpulse-simple.so.0 unavailable; running silent timing fallback\n");
    }

    float mix_f[MAX_AUDIO_SINKS][MIX_BLOCK * 2];
    int16_t out_s16[MIX_BLOCK * 2];

    while (g_mix_running) {
        memset(mix_f, 0, sizeof(mix_f));

        pthread_mutex_lock(&g_audio_lock);
        for (int i = 0; i < MAX_VOICES; i++) {
            cri_voice_t *v = &g_voices[i];
            if (!v->active || v->paused) continue;
            int alive = voice_mix_block(v, mix_f, g_num_sinks, MIX_BLOCK);

            int pidx = v->player_idx;
            cri_player_t *pl = (pidx >= 0 && pidx < MAX_PLAYERS && g_players[pidx].used) ? &g_players[pidx] : NULL;
            int64_t pos_ms = v->start_time_ms + (int64_t)(v->rendered_out_frames * 1000ULL / MIX_SR);

            if (pl && pl->last_pb_id == v->pb_id) {
                pl->last_time_ms = pos_ms;
                pl->last_samples = v->rendered_out_frames;
                pl->last_sr = MIX_SR;
                if (!alive) {
                    pl->status = v->fading_out ? ATOM_STATUS_STOP : ATOM_STATUS_PLAYEND;
                }
            }
            if (!alive) {
                free_voice(v);
            }
        }
        pthread_mutex_unlock(&g_audio_lock);

        int wrote_any = 0;
        for (int s = 0; s < g_num_sinks; s++) {
            if (!pa[s]) continue;
            for (int i = 0; i < MIX_BLOCK * 2; i++) {
                float x = mix_f[s][i];
                if (x > 1.0f) x = 1.0f;
                else if (x < -1.0f) x = -1.0f;
                out_s16[i] = (int16_t)(x * 32767.0f);
            }
            int err = 0;
            if (fn_write(pa[s], out_s16, sizeof(out_s16), &err) >= 0) {
                wrote_any = 1;
            }
        }
        if (!any_pa || !wrote_any) {
            usleep((useconds_t)(MIX_BLOCK * 1000000ULL / MIX_SR));
        }
    }

    for (int s = 0; s < g_num_sinks; s++) {
        if (pa[s] && fn_free) fn_free(pa[s]);
    }
    if (pa_lib) dlclose(pa_lib);
    return NULL;
}

/* =========================================================================
 * Public API
 * ========================================================================= */
void cri_audio_init(void) {
    pthread_mutex_lock(&g_audio_lock);
    if (!g_mix_running) {
        g_mix_running = 1;
        pthread_create(&g_mix_thread, NULL, mixer_thread_main, NULL);
    }
    pthread_mutex_unlock(&g_audio_lock);
}

void cri_audio_shutdown(void) {
    int was_running = 0;
    pthread_mutex_lock(&g_audio_lock);
    if (g_mix_running) {
        g_mix_running = 0;
        was_running = 1;
    }
    pthread_mutex_unlock(&g_audio_lock);
    if (was_running) pthread_join(g_mix_thread, NULL);

    pthread_mutex_lock(&g_audio_lock);
    for (int i = 0; i < MAX_VOICES; i++) free_voice(&g_voices[i]);
    for (int i = 0; i < MAX_ACBS; i++) if (g_acbs[i].used) acb_free_slot(&g_acbs[i]);
    memset(g_players, 0, sizeof(g_players));
    pthread_mutex_unlock(&g_audio_lock);
}

void cri_audio_set_keycode(uint64_t keycode) {
    pthread_mutex_lock(&g_audio_lock);
    g_keycode = keycode;
    pthread_mutex_unlock(&g_audio_lock);
}

static void *register_acb_buffer(uint8_t *buf, size_t sz, const char *awb_path) {
    cri_acb_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.used = 1;
    tmp.acb_buf = buf;
    tmp.acb_size = sz;

    if (utf_parse(buf, sz, &tmp.header_tbl) != 0 || tmp.header_tbl.num_rows == 0) {
        free(buf);
        return NULL;
    }
    tmp.has_cue  = acb_parse_subtable(&tmp.header_tbl, "CueTable", &tmp.cue_tbl);
    tmp.has_name = acb_parse_subtable(&tmp.header_tbl, "CueNameTable", &tmp.name_tbl);
    tmp.has_seq    = acb_parse_subtable(&tmp.header_tbl, "SequenceTable", &tmp.seq_tbl);
    tmp.has_trk    = acb_parse_subtable(&tmp.header_tbl, "TrackTable", &tmp.trk_tbl);
    tmp.has_trk_ev = acb_parse_subtable(&tmp.header_tbl, "TrackEventTable", &tmp.trk_ev_tbl);
    tmp.has_syn    = acb_parse_subtable(&tmp.header_tbl, "SynthTable", &tmp.syn_tbl);
    tmp.has_wav    = acb_parse_subtable(&tmp.header_tbl, "WaveformTable", &tmp.wav_tbl);
    tmp.has_seq_cmd = acb_parse_subtable(&tmp.header_tbl, "SeqCommandTable", &tmp.seq_cmd_tbl);
    tmp.has_trk_cmd = acb_parse_subtable(&tmp.header_tbl, "TrackCommandTable", &tmp.trk_cmd_tbl);
    tmp.has_syn_cmd = acb_parse_subtable(&tmp.header_tbl, "SynthCommandTable", &tmp.syn_cmd_tbl);

    uint32_t emb_len = 0;
    const uint8_t *emb = utf_get_data(&tmp.header_tbl, 0, "AwbFile", &emb_len);
    if (emb && emb_len >= 16) {
        tmp.emb_awb_ptr = emb;
        tmp.emb_awb_size = emb_len;
        afs2_parse_header(emb, emb_len, emb_len, &tmp.emb_afs2);
    }
    if (awb_path && awb_path[0]) {
        acb_init_ext_awb(&tmp, awb_path);
    }

    pthread_mutex_lock(&g_audio_lock);
    int slot = -1;
    for (int i = 0; i < MAX_ACBS; i++) {
        if (!g_acbs[i].used) { slot = i; break; }
    }
    if (slot >= 0) {
        g_acbs[slot] = tmp;
        void *hn = &g_acbs[slot];
        pthread_mutex_unlock(&g_audio_lock);
        return hn;
    }
    pthread_mutex_unlock(&g_audio_lock);
    acb_free_slot(&tmp);
    return NULL;
}

void *cri_audio_load_acb_file(const char *acb_path, const char *awb_path) {
    size_t sz = 0;
    uint8_t *buf = read_entire_file(acb_path, &sz);
    if (!buf) return NULL;
    return register_acb_buffer(buf, sz, awb_path);
}

void *cri_audio_load_acb_data(const uint8_t *acb_data, int32_t acb_size, const char *awb_path) {
    if (!acb_data || acb_size <= 0) return NULL;
    uint8_t *buf = (uint8_t *)malloc((size_t)acb_size);
    if (!buf) return NULL;
    memcpy(buf, acb_data, (size_t)acb_size);
    return register_acb_buffer(buf, (size_t)acb_size, awb_path);
}

static cri_acb_t *find_acb(void *acb_hn) {
    for (int i = 0; i < MAX_ACBS; i++) {
        if (g_acbs[i].used && &g_acbs[i] == acb_hn) return &g_acbs[i];
    }
    return NULL;
}

void cri_audio_release_acb(void *acb_hn) {
    pthread_mutex_lock(&g_audio_lock);
    cri_acb_t *a = find_acb(acb_hn);
    if (a) {
        for (int p = 0; p < MAX_PLAYERS; p++) {
            if (g_players[p].used && g_players[p].acb == a) g_players[p].acb = NULL;
        }
        acb_free_slot(a);
    }
    pthread_mutex_unlock(&g_audio_lock);
}

int32_t cri_audio_acb_get_num_cues(void *acb_hn) {
    pthread_mutex_lock(&g_audio_lock);
    cri_acb_t *a = find_acb(acb_hn);
    int32_t r = (a && a->has_cue) ? (int32_t)a->cue_tbl.num_rows : 0;
    pthread_mutex_unlock(&g_audio_lock);
    return r;
}

int32_t cri_audio_acb_exists_id(void *acb_hn, int32_t id) {
    pthread_mutex_lock(&g_audio_lock);
    cri_acb_t *a = find_acb(acb_hn);
    int32_t r = (a && acb_find_cue_index_by_id(a, id) >= 0) ? 1 : 0;
    pthread_mutex_unlock(&g_audio_lock);
    return r;
}

int32_t cri_audio_acb_exists_name(void *acb_hn, const char *name) {
    pthread_mutex_lock(&g_audio_lock);
    cri_acb_t *a = find_acb(acb_hn);
    int32_t r = (a && acb_find_cue_index_by_name(a, name) >= 0) ? 1 : 0;
    pthread_mutex_unlock(&g_audio_lock);
    return r;
}

static cri_player_t *find_player(void *player) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_players[i].used && &g_players[i] == player) return &g_players[i];
    }
    return NULL;
}

static void player_reset_fields(cri_player_t *p) {
    p->volume = 1.0f;
    for (int i = 0; i < 8; i++) { p->aisac[i] = 1.0f; p->aisac_set[i] = 0; }
    p->start_time_ms = 0;
    p->fade_in_ms = 0;
    p->fade_out_ms = 0;
}

void *cri_audio_player_create(void) {
    cri_audio_init();
    pthread_mutex_lock(&g_audio_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!g_players[i].used) {
            memset(&g_players[i], 0, sizeof(g_players[i]));
            g_players[i].used = 1;
            player_reset_fields(&g_players[i]);
            void *ret = &g_players[i];
            pthread_mutex_unlock(&g_audio_lock);
            return ret;
        }
    }
    pthread_mutex_unlock(&g_audio_lock);
    return NULL;
}

void cri_audio_player_destroy(void *player) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    if (p) {
        int pidx = (int)(p - g_players);
        for (int i = 0; i < MAX_VOICES; i++) {
            if (g_voices[i].active && g_voices[i].player_idx == pidx) free_voice(&g_voices[i]);
        }
        memset(p, 0, sizeof(*p));
    }
    pthread_mutex_unlock(&g_audio_lock);
}

void cri_audio_player_reset_params(void *player) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    if (p) player_reset_fields(p);
    pthread_mutex_unlock(&g_audio_lock);
}

void cri_audio_player_set_cue_id(void *player, void *acb_hn, int32_t id) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    if (p) { p->acb = find_acb(acb_hn); p->cue_mode = 1; p->cue_id = id; }
    pthread_mutex_unlock(&g_audio_lock);
}

void cri_audio_player_set_cue_name(void *player, void *acb_hn, const char *name) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    if (p) {
        p->acb = find_acb(acb_hn);
        p->cue_mode = 2;
        snprintf(p->cue_name, sizeof(p->cue_name), "%s", name ? name : "");
    }
    pthread_mutex_unlock(&g_audio_lock);
}

void cri_audio_player_set_cue_index(void *player, void *acb_hn, int32_t index) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    if (p) { p->acb = find_acb(acb_hn); p->cue_mode = 3; p->cue_index = index; }
    pthread_mutex_unlock(&g_audio_lock);
}

void cri_audio_player_set_start_time(void *player, int64_t start_time_ms) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    if (p) p->start_time_ms = start_time_ms;
    pthread_mutex_unlock(&g_audio_lock);
}

static void update_live_player_gains(cri_player_t *p) {
    int pidx = (int)(p - g_players);
    for (int i = 0; i < MAX_VOICES; i++) {
        if (g_voices[i].active && g_voices[i].player_idx == pidx) {
            apply_voice_gains(p, &g_voices[i]);
        }
    }
}

static void update_all_live_gains_locked(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_players[i].used) update_live_player_gains(&g_players[i]);
    }
}

int32_t cri_audio_register_acf_file(const char *acf_path) {
    size_t sz = 0;
    uint8_t *buf = read_entire_file(acf_path, &sz);
    if (!buf) return -1;
    utf_table_t hdr, cat_tbl, cname_tbl, cmd_tbl;
    if (utf_parse(buf, sz, &hdr) != 0 || hdr.num_rows == 0) {
        free(buf);
        return -1;
    }
    int has_cat = acb_parse_subtable(&hdr, "CategoryTable", &cat_tbl);
    int has_cn  = acb_parse_subtable(&hdr, "CategoryNameTable", &cname_tbl);
    int has_cmd = acb_parse_subtable(&hdr, "CommandTable", &cmd_tbl);
    if (!has_cat) {
        free(buf);
        return -1;
    }

    pthread_mutex_lock(&g_audio_lock);
    memset(g_cats, 0, sizeof(g_cats));
    uint32_t n = cat_tbl.num_rows < MAX_CATEGORIES ? cat_tbl.num_rows : MAX_CATEGORIES;
    for (uint32_t i = 0; i < n; i++) {
        g_cats[i].used = 1;
        g_cats[i].id = (uint32_t)utf_get_u64(&cat_tbl, i, "Id", i);
        g_cats[i].acf_vol = read_row_cmd_vol_and_cats(&cat_tbl, i, has_cmd, &cmd_tbl, NULL, NULL, 0);
        g_cats[i].runtime_vol = 1.0f;
        g_cats[i].muted = 0;
    }
    if (has_cn) {
        for (uint32_t r = 0; r < cname_tbl.num_rows; r++) {
            uint32_t idx = (uint32_t)utf_get_u64(&cname_tbl, r, "Index", 0xFFFF);
            const char *nm = utf_get_str(&cname_tbl, r, "Name");
            if (idx < n && nm) {
                snprintf(g_cats[idx].name, sizeof(g_cats[idx].name), "%s", nm);
            }
        }
    }
    update_all_live_gains_locked();
    pthread_mutex_unlock(&g_audio_lock);
    free(buf);
    return 0;
}

void cri_audio_category_set_volume_by_id(uint32_t id, float vol) {
    pthread_mutex_lock(&g_audio_lock);
    for (int i = 0; i < MAX_CATEGORIES; i++) {
        if (g_cats[i].used && g_cats[i].id == id) {
            g_cats[i].runtime_vol = vol;
        }
    }
    update_all_live_gains_locked();
    pthread_mutex_unlock(&g_audio_lock);
}

void cri_audio_category_set_volume_by_name(const char *name, float vol) {
    if (!name) return;
    pthread_mutex_lock(&g_audio_lock);
    for (int i = 0; i < MAX_CATEGORIES; i++) {
        if (g_cats[i].used && strcmp(g_cats[i].name, name) == 0) {
            g_cats[i].runtime_vol = vol;
        }
    }
    update_all_live_gains_locked();
    pthread_mutex_unlock(&g_audio_lock);
}

float cri_audio_category_get_volume_by_id(uint32_t id) {
    float v = 1.0f;
    pthread_mutex_lock(&g_audio_lock);
    for (int i = 0; i < MAX_CATEGORIES; i++) {
        if (g_cats[i].used && g_cats[i].id == id) { v = g_cats[i].runtime_vol; break; }
    }
    pthread_mutex_unlock(&g_audio_lock);
    return v;
}

float cri_audio_category_get_volume_by_name(const char *name) {
    float v = 1.0f;
    if (!name) return v;
    pthread_mutex_lock(&g_audio_lock);
    for (int i = 0; i < MAX_CATEGORIES; i++) {
        if (g_cats[i].used && strcmp(g_cats[i].name, name) == 0) { v = g_cats[i].runtime_vol; break; }
    }
    pthread_mutex_unlock(&g_audio_lock);
    return v;
}

void cri_audio_category_mute_by_id(uint32_t id, int32_t mute) {
    pthread_mutex_lock(&g_audio_lock);
    for (int i = 0; i < MAX_CATEGORIES; i++) {
        if (g_cats[i].used && g_cats[i].id == id) g_cats[i].muted = mute ? 1 : 0;
    }
    update_all_live_gains_locked();
    pthread_mutex_unlock(&g_audio_lock);
}

void cri_audio_category_mute_by_name(const char *name, int32_t mute) {
    if (!name) return;
    pthread_mutex_lock(&g_audio_lock);
    for (int i = 0; i < MAX_CATEGORIES; i++) {
        if (g_cats[i].used && strcmp(g_cats[i].name, name) == 0) g_cats[i].muted = mute ? 1 : 0;
    }
    update_all_live_gains_locked();
    pthread_mutex_unlock(&g_audio_lock);
}

void cri_audio_player_set_volume(void *player, float vol) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    if (p) { p->volume = vol; update_live_player_gains(p); }
    pthread_mutex_unlock(&g_audio_lock);
}

void cri_audio_player_set_aisac_by_id(void *player, uint16_t ctrl_id, float val) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    if (p && ctrl_id < 8) { p->aisac[ctrl_id] = val; p->aisac_set[ctrl_id] = 1; update_live_player_gains(p); }
    pthread_mutex_unlock(&g_audio_lock);
}

void cri_audio_player_set_fade_in_time(void *player, int32_t ms) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    if (p) p->fade_in_ms = ms;
    pthread_mutex_unlock(&g_audio_lock);
}

void cri_audio_player_set_fade_out_time(void *player, int32_t ms) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    if (p) p->fade_out_ms = ms;
    pthread_mutex_unlock(&g_audio_lock);
}

void cri_audio_player_pause(void *player, int32_t sw) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    if (p) {
        p->paused = sw ? 1 : 0;
        if (!p->paused && p->status == ATOM_STATUS_PREP) {
            p->status = ATOM_STATUS_PLAYING;
        }
        int pidx = (int)(p - g_players);
        for (int i = 0; i < MAX_VOICES; i++) {
            if (g_voices[i].active && g_voices[i].player_idx == pidx) g_voices[i].paused = p->paused;
        }
    }
    pthread_mutex_unlock(&g_audio_lock);
}

int32_t cri_audio_player_is_paused(void *player) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    int32_t r = p ? p->paused : 0;
    pthread_mutex_unlock(&g_audio_lock);
    return r;
}

void cri_audio_player_update_all(void *player) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    if (p) update_live_player_gains(p);
    pthread_mutex_unlock(&g_audio_lock);
}

static uint32_t launch_player(void *player, int start_paused) {
    /* Step 1: resolve cue and collect disk/memory offsets under lock */
    resolved_wave_t waves[4];
    memset(waves, 0, sizeof(waves));
    int nwaves = 0;
    char ext_path[1024] = {0};
    uint64_t spans_off[4] = {0}, spans_len[4] = {0};
    int span_ok[4] = {0};
    cri_player_t snap;
    int pidx = -1;

    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    if (!p) { pthread_mutex_unlock(&g_audio_lock); return 0; }
    pidx = (int)(p - g_players);
    snap = *p;
    cri_acb_t *a = p->acb;
    if (a) {
        int32_t cidx = -1;
        if (p->cue_mode == 1) cidx = acb_find_cue_index_by_id(a, p->cue_id);
        else if (p->cue_mode == 2) cidx = acb_find_cue_index_by_name(a, p->cue_name);
        else if (p->cue_mode == 3) cidx = p->cue_index;

        if (cidx >= 0) nwaves = acb_resolve_cue_index(a, cidx, waves, 4);
        snprintf(ext_path, sizeof(ext_path), "%s", a->awb_path);
        for (int i = 0; i < nwaves; i++) {
            if (waves[i].streaming) {
                if (afs2_get_span(&a->ext_afs2, waves[i].awb_id, &spans_off[i], &spans_len[i]) == 0) {
                    span_ok[i] = 1;
                }
            } else {
                if (afs2_get_span(&a->emb_afs2, waves[i].awb_id, &spans_off[i], &spans_len[i]) == 0 &&
                    a->emb_awb_ptr && spans_off[i] + spans_len[i] <= a->emb_awb_size) {
                    span_ok[i] = 2;
                }
            }
        }
    }
    uint32_t pb_id = g_next_pb_id++;
    if (g_next_pb_id == 0) g_next_pb_id = 1;
    pthread_mutex_unlock(&g_audio_lock);

    /* Step 2: allocate and initialize voices OUTSIDE g_audio_lock */
    cri_voice_t prepared[4];
    int n_prep = 0;
    memset(prepared, 0, sizeof(prepared));
    for (int i = 0; i < nwaves; i++) {
        if (!span_ok[i]) continue;
        uint8_t *owned = NULL;
        size_t dlen = (size_t)spans_len[i];
        owned = (uint8_t *)malloc(dlen);
        if (!owned) continue;

        if (span_ok[i] == 1) {
            FILE *f = fopen(ext_path, "rb");
            if (!f || fseek(f, (long)spans_off[i], SEEK_SET) != 0 || fread(owned, 1, dlen, f) != dlen) {
                if (f) fclose(f);
                free(owned);
                continue;
            }
            fclose(f);
        } else {
            /* Copy embedded AWB slice safely while checking under lock */
            pthread_mutex_lock(&g_audio_lock);
            cri_acb_t *aa = find_player(player) ? find_player(player)->acb : NULL;
            int copied = 0;
            if (aa && aa->emb_awb_ptr && spans_off[i] + dlen <= aa->emb_awb_size) {
                memcpy(owned, aa->emb_awb_ptr + spans_off[i], dlen);
                copied = 1;
            }
            pthread_mutex_unlock(&g_audio_lock);
            if (!copied) { free(owned); continue; }
        }

        cri_voice_t *v = &prepared[n_prep];
        v->owned_data = owned;
        v->data = owned;
        v->data_len = dlen;

        int64_t seek_ms = snap.start_time_ms;
        if (seek_ms <= 0 && waves[i].ev_has_start && waves[i].ev_start_ms > 0) {
            seek_ms = waves[i].ev_start_ms;
        }

        int dec_ok = -1;
        if (waves[i].encode_type == 2 || (dlen >= 4 && clHCA_isOurFile(owned, (unsigned int)dlen) > 0)) {
            dec_ok = init_hca_voice(v, seek_ms);
        } else if (waves[i].encode_type == 0) {
            dec_ok = init_adx_voice(v, seek_ms);
        }
        if (dec_ok != 0) {
            free_voice(v);
            continue;
        }
        if (v->codec == VOICE_HCA && waves[i].ev_has_start && waves[i].ev_loop && !v->loop_enabled) {
            uint64_t ev_s = (uint64_t)waves[i].ev_start_ms * v->src_sr / 1000ULL;
            uint64_t ev_e = (waves[i].ev_has_end && waves[i].ev_end_ms > waves[i].ev_start_ms)
                ? ((uint64_t)waves[i].ev_end_ms * v->src_sr / 1000ULL)
                : v->hca_info.sampleCount;
            if (ev_e > v->hca_info.sampleCount) ev_e = v->hca_info.sampleCount;
            if (ev_s < ev_e) {
                v->hca_ev_loop = 1;
                v->hca_ev_start_sample = ev_s;
                v->hca_ev_end_sample = ev_e;
            }
        }
        v->active = 1;
        v->pb_id = pb_id;
        v->player_idx = pidx;
        v->paused = start_paused ? 1 : snap.paused;
        v->start_time_ms = seek_ms > 0 ? seek_ms : 0;
        v->src_step = (double)v->src_sr / (double)MIX_SR;
        v->cue_gain = waves[i].cue_gain > 0.0f ? waves[i].cue_gain : 1.0f;
        v->n_cats = waves[i].n_cats;
        for (int c = 0; c < waves[i].n_cats; c++) v->cat_ids[c] = waves[i].cat_ids[c];
        apply_voice_gains(&snap, v);
        if (snap.fade_in_ms > 0) {
            v->env_gain = 0.0f;
            v->fade_in_step = 1.0f / ((float)snap.fade_in_ms * MIX_SR / 1000.0f);
        } else {
            v->env_gain = 1.0f;
        }
        n_prep++;
    }

    /* Step 3: install prepared voices atomically under lock */
    pthread_mutex_lock(&g_audio_lock);
    p = find_player(player);
    if (!p) {
        pthread_mutex_unlock(&g_audio_lock);
        for (int i = 0; i < n_prep; i++) free_voice(&prepared[i]);
        return 0;
    }
    /* Reapply most recent volume/aisac in case SetVolume/SetAisac happened */
    p->last_pb_id = pb_id;
    p->last_time_ms = p->start_time_ms > 0 ? p->start_time_ms : 0;
    p->last_samples = 0;
    p->last_sr = MIX_SR;

    if (n_prep == 0) {
        p->status = start_paused ? ATOM_STATUS_PREP : ATOM_STATUS_PLAYEND;
        pthread_mutex_unlock(&g_audio_lock);
        return pb_id;
    }
    p->status = start_paused ? ATOM_STATUS_PREP : ATOM_STATUS_PLAYING;

    for (int i = 0; i < n_prep; i++) {
        apply_voice_gains(p, &prepared[i]);
        int vslot = -1;
        for (int s = 0; s < MAX_VOICES; s++) {
            if (!g_voices[s].active) { vslot = s; break; }
        }
        if (vslot < 0) {
            /* Steal oldest voice if saturated */
            vslot = 0;
            free_voice(&g_voices[vslot]);
        }
        g_voices[vslot] = prepared[i];
    }
    pthread_mutex_unlock(&g_audio_lock);
    return pb_id;
}

uint32_t cri_audio_player_start(void *player) {
    return launch_player(player, 0);
}

uint32_t cri_audio_player_prepare(void *player) {
    return launch_player(player, 1);
}

void cri_audio_player_stop(void *player, int32_t immediate) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    if (p) {
        int pidx = (int)(p - g_players);
        int do_fade = (!immediate && p->fade_out_ms > 0);
        for (int i = 0; i < MAX_VOICES; i++) {
            if (g_voices[i].active && g_voices[i].player_idx == pidx) {
                if (do_fade) {
                    g_voices[i].fading_out = 1;
                    g_voices[i].fade_out_step = 1.0f / ((float)p->fade_out_ms * MIX_SR / 1000.0f);
                } else {
                    free_voice(&g_voices[i]);
                }
            }
        }
        if (!do_fade) p->status = ATOM_STATUS_STOP;
    }
    pthread_mutex_unlock(&g_audio_lock);
}

int32_t cri_audio_player_get_status(void *player) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    int32_t r = p ? p->status : ATOM_STATUS_STOP;
    pthread_mutex_unlock(&g_audio_lock);
    return r;
}

int64_t cri_audio_player_get_time(void *player) {
    pthread_mutex_lock(&g_audio_lock);
    cri_player_t *p = find_player(player);
    int64_t r = p ? p->last_time_ms : 0;
    pthread_mutex_unlock(&g_audio_lock);
    return r;
}

static cri_voice_t *find_voice_by_pb(uint32_t pb_id) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (g_voices[i].active && g_voices[i].pb_id == pb_id) return &g_voices[i];
    }
    return NULL;
}

void cri_audio_pb_stop(uint32_t pb_id, int32_t immediate) {
    pthread_mutex_lock(&g_audio_lock);
    for (int i = 0; i < MAX_VOICES; i++) {
        if (g_voices[i].active && g_voices[i].pb_id == pb_id) {
            int pidx = g_voices[i].player_idx;
            cri_player_t *p = (pidx >= 0 && pidx < MAX_PLAYERS && g_players[pidx].used) ? &g_players[pidx] : NULL;
            if (!immediate && p && p->fade_out_ms > 0) {
                g_voices[i].fading_out = 1;
                g_voices[i].fade_out_step = 1.0f / ((float)p->fade_out_ms * MIX_SR / 1000.0f);
            } else {
                free_voice(&g_voices[i]);
                if (p && p->last_pb_id == pb_id) p->status = ATOM_STATUS_STOP;
            }
        }
    }
    pthread_mutex_unlock(&g_audio_lock);
}

void cri_audio_pb_pause(uint32_t pb_id, int32_t sw) {
    pthread_mutex_lock(&g_audio_lock);
    for (int i = 0; i < MAX_VOICES; i++) {
        if (g_voices[i].active && g_voices[i].pb_id == pb_id) g_voices[i].paused = sw ? 1 : 0;
    }
    pthread_mutex_unlock(&g_audio_lock);
}

int32_t cri_audio_pb_is_paused(uint32_t pb_id) {
    pthread_mutex_lock(&g_audio_lock);
    cri_voice_t *v = find_voice_by_pb(pb_id);
    int32_t r = v ? v->paused : 0;
    pthread_mutex_unlock(&g_audio_lock);
    return r;
}

int32_t cri_audio_pb_get_status(uint32_t pb_id) {
    pthread_mutex_lock(&g_audio_lock);
    cri_voice_t *v = find_voice_by_pb(pb_id);
    int32_t r = v ? (v->paused ? ATOM_STATUS_PREP : ATOM_STATUS_PLAYING) : ATOM_STATUS_PLAYEND;
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (g_players[p].used && g_players[p].last_pb_id == pb_id) { r = g_players[p].status; break; }
    }
    pthread_mutex_unlock(&g_audio_lock);
    return r;
}

int64_t cri_audio_pb_get_time(uint32_t pb_id) {
    pthread_mutex_lock(&g_audio_lock);
    cri_voice_t *v = find_voice_by_pb(pb_id);
    int64_t r = 0;
    if (v) r = v->start_time_ms + (int64_t)(v->rendered_out_frames * 1000ULL / MIX_SR);
    else {
        for (int p = 0; p < MAX_PLAYERS; p++) {
            if (g_players[p].used && g_players[p].last_pb_id == pb_id) { r = g_players[p].last_time_ms; break; }
        }
    }
    pthread_mutex_unlock(&g_audio_lock);
    return r;
}

int32_t cri_audio_pb_get_num_played_samples(uint32_t pb_id, int64_t *num_samples, int32_t *sampling_rate) {
    pthread_mutex_lock(&g_audio_lock);
    cri_voice_t *v = find_voice_by_pb(pb_id);
    int32_t ok = 0;
    if (v) {
        if (num_samples) *num_samples = (int64_t)v->rendered_out_frames;
        if (sampling_rate) *sampling_rate = MIX_SR;
        ok = 1;
    }
    pthread_mutex_unlock(&g_audio_lock);
    return ok;
}

int32_t cri_audio_parse_usm_video_info(const char *path,
                                       uint32_t *width, uint32_t *height,
                                       uint32_t *disp_width, uint32_t *disp_height,
                                       uint32_t *framerate_n, uint32_t *framerate_d,
                                       uint32_t *total_frames) {
    if (!path || !path[0]) return -1;
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    uint8_t buf[16384];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n < 32) return -1;

    size_t pos = 0;
    while (pos + 32 <= n) {
        uint32_t sz = rd_u32be(buf + pos + 4);
        if (sz < 24 || pos + 8 + sz > n) break;
        uint8_t doff = buf[pos + 9];
        uint8_t ctype = buf[pos + 15];
        if (memcmp(buf + pos, "@SFV", 4) == 0 && ctype == 1) {
            size_t pay = pos + 8 + doff;
            if (pay + 8 <= pos + 8 + sz && memcmp(buf + pay, "@UTF", 4) == 0) {
                utf_table_t tbl;
                if (utf_parse(buf + pay, (pos + 8 + sz) - pay, &tbl) == 0 && tbl.num_rows > 0) {
                    if (width)       *width       = (uint32_t)utf_get_u64(&tbl, 0, "width", 1080);
                    if (height)      *height      = (uint32_t)utf_get_u64(&tbl, 0, "height", 1080);
                    if (disp_width)  *disp_width  = (uint32_t)utf_get_u64(&tbl, 0, "disp_width", 1080);
                    if (disp_height) *disp_height = (uint32_t)utf_get_u64(&tbl, 0, "disp_height", 1080);
                    if (framerate_n) *framerate_n = (uint32_t)utf_get_u64(&tbl, 0, "framerate_n", 30000);
                    if (framerate_d) *framerate_d = (uint32_t)utf_get_u64(&tbl, 0, "framerate_d", 1000);
                    if (total_frames)*total_frames= (uint32_t)utf_get_u64(&tbl, 0, "total_frames", 9000);
                    return 0;
                }
            }
        }
        pos += 8 + sz;
    }
    return -1;
}
