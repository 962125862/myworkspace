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

    /* 逐字节读取避免未对齐访问 (ARM 等架构 buf+5 不保证 2 字节对齐) */
    header->length    = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
                      | ((uint32_t)buf[2] <<  8) |  (uint32_t)buf[3];
    header->type      = buf[4];
    header->stream_id = ((uint16_t)buf[5] << 8) | (uint16_t)buf[6];
    
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

    /* 逐字节读取，避免未对齐访问 */
    info->width   = ((uint32_t)buf[0]  << 24) | ((uint32_t)buf[1]  << 16)
                  | ((uint32_t)buf[2]  <<  8) |  (uint32_t)buf[3];
    info->height  = ((uint32_t)buf[4]  << 24) | ((uint32_t)buf[5]  << 16)
                  | ((uint32_t)buf[6]  <<  8) |  (uint32_t)buf[7];
    info->fps     = ((uint32_t)buf[8]  << 24) | ((uint32_t)buf[9]  << 16)
                  | ((uint32_t)buf[10] <<  8) |  (uint32_t)buf[11];
    info->bitrate = ((uint32_t)buf[12] << 24) | ((uint32_t)buf[13] << 16)
                  | ((uint32_t)buf[14] <<  8) |  (uint32_t)buf[15];
    
    return 0;
}

void protocol_build_header(uint8_t* buf, uint8_t type, uint16_t stream_id, uint32_t payload_len) {
    if (!buf) {
        return;
    }

    uint32_t total_len = TCP_HEADER_SIZE + payload_len;

    /* 逐字节写入，避免未对齐访问 */
    buf[0] = (uint8_t)(total_len >> 24);
    buf[1] = (uint8_t)(total_len >> 16);
    buf[2] = (uint8_t)(total_len >>  8);
    buf[3] = (uint8_t)(total_len);
    buf[4] = type;
    buf[5] = (uint8_t)(stream_id >> 8);
    buf[6] = (uint8_t)(stream_id);
}
