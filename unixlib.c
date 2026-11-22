/*
 * Unix-side implementation for xgameruntime: talking to xodus-service
 *
 * Copyright 2026 Xodus GDK-on-Linux project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"

#include "wine/debug.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

/* xodus-service's framing: a magic, then a 16-bit type and length, then the
 * body. The reply comes back with the request's type plus one. */
#define XODUS_XML_MAGIC          0x58445358
#define XODUS_XSTS_TOKEN_REQUEST 5

/* Escape text for XML. A URL's query string routinely contains '&', which
 * starts an entity reference -- embedding one raw makes the document
 * ill-formed and the service rejects the whole request, which is what stopped
 * profile lookups (their URLs carry query parameters) while plain ones worked. */
static char *xml_escape( const char *in )
{
    static const struct { char ch; const char *rep; } map[] = {
        { '&', "&amp;" }, { '<', "&lt;" }, { '>', "&gt;" },
        { '"', "&quot;" }, { '\'', "&apos;" },
    };
    size_t len = 0, i;
    const char *p;
    char *out, *o;

    if (!in) in = "";
    for (p = in; *p; p++)
    {
        size_t add = 1;
        for (i = 0; i < ARRAY_SIZE(map); i++)
            if (*p == map[i].ch) { add = strlen( map[i].rep ); break; }
        len += add;
    }
    if (!(out = malloc( len + 1 ))) return NULL;
    for (p = in, o = out; *p; p++)
    {
        const char *rep = NULL;
        for (i = 0; i < ARRAY_SIZE(map); i++)
            if (*p == map[i].ch) { rep = map[i].rep; break; }
        if (rep) { strcpy( o, rep ); o += strlen( rep ); }
        else *o++ = *p;
    }
    *o = 0;
    return out;
}

/* Standard base64, for carrying a request body through the XML. */
static char *base64_encode( const void *data, size_t len )
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *p = data;
    size_t i, o = 0;
    char *out;

    if (!(out = malloc( ((len + 2) / 3) * 4 + 1 ))) return NULL;
    for (i = 0; i + 2 < len; i += 3)
    {
        out[o++] = tbl[p[i] >> 2];
        out[o++] = tbl[((p[i] & 3) << 4) | (p[i+1] >> 4)];
        out[o++] = tbl[((p[i+1] & 15) << 2) | (p[i+2] >> 6)];
        out[o++] = tbl[p[i+2] & 63];
    }
    if (i < len)
    {
        out[o++] = tbl[p[i] >> 2];
        if (i + 1 < len)
        {
            out[o++] = tbl[((p[i] & 3) << 4) | (p[i+1] >> 4)];
            out[o++] = tbl[(p[i+1] & 15) << 2];
        }
        else
        {
            out[o++] = tbl[(p[i] & 3) << 4];
            out[o++] = '=';
        }
        out[o++] = '=';
    }
    out[o] = 0;
    return out;
}

/* Isolate a candidate service without changing desktop/audio discovery. */
static const char *xodus_runtime_dir(void)
{
    const char *override = getenv("XODUS_RUNTIME_DIR");
    return override && *override ? override : getenv("XDG_RUNTIME_DIR");
}

/* Every exchange with the service is bounded by one deadline, reads, writes and
 * the connect included. A title calls into here from threads it expects to
 * come back -- a service that is stalled, or busy waiting on a keyring prompt,
 * must cost it a timeout, not the thread. */
