/*
 * Unix-side interface for xgameruntime
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

/*
 * A real Xbox Live token can only come from the signed-in Microsoft account,
 * whose credentials live outside the prefix in xodus-service. That service
 * listens on a Unix socket, which a PE module cannot open, so the request goes
 * through this unixlib: the PE side asks for a token for a URL, the Unix side
 * talks to the service and hands back the finished
 * "XBL3.0 x=<user hash>;<token>" header plus the identity that came with it.
 *
 * Buffers are caller-allocated and fixed size to keep the boundary trivial.
 */

#ifndef __WINE_XGAMERUNTIME_UNIXLIB_H
#define __WINE_XGAMERUNTIME_UNIXLIB_H

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/unixlib.h"

#define XGR_TOKEN_MAX     4096
#define XGR_IDENTITY_MAX  64

#define XGR_SIGNATURE_MAX 512

/* Token request flags. FORCE_REFRESH asks the service to mint a new token
 * rather than answer from its cache -- what a title means by
 * XUserGetTokenAndSignatureOptions_ForceRefresh after a 401. It travels as
 * <ForceRefresh>true</ForceRefresh> in the request, and only when set, so a
 * service that does not know the element sees the request it always did. */
#define XGR_XSTS_FORCE_REFRESH 0x1

/* How long a whole token exchange may take, retries included, when the caller
 * does not say. The service allows itself 30 s per XSTS request and may retry
 * once without the title claim, so this sits just above that. */
#define XGR_XSTS_TIMEOUT_MS 65000

struct xsts_token_params
{
    const char *url;                        /* in:  the URL the title will call */
    const char *method;                     /* in:  its HTTP method */
    const void *body;                       /* in:  its body, may be NULL */
    ULONG body_size;                        /* in */
    const char *title_id;                   /* in:  decimal TitleId, may be NULL */
    const char *title_client_id;            /* in:  MSAAppId, may be NULL */
    UINT32 flags;                           /* in:  XGR_XSTS_* */
    UINT32 timeout_ms;                      /* in:  0 means XGR_XSTS_TIMEOUT_MS */
    char token[XGR_TOKEN_MAX];              /* out: "XBL3.0 x=<uhs>;<token>" */
    char signature[XGR_SIGNATURE_MAX];      /* out: Signature header, may be empty */
    char xuid[XGR_IDENTITY_MAX];            /* out: decimal XUID, may be empty */
    char gamertag[XGR_IDENTITY_MAX];        /* out: may be empty */
    INT64 expiry;                           /* out: unix time the token expires */
};

#define XGR_LICENSE_TOKEN_MAX 60001
struct license_token_params
{
    const char **ids;
    unsigned int count;
    const char *custom;
    char store_id[64], title_id[64], title_client_id[64];
    char token[XGR_LICENSE_TOKEN_MAX];
};

/* XStoreGetUserCollectionsId: the title service's ticket and publisher user
 * ID in, the Store's Collections ID key out (empty on failure). */
#define XGR_STORE_ID_KEY_MAX 60001
struct store_id_key_params
{
    const char *service_ticket;
    const char *publisher_user_id;
    char key[XGR_STORE_ID_KEY_MAX];
};

#define XGR_CATALOG_MAX (8 * 1024 * 1024)
struct store_catalog_params
{
    const void *request;
    ULONG request_size;
    void *response;
    ULONG response_size;
};

/* account_roles and store_entitlement answer STATUS_SUCCESS with a definitive
 * state, STATUS_NOT_SUPPORTED when the service replied in a version this side
 * cannot read (also definitive: nothing can be verified, so callers fail
 * closed), and any other status when the exchange itself failed -- which says
 * nothing about the account and must not be read as a sign-out. */
struct account_roles_params
{
    UINT32 store_signed_in, player_signed_in, same_store_user;
};

struct store_entitlement_params
{
    char store_id[64];
    UINT32 active;
};

enum xgr_funcs
{
    unix_xsts_token,
    unix_license_token,
    unix_store_catalog,
    unix_account_roles,
    unix_store_entitlement,
    unix_store_id_key,
    unix_funcs_count,
};

#define XGR_UNIX_CALL( func, params ) WINE_UNIX_CALL( unix_ ## func, params )

#endif /* __WINE_XGAMERUNTIME_UNIXLIB_H */
