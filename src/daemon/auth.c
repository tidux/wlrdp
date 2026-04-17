#include "auth.h"
#include "common.h"

#include <security/pam_appl.h>
#include <pwd.h>

/* PAM conversation callback — supplies the password */
struct pam_conv_data {
    const char *password;
};

static int pam_conversation(int num_msg, const struct pam_message **msg,
                            struct pam_response **resp, void *appdata)
{
    struct pam_conv_data *data = appdata;
    struct pam_response *replies = calloc(num_msg, sizeof(*replies));
    if (!replies) return PAM_BUF_ERR;

    for (int i = 0; i < num_msg; i++) {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
            msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            replies[i].resp = strdup(data->password);
            if (!replies[i].resp) {
                free(replies);
                return PAM_BUF_ERR;
            }
        }
    }

    *resp = replies;
    return PAM_SUCCESS;
}

bool auth_check_credentials(const char *username, const char *password,
                            struct wlrdp_auth_result *result)
{
    struct pam_conv_data conv_data = { .password = password };
    struct pam_conv conv = {
        .conv = pam_conversation,
        .appdata_ptr = &conv_data,
    };

    pam_handle_t *pamh = NULL;
    int rc = pam_start("wlrdp", username, &conv, &pamh);
    if (rc != PAM_SUCCESS) {
        WLRDP_LOG_ERROR("pam_start failed: %s", pam_strerror(pamh, rc));
        return false;
    }

    rc = pam_authenticate(pamh, 0);
    if (rc != PAM_SUCCESS) {
        WLRDP_LOG_WARN("auth failed for '%s': %s",
                        username, pam_strerror(pamh, rc));
        pam_end(pamh, rc);
        return false;
    }

    rc = pam_acct_mgmt(pamh, 0);
    if (rc != PAM_SUCCESS) {
        WLRDP_LOG_WARN("account check failed for '%s': %s",
                        username, pam_strerror(pamh, rc));
        pam_end(pamh, rc);
        return false;
    }

    pam_end(pamh, PAM_SUCCESS);

    /* Look up uid/gid from passwd database */
    struct passwd *pw = getpwnam(username);
    if (!pw) {
        WLRDP_LOG_ERROR("getpwnam('%s') failed: %s",
                        username, strerror(errno));
        return false;
    }

    result->uid = pw->pw_uid;
    result->gid = pw->pw_gid;
    snprintf(result->home, sizeof(result->home), "%s", pw->pw_dir);
    snprintf(result->shell, sizeof(result->shell), "%s", pw->pw_shell);

    WLRDP_LOG_INFO("authenticated user '%s' (uid=%d gid=%d)",
                    username, result->uid, result->gid);
    return true;
}
