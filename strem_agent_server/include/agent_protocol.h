#ifndef AGENT_PROTOCOL_H
#define AGENT_PROTOCOL_H

#include <stdint.h>

/* Simple big-endian u32 length prefix for TCP control channel */

static inline uint32_t be32_read(const uint8_t b[4]) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static inline void be32_write(uint8_t b[4], uint32_t v) {
    b[0] = (uint8_t)((v >> 24) & 0xFF);
    b[1] = (uint8_t)((v >> 16) & 0xFF);
    b[2] = (uint8_t)((v >> 8) & 0xFF);
    b[3] = (uint8_t)(v & 0xFF);
}

#endif /* AGENT_PROTOCOL_H */

