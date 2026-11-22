/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XStore
 *
 * Copyright 2026 Olivia Ryan
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "private.h"
#include "unixlib.h"
#include <math.h>

WINE_DEFAULT_DEBUG_CHANNEL(xstore);
WINE_DECLARE_DEBUG_CHANNEL(storetrace);
static char q_empty[] = "";

/* Legacy titles retain their existing offline license/entitlement behavior.
 * XGR_STORE_CATALOG=1 opts a title into live read-only product queries through
 * the credential service's separately versioned catalog protocol. Catalog
 * results describe what can be listed; they never grant add-on ownership.
 * License-token requests keep their released authenticated implementation. */

struct x_store_context
{
    XUserHandle user;
};

struct x_store_license
{
    BOOL valid;
    BOOL verified;
    char product[13];
};

const char *xgr_store_license_product( void *handle )
{
    struct x_store_license *license = handle;
    return license ? license->product : NULL;
}

/* Legacy current-game data and opt-in immutable catalog snapshots share the
 * query handle. Pages retain the snapshot independently of earlier handles. */
struct x_store_product_query
{
    BOOL has_current;
    struct store_catalog_snapshot *catalog;
    UINT32 start, page_size;
    XStoreProduct product;
    char store_id[64], title[256], description[512], language[32], empty[1];
};

static void init_current_product( struct x_store_product_query *q )
{
    if (!xgr_config_element( "StoreId", q->store_id, sizeof(q->store_id) )) q->store_id[0] = 0;
    if (!xgr_config_attr( "<ShellVisuals", " DefaultDisplayName=\"", q->title, sizeof(q->title) ) || !q->title[0])
        strcpy( q->title, "Game" );
    if (!xgr_config_attr( "<ShellVisuals", " Description=\"", q->description, sizeof(q->description) ) || !q->description[0])
        strcpy( q->description, q->title );
    strcpy( q->language, "en" );
    q->product.storeId = q->store_id;
    q->product.title = q->title;
    q->product.description = q->description;
    q->product.language = q->language;
    q->product.inAppOfferToken = q->empty;
    q->product.linkUri = q->empty;
    q->product.price.currencyCode = q->empty;
    q->product.productKind = XStoreProductKind_Game;
    q->product.hasDigitalDownload = TRUE;
    q->product.isInUserCollection = TRUE;
}

/* Fill an XStoreGameLicense.skuStoreId from MicrosoftGame.config's StoreId, or
 * a neutral placeholder. It only needs to be a stable, non-empty SKU string. */
static void store_sku_id( char *out, SIZE_T cap )
{
    if (!xgr_config_element( "StoreId", out, cap ) || !out[0])
        snprintf( out, cap, "%s", "0000000000000000" );
}

enum store_role_kind { ROLE_QUERY, ROLE_ENTITLED, ROLE_GAME_LICENSE, ROLE_ACQUIRE, ROLE_CAN_ACQUIRE };
static HRESULT role_store_begin_product( XAsyncBlock *, enum store_role_kind, const char * );
static HRESULT role_store_begin( XAsyncBlock *async, enum store_role_kind kind );
static HRESULT role_entitled_begin( XAsyncBlock *, XStoreProductKind, UINT32 );

static BOOL account_roles_enabled(void)
{
    char enabled[8];
    return GetEnvironmentVariableA("XGR_ACCOUNT_ROLES_V1",enabled,sizeof(enabled)) && !strcmp(enabled,"1");
}

/* Hand back a fresh product-query handle via an async block.
 *
 * `with_current` decides whether enumerating it reports the running game. A
 * query that should contain it and comes back empty is not a harmless blank:
 * a title asking what it is entitled to, or what its own product is, treats
 * "nothing" as an answer and carries on with it. Forza Horizon 5 enumerates
 * entitled products at its welcome screen and faults within a second of being
 * told it owns nothing. */
static HRESULT complete_query( XAsyncBlock *async, BOOL with_current )
{
    struct x_store_product_query *q;

    if (!async) return E_INVALIDARG;
    if (with_current && account_roles_enabled()) return role_store_begin(async,ROLE_QUERY);
    if (!(q = calloc( 1, sizeof(*q) )))
    {
        xgr_async_complete( async, E_OUTOFMEMORY, NULL );
        return S_OK;
    }
    q->has_current = with_current;
    if (with_current) init_current_product( q );
    xgr_async_complete( async, S_OK, q );
    return S_OK;
}

static HRESULT complete_empty_query( XAsyncBlock *async )
{
    return complete_query( async, FALSE );
}

/* Live catalog snapshots own every string/SKU/image until the last page is
 * closed. Each page independently retains the immutable snapshot. */
struct store_catalog_snapshot
{
    LONG ref;
    UINT32 count;
    XStoreProduct *products;
    char *strings, *next_string, *strings_end;
};
static const char store_catalog_identity;

static void store_catalog_release( struct store_catalog_snapshot *s )
{
    UINT32 i, j;
    if (!s || InterlockedDecrement( &s->ref )) return;
    for (i = 0; s->products && i < s->count; i++)
    {
        XStoreProduct *p = s->products + i;
        for (j = 0; p->skus && j < p->skusCount; j++) free( p->skus[j].availabilities );
        free( p->skus ); free( p->images );
    }
    free( s->products ); free( s->strings ); free( s );
}

