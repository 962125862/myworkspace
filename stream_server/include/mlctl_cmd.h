#ifndef MLCTL_CMD_H
#define MLCTL_CMD_H

#include <stdint.h>

/* Copy of ml_worker control protocol header (MLCT).
 * Kept here to allow stream_server tap to request an IDR from ml_worker.
 */

#define ML_CTRL_MAGIC   0x4d4c4354u   /* "MLCT" */
#define ML_CTRL_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
    uint64_t seq;
} MlControlCmd;

#endif /* MLCTL_CMD_H */

