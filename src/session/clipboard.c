#include "clipboard.h"
#include "common.h"
#include "ext-data-control-v1-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <freerdp/channels/cliprdr.h>
#include <freerdp/server/cliprdr.h>
#include <winpr/user.h>
#include <winpr/string.h>

#define CLIPBOARD_MIME_TYPE "text/plain;charset=utf-8"
#define CLIPBOARD_MAX_SIZE (16 * 1024 * 1024) /* 16 MiB limit */

/* --- data control source callbacks (for setting clipboard) --- */

static void source_send(void *data,
                        struct ext_data_control_source_v1 *source,
                        const char *mime_type, int32_t fd)
{
    struct wlrdp_clipboard *cb = data;

    if (cb->pending_set_text && strcmp(mime_type, CLIPBOARD_MIME_TYPE) == 0) {
        size_t written = 0;
        while (written < cb->pending_set_len) {
            ssize_t n = write(fd, cb->pending_set_text + written,
                              cb->pending_set_len - written);
            if (n <= 0) break;
            written += n;
        }
    }
    close(fd);
}

static void source_cancelled(void *data,
                             struct ext_data_control_source_v1 *source)
{
    struct wlrdp_clipboard *cb = data;

    if (cb->source == source) {
        ext_data_control_source_v1_destroy(source);
        cb->source = NULL;
    }

    free(cb->pending_set_text);
    cb->pending_set_text = NULL;
    cb->pending_set_len = 0;
    cb->setting_from_rdp = false;
}

static const struct ext_data_control_source_v1_listener source_listener = {
    .send = source_send,
    .cancelled = source_cancelled,
};

/* --- data control offer callbacks (for reading clipboard) --- */

struct clipboard_offer {
    struct wlrdp_clipboard *cb;
    struct ext_data_control_offer_v1 *offer;
    bool has_text;
};

static void offer_offer(void *data,
                        struct ext_data_control_offer_v1 *offer,
                        const char *mime_type)
{
    struct clipboard_offer *co = data;
    if (strcmp(mime_type, CLIPBOARD_MIME_TYPE) == 0) {
        co->has_text = true;
    }
}

static const struct ext_data_control_offer_v1_listener offer_listener = {
    .offer = offer_offer,
};

/* --- data control device callbacks --- */

static void read_offer_text(struct wlrdp_clipboard *cb,
                            struct ext_data_control_offer_v1 *offer)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
        WLRDP_LOG_WARN("clipboard: pipe2 failed: %s", strerror(errno));
        return;
    }

    ext_data_control_offer_v1_receive(offer, CLIPBOARD_MIME_TYPE, fds[1]);
    close(fds[1]);

    wl_display_flush(cb->display);

    /* Switch to blocking mode for the read */
    int flags = fcntl(fds[0], F_GETFL);
    fcntl(fds[0], F_SETFL, flags & ~O_NONBLOCK);

    char *buf = NULL;
    size_t total = 0;
    size_t capacity = 0;

    for (;;) {
        if (total >= CLIPBOARD_MAX_SIZE) break;
        if (total + 4096 > capacity) {
            capacity = (capacity == 0) ? 4096 : capacity * 2;
            if (capacity > CLIPBOARD_MAX_SIZE) capacity = CLIPBOARD_MAX_SIZE;
            char *newbuf = realloc(buf, capacity);
            if (!newbuf) break;
            buf = newbuf;
        }
        ssize_t n = read(fds[0], buf + total, capacity - total);
        if (n <= 0) break;
        total += n;
    }
    close(fds[0]);

    if (total > 0 && buf) {
        free(cb->wl_clipboard_text);
        cb->wl_clipboard_text = buf;
        cb->wl_clipboard_len = total;

        if (cb->on_wl_clipboard_changed && !cb->setting_from_rdp) {
            cb->on_wl_clipboard_changed(cb->cb_data, buf, total);
        }
    } else {
        free(buf);
    }

    cb->setting_from_rdp = false;
}