struct catalog_reader { const unsigned char *p, *end; BOOL valid; };
static UINT32 catalog_read_u32( struct catalog_reader *r )
{
    UINT32 n = 0;
    if (r->end - r->p < 4) { r->valid = FALSE; return 0; }
    memcpy( &n, r->p, 4 ); r->p += 4; return n;
}
static char *catalog_read_text( struct catalog_reader *r, struct store_catalog_snapshot *s )
{
    UINT32 n = catalog_read_u32( r );
    char *out = s->next_string;
    if (!r->valid || n > 131072 || r->end - r->p < n || s->strings_end - out <= n || memchr(r->p,0,n))
    { r->valid = FALSE; return NULL; }
    memcpy( out, r->p, n ); out[n] = 0;
    r->p += n; s->next_string += n + 1; return out;
}
static void catalog_read_price( struct catalog_reader *r, struct store_catalog_snapshot *s, XStorePrice *price, time_t *end )
{
    char *currency;
    if (r->end - r->p < 8) { r->valid = FALSE; return; }
    memcpy( &price->basePrice, r->p, 4 ); memcpy( &price->price, r->p + 4, 4 ); r->p += 8;
    currency = catalog_read_text( r, s ); price->currencyCode = currency;
    if (r->end - r->p < 8 || !currency) { r->valid = FALSE; return; }
    memcpy( end, r->p, 8 ); r->p += 8;
    price->isOnSale = price->price < price->basePrice;
    price->saleEndDate = price->isOnSale ? *end : 0;
    snprintf( price->formattedBasePrice, sizeof(price->formattedBasePrice), "%.2f %.3s", price->basePrice, currency );
    snprintf( price->formattedPrice, sizeof(price->formattedPrice), "%.2f %.3s", price->price, currency );
}
static HRESULT catalog_decode( const void *wire, ULONG size, UINT32 page_size, struct x_store_product_query **result )
{
    struct catalog_reader r = { wire, (const unsigned char *)wire + size, TRUE };
    struct store_catalog_snapshot *s;
    struct x_store_product_query *q;
    UINT32 count, i, j, k;
    HRESULT hr;

    if (catalog_read_u32( &r ) != 1) return E_FAIL;
    hr = catalog_read_u32( &r ); count = catalog_read_u32( &r );
    if (!r.valid) return E_FAIL;
    if (FAILED(hr)) return hr;
    if (count > 2048) return E_FAIL;
    if (!(s = calloc( 1, sizeof(*s) ))) return E_OUTOFMEMORY;
    s->ref = 1; s->count = count;
    if (!(s->strings = malloc( (SIZE_T)size * 2 + 1 )) ||
        !(s->products = calloc( count ? count : 1, sizeof(*s->products) ))) goto oom;
    s->next_string = s->strings; s->strings_end = s->strings + (SIZE_T)size * 2 + 1;
    for (i = 0; i < count && r.valid; i++)
    {
        XStoreProduct *p = s->products + i;
        p->productKind = catalog_read_u32( &r );
        p->storeId = catalog_read_text( &r, s ); p->title = catalog_read_text( &r, s );
        p->description = catalog_read_text( &r, s ); p->language = catalog_read_text( &r, s );
        p->inAppOfferToken = catalog_read_text( &r, s ); p->linkUri = q_empty;
        p->skusCount = catalog_read_u32( &r );
        if (!r.valid || p->skusCount > 256) { p->skusCount = 0; goto malformed; }
        if (!(p->skus = calloc( p->skusCount ? p->skusCount : 1, sizeof(*p->skus) ))) { p->skusCount = 0; goto oom; }
        for (j = 0; j < p->skusCount && r.valid; j++)
        {
            XStoreSku *sku = p->skus + j;
            sku->skuId = catalog_read_text( &r, s ); sku->title = catalog_read_text( &r, s );
            sku->description = catalog_read_text( &r, s ); sku->language = catalog_read_text( &r, s );
            sku->isTrial = !!catalog_read_u32( &r ); p->hasDigitalDownload |= !!catalog_read_u32( &r );
            sku->collectionData.campaignId = q_empty; sku->collectionData.developerOfferId = p->inAppOfferToken;
            sku->availabilitiesCount = catalog_read_u32( &r );
            if (!r.valid || sku->availabilitiesCount > 256) { sku->availabilitiesCount = 0; goto malformed; }
            if (!(sku->availabilities = calloc( sku->availabilitiesCount ? sku->availabilitiesCount : 1, sizeof(*sku->availabilities) ))) goto oom;
            for (k = 0; k < sku->availabilitiesCount && r.valid; k++)
            {
                XStoreAvailability *a = sku->availabilities + k;
                a->availabilityId = catalog_read_text( &r, s );
                catalog_read_price( &r, s, &a->price, &a->endDate );
            }
            if (sku->availabilitiesCount) sku->price = sku->availabilities[0].price;
        }
        if (p->skusCount) p->price = p->skus[0].price;
        p->imagesCount = catalog_read_u32( &r );
        if (!r.valid || p->imagesCount > 256) { p->imagesCount = 0; goto malformed; }
        if (!(p->images = calloc( p->imagesCount ? p->imagesCount : 1, sizeof(*p->images) ))) goto oom;
        for (j = 0; j < p->imagesCount && r.valid; j++)
        {
            XStoreImage *image = p->images + j;
            image->uri = catalog_read_text( &r, s ); image->caption = catalog_read_text( &r, s );
            image->imagePurposeTag = catalog_read_text( &r, s );
            image->width = catalog_read_u32( &r ); image->height = catalog_read_u32( &r );
        }
    }
    if (!r.valid || r.p != r.end) goto malformed;
    if (!(q = calloc( 1, sizeof(*q) ))) goto oom;
    q->catalog = s; q->page_size = page_size; *result = q;
    TRACE_(storetrace)( "catalog snapshot: %u products, page size %u.\n", count, page_size );
    return S_OK;
oom:
    store_catalog_release( s ); return E_OUTOFMEMORY;
malformed:
    store_catalog_release( s ); return E_FAIL;
}

struct catalog_operation
{
    struct store_catalog_params ipc;
    unsigned char request[24000];
    struct x_store_product_query *result;
    UINT32 page_size;
};
static void catalog_operation_free( struct catalog_operation *op )
{
    if (op->result) { store_catalog_release( op->result->catalog ); free( op->result ); }
    free( op->ipc.response ); free( op );
}
static HRESULT CALLBACK catalog_provider( XAsyncOp operation, const XAsyncProviderData *data )
{
    struct catalog_operation *op = data->context;
    NTSTATUS status;
    HRESULT hr;
    switch (operation)
    {
    case XAsyncOp_Begin:
        return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );
    case XAsyncOp_DoWork:
        status = XGR_UNIX_CALL( store_catalog, &op->ipc );
        hr = status == STATUS_IO_TIMEOUT ? HRESULT_FROM_WIN32( ERROR_TIMEOUT ) : status ? E_FAIL :
             catalog_decode( op->ipc.response, op->ipc.response_size, op->page_size, &op->result );
        TRACE_(storetrace)( "catalog operation completed: hr %#lx.\n", hr );
        IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, hr, SUCCEEDED(hr) ? sizeof(op->result) : 0 );
        return E_PENDING;
    case XAsyncOp_GetResult:
        memcpy( data->buffer, &op->result, sizeof(op->result) ); op->result = NULL; return S_OK;
    case XAsyncOp_Cleanup:
        catalog_operation_free( op ); return S_OK;
    default:
        return S_OK;
    }
}
static BOOL catalog_enabled(void)
{
    char enabled[8];
    return GetEnvironmentVariableA( "XGR_STORE_CATALOG", enabled, sizeof(enabled) ) && !strcmp(enabled,"1");
}
static BOOL catalog_write_u32( struct catalog_operation *op, UINT32 n )
{
    if (op->ipc.request_size > sizeof(op->request) - 4) return FALSE;
    memcpy( op->request + op->ipc.request_size, &n, 4 ); op->ipc.request_size += 4; return TRUE;
}
static BOOL catalog_write_text( struct catalog_operation *op, const char *s )
{
    SIZE_T n = strnlen( s, 129 );
    if (n > 128 || !catalog_write_u32(op,n) || n > sizeof(op->request) - op->ipc.request_size) return FALSE;
    memcpy( op->request + op->ipc.request_size, s, n ); op->ipc.request_size += n; return TRUE;
}
static BOOL catalog_valid_id( const char *s )
{
    SIZE_T n, i;
    if (!s || ((n = strnlen(s,17)) != 12 && n != 16)) return FALSE;
    for (i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) return FALSE;
    return TRUE;
}
static HRESULT catalog_begin( const char *parent, XStoreProductKind kinds, BOOL associated,
                              UINT32 page_size, const char **ids, SIZE_T count,
                              const char **actions, SIZE_T action_count, XAsyncBlock *async )
{
    struct catalog_operation *op;
    WCHAR locale_w[LOCALE_NAME_MAX_LENGTH], market_w[8];
    char locale[32] = "en-US", market[8] = "US";
    SIZE_T i;
    HRESULT hr;
    if (!async || !catalog_valid_id(parent) || !kinds || (kinds & ~31) || !page_size || count > 256 ||
        action_count > 8 || (count && !ids) || (action_count && !actions) || (!associated && !count)) return E_INVALIDARG;
    for (i = 0; i < count; i++) if (!catalog_valid_id(ids[i])) return E_INVALIDARG;
    for (i = 0; i < action_count; i++) if (!actions[i]) return E_INVALIDARG;
    if (!(op = calloc(1,sizeof(*op)))) return E_OUTOFMEMORY;
    op->page_size = page_size; op->ipc.request = op->request;
    if (GetUserDefaultLocaleName(locale_w,ARRAY_SIZE(locale_w)))
        WideCharToMultiByte(CP_UTF8,0,locale_w,-1,locale,sizeof(locale),NULL,NULL);
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT,LOCALE_SISO3166CTRYNAME,market_w,ARRAY_SIZE(market_w)))
        WideCharToMultiByte(CP_UTF8,0,market_w,-1,market,sizeof(market),NULL,NULL);
    catalog_write_u32(op,1); catalog_write_u32(op,associated); catalog_write_u32(op,kinds);
    if (!catalog_write_text(op,parent) || !catalog_write_text(op,market) || !catalog_write_text(op,locale)) goto invalid;
    catalog_write_u32(op,count);
    for (i = 0; i < count; i++)
    {
        TRACE_(storetrace)( "requested Store ID %s.\n", debugstr_a(ids[i]) );
        if (!catalog_write_text(op,ids[i])) goto invalid;
    }
    catalog_write_u32(op,action_count);
    for (i = 0; i < action_count; i++) if (!catalog_write_text(op,actions[i])) goto invalid;
    if (!(op->ipc.response = malloc(XGR_CATALOG_MAX))) { catalog_operation_free(op); return E_OUTOFMEMORY; }
    hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, op, &store_catalog_identity, "StoreCatalog", catalog_provider );
    if (FAILED(hr)) catalog_operation_free(op);
    return hr;
