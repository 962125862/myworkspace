/**
 * @file protocol.c
 * @brief TCP视频流协议实现
 */

#include "protocol.h"
#include <string.h>

/* 大端序转换 */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    #define BE32(x) __builtin_bswap32(x)
    #define BE16(x) __builtin_bswap16(x)
#else
    #define BE32(x) (x)
    #define BE16(x) (x)
#endif

int protocol_parse_header(const uint8_t* buf, PacketHeader* header) {
    if (!buf || !header) {
        return -1;
    }
    
    /* 解析大端序字段 */
    header->length = BE32(*(uint32_t*)buf);
    header->type = buf[4];
    header->stream_id = BE16(*(uint16_t*)(buf + 5));
    
    /* 验证 */
    if (header->length < TCP_HEADER_SIZE || header->length > TCP_MAX_PACKET_SIZE) {
        return -1;
    }
    
    if (header->type < TCP_MSG_TYPE_VIDEO_DATA || header->type > TCP_MSG_TYPE_STREAM_STOP) {
        return -1;
    }
    
    if (header->stream_id == 0 || header->stream_id > MAX_STREAMS) {
        return -1;
    }
    
    return 0;
}

int protocol_parse_stream_info(const uint8_t* buf, StreamInfo* info) {
    if (!buf || !info) {
        return -1;
    }
    
    /* 解析大端序字段 */
    info->width = BE32(*(uint32_t*)buf);
    info->height = BE32(*(uint32_t*)(buf + 4));
    info->fps = BE32(*(uint32_t*)(buf + 8));
    info->bitrate = BE32(*(uint32_t*)(buf + 12));
    
    return 0;
}

void protocol_build_header(uint8_t* buf, uint8_t type, uint16_t stream_id, uint32_t payload_len) {
    if (!buf) {
        return;
    }
    
    uint32_t total_len = TCP_HEADER_SIZE + payload_len;
    
    /* 写入大端序 */
    *(uint32_t*)buf = BE32(total_len);
    buf[4] = type;
    *(uint16_t*)(buf + 5) = BE16(stream_id);
}
