#ifndef CRI_VIDEO_H
#define CRI_VIDEO_H

#include <stdint.h>

typedef struct CriVideoStream CriVideoStream;

/* Initialize global VP9 decoder library bindings cleanly (safe to call repeatedly). */
int cri_video_global_init(void);

/* Open a CRI .dat USM movie file and build its @SFV frame index.
 * If cipher_key != 0, @SFV payloads are decrypted with the USM VP9 key schedule.
 * Returns NULL if file cannot be opened or contains no valid VP9 @SFV chunks. */
CriVideoStream *cri_video_open(const char *path, uint64_t cipher_key);

/* Close stream and release all decoder/frame buffers. */
void cri_video_close(CriVideoStream *vs);

/* Query video metadata parsed from VIDEO_HDRINFO / IVF header. */
uint32_t cri_video_get_width(const CriVideoStream *vs);
uint32_t cri_video_get_height(const CriVideoStream *vs);
uint32_t cri_video_get_frame_count(const CriVideoStream *vs);
uint32_t cri_video_get_alpha_type(const CriVideoStream *vs);

/* Decode up to target_frame (0-based), updating internal Y/U/V buffers.
 * Returns 0 on success (frame available), -1 on decode error. */
int cri_video_decode_frame(CriVideoStream *vs, uint32_t target_frame);

/* Copy the currently decoded Y, U, V planes into caller-owned buffers if frame changed.
 * caller_bufs must hold at least w*h (Y), (w/2)*(h/2) (U), (w/2)*(h/2) (V) bytes.
 * Returns 1 if copied, 0 if no frame decoded yet. */
int cri_video_read_planes(CriVideoStream *vs, uint8_t *y_out, uint8_t *u_out, uint8_t *v_out,
                          uint32_t *out_w, uint32_t *out_h, uint32_t *out_decoded_seq);

#endif