invalid:
    catalog_operation_free(op); return E_INVALIDARG;
}

/* In profile mode every ownership result comes from the Store account's
 * authenticated content licence, and the slow request runs on the XAsync work
 * queue. The legacy running-title fallback stays outside this mode. */
static const char role_game_license_identity, role_acquire_identity, role_can_acquire_identity;
struct role_store_operation
{
    enum store_role_kind kind;
    XStoreProductKind kinds;
    UINT32 page_size;
    struct store_entitlement_params ipc;
    union
    {
        struct x_store_product_query *query;
        struct x_store_license *license;
        XStoreGameLicense game;
        XStoreCanAcquireLicenseResult can;
    } result;
};

static void role_store_free( struct role_store_operation *op )
{
    if ((op->kind == ROLE_QUERY || op->kind == ROLE_ENTITLED) && op->result.query)
    {
        store_catalog_release(op->result.query->catalog);
        free(op->result.query);
    }
    if (op->kind == ROLE_ACQUIRE) free(op->result.license);
    free(op);
}

/* Entitled content is filtered by the Store account's authenticated content
 * licences, never by installation alone. Retain an ordinary catalog snapshot
 * so page handles and callback data have the same lifetime rules. */
static HRESULT role_entitled_query( struct role_store_operation *op )
{
    const struct xgr_related_package *packages;
    struct x_store_product_query *q;
    struct store_catalog_snapshot *snapshot;
    struct store_entitlement_params ipc = {{0}};
    XStoreProduct *product;
    unsigned int i, count = xgr_related_packages( &packages );
    NTSTATUS status;

    if (!(q = calloc(1,sizeof(*q)))) return E_OUTOFMEMORY;
    op->result.query = q;
    if (!(snapshot = calloc(1,sizeof(*snapshot)))) return E_OUTOFMEMORY;
    snapshot->ref = 1;
    q->catalog = snapshot;
    q->page_size = op->page_size;
    if (!(snapshot->products = calloc(count + 1,sizeof(*snapshot->products))) ||
        !(snapshot->strings = calloc(1,sizeof(q->store_id) + sizeof(q->title) + sizeof(q->description))))
        return E_OUTOFMEMORY;
    if (op->ipc.active && (op->kinds & XStoreProductKind_Game))
    {
        init_current_product(q);
        product = snapshot->products + snapshot->count++;
        *product = q->product;
        memcpy(snapshot->strings,q->store_id,sizeof(q->store_id));
        memcpy(snapshot->strings + sizeof(q->store_id),q->title,sizeof(q->title));
        memcpy(snapshot->strings + sizeof(q->store_id) + sizeof(q->title),q->description,sizeof(q->description));
        product->storeId = snapshot->strings;
        product->title = snapshot->strings + sizeof(q->store_id);
        product->description = snapshot->strings + sizeof(q->store_id) + sizeof(q->title);
        product->language = "en";
        product->inAppOfferToken = product->linkUri = product->price.currencyCode = q_empty;
    }
    if (op->kinds & XStoreProductKind_Durable) for (i = 0; i < count; i++)
    {
        snprintf(ipc.store_id,sizeof(ipc.store_id),"%s",packages[i].store);
        status = XGR_UNIX_CALL(store_entitlement,&ipc);
        xgr_role_product_entitlement_store(ipc.store_id,status,ipc.active);
        if (status || !ipc.active) continue;
        product = snapshot->products + snapshot->count++;
        product->storeId = packages[i].store;
        product->title = packages[i].title;
        product->description = packages[i].description;
        product->language = "en";
        product->inAppOfferToken = product->linkUri = product->price.currencyCode = q_empty;
        product->productKind = XStoreProductKind_Durable;
        product->hasDigitalDownload = product->isInUserCollection = TRUE;
        TRACE("authenticated related entitlement %s\n",debugstr_a(product->storeId));
    }
    return S_OK;
}

static HRESULT CALLBACK role_store_provider( XAsyncOp operation, const XAsyncProviderData *data )
{
    struct role_store_operation *op = data->context;
    NTSTATUS status;
    SIZE_T size = 0;
    HRESULT hr;
    FILETIME time;
    ULONGLONG seconds;

    switch (operation)
    {
    case XAsyncOp_Begin:
        return IXThreadingImpl_XAsyncSchedule(x_threading_impl,data->async,0);
    case XAsyncOp_DoWork:
        status = XGR_UNIX_CALL(store_entitlement,&op->ipc);
        /* Every definitive answer refreshes what the cheap checks report. */
        xgr_role_product_entitlement_store(op->ipc.store_id,status,op->ipc.active);
        hr = status == STATUS_IO_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : status ? E_FAIL : S_OK;
        if (SUCCEEDED(hr)) switch (op->kind)
        {
        case ROLE_QUERY:
            if (!(op->result.query = calloc(1,sizeof(*op->result.query)))) { hr = E_OUTOFMEMORY; break; }
            op->result.query->has_current = op->ipc.active;
            if (op->ipc.active) init_current_product(op->result.query);
            size = sizeof(op->result.query);
            break;
        case ROLE_ENTITLED:
            hr = role_entitled_query(op);
            size = sizeof(op->result.query);
            break;
        case ROLE_GAME_LICENSE:
            op->result.game.isActive = op->ipc.active;
            store_sku_id(op->result.game.skuStoreId,sizeof(op->result.game.skuStoreId));
            GetSystemTimeAsFileTime(&time);
            seconds = (((ULONGLONG)time.dwHighDateTime << 32) | time.dwLowDateTime) / 10000000 - 11644473600ULL;
            op->result.game.expirationDate = seconds + 60;
            size = sizeof(op->result.game);
            break;
        case ROLE_ACQUIRE:
            if (!op->ipc.active) { hr = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED); break; }
            if (!(op->result.license = calloc(1,sizeof(*op->result.license)))) { hr = E_OUTOFMEMORY; break; }
            op->result.license->valid = TRUE;
            op->result.license->verified = TRUE;
            memcpy(op->result.license->product,op->ipc.store_id,13);
            size = sizeof(op->result.license);
            break;
        case ROLE_CAN_ACQUIRE:
            op->result.can.status = op->ipc.active ? XStoreCanLicenseStatus_Licensable : XStoreCanLicenseStatus_NotLicensableToUser;
            size = sizeof(op->result.can);
            break;
        }
        IXThreadingImpl_XAsyncComplete(x_threading_impl,data->async,hr,SUCCEEDED(hr) ? size : 0);
        return E_PENDING;
    case XAsyncOp_GetResult:
        switch (op->kind)
        {
        case ROLE_QUERY:
        case ROLE_ENTITLED:
            memcpy(data->buffer,&op->result.query,sizeof(op->result.query)); op->result.query = NULL; break;
        case ROLE_GAME_LICENSE:
            memcpy(data->buffer,&op->result.game,sizeof(op->result.game)); break;
        case ROLE_ACQUIRE:
            memcpy(data->buffer,&op->result.license,sizeof(op->result.license)); op->result.license = NULL; break;
        case ROLE_CAN_ACQUIRE:
            memcpy(data->buffer,&op->result.can,sizeof(op->result.can)); break;
        }
        return S_OK;
    case XAsyncOp_Cleanup:
        role_store_free(op); return S_OK;
    default:
        return S_OK;
    }
}

static HRESULT role_store_start( XAsyncBlock *async, enum store_role_kind kind, const char *product, XStoreProductKind kinds, UINT32 page_size )
{
    struct role_store_operation *op;
    const void *identity;
    HRESULT hr;

    if (!async) return E_INVALIDARG;
    if (!(op = calloc(1,sizeof(*op)))) return E_OUTOFMEMORY;
    op->kind = kind;
    op->kinds = kinds;
    op->page_size = page_size;
    if (product) snprintf(op->ipc.store_id,sizeof(op->ipc.store_id),"%s",product);
    else if (!xgr_config_element("StoreId",op->ipc.store_id,sizeof(op->ipc.store_id)))
    { free(op); return E_INVALIDARG; }
    switch (kind)
    {
    case ROLE_QUERY:
    case ROLE_ENTITLED: identity = &store_catalog_identity; break;
    case ROLE_GAME_LICENSE: identity = &role_game_license_identity; break;
    case ROLE_ACQUIRE: identity = &role_acquire_identity; break;
    default: identity = &role_can_acquire_identity; break;
    }
    hr = IXThreadingImpl_XAsyncBegin(x_threading_impl,async,op,identity,"StoreAccountLicense",role_store_provider);
    if (FAILED(hr)) role_store_free(op);
    return hr;
}

