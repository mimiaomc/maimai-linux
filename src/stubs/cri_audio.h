#ifndef CRI_AUDIO_H
#define CRI_AUDIO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void cri_audio_init(void);
void cri_audio_shutdown(void);
void cri_audio_set_keycode(uint64_t keycode);
int32_t cri_audio_register_acf_file(const char *acf_path);
void    cri_audio_category_set_volume_by_id(uint32_t id, float vol);
void    cri_audio_category_set_volume_by_name(const char *name, float vol);
float   cri_audio_category_get_volume_by_id(uint32_t id);
float   cri_audio_category_get_volume_by_name(const char *name);
void    cri_audio_category_mute_by_id(uint32_t id, int32_t mute);
void    cri_audio_category_mute_by_name(const char *name, int32_t mute);

/* ACB management */
void *cri_audio_load_acb_file(const char *acb_path, const char *awb_path);
void *cri_audio_load_acb_data(const uint8_t *acb_data, int32_t acb_size, const char *awb_path);
void  cri_audio_release_acb(void *acb_hn);
int32_t cri_audio_acb_get_num_cues(void *acb_hn);
int32_t cri_audio_acb_exists_id(void *acb_hn, int32_t id);
int32_t cri_audio_acb_exists_name(void *acb_hn, const char *name);

/* Player & Playback management */
void *cri_audio_player_create(void);
void  cri_audio_player_destroy(void *player);
void  cri_audio_player_reset_params(void *player);
void  cri_audio_player_set_cue_id(void *player, void *acb_hn, int32_t id);
void  cri_audio_player_set_cue_name(void *player, void *acb_hn, const char *name);
void  cri_audio_player_set_cue_index(void *player, void *acb_hn, int32_t index);
void  cri_audio_player_set_start_time(void *player, int64_t start_time_ms);
void  cri_audio_player_set_volume(void *player, float vol);
void  cri_audio_player_set_aisac_by_id(void *player, uint16_t ctrl_id, float val);
void  cri_audio_player_set_fade_in_time(void *player, int32_t ms);
void  cri_audio_player_set_fade_out_time(void *player, int32_t ms);
void  cri_audio_player_pause(void *player, int32_t sw);
int32_t cri_audio_player_is_paused(void *player);
uint32_t cri_audio_player_start(void *player);
uint32_t cri_audio_player_prepare(void *player);
void  cri_audio_player_stop(void *player, int32_t immediate);
void  cri_audio_player_update_all(void *player);
int32_t cri_audio_player_get_status(void *player);
int64_t cri_audio_player_get_time(void *player);

/* Playback handle control */
void    cri_audio_pb_stop(uint32_t pb_id, int32_t immediate);
void    cri_audio_pb_pause(uint32_t pb_id, int32_t sw);
int32_t cri_audio_pb_is_paused(uint32_t pb_id);
int32_t cri_audio_pb_get_status(uint32_t pb_id);
int64_t cri_audio_pb_get_time(uint32_t pb_id);
int32_t cri_audio_pb_get_num_played_samples(uint32_t pb_id, int64_t *num_samples, int32_t *sampling_rate);

/* USM (@SFV) VIDEO_HDRINFO metadata reader */
int32_t cri_audio_parse_usm_video_info(const char *path,
                                       uint32_t *width, uint32_t *height,
                                       uint32_t *disp_width, uint32_t *disp_height,
                                       uint32_t *framerate_n, uint32_t *framerate_d,
                                       uint32_t *total_frames);

#ifdef __cplusplus
}
#endif

#endif
