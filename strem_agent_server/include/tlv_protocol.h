/* TLV protocol compatible with stream_server/include/protocol.h
 * Duplicated here to keep strem_agent_server standalone.
 */

#ifndef TLV_PROTOCOL_H
#define TLV_PROTOCOL_H

#include <stdint.h>

#define TCP_MSG_TYPE_VIDEO_DATA     0x01
#define TCP_MSG_TYPE_HEARTBEAT      0x02
#define TCP_MSG_TYPE_STREAM_START   0x03
#define TCP_MSG_TYPE_STREAM_STOP    0x04

#define TCP_HEADER_SIZE 7
#define TCP_MAX_PACKET_SIZE (10 * 1024 * 1024)
#define MAX_STREAMS 20

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t bitrate;
} StreamInfo;

typedef struct {
    uint32_t length;
    uint8_t  type;
    uint16_t stream_id;
} PacketHeader;

int tlv_parse_header(const uint8_t* buf, PacketHeader* header);

#endif