static HRESULT role_entitled_begin( XAsyncBlock *async, XStoreProductKind kinds, UINT32 page_size )
{
    if (!kinds || (kinds & ~31) || !page_size) return E_INVALIDARG;
    return role_store_start(async,ROLE_ENTITLED,NULL,kinds,page_size);
}

static HRESULT role_store_begin_product( XAsyncBlock *async, enum store_role_kind kind, const char *product )
{
    return role_store_start(async,kind,product,0,0);
}

static HRESULT role_store_begin( XAsyncBlock *async, enum store_role_kind kind )
{
    return role_store_begin_product( async, kind, NULL );
}

static BOOL current_package( const char *package )
{
    const char *current = xgr_current_package_identifier();
    return package && current && !strcmp(package,current);
}

static HRESULT query_result( XAsyncBlock *async, XStoreProductQueryHandle *out )
{
    void *payload;
    HRESULT hr;

    if (!out) return E_INVALIDARG;
    if (!xgr_async_completed( async, &hr, &payload )) return E_PENDING;
    if (SUCCEEDED(hr) && (SIZE_T)async->internal[3] == sizeof(*out))
        return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, &store_catalog_identity, sizeof(*out), out, NULL );
    *out = SUCCEEDED( hr ) ? payload : NULL;
    return hr;
}

struct x_store
{
    IXStoreImpl6 IXStoreImpl6_iface;
    LONG ref;
};

static inline struct x_store *impl_from_IXStoreImpl6( IXStoreImpl6 *iface )
{
    return CONTAINING_RECORD( iface, struct x_store, IXStoreImpl6_iface );
}

static HRESULT WINAPI x_store_QueryInterface( IXStoreImpl6 *iface, REFIID iid, void **out )
{
    struct x_store *impl = impl_from_IXStoreImpl6( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown     ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl  ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl2 ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl3 ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl4 ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl5 ) ||
        IsEqualGUID( iid, &IID_IXStoreImpl6 ))
    {
        IXStoreImpl6_AddRef( *out = &impl->IXStoreImpl6_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_store_AddRef( IXStoreImpl6 *iface )
{
    struct x_store *impl = impl_from_IXStoreImpl6( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_store_Release( IXStoreImpl6 *iface )
{
    struct x_store *impl = impl_from_IXStoreImpl6( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI x_store_XStoreCreateContext( IXStoreImpl6 *iface, const XUserHandle user, XStoreContextHandle *storeContextHandle )
{
    struct x_store_context *ctx;

    TRACE( "iface %p, user %p, storeContextHandle %p.\n", iface, user, storeContextHandle );

    if (!storeContextHandle) return E_INVALIDARG;
    if (!(ctx = calloc( 1, sizeof(*ctx) ))) return E_OUTOFMEMORY;
    ctx->user = user;
    *storeContextHandle = ctx;
    return S_OK;
}

static void WINAPI x_store_XStoreCloseContextHandle( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle )
{
    TRACE( "iface %p, storeContextHandle %p.\n", iface, storeContextHandle );
    xgr_role_watch_close(XGR_ROLE_WATCH_GAME,storeContextHandle);
    free( storeContextHandle );
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, UINT32 maxItemsToRetrievePerPage, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, productKinds %#x, async %p.\n", iface, storeContextHandle, productKinds, async );
    if (catalog_enabled())
    {
        char parent[64];
        if (!storeContextHandle || !xgr_config_element("StoreId",parent,sizeof(parent))) return E_INVALIDARG;
        return catalog_begin(parent,productKinds,TRUE,maxItemsToRetrievePerPage,NULL,0,NULL,0,async);
    }
    return complete_empty_query( async );
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    TRACE( "iface %p, async %p, productQueryHandle %p.\n", iface, async, productQueryHandle );
    return query_result( async, productQueryHandle );
}

static HRESULT WINAPI x_store_XStoreQueryProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, const char **storeIds, SIZE_T storeIdsCount, const char **actionFilters, SIZE_T actionFiltersCount, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, productKinds %#x, storeIdsCount %Iu, async %p.\n", iface, storeContextHandle, productKinds, storeIdsCount, async );
    if (catalog_enabled())
    {
        char parent[64];
        if (!storeContextHandle || !xgr_config_element("StoreId",parent,sizeof(parent))) return E_INVALIDARG;
        return catalog_begin(parent,productKinds,FALSE,256,storeIds,storeIdsCount,actionFilters,actionFiltersCount,async);
    }
    return complete_empty_query( async );
}

static HRESULT WINAPI x_store_XStoreQueryProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    TRACE( "iface %p, async %p, productQueryHandle %p.\n", iface, async, productQueryHandle );
    return query_result( async, productQueryHandle );
}

static HRESULT WINAPI x_store_XStoreQueryEntitledProductsAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, UINT32 maxItemsToRetrievePerPage, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, productKinds %#x, async %p.\n", iface, storeContextHandle, productKinds, async );
    if (account_roles_enabled()) return role_entitled_begin(async,productKinds,maxItemsToRetrievePerPage);
    return complete_query( async, !!(productKinds & XStoreProductKind_Game) );
}

static HRESULT WINAPI x_store_XStoreQueryEntitledProductsResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    TRACE( "iface %p, async %p, productQueryHandle %p.\n", iface, async, productQueryHandle );
    return query_result( async, productQueryHandle );
}

static HRESULT WINAPI x_store_XStoreQueryProductForCurrentGameAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, async %p.\n", iface, storeContextHandle, async );
    return complete_query( async, TRUE );
}

static HRESULT WINAPI x_store_XStoreQueryProductForCurrentGameResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    TRACE( "iface %p, async %p, productQueryHandle %p.\n", iface, async, productQueryHandle );
    return query_result( async, productQueryHandle );
}

static HRESULT WINAPI x_store_XStoreQueryProductForPackageAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XStoreProductKind productKinds, const char *packageIdentifier, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, productKinds %#x, packageIdentifier %s, async %p stub!\n", iface, storeContextHandle, productKinds, debugstr_a( packageIdentifier ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryProductForPackageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    FIXME( "iface %p, async %p, productQueryHandle %p stub!\n", iface, async, productQueryHandle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreEnumerateProductsQuery( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle, void *context, XStoreProductQueryCallback *callback )
{
    struct x_store_product_query *q = productQueryHandle;
    TRACE( "iface %p, productQueryHandle %p, context %p, callback %p.\n", iface, productQueryHandle, context, callback );

    if (!callback) return E_INVALIDARG;
    if (q && q->catalog)
    {
        UINT32 i, end = q->start + min(q->page_size,q->catalog->count - q->start);
        for (i = q->start; i < end; i++) if (!callback(q->catalog->products + i,context)) break;
        return S_OK;
    }
    if (!q || !q->has_current) return S_OK;

    /* The one product that can be described honestly: the game running now.
     * Its identity comes from MicrosoftGame.config, and isInUserCollection is
     * true because the person is playing it -- the image was decrypted with
     * their own licence to get this far. Nothing is claimed about add-ons;
     * those really are unknown offline, and saying otherwise would be worse
     * than saying nothing. */
    TRACE( "reporting the running game as a product: store %s, title %s (query-owned product)\n",
           debugstr_a( q->store_id ), debugstr_a( q->title ) );
    callback( &q->product, context );
    return S_OK;
}

static BOOLEAN WINAPI x_store_XStoreProductsQueryHasMorePages( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle )
{
    struct x_store_product_query *q = productQueryHandle;

    TRACE( "iface %p, productQueryHandle %p.\n", iface, productQueryHandle );
    return q && q->catalog && q->page_size < q->catalog->count - q->start;
}

