#ifndef WORKER_DEFS_H
#define WORKER_DEFS_H

#define WORKER_FATAL_NONE                    0
#define WORKER_FATAL_CONNECTION_TERMINATED   1
#define WORKER_FATAL_SHM_OPEN                2  /* 已废弃，保留兼容性 */
#define WORKER_FATAL_SHM_WRITE               3  /* 已废弃，保留兼容性 */
#define WORKER_FATAL_FRAME_SIZE_CHANGED      4  /* 已废弃，保留兼容性 */
#define WORKER_FATAL_TCP_CONNECT             5
#define WORKER_FATAL_TCP_SEND                6
#define WORKER_FATAL_TCP_DISCONNECTED        7

#endif
