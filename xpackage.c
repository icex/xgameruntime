/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XPackage
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

/*
 * The package model, as seen by a single locally-installed title running
 * offline. There is exactly one package -- the game itself -- and it is fully
 * installed, so the questions this API answers have simple, honest answers:
 * the current package identifier is the game's, the mount path is its install
 * directory, enumeration returns that one package, and every chunk is Ready.
 * The download/uninstall entry points that only make sense against a live
 * Store succeed as no-ops rather than failing, because titles call them
 * defensively at startup and an E_NOTIMPL there stops them dead. This is what
 * UE5 GDK titles need in particular: they call XPackageGetMountPath during
 * initialisation and abort if it fails.
 */

#include "private.h"
#include <bcrypt.h>

WINE_DEFAULT_DEBUG_CHANNEL(xpackage);

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* Older GDKs use the same initial vtable under a different IID. Their
 * package operations occupy the reserved slots in IXPackageImpl. */
static const GUID legacy_package_iid = {0xbe3f3e9f, 0xc4b3, 0x424d,
                                       {0x9a, 0x95, 0xd1, 0xaf, 0x06, 0x05, 0x59, 0xa5}};

/* A mount handle just carries the filesystem path it resolves to. */
struct x_package_mount
{
    char *path;
};

/* An installation monitor over an already-installed package: nothing to track,
 * but it must be a distinct non-NULL handle the title can query and close. */
struct x_package_monitor
{
    LONG dummy;
};

struct x_package
{
    IXPackageImpl3 IXPackageImpl3_iface;
    LONG ref;
};

static inline struct x_package *impl_from_IXPackageImpl3( IXPackageImpl3 *iface )
{
    return CONTAINING_RECORD( iface, struct x_package, IXPackageImpl3_iface );
}

/* ---- helpers ------------------------------------------------------------ */

static char *wide_to_utf8( const WCHAR *str )
{
    char *ret;
    int len;

    if (!str) return NULL;
    if (!(len = WideCharToMultiByte( CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL ))) return NULL;
    if (!(ret = malloc( len ))) return NULL;
    WideCharToMultiByte( CP_UTF8, 0, str, -1, ret, len, NULL, NULL );
    return ret;
}

/* Wrap a filesystem path in a mount handle. Shared with XPersistentLocalStorage
 * so both hand back handles XPackageGetMountPath can read. */
void *xgr_make_mount( const char *path )
{
    struct x_package_mount *m;
    SIZE_T len;

    if (!path || !(m = calloc( 1, sizeof(*m) ))) return NULL;
    len = strlen( path ) + 1;
    if (!(m->path = malloc( len )))
    {
        free( m );
        return NULL;
    }
    memcpy( m->path, path, len );
    return m;
}

/* The current package's mount path: its install directory, as a DOS path. */
static char *current_mount_path( void )
{
    WCHAR *root = xgr_package_root();
    char *path;

    if (!root) return NULL;
    path = wide_to_utf8( root );
    free( root );
    return path;
}

/* MicrosoftGame.config text, loaded once (inline or from disk). Under an
 * InitOnce, so threads asking at the same time share one copy rather than
 * each loading their own. */
static BOOL WINAPI init_game_config( INIT_ONCE *once, void *param, void **context )
{
    *context = xgr_load_game_config();
    return TRUE;
}

static const char *game_config( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    void *config = NULL;

    InitOnceExecuteOnce( &once, init_game_config, NULL, &config );
    return config;
}

/* See private.h: the text of <element>text</element> in MicrosoftGame.config. */
BOOL xgr_config_element( const char *element, char *out, SIZE_T cap )
{
    const char *xml = game_config(), *value;
    char tag[64];
    SIZE_T len;

    out[0] = 0;
    if (!xml) return FALSE;
    snprintf( tag, sizeof(tag), "<%s>", element );
    if (!(value = strstr( xml, tag ))) return FALSE;
    value += strlen( tag );
    len = strcspn( value, "<" );
    if (len >= cap) return FALSE;
    memcpy( out, value, len );
    out[len] = 0;
    return TRUE;
}

/* See private.h: the title identity Xbox mints the XSTS title claim from. */
void xgr_title_identity( char *title_id, SIZE_T title_id_len, char *client_id, SIZE_T client_id_len )
{
    char hex[64];

    title_id[0] = client_id[0] = 0;
    /* The config spells TitleId in hexadecimal ("69C7AA31"); every Xbox service
     * and the token endpoints want it decimal, so convert here rather than
     * leaving each caller to guess which form it is holding. */
    if (xgr_config_element( "TitleId", hex, sizeof(hex) ) && hex[0])
        snprintf( title_id, title_id_len, "%lu", strtoul( hex, NULL, 16 ) );
    xgr_config_element( "MSAAppId", client_id, client_id_len );
}