static void device_data_offer(void *data,
                              struct ext_data_control_device_v1 *device,
                              struct ext_data_control_offer_v1 *offer)
{
    struct wlrdp_clipboard *cb = data;
    (void)cb;

    struct clipboard_offer *co = calloc(1, sizeof(*co));
    if (!co) return;
    co->cb = cb;
    co->offer = offer;
    co->has_text = false;

    ext_data_control_offer_v1_add_listener(offer, &offer_listener, co);
}

static void device_selection(void *data,
                             struct ext_data_control_device_v1 *device,
                             struct ext_data_control_offer_v1 *offer)
{
    struct wlrdp_clipboard *cb = data;
    (void)device;

    if (!offer) return;

    struct clipboard_offer *co = wl_proxy_get_user_data(
        (struct wl_proxy *)offer);
    if (!co) {
        ext_data_control_offer_v1_destroy(offer);
        return;
    }

    if (co->has_text) {
        read_offer_text(cb, offer);
    }

    ext_data_control_offer_v1_destroy(offer);
    free(co);
}

static void device_finished(void *data,
                            struct ext_data_control_device_v1 *device)
{
    struct wlrdp_clipboard *cb = data;
    if (cb->device == device) {
        ext_data_control_device_v1_destroy(device);
        cb->device = NULL;
    }
}

static void device_primary_selection(void *data,
                                     struct ext_data_control_device_v1 *device,
                                     struct ext_data_control_offer_v1 *offer)
{
    (void)data; (void)device;
    if (offer) {
        ext_data_control_offer_v1_destroy(offer);
    }
}

static const struct ext_data_control_device_v1_listener device_listener = {
    .data_offer = device_data_offer,
    .selection = device_selection,
    .finished = device_finished,
    .primary_selection = device_primary_selection,
};

