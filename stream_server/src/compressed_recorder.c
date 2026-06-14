#define _GNU_SOURCE
#include "compressed_recorder.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <mntent.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SEGMENT_SEC 1800u
#define DEFAULT_IDR_INTERVAL_SEC 30u
#define DEFAULT_QUEUE_BYTES (256ull * 1024ull * 1024ull)
#define PARAM_SET_LIMIT (1024u * 1024u)
#define PERIODIC_IDR_MIN_GAP_NS (2ull * 1000ull * 1000ull * 1000ull)
#define NFS_MOUNT_CHECK_INTERVAL_NS (1000ull * 1000ull * 1000ull)
#define NFS_MOUNT_LOG_INTERVAL_NS (10ull * 1000ull * 1000ull * 1000ull)

typedef enum {
    REC_EVENT_STREAM_START = 1,
    REC_EVENT_VIDEO = 2,
    REC_EVENT_STREAM_STOP = 3
} RecorderEventType;

typedef struct RecorderEvent {
    RecorderEventType type;
    uint16_t stream_id;
    uint64_t wall_ms;
    uint64_t mono_ns;
    StreamInfo info;
    uint8_t* data;
    size_t len;
    struct RecorderEvent* next;
} RecorderEvent;

typedef struct {
    uint8_t* data;
    size_t len;
} ParamSet;

typedef struct {
    uint16_t stream_id;
    bool enabled;
    bool active;
    uint32_t codec;
    const char* ext;
    bool waiting_keyframe;
    bool rollover_pending;

    FILE* fp;
    char tmp_path[PATH_MAX];
    char final_path[PATH_MAX];
    uint64_t segment_start_wall_ms;
    uint64_t segment_start_mono_ns;
    uint64_t bytes_written;
    uint64_t packets_written;

    ParamSet params[3]; /* H264: SPS/PPS, HEVC: VPS/SPS/PPS */
    uint64_t last_idr_request_ns;
    uint64_t next_periodic_idr_ns;
    uint32_t idr_request_count;
} RecorderStream;

typedef struct {
    bool configured;
    bool running;
    bool accepting;
    char record_dir[512];
    char raw_dir[512];
    uint16_t max_streams;
    uint32_t segment_sec;
    uint32_t idr_interval_sec;
    size_t queue_limit_bytes;
    bool require_nfs_mount;
    bool nfs_mounted;
    uint64_t last_mount_check_ns;
    uint64_t last_mount_log_ns;

    bool enabled_streams[MAX_STREAMS + 1];
    RecorderStream streams[MAX_STREAMS + 1];

    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    RecorderEvent* head;
    RecorderEvent* tail;
    size_t queued_bytes;
    uint64_t queued_events;
    uint64_t dropped_packets;
    uint64_t dropped_bytes;
    uint64_t mount_skipped_packets;
    uint64_t write_errors;
    uint64_t segments_closed;

    pthread_t thread;
    CompressedRecorderRequestIdrFn request_idr;
    void* request_idr_user;
} CompressedRecorder;

typedef struct {
    bool has_keyframe;
} PayloadAnalysis;

static CompressedRecorder g_recorder;
static const uint8_t kStartCode4[4] = {0, 0, 0, 1};

static uint64_t now_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t now_wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
}

static void recorder_note_write_error(CompressedRecorder* r) {
    if (!r || !r->configured) return;
    pthread_mutex_lock(&r->lock);
    r->write_errors++;
    pthread_mutex_unlock(&r->lock);
}

static void recorder_note_mount_skip(CompressedRecorder* r) {
    if (!r || !r->configured) return;
    pthread_mutex_lock(&r->lock);
    r->mount_skipped_packets++;
    pthread_mutex_unlock(&r->lock);
}

static void recorder_note_segment_closed(CompressedRecorder* r) {
    if (!r || !r->configured) return;
    pthread_mutex_lock(&r->lock);
    r->segments_closed++;
    pthread_mutex_unlock(&r->lock);
}

static int mkdir_p(const char* path) {
    if (!path || !*path) {
        return -1;
    }

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);

    size_t len = strlen(tmp);
    if (len == 0) {
        return -1;
    }
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0775) < 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0775) < 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static bool path_is_under_mountpoint(const char* path, const char* mountpoint) {
    if (!path || !mountpoint || !*path || !*mountpoint) {
        return false;
    }

    size_t path_len = strlen(path);
    size_t mount_len = strlen(mountpoint);
    while (path_len > 1 && path[path_len - 1] == '/') {
        path_len--;
    }
    while (mount_len > 1 && mountpoint[mount_len - 1] == '/') {
        mount_len--;
    }

    if (mount_len == 1 && mountpoint[0] == '/') {
        return path[0] == '/';
    }
    if (path_len < mount_len) {
        return false;
    }
    if (strncmp(path, mountpoint, mount_len) != 0) {
        return false;
    }
    return path_len == mount_len || path[mount_len] == '/';
}