/* See private.h: attr="value" from the first <element ...> in MicrosoftGame.config. */
BOOL xgr_config_attr( const char *element, const char *attr, char *out, SIZE_T cap )
{
    const char *xml = game_config(), *start, *end, *value;
    SIZE_T len;

    out[0] = 0;
    if (!xml || !(start = strstr( xml, element )) || !(end = strchr( start, '>' ))) return FALSE;
    if (!(value = strstr( start, attr )) || value > end) return FALSE;
    value += strlen( attr );
    len = strcspn( value, "\"" );
    if (len >= cap) return FALSE;
    memcpy( out, value, len );
    out[len] = 0;
    return TRUE;
}

/* Fill an XPackageDetails describing the current game package. The strings it
 * points at are owned by the caller-provided storage, valid for the callback. */
static void fill_current_details( XPackageDetails *details, const char *identifier, char *display,
                                  char *description, char *publisher, char *store_id,
                                  char *title_id )
{
    char version[64] = {0};

    memset( details, 0, sizeof(*details) );
    details->packageIdentifier = identifier;
    details->kind = XPackageKind_Game;
    details->displayName = display;
    details->description = description;
    details->publisher = publisher;
    details->storeId = store_id;
    details->titleID = title_id;
    details->installing = FALSE;
    details->ageRestricted = FALSE;
    details->index = 0;
    details->count = 1;

    if (xgr_config_attr( "<Identity", " Version=\"", version, sizeof(version) ))
    {
        unsigned int a = 0, b = 0, c = 0, d = 0;
        sscanf( version, "%u.%u.%u.%u", &a, &b, &c, &d );
        details->version.major = a;
        details->version.minor = b;
        details->version.build = c;
        details->version.revision = d;
    }
}

/* ---- QueryInterface / lifetime ------------------------------------------ */