/* --- registry --- */

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    struct wlrdp_clipboard *cb = data;
    (void)version;

    if (strcmp(interface, "ext_data_control_manager_v1") == 0) {
        cb->dcm = wl_registry_bind(registry, name,
            &ext_data_control_manager_v1_interface, 1);
    } else if (strcmp(interface, "wl_seat") == 0) {
        if (!cb->seat) {
            cb->seat = wl_registry_bind(registry, name,
                &wl_seat_interface, 1);
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* --- public API --- */

bool clipboard_init(struct wlrdp_clipboard *cb, const char *display_name,
                    void (*on_change)(void *data, const char *text, size_t len),
                    void *cb_data)
{
    memset(cb, 0, sizeof(*cb));
    cb->on_wl_clipboard_changed = on_change;
    cb->cb_data = cb_data;

    cb->display = wl_display_connect(display_name);
    if (!cb->display) {
        WLRDP_LOG_ERROR("clipboard: failed to connect to '%s'", display_name);
        return false;
    }

    cb->registry = wl_display_get_registry(cb->display);
    wl_registry_add_listener(cb->registry, &registry_listener, cb);
    wl_display_roundtrip(cb->display);

    if (!cb->dcm) {
        WLRDP_LOG_WARN("compositor lacks ext_data_control_manager_v1, "
                        "clipboard disabled");
        clipboard_destroy(cb);
        return false;
    }
    if (!cb->seat) {
        WLRDP_LOG_WARN("clipboard: no wl_seat found");
        clipboard_destroy(cb);
        return false;
    }

    cb->device = ext_data_control_manager_v1_get_data_device(cb->dcm, cb->seat);
    ext_data_control_device_v1_add_listener(cb->device, &device_listener, cb);

    wl_display_roundtrip(cb->display);

    WLRDP_LOG_INFO("clipboard initialized on display '%s'", display_name);
    return true;
}

void clipboard_set_text(struct wlrdp_clipboard *cb, const char *text, size_t len)
{
    if (!cb->dcm || !cb->device) return;

    if (cb->source) {
        ext_data_control_source_v1_destroy(cb->source);
        cb->source = NULL;
    }

    free(cb->pending_set_text);
    cb->pending_set_text = malloc(len);
    if (!cb->pending_set_text) return;
    memcpy(cb->pending_set_text, text, len);
    cb->pending_set_len = len;

    cb->source = ext_data_control_manager_v1_create_data_source(cb->dcm);
    ext_data_control_source_v1_add_listener(cb->source, &source_listener, cb);
    ext_data_control_source_v1_offer(cb->source, CLIPBOARD_MIME_TYPE);

    cb->setting_from_rdp = true;
    ext_data_control_device_v1_set_selection(cb->device, cb->source);

    wl_display_flush(cb->display);
}

/* --- CLIPRDR server callbacks --- */

static UINT on_cliprdr_client_capabilities(CliprdrServerContext *context,
                                           const CLIPRDR_CAPABILITIES *caps)
{
    (void)context; (void)caps;
    WLRDP_LOG_INFO("CLIPRDR: client capabilities received");
    return CHANNEL_RC_OK;
}

static UINT on_cliprdr_client_format_list(CliprdrServerContext *context,
                                          const CLIPRDR_FORMAT_LIST *list)
{
    (void)context->custom; /* cb used only after format data arrives */

    /* Check if client has text on its clipboard */
    bool has_text = false;
    for (uint32_t i = 0; i < list->numFormats; i++) {
        uint32_t id = list->formats[i].formatId;
        if (id == CF_TEXT || id == CF_UNICODETEXT || id == CF_OEMTEXT) {
            has_text = true;
            break;
        }
    }

    /* Acknowledge the format list */
    CLIPRDR_FORMAT_LIST_RESPONSE resp = {
        .common = { .msgFlags = CB_RESPONSE_OK },
    };
    context->ServerFormatListResponse(context, &resp);

    if (has_text) {
        /* Request the text data from the client */
        CLIPRDR_FORMAT_DATA_REQUEST req = {
            .requestedFormatId = CF_UNICODETEXT,
        };
        context->ServerFormatDataRequest(context, &req);
    }

    return CHANNEL_RC_OK;
}

static UINT on_cliprdr_client_format_data_response(
    CliprdrServerContext *context,
    const CLIPRDR_FORMAT_DATA_RESPONSE *resp)
{
    struct wlrdp_clipboard *cb = context->custom;

    if (resp->common.msgFlags != CB_RESPONSE_OK) return CHANNEL_RC_OK;

    const BYTE *data = resp->requestedFormatData;
    uint32_t len = resp->common.dataLen;
    if (!data || len == 0) return CHANNEL_RC_OK;

    /* Convert from UTF-16LE (CF_UNICODETEXT) to UTF-8 */
    size_t utf8_len = 0;
    char *utf8 = ConvertWCharNToUtf8Alloc((const WCHAR *)data,
                                           len / sizeof(WCHAR), &utf8_len);
    if (utf8 && utf8_len > 0) {
        /* Strip trailing null if present */
        while (utf8_len > 0 && utf8[utf8_len - 1] == '\0') utf8_len--;
        clipboard_set_text(cb, utf8, utf8_len);
        WLRDP_LOG_INFO("CLIPRDR: received %zu bytes text from client", utf8_len);
    }
    free(utf8);

    return CHANNEL_RC_OK;
}

static UINT on_cliprdr_client_format_data_request(
    CliprdrServerContext *context,
    const CLIPRDR_FORMAT_DATA_REQUEST *req)
{
    struct wlrdp_clipboard *cb = context->custom;

    CLIPRDR_FORMAT_DATA_RESPONSE resp = {
        .common = { .msgFlags = CB_RESPONSE_FAIL },
    };

    if (cb->wl_clipboard_text && cb->wl_clipboard_len > 0 &&
        (req->requestedFormatId == CF_UNICODETEXT ||
         req->requestedFormatId == CF_TEXT)) {

        if (req->requestedFormatId == CF_UNICODETEXT) {
            size_t wlen = 0;
            WCHAR *wstr = ConvertUtf8NToWCharAlloc(cb->wl_clipboard_text,
                                                    cb->wl_clipboard_len, &wlen);
            if (wstr) {
                resp.common.msgFlags = CB_RESPONSE_OK;
                resp.common.dataLen = (wlen + 1) * sizeof(WCHAR);
                resp.requestedFormatData = (const BYTE *)wstr;
                context->ServerFormatDataResponse(context, &resp);
                free(wstr);
                return CHANNEL_RC_OK;
            }
        } else {
            /* CF_TEXT: send as-is (ASCII subset) */
            resp.common.msgFlags = CB_RESPONSE_OK;
            resp.common.dataLen = cb->wl_clipboard_len + 1;
            resp.requestedFormatData = (const BYTE *)cb->wl_clipboard_text;
            context->ServerFormatDataResponse(context, &resp);
            return CHANNEL_RC_OK;
        }
    }

    context->ServerFormatDataResponse(context, &resp);
    return CHANNEL_RC_OK;
}

/* --- CLIPRDR open/close --- */

bool clipboard_open_cliprdr(struct wlrdp_clipboard *cb, void *vcm)
{
    CliprdrServerContext *cliprdr = cliprdr_server_context_new(vcm);
    if (!cliprdr) {
        WLRDP_LOG_WARN("failed to create CLIPRDR server context");
        return false;
    }

    cliprdr->custom = cb;
    cliprdr->useLongFormatNames = TRUE;
    cliprdr->streamFileClipEnabled = FALSE;
    cliprdr->canLockClipData = FALSE;
    cliprdr->autoInitializationSequence = TRUE;

    cliprdr->ClientCapabilities = on_cliprdr_client_capabilities;
    cliprdr->ClientFormatList = on_cliprdr_client_format_list;
    cliprdr->ClientFormatDataResponse = on_cliprdr_client_format_data_response;
    cliprdr->ClientFormatDataRequest = on_cliprdr_client_format_data_request;

    if (cliprdr->Open(cliprdr) != CHANNEL_RC_OK) {
        WLRDP_LOG_WARN("CLIPRDR channel Open failed");
        cliprdr_server_context_free(cliprdr);
        return false;
    }

    cb->cliprdr_context = cliprdr;
    WLRDP_LOG_INFO("CLIPRDR channel opened");
    return true;
}

void clipboard_close_cliprdr(struct wlrdp_clipboard *cb)
{
    if (!cb->cliprdr_context) return;
    CliprdrServerContext *cliprdr = cb->cliprdr_context;
    cliprdr->Close(cliprdr);
    cliprdr_server_context_free(cliprdr);
    cb->cliprdr_context = NULL;
}

void clipboard_notify_rdp(struct wlrdp_clipboard *cb)
{
    CliprdrServerContext *cliprdr = cb->cliprdr_context;
    if (!cliprdr) return;

    CLIPRDR_FORMAT formats[] = {
        { .formatId = CF_UNICODETEXT, .formatName = NULL },
        { .formatId = CF_TEXT, .formatName = NULL },
    };
    CLIPRDR_FORMAT_LIST list = {
        .numFormats = 2,
        .formats = formats,
    };

    cliprdr->ServerFormatList(cliprdr, &list);
}

int clipboard_get_fd(struct wlrdp_clipboard *cb)
{
    return wl_display_get_fd(cb->display);
}

int clipboard_dispatch(struct wlrdp_clipboard *cb)
{
    return wl_display_dispatch(cb->display);
}

void clipboard_flush(struct wlrdp_clipboard *cb)
{
    wl_display_flush(cb->display);
}

void clipboard_destroy(struct wlrdp_clipboard *cb)
{
    free(cb->wl_clipboard_text);
    free(cb->pending_set_text);
    if (cb->source) ext_data_control_source_v1_destroy(cb->source);
    if (cb->device) ext_data_control_device_v1_destroy(cb->device);
    if (cb->dcm) ext_data_control_manager_v1_destroy(cb->dcm);
    if (cb->seat) wl_seat_destroy(cb->seat);
    if (cb->registry) wl_registry_destroy(cb->registry);
    if (cb->display) wl_display_disconnect(cb->display);
    memset(cb, 0, sizeof(*cb));
}
