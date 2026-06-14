#ifndef COMPRESSED_RECORDER_H
#define COMPRESSED_RECORDER_H

#include "protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*CompressedRecorderRequestIdrFn)(uint16_t stream_id,
                                               const char* reason,
                                               void* user);

typedef struct {
    char record_dir[512];
    char streams_spec[256];
    uint16_t max_streams;
    uint32_t segment_sec;
    uint32_t idr_interval_sec;
    size_t queue_bytes;
    bool require_nfs_mount;
} CompressedRecorderConfig;

void compressed_recorder_config_defaults(CompressedRecorderConfig* cfg);
int compressed_recorder_start(const CompressedRecorderConfig* cfg,
                              CompressedRecorderRequestIdrFn request_idr,
                              void* request_idr_user);
void compressed_recorder_set_request_idr_callback(CompressedRecorderRequestIdrFn request_idr,
                                                  void* request_idr_user);
void compressed_recorder_stop(void);
int compressed_recorder_is_enabled(void);

void compressed_recorder_on_stream_start(uint16_t stream_id, const StreamInfo* info);
void compressed_recorder_on_video(uint16_t stream_id, const uint8_t* data, size_t len);
void compressed_recorder_on_stream_stop(uint16_t stream_id);
void compressed_recorder_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* COMPRESSED_RECORDER_H */