static long long license_now_ms( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static BOOL license_io( int fd, void *buffer, SIZE_T size, BOOL writing, long long deadline )
{
    char *p = buffer;
    struct pollfd pollfd = { fd, writing ? POLLOUT : POLLIN, 0 };
    long long left;
    ssize_t n;
    int ret;

    while (size)
    {
        left = deadline - license_now_ms();
        if (left <= 0) { errno = ETIMEDOUT; return FALSE; }
        ret = poll( &pollfd, 1, left );
        if (!ret) { errno = ETIMEDOUT; return FALSE; }
        if (ret < 0) { if (errno == EINTR) continue; return FALSE; }
        n = writing ? send( fd, p, size, MSG_NOSIGNAL ) : recv( fd, p, size, 0 );
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return FALSE;
        size -= n;
        p += n;
    }
    return TRUE;
}

/* The socket is non-blocking and close-on-exec from the start: a process the
 * title starts while a request is open (a helper, a crash reporter) must not
 * inherit the connection and keep it alive in the service. A listener whose
 * backlog is full answers EAGAIN rather than queueing the connect, so that is
 * retried until the deadline instead of waited on for ever. */
static int connect_service( long long deadline )
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    const char *runtime_dir = xodus_runtime_dir();
    struct pollfd pollfd;
    socklen_t error_size;
    long long left;
    int fd, error;

    if (!runtime_dir || !runtime_dir[0])
    {
        WARN( "XDG_RUNTIME_DIR is not set; cannot reach xodus-service.\n" );
        errno = ENOENT;
        return -1;
    }
    if (snprintf( addr.sun_path, sizeof(addr.sun_path), "%s/xodus.sock", runtime_dir ) >= (int)sizeof(addr.sun_path))
    {
        WARN( "socket path does not fit.\n" );
        errno = ENAMETOOLONG;
        return -1;
    }

    if ((fd = socket( AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0 )) < 0) return -1;
    for (;;)
    {
        if (!connect( fd, (struct sockaddr *)&addr, sizeof(addr) )) return fd;
        if (errno == EINTR) continue;
        left = deadline - license_now_ms();
        if (errno == EAGAIN && left > 0)
        {
            usleep( left > 10 ? 10000 : left * 1000 );
            continue;
        }
        if (errno == EINPROGRESS && left > 0)
        {
            pollfd.fd = fd; pollfd.events = POLLOUT; pollfd.revents = 0;
            if (poll( &pollfd, 1, left ) <= 0) { error = ETIMEDOUT; break; }
            error_size = sizeof(error);
            if (getsockopt( fd, SOL_SOCKET, SO_ERROR, &error, &error_size )) error = errno;
            if (!error) return fd;
            break;
        }
        error = (errno == EAGAIN || errno == EINPROGRESS) ? ETIMEDOUT : errno;
        break;
    }
    WARN( "xodus-service is not reachable on %s (%s); no Xbox Live sign-in.\n",
          addr.sun_path, strerror( error ) );
    close( fd );
    errno = error;
    return -1;
}

/* What a failed exchange means to the caller: out of time, or not there. */
static NTSTATUS io_status( NTSTATUS fallback )
{
    return errno == ETIMEDOUT ? STATUS_IO_TIMEOUT : fallback;
}

/* Copy the text between <tag> and </tag> into out. The service's replies are
 * small, flat and machine-generated, so a scan is enough and saves dragging an
 * XML parser into the unix side. */
static void extract_tag( const char *xml, const char *tag, char *out, size_t out_len )
{
    char open_tag[64], close_tag[64];
    const char *start, *end;
    size_t len;

    out[0] = 0;
    snprintf( open_tag, sizeof(open_tag), "<%s>", tag );
    snprintf( close_tag, sizeof(close_tag), "</%s>", tag );

    if (!(start = strstr( xml, open_tag ))) return;
    start += strlen( open_tag );
    if (!(end = strstr( start, close_tag ))) return;

    len = end - start;
    if (len >= out_len) len = out_len - 1;
    memcpy( out, start, len );
    out[len] = 0;
}

