#include "connection_callbacks.h"
#include "worker_defs.h"

#include <stdio.h>
#include <stdarg.h>

static volatile int* g_fatal_code = NULL;

void connection_callbacks_set_fatal_code(volatile int* fatal_code_ptr) {
    g_fatal_code = fatal_code_ptr;
}

static void set_fatal_once(int code) {
    if (g_fatal_code && *g_fatal_code == WORKER_FATAL_NONE) {
        *g_fatal_code = code;
    }
}

static void connection_terminated(int errorCode) {
    fprintf(stderr, "Connection terminated: %d\n", errorCode);
    set_fatal_once(WORKER_FATAL_CONNECTION_TERMINATED);
}

static void connection_log_message(const char* format, ...) {
    va_list arglist;
    va_start(arglist, format);
    vfprintf(stderr, format, arglist);
    va_end(arglist);
}

static void connection_status_update(int status) {
    switch (status) {
        case CONN_STATUS_OKAY:
            fprintf(stderr, "Connection is okay\n");
            break;
        case CONN_STATUS_POOR:
            fprintf(stderr, "Connection is poor\n");
            break;
        default:
            fprintf(stderr, "Connection status: %d\n", status);
            break;
    }
}

CONNECTION_LISTENER_CALLBACKS connection_callbacks = {
    .stageStarting = NULL,
    .stageComplete = NULL,
    .stageFailed = NULL,
    .connectionStarted = NULL,
    .connectionTerminated = connection_terminated,
    .logMessage = connection_log_message,
    .rumble = NULL,
    .connectionStatusUpdate = connection_status_update,
    .setHdrMode = NULL,
    .rumbleTriggers = NULL,
    .setMotionEventState = NULL,
    .setControllerLED = NULL,
    .setAdaptiveTriggers = NULL,
};
