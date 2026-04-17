#ifndef WLRDP_AUTH_H
#define WLRDP_AUTH_H

#include <stdbool.h>
#include <sys/types.h>

struct wlrdp_auth_result {
    uid_t uid;
    gid_t gid;
    char home[256];
    char shell[256];
};

/*
 * Authenticate a user via PAM. On success, fills result with
 * the user's uid, gid, home directory, and shell.
 * Returns true on success, false on auth failure.
 */
bool auth_check_credentials(const char *username, const char *password,
                            struct wlrdp_auth_result *result);

#endif /* WLRDP_AUTH_H */