static NTSTATUS xsts_token_once( struct xsts_token_params *params, long long deadline )
{
    char *request = NULL, header[8], *body = NULL, *body_b64 = NULL;
    unsigned int extended_size;
    char *url_esc = NULL, *method_esc = NULL, *title_esc = NULL, *client_esc = NULL;
    unsigned int magic, size = 0;
    unsigned short type, len;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    char expiry[XGR_IDENTITY_MAX];
    int fd, body_len;
    size_t request_size;

    params->token[0] = params->xuid[0] = params->gamertag[0] = params->signature[0] = 0;
    params->expiry = 0;

    if ((fd = connect_service( deadline )) < 0) return io_status( STATUS_NOT_FOUND );

    /* The signature covers the body, so it has to travel with the ask; base64
     * keeps arbitrary bytes safe inside the XML. */
    if (params->body_size > 6 * 1024 * 1024)
    { status = STATUS_INVALID_BUFFER_SIZE; goto done; }
    if (params->body && params->body_size)
    {
        if (!(body_b64 = base64_encode( params->body, params->body_size ))) goto done;
    }

    if (!(url_esc = xml_escape( params->url )) ||
        !(method_esc = xml_escape( params->method ? params->method : "GET" )) ||
        !(title_esc = xml_escape( params->title_id ? params->title_id : "" )) ||
        !(client_esc = xml_escape( params->title_client_id ? params->title_client_id : "" )))
    { status = STATUS_NO_MEMORY; goto done; }

    request_size = 512 + strlen( url_esc ) + strlen( method_esc ) +
                   strlen( title_esc ) + strlen( client_esc ) +
                   (body_b64 ? strlen( body_b64 ) : 0);
    if (!(request = malloc( request_size ))) { status = STATUS_NO_MEMORY; goto done; }

    /* RelyingParty stays empty on purpose: the service resolves it from the
     * title-management endpoint list, which is authoritative and knows hosts
     * this side does not. TitleId and MSAAppId, on the other hand, only this
     * side can see -- they come out of the game's own MicrosoftGame.config. */
    body_len = snprintf( request, request_size,
                         "<XSTSTokenRequest><Url>%s</Url><RelyingParty></RelyingParty>"
                         "<Method>%s</Method><TitleId>%s</TitleId>"
                         "<TitleClientId>%s</TitleClientId>"
                         "<BodyBase64>%s</BodyBase64>%s</XSTSTokenRequest>",
                         url_esc, method_esc, title_esc, client_esc,
                         body_b64 ? body_b64 : "",
                         (params->flags & XGR_XSTS_FORCE_REFRESH) ? "<ForceRefresh>true</ForceRefresh>" : "" );
    if (body_len < 0 || body_len >= (int)request_size) goto done;
    if (body_len > 8 * 1024 * 1024)
    { status = STATUS_INVALID_BUFFER_SIZE; goto done; }

    magic = XODUS_XML_MAGIC;
    type = XODUS_XSTS_TOKEN_REQUEST | (body_len > 0xffff ? 0x8000 : 0);
    len = body_len > 0xffff ? 0xffff : body_len;
    memcpy( header, &magic, 4 );
    memcpy( header + 4, &type, 2 );
    memcpy( header + 6, &len, 2 );

    errno = 0;
    if (!license_io( fd, header, sizeof(header), TRUE, deadline )) goto io_failed;
    extended_size = body_len;
    if (body_len > 0xffff && !license_io( fd, &extended_size, sizeof(extended_size), TRUE, deadline ))
        goto io_failed;
    if (!license_io( fd, request, body_len, TRUE, deadline )) goto io_failed;

    if (!license_io( fd, header, sizeof(header), FALSE, deadline )) goto io_failed;
    memcpy( &magic, header, 4 );
    memcpy( &type, header + 4, 2 );
    memcpy( &len, header + 6, 2 );
    if (magic != XODUS_XML_MAGIC || type != XODUS_XSTS_TOKEN_REQUEST + 1)
    {
        WARN( "unexpected reply magic %#x.\n", magic );
        goto done;
    }
    if (!len)
    {
        WARN( "xodus-service returned no token; the account may not be signed in.\n" );
        status = STATUS_LOGON_FAILURE;
        goto done;
    }

    size = len;
    if (!(body = malloc( size + 1 ))) { status = STATUS_NO_MEMORY; goto done; }
    if (!license_io( fd, body, size, FALSE, deadline )) goto io_failed;
    body[size] = 0;

    extract_tag( body, "Token", params->token, sizeof(params->token) );
    extract_tag( body, "Xuid", params->xuid, sizeof(params->xuid) );
    extract_tag( body, "Gamertag", params->gamertag, sizeof(params->gamertag) );
    extract_tag( body, "Signature", params->signature, sizeof(params->signature) );
    extract_tag( body, "Expiry", expiry, sizeof(expiry) );
    params->expiry = atoll( expiry );

    if (!params->token[0])
    {
        status = STATUS_LOGON_FAILURE;
        goto done;
    }

    TRACE( "got a %zu-character token for %s (gamertag %s).\n",
           strlen( params->token ), debugstr_a( params->url ), debugstr_a( params->gamertag ) );
    status = STATUS_SUCCESS;
    goto done;

io_failed:
    status = io_status( STATUS_UNSUCCESSFUL );
done:
    free( url_esc );
    free( method_esc );
    free( title_esc );
    free( client_esc );
    free( request );
    free( body_b64 );
    if (body)
    {
        /* The reply carries the bearer token; do not leave it in freed memory. */
        memset( body, 0, size );
        free( body );
    }
    close( fd );
    return status;
}

