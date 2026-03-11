/* Minimal H.264 tap server: publish AnnexB bytestream over TCP.
 * Copied from stream_server/include/h264_tap.h (kept standalone).
 */

#ifndef AGENT_H264_TAP_H
#define AGENT_H264_TAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int h264_tap_start(const char* bind_ip, uint16_t port);
void h264_tap_stop(void);

void h264_tap_publish(uint16_t stream_id, const uint8_t* data, int size);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_H264_TAP_H */