static HRESULT WINAPI x_store_XStoreProductsQueryNextPageAsync( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle, XAsyncBlock *async )
{
    struct x_store_product_query *q = productQueryHandle, *next;

    TRACE( "iface %p, productQueryHandle %p, async %p.\n", iface, productQueryHandle, async );
    if (q && q->catalog)
    {
        if (!async) return E_INVALIDARG;
        if (!(next = calloc(1,sizeof(*next)))) return E_OUTOFMEMORY;
        next->catalog = q->catalog; InterlockedIncrement(&next->catalog->ref);
        next->page_size = q->page_size;
        next->start = q->start + min(q->page_size,q->catalog->count - q->start);
        xgr_async_complete(async,S_OK,next); return S_OK;
    }
    return complete_empty_query( async );
}

static HRESULT WINAPI x_store_XStoreProductsQueryNextPageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    TRACE( "iface %p, async %p, productQueryHandle %p.\n", iface, async, productQueryHandle );
    return query_result( async, productQueryHandle );
}

static void WINAPI x_store_XStoreCloseProductsQueryHandle( IXStoreImpl6 *iface, XStoreProductQueryHandle productQueryHandle )
{
    TRACE( "iface %p, productQueryHandle %p.\n", iface, productQueryHandle );
    if (productQueryHandle) store_catalog_release(((struct x_store_product_query *)productQueryHandle)->catalog);
    free( productQueryHandle );
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForPackageAsync( IXStoreImpl6 *iface, const XStoreProductQueryHandle productQueryHandle, const char *packageIdentifier, XAsyncBlock *async )
{
    struct x_store_license *lic;

    TRACE( "iface %p, productQueryHandle %p, packageIdentifier %s, async %p.\n", iface, productQueryHandle, debugstr_a( packageIdentifier ), async );
    if (account_roles_enabled())
    {
        const struct xgr_related_package *package = xgr_related_by_id(packageIdentifier);
        if (!package && !current_package(packageIdentifier)) return E_INVALIDARG;
        return role_store_begin_product(async,ROLE_ACQUIRE,package ? package->store : NULL);
    }
    if (!async) return E_INVALIDARG;
    if (!(lic = calloc( 1, sizeof(*lic) )))
    {
        xgr_async_complete( async, E_OUTOFMEMORY, NULL );
        return S_OK;
    }
    lic->valid = TRUE;
    xgr_async_complete( async, S_OK, lic );
    return S_OK;
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForPackageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreLicenseHandle *storeLicenseHandle )
{
    void *payload;
    HRESULT hr;

    TRACE( "iface %p, async %p, storeLicenseHandle %p.\n", iface, async, storeLicenseHandle );
    if (!storeLicenseHandle) return E_INVALIDARG;
    if (account_roles_enabled()) return IXThreadingImpl_XAsyncGetResult(x_threading_impl,async,
        &role_acquire_identity,sizeof(*storeLicenseHandle),storeLicenseHandle,NULL);
    if (!xgr_async_completed( async, &hr, &payload )) return E_PENDING;
    *storeLicenseHandle = SUCCEEDED( hr ) ? payload : NULL;
    return hr;
}

/* A local check, as on Windows: the entitlement is kept current off the
 * title's threads (xuser.c), and this reads it. Only a definitive answer moves
 * the handle, either way -- an unreachable service leaves it as it was rather
 * than invalidating it for the rest of the session, and a Store account that
 * signs back in makes it valid again. */
static BOOLEAN WINAPI x_store_XStoreIsLicenseValid( IXStoreImpl6 *iface, const XStoreLicenseHandle storeLicenseHandle )
{
    struct x_store_license *lic = storeLicenseHandle;
    UINT32 active;

    TRACE( "iface %p, storeLicenseHandle %p.\n", iface, storeLicenseHandle );
    if (lic && lic->verified)
    {
        if (xgr_role_product_entitlement(lic->product,&active, TRUE)) lic->valid = !!active;
        return lic->valid;
    }
    return lic && lic->valid;
}

static void WINAPI x_store_XStoreCloseLicenseHandle( IXStoreImpl6 *iface, XStoreLicenseHandle storeLicenseHandle )
{
    TRACE( "iface %p, storeLicenseHandle %p.\n", iface, storeLicenseHandle );
    xgr_role_watch_close(XGR_ROLE_WATCH_PACKAGE,storeLicenseHandle);
    free( storeLicenseHandle );
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForStoreIdAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeProductId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeProductId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeProductId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForStoreIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreCanAcquireLicenseResult *storeCanAcquireLicense )
{
    FIXME( "iface %p, async %p, storeCanAcquireLicense %p stub!\n", iface, async, storeCanAcquireLicense );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForPackageAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *packageIdentifier, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, packageIdentifier %s, async %p.\n", iface, storeContextHandle, debugstr_a( packageIdentifier ), async );
    if (account_roles_enabled())
    {
        const struct xgr_related_package *package = xgr_related_by_id(packageIdentifier);
        if (!package && !current_package(packageIdentifier)) return E_INVALIDARG;
        return role_store_begin_product(async,ROLE_CAN_ACQUIRE,package ? package->store : NULL);
    }
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, S_OK, NULL );
    return S_OK;
}

static HRESULT WINAPI x_store_XStoreCanAcquireLicenseForPackageResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreCanAcquireLicenseResult *storeCanAcquireLicense )
{
    HRESULT hr;

    TRACE( "iface %p, async %p, storeCanAcquireLicense %p.\n", iface, async, storeCanAcquireLicense );
    if (!storeCanAcquireLicense) return E_INVALIDARG;
    if (account_roles_enabled()) return IXThreadingImpl_XAsyncGetResult(x_threading_impl,async,
        &role_can_acquire_identity,sizeof(*storeCanAcquireLicense),storeCanAcquireLicense,NULL);
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    memset( storeCanAcquireLicense, 0, sizeof(*storeCanAcquireLicense) );
    storeCanAcquireLicense->status = XStoreCanLicenseStatus_Licensable;
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryGameLicenseAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, async %p.\n", iface, storeContextHandle, async );
    if (account_roles_enabled())
    {
        return role_store_begin(async,ROLE_GAME_LICENSE);
    }
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, S_OK, NULL );
    return S_OK;
}

static HRESULT WINAPI x_store_XStoreQueryGameLicenseResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreGameLicense *license )
{
    HRESULT hr;

    TRACE( "iface %p, async %p, license %p.\n", iface, async, license );

    if (!license) return E_INVALIDARG;
    if (account_roles_enabled()) return IXThreadingImpl_XAsyncGetResult(x_threading_impl,async,
        &role_game_license_identity,sizeof(*license),license,NULL);
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    if (FAILED( hr )) return hr;
    memset( license, 0, sizeof(*license) );
    license->isActive = TRUE;
    license->isTrial = FALSE;
    license->isTrialOwnedByThisUser = FALSE;
    license->isDiscLicense = FALSE;
    license->trialTimeRemainingInSeconds = 0;
    license->expirationDate = 0; /* perpetual */
    store_sku_id( license->skuStoreId, sizeof(license->skuStoreId) );
    return S_OK;
}

static HRESULT WINAPI x_store_XStoreQueryAddOnLicensesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, async %p.\n", iface, storeContextHandle, async );
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, S_OK, NULL );
    return S_OK;
}

static HRESULT WINAPI x_store_XStoreQueryAddOnLicensesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    HRESULT hr;

    TRACE( "iface %p, async %p, count %p.\n", iface, async, count );

    if (!count) return E_INVALIDARG;
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    *count = 0; /* no add-ons owned */
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryAddOnLicensesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, XStoreAddonLicense *addOnLicenses )
{
    HRESULT hr;

    TRACE( "iface %p, async %p, count %u, addOnLicenses %p.\n", iface, async, count, addOnLicenses );
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryConsumableBalanceRemainingAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeProductId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeProductId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeProductId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryConsumableBalanceRemainingResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreConsumableResult *consumableResult )
{
    FIXME( "iface %p, async %p, consumableResult %p stub!\n", iface, async, consumableResult );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreReportConsumableFulfillmentAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeProductId, UINT32 quantity, GUID trackingId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeProductId %s, quantity %u, trackingId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeProductId ), quantity, debugstr_guid( &trackingId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreReportConsumableFulfillmentResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreConsumableResult *consumableResult )
{
    FIXME( "iface %p, async %p, consumableResult %p stub!\n", iface, async, consumableResult );
    return E_NOTIMPL;
}