/* Ask again after a transient failure. The service is a separate process, so a
 * request can lose a race with it at startup -- and the caller here is usually
 * resolving the signed-in account, where giving up once meant the title showed
 * nobody signed in until it was restarted. A refused sign-in is not transient
 * and is returned as it stands. */
static NTSTATUS xsts_token( void *args )
{
    static const int backoff_ms[] = { 50, 150, 300 };
    struct xsts_token_params *params = args;
    long long deadline = license_now_ms() + (params->timeout_ms ? params->timeout_ms : XGR_XSTS_TIMEOUT_MS);
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    long long left;
    unsigned int i;

    /* Retries share the request's one deadline, and running out of it is final:
     * a service that did not answer in time will not answer a second ask any
     * sooner. */
    for (i = 0; i < ARRAY_SIZE(backoff_ms) + 1; i++)
    {
        status = xsts_token_once( params, deadline );
        if (status == STATUS_SUCCESS || status == STATUS_LOGON_FAILURE || status == STATUS_IO_TIMEOUT)
            return status;
        if (i < ARRAY_SIZE(backoff_ms))
        {
            left = deadline - license_now_ms();
            if (left <= backoff_ms[i]) return STATUS_IO_TIMEOUT;
            TRACE( "token request failed (%#x); retrying.\n", (unsigned int)status );
            usleep( backoff_ms[i] * 1000 );
        }
    }
    return status;
}

/* A dedicated request keeps account authorization and HTTP inside the service.
 * The XML request uses base64 for custom text (XML normalizes controls). The
 * response frame contains only the validated ASCII JWT, or is empty on error.
 * Old services reply with an unknown-message frame; reject it, never fall back
 * to a successful empty token. Bound the entire IPC exchange, including reads. */
static NTSTATUS license_token( void *args )
{
    struct license_token_params *params = args;
    char *custom = NULL, *request = NULL, *parent = NULL, *title = NULL, *client = NULL, *id;
    unsigned char header[8];
    unsigned int size, i;
    int fd = -1, n, used;
    long long deadline = license_now_ms() + 50000;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    params->token[0] = 0;
    if (!(custom = base64_encode( params->custom, strlen(params->custom) )) ||
        !(parent = xml_escape( params->store_id )) || !(title = xml_escape( params->title_id )) ||
        !(client = xml_escape( params->title_client_id )) || !(request = malloc( 65536 )))
    { status = STATUS_NO_MEMORY; goto done; }
    n = snprintf( request, 65536, "<LicenseTokenRequest><ParentProductId>%s</ParentProductId>"
                  "<CustomBase64>%s</CustomBase64><TitleId>%s</TitleId>"
                  "<TitleClientId>%s</TitleClientId><ProductIds>", parent, custom, title, client );
    if (n < 0 || n >= 65536) goto done;
    used = n;
    for (i = 0; i < params->count; i++)
    {
        if (!(id = xml_escape( params->ids[i] ))) { status = STATUS_NO_MEMORY; goto done; }
        n = snprintf( request + used, 65536 - used, "<ProductId>%s</ProductId>", id );
        free( id );
        if (n < 0 || n >= 65536 - used) goto done;
        used += n;
    }
    n = snprintf( request + used, 65536 - used, "</ProductIds></LicenseTokenRequest>" );
    if (n < 0 || n >= 65536 - used) goto done;
    used += n;
    if ((fd = connect_service( deadline )) < 0) { status = io_status( STATUS_NOT_FOUND ); goto done; }
    header[0] = 'X'; header[1] = 'S'; header[2] = 'D'; header[3] = 'X';
    header[4] = 7; header[5] = 0;
    header[6] = used & 0xff; header[7] = used >> 8;
    if (!license_io( fd, header, sizeof(header), TRUE, deadline ) ||
        !license_io( fd, request, used, TRUE, deadline ) ||
        !license_io( fd, header, sizeof(header), FALSE, deadline ))
    { if (errno == ETIMEDOUT) status = STATUS_IO_TIMEOUT; goto done; }
    if (memcmp( header, "XSDX", 4 ) || header[4] != 8 || header[5]) goto done;
    size = header[6] | (header[7] << 8);
    if (!size || size >= sizeof(params->token)) goto done;
    if (!license_io( fd, params->token, size, FALSE, deadline ))
    { if (errno == ETIMEDOUT) status = STATUS_IO_TIMEOUT; goto done; }
    for (i = 0; i < size; i++)
        if (!((params->token[i] >= 'a' && params->token[i] <= 'z') ||
              (params->token[i] >= 'A' && params->token[i] <= 'Z') ||
              (params->token[i] >= '0' && params->token[i] <= '9') ||
              params->token[i] == '-' || params->token[i] == '_' || params->token[i] == '.')) goto done;
    params->token[size] = 0;
    status = STATUS_SUCCESS;
done:
    if (status) params->token[0] = 0;
    if (fd >= 0) close( fd );
    free( request ); free( custom ); free( parent ); free( title ); free( client );
    return status;
}

