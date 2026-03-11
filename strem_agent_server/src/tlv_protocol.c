#include "tlv_protocol.h"

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t be16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

int tlv_parse_header(const uint8_t* buf, PacketHeader* header) {
    if (!buf || !header) return -1;
    header->length = be32(buf);
    header->type = buf[4];
    header->stream_id = be16(buf + 5);
    if (header->length < TCP_HEADER_SIZE) return -1;
    if (header->length > TCP_MAX_PACKET_SIZE) return -1;
    if (header->stream_id < 1 || header->stream_id > MAX_STREAMS) return -1;
    return 0;
}