static bool path_has_nfs_mount(const char* path) {
    if (!path || !*path) {
        return false;
    }

    FILE* fp = setmntent("/proc/mounts", "r");
    if (!fp) {
        return false;
    }

    bool found = false;
    struct mntent* ent = NULL;
    while ((ent = getmntent(fp)) != NULL) {
        if (strcmp(ent->mnt_type, "nfs") != 0 && strcmp(ent->mnt_type, "nfs4") != 0) {
            continue;
        }
        if (path_is_under_mountpoint(path, ent->mnt_dir)) {
            found = true;
            break;
        }
    }

    endmntent(fp);
    return found;
}

static bool recorder_storage_available(CompressedRecorder* r, uint64_t now_ns) {
    if (!r || !r->require_nfs_mount) {
        return true;
    }
    if (r->nfs_mounted) {
        return true;
    }

    if (r->last_mount_check_ns == 0 ||
        now_ns - r->last_mount_check_ns >= NFS_MOUNT_CHECK_INTERVAL_NS) {
        r->last_mount_check_ns = now_ns;
        r->nfs_mounted = path_has_nfs_mount(r->record_dir);
        if (r->nfs_mounted) {
            fprintf(stderr, "[Recorder] NFS mount available again: %s\n", r->record_dir);
        }
    }
    return r->nfs_mounted;
}

static void recorder_skip_unmounted(CompressedRecorder* r, RecorderStream* s, uint64_t now_ns) {
    recorder_note_mount_skip(r);
    if (s) {
        s->waiting_keyframe = true;
        s->rollover_pending = false;
    }
    if (!r || !r->require_nfs_mount) {
        return;
    }
    if (r->last_mount_log_ns == 0 ||
        now_ns - r->last_mount_log_ns >= NFS_MOUNT_LOG_INTERVAL_NS) {
        fprintf(stderr, "[Recorder] NFS not mounted, skip recording: %s\n", r->record_dir);
        r->last_mount_log_ns = now_ns;
    }
}

static void recorder_recheck_mount_after_error(CompressedRecorder* r,
                                               const char* op,
                                               uint64_t now_ns) {
    if (!r || !r->require_nfs_mount) {
        return;
    }

    r->last_mount_check_ns = now_ns;
    bool mounted = path_has_nfs_mount(r->record_dir);
    r->nfs_mounted = false;
    if (!mounted) {
        fprintf(stderr, "[Recorder] %s failed and NFS is not mounted, pause recording: %s\n",
                op ? op : "write", r->record_dir);
    } else {
        fprintf(stderr, "[Recorder] %s failed while NFS is mounted, pause recording briefly: %s\n",
                op ? op : "write", r->record_dir);
    }
}

static int codec_supported(uint32_t codec) {
    return codec == 0 || codec == 1;
}

static const char* codec_ext(uint32_t codec) {
    if (codec == 0) return "h264";
    if (codec == 1) return "hevc";
    return "bin";
}

static void free_param_sets(RecorderStream* s) {
    for (size_t i = 0; i < 3; i++) {
        free(s->params[i].data);
        s->params[i].data = NULL;
        s->params[i].len = 0;
    }
}

static void reset_stream_file_state(RecorderStream* s) {
    if (!s) return;
    s->fp = NULL;
    s->tmp_path[0] = '\0';
    s->final_path[0] = '\0';
    s->segment_start_wall_ms = 0;
    s->segment_start_mono_ns = 0;
    s->bytes_written = 0;
    s->packets_written = 0;
    s->waiting_keyframe = true;
    s->rollover_pending = false;
    s->last_idr_request_ns = 0;
    s->next_periodic_idr_ns = 0;
    s->idr_request_count = 0;
}

static bool params_ready(const RecorderStream* s) {
    if (!s) return false;
    if (s->codec == 0) {
        return s->params[0].len > 0 && s->params[1].len > 0;
    }
    if (s->codec == 1) {
        return s->params[0].len > 0 && s->params[1].len > 0 && s->params[2].len > 0;
    }
    return false;
}

static void cache_param_set(RecorderStream* s, int index, const uint8_t* nal, size_t nal_len) {
    if (!s || index < 0 || index >= 3 || !nal || nal_len == 0 || nal_len > PARAM_SET_LIMIT) {
        return;
    }

    size_t stored_len = sizeof(kStartCode4) + nal_len;
    ParamSet* ps = &s->params[index];
    if (ps->len == stored_len &&
        ps->data &&
        memcmp(ps->data, kStartCode4, sizeof(kStartCode4)) == 0 &&
        memcmp(ps->data + sizeof(kStartCode4), nal, nal_len) == 0) {
        return;
    }

    uint8_t* copy = malloc(stored_len);
    if (!copy) {
        return;
    }
    memcpy(copy, kStartCode4, sizeof(kStartCode4));
    memcpy(copy + sizeof(kStartCode4), nal, nal_len);

    free(ps->data);
    ps->data = copy;
    ps->len = stored_len;
}

static size_t start_code_len_at(const uint8_t* data, size_t len, size_t pos) {
    if (pos + 3 <= len && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) {
        return 3;
    }
    if (pos + 4 <= len && data[pos] == 0 && data[pos + 1] == 0 &&
        data[pos + 2] == 0 && data[pos + 3] == 1) {
        return 4;
    }
    return 0;
}