/* serviceTicket is a bearer credential from the title's own service, and
 * publisherUserId identifies the player to it: neither belongs in a log, which
 * gets attached to bug reports. Say whether they came and how long they are. */
static SIZE_T secret_length( const char *value )
{
    return value ? strnlen( value, 65536 ) : 0;
}

static const char collections_id_identity;

static void store_id_key_free( struct store_id_key_params *params )
{
    if (!params) return;
    free( (void *)params->service_ticket );
    free( (void *)params->publisher_user_id );
    SecureZeroMemory( params->key, sizeof(params->key) );
    free( params );
}

static HRESULT CALLBACK collections_id_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct store_id_key_params *params = data->context;
    NTSTATUS status;
    HRESULT hr;

    switch (op)
    {
    case XAsyncOp_Begin:
        return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );
    case XAsyncOp_DoWork:
        status = XGR_UNIX_CALL( store_id_key, params );
        hr = status == STATUS_IO_TIMEOUT ? HRESULT_FROM_WIN32( ERROR_TIMEOUT ) :
             status == STATUS_NOT_FOUND ? HRESULT_FROM_WIN32( ERROR_SERVICE_NOT_ACTIVE ) :
             status ? E_FAIL : S_OK;
        TRACE( "Collections ID key: status %#lx, hr %#lx, %Iu characters.\n", status, hr,
               SUCCEEDED(hr) ? strlen( params->key ) : 0 );
        IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, hr,
                                        SUCCEEDED(hr) ? strlen( params->key ) + 1 : 0 );
        /* Completion may release the context on another thread. */
        return E_PENDING;
    case XAsyncOp_GetResult:
        memcpy( data->buffer, params->key, strlen( params->key ) + 1 );
        return S_OK;
    case XAsyncOp_Cleanup:
        store_id_key_free( params );
        return S_OK;
    default:
        return S_OK;
    }
}

/* The title's own service supplies a ticket for the Store; the Store returns
 * the Collections ID key the title's service then uses to see what the
 * player owns. xodus-service makes that request for the Store account. */
static HRESULT WINAPI x_store_XStoreGetUserCollectionsIdAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *serviceTicket, const char *publisherUserId, XAsyncBlock *async )
{
    struct store_id_key_params *params;
    SIZE_T ticket_len, id_len;
    HRESULT hr;

    TRACE( "iface %p, storeContextHandle %p, serviceTicket %s (%Iu chars), publisherUserId %s (%Iu chars), async %p.\n",
           iface, storeContextHandle, serviceTicket ? "present" : "absent", secret_length( serviceTicket ),
           publisherUserId ? "present" : "absent", secret_length( publisherUserId ), async );
    if (!async || !serviceTicket) return E_INVALIDARG;
    ticket_len = strnlen( serviceTicket, 16385 );
    id_len = publisherUserId ? strnlen( publisherUserId, 1025 ) : 0;
    if (!ticket_len || ticket_len > 16384 || id_len > 1024) return E_INVALIDARG;
    if (!(params = calloc( 1, sizeof(*params) ))) return E_OUTOFMEMORY;
    if (!(params->service_ticket = strdup( serviceTicket )) ||
        !(params->publisher_user_id = strdup( publisherUserId ? publisherUserId : "" )))
    {
        store_id_key_free( params );
        return E_OUTOFMEMORY;
    }
    hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, params, &collections_id_identity,
                                     "XStoreGetUserCollectionsId", collections_id_provider );
    if (FAILED(hr)) store_id_key_free( params );
    return hr;
}

static HRESULT WINAPI x_store_XStoreGetUserCollectionsIdResultSize( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T *size )
{
    TRACE( "iface %p, async %p, size %p.\n", iface, async, size );
    return IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, size );
}

static HRESULT WINAPI x_store_XStoreGetUserCollectionsIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T size, char *result )
{
    TRACE( "iface %p, async %p, size %Iu, result %p.\n", iface, async, size, result );
    if (!result || !size) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, &collections_id_identity,
                                          size, result, NULL );
}

static HRESULT WINAPI x_store_XStoreGetUserPurchaseIdAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *serviceTicket, const char *publisherUserId, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, serviceTicket %s (%Iu chars), publisherUserId %s (%Iu chars), async %p.\n",
           iface, storeContextHandle, serviceTicket ? "present" : "absent", secret_length( serviceTicket ),
           publisherUserId ? "present" : "absent", secret_length( publisherUserId ), async );
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, S_OK, NULL );
    return S_OK;
}

static HRESULT WINAPI x_store_XStoreGetUserPurchaseIdResultSize( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T *size )
{
    HRESULT hr;

    TRACE( "iface %p, async %p, size %p.\n", iface, async, size );
    if (!size) return E_INVALIDARG;
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    *size = 1;
    return hr;
}

static HRESULT WINAPI x_store_XStoreGetUserPurchaseIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T size, char *result )
{
    HRESULT hr;

    TRACE( "iface %p, async %p, size %Iu, result %p.\n", iface, async, size, result );
    if (!result || !size) return E_INVALIDARG;
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    result[0] = 0;
    return hr;
}

static const char license_token_identity;

static void license_token_free( struct license_token_params *params )
{
    unsigned int i;
    for (i = 0; i < params->count; i++) free( (void *)params->ids[i] );
    free( params->ids );
    free( (void *)params->custom );
    free( params );
}

static HRESULT CALLBACK license_token_provider( XAsyncOp op, const XAsyncProviderData *data )
{
    struct license_token_params *params = data->context;
    NTSTATUS status;
    HRESULT hr;

    switch (op)
    {
    case XAsyncOp_Begin:
        return IXThreadingImpl_XAsyncSchedule( x_threading_impl, data->async, 0 );
    case XAsyncOp_DoWork:
        status = XGR_UNIX_CALL( license_token, params );
        hr = status == STATUS_IO_TIMEOUT ? HRESULT_FROM_WIN32( ERROR_TIMEOUT ) :
             status ? HRESULT_FROM_WIN32( ERROR_NOT_FOUND ) : S_OK;
        IXThreadingImpl_XAsyncComplete( x_threading_impl, data->async, hr,
                                        SUCCEEDED(hr) ? strlen(params->token) + 1 : 0 );
        /* Completion may release the context on another thread. */
        return E_PENDING;
    case XAsyncOp_GetResult:
        memcpy( data->buffer, params->token, strlen(params->token) + 1 );
        return S_OK;
    case XAsyncOp_Cleanup:
        license_token_free( params );
        return S_OK;
    default:
        return S_OK;
    }
}