static HRESULT WINAPI x_package_QueryInterface( IXPackageImpl3 *iface, REFIID iid, void **out )
{
    struct x_package *impl = impl_from_IXPackageImpl3( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown       ) ||
        IsEqualGUID( iid, &legacy_package_iid ) ||
        IsEqualGUID( iid, &IID_IXPackageImpl  ) ||
        IsEqualGUID( iid, &IID_IXPackageImpl2 ) ||
        IsEqualGUID( iid, &IID_IXPackageImpl3 ))
    {
        IXPackageImpl_AddRef( *out = &impl->IXPackageImpl3_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_package_AddRef( IXPackageImpl3 *iface )
{
    struct x_package *impl = impl_from_IXPackageImpl3( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_package_Release( IXPackageImpl3 *iface )
{
    struct x_package *impl = impl_from_IXPackageImpl3( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

/* ---- identity ----------------------------------------------------------- */

static char current_identifier[33];

static BOOL WINAPI init_package_identifier( INIT_ONCE *once, void *param, void **context )
{
    static const char hex[] = "0123456789abcdef";
    UCHAR bytes[16];
    unsigned int i;

    if (BCryptGenRandom( NULL, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG )) return FALSE;
    for (i = 0; i < sizeof(bytes); i++)
    {
        current_identifier[2 * i] = hex[bytes[i] >> 4];
        current_identifier[2 * i + 1] = hex[bytes[i] & 15];
    }
    TRACE( "created an opaque package identifier within the 33-byte GDK limit.\n" );
    return TRUE;
}

/* XPackage identifiers are opaque, per-session installation handles. The
 * package family names used for AppModel and save paths are a different API
 * and can exceed XPACKAGE_IDENTIFIER_MAX_LENGTH (33 including the NUL).
 * Share one identifier between the query and enumeration for this package. */
const char *xgr_current_package_identifier( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

    if (!InitOnceExecuteOnce( &once, init_package_identifier, NULL, NULL )) return NULL;
    return current_identifier;
}

static HRESULT WINAPI x_package_XPackageGetCurrentProcessPackageIdentifier( IXPackageImpl3 *iface, SIZE_T bufferSize, char *buffer )
{
    const char *id;
    SIZE_T len;

    TRACE( "iface %p, bufferSize %Iu, buffer %p.\n", iface, bufferSize, buffer );

    if (!buffer) return E_INVALIDARG;
    if (!(id = xgr_current_package_identifier())) return HRESULT_FROM_WIN32( ERROR_INSTALL_FAILURE );
    len = strlen( id ) + 1;
    if (bufferSize < len)
    {
        return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    }
    memcpy( buffer, id, len );
    return S_OK;
}

static BOOLEAN WINAPI x_package_XPackageIsPackagedProcess( IXPackageImpl3 *iface )
{
    TRACE( "iface %p.\n", iface );
    return TRUE;
}

static HRESULT WINAPI x_package_XPackageGetUserLocale( IXPackageImpl3 *iface, SIZE_T localeSize, char *locale )
{
    WCHAR buf[LOCALE_NAME_MAX_LENGTH];
    char out[LOCALE_NAME_MAX_LENGTH * 3];
    SIZE_T len;

    TRACE( "iface %p, localeSize %Iu, locale %p.\n", iface, localeSize, locale );

    if (!locale) return E_INVALIDARG;
    if (!GetUserDefaultLocaleName( buf, ARRAY_SIZE(buf) )) wcscpy( buf, L"en-US" );
    WideCharToMultiByte( CP_UTF8, 0, buf, -1, out, sizeof(out), NULL, NULL );
    len = strlen( out ) + 1;
    if (localeSize < len) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    memcpy( locale, out, len );
    return S_OK;
}

/* ---- installation state (everything is already installed) --------------- */

static HRESULT WINAPI x_package_XPackageCreateInstallationMonitor( IXPackageImpl3 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors, UINT32 minimumUpdateIntervalMs, XTaskQueueHandle queue, XPackageInstallationMonitorHandle *installationMonitor )
{
    struct x_package_monitor *monitor;

    TRACE( "iface %p, packageIdentifier %s, selectorCount %u, minimumUpdateIntervalMs %u, installationMonitor %p.\n",
           iface, debugstr_a( packageIdentifier ), selectorCount, minimumUpdateIntervalMs, installationMonitor );

    if (!installationMonitor) return E_INVALIDARG;
    if (!(monitor = calloc( 1, sizeof(*monitor) ))) return E_OUTOFMEMORY;
    *installationMonitor = (XPackageInstallationMonitorHandle)monitor;
    return S_OK;
}

static void WINAPI x_package_XPackageCloseInstallationMonitorHandle( IXPackageImpl3 *iface, XPackageInstallationMonitorHandle installationMonitor )
{
    TRACE( "iface %p, installationMonitor %p.\n", iface, installationMonitor );
    free( installationMonitor );
}

static void WINAPI x_package_XPackageGetInstallationProgress( IXPackageImpl3 *iface, XPackageInstallationMonitorHandle installationMonitor, XPackageInstallationProgress *progress )
{
    TRACE( "iface %p, installationMonitor %p, progress %p.\n", iface, installationMonitor, progress );

    if (!progress) return;
    /* fully installed: a non-zero, all-installed, launchable, completed state */
    progress->totalBytes = 1;
    progress->installedBytes = 1;
    progress->launchBytes = 0;
    progress->launchable = TRUE;
    progress->completed = TRUE;
}

static BOOLEAN WINAPI x_package_XPackageUpdateInstallationMonitor( IXPackageImpl3 *iface, XPackageInstallationMonitorHandle installationMonitor )
{
    TRACE( "iface %p, installationMonitor %p.\n", iface, installationMonitor );
    /* nothing changes: install is already complete */
    return FALSE;
}

static HRESULT WINAPI x_package_XPackageRegisterInstallationProgressChanged( IXPackageImpl3 *iface, XPackageInstallationMonitorHandle installationMonitor, void *context, XPackageInstallationProgressCallback *callback, XTaskQueueRegistrationToken *token )
{
    TRACE( "iface %p, installationMonitor %p, context %p, callback %p, token %p.\n", iface, installationMonitor, context, callback, token );

    /* nothing will ever change, so no callback is scheduled; hand back a token */
    if (token) token->token = 1;
    return S_OK;
}

static BOOLEAN WINAPI x_package_XPackageUnregisterInstallationProgressChanged( IXPackageImpl3 *iface, XPackageInstallationMonitorHandle installationMonitor, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "iface %p, installationMonitor %p, wait %d.\n", iface, installationMonitor, wait );
    return TRUE;
}

/* ---- chunks (all present) ----------------------------------------------- */

static HRESULT WINAPI x_package_XPackageFindChunkAvailability( IXPackageImpl3 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors, XPackageChunkAvailability *availability )
{
    TRACE( "iface %p, packageIdentifier %s, selectorCount %u, availability %p.\n", iface, debugstr_a( packageIdentifier ), selectorCount, availability );

    if (!availability) return E_INVALIDARG;
    *availability = XPackageChunkAvailability_Ready;
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageEnumerateChunkAvailability( IXPackageImpl3 *iface, const char *packageIdentifier, XPackageChunkSelectorType type, void *context, XPackageChunkAvailabilityCallback *callback )
{
    TRACE( "iface %p, packageIdentifier %s, type %d, context %p, callback %p.\n", iface, debugstr_a( packageIdentifier ), type, context, callback );
    /* no pending chunks to report; everything is already Ready */
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageChangeChunkInstallOrder( IXPackageImpl3 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors )
{
    TRACE( "iface %p, packageIdentifier %s, selectorCount %u.\n", iface, debugstr_a( packageIdentifier ), selectorCount );
    /* order is moot when nothing is queued for download */
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageInstallChunks( IXPackageImpl3 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors, UINT32 minimumUpdateIntervalMs, BOOLEAN suppressUserConfirmation, XTaskQueueHandle queue, XPackageInstallationMonitorHandle *installationMonitor )
{
    TRACE( "iface %p, packageIdentifier %s, selectorCount %u, installationMonitor %p.\n", iface, debugstr_a( packageIdentifier ), selectorCount, installationMonitor );
    /* nothing to install; hand back a completed monitor */
    return x_package_XPackageCreateInstallationMonitor( iface, packageIdentifier, selectorCount, selectors, minimumUpdateIntervalMs, queue, installationMonitor );
}

static HRESULT WINAPI x_package_XPackageInstallChunksAsync( IXPackageImpl3 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors, UINT32 minimumUpdateIntervalMs, BOOLEAN suppressUserConfirmation, XAsyncBlock *asyncBlock )
{
    struct x_package_monitor *monitor;

    TRACE( "iface %p, packageIdentifier %s, selectorCount %u, asyncBlock %p.\n", iface, debugstr_a( packageIdentifier ), selectorCount, asyncBlock );

    if (!asyncBlock) return E_INVALIDARG;
    monitor = calloc( 1, sizeof(*monitor) );
    xgr_async_complete( asyncBlock, monitor ? S_OK : E_OUTOFMEMORY, monitor );
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageInstallChunksResult( IXPackageImpl3 *iface, XAsyncBlock *asyncBlock, XPackageInstallationMonitorHandle *installationMonitor )
{
    void *payload;
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, installationMonitor %p.\n", iface, asyncBlock, installationMonitor );

    if (!installationMonitor) return E_INVALIDARG;
    if (!xgr_async_completed( asyncBlock, &hr, &payload )) return E_PENDING;
    *installationMonitor = SUCCEEDED( hr ) ? payload : NULL;
    return hr;
}

static HRESULT WINAPI x_package_XPackageEstimateDownloadSize( IXPackageImpl3 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors, UINT64 *downloadSize, BOOLEAN *shouldPresentUserConfirmation )
{
    TRACE( "iface %p, packageIdentifier %s, selectorCount %u, downloadSize %p.\n", iface, debugstr_a( packageIdentifier ), selectorCount, downloadSize );

    if (downloadSize) *downloadSize = 0;
    if (shouldPresentUserConfirmation) *shouldPresentUserConfirmation = FALSE;
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageUninstallChunks( IXPackageImpl3 *iface, const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors )
{
    TRACE( "iface %p, packageIdentifier %s, selectorCount %u.\n", iface, debugstr_a( packageIdentifier ), selectorCount );
    /* refuse to remove the content that is running the game -- but not an error */
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageRegisterPackageInstalledLegacy( IXPackageImpl3 *iface,
    XTaskQueueHandle queue, void *context, XPackageInstalledCallback *callback, XTaskQueueRegistrationToken *token )
{
    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );
    if (!token) return E_INVALIDARG;
    token->token = 0;
    if (!callback) return E_INVALIDARG;
    return E_NOTIMPL;
}

static BOOLEAN WINAPI x_package_XPackageUnregisterPackageInstalled( IXPackageImpl3 *iface, XTaskQueueRegistrationToken token, BOOLEAN wait )
{
    TRACE( "iface %p, wait %d.\n", iface, wait );
    return TRUE;
}

static HRESULT WINAPI x_package_XPackageMountLegacy( IXPackageImpl3 *iface, const char *identifier,
                                                     XPackageMountHandle *mount )
{
    TRACE( "iface %p, identifier %s, mount %p.\n", iface, debugstr_a( identifier ), mount );
    if (!mount) return E_INVALIDARG;
    *mount = NULL;
    if (!identifier || !*identifier) return E_INVALIDARG;
    /* This API requires the licensed content mount, beyond package discovery. */
    return E_NOTIMPL;
}

/* ---- mount (of the installed content) ----------------------------------- */

static HRESULT WINAPI x_package_XPackageGetMountPathSize( IXPackageImpl3 *iface, XPackageMountHandle mount, SIZE_T *pathSize )
{
    struct x_package_mount *m = (struct x_package_mount *)mount;

    TRACE( "iface %p, mount %p, pathSize %p.\n", iface, mount, pathSize );

    if (!m || !m->path || !pathSize) return E_INVALIDARG;
    *pathSize = strlen( m->path ) + 1;
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageGetMountPath( IXPackageImpl3 *iface, XPackageMountHandle mount, SIZE_T pathSize, char *path )
{
    struct x_package_mount *m = (struct x_package_mount *)mount;
    SIZE_T len;

    TRACE( "iface %p, mount %p, pathSize %Iu, path %p.\n", iface, mount, pathSize, path );

    if (!m || !m->path || !path) return E_INVALIDARG;
    len = strlen( m->path ) + 1;
    if (pathSize < len) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    memcpy( path, m->path, len );
    TRACE( "-> %s\n", debugstr_a( m->path ) );
    return S_OK;
}

static void WINAPI x_package_XPackageCloseMountHandle( IXPackageImpl3 *iface, XPackageMountHandle mount )
{
    struct x_package_mount *m = (struct x_package_mount *)mount;

    TRACE( "iface %p, mount %p.\n", iface, mount );
    if (!m) return;
    free( m->path );
    free( m );
}

static HRESULT WINAPI x_package_XPackageGetIdentifierLegacy( IXPackageImpl3 *iface, const char *store,
                                                              SIZE_T size, char *identifier )
{
    const struct xgr_related_package *packages;
    char current_store[64];
    unsigned int count, i;

    TRACE( "iface %p, store %s, size %Iu, identifier %p.\n", iface, debugstr_a( store ), size, identifier );
    if (!identifier || !size) return E_INVALIDARG;
    *identifier = 0;
    if (!store || !*store) return E_INVALIDARG;
    if (size < sizeof(current_identifier)) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );

    if (xgr_config_element( "StoreId", current_store, sizeof(current_store) ) && !stricmp( store, current_store ))
        return x_package_XPackageGetCurrentProcessPackageIdentifier( iface, size, identifier );

    count = xgr_related_packages( &packages );
    for (i = 0; i < count; ++i)
    {
        if (stricmp( store, packages[i].store )) continue;
        strcpy( identifier, packages[i].identifier );
        return S_OK;
    }
    return E_INVALIDARG;
}

/* ---- enumeration -------------------------------------------------------- */

/* The launcher supplies only installed content related to this game. Recheck
 * both manifest relationships before exposing any of it. This is installation
 * discovery, not a grant of ownership or a substitute for Store licensing. */
static BOOL xml_attr( const char *xml, const char *element, const char *attr, char *out, SIZE_T cap )
{
    const char *start, *end, *value;
    SIZE_T len;

    out[0] = 0;
    if (!(start = strstr( xml, element )) || !(end = strchr( start, '>' )) ||
        !(value = strstr( start, attr )) || value > end) return FALSE;
    value += strlen( attr );
    len = strcspn( value, "\"" );
    if (len >= cap) return FALSE;
    memcpy( out, value, len );
    out[len] = 0;
    return TRUE;
}

static BOOL xml_element( const char *xml, const char *element, char *out, SIZE_T cap )
{
    char tag[64];
    const char *value;
    SIZE_T len;

    out[0] = 0;
    snprintf( tag, sizeof(tag), "<%s>", element );
    if (!(value = strstr( xml, tag ))) return FALSE;
    value += strlen( tag );
    len = strcspn( value, "<" );
    if (len >= cap) return FALSE;
    memcpy( out, value, len );
    out[len] = 0;
    return TRUE;
}

static struct xgr_related_package related[128];
static unsigned int related_count;

static BOOL read_related( const WCHAR *unix_path, const char *allowed, const char *dependency )
{
    static const char hex[] = "0123456789abcdef";
    struct xgr_related_package *p = related + related_count;
    unsigned int i, a = 0, b = 0, c = 0, d = 0;
    char *xml, *root, version[64];
    WCHAR *file;
    HANDLE handle;
    DWORD size, read;
    UCHAR random[16];

    size = wcslen( unix_path );
    if (*unix_path != '/' || size > 32700 || !(file = calloc( size + 32, sizeof(WCHAR) ))) return FALSE;
    file[0] = 'Z';
    file[1] = ':';
    wcscpy( file + 2, unix_path );
    for (i = 2; file[i]; i++) if (file[i] == '/') file[i] = '\\';
    root = wide_to_utf8( file );
    wcscat( file, L"\\MicrosoftGame.config" );
    handle = CreateFileW( file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    free( file );
    if (!root || handle == INVALID_HANDLE_VALUE)
    {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle( handle );
        free( root );
        return FALSE;
    }
    size = GetFileSize( handle, NULL );
    if (!size || size > 1024 * 1024 || !(xml = calloc( size + 1, 1 )))
    {
        CloseHandle( handle );
        free( root );
        return FALSE;
    }
    if (!ReadFile( handle, xml, size, &read, NULL ) || read != size)
    {
        CloseHandle( handle );
        free( xml );
        free( root );
        return FALSE;
    }
    CloseHandle( handle );
    if (!strstr( xml, allowed ) || !strstr( xml, dependency )) goto invalid;
    xml_element( xml, "StoreId", p->store, sizeof(p->store) );
    if (strlen( p->store ) != 12 || BCryptGenRandom( NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG ))
        goto invalid;

    xml_attr( xml, "<ShellVisuals", " DefaultDisplayName=\"", p->title, sizeof(p->title) );
    xml_attr( xml, "<ShellVisuals", " Description=\"", p->description, sizeof(p->description) );
    xml_attr( xml, "<ShellVisuals", " PublisherDisplayName=\"", p->publisher, sizeof(p->publisher) );
    xml_attr( xml, "<Identity", " Version=\"", version, sizeof(version) );
    sscanf( version, "%u.%u.%u.%u", &a, &b, &c, &d );
    for (i = 0; i < sizeof(random); i++)
    {
        p->identifier[2 * i] = hex[random[i] >> 4];
        p->identifier[2 * i + 1] = hex[random[i] & 15];
    }
    p->path = root;
    p->details.packageIdentifier = p->identifier;
    p->details.kind = XPackageKind_Content;
    p->details.storeId = p->store;
    p->details.displayName = p->title;
    p->details.description = p->description;
    p->details.publisher = p->publisher;
    p->details.titleID = p->title_id;
    p->details.count = 1;
    p->details.version.major = a;
    p->details.version.minor = b;
    p->details.version.build = c;
    p->details.version.revision = d;
    free( xml );
    return TRUE;

invalid:
    free( xml );
    free( root );
    memset( p, 0, sizeof(*p) );
    return FALSE;
}

static BOOL WINAPI init_related( INIT_ONCE *once, void *param, void **context )
{
    WCHAR *paths, *at, *next;
    char allowed[96], dependency[512], current[64], identity[256];
    DWORD size;

    size = GetEnvironmentVariableW( L"XGR_RELATED_PACKAGE_PATHS", NULL, 0 );
    if (!size || size > 65536) return TRUE;
    if (!(paths = calloc( size, sizeof(WCHAR) ))) return FALSE;
    if (!GetEnvironmentVariableW( L"XGR_RELATED_PACKAGE_PATHS", paths, size ))
    {
        free( paths );
        return TRUE;
    }
    xgr_config_element( "StoreId", current, sizeof(current) );
    xgr_config_attr( "<Identity", " Name=\"", identity, sizeof(identity) );
    snprintf( allowed, sizeof(allowed), "<AllowedProduct>%s</AllowedProduct>", current );
    snprintf( dependency, sizeof(dependency), "<MainPackageDependency Name=\"%s\"", identity );
    for (at = paths; at && *at && related_count < ARRAY_SIZE(related); at = next)
    {
        next = wcschr( at, '\n' );
        if (next) *next++ = 0;
        if (read_related( at, allowed, dependency )) related_count++;
    }
    free( paths );
    return TRUE;
}

unsigned int xgr_related_packages( const struct xgr_related_package **packages )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

    InitOnceExecuteOnce( &once, init_related, NULL, NULL );
    *packages = related;
    return related_count;
}

const struct xgr_related_package *xgr_related_by_id( const char *id )
{
    const struct xgr_related_package *packages;
    unsigned int i, count = xgr_related_packages( &packages );

    for (i = 0; id && i < count; i++)
        if (!strcmp( id, packages[i].identifier )) return packages + i;
    return NULL;
}

static HRESULT enumerate_current( XPackageKind kind, XPackageEnumerationScope scope, void *context, XPackageEnumerationCallback *callback )
{
    XPackageDetails details;
    const char *identifier;
    char display[256], description[512], publisher[256], store_id[64], title_id[64];

    if (!callback) return E_INVALIDARG;
    if (kind == XPackageKind_Content)
    {
        const struct xgr_related_package *packages;
        unsigned int i, count;

        if (scope == XPackageEnumerationScope_ThisOnly) return S_OK;
        count = xgr_related_packages( &packages );
        for (i = 0; i < count; i++)
        {
            TRACE_(xpackage)( "reporting installed related package %s, store %s\n",
                   debugstr_a( packages[i].identifier ), debugstr_a( packages[i].store ) );
            if (!callback( context, &packages[i].details )) break;
        }
        return S_OK;
    }
    if (kind != XPackageKind_Game) return E_INVALIDARG;

    if (!(identifier = xgr_current_package_identifier())) return HRESULT_FROM_WIN32( ERROR_INSTALL_FAILURE );

    /* MicrosoftGame.config carries all of this. Reporting the package family
     * name as the display name and blanks for the rest is not harmless: a title
     * looking for itself in the list it just asked for will not recognise the
     * entry, and the ones that key on storeId find nothing at all. */
    if (!xgr_config_attr( "<ShellVisuals", " DefaultDisplayName=\"", display, sizeof(display) ) || !display[0])
        if (!xgr_config_attr( "<Identity", " Name=\"", display, sizeof(display) ))
            strcpy( display, "Game" );
    if (!xgr_config_attr( "<ShellVisuals", " Description=\"", description, sizeof(description) ) || !description[0])
        strcpy( description, display );
    if (!xgr_config_attr( "<ShellVisuals", " PublisherDisplayName=\"", publisher, sizeof(publisher) ))
        publisher[0] = 0;
    if (!xgr_config_element( "StoreId", store_id, sizeof(store_id) )) store_id[0] = 0;
    if (!xgr_config_element( "TitleId", title_id, sizeof(title_id) )) title_id[0] = 0;

    fill_current_details( &details, identifier, display, description, publisher, store_id, title_id );
    TRACE( "reporting package %s (%s), store %s, title %s\n", debugstr_a( identifier ),
           debugstr_a( display ), debugstr_a( store_id ), debugstr_a( title_id ) );
    callback( context, &details );
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageEnumeratePackages( IXPackageImpl3 *iface, XPackageKind kind, XPackageEnumerationScope scope, void *context, XPackageEnumerationCallback *callback )
{
    TRACE( "iface %p, kind %d, scope %d, context %p, callback %p.\n", iface, kind, scope, context, callback );
    return enumerate_current( kind, scope, context, callback );
}

static HRESULT WINAPI x_package_XPackageRegisterPackageInstalled( IXPackageImpl3 *iface, XTaskQueueHandle queue, void *context, XPackageInstalledCallback *callback, XTaskQueueRegistrationToken *token )
{
    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );
    /* the one package is already installed, so no install event ever fires */
    if (token) token->token = 1;
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageGetWriteStats( IXPackageImpl3 *iface, XPackageWriteStats *writeStats )
{
    TRACE( "iface %p, writeStats %p.\n", iface, writeStats );

    if (!writeStats) return E_INVALIDARG;
    memset( writeStats, 0, sizeof(*writeStats) );
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageUninstallUWPInstance( IXPackageImpl3 *iface, const char *packageName )
{
    TRACE( "iface %p, packageName %s.\n", iface, debugstr_a( packageName ) );
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageEnumerateFeatures( IXPackageImpl3 *iface, const char *packageIdentifier, void *context, XPackageFeatureEnumerationCallback *callback )
{
    TRACE( "iface %p, packageIdentifier %s, context %p, callback %p.\n", iface, debugstr_a( packageIdentifier ), context, callback );
    /* no optional features declared */
    return S_OK;
}

static BOOLEAN WINAPI x_package_XPackageUninstallPackage( IXPackageImpl3 *iface, const char *packageIdentifier )
{
    TRACE( "iface %p, packageIdentifier %s.\n", iface, debugstr_a( packageIdentifier ) );
    return FALSE;
}

/* ---- IXPackageImpl2 / Impl3 (same behaviour) ---------------------------- */

static HRESULT WINAPI x_package_XPackageEnumeratePackages2( IXPackageImpl3 *iface, XPackageKind kind, XPackageEnumerationScope scope, void *context, XPackageEnumerationCallback *callback )
{
    TRACE( "iface %p, kind %d, scope %d, context %p, callback %p.\n", iface, kind, scope, context, callback );
    return enumerate_current( kind, scope, context, callback );
}

static HRESULT WINAPI x_package_XPackageRegisterPackageInstalled2( IXPackageImpl3 *iface, XTaskQueueHandle queue, void *context, XPackageInstalledCallback *callback, XTaskQueueRegistrationToken *token )
{
    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );
    if (token) token->token = 1;
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageMountWithUiAsync( IXPackageImpl3 *iface, const char *packageIdentifier, XAsyncBlock *async )
{
    struct x_package_mount *mount;

    TRACE( "iface %p, packageIdentifier %s, async %p.\n", iface, debugstr_a( packageIdentifier ), async );

    if (!async) return E_INVALIDARG;
    if (!(mount = calloc( 1, sizeof(*mount) )))
    {
        xgr_async_complete( async, E_OUTOFMEMORY, NULL );
        return S_OK;
    }
    /* Enumerated content mounts its own installation; the current game keeps
     * the existing install-root mount behavior. */
    {
        const struct xgr_related_package *p = xgr_related_by_id( packageIdentifier );
        if (!(mount->path = p ? strdup( p->path ) : current_mount_path()))
        {
            free( mount );
            xgr_async_complete( async, HRESULT_FROM_WIN32( ERROR_INSTALL_FAILURE ), NULL );
            return S_OK;
        }
    }
    xgr_async_complete( async, S_OK, mount );
    return S_OK;
}

static HRESULT WINAPI x_package_XPackageMountWithUiResult( IXPackageImpl3 *iface, XAsyncBlock *async, XPackageMountHandle *mount )
{
    void *payload;
    HRESULT hr;

    TRACE( "iface %p, async %p, mount %p.\n", iface, async, mount );

    if (!mount) return E_INVALIDARG;
    if (!xgr_async_completed( async, &hr, &payload )) return E_PENDING;
    *mount = SUCCEEDED( hr ) ? (XPackageMountHandle)payload : NULL;
    return hr;
}

static HRESULT WINAPI x_package_XPackageEnumeratePackages3( IXPackageImpl3 *iface, XPackageKind kind, XPackageEnumerationScope scope, void *context, XPackageEnumerationCallback *callback )
{
    TRACE( "iface %p, kind %d, scope %d, context %p, callback %p.\n", iface, kind, scope, context, callback );
    return enumerate_current( kind, scope, context, callback );
}

static HRESULT WINAPI x_package_XPackageRegisterPackageInstalled3( IXPackageImpl3 *iface, XTaskQueueHandle queue, void *context, XPackageInstalledCallback *callback, XTaskQueueRegistrationToken *token )
{
    TRACE( "iface %p, queue %p, context %p, callback %p, token %p.\n", iface, queue, context, callback, token );
    if (token) token->token = 1;
    return S_OK;
}

static const struct IXPackageImpl3Vtbl x_package_vtbl =
{
    x_package_QueryInterface,
    x_package_AddRef,
    x_package_Release,
    /* IXPackageImpl methods */
    x_package_XPackageGetCurrentProcessPackageIdentifier,
    x_package_XPackageIsPackagedProcess,
    x_package_XPackageCreateInstallationMonitor,
    x_package_XPackageCloseInstallationMonitorHandle,
    x_package_XPackageGetInstallationProgress,
    x_package_XPackageUpdateInstallationMonitor,
    x_package_XPackageRegisterInstallationProgressChanged,
    x_package_XPackageUnregisterInstallationProgressChanged,
    x_package_XPackageGetUserLocale,
    x_package_XPackageFindChunkAvailability,
    x_package_XPackageEnumerateChunkAvailability,
    x_package_XPackageChangeChunkInstallOrder,
    x_package_XPackageInstallChunks,
    x_package_XPackageInstallChunksAsync,
    x_package_XPackageInstallChunksResult,
    x_package_XPackageEstimateDownloadSize,
    x_package_XPackageUninstallChunks,
    x_package_XPackageEnumeratePackages,
    x_package_XPackageRegisterPackageInstalledLegacy,
    x_package_XPackageUnregisterPackageInstalled,
    x_package_XPackageMountLegacy,
    x_package_XPackageGetMountPathSize,
    x_package_XPackageGetMountPath,
    x_package_XPackageCloseMountHandle,
    x_package_XPackageGetIdentifierLegacy,
    x_package_XPackageEnumeratePackages,
    x_package_XPackageRegisterPackageInstalled,
    x_package_XPackageGetWriteStats,
    x_package_XPackageEnumerateFeatures,
    x_package_XPackageUninstallUWPInstance,
    x_package_XPackageEnumerateFeatures,
    x_package_XPackageUninstallPackage,
    /* IXPackageImpl2 methods */
    x_package_XPackageEnumeratePackages2,
    x_package_XPackageRegisterPackageInstalled2,
    x_package_XPackageMountWithUiAsync,
    x_package_XPackageMountWithUiResult,
    /* IXPackageImpl3 methods */
    x_package_XPackageEnumeratePackages3,
    x_package_XPackageRegisterPackageInstalled3,
};

static struct x_package x_package =
{
    {&x_package_vtbl},
    0,
};

IXPackageImpl *x_package_impl = (IXPackageImpl *)&x_package.IXPackageImpl3_iface;