static size_t find_start_code(const uint8_t* data, size_t len, size_t start, size_t* sc_len) {
    for (size_t i = start; i + 3 <= len; i++) {
        size_t n = start_code_len_at(data, len, i);
        if (n > 0) {
            if (sc_len) *sc_len = n;
            return i;
        }
    }
    if (sc_len) *sc_len = 0;
    return SIZE_MAX;
}

static bool payload_has_start_code(const uint8_t* data, size_t len) {
    size_t sc_len = 0;
    return find_start_code(data, len, 0, &sc_len) != SIZE_MAX;
}

static void analyze_nal(RecorderStream* s, const uint8_t* nal, size_t nal_len,
                        PayloadAnalysis* analysis) {
    if (!s || !nal || nal_len == 0 || !analysis) {
        return;
    }

    if (s->codec == 0) {
        uint8_t nal_type = nal[0] & 0x1f;
        if (nal_type == 7) {
            cache_param_set(s, 0, nal, nal_len);
        } else if (nal_type == 8) {
            cache_param_set(s, 1, nal, nal_len);
        } else if (nal_type == 5) {
            analysis->has_keyframe = true;
        }
        return;
    }

    if (s->codec == 1 && nal_len >= 2) {
        uint8_t nal_type = (uint8_t)((nal[0] >> 1) & 0x3f);
        if (nal_type == 32) {
            cache_param_set(s, 0, nal, nal_len);
        } else if (nal_type == 33) {
            cache_param_set(s, 1, nal, nal_len);
        } else if (nal_type == 34) {
            cache_param_set(s, 2, nal, nal_len);
        } else if ((nal_type >= 16 && nal_type <= 18) ||
                   nal_type == 19 || nal_type == 20 || nal_type == 21) {
            analysis->has_keyframe = true;
        }
    }
}

static PayloadAnalysis analyze_payload(RecorderStream* s, const uint8_t* data, size_t len) {
    PayloadAnalysis analysis;
    memset(&analysis, 0, sizeof(analysis));

    if (!s || !data || len == 0) {
        return analysis;
    }

    size_t sc_len = 0;
    size_t pos = find_start_code(data, len, 0, &sc_len);
    if (pos == SIZE_MAX) {
        analyze_nal(s, data, len, &analysis);
        return analysis;
    }

    while (pos != SIZE_MAX) {
        size_t nal_start = pos + sc_len;
        size_t next_sc_len = 0;
        size_t next = find_start_code(data, len, nal_start, &next_sc_len);
        size_t nal_end = (next == SIZE_MAX) ? len : next;

        while (nal_end > nal_start && data[nal_end - 1] == 0) {
            nal_end--;
        }
        if (nal_end > nal_start) {
            analyze_nal(s, data + nal_start, nal_end - nal_start, &analysis);
        }

        pos = next;
        sc_len = next_sc_len;
    }

    return analysis;
}

static int write_all(FILE* fp, const uint8_t* data, size_t len) {
    if (!fp || !data || len == 0) {
        return 0;
    }
    return fwrite(data, 1, len, fp) == len ? 0 : -1;
}

static int write_payload_annexb(FILE* fp, const uint8_t* data, size_t len) {
    if (!fp || !data || len == 0) {
        return 0;
    }
    if (!payload_has_start_code(data, len)) {
        if (write_all(fp, kStartCode4, sizeof(kStartCode4)) < 0) {
            return -1;
        }
    }
    return write_all(fp, data, len);
}

