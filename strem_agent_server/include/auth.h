#ifndef AGENT_AUTH_H
#define AGENT_AUTH_H

/* Line-based AUTH helper.
 * If token is empty, auth is disabled and always succeeds.
 */

int agent_auth_read_and_check(int fd, const char* token, int timeout_ms);

#endif

