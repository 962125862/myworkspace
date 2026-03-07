#include "connection_callbacks.h"

#include <stdio.h>
#include <stdarg.h>

static void connection_terminated(int errorCode) {
    fprintf(stderr, "Connection terminated: %d\n", errorCode);
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