static int write_param_sets(FILE* fp, RecorderStream* s) {
    if (!fp || !s) {
        return -1;
    }
    size_t count = (s->codec == 1) ? 3 : 2;
    for (size_t i = 0; i < count; i++) {
        if (s->params[i].data && s->params[i].len > 0) {
            if (write_all(fp, s->params[i].data, s->params[i].len) < 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int format_segment_paths(CompressedRecorder* r, RecorderStream* s, uint64_t wall_ms) {
    time_t sec = (time_t)(wall_ms / 1000ull);
    struct tm tmv;
    localtime_r(&sec, &tmv);

    char dir[PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/s%02u", r->raw_dir, (unsigned)s->stream_id);
    if (n < 0 || (size_t)n >= sizeof(dir)) {
        fprintf(stderr, "[Recorder] stream=%u segment directory path too long\n",
                (unsigned)s->stream_id);
        return -1;
    }
    if (mkdir_p(dir) != 0) {
        fprintf(stderr, "[Recorder] stream=%u mkdir failed: %s: %s\n",
                (unsigned)s->stream_id, dir, strerror(errno));
        return -1;
    }

    char stamp[64];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
    n = snprintf(s->final_path, sizeof(s->final_path), "%s/%s.%s", dir, stamp, s->ext);
    if (n < 0 || (size_t)n >= sizeof(s->final_path)) {
        fprintf(stderr, "[Recorder] stream=%u segment file path too long\n",
                (unsigned)s->stream_id);
        s->final_path[0] = '\0';
        s->tmp_path[0] = '\0';
        return -1;
    }
    n = snprintf(s->tmp_path, sizeof(s->tmp_path), "%s.tmp", s->final_path);
    if (n < 0 || (size_t)n >= sizeof(s->tmp_path)) {
        fprintf(stderr, "[Recorder] stream=%u segment tmp path too long\n",
                (unsigned)s->stream_id);
        s->final_path[0] = '\0';
        s->tmp_path[0] = '\0';
        return -1;
    }
    return 0;
}

static int close_segment(CompressedRecorder* r, RecorderStream* s, bool finalize) {
    if (!s || !s->fp) {
        return 0;
    }

    int rc = 0;
    if (fclose(s->fp) != 0) {
        rc = -1;
    }
    s->fp = NULL;

    if (finalize && s->tmp_path[0] && s->final_path[0]) {
        if (rename(s->tmp_path, s->final_path) != 0) {
            fprintf(stderr, "[Recorder] stream=%u rename failed: %s -> %s: %s\n",
                    (unsigned)s->stream_id, s->tmp_path, s->final_path, strerror(errno));
            recorder_note_write_error(r);
            recorder_recheck_mount_after_error(r, "rename", now_monotonic_ns());
            rc = -1;
        } else {
            recorder_note_segment_closed(r);
            fprintf(stderr,
                    "[Recorder] stream=%u segment closed path=%s bytes=%llu packets=%llu\n",
                    (unsigned)s->stream_id,
                    s->final_path,
                    (unsigned long long)s->bytes_written,
                    (unsigned long long)s->packets_written);
        }
    } else if (!finalize && s->tmp_path[0]) {
        unlink(s->tmp_path);
    }

    s->tmp_path[0] = '\0';
    s->final_path[0] = '\0';
    s->segment_start_wall_ms = 0;
    s->segment_start_mono_ns = 0;
    s->next_periodic_idr_ns = 0;
    s->bytes_written = 0;
    s->packets_written = 0;
    return rc;
}

static void recorder_request_idr(CompressedRecorder* r, RecorderStream* s,
                                 const char* reason, uint64_t now_ns, bool force) {
    if (!r || !s || !r->request_idr) {
        return;
    }

    uint64_t backoff_ns = 0;
    if (!force) {
        if (s->idr_request_count == 0) {
            backoff_ns = 0;
        } else if (s->idr_request_count == 1) {
            backoff_ns = 2ull * 1000ull * 1000ull * 1000ull;
        } else if (s->idr_request_count == 2) {
            backoff_ns = 5ull * 1000ull * 1000ull * 1000ull;
        } else {
            backoff_ns = 10ull * 1000ull * 1000ull * 1000ull;
        }
    }

    if (!force && s->last_idr_request_ns != 0 &&
        now_ns - s->last_idr_request_ns < backoff_ns) {
        return;
    }

    r->request_idr(s->stream_id, reason, r->request_idr_user);
    s->last_idr_request_ns = now_ns;
    s->idr_request_count++;
}

static bool recorder_idr_request_recent(const RecorderStream* s,
                                        uint64_t now_ns,
                                        uint64_t gap_ns) {
    return s && s->last_idr_request_ns != 0 &&
           now_ns >= s->last_idr_request_ns &&
           now_ns - s->last_idr_request_ns < gap_ns;
}

static uint64_t recorder_idr_interval_ns(const CompressedRecorder* r) {
    if (!r || r->idr_interval_sec == 0) {
        return 0;
    }
    return (uint64_t)r->idr_interval_sec * 1000ull * 1000ull * 1000ull;
}

static uint64_t recorder_periodic_idr_offset_ns(const CompressedRecorder* r,
                                                const RecorderStream* s) {
    uint64_t interval_ns = recorder_idr_interval_ns(r);
    if (!r || !s || interval_ns == 0) {
        return 0;
    }

    uint16_t spread = r->max_streams > 0 ? r->max_streams : 1;
    uint16_t index = s->stream_id > 0 ? (uint16_t)(s->stream_id - 1) : 0;
    return ((uint64_t)index * interval_ns) / spread;
}

static uint64_t recorder_next_periodic_idr_ns(CompressedRecorder* r,
                                              RecorderStream* s,
                                              uint64_t now_ns) {
    uint64_t interval_ns = recorder_idr_interval_ns(r);
    if (!r || !s || interval_ns == 0) {
        return 0;
    }

    uint64_t offset_ns = recorder_periodic_idr_offset_ns(r, s);
    uint64_t base_ns = (now_ns / interval_ns) * interval_ns;
    uint64_t next_ns = base_ns + offset_ns;
    uint64_t min_next_ns = now_ns + PERIODIC_IDR_MIN_GAP_NS;

    while (next_ns < min_next_ns) {
        next_ns += interval_ns;
    }
    return next_ns;
}

static void recorder_maybe_request_periodic_idr(CompressedRecorder* r,
                                                RecorderStream* s,
                                                uint64_t now_ns) {
    uint64_t interval_ns = recorder_idr_interval_ns(r);
    if (!r || !s || interval_ns == 0 || !s->fp ||
        s->waiting_keyframe || s->rollover_pending) {
        return;
    }

    if (s->next_periodic_idr_ns == 0) {
        s->next_periodic_idr_ns = recorder_next_periodic_idr_ns(r, s, now_ns);
    }
    if (s->next_periodic_idr_ns == 0 || now_ns < s->next_periodic_idr_ns) {
        return;
    }

    if (recorder_idr_request_recent(s, now_ns, PERIODIC_IDR_MIN_GAP_NS)) {
        s->next_periodic_idr_ns = recorder_next_periodic_idr_ns(r, s, now_ns);
        return;
    }

    recorder_request_idr(r, s, "record_periodic", now_ns, true);

    do {
        s->next_periodic_idr_ns += interval_ns;
    } while (s->next_periodic_idr_ns <= now_ns);
}

static int open_segment(CompressedRecorder* r, RecorderStream* s,
                        uint64_t wall_ms, uint64_t mono_ns) {
    if (format_segment_paths(r, s, wall_ms) != 0) {
        recorder_note_write_error(r);
        recorder_recheck_mount_after_error(r, "mkdir", mono_ns);
        return -1;
    }
    s->fp = fopen(s->tmp_path, "wb");
    if (!s->fp) {
        fprintf(stderr, "[Recorder] stream=%u open failed: %s: %s\n",
                (unsigned)s->stream_id, s->tmp_path, strerror(errno));
        recorder_note_write_error(r);
        recorder_recheck_mount_after_error(r, "open", mono_ns);
        return -1;
    }

    s->segment_start_wall_ms = wall_ms;
    s->segment_start_mono_ns = mono_ns;
    s->bytes_written = 0;
    s->packets_written = 0;
    s->waiting_keyframe = false;
    s->rollover_pending = false;
    s->next_periodic_idr_ns = recorder_next_periodic_idr_ns(r, s, mono_ns);
    s->idr_request_count = 0;
    fprintf(stderr, "[Recorder] stream=%u segment opened path=%s\n",
            (unsigned)s->stream_id, s->tmp_path);
    return 0;
}

static int write_segment_packet(CompressedRecorder* r, RecorderStream* s,
                                const uint8_t* data, size_t len) {
    if (!s->fp) {
        return -1;
    }

    if (write_payload_annexb(s->fp, data, len) < 0) {
        fprintf(stderr, "[Recorder] stream=%u write failed: %s\n",
                (unsigned)s->stream_id, strerror(errno));
        recorder_note_write_error(r);
        recorder_recheck_mount_after_error(r, "write", now_monotonic_ns());
        return -1;
    }

    s->bytes_written += len;
    if (!payload_has_start_code(data, len)) {
        s->bytes_written += sizeof(kStartCode4);
    }
    s->packets_written++;
    return 0;
}

static void process_stream_start(CompressedRecorder* r, const RecorderEvent* ev) {
    if (ev->stream_id == 0 || ev->stream_id > r->max_streams) {
        return;
    }

    RecorderStream* s = &r->streams[ev->stream_id];
    close_segment(r, s, true);
    free_param_sets(s);
    memset(s, 0, sizeof(*s));
    s->stream_id = ev->stream_id;
    s->enabled = r->enabled_streams[ev->stream_id];
    s->codec = ev->info.codec;
    s->ext = codec_ext(s->codec);
    reset_stream_file_state(s);

    if (!s->enabled) {
        return;
    }
    if (!codec_supported(s->codec)) {
        fprintf(stderr, "[Recorder] stream=%u unsupported codec=%u, skip recording\n",
                (unsigned)ev->stream_id, ev->info.codec);
        s->enabled = false;
        return;
    }

    s->active = true;
    fprintf(stderr,
            "[Recorder] stream=%u recording armed codec=%s segment=%us\n",
            (unsigned)ev->stream_id, s->ext, r->segment_sec);
    recorder_request_idr(r, s, "record_start", ev->mono_ns, true);
}

static void process_stream_stop(CompressedRecorder* r, const RecorderEvent* ev) {
    if (ev->stream_id == 0 || ev->stream_id > r->max_streams) {
        return;
    }
    RecorderStream* s = &r->streams[ev->stream_id];
    close_segment(r, s, true);
    free_param_sets(s);
    memset(s, 0, sizeof(*s));
    s->stream_id = ev->stream_id;
}

static bool should_rollover(const CompressedRecorder* r, const RecorderStream* s,
                            uint64_t wall_ms) {
    if (!r || !s || !s->fp || r->segment_sec == 0 || s->segment_start_wall_ms == 0) {
        return false;
    }
    uint64_t elapsed_ms = wall_ms - s->segment_start_wall_ms;
    return elapsed_ms >= (uint64_t)r->segment_sec * 1000ull;
}

static void process_video(CompressedRecorder* r, const RecorderEvent* ev) {
    if (ev->stream_id == 0 || ev->stream_id > r->max_streams || !ev->data || ev->len == 0) {
        return;
    }

    RecorderStream* s = &r->streams[ev->stream_id];
    if (!s->active || !s->enabled || !codec_supported(s->codec)) {
        return;
    }

    PayloadAnalysis analysis = analyze_payload(s, ev->data, ev->len);
    bool ready = params_ready(s);

    if (!recorder_storage_available(r, ev->mono_ns)) {
        recorder_skip_unmounted(r, s, ev->mono_ns);
        return;
    }

    if (s->waiting_keyframe) {
        if (!analysis.has_keyframe || !ready) {
            recorder_request_idr(r, s,
                                 ready ? "record_wait_keyframe" : "record_wait_params",
                                 ev->mono_ns, false);
            return;
        }

        if (open_segment(r, s, ev->wall_ms, ev->mono_ns) != 0) {
            return;
        }
        if (write_param_sets(s->fp, s) < 0 ||
            write_segment_packet(r, s, ev->data, ev->len) < 0) {
            close_segment(r, s, false);
            s->waiting_keyframe = true;
        }
        return;
    }

    if (!s->rollover_pending && should_rollover(r, s, ev->wall_ms)) {
        s->rollover_pending = true;
        if ((!analysis.has_keyframe || !ready) &&
            !recorder_idr_request_recent(s, ev->mono_ns, PERIODIC_IDR_MIN_GAP_NS)) {
            recorder_request_idr(r, s, "record_rollover", ev->mono_ns, true);
        }
    }

    if (s->rollover_pending) {
        if (analysis.has_keyframe && ready) {
            close_segment(r, s, true);
            if (open_segment(r, s, ev->wall_ms, ev->mono_ns) != 0) {
                s->waiting_keyframe = true;
                return;
            }
            if (write_param_sets(s->fp, s) < 0 ||
                write_segment_packet(r, s, ev->data, ev->len) < 0) {
                close_segment(r, s, false);
                s->waiting_keyframe = true;
            }
            return;
        }

        recorder_request_idr(r, s,
                             ready ? "record_wait_rollover_keyframe" :
                                     "record_wait_rollover_params",
                             ev->mono_ns, false);
    }

    recorder_maybe_request_periodic_idr(r, s, ev->mono_ns);

    if (s->fp) {
        if (write_segment_packet(r, s, ev->data, ev->len) < 0) {
            close_segment(r, s, false);
            s->waiting_keyframe = true;
            recorder_request_idr(r, s, "record_write_error", ev->mono_ns, true);
        }
    }
}

static void free_event(RecorderEvent* ev) {
    if (!ev) return;
    free(ev->data);
    free(ev);
}

static void* recorder_thread_main(void* arg) {
    CompressedRecorder* r = (CompressedRecorder*)arg;

    while (1) {
        pthread_mutex_lock(&r->lock);
        while (!r->head && r->running) {
            pthread_cond_wait(&r->not_empty, &r->lock);
        }
        if (!r->head && !r->running) {
            pthread_mutex_unlock(&r->lock);
            break;
        }

        RecorderEvent* ev = r->head;
        r->head = ev->next;
        if (!r->head) {
            r->tail = NULL;
        }
        r->queued_events--;
        if (ev->type == REC_EVENT_VIDEO) {
            r->queued_bytes -= ev->len;
        }
        pthread_mutex_unlock(&r->lock);

        switch (ev->type) {
            case REC_EVENT_STREAM_START:
                process_stream_start(r, ev);
                break;
            case REC_EVENT_VIDEO:
                process_video(r, ev);
                break;
            case REC_EVENT_STREAM_STOP:
                process_stream_stop(r, ev);
                break;
        }

        free_event(ev);
    }

    for (uint16_t i = 1; i <= r->max_streams; i++) {
        close_segment(r, &r->streams[i], true);
        free_param_sets(&r->streams[i]);
    }

    return NULL;
}

void compressed_recorder_config_defaults(CompressedRecorderConfig* cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_streams = DEFAULT_MAX_STREAMS;
    cfg->segment_sec = DEFAULT_SEGMENT_SEC;
    cfg->idr_interval_sec = DEFAULT_IDR_INTERVAL_SEC;
    cfg->queue_bytes = DEFAULT_QUEUE_BYTES;
}

static void enable_all_streams(CompressedRecorder* r) {
    for (uint16_t i = 1; i <= r->max_streams; i++) {
        r->enabled_streams[i] = true;
    }
}

static void parse_streams_spec(CompressedRecorder* r, const char* spec) {
    memset(r->enabled_streams, 0, sizeof(r->enabled_streams));
    if (!spec || !*spec || strcmp(spec, "all") == 0 || strcmp(spec, "*") == 0) {
        enable_all_streams(r);
        return;
    }

    char copy[256];
    snprintf(copy, sizeof(copy), "%s", spec);
    char* saveptr = NULL;
    for (char* item = strtok_r(copy, ",", &saveptr);
         item != NULL;
         item = strtok_r(NULL, ",", &saveptr)) {
        while (*item && isspace((unsigned char)*item)) item++;
        if (!*item) continue;

        unsigned a = 0;
        unsigned b = 0;
        if (sscanf(item, "%u-%u", &a, &b) == 2) {
            if (a > b) {
                unsigned tmp = a;
                a = b;
                b = tmp;
            }
            if (b > r->max_streams) b = r->max_streams;
            for (unsigned sid = a; sid <= b; sid++) {
                if (sid > 0) r->enabled_streams[sid] = true;
            }
        } else if (sscanf(item, "%u", &a) == 1) {
            if (a > 0 && a <= r->max_streams) {
                r->enabled_streams[a] = true;
            }
        }
    }
}

int compressed_recorder_start(const CompressedRecorderConfig* cfg,
                              CompressedRecorderRequestIdrFn request_idr,
                              void* request_idr_user) {
    if (!cfg || !cfg->record_dir[0]) {
        return 0;
    }
    if (g_recorder.running) {
        return 0;
    }

    memset(&g_recorder, 0, sizeof(g_recorder));
    int n = snprintf(g_recorder.record_dir, sizeof(g_recorder.record_dir), "%s", cfg->record_dir);
    if (n < 0 || (size_t)n >= sizeof(g_recorder.record_dir)) {
        fprintf(stderr, "[Recorder] record dir too long\n");
        memset(&g_recorder, 0, sizeof(g_recorder));
        return -1;
    }
    n = snprintf(g_recorder.raw_dir, sizeof(g_recorder.raw_dir), "%s/raw", cfg->record_dir);
    if (n < 0 || (size_t)n >= sizeof(g_recorder.raw_dir)) {
        fprintf(stderr, "[Recorder] raw dir too long\n");
        memset(&g_recorder, 0, sizeof(g_recorder));
        return -1;
    }
    g_recorder.max_streams = cfg->max_streams;
    if (g_recorder.max_streams < 1 || g_recorder.max_streams > MAX_STREAMS) {
        g_recorder.max_streams = DEFAULT_MAX_STREAMS;
    }
    g_recorder.segment_sec = cfg->segment_sec ? cfg->segment_sec : DEFAULT_SEGMENT_SEC;
    g_recorder.idr_interval_sec = cfg->idr_interval_sec;
    g_recorder.queue_limit_bytes = cfg->queue_bytes ? cfg->queue_bytes : DEFAULT_QUEUE_BYTES;
    g_recorder.require_nfs_mount = cfg->require_nfs_mount;
    g_recorder.nfs_mounted = !g_recorder.require_nfs_mount ||
                             path_has_nfs_mount(g_recorder.record_dir);
    g_recorder.last_mount_check_ns = now_monotonic_ns();
    g_recorder.request_idr = request_idr;
    g_recorder.request_idr_user = request_idr_user;

    parse_streams_spec(&g_recorder, cfg->streams_spec);

    if (g_recorder.nfs_mounted && mkdir_p(g_recorder.raw_dir) != 0) {
        fprintf(stderr, "[Recorder] failed to create record dir: %s: %s\n",
                g_recorder.raw_dir, strerror(errno));
        memset(&g_recorder, 0, sizeof(g_recorder));
        return -1;
    } else if (g_recorder.require_nfs_mount && !g_recorder.nfs_mounted) {
        fprintf(stderr, "[Recorder] NFS not mounted at startup, recording will be skipped: %s\n",
                g_recorder.record_dir);
    }

    pthread_mutex_init(&g_recorder.lock, NULL);
    pthread_cond_init(&g_recorder.not_empty, NULL);
    g_recorder.running = true;
    g_recorder.accepting = true;
    g_recorder.configured = true;

    if (pthread_create(&g_recorder.thread, NULL, recorder_thread_main, &g_recorder) != 0) {
        fprintf(stderr, "[Recorder] failed to start writer thread\n");
        pthread_cond_destroy(&g_recorder.not_empty);
        pthread_mutex_destroy(&g_recorder.lock);
        memset(&g_recorder, 0, sizeof(g_recorder));
        return -1;
    }

    fprintf(stderr,
            "[Recorder] enabled dir=%s streams=%s segment=%us periodic_idr=%us queue=%.2fMB require_nfs=%d mounted=%d\n",
            g_recorder.record_dir,
            cfg->streams_spec[0] ? cfg->streams_spec : "all",
            g_recorder.segment_sec,
            g_recorder.idr_interval_sec,
            g_recorder.queue_limit_bytes / (1024.0 * 1024.0),
            g_recorder.require_nfs_mount ? 1 : 0,
            g_recorder.nfs_mounted ? 1 : 0);
    return 0;
}

void compressed_recorder_set_request_idr_callback(CompressedRecorderRequestIdrFn request_idr,
                                                  void* request_idr_user) {
    if (!g_recorder.configured) {
        return;
    }

    pthread_mutex_lock(&g_recorder.lock);
    g_recorder.request_idr = request_idr;
    g_recorder.request_idr_user = request_idr_user;
    pthread_mutex_unlock(&g_recorder.lock);
}

void compressed_recorder_stop(void) {
    if (!g_recorder.configured) {
        return;
    }

    pthread_mutex_lock(&g_recorder.lock);
    g_recorder.accepting = false;
    g_recorder.running = false;
    pthread_cond_signal(&g_recorder.not_empty);
    pthread_mutex_unlock(&g_recorder.lock);

    pthread_join(g_recorder.thread, NULL);

    RecorderEvent* ev = g_recorder.head;
    while (ev) {
        RecorderEvent* next = ev->next;
        free_event(ev);
        ev = next;
    }

    pthread_cond_destroy(&g_recorder.not_empty);
    pthread_mutex_destroy(&g_recorder.lock);

    fprintf(stderr,
            "[Recorder] stopped dropped=%llu dropped_bytes=%.2fMB write_errors=%llu segments=%llu\n",
            (unsigned long long)g_recorder.dropped_packets,
            g_recorder.dropped_bytes / (1024.0 * 1024.0),
            (unsigned long long)g_recorder.write_errors,
            (unsigned long long)g_recorder.segments_closed);

    memset(&g_recorder, 0, sizeof(g_recorder));
}

int compressed_recorder_is_enabled(void) {
    return g_recorder.configured && g_recorder.accepting;
}

static void enqueue_event(RecorderEvent* ev) {
    if (!ev) return;
    if (!g_recorder.configured || !g_recorder.accepting) {
        free_event(ev);
        return;
    }

    pthread_mutex_lock(&g_recorder.lock);
    if (!g_recorder.accepting) {
        pthread_mutex_unlock(&g_recorder.lock);
        free_event(ev);
        return;
    }

    if (ev->type == REC_EVENT_VIDEO &&
        g_recorder.queued_bytes + ev->len > g_recorder.queue_limit_bytes) {
        g_recorder.dropped_packets++;
        g_recorder.dropped_bytes += ev->len;
        pthread_mutex_unlock(&g_recorder.lock);
        free_event(ev);
        return;
    }

    ev->next = NULL;
    if (g_recorder.tail) {
        g_recorder.tail->next = ev;
    } else {
        g_recorder.head = ev;
    }
    g_recorder.tail = ev;
    g_recorder.queued_events++;
    if (ev->type == REC_EVENT_VIDEO) {
        g_recorder.queued_bytes += ev->len;
    }
    pthread_cond_signal(&g_recorder.not_empty);
    pthread_mutex_unlock(&g_recorder.lock);
}

void compressed_recorder_on_stream_start(uint16_t stream_id, const StreamInfo* info) {
    if (!info || !g_recorder.configured || !g_recorder.accepting) {
        return;
    }
    RecorderEvent* ev = calloc(1, sizeof(*ev));
    if (!ev) return;
    ev->type = REC_EVENT_STREAM_START;
    ev->stream_id = stream_id;
    ev->wall_ms = now_wall_ms();
    ev->mono_ns = now_monotonic_ns();
    ev->info = *info;
    enqueue_event(ev);
}

void compressed_recorder_on_video(uint16_t stream_id, const uint8_t* data, size_t len) {
    if (!data || len == 0 || !g_recorder.configured || !g_recorder.accepting) {
        return;
    }

    RecorderEvent* ev = calloc(1, sizeof(*ev));
    if (!ev) return;
    ev->type = REC_EVENT_VIDEO;
    ev->stream_id = stream_id;
    ev->wall_ms = now_wall_ms();
    ev->mono_ns = now_monotonic_ns();
    ev->len = len;
    ev->data = malloc(len);
    if (!ev->data) {
        free(ev);
        return;
    }
    memcpy(ev->data, data, len);
    enqueue_event(ev);
}

void compressed_recorder_on_stream_stop(uint16_t stream_id) {
    if (!g_recorder.configured || !g_recorder.accepting) {
        return;
    }
    RecorderEvent* ev = calloc(1, sizeof(*ev));
    if (!ev) return;
    ev->type = REC_EVENT_STREAM_STOP;
    ev->stream_id = stream_id;
    ev->wall_ms = now_wall_ms();
    ev->mono_ns = now_monotonic_ns();
    enqueue_event(ev);
}

void compressed_recorder_print_stats(void) {
    if (!g_recorder.configured) {
        return;
    }

    pthread_mutex_lock(&g_recorder.lock);
    size_t queued_bytes = g_recorder.queued_bytes;
    uint64_t queued_events = g_recorder.queued_events;
    uint64_t dropped_packets = g_recorder.dropped_packets;
    uint64_t dropped_bytes = g_recorder.dropped_bytes;
    uint64_t mount_skipped_packets = g_recorder.mount_skipped_packets;
    uint64_t write_errors = g_recorder.write_errors;
    uint64_t segments_closed = g_recorder.segments_closed;
    pthread_mutex_unlock(&g_recorder.lock);

    printf("[Recorder] queue_events=%llu queue=%.2fMB dropped=%llu dropped_bytes=%.2fMB mount_skipped=%llu write_errors=%llu segments=%llu\n",
           (unsigned long long)queued_events,
           queued_bytes / (1024.0 * 1024.0),
           (unsigned long long)dropped_packets,
           dropped_bytes / (1024.0 * 1024.0),
           (unsigned long long)mount_skipped_packets,
           (unsigned long long)write_errors,
           (unsigned long long)segments_closed);
}
