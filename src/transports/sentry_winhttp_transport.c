#include "sentry_winhttp_transport.h"

#include "sentry_alloc.h"
#include "sentry_core.h"
#include "sentry_envelope.h"
#include "sentry_string.h"
#include "sentry_sync.h"

#include <winhttp.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    sentry_bgworker_t *bgworker;
    wchar_t *user_agent;
    wchar_t *proxy;
    ULONGLONG disabled_until;
    HINTERNET session;
    HINTERNET connect;

    bool initialized;
} winhttp_transport_state_t;

struct task_state {
    winhttp_transport_state_t *transport_state;
    sentry_envelope_t *envelope;
};

struct header_info {
    int retry_after;
};

static winhttp_transport_state_t *
new_transport_state(void)
{
    winhttp_transport_state_t *state = SENTRY_MAKE(winhttp_transport_state_t);
    if (!state) {
        return NULL;
    }

    state->bgworker = sentry__bgworker_new();
    if (!state->bgworker) {
        sentry_free(state);
        return NULL;
    }

    state->disabled_until = 0;
    state->connect = NULL;
    state->session = NULL;
    state->user_agent = NULL;
    state->proxy = NULL;

    return state;
}

static void
winhttp_transport_start(sentry_transport_t *transport)
{
    winhttp_transport_state_t *state = transport->data;

    const sentry_options_t *opts = sentry_get_options();

    state->user_agent = sentry__string_to_wstr(SENTRY_SDK_USER_AGENT);

    if (opts->http_proxy && strstr(opts->http_proxy, "http://") == 0) {
        const char *ptr = opts->http_proxy + 7;
        const char *slash = strchr(ptr, '/');
        if (slash) {
            char *copy = sentry__string_clonen(ptr, slash - ptr);
            state->proxy = sentry__string_to_wstr(copy);
            sentry_free(copy);
        } else {
            state->proxy = sentry__string_to_wstr(ptr);
        }
    }

    if (state->proxy) {
        state->session
            = WinHttpOpen(state->user_agent, WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                state->proxy, WINHTTP_NO_PROXY_BYPASS, 0);
    } else {
        state->session = WinHttpOpen(state->user_agent,
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        // On windows 7, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY does
        // not work on error we fallback to
        // WINHTTP_ACCESS_TYPE_NO_PROXY
        if (!state->session) {
            state->session
                = WinHttpOpen(state->user_agent, WINHTTP_ACCESS_TYPE_NO_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        }
    }

    sentry__bgworker_start(state->bgworker);
}

static void
winhttp_transport_shutdown(sentry_transport_t *transport)
{
    winhttp_transport_state_t *state = transport->data;
    sentry__bgworker_shutdown(state->bgworker, 5000);
}

static void
winhttp_transport_free(sentry_transport_t *transport)
{
    winhttp_transport_state_t *state = transport->data;
    sentry__bgworker_free(state->bgworker);
    WinHttpCloseHandle(state->connect);
    WinHttpCloseHandle(state->session);
    sentry_free(state->user_agent);
    sentry_free(state->proxy);
    sentry_free(state);
}

static bool
for_each_request_callback(sentry_prepared_http_request_t *req,
    const sentry_envelope_t *envelope, void *data)
{
    struct task_state *ts = data;
    winhttp_transport_state_t *state = ts->transport_state;
    const sentry_options_t *opts = sentry_get_options();

    if (!opts || opts->dsn.empty || sentry__should_skip_upload()) {
        SENTRY_DEBUG("skipping event upload");
        return false;
    }

    ULONGLONG now = GetTickCount64();
    if (now < state->disabled_until) {
        return false;
    }

    wchar_t *url = sentry__string_to_wstr(req->url);

    URL_COMPONENTS url_components;
    wchar_t hostname[128];
    wchar_t url_path[4096];
    memset(&url_components, 0, sizeof(URL_COMPONENTS));
    url_components.dwStructSize = sizeof(URL_COMPONENTS);
    url_components.lpszHostName = hostname;
    url_components.dwHostNameLength = 128;
    url_components.lpszUrlPath = url_path;
    url_components.dwUrlPathLength = 1024;

    WinHttpCrackUrl(url, 0, 0, &url_components);
    if (!state->connect) {
        state->connect = WinHttpConnect(state->session,
            url_components.lpszHostName, url_components.nPort, 0);
    }

    bool is_secure = strstr(req->url, "https") == req->url;
    HINTERNET request = WinHttpOpenRequest(state->connect, L"POST",
        url_components.lpszUrlPath, NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, is_secure ? WINHTTP_FLAG_SECURE : 0);

    sentry_stringbuilder_t sb;
    sentry__stringbuilder_init(&sb);

    for (size_t i = 0; i < req->headers_len; i++) {
        sentry__stringbuilder_append(&sb, req->headers[i].key);
        sentry__stringbuilder_append_char(&sb, ':');
        sentry__stringbuilder_append(&sb, req->headers[i].value);
        sentry__stringbuilder_append(&sb, "\r\n");
    }

    char *buf = sentry__stringbuilder_into_string(&sb);
    wchar_t *headers = sentry__string_to_wstr(buf);
    sentry_free(buf);

    if (WinHttpSendRequest(request, headers, -1, (LPVOID)req->payload,
            (DWORD)req->payload_len, (DWORD)req->payload_len, 0)) {
        WinHttpReceiveResponse(request, NULL);

        DWORD status_code = 0;
        DWORD status_code_size = sizeof(DWORD);

        bool foo = WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_code_size,
            WINHTTP_NO_HEADER_INDEX);
        DWORD err = GetLastError();

        if (status_code == 429) {
            DWORD retry_after = 0;
            DWORD retry_after_size = sizeof(DWORD);
            if (WinHttpQueryHeaders(request,
                    WINHTTP_QUERY_RETRY_AFTER | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &retry_after,
                    &retry_after_size, WINHTTP_NO_HEADER_INDEX)) {
                ts->transport_state->disabled_until
                    = GetTickCount64() + retry_after * 1000;
            }
        }
    }
    WinHttpCloseHandle(request);
    sentry_free(url);
    sentry_free(headers);

    sentry__prepared_http_request_free(req);
    return true;
}

static void
task_exec_func(void *data)
{
    struct task_state *ts = data;
    sentry__envelope_for_each_request(
        ts->envelope, for_each_request_callback, data);
}

static void
task_cleanup_func(void *data)
{
    struct task_state *ts = data;
    sentry_envelope_free(ts->envelope);
    sentry_free(ts);
}

static void
winhttp_transport_send_envelope(
    struct sentry_transport_s *transport, sentry_envelope_t *envelope)
{
    winhttp_transport_state_t *state = transport->data;
    struct task_state *ts = SENTRY_MAKE(struct task_state);
    if (!ts) {
        sentry_envelope_free(envelope);
        return;
    }
    ts->transport_state = state;
    ts->envelope = envelope;
    sentry__bgworker_submit(
        state->bgworker, task_exec_func, task_cleanup_func, ts);
}

sentry_transport_t *
sentry__new_winhttp_transport(void)
{
    SENTRY_DEBUG("initializing winhttp transport");
    sentry_transport_t *transport = SENTRY_MAKE(sentry_transport_t);
    if (!transport) {
        return NULL;
    }

    winhttp_transport_state_t *state = new_transport_state();
    if (!state) {
        sentry_free(transport);
        return NULL;
    }

    transport->data = state;
    transport->free_func = winhttp_transport_free;
    transport->send_envelope_func = winhttp_transport_send_envelope;
    transport->startup_func = winhttp_transport_start;
    transport->shutdown_func = winhttp_transport_shutdown;

    return transport;
}
