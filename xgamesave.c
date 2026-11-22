/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XGameSave and XGameSaveFiles
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
 * Connected storage, kept in the same on-disk layout the Windows Gaming
 * Services use, so a save folder copied over from a Windows install loads
 * as-is and anything written here can be copied back.
 *
 *   <root>\<XUID>_<SCID>\containers.index          list of containers
 *   <root>\<XUID>_<SCID>\<FOLDER>\container.<N>    blob manifest of one container
 *   <root>\<XUID>_<SCID>\<FOLDER>\<FILE>           one blob
 *
 * <root> is %LOCALAPPDATA%\Packages\<PackageFamilyName>\SystemAppData\wgs,
 * with the family name derived from MicrosoftGame.config the way Windows does
 * it; XGR_WGS_ROOT overrides it. <FOLDER> and <FILE> are GUIDs written as
 * 32 uppercase hex digits. Strings in the index are length-prefixed UTF-16,
 * times are FILETIMEs. The manifest is a fixed-size table of blob names.
 *
 * Every operation re-reads the index, so the file on disk is the only state
 * that matters and several providers can coexist in one process.
 */

#include "private.h"
#include "pfxgamesave.h"

#include <objbase.h>
#include <bcrypt.h>

#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define WGS_INDEX_VERSION       14
#define WGS_MANIFEST_VERSION    4
#define WGS_BLOB_NAME_CHARS     64
#define WGS_DEFAULT_QUOTA       (256ull << 20)
#define GS_MAX_UPDATE_SIZE      (16ull << 20)
#define TICKS_1601_TO_0001      504911232000000000ull
#define TICKS_1601_TO_1970      116444736000000000ull

struct wgs_blob
{
    struct list entry;
    WCHAR name[WGS_BLOB_NAME_CHARS];
    GUID file;
    GUID file_alt;
    UINT64 size;
};

struct wgs_container
{
    struct list entry;
    WCHAR *name;
    WCHAR *display_name;
    WCHAR *etag;
    BYTE number;        /* the N of container.<N> */
    UINT32 flags;
    GUID folder;
    UINT64 mtime;
    UINT64 reserved;
    UINT64 size;
};

struct wgs_index
{
    UINT32 version;
    UINT32 reserved0;
    WCHAR *aumid;
    UINT64 mtime;
    UINT32 reserved1;
    WCHAR *id;
    UINT64 quota;
    struct list containers;
};

struct XGameSaveProvider
{
    LONG ref;
    XGameSaveProviderHandle result_handle;
    WCHAR *dir;
    BOOL read_only;         /* set when the index could not be parsed */
    struct wgs_index index;
};

struct x_string_entry
{
    struct list entry;
    char str[1];
};

struct XGameSaveContainer
{
    LONG ref;               /* the title's handle, and each update or read on it */
    struct XGameSaveProvider *provider;
    WCHAR *name;
    struct list strings;    /* blob names handed out to the title */
};

struct wgs_op
{
    struct list entry;
    BOOL is_delete;
    WCHAR name[WGS_BLOB_NAME_CHARS];
    GUID file;
    BYTE *data;
    SIZE_T size;
};

struct XGameSaveUpdate
{
    LONG ref;               /* the title's handle, and a submit in flight */
    struct XGameSaveContainer *container;
    WCHAR *display_name;
    struct list ops;
    SIZE_T total_size;
};

struct read_request
{
    struct XGameSaveContainer *container;
    UINT32 count;
    char **names;
};

struct old_file
{
    struct list entry;
    GUID file;
};

static CRITICAL_SECTION gs_cs;
static CRITICAL_SECTION_DEBUG gs_cs_debug =
{
    0, 0, &gs_cs,
    { &gs_cs_debug.ProcessLocksList, &gs_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": gs_cs") }
};
static CRITICAL_SECTION gs_cs = { &gs_cs_debug, -1, 0, 0, 0, 0 };

/* ---- strings ------------------------------------------------------------ */

static WCHAR *utf8_to_wide( const char *str )
{
    WCHAR *ret;
    int len;

    if (!str) return NULL;
    if (!(len = MultiByteToWideChar( CP_UTF8, 0, str, -1, NULL, 0 ))) return NULL;
    if (!(ret = malloc( len * sizeof(WCHAR) ))) return NULL;
    MultiByteToWideChar( CP_UTF8, 0, str, -1, ret, len );
    return ret;
}

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

static WCHAR *wide_dup( const WCHAR *str )
{
    WCHAR *ret;
    SIZE_T size;

    if (!str) return NULL;
    size = (wcslen( str ) + 1) * sizeof(WCHAR);
    if ((ret = malloc( size ))) memcpy( ret, str, size );
    return ret;
}

static char *narrow_dup( const char *str )
{
    char *ret;
    SIZE_T size;

    if (!str) return NULL;
    size = strlen( str ) + 1;
    if ((ret = malloc( size ))) memcpy( ret, str, size );
    return ret;
}

static WCHAR *path_join( const WCHAR *dir, const WCHAR *leaf )
{
    WCHAR *ret;
    SIZE_T len = wcslen( dir ) + 1 + wcslen( leaf ) + 1;

    if (!(ret = malloc( len * sizeof(WCHAR) ))) return NULL;
    swprintf( ret, len, L"%s\\%s", dir, leaf );
    return ret;
}

/* 32 uppercase hex digits, the way the folder and blob files are named */
static void guid_to_name( const GUID *guid, WCHAR name[33] )
{
    swprintf( name, 33, L"%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
              (unsigned int)guid->Data1, guid->Data2, guid->Data3,
              guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
              guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7] );
}

static void guid_to_string( const GUID *guid, WCHAR str[37] )
{
    swprintf( str, 37, L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
              (unsigned int)guid->Data1, guid->Data2, guid->Data3,
              guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
              guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7] );
}