static HRESULT WINAPI x_store_XStoreQueryLicenseTokenAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char **productIds, SIZE_T productIdsCount, const char *customDeveloperString, XAsyncBlock *async )
{
    struct license_token_params *params;
    SIZE_T i, j, size;
    HRESULT hr;

    TRACE( "iface %p, context %p, count %Iu, async %p.\n", iface, storeContextHandle, productIdsCount, async );
    if (!async || !storeContextHandle || !customDeveloperString || !customDeveloperString[0] ||
        strnlen( customDeveloperString, 16385 ) > 16384 || productIdsCount > 256 ||
        (productIdsCount && !productIds)) return E_INVALIDARG;
    for (i = 0; i < productIdsCount; i++)
    {
        if (!productIds[i] || !(size = strnlen(productIds[i], 65)) || size > 64) return E_INVALIDARG;
        for (j = 0; j < size; j++)
            if (!((productIds[i][j] >= '0' && productIds[i][j] <= '9') ||
                  (productIds[i][j] >= 'A' && productIds[i][j] <= 'Z') ||
                  (productIds[i][j] >= 'a' && productIds[i][j] <= 'z'))) return E_INVALIDARG;
    }
    if (!(params = calloc( 1, sizeof(*params) ))) return E_OUTOFMEMORY;
    if (!xgr_config_element( "StoreId", params->store_id, sizeof(params->store_id) ) || !params->store_id[0])
    { free( params ); return HRESULT_FROM_WIN32( ERROR_NOT_FOUND ); }
    xgr_title_identity( params->title_id, sizeof(params->title_id), params->title_client_id, sizeof(params->title_client_id) );
    if (!(params->custom = strdup( customDeveloperString ))) goto out_of_memory;
    if (productIdsCount)
    {
        if (!(params->ids = calloc( productIdsCount, sizeof(*params->ids) ))) goto out_of_memory;
        params->count = productIdsCount;
        for (i = 0; i < productIdsCount; i++)
            if (!(params->ids[i] = strdup( productIds[i] ))) goto out_of_memory;
    }
    hr = IXThreadingImpl_XAsyncBegin( x_threading_impl, async, params, &license_token_identity,
                                     "XStoreQueryLicenseToken", license_token_provider );
    if (FAILED(hr)) license_token_free( params );
    return hr;
out_of_memory:
    license_token_free( params );
    return E_OUTOFMEMORY;
}

static HRESULT WINAPI x_store_XStoreQueryLicenseTokenResultSize( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T *size )
{
    return IXThreadingImpl_XAsyncGetResultSize( x_threading_impl, async, size );
}

static HRESULT WINAPI x_store_XStoreQueryLicenseTokenResult( IXStoreImpl6 *iface, XAsyncBlock *async, SIZE_T size, char *result )
{
    if (!result || !size) return E_INVALIDARG;
    return IXThreadingImpl_XAsyncGetResult( x_threading_impl, async, &license_token_identity,
                                          size, result, NULL );
}