/* Separate bounded catalog framing leaves all released IPC messages intact. */
static NTSTATUS store_catalog( void *args )
{
    struct store_catalog_params *params = args;
    unsigned char header[8];
    unsigned int size;
    long long deadline = license_now_ms() + 50000;
    int fd;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    params->response_size = 0;
    if (!params->request_size || params->request_size > 24000 || !params->response)
        return STATUS_INVALID_PARAMETER;
    if ((fd = connect_service( deadline )) < 0) return io_status( STATUS_NOT_FOUND );
    errno = 0;
    header[0] = 'C'; header[1] = 'S'; header[2] = 'D'; header[3] = 'X';
    size = params->request_size;
    header[4] = size; header[5] = size >> 8; header[6] = size >> 16; header[7] = size >> 24;
    if (!license_io( fd, header, 8, TRUE, deadline ) ||
        !license_io( fd, (void *)params->request, size, TRUE, deadline ) ||
        !license_io( fd, header, 8, FALSE, deadline )) goto done;
    if (memcmp( header, "CSDX", 4 )) goto done;
    size = header[4] | ((unsigned int)header[5] << 8) | ((unsigned int)header[6] << 16) | ((unsigned int)header[7] << 24);
    if (size < 12 || size > XGR_CATALOG_MAX) goto done;
    if (!license_io( fd, params->response, size, FALSE, deadline )) goto done;
    params->response_size = size;
    status = STATUS_SUCCESS;
done:
    if (status && errno == ETIMEDOUT) status = STATUS_IO_TIMEOUT;
    close( fd );
    return status;
}

/* Ferestre account roles v1: fixed-size identity response, no profile IDs or
 * credentials supplied by a title. The selected service owns both contexts. */
static NTSTATUS account_roles( void *args )
{
    struct account_roles_params *params = args;
    UINT32 request[2] = {0x58445341, 1}, response[4];
    long long deadline = license_now_ms() + 5000;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    int fd;

    memset( params, 0, sizeof(*params) );
    if ((fd = connect_service( deadline )) < 0) return io_status( STATUS_NOT_FOUND );
    errno = 0;
    if (!license_io(fd,request,sizeof(request),TRUE,deadline) ||
        !license_io(fd,response,sizeof(response),FALSE,deadline))
    { status = io_status( STATUS_UNSUCCESSFUL ); goto done; }
    if (response[0] != 1 || response[1] > 1 || response[2] > 1 || response[3] > 1)
    {
        WARN( "unsupported account roles reply (version %u).\n", response[0] );
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }
    params->store_signed_in = response[1];
    params->player_signed_in = response[2];
    params->same_store_user = response[1] && response[2] && response[3];
    TRACE("Ferestre account roles v1: Store %u Player %u same %u\n",response[1],response[2],response[3]);
    status = STATUS_SUCCESS;
done:
    close(fd);
    return status;
}