static UINT64 filetime_now( void )
{
    FILETIME ft;
    GetSystemTimeAsFileTime( &ft );
    return ((UINT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

/* ---- files -------------------------------------------------------------- */

/* On failure the last error says why, and only a file that is not there
 * reports ERROR_FILE_NOT_FOUND or ERROR_PATH_NOT_FOUND: callers treat those
 * two as "nothing saved yet", which an unreadable file must never pass for. */
static BOOL read_whole_file( const WCHAR *path, BYTE **data, SIZE_T *size )
{
    HANDLE file;
    LARGE_INTEGER len;
    DWORD read, total = 0, error;
    BYTE *buf = NULL;

    *data = NULL;
    *size = 0;
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (!GetFileSizeEx( file, &len )) error = GetLastError();
    else if (len.QuadPart > 0x7fffffff) error = ERROR_FILE_TOO_LARGE;
    else if (!(buf = malloc( len.QuadPart + 1 ))) error = ERROR_NOT_ENOUGH_MEMORY;
    else error = ERROR_SUCCESS;
    if (error)
    {
        CloseHandle( file );
        SetLastError( error );
        return FALSE;
    }
    while (total < len.QuadPart)
    {
        if (!ReadFile( file, buf + total, len.QuadPart - total, &read, NULL )) break;
        if (!read)
        {
            SetLastError( ERROR_HANDLE_EOF );
            break;
        }
        total += read;
    }
    error = GetLastError();
    CloseHandle( file );
    if (total != len.QuadPart)
    {
        free( buf );
        SetLastError( error );
        return FALSE;
    }
    buf[total] = 0;
    *data = buf;
    *size = total;
    return TRUE;
}

static BOOL write_whole_file( const WCHAR *path, const BYTE *data, SIZE_T size )
{
    HANDLE file;
    DWORD written;
    SIZE_T total = 0;
    BOOL ok = TRUE;

    file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    while (ok && total < size)
    {
        ok = WriteFile( file, data + total, size - total, &written, NULL );
        total += written;
    }
    if (ok) ok = FlushFileBuffers( file );
    CloseHandle( file );
    if (!ok) DeleteFileW( path );
    return ok;
}

/* Replace a file without ever leaving a half-written one in its place. */
static BOOL write_file_atomic( const WCHAR *path, const BYTE *data, SIZE_T size )
{
    WCHAR *tmp;
    SIZE_T len = wcslen( path ) + 5;
    BOOL ok;

    if (!(tmp = malloc( len * sizeof(WCHAR) ))) return FALSE;
    swprintf( tmp, len, L"%s.tmp", path );
    ok = write_whole_file( tmp, data, size ) && MoveFileExW( tmp, path, MOVEFILE_REPLACE_EXISTING );
    if (!ok) DeleteFileW( tmp );
    free( tmp );
    return ok;
}

static BOOL file_size( const WCHAR *path, UINT64 *size )
{
    WIN32_FILE_ATTRIBUTE_DATA info;

    if (!GetFileAttributesExW( path, GetFileExInfoStandard, &info )) return FALSE;
    *size = ((UINT64)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    return TRUE;
}

static BOOL make_directory( const WCHAR *path )
{
    WCHAR *copy, *p, *start;
    BOOL ok = TRUE;

    if (!(copy = wide_dup( path ))) return FALSE;
    start = copy;
    if (start[0] && start[1] == ':') start += 2;
    while (*start == '\\') start++;
    for (p = start; ok && *p; p++)
    {
        if (*p != '\\') continue;
        *p = 0;
        if (!CreateDirectoryW( copy, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS) ok = FALSE;
        *p = '\\';
    }
    if (ok && !CreateDirectoryW( copy, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS) ok = FALSE;
    free( copy );
    return ok;
}

/* ---- binary reader / writer --------------------------------------------- */

struct reader
{
    const BYTE *p;
    const BYTE *end;
    BOOL ok;
};

static BOOL rd_bytes( struct reader *r, void *out, SIZE_T len )
{
    if (!r->ok || (SIZE_T)(r->end - r->p) < len)
    {
        r->ok = FALSE;
        return FALSE;
    }
    memcpy( out, r->p, len );
    r->p += len;
    return TRUE;
}

static UINT32 rd_u32( struct reader *r )
{
    UINT32 v = 0;
    rd_bytes( r, &v, sizeof(v) );
    return v;
}

static UINT64 rd_u64( struct reader *r )
{
    UINT64 v = 0;
    rd_bytes( r, &v, sizeof(v) );
    return v;
}

static BYTE rd_u8( struct reader *r )
{
    BYTE v = 0;
    rd_bytes( r, &v, sizeof(v) );
    return v;
}

static WCHAR *rd_wstr( struct reader *r )
{
    UINT32 len = rd_u32( r );
    WCHAR *str;

    if (!r->ok || len > 0x10000 || (SIZE_T)(r->end - r->p) < len * sizeof(WCHAR) ||
        !(str = malloc( (len + 1) * sizeof(WCHAR) )))
    {
        r->ok = FALSE;
        return NULL;
    }
    memcpy( str, r->p, len * sizeof(WCHAR) );
    str[len] = 0;
    r->p += len * sizeof(WCHAR);
    return str;
}

struct writer
{
    BYTE *buf;
    SIZE_T len;
    SIZE_T cap;
    BOOL ok;
};

static void wr_bytes( struct writer *w, const void *data, SIZE_T len )
{
    if (!w->ok) return;
    if (w->len + len > w->cap)
    {
        SIZE_T cap = w->cap * 2 > w->len + len + 1024 ? w->cap * 2 : w->len + len + 1024;
        BYTE *buf = realloc( w->buf, cap );
        if (!buf)
        {
            w->ok = FALSE;
            return;
        }
        w->buf = buf;
        w->cap = cap;
    }
    memcpy( w->buf + w->len, data, len );
    w->len += len;
}

static void wr_u32( struct writer *w, UINT32 v ) { wr_bytes( w, &v, sizeof(v) ); }
static void wr_u64( struct writer *w, UINT64 v ) { wr_bytes( w, &v, sizeof(v) ); }
static void wr_u8( struct writer *w, BYTE v ) { wr_bytes( w, &v, sizeof(v) ); }

static void wr_wstr( struct writer *w, const WCHAR *str )
{
    UINT32 len = str ? wcslen( str ) : 0;
    wr_u32( w, len );
    if (len) wr_bytes( w, str, len * sizeof(WCHAR) );
}

/* ---- package identity ---------------------------------------------------- */

static BYTE *load_game_config( void )
{
    WCHAR exe[MAX_PATH], *path;
    BYTE *data;
    SIZE_T size;
    int i;

    if (xgr_game_config && strstr( xgr_game_config, "<Identity" )) return (BYTE *)narrow_dup( xgr_game_config );

    if (GetCurrentDirectoryW( ARRAY_SIZE(exe), exe ) && (path = path_join( exe, L"MicrosoftGame.config" )))
    {
        BOOL ok = read_whole_file( path, &data, &size );
        free( path );
        if (ok) return data;
    }
    if (GetModuleFileNameW( NULL, exe, ARRAY_SIZE(exe) ))
    {
        for (i = 0; i < 6; i++)
        {
            WCHAR *sep = wcsrchr( exe, '\\' );
            BOOL ok;

            if (!sep) break;
            *sep = 0;
            if (!(path = path_join( exe, L"MicrosoftGame.config" ))) break;
            ok = read_whole_file( path, &data, &size );
            free( path );
            if (ok) return data;
        }
    }
    return NULL;
}

/* Pull attr="value" out of the first <element ...> in the document. */
static BOOL xml_attr( const char *xml, const char *element, const char *attr, char *out, SIZE_T cap )
{
    const char *start, *end, *value;
    SIZE_T len;

    if (!(start = strstr( xml, element )) || !(end = strchr( start, '>' ))) return FALSE;
    if (!(value = strstr( start, attr )) || value > end) return FALSE;
    value += strlen( attr );
    len = strcspn( value, "\"" );
    if (len >= cap) return FALSE;
    memcpy( out, value, len );
    out[len] = 0;
    return TRUE;
}

/* The 13-character suffix of a package family name: Crockford base32 of the
 * first 8 bytes of SHA-256 over the UTF-16 publisher string. */
static BOOL publisher_hash( const char *publisher, WCHAR out[14] )
{
    static const char alphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";
    BCRYPT_ALG_HANDLE alg;
    UCHAR hash[32];
    WCHAR *wide;
    UINT64 v = 0;
    NTSTATUS status;
    int i;

    if (!(wide = utf8_to_wide( publisher ))) return FALSE;
    status = BCryptOpenAlgorithmProvider( &alg, BCRYPT_SHA256_ALGORITHM, NULL, 0 );
    if (!status)
    {
        status = BCryptHash( alg, NULL, 0, (UCHAR *)wide, wcslen( wide ) * sizeof(WCHAR), hash, sizeof(hash) );
        BCryptCloseAlgorithmProvider( alg, 0 );
    }
    free( wide );
    if (status) return FALSE;

    for (i = 0; i < 8; i++) v = (v << 8) | hash[i];
    for (i = 0; i < 12; i++) out[i] = alphabet[(v >> (59 - 5 * i)) & 31];
    out[12] = alphabet[(v & 15) << 1];
    out[13] = 0;
    return TRUE;
}

static WCHAR *package_family;   /* <Name>_<hash>, NULL when unknown */
static WCHAR *package_aumid;    /* <family>!<executable id> */

/* Work out the package family name from MicrosoftGame.config, once.
 *
 * Any thread can be first here -- a provider opened on a worker while the
 * main thread asks for its persistent local storage -- and the work takes
 * a file read and a hash. Run under an InitOnce, so a second caller waits
 * for the answer instead of seeing "done" with no family yet and putting
 * the saves under Packages\UnknownPackage for the rest of the session. */
static BOOL WINAPI init_package_identity( INIT_ONCE *once, void *param, void **context )
{
    BYTE *xml;
    char name[128], publisher[512], app[128];
    WCHAR hash[14], *name_w, buf[320];

    if (!(xml = load_game_config()))
    {
        WARN( "MicrosoftGame.config not found, package identity unknown.\n" );
        return TRUE;
    }
    if (xml_attr( (char *)xml, "<Identity", " Name=\"", name, sizeof(name) ) &&
        xml_attr( (char *)xml, "<Identity", " Publisher=\"", publisher, sizeof(publisher) ) &&
        publisher_hash( publisher, hash ) && (name_w = utf8_to_wide( name )))
    {
        swprintf( buf, ARRAY_SIZE(buf), L"%s_%s", name_w, hash );
        free( name_w );
        package_family = wide_dup( buf );

        if (!xml_attr( (char *)xml, "<Executable ", " Id=\"", app, sizeof(app) )) strcpy( app, "Game" );
        if ((name_w = utf8_to_wide( app )))
        {
            swprintf( buf, ARRAY_SIZE(buf), L"%s!%s", package_family, name_w );
            free( name_w );
            package_aumid = wide_dup( buf );
        }
        TRACE( "package family %s, aumid %s.\n", debugstr_w( package_family ), debugstr_w( package_aumid ) );
    }
    else WARN( "could not read package identity from MicrosoftGame.config.\n" );
    free( xml );
    return TRUE;
}

static void resolve_package_identity( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

    InitOnceExecuteOnce( &once, init_package_identity, NULL, NULL );
}

/* %LOCALAPPDATA%\Packages\<family>\SystemAppData\wgs, unless overridden */
static WCHAR *resolve_root( void )
{
    WCHAR buf[MAX_PATH], *base, *packages, *root = NULL;
    DWORD len;

    if ((len = GetEnvironmentVariableW( L"XGR_WGS_ROOT", buf, ARRAY_SIZE(buf) )) && len < ARRAY_SIZE(buf))
        return wide_dup( buf );

    if ((len = GetEnvironmentVariableW( L"LOCALAPPDATA", buf, ARRAY_SIZE(buf) )) && len < ARRAY_SIZE(buf))
        base = wide_dup( buf );
    else if ((len = GetEnvironmentVariableW( L"USERPROFILE", buf, ARRAY_SIZE(buf) )) && len < ARRAY_SIZE(buf))
        base = path_join( buf, L"AppData\\Local" );
    else
        return NULL;
    if (!base) return NULL;

    resolve_package_identity();
    if ((packages = path_join( base, L"Packages" )))
    {
        WCHAR *pkg = path_join( packages, package_family ? package_family : L"UnknownPackage" );
        if (pkg) root = path_join( pkg, L"SystemAppData\\wgs" );
        free( pkg );
        free( packages );
    }
    free( base );
    return root;
}

/* "00000000-0000-0000-0000-0000697f9ec3" -> "000000000000000000000000697F9EC3" */
static BOOL scid_to_suffix( const char *scid, WCHAR out[33] )
{
    int n = 0;

    for (; *scid && n < 32; scid++)
    {
        char c = *scid;
        if (c == '-' || c == '{' || c == '}') continue;
        if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return FALSE;
        out[n++] = c;
    }
    out[n] = 0;
    return n == 32 && !*scid;
}

/* The SCID a title's saves belong to, when the title does not say.
 *
 * Forza Horizon 5 initialises its provider with an all-zero configuration id,
 * which in the GDK means "the one this title was built with" rather than a
 * service configuration whose every digit is zero. Taking it literally puts
 * the saves under a folder named for a zero SCID, so a profile the player
 * already has -- copied from Windows, or written by an earlier run -- is
 * invisible, and the title starts as though it had never been played.
 *
 * The default follows the convention every Xbox title id-scoped SCID uses:
 * the last eight digits are the title id, everything before them zero. */
static BOOL title_default_suffix( WCHAR out[33] )
{
    char hex[32];

    if (!xgr_config_element( "TitleId", hex, sizeof(hex) ) || !hex[0]) return FALSE;
    swprintf( out, 33, L"000000000000000000000000%08lX", strtoul( hex, NULL, 16 ) );
    return TRUE;
}

static BOOL suffix_all_zero( const WCHAR *suffix )
{
    while (*suffix) if (*suffix++ != '0') return FALSE;
    return TRUE;
}

/* When a candidate folder's containers.index was last written, or 0 for a
 * folder that has none. */
static UINT64 index_mtime( const WCHAR *root, const WCHAR *leaf )
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    WCHAR *dir, *path;
    UINT64 mtime = 0;

    if (!(dir = path_join( root, leaf ))) return 0;
    if ((path = path_join( dir, L"containers.index" )))
    {
        if (GetFileAttributesExW( path, GetFileExInfoStandard, &info ))
            mtime = ((UINT64)info.ftLastWriteTime.dwHighDateTime << 32) | info.ftLastWriteTime.dwLowDateTime;
        free( path );
    }
    free( dir );
    return mtime;
}

/* The per-user folder for a service configuration, <XUID>_<SCID>.
 *
 * An existing folder with the right SCID suffix is used whatever XUID it
 * carries, which is how a save copied over from Windows is picked up. When
 * there are several -- a profile played here before the Windows one was
 * copied in, or a Windows install with more than one account -- the choice
 * must not be whichever the directory listing happens to return first:
 *
 *   - the folder of the configured user (XGR_XUID) wins outright;
 *   - otherwise the one whose index was written last, the profile in use;
 *   - and a tie goes to the lowest name, so the answer never changes between
 *     runs on its own.
 *
 * With no folder at all, one is named after the local user. */
static WCHAR *resolve_user_dir( const WCHAR *root, const char *scid )
{
    WCHAR suffix[33], leaf[80], best[MAX_PATH] = { 0 }, *pattern;
    WIN32_FIND_DATAW data;
    UINT64 xuid = xgr_local_xuid(), best_mtime = 0;
    unsigned int candidates = 0;
    BOOL exact = FALSE;
    HANDLE find;

    if (!scid_to_suffix( scid, suffix )) return NULL;
    /* a title with no id in its config keeps the zero suffix; there is nothing
     * better to say, and refusing to open a provider would be worse */
    if (suffix_all_zero( suffix ) && title_default_suffix( suffix ))
        TRACE( "title named no service configuration; using its own, %s.\n", debugstr_w( suffix ) );
    swprintf( leaf, ARRAY_SIZE(leaf), L"*_%s", suffix );
    if (!(pattern = path_join( root, leaf ))) return NULL;
    swprintf( leaf, ARRAY_SIZE(leaf), L"%016I64X_%s", xuid, suffix );
    find = FindFirstFileW( pattern, &data );
    free( pattern );
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            UINT64 mtime;

            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || wcslen( data.cFileName ) != 16 + 1 + 32 ||
                data.cFileName[16] != '_' || _wcsicmp( data.cFileName + 17, suffix ))
                continue;
            candidates++;
            if (exact) continue;
            if (xuid && !_wcsicmp( data.cFileName, leaf ))
            {
                wcscpy( best, data.cFileName );
                exact = TRUE;
                continue;
            }
            mtime = index_mtime( root, data.cFileName );
            if (!best[0] || mtime > best_mtime || (mtime == best_mtime && _wcsicmp( data.cFileName, best ) < 0))
            {
                wcscpy( best, data.cFileName );
                best_mtime = mtime;
            }
        } while (FindNextFileW( find, &data ));
        FindClose( find );
    }
    if (best[0])
    {
        if (candidates > 1 && !exact)
            WARN( "%u save folders for %s; using %s, the most recently written%s.\n", candidates,
                  debugstr_w( suffix ), debugstr_w( best ), xuid ? "" : " (set XGR_XUID to choose)" );
        else if (candidates > 1)
            WARN( "%u save folders for %s; using %s, the configured user's.\n", candidates,
                  debugstr_w( suffix ), debugstr_w( best ) );
        return path_join( root, best );
    }
    return path_join( root, leaf );
}

/* ---- shared package identity (used by XPackage too) --------------------- */

/* MicrosoftGame.config text, from the inline copy or from disk next to the
 * executable (NUL-terminated, caller frees). Shared with XPackage/XStore. */
char *xgr_load_game_config( void )
{
    return (char *)load_game_config();
}

/* The derived package family name (<Identity Name>_<hash>) as UTF-8, or NULL.
 * Single source of truth: the same value the save-folder layout is keyed on. */
char *xgr_package_family( void )
{
    resolve_package_identity();
    return package_family ? wide_to_utf8( package_family ) : NULL;
}

/* The install root: the directory that holds MicrosoftGame.config, which is
 * what XPackage reports as the current package's mount path. The executable
 * lives several directories below it (Sandfall\Binaries\WinGDK\... for a UE5
 * title), so walk up until the config is found; fall back to the CWD. */
WCHAR *xgr_package_root( void )
{
    WCHAR exe[MAX_PATH], *path;
    int i;

    if (GetModuleFileNameW( NULL, exe, ARRAY_SIZE(exe) ))
    {
        for (i = 0; i < 8; i++)
        {
            WCHAR *sep = wcsrchr( exe, '\\' );
            DWORD attr;

            if (!sep) break;
            *sep = 0;
            if (!(path = path_join( exe, L"MicrosoftGame.config" ))) break;
            attr = GetFileAttributesW( path );
            free( path );
            if (attr != INVALID_FILE_ATTRIBUTES) return wide_dup( exe );
        }
    }
    if (GetCurrentDirectoryW( ARRAY_SIZE(exe), exe )) return wide_dup( exe );
    return NULL;
}

/* ---- index -------------------------------------------------------------- */

static void free_container( struct wgs_container *c )
{
    free( c->name );
    free( c->display_name );
    free( c->etag );
    free( c );
}

static void index_free( struct wgs_index *idx )
{
    struct wgs_container *c, *next;

    if (idx->containers.next)
    {
        LIST_FOR_EACH_ENTRY_SAFE( c, next, &idx->containers, struct wgs_container, entry )
        {
            list_remove( &c->entry );
            free_container( c );
        }
    }
    free( idx->aumid );
    free( idx->id );
    memset( idx, 0, sizeof(*idx) );
    list_init( &idx->containers );
}

static void index_init_new( struct wgs_index *idx )
{
    GUID guid;
    WCHAR buf[37];

    idx->version = WGS_INDEX_VERSION;
    idx->mtime = filetime_now();
    idx->reserved1 = 3;
    CoCreateGuid( &guid );
    guid_to_string( &guid, buf );
    idx->id = wide_dup( buf );
    idx->quota = WGS_DEFAULT_QUOTA;
}

/* S_OK: parsed; S_FALSE: no index yet (a fresh one is set up); error: corrupt. */
static HRESULT index_read( struct wgs_index *idx, const WCHAR *path )
{
    BYTE *data;
    SIZE_T size;
    struct reader r;
    UINT32 count, i;

    index_free( idx );
    if (!read_whole_file( path, &data, &size ))
    {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
        {
            ERR( "cannot read %s, error %lu.\n", debugstr_w( path ), err );
            return E_GS_NO_ACCESS;
        }
        index_init_new( idx );
        return S_FALSE;
    }

    r.p = data;
    r.end = data + size;
    r.ok = TRUE;
    idx->version = rd_u32( &r );
    count = rd_u32( &r );
    idx->reserved0 = rd_u32( &r );
    idx->aumid = rd_wstr( &r );
    idx->mtime = rd_u64( &r );
    idx->reserved1 = rd_u32( &r );
    idx->id = rd_wstr( &r );
    idx->quota = rd_u64( &r );
    for (i = 0; r.ok && i < count; i++)
    {
        struct wgs_container *c;

        if (!(c = calloc( 1, sizeof(*c) )))
        {
            r.ok = FALSE;
            break;
        }
        c->name = rd_wstr( &r );
        c->display_name = rd_wstr( &r );
        c->etag = rd_wstr( &r );
        c->number = rd_u8( &r );
        c->flags = rd_u32( &r );
        rd_bytes( &r, &c->folder, sizeof(c->folder) );
        c->mtime = rd_u64( &r );
        c->reserved = rd_u64( &r );
        c->size = rd_u64( &r );
        list_add_tail( &idx->containers, &c->entry );
    }
    free( data );

    if (!r.ok || idx->version != WGS_INDEX_VERSION)
    {
        ERR( "%s: unrecognised index, version %u, %u containers, %s.\n", debugstr_w( path ),
             idx->version, count, r.ok ? "parsed" : "truncated" );
        index_free( idx );
        return E_GS_CONTAINER_SYNC_FAILED;
    }
    if (!idx->quota) idx->quota = WGS_DEFAULT_QUOTA;
    TRACE( "%s: %u containers, quota %I64u, aumid %s.\n", debugstr_w( path ), count, idx->quota, debugstr_w( idx->aumid ) );
    return S_OK;
}

static BOOL index_write( struct wgs_index *idx, const WCHAR *path )
{
    struct writer w = { 0 };
    struct wgs_container *c;
    UINT32 count = 0;
    BOOL ok;

    w.ok = TRUE;
    LIST_FOR_EACH_ENTRY( c, &idx->containers, struct wgs_container, entry ) count++;
    idx->mtime = filetime_now();

    wr_u32( &w, idx->version );
    wr_u32( &w, count );
    wr_u32( &w, idx->reserved0 );
    wr_wstr( &w, idx->aumid );
    wr_u64( &w, idx->mtime );
    wr_u32( &w, idx->reserved1 );
    wr_wstr( &w, idx->id );
    wr_u64( &w, idx->quota );
    LIST_FOR_EACH_ENTRY( c, &idx->containers, struct wgs_container, entry )
    {
        wr_wstr( &w, c->name );
        wr_wstr( &w, c->display_name );
        wr_wstr( &w, c->etag );
        wr_u8( &w, c->number );
        wr_u32( &w, c->flags );
        wr_bytes( &w, &c->folder, sizeof(c->folder) );
        wr_u64( &w, c->mtime );
        wr_u64( &w, c->reserved );
        wr_u64( &w, c->size );
    }
    ok = w.ok && write_file_atomic( path, w.buf, w.len );
    free( w.buf );
    return ok;
}

/* ---- container manifest ------------------------------------------------- */

static void blobs_free( struct list *blobs )
{
    struct wgs_blob *b, *next;

    LIST_FOR_EACH_ENTRY_SAFE( b, next, blobs, struct wgs_blob, entry )
    {
        list_remove( &b->entry );
        free( b );
    }
}

static struct wgs_blob *blobs_find( struct list *blobs, const WCHAR *name )
{
    struct wgs_blob *b;

    LIST_FOR_EACH_ENTRY( b, blobs, struct wgs_blob, entry )
        if (!_wcsicmp( b->name, name )) return b;
    return NULL;
}

static WCHAR *container_dir( const struct XGameSaveProvider *p, const struct wgs_container *c )
{
    WCHAR name[33];
    guid_to_name( &c->folder, name );
    return path_join( p->dir, name );
}

static WCHAR *manifest_path( const WCHAR *dir, BYTE number )
{
    WCHAR leaf[32];
    swprintf( leaf, ARRAY_SIZE(leaf), L"container.%u", number );
    return path_join( dir, leaf );
}

static WCHAR *blob_path( const WCHAR *dir, const GUID *file )
{
    WCHAR name[33];
    guid_to_name( file, name );
    return path_join( dir, name );
}

/* S_OK: parsed; S_FALSE: no manifest (empty list); error: corrupt or
 * unreadable. Sizes come from the blob files themselves, the manifest only
 * maps names to them.
 *
 * Only a manifest that is not there is an empty container. One that exists
 * but cannot be read -- a sharing violation, no access, no memory -- is an
 * error, as it is for the index: an update built on an empty list instead
 * writes a manifest naming only its own blobs, and every other blob in the
 * container is lost to the title. */
static HRESULT manifest_read( const WCHAR *dir, const struct wgs_container *c, struct list *blobs )
{
    WCHAR *path;
    BYTE *data;
    SIZE_T size;
    struct reader r;
    UINT32 version, count, i;

    list_init( blobs );
    if (!(path = manifest_path( dir, c->number ))) return E_OUTOFMEMORY;
    if (!read_whole_file( path, &data, &size ))
    {
        DWORD err = GetLastError();

        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
        {
            ERR( "cannot read %s, error %lu.\n", debugstr_w( path ), err );
            free( path );
            return E_GS_NO_ACCESS;
        }
        if (c->size) WARN( "%s is missing; container %s, %I64u bytes, has no blobs.\n",
                           debugstr_w( path ), debugstr_w( c->name ), c->size );
        free( path );
        return S_FALSE;
    }

    r.p = data;
    r.end = data + size;
    r.ok = TRUE;
    version = rd_u32( &r );
    count = rd_u32( &r );
    for (i = 0; r.ok && i < count; i++)
    {
        struct wgs_blob *b;
        WCHAR *file;

        if (!(b = calloc( 1, sizeof(*b) )))
        {
            r.ok = FALSE;
            break;
        }
        rd_bytes( &r, b->name, sizeof(b->name) );
        b->name[WGS_BLOB_NAME_CHARS - 1] = 0;
        rd_bytes( &r, &b->file, sizeof(b->file) );
        rd_bytes( &r, &b->file_alt, sizeof(b->file_alt) );
        if ((file = blob_path( dir, &b->file )))
        {
            if (!file_size( file, &b->size )) b->size = 0;
            free( file );
        }
        list_add_tail( blobs, &b->entry );
    }
    free( data );

    if (!r.ok || version != WGS_MANIFEST_VERSION)
    {
        ERR( "%s: unrecognised manifest, version %u, %u blobs, %s.\n", debugstr_w( path ),
             version, count, r.ok ? "parsed" : "truncated" );
        free( path );
        blobs_free( blobs );
        return E_GS_CONTAINER_SYNC_FAILED;
    }
    free( path );
    return S_OK;
}

static BOOL manifest_write( const WCHAR *dir, BYTE number, struct list *blobs )
{
    struct writer w = { 0 };
    struct wgs_blob *b;
    WCHAR *path;
    UINT32 count = 0;
    BOOL ok;

    w.ok = TRUE;
    LIST_FOR_EACH_ENTRY( b, blobs, struct wgs_blob, entry ) count++;
    wr_u32( &w, WGS_MANIFEST_VERSION );
    wr_u32( &w, count );
    LIST_FOR_EACH_ENTRY( b, blobs, struct wgs_blob, entry )
    {
        wr_bytes( &w, b->name, sizeof(b->name) );
        wr_bytes( &w, &b->file, sizeof(b->file) );
        wr_bytes( &w, &b->file_alt, sizeof(b->file_alt) );
    }
    if (!(path = manifest_path( dir, number )))
    {
        free( w.buf );
        return FALSE;
    }
    ok = w.ok && write_file_atomic( path, w.buf, w.len );
    free( path );
    free( w.buf );
    return ok;
}

/* ---- provider ----------------------------------------------------------- */

static void provider_addref( struct XGameSaveProvider *p )
{
    InterlockedIncrement( &p->ref );
}

static void provider_release( struct XGameSaveProvider *p )
{
    if (InterlockedDecrement( &p->ref )) return;
    index_free( &p->index );
    free( p->dir );
    free( p );
}

/* Refresh from disk. Called with gs_cs held. */
static HRESULT provider_reload( struct XGameSaveProvider *p )
{
    WCHAR *path;
    HRESULT hr;

    if (!(path = path_join( p->dir, L"containers.index" ))) return E_OUTOFMEMORY;
    hr = index_read( &p->index, path );
    free( path );
    p->read_only = FAILED( hr );
    return FAILED( hr ) ? hr : S_OK;
}

/* Write the index back. Called with gs_cs held. */
static HRESULT provider_commit( struct XGameSaveProvider *p )
{
    WCHAR *path;
    BOOL ok;

    if (p->read_only) return E_GS_NO_ACCESS;
    if (!p->index.aumid)
    {
        resolve_package_identity();
        p->index.aumid = wide_dup( package_aumid ? package_aumid : L"" );
    }
    if (!make_directory( p->dir )) return E_GS_OUT_OF_LOCAL_STORAGE;
    if (!(path = path_join( p->dir, L"containers.index" ))) return E_OUTOFMEMORY;
    ok = index_write( &p->index, path );
    if (!ok) ERR( "cannot write %s, error %lu.\n", debugstr_w( path ), GetLastError() );
    free( path );
    return ok ? S_OK : E_GS_OUT_OF_LOCAL_STORAGE;
}

static struct wgs_container *provider_find( struct XGameSaveProvider *p, const WCHAR *name )
{
    struct wgs_container *c;

    LIST_FOR_EACH_ENTRY( c, &p->index.containers, struct wgs_container, entry )
        if (!_wcsicmp( c->name, name )) return c;
    return NULL;
}

static UINT64 provider_used( struct XGameSaveProvider *p )
{
    struct wgs_container *c;
    UINT64 used = 0;

    LIST_FOR_EACH_ENTRY( c, &p->index.containers, struct wgs_container, entry ) used += c->size;
    return used;
}

static INT64 provider_remaining( struct XGameSaveProvider *p )
{
    UINT64 used = provider_used( p );
    return used < p->index.quota ? p->index.quota - used : 0;
}

static HRESULT create_provider( XUserHandle user, const char *scid, struct XGameSaveProvider **out )
{
    struct XGameSaveProvider *p;
    char declared[64];
    WCHAR suffix[33];
    WCHAR *root;
    HRESULT hr;

    /* No configuration id means the title's own: the SCID its
     * MicrosoftGame.config names under <SaveGameStorage>, which is where
     * Windows keeps those saves, or else what an all-zero id resolves to.
     * Batman: Arkham Knight passes none; refusing it left the title without a
     * save provider and its first save spinning for good. */
    if (!scid)
    {
        if (xgr_config_element( "SCID", declared, sizeof(declared) ) && scid_to_suffix( declared, suffix ))
            scid = declared;
        else
            scid = "00000000-0000-0000-0000-000000000000";
        TRACE( "no configuration id; using %s.\n", debugstr_a( scid ) );
    }
    if (!user || !out) return E_INVALIDARG;
    if (!(p = calloc( 1, sizeof(*p) ))) return E_OUTOFMEMORY;
    p->ref = 1;
    p->result_handle = p;
    list_init( &p->index.containers );

    if (!(root = resolve_root()))
    {
        provider_release( p );
        return E_GS_OUT_OF_LOCAL_STORAGE;
    }
    p->dir = resolve_user_dir( root, scid );
    free( root );
    if (!p->dir)
    {
        provider_release( p );
        return E_INVALIDARG;
    }

    EnterCriticalSection( &gs_cs );
    hr = provider_reload( p );
    LeaveCriticalSection( &gs_cs );
    if (FAILED( hr ))
    {
        provider_release( p );
        return hr;
    }
    TRACE( "provider %p for %s at %s.\n", p, debugstr_a( scid ), debugstr_w( p->dir ) );
    *out = p;
    return S_OK;
}

/* Rules from the documentation: the path part may use [A-Za-z0-9_/], the
 * final component [A-Za-z0-9_.-], non-empty, no trailing or doubled dot. */
static BOOL is_name_char( char c, BOOL leaf )
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') return TRUE;
    return leaf ? (c == '.' || c == '-') : c == '/';
}

static BOOL valid_container_name( const char *name )
{
    const char *p, *leaf;

    if (!name || !*name) return FALSE;
    leaf = strrchr( name, '/' );
    leaf = leaf ? leaf + 1 : name;
    for (p = name; p < leaf; p++) if (!is_name_char( *p, FALSE )) return FALSE;
    if (!*leaf || leaf[strlen( leaf ) - 1] == '.' || strstr( leaf, ".." )) return FALSE;
    for (p = leaf; *p; p++) if (!is_name_char( *p, TRUE )) return FALSE;
    return TRUE;
}

static UINT64 filetime_to_unix( UINT64 ft )
{
    return ft > TICKS_1601_TO_1970 ? (ft - TICKS_1601_TO_1970) / 10000000 : 0;
}

/* Hand one container's description to the title. Called with gs_cs held. */
static BOOL report_container( struct XGameSaveProvider *p, struct wgs_container *c, void *context,
                              XGameSaveContainerInfoCallback *callback )
{
    XGameSaveContainerInfo info;
    struct list blobs;
    struct wgs_blob *b;
    WCHAR *dir;
    UINT32 count = 0;
    BOOL cont = TRUE;

    memset( &info, 0, sizeof(info) );
    info.name = wide_to_utf8( c->name );
    info.displayName = wide_to_utf8( c->display_name );
    if ((dir = container_dir( p, c )))
    {
        if (SUCCEEDED( manifest_read( dir, c, &blobs ) ))
        {
            LIST_FOR_EACH_ENTRY( b, &blobs, struct wgs_blob, entry ) count++;
            blobs_free( &blobs );
        }
        free( dir );
    }
    info.blobCount = count;
    info.totalSize = c->size;
    info.lastModifiedTime = filetime_to_unix( c->mtime );
    info.needsSync = FALSE;

    TRACE( "container %s (%s), %u blobs, %I64u bytes.\n", debugstr_a( info.name ), debugstr_a( info.displayName ),
           count, c->size );
    if (info.name && info.displayName) cont = callback( &info, context );
    free( (char *)info.name );
    free( (char *)info.displayName );
    return cont;
}

/* Remember a blob name for as long as the container handle lives, for the
 * case where the title's buffer has no room for the names. */
static const char *container_string( struct XGameSaveContainer *container, const WCHAR *str )
{
    struct x_string_entry *e;
    int len;

    if (!(len = WideCharToMultiByte( CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL ))) return NULL;
    if (!(e = malloc( sizeof(*e) + len ))) return NULL;
    WideCharToMultiByte( CP_UTF8, 0, str, -1, e->str, len, NULL, NULL );
    list_add_tail( &container->strings, &e->entry );
    return e->str;
}

/* The core of XGameSaveReadBlobData. With buffer NULL only the count and the
 * size a buffer needs are reported. Called with gs_cs held.
 *
 * The buffer holds an XGameSaveBlob per blob followed by the data; the names
 * are appended too when there is room, otherwise they live with the handle. */
static HRESULT read_blobs( struct XGameSaveContainer *container, const char **names, UINT32 *count,
                           SIZE_T buffer_size, XGameSaveBlob *buffer, SIZE_T *required )
{
    struct XGameSaveProvider *p = container->provider;
    struct wgs_container *c;
    struct wgs_blob **selected = NULL, *b;
    struct list blobs;
    WCHAR *dir = NULL;
    SIZE_T data_size = 0, names_size = 0, need;
    UINT32 n = 0, total = 0, i;
    BYTE *cursor;
    char *name_cursor;
    HRESULT hr;

    list_init( &blobs );
    if ((hr = provider_reload( p )) != S_OK) return hr;
    if ((c = provider_find( p, container->name )))
    {
        if (!(dir = container_dir( p, c ))) return E_OUTOFMEMORY;
        if (FAILED( hr = manifest_read( dir, c, &blobs ) ))
        {
            free( dir );
            return hr;
        }
    }

    LIST_FOR_EACH_ENTRY( b, &blobs, struct wgs_blob, entry ) total++;
    if (!(selected = calloc( (names ? *count : total) + 1, sizeof(*selected) )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (names)
    {
        for (i = 0; i < *count; i++)
        {
            WCHAR *name = utf8_to_wide( names[i] );
            struct wgs_blob *found = name ? blobs_find( &blobs, name ) : NULL;

            free( name );
            if (!found)
            {
                TRACE( "blob %s not found in %s.\n", debugstr_a( names[i] ), debugstr_w( container->name ) );
                hr = E_GS_BLOB_NOT_FOUND;
                goto done;
            }
            selected[n++] = found;
        }
    }
    else
    {
        LIST_FOR_EACH_ENTRY( b, &blobs, struct wgs_blob, entry ) selected[n++] = b;
    }

    for (i = 0; i < n; i++)
    {
        data_size += selected[i]->size;
        names_size += WideCharToMultiByte( CP_UTF8, 0, selected[i]->name, -1, NULL, 0, NULL, NULL );
    }
    need = n * sizeof(XGameSaveBlob) + data_size;
    if (required) *required = need + names_size;
    if (!buffer)
    {
        *count = n;
        hr = S_OK;
        goto done;
    }
    if (buffer_size < need)
    {
        WARN( "buffer %Iu bytes, %u blobs need %Iu.\n", buffer_size, n, need );
        *count = n;
        hr = E_GS_PROVIDED_BUFFER_TOO_SMALL;
        goto done;
    }

    cursor = (BYTE *)buffer + n * sizeof(XGameSaveBlob);
    name_cursor = buffer_size >= need + names_size ? (char *)cursor + data_size : NULL;
    for (i = 0; i < n; i++)
    {
        WCHAR *path;
        BYTE *data;
        SIZE_T size;
        BOOL ok;

        b = selected[i];
        if (!(path = blob_path( dir, &b->file )))
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        ok = read_whole_file( path, &data, &size );
        free( path );
        if (!ok)
        {
            ERR( "blob %s of %s has no file.\n", debugstr_w( b->name ), debugstr_w( container->name ) );
            hr = E_GS_BLOB_NOT_FOUND;
            goto done;
        }
        if (size > b->size) size = b->size;
        memcpy( cursor, data, size );
        if (size < b->size) memset( cursor + size, 0, b->size - size );
        free( data );

        buffer[i].data = cursor;
        buffer[i].info.size = b->size;
        cursor += b->size;
        if (name_cursor)
        {
            int len = WideCharToMultiByte( CP_UTF8, 0, b->name, -1, name_cursor, names_size, NULL, NULL );
            buffer[i].info.name = name_cursor;
            name_cursor += len;
            names_size -= len;
        }
        else buffer[i].info.name = container_string( container, b->name );
        TRACE( "read blob %s, %u bytes.\n", debugstr_a( buffer[i].info.name ), buffer[i].info.size );
    }
    *count = n;
    hr = S_OK;

done:
    free( selected );
    blobs_free( &blobs );
    free( dir );
    return hr;
}

static void container_release( struct XGameSaveContainer *container );

static void free_read_request( struct read_request *req )
{
    UINT32 i;

    if (!req) return;
    if (req->names) for (i = 0; i < req->count; i++) free( req->names[i] );
    free( req->names );
    container_release( req->container );
    free( req );
}

/* Apply a queued update to disk. Called with gs_cs held. */
static HRESULT submit_update( struct XGameSaveUpdate *update )
{
    struct XGameSaveContainer *container = update->container;
    struct XGameSaveProvider *p = container->provider;
    struct wgs_container *c;
    struct wgs_op *op;
    struct wgs_blob *b;
    struct old_file *of, *of_next;
    struct list blobs, old_files;
    WCHAR *dir = NULL, *path;
    UINT64 used, new_size = 0;
    BYTE old_number;
    BOOL is_new, linked = FALSE;
    HRESULT hr;

    list_init( &blobs );
    list_init( &old_files );
    if ((hr = provider_reload( p )) != S_OK) return hr;
    if (p->read_only) return E_GS_NO_ACCESS;

    if (!(c = provider_find( p, container->name )))
    {
        if (!(c = calloc( 1, sizeof(*c) ))) return E_OUTOFMEMORY;
        c->name = wide_dup( container->name );
        c->display_name = wide_dup( update->display_name ? update->display_name : container->name );
        c->flags = 1;
        CoCreateGuid( &c->folder );
        is_new = TRUE;
    }
    else is_new = FALSE;
    old_number = c->number;

    if (!(dir = container_dir( p, c )))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (!is_new && FAILED( hr = manifest_read( dir, c, &blobs ) )) goto done;

    /* work out the resulting blob table before touching the disk */
    LIST_FOR_EACH_ENTRY( op, &update->ops, struct wgs_op, entry )
    {
        struct wgs_blob *found = blobs_find( &blobs, op->name );

        if (found)
        {
            if (!(of = malloc( sizeof(*of) )))
            {
                hr = E_OUTOFMEMORY;
                goto done;
            }
            of->file = found->file;
            list_add_tail( &old_files, &of->entry );
        }
        if (op->is_delete)
        {
            if (found)
            {
                list_remove( &found->entry );
                free( found );
            }
            continue;
        }
        if (!found)
        {
            if (!(found = calloc( 1, sizeof(*found) )))
            {
                hr = E_OUTOFMEMORY;
                goto done;
            }
            wcscpy( found->name, op->name );
            list_add_tail( &blobs, &found->entry );
        }
        CoCreateGuid( &found->file );
        found->file_alt = found->file;
        found->size = op->size;
        op->file = found->file;
    }
    LIST_FOR_EACH_ENTRY( b, &blobs, struct wgs_blob, entry ) new_size += b->size;

    used = provider_used( p ) - (is_new ? 0 : c->size);
    if (used + new_size > p->index.quota)
    {
        WARN( "quota exceeded: %I64u used, %I64u for %s, quota %I64u.\n", used, new_size,
              debugstr_w( container->name ), p->index.quota );
        hr = E_GS_QUOTA_EXCEEDED;
        goto done;
    }

    /* new blob files first, then the manifest that references them, then the
     * index: a failure part way leaves the previous generation intact */
    if (!make_directory( dir ))
    {
        hr = E_GS_OUT_OF_LOCAL_STORAGE;
        goto done;
    }
    LIST_FOR_EACH_ENTRY( op, &update->ops, struct wgs_op, entry )
    {
        BOOL ok;

        if (op->is_delete) continue;
        if (!(path = blob_path( dir, &op->file )))
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        ok = write_whole_file( path, op->data, op->size );
        if (!ok) ERR( "cannot write %s, error %lu.\n", debugstr_w( path ), GetLastError() );
        free( path );
        if (!ok)
        {
            hr = E_GS_OUT_OF_LOCAL_STORAGE;
            goto done;
        }
    }

    c->number = old_number + 1;
    if (!manifest_write( dir, c->number, &blobs ))
    {
        c->number = old_number;
        hr = E_GS_OUT_OF_LOCAL_STORAGE;
        goto done;
    }
    if (update->display_name)
    {
        free( c->display_name );
        c->display_name = wide_dup( update->display_name );
    }
    free( c->etag );
    if ((c->etag = malloc( 32 * sizeof(WCHAR) )))
        swprintf( c->etag, 32, L"\"0x%I64X\"", filetime_now() + TICKS_1601_TO_0001 );
    c->mtime = filetime_now();
    c->size = new_size;
    if (is_new)
    {
        list_add_tail( &p->index.containers, &c->entry );
        linked = TRUE;
    }
    if (FAILED( hr = provider_commit( p ) ))
    {
        if (linked)
        {
            list_remove( &c->entry );
            linked = FALSE;
        }
        goto done;
    }

    /* the update is on disk; what it superseded can go */
    if (!is_new && (path = manifest_path( dir, old_number )))
    {
        DeleteFileW( path );
        free( path );
    }
    LIST_FOR_EACH_ENTRY( of, &old_files, struct old_file, entry )
    {
        if ((path = blob_path( dir, &of->file )))
        {
            DeleteFileW( path );
            free( path );
        }
    }
    TRACE( "container %s now generation %u, %I64u bytes.\n", debugstr_w( c->name ), c->number, c->size );
    hr = S_OK;

done:
    if (is_new && !linked) free_container( c );
    LIST_FOR_EACH_ENTRY_SAFE( of, of_next, &old_files, struct old_file, entry )
    {
        list_remove( &of->entry );
        free( of );
    }
    blobs_free( &blobs );
    free( dir );
    return hr;
}

static HRESULT delete_container( struct XGameSaveProvider *p, const char *name )
{
    struct wgs_container *c;
    WCHAR *name_w, *dir, *pattern;
    WIN32_FIND_DATAW data;
    HANDLE find;
    HRESULT hr;

    if (!(name_w = utf8_to_wide( name ))) return E_OUTOFMEMORY;
    if ((hr = provider_reload( p )) != S_OK || !(c = provider_find( p, name_w )))
    {
        free( name_w );
        return hr;
    }
    free( name_w );
    if (p->read_only) return E_GS_NO_ACCESS;

    if ((dir = container_dir( p, c )))
    {
        if ((pattern = path_join( dir, L"*" )))
        {
            if ((find = FindFirstFileW( pattern, &data )) != INVALID_HANDLE_VALUE)
            {
                do
                {
                    WCHAR *path;

                    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    if (!(path = path_join( dir, data.cFileName ))) continue;
                    DeleteFileW( path );
                    free( path );
                } while (FindNextFileW( find, &data ));
                FindClose( find );
            }
            free( pattern );
        }
        RemoveDirectoryW( dir );
        free( dir );
    }
    list_remove( &c->entry );
    free_container( c );
    return provider_commit( p );
}

static struct wgs_op *update_find_op( struct XGameSaveUpdate *update, const WCHAR *name )
{
    struct wgs_op *op;

    LIST_FOR_EACH_ENTRY( op, &update->ops, struct wgs_op, entry )
        if (!_wcsicmp( op->name, name )) return op;
    return NULL;
}

static void free_op( struct wgs_op *op )
{
    free( op->data );
    free( op );
}

/* Queue a write or delete. A later operation on the same blob replaces an
 * earlier one, so the queue never holds two entries for one name. */
static HRESULT queue_op( struct XGameSaveUpdate *update, const char *blob_name, BOOL is_delete,
                         const BYTE *data, SIZE_T size )
{
    WCHAR *name;
    struct wgs_op *op;

    if (!blob_name || !*blob_name) return E_INVALIDARG;
    if (!(name = utf8_to_wide( blob_name ))) return E_OUTOFMEMORY;
    if (wcslen( name ) >= WGS_BLOB_NAME_CHARS)
    {
        WARN( "blob name %s too long.\n", debugstr_a( blob_name ) );
        free( name );
        return E_INVALIDARG;
    }
    if ((op = update_find_op( update, name )))
    {
        list_remove( &op->entry );
        update->total_size -= op->size;
        free_op( op );
    }
    if (!is_delete && update->total_size + size > GS_MAX_UPDATE_SIZE)
    {
        WARN( "update would grow to %Iu bytes.\n", update->total_size + size );
        free( name );
        return E_GS_UPDATE_TOO_BIG;
    }
    if (!(op = calloc( 1, sizeof(*op) )))
    {
        free( name );
        return E_OUTOFMEMORY;
    }
    wcscpy( op->name, name );
    free( name );
    op->is_delete = is_delete;
    if (!is_delete && size)
    {
        if (!(op->data = malloc( size )))
        {
            free( op );
            return E_OUTOFMEMORY;
        }
        memcpy( op->data, data, size );
    }
    op->size = is_delete ? 0 : size;
    list_add_tail( &update->ops, &op->entry );
    update->total_size += op->size;
    return S_OK;
}

/* ---- handles ------------------------------------------------------------ */

static void container_addref( struct XGameSaveContainer *container )
{
    InterlockedIncrement( &container->ref );
}

static void container_release( struct XGameSaveContainer *container )
{
    struct x_string_entry *e, *next;

    if (!container || InterlockedDecrement( &container->ref )) return;
    EnterCriticalSection( &gs_cs );
    LIST_FOR_EACH_ENTRY_SAFE( e, next, &container->strings, struct x_string_entry, entry )
    {
        list_remove( &e->entry );
        free( e );
    }
    LeaveCriticalSection( &gs_cs );
    provider_release( container->provider );
    free( container->name );
    free( container );
}

static void update_release( struct XGameSaveUpdate *update )
{
    struct wgs_op *op, *next;

    if (InterlockedDecrement( &update->ref )) return;
    LIST_FOR_EACH_ENTRY_SAFE( op, next, &update->ops, struct wgs_op, entry )
    {
        list_remove( &op->entry );
        free_op( op );
    }
    container_release( update->container );
    free( update->display_name );
    free( update );
}

/* ---- background writes --------------------------------------------------- */

/* SubmitUpdateAsync and DeleteContainerAsync are what a title calls from a
 * thread it cannot stall -- an autosave from the render loop -- and the work
 * behind them is not small: up to 16 MB of blob files, a manifest and the
 * index, each flushed to disk. So they are done here, off the caller's
 * thread, and the block completes when the data is on disk.
 *
 * Every change still lands in the order it was made. A job is only taken off
 * the queue by someone holding gs_cs, so whoever holds the saves sees every
 * job that has not started yet, and each call that takes them first writes
 * what is queued (lock_saves). A synchronous update made after an async one
 * therefore lands on top of it, as it did when both were written inline, and
 * a read sees what was submitted before it. Each job holds what it needs
 * alive -- the update and through it its container and provider -- so a
 * title may close its handles as soon as the call returns. */
struct save_job
{
    struct list entry;
    XAsyncBlock *async;
    struct XGameSaveUpdate *update;         /* a submit... */
    struct XGameSaveProvider *provider;     /* ...or a delete */
    char *name;
    HRESULT hr;
};

static struct list save_jobs = LIST_INIT( save_jobs );
static BOOL save_worker_running;
static CRITICAL_SECTION jobs_cs;
static CRITICAL_SECTION_DEBUG jobs_cs_debug =
{
    0, 0, &jobs_cs,
    { &jobs_cs_debug.ProcessLocksList, &jobs_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": jobs_cs") }
};
static CRITICAL_SECTION jobs_cs = { &jobs_cs_debug, -1, 0, 0, 0, 0 };

static void free_save_job( struct save_job *job )
{
    if (job->update) update_release( job->update );
    if (job->provider) provider_release( job->provider );
    free( job->name );
    free( job );
}

/* The oldest job not yet started. Called with gs_cs held. */
static struct save_job *next_save_job( void )
{
    struct save_job *job = NULL;

    EnterCriticalSection( &jobs_cs );
    if (!list_empty( &save_jobs ))
    {
        job = LIST_ENTRY( list_head( &save_jobs ), struct save_job, entry );
        list_remove( &job->entry );
    }
    LeaveCriticalSection( &jobs_cs );
    return job;
}

/* Called with gs_cs held. */
static void run_save_job( struct save_job *job )
{
    if (job->update) job->hr = submit_update( job->update );
    else job->hr = delete_container( job->provider, job->name );
    TRACE( "%s for block %p finished, hr %#lx.\n", job->update ? "update" : "delete", job->async, job->hr );
}

static void complete_save_job( struct save_job *job )
{
    XAsyncBlock *async = job->async;
    HRESULT hr = job->hr;

    free_save_job( job );
    xgr_async_complete( async, hr, NULL );
}

/* Take the saves, first writing whatever was queued before this call. The
 * jobs written here are completed by unlock_saves, once the saves are
 * released, so a completion callback does not run under gs_cs. */
static void lock_saves( struct list *done )
{
    struct save_job *job;

    list_init( done );
    EnterCriticalSection( &gs_cs );
    while ((job = next_save_job()))
    {
        run_save_job( job );
        list_add_tail( done, &job->entry );
    }
}

static void unlock_saves( struct list *done )
{
    struct save_job *job, *next;

    LeaveCriticalSection( &gs_cs );
    LIST_FOR_EACH_ENTRY_SAFE( job, next, done, struct save_job, entry )
    {
        list_remove( &job->entry );
        complete_save_job( job );
    }
}

static void run_save_jobs( void )
{
    struct save_job *job;

    for (;;)
    {
        EnterCriticalSection( &gs_cs );
        EnterCriticalSection( &jobs_cs );
        if (list_empty( &save_jobs ))
        {
            save_worker_running = FALSE;
            LeaveCriticalSection( &jobs_cs );
            LeaveCriticalSection( &gs_cs );
            return;
        }
        job = LIST_ENTRY( list_head( &save_jobs ), struct save_job, entry );
        list_remove( &job->entry );
        LeaveCriticalSection( &jobs_cs );
        run_save_job( job );
        LeaveCriticalSection( &gs_cs );
        complete_save_job( job );
    }
}

static void CALLBACK save_jobs_pool_callback( TP_CALLBACK_INSTANCE *instance, void *context )
{
    run_save_jobs();
}

static DWORD WINAPI save_jobs_thread( void *context )
{
    run_save_jobs();
    return 0;
}

/* Takes ownership of the job. The block reads as pending from here until the
 * job is written and completed. */
static void queue_save_job( struct save_job *job )
{
    struct list done;
    HANDLE thread;
    BOOL start;

    memset( job->async->internal, 0, sizeof(job->async->internal) );

    /* Called from one of the enumeration callbacks, this thread already
     * holds the saves, and the writer cannot take them before the callback
     * returns. A title that waits on the block right there would wait for
     * good, so the job is written here, after anything queued before it, as
     * it was before the writer existed. */
    if (gs_cs.OwningThread == ULongToHandle( GetCurrentThreadId() ))
    {
        lock_saves( &done );
        run_save_job( job );
        list_add_tail( &done, &job->entry );
        unlock_saves( &done );
        return;
    }

    EnterCriticalSection( &jobs_cs );
    list_add_tail( &save_jobs, &job->entry );
    start = !save_worker_running;
    save_worker_running = TRUE;
    LeaveCriticalSection( &jobs_cs );
    if (!start) return;

    if (TrySubmitThreadpoolCallback( save_jobs_pool_callback, NULL, NULL )) return;
    if ((thread = CreateThread( NULL, 0, save_jobs_thread, NULL, 0, NULL )))
    {
        CloseHandle( thread );
        return;
    }
    /* nowhere else to run it; doing it late beats not doing it */
    WARN( "no thread for the save writer, error %lu; writing on the caller's thread.\n", GetLastError() );
    run_save_jobs();
}

/* At process exit, write what is still queued. A title that submits its last
 * save and quits without waiting would otherwise lose it: the threads that
 * would have written it are gone by now. Only when the saves are free -- a
 * writer stopped halfway through a job left them held, and its half-changed
 * state with them, and that job is lost either way. The blocks are not
 * completed; nobody is left to look at them. */
void xgr_save_process_exit( void )
{
    struct save_job *job;

    if (!TryEnterCriticalSection( &gs_cs )) return;
    while (TryEnterCriticalSection( &jobs_cs ))
    {
        job = list_empty( &save_jobs ) ? NULL : LIST_ENTRY( list_head( &save_jobs ), struct save_job, entry );
        if (job) list_remove( &job->entry );
        LeaveCriticalSection( &jobs_cs );
        if (!job) break;
        TRACE( "writing a save left queued at exit.\n" );
        run_save_job( job );
    }
    LeaveCriticalSection( &gs_cs );
}

/* ---- interface ---------------------------------------------------------- */

struct x_game_save
{
    IXGameSaveImpl4 IXGameSaveImpl4_iface;
    LONG ref;
};

static inline struct x_game_save *impl_from_IXGameSaveImpl4( IXGameSaveImpl4 *iface )
{
    return CONTAINING_RECORD( iface, struct x_game_save, IXGameSaveImpl4_iface );
}

/* Ferestre's runtime capability check looks for this text in the built DLL
 * (titles/capabilities.toml, xgameruntime.gamesave-interface4-legacy). It is
 * referenced below rather than left unused, so no compiler or linker drops it,
 * and it does not depend on the wording of any diagnostic. */
static const char interface4_marker[] = "xgameruntime.gamesave-interface4-legacy";

static HRESULT WINAPI x_game_save_QueryInterface( IXGameSaveImpl4 *iface, REFIID iid, void **out )
{
    struct x_game_save *impl = impl_from_IXGameSaveImpl4( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IXGameSaveImpl4 ))
        TRACE( "%s: returning the complete interface; PlayFab methods fail.\n", interface4_marker );

    if (IsEqualGUID( iid, &IID_IUnknown        ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl  ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl2 ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl3 ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl4 ))
    {
        IXGameSaveImpl_AddRef( *out = &impl->IXGameSaveImpl4_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_game_save_AddRef( IXGameSaveImpl4 *iface )
{
    struct x_game_save *impl = impl_from_IXGameSaveImpl4( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_game_save_Release( IXGameSaveImpl4 *iface )
{
    struct x_game_save *impl = impl_from_IXGameSaveImpl4( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI x_game_save_XGameSaveInitializeProvider( IXGameSaveImpl4 *iface, XUserHandle requestingUser, const char *configurationId, BOOLEAN syncOnDemand, XGameSaveProviderHandle *provider )
{
    TRACE( "iface %p, requestingUser %p, configurationId %s, syncOnDemand %d, provider %p.\n", iface, requestingUser, debugstr_a( configurationId ), syncOnDemand, provider );

    if (!provider) return E_INVALIDARG;
    return create_provider( requestingUser, configurationId, provider );
}

static HRESULT WINAPI x_game_save_XGameSaveInitializeProviderAsync( IXGameSaveImpl4 *iface, XUserHandle requestingUser, const char *configurationId, BOOLEAN syncOnDemand, XAsyncBlock *async )
{
    struct XGameSaveProvider *provider = NULL;
    HRESULT hr;

    TRACE( "iface %p, requestingUser %p, configurationId %s, syncOnDemand %d, async %p.\n", iface, requestingUser, debugstr_a( configurationId ), syncOnDemand, async );

    if (!async) return E_INVALIDARG;
    hr = create_provider( requestingUser, configurationId, &provider );
    TRACE( "save provider async result: %Iu bytes, hr %#lx.\n",
           SUCCEEDED(hr) ? sizeof(XGameSaveProviderHandle) : (SIZE_T)0, hr );
    xgr_async_complete_ex( async, hr, provider ? &provider->result_handle : NULL,
                           SUCCEEDED(hr) ? sizeof(XGameSaveProviderHandle) : 0 );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveInitializeProviderResult( IXGameSaveImpl4 *iface, XAsyncBlock *async, XGameSaveProviderHandle *provider )
{
    HRESULT hr;
    void *payload;

    TRACE( "iface %p, async %p, provider %p.\n", iface, async, provider );

    if (!provider) return E_INVALIDARG;
    if (!xgr_async_completed( async, &hr, &payload )) return E_PENDING;
    *provider = SUCCEEDED( hr ) ? *(XGameSaveProviderHandle *)payload : NULL;
    return hr;
}

static void WINAPI x_game_save_XGameSaveCloseProvider( IXGameSaveImpl4 *iface, XGameSaveProviderHandle provider )
{
    TRACE( "iface %p, provider %p.\n", iface, provider );

    if (provider) provider_release( provider );
}

static HRESULT WINAPI x_game_save_XGameSaveGetRemainingQuota( IXGameSaveImpl4 *iface, XGameSaveProviderHandle provider, INT64 *remainingQuota )
{
    struct list done;
    HRESULT hr;

    TRACE( "iface %p, provider %p, remainingQuota %p.\n", iface, provider, remainingQuota );

    if (!provider || !remainingQuota) return E_INVALIDARG;
    lock_saves( &done );
    if ((hr = provider_reload( provider )) == S_OK) *remainingQuota = provider_remaining( provider );
    unlock_saves( &done );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveGetRemainingQuotaAsync( IXGameSaveImpl4 *iface, XGameSaveProviderHandle provider, XAsyncBlock *async )
{
    INT64 *remaining;
    HRESULT hr;

    TRACE( "iface %p, provider %p, async %p.\n", iface, provider, async );

    if (!async) return E_INVALIDARG;
    if (!(remaining = malloc( sizeof(*remaining) ))) return E_OUTOFMEMORY;
    if (FAILED( hr = x_game_save_XGameSaveGetRemainingQuota( iface, provider, remaining ) ))
    {
        free( remaining );
        remaining = NULL;
    }
    TRACE( "save quota async result: %Iu bytes, hr %#lx.\n", SUCCEEDED(hr) ? sizeof(*remaining) : (SIZE_T)0, hr );
    xgr_async_complete_ex( async, hr, remaining, SUCCEEDED(hr) ? sizeof(*remaining) : 0 );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveGetRemainingQuotaResult( IXGameSaveImpl4 *iface, XAsyncBlock *async, INT64 *remainingQuota )
{
    INT64 *remaining;
    HRESULT hr;

    TRACE( "iface %p, async %p, remainingQuota %p.\n", iface, async, remainingQuota );

    if (!remainingQuota) return E_INVALIDARG;
    if (!xgr_async_completed( async, &hr, (void **)&remaining )) return E_PENDING;
    if (FAILED( hr )) return hr;
    if (!remaining) return E_INVALIDARG;
    *remainingQuota = *remaining;
    free( remaining );
    async->internal[2] = NULL;
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveDeleteContainer( IXGameSaveImpl4 *iface, XGameSaveProviderHandle provider, const char *containerName )
{
    struct list done;
    HRESULT hr;

    TRACE( "iface %p, provider %p, containerName %s.\n", iface, provider, debugstr_a( containerName ) );

    if (!provider) return E_INVALIDARG;
    if (!valid_container_name( containerName )) return E_GS_INVALID_CONTAINER_NAME;
    lock_saves( &done );
    hr = delete_container( provider, containerName );
    unlock_saves( &done );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveDeleteContainerAsync( IXGameSaveImpl4 *iface, XGameSaveProviderHandle provider, const char *containerName, XAsyncBlock *async )
{
    struct save_job *job;
    HRESULT hr = S_OK;

    TRACE( "iface %p, provider %p, containerName %s, async %p.\n", iface, provider, debugstr_a( containerName ), async );

    if (!async) return E_INVALIDARG;
    if (!provider) hr = E_INVALIDARG;
    else if (!valid_container_name( containerName )) hr = E_GS_INVALID_CONTAINER_NAME;
    if (FAILED( hr ))
    {
        xgr_async_complete( async, hr, NULL );
        return S_OK;
    }
    if (!(job = calloc( 1, sizeof(*job) ))) return E_OUTOFMEMORY;
    if (!(job->name = narrow_dup( containerName )))
    {
        free( job );
        return E_OUTOFMEMORY;
    }
    job->async = async;
    job->provider = provider;
    provider_addref( provider );
    queue_save_job( job );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveDeleteContainerResult( IXGameSaveImpl4 *iface, XAsyncBlock *async )
{
    HRESULT hr;

    TRACE( "iface %p, async %p.\n", iface, async );

    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveGetContainerInfo( IXGameSaveImpl4 *iface, XGameSaveProviderHandle provider, const char *containerName, void *context, XGameSaveContainerInfoCallback *callback )
{
    struct list done;
    struct wgs_container *c;
    WCHAR *name;
    HRESULT hr;

    TRACE( "iface %p, provider %p, containerName %s, context %p, callback %p.\n", iface, provider, debugstr_a( containerName ), context, callback );

    if (!provider || !callback) return E_INVALIDARG;
    if (!valid_container_name( containerName )) return E_GS_INVALID_CONTAINER_NAME;
    if (!(name = utf8_to_wide( containerName ))) return E_OUTOFMEMORY;
    lock_saves( &done );
    if ((hr = provider_reload( provider )) == S_OK && (c = provider_find( provider, name )))
        report_container( provider, c, context, callback );
    unlock_saves( &done );
    free( name );
    return hr;
}

static HRESULT enumerate_containers( struct XGameSaveProvider *provider, const char *prefix, void *context, XGameSaveContainerInfoCallback *callback )
{
    struct list done;
    struct wgs_container *c;
    WCHAR *prefix_w = NULL, **names;
    SIZE_T prefix_len = 0;
    UINT32 count = 0, i;
    HRESULT hr;

    if (!provider || !callback) return E_INVALIDARG;
    if (prefix && *prefix)
    {
        if (!(prefix_w = utf8_to_wide( prefix ))) return E_OUTOFMEMORY;
        prefix_len = wcslen( prefix_w );
    }
    lock_saves( &done );
    if ((hr = provider_reload( provider )) == S_OK)
    {
        /* the callback may call back in and reload the list, so walk a
         * snapshot of the names and look each one up again */
        LIST_FOR_EACH_ENTRY( c, &provider->index.containers, struct wgs_container, entry ) count++;
        if (!(names = calloc( count + 1, sizeof(*names) ))) hr = E_OUTOFMEMORY;
        else
        {
            i = 0;
            LIST_FOR_EACH_ENTRY( c, &provider->index.containers, struct wgs_container, entry )
                names[i++] = wide_dup( c->name );
            for (i = 0; i < count; i++)
            {
                if (!names[i] || (prefix_w && _wcsnicmp( names[i], prefix_w, prefix_len ))) continue;
                if (!(c = provider_find( provider, names[i] ))) continue;
                if (!report_container( provider, c, context, callback )) break;
            }
            for (i = 0; i < count; i++) free( names[i] );
            free( names );
        }
    }
    unlock_saves( &done );
    free( prefix_w );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateContainerInfo( IXGameSaveImpl4 *iface, XGameSaveProviderHandle provider, void *context, XGameSaveContainerInfoCallback *callback )
{
    TRACE( "iface %p, provider %p, context %p, callback %p.\n", iface, provider, context, callback );

    return enumerate_containers( provider, NULL, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateContainerInfoByName( IXGameSaveImpl4 *iface, XGameSaveProviderHandle provider, const char *containerNamePrefix, void *context, XGameSaveContainerInfoCallback *callback )
{
    TRACE( "iface %p, provider %p, containerNamePrefix %s, context %p, callback %p.\n", iface, provider, debugstr_a( containerNamePrefix ), context, callback );

    return enumerate_containers( provider, containerNamePrefix, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveCreateContainer( IXGameSaveImpl4 *iface, XGameSaveProviderHandle provider, const char *containerName, XGameSaveContainerHandle *containerContext )
{
    struct XGameSaveContainer *container;

    TRACE( "iface %p, provider %p, containerName %s, containerContext %p.\n", iface, provider, debugstr_a( containerName ), containerContext );

    if (!provider || !containerContext) return E_INVALIDARG;
    if (!valid_container_name( containerName )) return E_GS_INVALID_CONTAINER_NAME;
    if (!(container = calloc( 1, sizeof(*container) ))) return E_OUTOFMEMORY;
    if (!(container->name = utf8_to_wide( containerName )))
    {
        free( container );
        return E_OUTOFMEMORY;
    }
    list_init( &container->strings );
    container->ref = 1;
    container->provider = provider;
    provider_addref( provider );
    /* nothing reaches the disk until an update is submitted */
    *containerContext = container;
    return S_OK;
}

static void WINAPI x_game_save_XGameSaveCloseContainer( IXGameSaveImpl4 *iface, XGameSaveContainerHandle context )
{
    TRACE( "iface %p, context %p.\n", iface, context );

    container_release( context );
}

static HRESULT enumerate_blobs( struct XGameSaveContainer *container, const char *prefix, void *context, XGameSaveBlobInfoCallback *callback )
{
    struct list done;
    struct XGameSaveProvider *p;
    struct wgs_container *c;
    struct wgs_blob *b;
    struct list blobs;
    WCHAR *prefix_w = NULL, *dir;
    SIZE_T prefix_len = 0;
    HRESULT hr;

    if (!container || !callback) return E_INVALIDARG;
    p = container->provider;
    if (prefix && *prefix)
    {
        if (!(prefix_w = utf8_to_wide( prefix ))) return E_OUTOFMEMORY;
        prefix_len = wcslen( prefix_w );
    }
    lock_saves( &done );
    if ((hr = provider_reload( p )) == S_OK && (c = provider_find( p, container->name )))
    {
        if (!(dir = container_dir( p, c ))) hr = E_OUTOFMEMORY;
        else
        {
            if (SUCCEEDED( hr = manifest_read( dir, c, &blobs ) ))
            {
                LIST_FOR_EACH_ENTRY( b, &blobs, struct wgs_blob, entry )
                {
                    XGameSaveBlobInfo info;
                    BOOL cont;

                    if (prefix_w && _wcsnicmp( b->name, prefix_w, prefix_len )) continue;
                    if (!(info.name = wide_to_utf8( b->name ))) continue;
                    info.size = b->size;
                    TRACE( "blob %s, %u bytes.\n", debugstr_a( info.name ), info.size );
                    cont = callback( &info, context );
                    free( (char *)info.name );
                    if (!cont) break;
                }
                blobs_free( &blobs );
                hr = S_OK;
            }
            free( dir );
        }
    }
    unlock_saves( &done );
    free( prefix_w );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateBlobInfo( IXGameSaveImpl4 *iface, XGameSaveContainerHandle container, void *context, XGameSaveBlobInfoCallback *callback )
{
    TRACE( "iface %p, container %p, context %p, callback %p.\n", iface, container, context, callback );

    return enumerate_blobs( container, NULL, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateBlobInfoByName( IXGameSaveImpl4 *iface, XGameSaveContainerHandle container, const char *blobNamePrefix, void *context, XGameSaveBlobInfoCallback *callback )
{
    TRACE( "iface %p, container %p, blobNamePrefix %s, context %p, callback %p.\n", iface, container, debugstr_a( blobNamePrefix ), context, callback );

    return enumerate_blobs( container, blobNamePrefix, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveReadBlobData( IXGameSaveImpl4 *iface, XGameSaveContainerHandle container, const char **blobNames, UINT32 *countOfBlobs, SIZE_T blobsSize, XGameSaveBlob *blobData )
{
    struct list done;
    HRESULT hr;

    TRACE( "iface %p, container %p, blobNames %p, countOfBlobs %p, blobsSize %Iu, blobData %p.\n", iface, container, blobNames, countOfBlobs, blobsSize, blobData );

    if (!container || !countOfBlobs) return E_INVALIDARG;
    lock_saves( &done );
    hr = read_blobs( container, blobNames, countOfBlobs, blobsSize, blobData, NULL );
    unlock_saves( &done );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveReadBlobDataAsync( IXGameSaveImpl4 *iface, XGameSaveContainerHandle container, const char **blobNames, UINT32 countOfBlobs, XAsyncBlock *async )
{
    struct list done;
    struct read_request *req;
    SIZE_T required = 0;
    UINT32 n = countOfBlobs, i;
    HRESULT hr;

    TRACE( "iface %p, container %p, blobNames %p, countOfBlobs %u, async %p.\n", iface, container, blobNames, countOfBlobs, async );

    if (!container || !async) return E_INVALIDARG;
    if (!(req = calloc( 1, sizeof(*req) ))) return E_OUTOFMEMORY;
    req->container = container;
    container_addref( container );
    if (blobNames)
    {
        req->count = n;
        if (!(req->names = calloc( n + 1, sizeof(*req->names) )))
        {
            free_read_request( req );
            return E_OUTOFMEMORY;
        }
        for (i = 0; i < n; i++)
        {
            if (!(req->names[i] = narrow_dup( blobNames[i] )))
            {
                free_read_request( req );
                return E_OUTOFMEMORY;
            }
        }
    }

    /* validate now and report the buffer size; the copy happens in *Result */
    lock_saves( &done );
    hr = read_blobs( container, (const char **)req->names, &n, 0, NULL, &required );
    unlock_saves( &done );
    if (FAILED( hr ))
    {
        free_read_request( req );
        req = NULL;
    }
    xgr_async_complete_ex( async, hr, req, required );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveReadBlobDataResult( IXGameSaveImpl4 *iface, XAsyncBlock *async, SIZE_T blobsSize, XGameSaveBlob *blobData, UINT32 *countOfBlobs )
{
    struct list done;
    struct read_request *req;
    UINT32 n;
    HRESULT hr;

    TRACE( "iface %p, async %p, blobsSize %Iu, blobData %p, countOfBlobs %p.\n", iface, async, blobsSize, blobData, countOfBlobs );

    if (!xgr_async_completed( async, &hr, (void **)&req )) return E_PENDING;
    if (FAILED( hr )) return hr;
    if (!req) return E_INVALIDARG;

    n = req->count;
    lock_saves( &done );
    hr = read_blobs( req->container, (const char **)req->names, &n, blobsSize, blobData, NULL );
    unlock_saves( &done );
    if (countOfBlobs) *countOfBlobs = n;
    if (hr != E_GS_PROVIDED_BUFFER_TOO_SMALL)
    {
        /* keep the request around for a retry with a bigger buffer only */
        async->internal[2] = NULL;
        free_read_request( req );
    }
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveCreateUpdate( IXGameSaveImpl4 *iface, XGameSaveContainerHandle container, const char *containerDisplayName, XGameSaveUpdateHandle *updateContext )
{
    struct XGameSaveUpdate *update;

    TRACE( "iface %p, container %p, containerDisplayName %s, updateContext %p.\n", iface, container, debugstr_a( containerDisplayName ), updateContext );

    if (!container || !updateContext) return E_INVALIDARG;
    if (!(update = calloc( 1, sizeof(*update) ))) return E_OUTOFMEMORY;
    if (containerDisplayName && !(update->display_name = utf8_to_wide( containerDisplayName )))
    {
        free( update );
        return E_OUTOFMEMORY;
    }
    list_init( &update->ops );
    update->ref = 1;
    update->container = container;
    container_addref( container );
    *updateContext = update;
    return S_OK;
}

static void WINAPI x_game_save_XGameSaveCloseUpdate( IXGameSaveImpl4 *iface, XGameSaveUpdateHandle context )
{
    TRACE( "iface %p, context %p.\n", iface, context );

    if (context) update_release( context );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitBlobWrite( IXGameSaveImpl4 *iface, XGameSaveUpdateHandle updateContext, const char *blobName, UINT8 *data, SIZE_T byteCount )
{
    TRACE( "iface %p, updateContext %p, blobName %s, data %p, byteCount %Iu.\n", iface, updateContext, debugstr_a( blobName ), data, byteCount );

    if (!updateContext || (!data && byteCount)) return E_INVALIDARG;
    return queue_op( updateContext, blobName, FALSE, data, byteCount );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitBlobDelete( IXGameSaveImpl4 *iface, XGameSaveUpdateHandle updateContext, const char *blobName )
{
    TRACE( "iface %p, updateContext %p, blobName %s.\n", iface, updateContext, debugstr_a( blobName ) );

    if (!updateContext) return E_INVALIDARG;
    return queue_op( updateContext, blobName, TRUE, NULL, 0 );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitUpdate( IXGameSaveImpl4 *iface, XGameSaveUpdateHandle updateContext )
{
    struct list done;
    HRESULT hr;

    TRACE( "iface %p, updateContext %p.\n", iface, updateContext );

    if (!updateContext) return E_INVALIDARG;
    lock_saves( &done );
    hr = submit_update( updateContext );
    unlock_saves( &done );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitUpdateAsync( IXGameSaveImpl4 *iface, XGameSaveUpdateHandle updateContext, XAsyncBlock *async )
{
    struct save_job *job;

    TRACE( "iface %p, updateContext %p, async %p.\n", iface, updateContext, async );

    if (!async) return E_INVALIDARG;
    if (!updateContext)
    {
        xgr_async_complete( async, E_INVALIDARG, NULL );
        return S_OK;
    }
    if (!(job = calloc( 1, sizeof(*job) ))) return E_OUTOFMEMORY;
    job->async = async;
    job->update = updateContext;
    InterlockedIncrement( &updateContext->ref );
    queue_save_job( job );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitUpdateResult( IXGameSaveImpl4 *iface, XAsyncBlock *async )
{
    HRESULT hr;

    TRACE( "iface %p, async %p.\n", iface, async );

    if (!xgr_async_completed( async, &hr, NULL )) return E_PENDING;
    return hr;
}

/* XGameSaveFiles: a plain folder the title reads and writes itself. It sits
 * inside the same per-user directory so everything for one title stays put. */
static HRESULT WINAPI x_game_save_XGameSaveFilesGetFolderWithUiAsync( IXGameSaveImpl4 *iface, XUserHandle requestingUser, const char *configurationId, XAsyncBlock *async )
{
    struct XGameSaveProvider *provider;
    WCHAR *dir;
    char *folder = NULL;
    HRESULT hr;

    TRACE( "iface %p, requestingUser %p, configurationId %s, async %p.\n", iface, requestingUser, debugstr_a( configurationId ), async );

    if (!async) return E_INVALIDARG;
    if (SUCCEEDED( hr = create_provider( requestingUser, configurationId, &provider ) ))
    {
        if ((dir = path_join( provider->dir, L"files" )) && make_directory( dir )) folder = wide_to_utf8( dir );
        if (!folder) hr = E_GS_OUT_OF_LOCAL_STORAGE;
        free( dir );
        provider_release( provider );
    }
    TRACE( "folder %s, hr %#lx.\n", debugstr_a( folder ), hr );
    xgr_async_complete_ex( async, hr, folder, folder ? strlen( folder ) + 1 : 0 );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveFilesGetFolderWithUiResult( IXGameSaveImpl4 *iface, XAsyncBlock *async, SIZE_T folderSize, char *folderResult )
{
    char *folder;
    SIZE_T len;
    HRESULT hr;

    TRACE( "iface %p, async %p, folderSize %Iu, folderResult %p.\n", iface, async, folderSize, folderResult );

    if (!xgr_async_completed( async, &hr, (void **)&folder )) return E_PENDING;
    if (FAILED( hr )) return hr;
    if (!folder) return E_INVALIDARG;
    len = strlen( folder ) + 1;
    if (!folderResult || folderSize < len) return E_GS_PROVIDED_BUFFER_TOO_SMALL;
    memcpy( folderResult, folder, len );
    free( folder );
    async->internal[2] = NULL;
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveFilesGetRemainingQuota( IXGameSaveImpl4 *iface, XUserHandle userContext, const char *configurationId, INT64 *remainingQuota )
{
    struct list done;
    struct XGameSaveProvider *provider;
    HRESULT hr;

    TRACE( "iface %p, userContext %p, configurationId %s, remainingQuota %p.\n", iface, userContext, debugstr_a( configurationId ), remainingQuota );

    if (!remainingQuota) return E_INVALIDARG;
    if (FAILED( hr = create_provider( userContext, configurationId, &provider ) )) return hr;
    lock_saves( &done );
    *remainingQuota = provider_remaining( provider );
    unlock_saves( &done );
    provider_release( provider );
    return S_OK;
}

/* The PlayFab half delegates to its separate authenticated synchronization
 * backend; legacy WGS behavior and interface slots remain unchanged. */
static HRESULT WINAPI x_game_save_PFXGameSaveInitializeConfig( IXGameSaveImpl4 *iface, struct PFXGameSaveConfigRequest *request, PFXGameSaveConfigHandle *config )
{
    return pfx_config_create(request, config);
}

static void WINAPI x_game_save_PFXGameSaveFreeConfig( IXGameSaveImpl4 *iface, PFXGameSaveConfigHandle config )
{
    pfx_config_close(config);
}

static HRESULT WINAPI x_game_save_PFXGameSaveFilesGetFolderWithUiAsync( IXGameSaveImpl4 *iface, PFXGameSaveConfigHandle config, XAsyncBlock *async )
{
    return pfx_folder_async(config, async);
}

static HRESULT WINAPI x_game_save_PFXGameSaveFilesGetFolderWithUiResult( IXGameSaveImpl4 *iface, XAsyncBlock *async, SIZE_T folderSize, char *folderResult )
{
    return pfx_folder_result(async, folderSize, folderResult);
}

static HRESULT WINAPI x_game_save_PFXGameSaveFilesGetRemainingQuota( IXGameSaveImpl4 *iface, PFXGameSaveConfigHandle config, INT64 *remainingQuota )
{
    return pfx_remaining_quota(config, remainingQuota);
}

static HRESULT WINAPI x_game_save_PFXGameSaveSetUiCallbacks( IXGameSaveImpl4 *iface, struct PFXGameSaveUiCallbacks *callbacks )
{
    return pfx_set_ui(callbacks);
}

static HRESULT WINAPI x_game_save_PFXGameSaveProgressUiGetProgress( IXGameSaveImpl4 *iface, XUserHandle user, UINT32 *syncState, UINT64 *current, UINT64 *total )
{
    return pfx_progress(user, syncState, current, total);
}

static HRESULT WINAPI x_game_save_PFXGameSaveSetProgressUiResponse( IXGameSaveImpl4 *iface, XUserHandle user, UINT32 response )
{
    return pfx_ui_response(user, 0, response);
}

static HRESULT WINAPI x_game_save_PFXGameSaveSetSyncFailedUiResponse( IXGameSaveImpl4 *iface, XUserHandle user, UINT32 response )
{
    return pfx_ui_response(user, 1, response);
}

static HRESULT WINAPI x_game_save_PFXGameSaveSetActiveDeviceContentionUiResponse( IXGameSaveImpl4 *iface, XUserHandle user, UINT32 response )
{
    return pfx_ui_response(user, 2, response);
}

static HRESULT WINAPI x_game_save_PFXGameSaveSetConflictUiResponse( IXGameSaveImpl4 *iface, XUserHandle user, UINT32 response )
{
    return pfx_ui_response(user, 3, response);
}

static HRESULT WINAPI x_game_save_PFXGameSaveSetOutOfStorageUiResponse( IXGameSaveImpl4 *iface, XUserHandle user, UINT32 response )
{
    return pfx_ui_response(user, 4, response);
}

static HRESULT WINAPI x_game_save_PFXGameSaveFilesUploadWithUiAsync( IXGameSaveImpl4 *iface, PFXGameSaveConfigHandle config, UINT32 options, XAsyncBlock *async )
{
    return pfx_upload_async(config, options, async);
}

static HRESULT WINAPI x_game_save_PFXGameSaveFilesUploadWithUiResult( IXGameSaveImpl4 *iface, XAsyncBlock *async )
{
    return pfx_status_result(async);
}

static HRESULT WINAPI x_game_save_PFXGameSaveFilesSetSaveDescriptionAsync( IXGameSaveImpl4 *iface, PFXGameSaveConfigHandle config, const char *description, XAsyncBlock *async )
{
    return pfx_description_async(config, description, async);
}

static HRESULT WINAPI x_game_save_PFXGameSaveFilesSetSaveDescriptionResult( IXGameSaveImpl4 *iface, XAsyncBlock *async )
{
    return pfx_status_result(async);
}

static HRESULT WINAPI x_game_save_PFXGameSaveSetActiveDeviceChangedCallback( IXGameSaveImpl4 *iface, void *callback, void *context )
{
    return pfx_set_device_callback(callback, context);
}

static const struct IXGameSaveImpl4Vtbl x_game_save_vtbl =
{
    x_game_save_QueryInterface,
    x_game_save_AddRef,
    x_game_save_Release,
    /* IXGameSaveImpl methods */
    x_game_save_XGameSaveInitializeProvider,
    x_game_save_XGameSaveInitializeProviderAsync,
    x_game_save_XGameSaveInitializeProviderResult,
    x_game_save_XGameSaveCloseProvider,
    x_game_save_XGameSaveGetRemainingQuota,
    x_game_save_XGameSaveGetRemainingQuotaAsync,
    x_game_save_XGameSaveGetRemainingQuotaResult,
    x_game_save_XGameSaveDeleteContainer,
    x_game_save_XGameSaveDeleteContainerAsync,
    x_game_save_XGameSaveDeleteContainerResult,
    x_game_save_XGameSaveGetContainerInfo,
    x_game_save_XGameSaveEnumerateContainerInfo,
    x_game_save_XGameSaveEnumerateContainerInfoByName,
    x_game_save_XGameSaveCreateContainer,
    x_game_save_XGameSaveCloseContainer,
    x_game_save_XGameSaveEnumerateBlobInfo,
    x_game_save_XGameSaveEnumerateBlobInfoByName,
    x_game_save_XGameSaveReadBlobData,
    x_game_save_XGameSaveReadBlobDataAsync,
    x_game_save_XGameSaveReadBlobDataResult,
    x_game_save_XGameSaveCreateUpdate,
    x_game_save_XGameSaveCloseUpdate,
    x_game_save_XGameSaveSubmitBlobWrite,
    x_game_save_XGameSaveSubmitBlobDelete,
    x_game_save_XGameSaveSubmitUpdate,
    x_game_save_XGameSaveSubmitUpdateAsync,
    x_game_save_XGameSaveSubmitUpdateResult,
    /* IXGameSaveImpl2 methods */
    x_game_save_XGameSaveFilesGetFolderWithUiAsync,
    x_game_save_XGameSaveFilesGetFolderWithUiResult,
    x_game_save_XGameSaveFilesGetRemainingQuota,
    x_game_save_PFXGameSaveInitializeConfig,
    x_game_save_PFXGameSaveFreeConfig,
    x_game_save_PFXGameSaveFilesGetFolderWithUiAsync,
    x_game_save_PFXGameSaveFilesGetFolderWithUiResult,
    x_game_save_PFXGameSaveFilesGetRemainingQuota,
    x_game_save_PFXGameSaveSetUiCallbacks,
    x_game_save_PFXGameSaveProgressUiGetProgress,
    x_game_save_PFXGameSaveSetProgressUiResponse,
    x_game_save_PFXGameSaveSetSyncFailedUiResponse,
    x_game_save_PFXGameSaveSetActiveDeviceContentionUiResponse,
    x_game_save_PFXGameSaveSetConflictUiResponse,
    x_game_save_PFXGameSaveSetOutOfStorageUiResponse,
    x_game_save_PFXGameSaveFilesUploadWithUiAsync,
    x_game_save_PFXGameSaveFilesUploadWithUiResult,
    x_game_save_PFXGameSaveFilesSetSaveDescriptionAsync,
    x_game_save_PFXGameSaveFilesSetSaveDescriptionResult,
    x_game_save_PFXGameSaveSetActiveDeviceChangedCallback,
};

static struct x_game_save x_game_save =
{
    {&x_game_save_vtbl},
    0,
};

IXGameSaveImpl *x_game_save_impl = (IXGameSaveImpl *)&x_game_save.IXGameSaveImpl4_iface;