static HRESULT WINAPI __PADDING__( IXStoreImpl6 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_2__( IXStoreImpl6 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI __PADDING_3__( IXStoreImpl6 *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowPurchaseUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, const char *name, const char *extendedJsonData, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, name %s, extendedJsonData %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), debugstr_a( name ), debugstr_a( extendedJsonData ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowPurchaseUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowRateAndReviewUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, async %p stub!\n", iface, storeContextHandle, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowRateAndReviewUIResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreRateAndReviewResult *result )
{
    FIXME( "iface %p, async %p, result %p stub!\n", iface, async, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowRedeemTokenUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *token, const char **allowedStoreIds, SIZE_T allowedStoreIdsCount, BOOLEAN disallowCsvRedemption, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, token %s, allowedStoreIds %p, allowedStoreIdsCount %Iu, disallowCsvRedemption %d, async %p stub!\n", iface, storeContextHandle, debugstr_a( token ), allowedStoreIds, allowedStoreIdsCount, disallowCsvRedemption, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowRedeemTokenUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryGameAndDlcPackageUpdatesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, async %p.\n", iface, storeContextHandle, async );
    if (!async) return E_INVALIDARG;
    xgr_async_complete( async, S_OK, NULL );
    return S_OK;
}

static HRESULT WINAPI x_store_XStoreQueryGameAndDlcPackageUpdatesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    HRESULT hr;

    TRACE( "iface %p, async %p, count %p.\n", iface, async, count );
    if (!count) return E_INVALIDARG;
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    *count = 0; /* nothing to update */
    return hr;
}

static HRESULT WINAPI x_store_XStoreQueryGameAndDlcPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, XStorePackageUpdate *packageUpdates )
{
    HRESULT hr;

    TRACE( "iface %p, async %p, count %u, packageUpdates %p.\n", iface, async, count, packageUpdates );
    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    return hr;
}

static HRESULT WINAPI x_store_XStoreDownloadPackageUpdatesAsync( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, const char **packageIdentifiers, SIZE_T packageIdentifiersCount, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, packageIdentifiers %p, packageIdentifiersCount %Iu, async %p stub!\n", iface, storeContextHandle, packageIdentifiers, packageIdentifiersCount, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackageUpdatesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char **packageIdentifiers, SIZE_T packageIdentifiersCount, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, packageIdentifiers %p, packageIdentifiersCount %Iu, async %p stub!\n", iface, storeContextHandle, packageIdentifiers, packageIdentifiersCount, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackagesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char **storeIds, SIZE_T storeIdsCount, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeIds %p, storeIdsCount %Iu, async %p stub!\n", iface, storeContextHandle, storeIds, storeIdsCount, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackagesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    FIXME( "iface %p, async %p, count %p stub!\n", iface, async, count );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreDownloadAndInstallPackagesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, char **packageIdentifiers )
{
    FIXME( "iface %p, async %p, count %u, packageIdentifiers %p stub!\n", iface, async, count, packageIdentifiers );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryPackageIdentifier( IXStoreImpl6 *iface, const char *storeId, SIZE_T size, char *packageIdentifier )
{
    FIXME( "iface %p, storeId %s, size %Iu, packageIdentifier %p stub!\n", iface, debugstr_a( storeId ), size, packageIdentifier );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreRegisterGameLicenseChanged( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, XTaskQueueHandle queue, void *context, XStoreGameLicenseChangedCallback *callback, XTaskQueueRegistrationToken *token )
{
    char store_id[64];
    UINT32 active;

    TRACE( "iface %p, storeContextHandle %p, queue %p, context %p, callback %p, token %p.\n", iface, storeContextHandle, queue, context, callback, token );
    if (account_roles_enabled())
    {
        if (!storeContextHandle || !xgr_config_element("StoreId",store_id,sizeof(store_id))) return E_INVALIDARG;
        /* No request here: the watch takes the first answer as its baseline. */
        return xgr_role_watch_register(XGR_ROLE_WATCH_GAME,storeContextHandle,queue,context,NULL,callback,
            xgr_role_entitlement(&active, FALSE) ? (int)!!active : -1,token);
    }
    /* Legacy mode has no live Store state. */
    if (token) token->token = 1;
    return S_OK;
}

static BOOLEAN WINAPI x_store_XStoreUnregisterGameLicenseChanged( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "iface %p, storeContextHandle %p, wait %d.\n", iface, storeContextHandle, wait );
    if (account_roles_enabled()) return xgr_role_watch_unregister(XGR_ROLE_WATCH_GAME,storeContextHandle,token,wait);
    return TRUE;
}

static HRESULT WINAPI x_store_XStoreRegisterPackageLicenseLost( IXStoreImpl6 *iface, XStoreLicenseHandle storeLicenseHandle, XTaskQueueHandle queue, void *context, XStorePackageLicenseLostCallback *callback, XTaskQueueRegistrationToken *token)
{
    TRACE( "iface %p, storeLicenseHandle %p, queue %p, context %p, callback %p, token %p.\n", iface, storeLicenseHandle, queue, context, callback, token );
    if (account_roles_enabled())
    {
        struct x_store_license *license = storeLicenseHandle;
        if (!license || !license->verified) return E_INVALIDARG;
        return xgr_role_watch_register(XGR_ROLE_WATCH_PACKAGE,storeLicenseHandle,queue,context,NULL,callback,!!license->valid,token);
    }
    /* Legacy mode has no live Store state. */
    if (token) token->token = 1;
    return S_OK;
}

static BOOLEAN WINAPI x_store_XStoreUnregisterPackageLicenseLost( IXStoreImpl6 *iface, XStoreLicenseHandle licenseHandle, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "iface %p, licenseHandle %p, wait %d.\n", iface, licenseHandle, wait );
    if (account_roles_enabled()) return xgr_role_watch_unregister(XGR_ROLE_WATCH_PACKAGE,licenseHandle,token,wait);
    return TRUE;
}

static BOOLEAN WINAPI x_store_XStoreIsAvailabilityPurchasable( IXStoreImpl6 *iface, const XStoreAvailability availability )
{
    FIXME( "iface %p, availability %p stub!\n", iface, &availability );
    return FALSE;
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForDurablesAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreAcquireLicenseForDurablesResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreLicenseHandle *storeLicenseHandle )
{
    FIXME( "iface %p, async %p, storeLicenseHandle %p stub!\n", iface, async, storeLicenseHandle );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowAssociatedProductsUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, XStoreProductKind productKinds, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, productKinds %#x, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), productKinds, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowAssociatedProductsUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowProductPageUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowProductPageUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsForStoreIdAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, XStoreProductKind productKinds, UINT32 maxItemsToRetrievePerPage, XAsyncBlock *async )
{
    TRACE( "iface %p, storeContextHandle %p, storeId %s, async %p.\n", iface, storeContextHandle, debugstr_a( storeId ), async );
    if (catalog_enabled())
        return storeContextHandle ? catalog_begin(storeId,productKinds,TRUE,maxItemsToRetrievePerPage,NULL,0,NULL,0,async) : E_INVALIDARG;
    return complete_empty_query( async );
}

static HRESULT WINAPI x_store_XStoreQueryAssociatedProductsForStoreIdResult( IXStoreImpl6 *iface, XAsyncBlock *async, XStoreProductQueryHandle *productQueryHandle )
{
    TRACE( "iface %p, async %p, productQueryHandle %p.\n", iface, async, productQueryHandle );
    return query_result( async, productQueryHandle );
}

static HRESULT WINAPI x_store_XStoreQueryPackageUpdatesAsync( IXStoreImpl6 *iface, XStoreContextHandle storeContextHandle, const char **packageIdentifiers, SIZE_T packageIdentifiersCount, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, packageIdentifiers %p, packageIdentifiersCount %Iu, async %p stub!\n", iface, storeContextHandle, packageIdentifiers, packageIdentifiersCount, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryPackageUpdatesResultCount( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 *count )
{
    FIXME( "iface %p, async %p, count %p stub!\n", iface, async, count );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreQueryPackageUpdatesResult( IXStoreImpl6 *iface, XAsyncBlock *async, UINT32 count, XStorePackageUpdate *packageUpdates )
{
    FIXME( "iface %p, async %p, count %u, packageUpdates %p stub!\n", iface, async, count, packageUpdates );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowGiftingUIAsync( IXStoreImpl6 *iface, const XStoreContextHandle storeContextHandle, const char *storeId, const char *name, const char *extendedJsonData, XAsyncBlock *async )
{
    FIXME( "iface %p, storeContextHandle %p, storeId %s, name %s, extendedJsonData %s, async %p stub!\n", iface, storeContextHandle, debugstr_a( storeId ), debugstr_a( name ), debugstr_a( extendedJsonData ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_store_XStoreShowGiftingUIResult( IXStoreImpl6 *iface, XAsyncBlock *async )
{
    FIXME( "iface %p, async %p stub!\n", iface, async );
    return E_NOTIMPL;
}

static const struct IXStoreImpl6Vtbl x_store_vtbl =
{
    x_store_QueryInterface,
    x_store_AddRef,
    x_store_Release,
    /* IXStoreImpl methods */
    x_store_XStoreCreateContext,
    x_store_XStoreCloseContextHandle,
    x_store_XStoreQueryAssociatedProductsAsync,
    x_store_XStoreQueryAssociatedProductsResult,
    x_store_XStoreQueryProductsAsync,
    x_store_XStoreQueryProductsResult,
    x_store_XStoreQueryEntitledProductsAsync,
    x_store_XStoreQueryEntitledProductsResult,
    x_store_XStoreQueryProductForCurrentGameAsync,
    x_store_XStoreQueryProductForCurrentGameResult,
    x_store_XStoreQueryProductForPackageAsync,
    x_store_XStoreQueryProductForPackageResult,
    x_store_XStoreEnumerateProductsQuery,
    x_store_XStoreProductsQueryHasMorePages,
    x_store_XStoreProductsQueryNextPageAsync,
    x_store_XStoreProductsQueryNextPageResult,
    x_store_XStoreCloseProductsQueryHandle,
    x_store_XStoreAcquireLicenseForPackageAsync,
    x_store_XStoreAcquireLicenseForPackageResult,
    x_store_XStoreIsLicenseValid,
    x_store_XStoreCloseLicenseHandle,
    x_store_XStoreCanAcquireLicenseForStoreIdAsync,
    x_store_XStoreCanAcquireLicenseForStoreIdResult,
    x_store_XStoreCanAcquireLicenseForPackageAsync,
    x_store_XStoreCanAcquireLicenseForPackageResult,
    x_store_XStoreQueryGameLicenseAsync,
    x_store_XStoreQueryGameLicenseResult,
    x_store_XStoreQueryAddOnLicensesAsync,
    x_store_XStoreQueryAddOnLicensesResultCount,
    x_store_XStoreQueryAddOnLicensesResult,
    x_store_XStoreQueryConsumableBalanceRemainingAsync,
    x_store_XStoreQueryConsumableBalanceRemainingResult,
    x_store_XStoreReportConsumableFulfillmentAsync,
    x_store_XStoreReportConsumableFulfillmentResult,
    x_store_XStoreGetUserCollectionsIdAsync,
    x_store_XStoreGetUserCollectionsIdResultSize,
    x_store_XStoreGetUserCollectionsIdResult,
    x_store_XStoreGetUserPurchaseIdAsync,
    x_store_XStoreGetUserPurchaseIdResultSize,
    x_store_XStoreGetUserPurchaseIdResult,
    x_store_XStoreQueryLicenseTokenAsync,
    x_store_XStoreQueryLicenseTokenResultSize,
    x_store_XStoreQueryLicenseTokenResult,
    __PADDING__,
    __PADDING_2__,
    __PADDING_3__,
    x_store_XStoreShowPurchaseUIAsync,
    x_store_XStoreShowPurchaseUIResult,
    x_store_XStoreShowRateAndReviewUIAsync,
    x_store_XStoreShowRateAndReviewUIResult,
    x_store_XStoreShowRedeemTokenUIAsync,
    x_store_XStoreShowRedeemTokenUIResult,
    x_store_XStoreQueryGameAndDlcPackageUpdatesAsync,
    x_store_XStoreQueryGameAndDlcPackageUpdatesResultCount,
    x_store_XStoreQueryGameAndDlcPackageUpdatesResult,
    x_store_XStoreDownloadPackageUpdatesAsync,
    x_store_XStoreDownloadPackageUpdatesResult,
    x_store_XStoreDownloadAndInstallPackageUpdatesAsync,
    x_store_XStoreDownloadAndInstallPackageUpdatesResult,
    x_store_XStoreDownloadAndInstallPackagesAsync,
    x_store_XStoreDownloadAndInstallPackagesResultCount,
    x_store_XStoreDownloadAndInstallPackagesResult,
    x_store_XStoreQueryPackageIdentifier,
    x_store_XStoreRegisterGameLicenseChanged,
    x_store_XStoreUnregisterGameLicenseChanged,
    x_store_XStoreRegisterPackageLicenseLost,
    x_store_XStoreUnregisterPackageLicenseLost,
    /* IXStoreImpl2 methods */
    x_store_XStoreIsAvailabilityPurchasable,
    /* IXStoreImpl3 methods */
    x_store_XStoreAcquireLicenseForDurablesAsync,
    x_store_XStoreAcquireLicenseForDurablesResult,
    /* IXStoreImpl4 methods */
    x_store_XStoreShowAssociatedProductsUIAsync,
    x_store_XStoreShowAssociatedProductsUIResult,
    x_store_XStoreShowProductPageUIAsync,
    x_store_XStoreShowProductPageUIResult,
    /* IXStoreImpl5 methods */
    x_store_XStoreQueryAssociatedProductsForStoreIdAsync,
    x_store_XStoreQueryAssociatedProductsForStoreIdResult,
    x_store_XStoreQueryPackageUpdatesAsync,
    x_store_XStoreQueryPackageUpdatesResultCount,
    x_store_XStoreQueryPackageUpdatesResult,
    /* IXStoreImpl6 methods */
    x_store_XStoreShowGiftingUIAsync,
    x_store_XStoreShowGiftingUIResult,
};

static struct x_store x_store =
{
    {&x_store_vtbl},
    0,
};

IXStoreImpl *x_store_impl = (IXStoreImpl *)&x_store.IXStoreImpl6_iface;
