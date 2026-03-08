#ifndef CONNECTION_CALLBACKS_H
#define CONNECTION_CALLBACKS_H

#include <Limelight.h>

extern CONNECTION_LISTENER_CALLBACKS connection_callbacks;

void connection_callbacks_set_fatal_code(volatile int* fatal_code_ptr);

#endif