static NTSTATUS store_entitlement( void *args )
{
    struct store_entitlement_params *params = args;
    UINT32 request[3] = {0x58445345,1,12}, response[2];
    long long deadline = license_now_ms() + 30000;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    int fd;

    params->active = 0;
    if (strnlen(params->store_id,sizeof(params->store_id)) != 12) return STATUS_INVALID_PARAMETER;
    if ((fd = connect_service( deadline )) < 0) return io_status( STATUS_NOT_FOUND );
    errno = 0;
    if (!license_io(fd,request,sizeof(request),TRUE,deadline) ||
        !license_io(fd,params->store_id,12,TRUE,deadline) ||
        !license_io(fd,response,sizeof(response),FALSE,deadline))
    { status = io_status( STATUS_UNSUCCESSFUL ); goto done; }
    if (response[0] != 1 || response[1] > 1)
    {
        WARN( "unsupported entitlement reply (version %u).\n", response[0] );
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }
    params->active = response[1];
    status = STATUS_SUCCESS;
done:
    close(fd);
    return status;
}

/* Ferestre Store ID key v1 ("KSDX"): the service owns the Store account's
 * Microsoft account ticket and posts the title's service ticket to the
 * Collections keys endpoint. Neither ticket nor key is logged. The reply is
 * fixed framing with the key as ASCII JWT characters only. */
static NTSTATUS store_id_key( void *args )
{
    struct store_id_key_params *params = args;
    UINT32 request[5], response[3];
    long long deadline = license_now_ms() + 50000;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    size_t ticket_len, id_len, i;
    int fd;

    params->key[0] = 0;
    if (!params->service_ticket) return STATUS_INVALID_PARAMETER;
    ticket_len = strnlen( params->service_ticket, 16385 );
    id_len = params->publisher_user_id ? strnlen( params->publisher_user_id, 1025 ) : 0;
    if (!ticket_len || ticket_len > 16384 || id_len > 1024) return STATUS_INVALID_PARAMETER;
    if ((fd = connect_service( deadline )) < 0) return io_status( STATUS_NOT_FOUND );
    request[0] = 0x5844534b; request[1] = 1; request[2] = 1;
    request[3] = ticket_len; request[4] = id_len;
    errno = 0;
    if (!license_io( fd, request, sizeof(request), TRUE, deadline ) ||
        !license_io( fd, (void *)params->service_ticket, ticket_len, TRUE, deadline ) ||
        (id_len && !license_io( fd, (void *)params->publisher_user_id, id_len, TRUE, deadline )) ||
        !license_io( fd, response, sizeof(response), FALSE, deadline ))
    { status = io_status( STATUS_UNSUCCESSFUL ); goto done; }
    if (response[0] != 1)
    {
        WARN( "unsupported Store ID key reply (version %u).\n", response[0] );
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }
    if (response[1] || !response[2] || response[2] >= sizeof(params->key))
    {
        WARN( "xodus-service issued no Store ID key (status %u).\n", response[1] );
        status = response[1] == 2 ? STATUS_NOT_SUPPORTED : STATUS_UNSUCCESSFUL;
        goto done;
    }
    if (!license_io( fd, params->key, response[2], FALSE, deadline ))
    { status = io_status( STATUS_UNSUCCESSFUL ); goto done; }
    for (i = 0; i < response[2]; i++)
        if (!((params->key[i] >= 'a' && params->key[i] <= 'z') ||
              (params->key[i] >= 'A' && params->key[i] <= 'Z') ||
              (params->key[i] >= '0' && params->key[i] <= '9') ||
              params->key[i] == '-' || params->key[i] == '_' || params->key[i] == '.')) goto done;
    params->key[response[2]] = 0;
    TRACE( "got a %u-character Store ID key.\n", response[2] );
    status = STATUS_SUCCESS;
done:
    if (status) params->key[0] = 0;
    close( fd );
    return status;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    xsts_token,
    license_token,
    store_catalog,
    account_roles,
    store_entitlement,
    store_id_key,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );
