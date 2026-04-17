#include "ipc.h"
#include "common.h"

#include <sys/socket.h>
#include <unistd.h>

bool ipc_send_msg(int sock_fd, const struct wlrdp_ipc_msg *msg, int fd)
{
    uint8_t buf[8 + sizeof(msg->payload)];
    uint32_t total = 8 + msg->payload_len;

    memcpy(buf, &msg->type, 4);
    memcpy(buf + 4, &msg->payload_len, 4);
    if (msg->payload_len > 0) {
        memcpy(buf + 8, msg->payload, msg->payload_len);
    }

    struct iovec iov = {
        .iov_base = buf,
        .iov_len = total,
    };

    struct msghdr hdr = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };

    /* Attach fd via SCM_RIGHTS if provided */
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;

    if (fd >= 0) {
        hdr.msg_control = cmsg_buf.buf;
        hdr.msg_controllen = sizeof(cmsg_buf.buf);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&hdr);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    }

    ssize_t n = sendmsg(sock_fd, &hdr, MSG_NOSIGNAL);
    if (n < 0) {
        WLRDP_LOG_ERROR("ipc_send_msg: sendmsg failed: %s", strerror(errno));
        return false;
    }

    return (size_t)n == total;
}

bool ipc_recv_msg(int sock_fd, struct wlrdp_ipc_msg *msg, int *out_fd)
{
    *out_fd = -1;

    uint8_t buf[8 + sizeof(msg->payload)];

    struct iovec iov = {
        .iov_base = buf,
        .iov_len = sizeof(buf),
    };

    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;

    struct msghdr hdr = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg_buf.buf,
        .msg_controllen = sizeof(cmsg_buf.buf),
    };

    ssize_t n = recvmsg(sock_fd, &hdr, 0);
    if (n <= 0) {
        return false;
    }

    if ((size_t)n < 8) {
        WLRDP_LOG_ERROR("ipc_recv_msg: short read (%zd bytes)", n);
        return false;
    }

    memcpy(&msg->type, buf, 4);
    memcpy(&msg->payload_len, buf + 4, 4);

    if (msg->payload_len > sizeof(msg->payload)) {
        WLRDP_LOG_ERROR("ipc_recv_msg: payload too large (%u)", msg->payload_len);
        return false;
    }

    if (msg->payload_len > 0 && (size_t)n >= 8 + msg->payload_len) {
        memcpy(msg->payload, buf + 8, msg->payload_len);
    }

    /* Extract ancillary fd if present */
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&hdr);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
        cmsg->cmsg_type == SCM_RIGHTS) {
        memcpy(out_fd, CMSG_DATA(cmsg), sizeof(int));
    }

    return true;
}
