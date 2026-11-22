/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XAsync, XTaskQueue and XThread
 *
 * Written by Weather
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

#include "private.h"

#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);
WINE_DECLARE_DEBUG_CHANNEL(xqueue);

struct x_threading
{
    IXThreadingImpl IXThreadingImpl_iface;
    LONG ref;
};

static inline struct x_threading *impl_from_IXThreadingImpl( IXThreadingImpl *iface )
{
    return CONTAINING_RECORD( iface, struct x_threading, IXThreadingImpl_iface );
}

static HRESULT WINAPI x_threading_QueryInterface( IXThreadingImpl *iface, REFIID iid, void **out )
{
    struct x_threading *impl = impl_from_IXThreadingImpl( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown        ) ||
        IsEqualGUID( iid, &IID_IXThreadingImpl ))
    {
        IXThreadingImpl_AddRef( *out = &impl->IXThreadingImpl_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_threading_AddRef( IXThreadingImpl *iface )
{
    struct x_threading *impl = impl_from_IXThreadingImpl( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_threading_Release( IXThreadingImpl *iface )
{
    struct x_threading *impl = impl_from_IXThreadingImpl( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

struct x_task_queue;
struct x_queue_port;
struct x_async_op;
static BOOL deliver_one_completion( XTaskQueueHandle queue, XTaskQueuePort port, BOOL own_only );
static BOOL dispatch_manual_work( XTaskQueueHandle queue, XTaskQueuePort port );
static XTaskQueueDispatchMode work_mode_for( XTaskQueueHandle queue );
static struct x_queue_port *queue_port( XTaskQueueHandle queue, XTaskQueuePort port );
static HRESULT WINAPI x_threading_XTaskQueueSubmitDelayedCallback( IXThreadingImpl *iface,
    XTaskQueueHandle queue, XTaskQueuePort port, UINT32 delayMs, void *context, XTaskQueueCallback *callback );
static HRESULT queue_submit( struct x_task_queue *queue, XTaskQueuePort port, void *context,
                             XTaskQueueCallback *callback, UINT32 delay_ms, BOOL serial );

/* Where an operation's completion callback goes, read from the caller's block
 * before its result is published. Once XAsyncGetStatus can report the block
 * done, its owner may free or reuse it -- a stack block in the synchronous
 * pattern goes out of scope straight away -- so nothing may read the block
 * after that. Operations begun here take theirs when they begin, as the GDK
 * does. */
struct x_completion_route
{
    XAsyncBlock *async;
    XAsyncCompletionRoutine *callback;
    XTaskQueueHandle queue;             /* as the block named it, possibly null */
    struct x_queue_port *port;
    XTaskQueueHandle origin;
    XTaskQueueDispatchMode mode;
};

static void capture_completion_route( const XAsyncBlock *async, struct x_completion_route *route );
static void deliver_completion( const struct x_completion_route *route, struct x_async_op *keep_alive );
static void complete_and_deliver( const struct x_completion_route *route, HRESULT hr, void *payload, SIZE_T size );

/* Somewhere to block until an operation finishes or a terminated queue goes
 * quiet. One condition variable serves every waiter: waits are rare and each
 * re-checks its own condition, so a broadcast costs little, and whoever makes
 * progress only takes the lock when somebody is actually waiting. */
static CRITICAL_SECTION wait_cs;
static CRITICAL_SECTION_DEBUG wait_cs_debug =
{
    0, 0, &wait_cs,
    { &wait_cs_debug.ProcessLocksList, &wait_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": wait_cs") }
};
static CRITICAL_SECTION wait_cs = { &wait_cs_debug, -1, 0, 0, 0, 0 };
static CONDITION_VARIABLE wait_cv = CONDITION_VARIABLE_INIT;
static LONG waiters;
static BOOL callback_blocks_wait( const XAsyncBlock *async );

/* The caller has just made its progress visible with an interlocked
 * operation, and a waiter registers itself with one before it checks, so
 * either the waiter sees the progress or this sees the waiter. Passing
 * through the lock means a waiter that saw nothing is already asleep. */
static void wake_waiters( void )
{
    if (!InterlockedCompareExchange( &waiters, 0, 0 )) return;
    EnterCriticalSection( &wait_cs );
    LeaveCriticalSection( &wait_cs );
    WakeAllConditionVariable( &wait_cv );
}

/* xgr_async_completed with acquire ordering. The result fields are written
 * before the marker that says they are there, so they must be read after it;
 * otherwise a poller can see the marker and an HRESULT from before. */
static BOOL async_block_completed( const XAsyncBlock *async, HRESULT *hr, void **payload )
{
    if (ReadPointerAcquire( &async->internal[0] ) != XGR_ASYNC_MAGIC) return FALSE;
    if (hr) *hr = (HRESULT)(ULONG_PTR)async->internal[1];
    if (payload) *payload = async->internal[2];
    return TRUE;
}

static BOOL async_wait_completed( const XAsyncBlock *async, HRESULT *hr )
{
    return async_block_completed( async, hr, NULL ) && !callback_blocks_wait( async );
}

static BOOL WINAPI init_manual_dispatch( INIT_ONCE *once, void *param, void **context )
{
    char value[2];
    *context = (void *)(ULONG_PTR)(GetEnvironmentVariableA( "XGR_STRICT_MANUAL_QUEUES", value, sizeof(value) ) == 1 &&
                                 value[0] == '1' ? 4 : 0);
    return TRUE;
}

static BOOL strict_manual_dispatch( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    void *context;
    InitOnceExecuteOnce( &once, init_manual_dispatch, NULL, &context );
    return context != NULL;
}

static HRESULT WINAPI x_threading_XAsyncGetStatus( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, BOOLEAN wait )
{
    ULONGLONG started, warned;
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, wait %d.\n", iface, asyncBlock, wait );

    if (!asyncBlock) return E_INVALIDARG;
    if (!wait) return async_block_completed( asyncBlock, &hr, NULL ) ? hr : E_PENDING;
    if (async_wait_completed( asyncBlock, &hr )) return hr;

    /* wait means "block until the operation finishes", for as long as that
     * takes. This used to give up after 30000 polls and return E_PENDING:
     * about 30 s, and much sooner when completions kept arriving, since a poll
     * that delivered one did not sleep. Licence and catalog requests are
     * allowed 50 s, so a slow one looked failed -- and the caller, having
     * given up, freed the block its result was later written into.
     *
     * Keep draining manually-dispatched completions while blocking: a
     * single-threaded caller that waits here instead of pumping its own queue
     * would otherwise deadlock against work only it can dispatch. */
    started = warned = GetTickCount64();
    EnterCriticalSection( &wait_cs );
    InterlockedIncrement( &waiters );
    for (;;)
    {
        BOOL delivered;

        if (async_wait_completed( asyncBlock, &hr )) break;
        LeaveCriticalSection( &wait_cs );
        delivered = deliver_one_completion( asyncBlock->queue, XTaskQueuePort_Completion, FALSE );
        EnterCriticalSection( &wait_cs );
        if (delivered || async_wait_completed( asyncBlock, &hr )) continue;

        /* Woken when any result is published or a completion is parked; the
         * timeout only bounds how stale a missed one can get. */
        SleepConditionVariableCS( &wait_cv, &wait_cs, 100 );
        if (GetTickCount64() - warned >= 30000)
        {
            warned = GetTickCount64();
            WARN( "still waiting for asyncBlock %p after %I64u s.\n", asyncBlock, (warned - started) / 1000 );
        }
    }
    InterlockedDecrement( &waiters );
    LeaveCriticalSection( &wait_cs );
    return hr;
}

static HRESULT WINAPI x_threading_XAsyncGetResultSize( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, SIZE_T *bufferSize )
{
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, bufferSize %p.\n", iface, asyncBlock, bufferSize );

    if (!asyncBlock || !bufferSize) return E_INVALIDARG;
    if (!async_block_completed( asyncBlock, &hr, NULL )) return E_PENDING;
    *bufferSize = (SIZE_T)asyncBlock->internal[3];
    return hr;
}

/* --- XAsync provider framework -------------------------------------------
 *
 * Everything above serves operations this DLL satisfies itself. This is the
 * other half: the contract a *caller* uses to implement its own asynchronous
 * operation on top of our task queues. libHttpClient (which recent GDK titles
 * bundle, Minecraft among them) builds every HTTP call on it, so while these
 * were stubs no networking of any kind could run.
 *
 * The shape is: XAsyncBegin() attaches a provider callback to the caller's
 * XAsyncBlock and invokes it with Begin. The provider drives itself forward by
 * calling XAsyncSchedule(), which invokes it again with DoWork on a worker
 * thread; DoWork returns E_PENDING while there is more to do. When finished it
 * calls XAsyncComplete(), which records the result and fires the block's
 * completion callback. The caller then calls XAsyncGetResult(), which asks the
 * provider to copy the payload out (GetResult) and finally releases it
 * (Cleanup). */
struct x_async_op
{
    struct list entry;
    LONG ref;               /* the list holds one; each in-flight user holds one */
    XAsyncBlock *async;
    XAsyncProvider *provider;
    XAsyncProviderData provider_data; /* retained through deferred provider callbacks */
    void *context;
    const void *identity;
    XAsyncWork *work;       /* XAsyncRun operations have no provider */
    UINT32 delay_ms;        /* pending XAsyncSchedule backoff */
    HRESULT result;
    SIZE_T required_size;
    BOOL completed;
    BOOL begin_running;     /* the provider's Begin has not returned yet */
    BOOL deliver_pending;   /* it completed in there; deliver once it has */
    BOOL cleanup_on_release; /* failed Begin has no result for its caller to take */
    BOOL canceling;         /* XAsyncCancel has asked the provider */
    XAsyncBlock cleanup_async; /* the completion may free the caller's block */
    struct x_completion_route route; /* taken when it began */
};

static struct list async_ops = LIST_INIT( async_ops );
static const char no_result_marker;
static CRITICAL_SECTION async_ops_cs;
static CRITICAL_SECTION_DEBUG async_ops_cs_debug =
{
    0, 0, &async_ops_cs,
    { &async_ops_cs_debug.ProcessLocksList, &async_ops_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": async_ops_cs") }
};
static CRITICAL_SECTION async_ops_cs = { &async_ops_cs_debug, -1, 0, 0, 0, 0 };

/* Caller must hold async_ops_cs. */
/* Newest first, deliberately.
 *
 * Operations are keyed by the address of the caller's XAsyncBlock, and titles
 * reuse those blocks: Minecraft runs three XUserAddAsync calls through one
 * address. If a finished operation for the same address is still listed --
 * it stays until the title collects its result -- then matching the first
 * entry hands newly scheduled work the stale operation, and the new one never
 * runs at all. That is what left XblAchievementsResultGetNextAsync begun,
 * scheduled and then silent, with the profile screen waiting on it. */
static struct x_async_op *find_async_op( const XAsyncBlock *async )
{
    struct x_async_op *op;

    LIST_FOR_EACH_ENTRY_REV( op, &async_ops, struct x_async_op, entry )
        if (op->async == async) return op;
    return NULL;
}

/* async_ops_cs protects the list, not the objects on it. Looking an operation
 * up and then using it after dropping the lock is a use-after-free: another
 * thread reaching XAsyncGetResult (or completing an XAsyncRun) removes the
 * operation and frees it while this one is still inside the provider. The
 * freed block is reused almost immediately and the damage surfaces far from
 * here -- a std::mutex living in the reused memory ends up with its owners
 * field cleared while threads are parked on it, so they are never woken and
 * the title freezes on a lock that looks free.
 *
 * The list holds one reference; every user takes its own. */
static struct x_async_op *acquire_async_op( const XAsyncBlock *async )
{
    struct x_async_op *op;

    EnterCriticalSection( &async_ops_cs );
    if ((op = find_async_op( async ))) InterlockedIncrement( &op->ref );
    LeaveCriticalSection( &async_ops_cs );
    return op;
}

static void release_async_op( struct x_async_op *op )
{
    if (op && !InterlockedDecrement( &op->ref ))
    {
        if (op->cleanup_on_release)
        {
            op->provider_data.async = &op->cleanup_async;
            op->provider_data.bufferSize = 0;
            op->provider_data.buffer = NULL;
            op->provider( XAsyncOp_Cleanup, &op->provider_data );
        }
        free( op );
    }
}

static void store_async_result( XAsyncBlock *async, HRESULT hr, void *payload, SIZE_T result_size );
static void fill_async_result( XAsyncBlock *async, HRESULT hr, void *payload, SIZE_T result_size );

/* A terminal outcome without a payload needs no GetResult. Its caller
 * may only read GetStatus and never call GetResult, so retain the provider
 * through Begin/work and the callback, then clean it up without a result read.
 * An inline completion wins over the later return value. */
static void complete_without_result( struct x_async_op *op, HRESULT hr )
{
    BOOL drop = FALSE;

    EnterCriticalSection( &async_ops_cs );
    if (!op->completed)
    {
        op->completed = TRUE;
        op->result = hr;
        op->required_size = 0;
        op->deliver_pending = TRUE;
        /* Copy the block before publishing: afterwards it is the caller's. */
        op->cleanup_async = *op->async;
        fill_async_result( &op->cleanup_async, hr, (void *)&no_result_marker, 0 );
        store_async_result( op->async, hr, (void *)&no_result_marker, 0 );
        op->cleanup_on_release = op->provider != NULL;
        list_remove( &op->entry );
        drop = TRUE;
        TRACE( "provider for block %p completes asynchronously; cleanup follows delivery.\n", op->async );
    }
    LeaveCriticalSection( &async_ops_cs );
    if (drop) release_async_op( op );
}

/* The same, and deliver the callback unless Begin is still running and will. */
static void complete_op_without_result( struct x_async_op *op, HRESULT hr )
{
    BOOL deliver;

    complete_without_result( op, hr );
    EnterCriticalSection( &async_ops_cs );
    deliver = !op->begin_running && op->deliver_pending;
    if (deliver) op->deliver_pending = FALSE;
    LeaveCriticalSection( &async_ops_cs );
    if (deliver) deliver_completion( &op->route, op );
}

/* Record an operation's outcome and fire the block's completion callback.
 * Shared by XAsyncComplete and by the DoWork paths that finish inline.
 *
 * A block with no operation behind it is not completed, it is *refused*.
 * XAsyncComplete only means anything for a block XAsyncBegin started here, and
 * a caller that reaches this with anything else is holding a pointer that has
 * stopped being an XAsyncBlock -- Age of Empires: Definitive Edition arrives
 * with 0x0000090b32e048a4 shortly after two nested libHttpClient operations
 * complete on two threads. Writing the result into it faults on the first
 * dereference, inside this dll, which reads as a runtime bug rather than as
 * what it is. */
static void x_threading_XAsyncComplete_helper( XAsyncBlock *async, HRESULT result, SIZE_T required_size )
{
    struct x_completion_route route;
    struct x_async_op *op, *drop = NULL;
    BOOL defer = FALSE;

    /* A success with no payload, like a failure, needs no GetResult. Retaining
     * these providers leaks their contexts and leaves XSAPI CleanupAsync
     * waiting forever for HTTP operations that have already completed.
     * Keep ownership through the worker and callback before Cleanup. */
    if (FAILED(result) || !required_size)
    {
        if (!(op = acquire_async_op( async ))) return;
        complete_op_without_result( op, result );
        release_async_op( op );
        return;
    }

    EnterCriticalSection( &async_ops_cs );
    op = find_async_op( async );
    if (!op)
    {
        LeaveCriticalSection( &async_ops_cs );
        WARN( "no operation for asyncBlock %p; refusing to complete a block this runtime never began.\n",
              async );
        return;
    }
    {
        if (op->completed)
        {
            LeaveCriticalSection( &async_ops_cs );
            return;
        }
        op->completed = TRUE;
        op->result = result;
        op->required_size = required_size;
        /* A provider is allowed to finish the operation from inside its own
         * Begin -- libHttpClient's websocket send does, because Wine's
         * WinHttpWebSocketSend completes a send that does not block there and
         * then rather than on a worker thread the way Windows does.
         *
         * The callback must not go out yet. Every GDK API promises to return
         * before its completion routine runs, and a title relies on that: it
         * calls HCWebSocketSendMessageAsync and finishes wiring up the object
         * the completion will reach through afterwards. Delivering inside the
         * call runs the handler against an object that is still half-built --
         * Forza Horizon 5 reads a field it has not assigned yet, finds -1
         * where it null-checks for 0, and calls through it.
         *
         * So record the outcome now (anything polling the block sees it) and
         * hand the callback to XAsyncBegin to deliver once it returns. */
        defer = op->begin_running;
        if (defer) op->deliver_pending = TRUE;
        route = op->route;
        /* An XAsyncRun operation has no provider payload to fetch later, so it
         * is finished with here; its caller reads the outcome from the block
         * itself. Keeping it listed would leak one entry per call. */
        if (!op->provider)
        {
            list_remove( &op->entry );
            drop = op;
        }
        TRACE( "operation %p completed for block %p, %Iu byte result, %s%s.\n",
               op, async, required_size, drop ? "withdrawn" : "kept for its provider",
               defer ? ", callback held until Begin returns" : "" );
    }
    LeaveCriticalSection( &async_ops_cs );

    release_async_op( drop );
    /* Store the result in the block, then deliver the callback according to
     * the queue's dispatch mode -- by the route the operation began with,
     * because once the result is stored the block is no longer ours to read. */
    if (defer) store_async_result( async, result, NULL, required_size );
    else complete_and_deliver( &route, result, NULL, required_size );
}

static void async_op_invoke( struct x_async_op *op, XAsyncOp which, SIZE_T buffer_size, void *buffer )
{
    /* Cancel may still be inside the provider while another thread collects
     * its result. Only GetResult owns the buffer fields in that interval. */
    if (which == XAsyncOp_GetResult || which == XAsyncOp_Cleanup)
    {
        op->provider_data.bufferSize = buffer_size;
        op->provider_data.buffer = buffer;
    }
    if (op->provider) op->provider( which, &op->provider_data );
}

/* Run one DoWork pass (or the XAsyncRun work function). */
static void async_op_do_work( struct x_async_op *op )
{
    XAsyncBlock *async = op->async;
    HRESULT hr;

    if (op->work)
    {
        hr = op->work( async );
        /* An XAsyncRun work function completes the block itself unless it
         * reports it is still going. */
        if (hr != E_PENDING)
        {
            EnterCriticalSection( &async_ops_cs );
            if (!op->completed)
            {
                LeaveCriticalSection( &async_ops_cs );
                x_threading_XAsyncComplete_helper( async, hr, 0 );
                return;
            }
            LeaveCriticalSection( &async_ops_cs );
        }
        return;
    }

    hr = op->provider ? op->provider( XAsyncOp_DoWork, &op->provider_data ) : E_NOTIMPL;

    /* E_PENDING means the provider will schedule itself again or complete
     * later; anything else that has not already completed is the result. */
    if (hr != E_PENDING)
    {
        BOOL done;
        EnterCriticalSection( &async_ops_cs );
        done = op->completed;
        LeaveCriticalSection( &async_ops_cs );
        if (!done) x_threading_XAsyncComplete_helper( async, hr, 0 );
    }
}

/* Somewhere to run work the thread pool would not take.
 *
 * TrySubmitThreadpoolCallback fails when the pool is saturated, and a title
 * that retries hard saturates it easily. Dropping the work then leaves the
 * operation begun and never finished: XblProfileGetUserProfilesAsync was
 * scheduled and simply never ran, so the profile screen waited for a result
 * that could not arrive. A thread costs more than a pool slot but it always
 * runs. */
struct x_offpool_work
{
    PTP_SIMPLE_CALLBACK callback;
    void *context;
};

static DWORD WINAPI offpool_thread( void *arg )
{
    struct x_offpool_work *w = arg;

    w->callback( NULL, w->context );
    free( w );
    return 0;
}

static HRESULT run_off_pool( PTP_SIMPLE_CALLBACK callback, void *context )
{
    struct x_offpool_work *w;
    HANDLE thread;

    if (!(w = calloc( 1, sizeof(*w) ))) return E_OUTOFMEMORY;
    w->callback = callback;
    w->context = context;

    if (!(thread = CreateThread( NULL, 0, offpool_thread, w, 0, NULL )))
    {
        free( w );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    CloseHandle( thread );
    return S_OK;
}

/* One pass of provider work, scheduled by XAsyncSchedule or XAsyncRun.
 *
 * The work item carries the operation itself, with a reference taken when it
 * was scheduled, not the block's address. Looked up by address when it fires,
 * it found whatever operation was newest there by then: one that had already
 * completed, which DoWork could start over, or a later operation reusing the
 * block, which received DoWork before it had scheduled any. Work for an
 * operation that is no longer the block's live one is simply dropped. */
static void cancel_async_op( struct x_async_op *op );

static void CALLBACK async_work_callback( void *context, BOOLEAN canceled )
{
    struct x_async_op *op = context;
    BOOL active;

    EnterCriticalSection( &async_ops_cs );
    active = find_async_op( op->async ) == op && !op->completed;
    LeaveCriticalSection( &async_ops_cs );
    if (active)
    {
        /* Its queue was terminated before the work ran, so no more work will
         * be scheduled for it. Tell the provider, as XAsyncCancel would, and
         * if that did not finish the operation, finish it here: nothing else
         * will. This is the one place the runtime itself completes an
         * operation with E_ABORT. */
        if (canceled)
        {
            BOOL done;

            cancel_async_op( op );
            EnterCriticalSection( &async_ops_cs );
            done = op->completed;
            LeaveCriticalSection( &async_ops_cs );
            if (!done) complete_op_without_result( op, E_ABORT );
        }
        else async_op_do_work( op );
    }
    else TRACE( "dropping work for operation %p, which is no longer running.\n", op );
    release_async_op( op );
}

static HRESULT schedule_async_work( struct x_async_op *op, UINT32 delay_ms )
{
    XTaskQueueHandle queue = op->route.queue;
    HRESULT hr;

    InterlockedIncrement( &op->ref );   /* the work item's own */
    if (strict_manual_dispatch() && work_mode_for( queue ) == XTaskQueueDispatchMode_Manual)
        hr = x_threading_XTaskQueueSubmitDelayedCallback( NULL, queue, XTaskQueuePort_Work,
                                                         delay_ms, op, async_work_callback );
    /* Provider work goes straight to the pool rather than through the queue's
     * runner, but it is still the queue's work: terminating the queue cancels
     * it if it has not started and waits for it if it has. A block without a
     * queue of its own is not tied to the process queue's lifetime. */
    else hr = queue_submit( (struct x_task_queue *)queue, XTaskQueuePort_Work, op,
                            async_work_callback, delay_ms, FALSE );
    if (FAILED(hr)) release_async_op( op );
    return hr;
}

static HRESULT WINAPI x_threading_XAsyncRun( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, XAsyncWork *work )
{
    struct x_async_op *op;
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, work %p.\n", iface, asyncBlock, work );

    if (!asyncBlock || !work) return E_INVALIDARG;
    if (!(op = calloc( 1, sizeof(*op) ))) return E_OUTOFMEMORY;
    op->async = asyncBlock;
    op->work = work;
    /* The list's reference, and this frame's: once scheduled, the work can
     * complete the operation -- and free it -- on another thread. */
    op->ref = 2;
    capture_completion_route( asyncBlock, &op->route );
    memset( asyncBlock->internal, 0, sizeof(asyncBlock->internal) );

    EnterCriticalSection( &async_ops_cs );
    list_add_tail( &async_ops, &op->entry );
    LeaveCriticalSection( &async_ops_cs );
    TRACE( "operation %p registered for block %p (run).\n", op, asyncBlock );

    if (FAILED(hr = schedule_async_work( op, 0 )))
    {
        /* The caller hears about this failure from the call itself, so the
         * operation must not stay listed against its block, and no callback
         * follows. The block records the failure, as the GDK's does when it
         * refuses a call: XAsyncGetStatus on it, even with wait, returns the
         * failure rather than waiting for an operation that does not exist. */
        BOOL withdraw;

        EnterCriticalSection( &async_ops_cs );
        if ((withdraw = !op->completed))
        {
            op->completed = TRUE;
            list_remove( &op->entry );
        }
        LeaveCriticalSection( &async_ops_cs );
        if (withdraw)
        {
            store_async_result( asyncBlock, hr, NULL, 0 );
            release_async_op( op );
        }
    }
    release_async_op( op );
    return hr;
}

static HRESULT WINAPI x_threading_XAsyncBegin( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, void *context, const void *identity, const char *identityName, XAsyncProvider *provider )
{
    struct x_async_op *op;
    BOOL deliver;
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, context %p, identity %p, identityName %s, provider %p.\n",
           iface, asyncBlock, context, identity, debugstr_a(identityName), provider );

    if (!asyncBlock || !provider) return E_INVALIDARG;
    if (!(op = calloc( 1, sizeof(*op) ))) return E_OUTOFMEMORY;
    op->async = asyncBlock;
    op->provider = provider;
    op->context = context;
    op->provider_data.async = asyncBlock;
    op->provider_data.context = context;
    op->identity = identity;
    /* The list holds one reference; this frame holds the other until Begin
     * returns, because a provider may finish the operation -- and another
     * thread take its result, freeing it -- before that happens. */
    op->ref = 2;

    op->begin_running = TRUE;
    capture_completion_route( asyncBlock, &op->route );
    memset( asyncBlock->internal, 0, sizeof(asyncBlock->internal) );

    EnterCriticalSection( &async_ops_cs );
    list_add_tail( &async_ops, &op->entry );
    LeaveCriticalSection( &async_ops_cs );
    TRACE( "operation %p registered for block %p (begin).\n", op, asyncBlock );
    TRACE( "retained provider data %p until Cleanup.\n", &op->provider_data );

    hr = provider( XAsyncOp_Begin, &op->provider_data );

    /* Setup succeeded. A provider's Begin result belongs to the async
     * operation, never to the caller of XAsyncBegin. Returning E_PENDING (or
     * another failure) there makes libHttpClient destroy its context while
     * the completion we accepted is still queued. An already completed
     * operation keeps its first result. */
    if (FAILED(hr) && hr != E_PENDING)
        complete_without_result( op, hr );

    EnterCriticalSection( &async_ops_cs );
    op->begin_running = FALSE;
    deliver = op->deliver_pending;
    op->deliver_pending = FALSE;
    LeaveCriticalSection( &async_ops_cs );

    /* Now, and not before: the operation's state is settled and this call is
     * on its way out, so the title is past the point where the callback could
     * catch it mid-setup. */
    if (deliver)
    {
        TRACE( "delivering the held completion for block %p.\n", asyncBlock );
        deliver_completion( &op->route, op->cleanup_on_release ? op : NULL );
    }
    release_async_op( op );
    return S_OK;
}

static HRESULT WINAPI __PADDING__( IXThreadingImpl *iface )
{
    WARN( "iface %p padding function called! It's unknown what this function does.\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_threading_XAsyncSchedule( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, UINT32 delayInMs )
{
    struct x_async_op *op;
    HRESULT hr;

    TRACE( "iface %p, asyncBlock %p, delayInMs %d.\n", iface, asyncBlock, delayInMs );

    if (!asyncBlock) return E_INVALIDARG;

    if (!(op = acquire_async_op( asyncBlock ))) return E_INVALIDARG;
    hr = schedule_async_work( op, delayInMs );
    release_async_op( op );
    return hr;
}

static void WINAPI x_threading_XAsyncComplete( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, HRESULT result, SIZE_T requiredBufferSize )
{
    /* The caller's address distinguishes "the same call site ran twice with a
     * stale pointer" from "a different code path", which is the whole question
     * when a block arrives that no operation matches. */
    TRACE( "iface %p, asyncBlock %p, result %#lx, requiredBufferSize %Iu, called from %p.\n",
           iface, asyncBlock, result, requiredBufferSize, __builtin_return_address(0) );
    x_threading_XAsyncComplete_helper( asyncBlock, result, requiredBufferSize );
}

/* Ask the provider to stop. That is all XAsyncCancel does, as in the GDK's
 * own AsyncLib: finishing the operation stays the provider's job, and it
 * reports the cancellation by completing with E_ABORT once its own work has
 * wound down. libHttpClient's HTTP provider answers Cancel by terminating its
 * private work queue, and completes the outer call later from the cancelled
 * request's callback, which still reads the context that its Cleanup frees.
 * Completing the operation here ran that Cleanup -- and delivered the title's
 * callback -- while the provider's cancelled work was still running.
 *
 * This used to be a stub, so the provider never heard about a cancellation at
 * all. An XAsyncRun operation has no provider, and cancelling one does
 * nothing: its work function finishes it. */
static void cancel_async_op( struct x_async_op *op )
{
    BOOL cancel;

    /* One Cancel at a time. While it runs the operation may complete and its
     * result be taken; Cleanup then waits for Cancel to return (see
     * XAsyncGetResult and release_async_op). */
    EnterCriticalSection( &async_ops_cs );
    cancel = !op->completed && !op->canceling && op->provider;
    if (cancel) op->canceling = TRUE;
    LeaveCriticalSection( &async_ops_cs );
    if (!cancel) return;

    TRACE( "cancelling operation %p for block %p.\n", op, op->async );
    async_op_invoke( op, XAsyncOp_Cancel, 0, NULL );

    EnterCriticalSection( &async_ops_cs );
    op->canceling = FALSE;
    LeaveCriticalSection( &async_ops_cs );
}

static void WINAPI x_threading_XAsyncCancel( IXThreadingImpl *iface, XAsyncBlock *asyncBlock )
{
    struct x_async_op *op;

    TRACE( "iface %p, asyncBlock %p.\n", iface, asyncBlock );

    if (!asyncBlock || !(op = acquire_async_op( asyncBlock ))) return;
    cancel_async_op( op );
    release_async_op( op );
}

static HRESULT WINAPI x_threading_XAsyncGetResult( IXThreadingImpl *iface, XAsyncBlock *asyncBlock, const void *identity, SIZE_T bufferSize, void *buffer, SIZE_T *bufferUsed )
{
    struct x_async_op *op;
    void *payload = NULL;
    BOOL defer_cleanup;
    HRESULT hr;

    TRACE( "iface %p asyncBlock %p, identity %p, bufferSize %Iu, buffer %p, bufferUsed %p.\n",
           iface, asyncBlock, identity, bufferSize, buffer, bufferUsed );

    if (!asyncBlock) return E_INVALIDARG;

    EnterCriticalSection( &async_ops_cs );
    /* Outcomes without a payload are detached after completion. Return the
     * block's current status without finding an older operation at its address. */
    if (async_block_completed( asyncBlock, &hr, &payload ) && payload == &no_result_marker)
    {
        if (bufferUsed) *bufferUsed = 0;
        LeaveCriticalSection( &async_ops_cs );
        return hr;
    }
    op = find_async_op( asyncBlock );
    if (!op)
    {
        SIZE_T size;

        LeaveCriticalSection( &async_ops_cs );

        /* Either an operation this DLL satisfied itself, or a provider-driven
         * one whose result has already been taken -- the two are
         * indistinguishable from here, because taking a result removes the op
         * but leaves the block carrying its completion.
         *
         * Either way, returning the stored HRESULT without touching the
         * caller's buffer is not an option. A success that writes nothing
         * leaves the caller reading whatever its result variable held before
         * the call, and a title that pre-poisons a local with -1 then calls a
         * method on it: `mov rbx,[rcx+0x38]` with rcx = 0xFFFFFFFFFFFFFFFF.
         * Same shape as the empty image name and the NULL struct return -- a
         * call that "succeeded" without leaving memory as promised. */
        if (!async_block_completed( asyncBlock, &hr, &payload )) return E_PENDING;
        if (FAILED(hr)) return hr;

        size = (SIZE_T)(ULONG_PTR)asyncBlock->internal[3];
        if (bufferUsed) *bufferUsed = size;
        if (size && payload)
        {
            if (!buffer || bufferSize < size) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
            memcpy( buffer, payload, size );
        }
        else if (buffer && bufferSize)
        {
            /* Success, a buffer asked for, and nothing recorded to put in it.
             * Saying so beats leaving the caller's memory as it found it.
             *
             * A recorded size with a null payload lands here too, and must:
             * a provider-driven block carries its size in the block but its
             * data in the provider, so once the operation is gone there is
             * nothing left to copy from. Trusting the size alone means
             * memcpy() from address zero -- which is not an error the caller
             * can see, it is a fault inside its own memcpy. */
            WARN( "async %p completed with no payload for a %Iu byte buffer (result size %Iu).\n",
                  asyncBlock, bufferSize, size );
            return E_INVALIDARG;
        }
        return hr;
    }
    /* Completed is decided under the lock, but the result reaches the block
     * just after. Taking it in between let the caller free the block before
     * the result was written into it. */
    if (!op->completed || !async_block_completed( asyncBlock, NULL, NULL ))
    {
        LeaveCriticalSection( &async_ops_cs );
        return E_PENDING;
    }
    if (identity && op->identity && identity != op->identity)
    {
        LeaveCriticalSection( &async_ops_cs );
        return E_INVALIDARG;
    }
    hr = op->result;
    if (bufferSize < op->required_size)
    {
        LeaveCriticalSection( &async_ops_cs );
        return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    }
    list_remove( &op->entry );
    /* The provider may still be inside its Cancel, which can complete the
     * operation before it returns. Cleanup frees what that Cancel is using, so
     * it waits for whoever called Cancel to let go of the operation. */
    if ((defer_cleanup = op->canceling))
    {
        op->cleanup_async = *asyncBlock;
        op->cleanup_on_release = TRUE;
    }
    LeaveCriticalSection( &async_ops_cs );
    TRACE( "operation %p withdrawn: its result is being taken from block %p.\n", op, asyncBlock );

    if (SUCCEEDED(hr) && op->required_size)
    {
        TRACE( "asking the provider for the result of %p.\n", asyncBlock );
        async_op_invoke( op, XAsyncOp_GetResult, bufferSize, buffer );
        TRACE( "provider filled the result of %p.\n", asyncBlock );
    }
    if (bufferUsed) *bufferUsed = op->required_size;

    if (!defer_cleanup) async_op_invoke( op, XAsyncOp_Cleanup, 0, NULL );
    release_async_op( op );
    return hr;
}

/* Composite queues share endpoint identities with their parent queues. */
struct x_queue_port
{
    struct x_task_queue *owner;
    XTaskQueuePort kind;
};

struct x_task_queue
{
    LONG ref;
    struct x_queue_port owned_ports[2];
    struct x_queue_port *ports[2];
    XTaskQueueDispatchMode work_mode;
    XTaskQueueDispatchMode completion_mode;
    struct list pending;        /* callbacks submitted but not run yet */
    struct list delayed;        /* callbacks waiting out a delay on a pool timer */
    struct list terminate_requests; /* terminated callbacks owed once it goes quiet */
    LONG inflight;              /* accepted and not yet returned, see queue_finish */
    BOOL running;               /* a runner is draining `pending` right now */
    BOOL terminated;            /* XTaskQueueTerminate was called */
    BOOL parallel;              /* ThreadPool ports need not run one at a time */
};

/* The GDK keeps a process-wide default task queue. Passing a null handle to an
 * API that takes one is not an error: it means "use that default", and a title
 * may install its own with XTaskQueueSetCurrentProcessTaskQueue. Rejecting null
 * instead is what broke profile lookups -- libHttpClient asks for the null
 * queue's port, got E_INVALIDARG, and Bedrock turned that into a failed profile
 * load followed by an orderly shutdown. */
static struct x_task_queue *process_queue;

/* ThreadPool queues run their callbacks one at a time, like SerializedThreadPool
 * ones; queue_runner says why. XGR_PARALLEL_THREADPOOL_QUEUES=1 gives back the
 * GDK's concurrent dispatch to a title that needs it -- one whose callback
 * waits for another callback on the same queue deadlocks when serialised. It
 * is read as each queue is created, so it can be set per title. */
static BOOL parallel_threadpool_requested( void )
{
    char value[2];
    return GetEnvironmentVariableA( "XGR_PARALLEL_THREADPOOL_QUEUES", value, sizeof(value) ) == 1 &&
           value[0] == '1';
}

static void init_queue_ports( struct x_task_queue *queue )
{
    unsigned int i;
    for (i = 0; i < 2; ++i)
    {
        queue->owned_ports[i].owner = queue;
        queue->owned_ports[i].kind = i;
        queue->ports[i] = &queue->owned_ports[i];
    }
    list_init( &queue->pending );
    list_init( &queue->delayed );
    list_init( &queue->terminate_requests );
    queue->parallel = parallel_threadpool_requested();
}


static BOOL WINAPI create_process_queue( INIT_ONCE *once, void *param, void **ctx )
{
    struct x_task_queue *obj;

    if (process_queue) return TRUE;    /* the title installed one first */
    if (!(obj = calloc( 1, sizeof(*obj) ))) return FALSE;
    obj->ref = 1;
    init_queue_ports( obj );
    obj->work_mode = XTaskQueueDispatchMode_ThreadPool;
    obj->completion_mode = XTaskQueueDispatchMode_ThreadPool;
    process_queue = obj;
    return TRUE;
}

static struct x_task_queue *default_task_queue( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

    InitOnceExecuteOnce( &once, create_process_queue, NULL, NULL );
    return process_queue;
}

static struct x_queue_port *queue_port( XTaskQueueHandle queue, XTaskQueuePort port )
{
    struct x_task_queue *obj = (struct x_task_queue *)queue;
    if (!obj) obj = default_task_queue();
    return obj && (port == XTaskQueuePort_Work || port == XTaskQueuePort_Completion) ? obj->ports[port] : NULL;
}

static XTaskQueueDispatchMode work_mode_for( XTaskQueueHandle queue )
{
    struct x_queue_port *port = queue_port( queue, XTaskQueuePort_Work );
    if (!port) return XTaskQueueDispatchMode_ThreadPool;
    return port->kind == XTaskQueuePort_Work ? port->owner->work_mode : port->owner->completion_mode;
}

/* --- what a queue still owes ----------------------------------------------
 *
 * The terminated callback is the title's signal that nothing else will ever be
 * called back on that queue, and it acts on it: libHttpClient frees the state
 * its pending retries point at. So a queue counts everything it has accepted
 * and not finished -- callbacks waiting to run or waiting out a delay, the one
 * running now, provider work, and completions handed to the pool for blocks
 * that name it -- and termination is complete only when that count reaches
 * zero. Before this, the callback fired the moment XTaskQueueTerminate was
 * called, and a delayed retry fired two seconds later into freed memory. */
static CRITICAL_SECTION queue_work_cs;
static CRITICAL_SECTION_DEBUG queue_work_cs_debug =
{
    0, 0, &queue_work_cs,
    { &queue_work_cs_debug.ProcessLocksList, &queue_work_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": queue_work_cs") }
};
static CRITICAL_SECTION queue_work_cs = { &queue_work_cs_debug, -1, 0, 0, 0, 0 };

struct x_terminate_request
{
    struct list entry;
    void *context;
    XTaskQueueTerminatedCallback *callback;
    XAsyncBlock async;
};

static void deliver_termination( struct x_task_queue *, struct x_terminate_request * );

/* Count one more thing the queue owes, whether or not it is terminated: a
 * completion still has to reach its callback after termination. */
static void queue_hold( struct x_task_queue *queue )
{
    EnterCriticalSection( &queue_work_cs );
    InterlockedIncrement( &queue->inflight );
    LeaveCriticalSection( &queue_work_cs );
}

/* One of them has returned. The last one out of a terminated queue owes the
 * title its terminated callbacks through the Completion port. */
static void queue_finish( struct x_task_queue *queue )
{
    struct x_terminate_request *request, *next;
    struct list owed;
    BOOL idle;

    list_init( &owed );
    EnterCriticalSection( &queue_work_cs );
    idle = !InterlockedDecrement( &queue->inflight ) && queue->terminated;
    if (idle) list_move_tail( &owed, &queue->terminate_requests );
    LeaveCriticalSection( &queue_work_cs );
    if (!idle) return;

    wake_waiters();
    LIST_FOR_EACH_ENTRY_SAFE( request, next, &owed, struct x_terminate_request, entry )
    {
        deliver_termination( queue, request );
    }
}

/* Provider work that must not run where DoWork was dispatched.
 *
 * In strict manual mode an operation's DoWork runs on the title's own Work
 * port, so a provider whose work blocks -- a service round trip with a long
 * deadline -- blocks the title's dispatch thread and everything queued behind
 * it. Such a provider hands the blocking part here from DoWork and returns
 * E_PENDING; the work runs on the pool, or a thread of its own when the pool
 * refuses, and completes the operation itself.
 *
 * It is still the queue's work, exactly as provider work is in the other
 * modes: the block's queue counts it until it returns, so terminating the
 * queue waits for it, and the terminated callback cannot overtake its
 * completion. */
struct x_detached_work
{
    struct x_task_queue *queue;
    PTP_SIMPLE_CALLBACK callback;
    void *context;
};

static void CALLBACK detached_work_callback( TP_CALLBACK_INSTANCE *instance, void *arg )
{
    struct x_detached_work *work = arg;
    struct x_task_queue *queue = work->queue;

    work->callback( instance, work->context );
    free( work );
    if (queue) queue_finish( queue );
}

HRESULT xgr_async_run_detached( XAsyncBlock *async, PTP_SIMPLE_CALLBACK callback, void *context )
{
    struct x_detached_work *work;
    struct x_async_op *op;
    HRESULT hr;

    if (!async || !callback) return E_INVALIDARG;
    if (!(work = calloc( 1, sizeof(*work) ))) return E_OUTOFMEMORY;
    work->callback = callback;
    work->context = context;
    if ((op = acquire_async_op( async )))
    {
        work->queue = (struct x_task_queue *)op->route.queue;
        release_async_op( op );
    }
    if (work->queue) queue_hold( work->queue );

    if (TrySubmitThreadpoolCallback( detached_work_callback, work, NULL )) return S_OK;
    if (SUCCEEDED(hr = run_off_pool( detached_work_callback, work ))) return S_OK;

    if (work->queue) queue_finish( work->queue );
    free( work );
    return hr;
}

/* Block until everything the queue owes has returned.
 *
 * Work still running can complete an operation on this queue while we wait,
 * and on a Manual completion port that completion is parked for whoever pumps
 * the queue -- usually the very thread waiting here, which XSAPI's shape
 * (thread-pool work, Manual completions) makes the common case. The parked
 * completion is part of the count, so it has to be delivered from here, the
 * way XAsyncGetStatus(wait) does, or the wait never ends. */
static void wait_until_quiet( struct x_task_queue *queue )
{
    XTaskQueueHandle handle = (XTaskQueueHandle)queue;

    EnterCriticalSection( &wait_cs );
    InterlockedIncrement( &waiters );
    while (InterlockedCompareExchange( &queue->inflight, 0, 0 ))
    {
        BOOL delivered;

        LeaveCriticalSection( &wait_cs );
        delivered = deliver_one_completion( handle, XTaskQueuePort_Completion, TRUE ) ||
                    deliver_one_completion( handle, XTaskQueuePort_Work, TRUE );
        EnterCriticalSection( &wait_cs );
        if (delivered) continue;
        /* Woken when the count reaches zero or a completion is parked. */
        SleepConditionVariableCS( &wait_cv, &wait_cs, 1000 );
    }
    InterlockedDecrement( &waiters );
    LeaveCriticalSection( &wait_cs );
}

/* Which queues' callbacks the current thread is inside, innermost first. A
 * callback that terminates its own queue and asks to wait would otherwise wait
 * for itself. */
struct x_running_frame
{
    struct x_task_queue *queue;
    struct x_running_frame *outer;
};

static DWORD running_tls = TLS_OUT_OF_INDEXES;

static BOOL WINAPI init_running_tls( INIT_ONCE *once, void *param, void **context )
{
    running_tls = TlsAlloc();
    return TRUE;
}

static DWORD running_slot( void )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce( &once, init_running_tls, NULL, NULL );
    return running_tls;
}

static void enter_frame( struct x_running_frame *frame, struct x_task_queue *queue )
{
    DWORD slot = running_slot();

    frame->queue = queue;
    frame->outer = NULL;
    if (slot == TLS_OUT_OF_INDEXES) return;
    frame->outer = TlsGetValue( slot );
    TlsSetValue( slot, frame );
}

static void leave_frame( struct x_running_frame *frame )
{
    DWORD slot = running_slot();
    if (slot != TLS_OUT_OF_INDEXES) TlsSetValue( slot, frame->outer );
}

static BOOL running_on( const struct x_task_queue *queue )
{
    DWORD slot = running_slot();
    struct x_running_frame *frame;

    if (slot == TLS_OUT_OF_INDEXES) return FALSE;
    for (frame = TlsGetValue( slot ); frame; frame = frame->outer)
        if (frame->queue == queue) return TRUE;
    return FALSE;
}

/* Completions waiting to be handed to whoever pumps the queue. */
struct x_pending_completion
{
    struct list entry;
    struct list wait_entry;
    XAsyncBlock *async;
    XAsyncCompletionRoutine *callback;
    struct x_async_op *keep_alive;
    struct x_queue_port *port;
    XTaskQueueHandle origin;
    struct x_task_queue *owner;     /* the queue the block named, which waits for this */
    DWORD callback_thread;
    BOOL wait_for_callback;
};

static struct list waited_callbacks = LIST_INIT( waited_callbacks );
static LONG waited_callback_count;

static BOOL callback_blocks_wait( const XAsyncBlock *async )
{
    const struct x_pending_completion *item;
    DWORD thread;
    BOOL pending = FALSE;

    if (!InterlockedCompareExchange( &waited_callback_count, 0, 0 )) return FALSE;
    thread = GetCurrentThreadId();
    EnterCriticalSection( &wait_cs );
    LIST_FOR_EACH_ENTRY( item, &waited_callbacks, struct x_pending_completion, wait_entry )
    {
        if (item->async != async) continue;
        /* A callback can query the status of the operation that called it. */
        pending = item->callback_thread != thread;
        break;
    }
    LeaveCriticalSection( &wait_cs );
    return pending;
}

/* Set once a title actually pumps a queue. Until then a Manual completion has
 * nothing that will ever drain it -- see xgr_async_complete_ex. */
static LONG queue_ever_dispatched;

static struct list pending_completions = LIST_INIT( pending_completions );
static HANDLE pending_event;   /* signalled while pending_completions is non-empty */
static LONG pending_count;     /* lock-free fast path for the common empty case */
static CRITICAL_SECTION pending_cs;
static CRITICAL_SECTION_DEBUG pending_cs_debug =
{
    0, 0, &pending_cs,
    { &pending_cs_debug.ProcessLocksList, &pending_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": pending_cs") }
};
static CRITICAL_SECTION pending_cs = { &pending_cs_debug, -1, 0, 0, 0, 0 };

/* Who is responsible for running this block's completion callback.
 *
 * A queue handle is the queue object itself. A Manual queue is pumped by the
 * title, so its completions wait for XTaskQueueDispatch; for every other mode
 * the runtime has to deliver them itself. A block with no queue is treated as
 * runtime-driven in compatibility mode; strict mode uses the process default. */
static XTaskQueueDispatchMode completion_mode_for( const XAsyncBlock *async )
{
    const struct x_task_queue *obj = (const struct x_task_queue *)async->queue;

    /* A block with no queue of its own is completed on the thread pool, even
     * when a process default queue is installed. Deliberately NOT the process
     * queue's mode: Bedrock installs a Manual queue as the process default and
     * then never calls XTaskQueueDispatch even once, so honouring that mode
     * here parks the completion for ever. That stranded half of its
     * XUserAddAsync calls, sign-in never finished, and everything needing a
     * token -- profile, presence, loading a saved world -- stopped working. */
    if (!obj && strict_manual_dispatch()) obj = default_task_queue();
    if (!obj) return XTaskQueueDispatchMode_ThreadPool;
    return obj->completion_mode;
}

static void capture_completion_route( const XAsyncBlock *async, struct x_completion_route *route )
{
    route->async = (XAsyncBlock *)async;
    route->callback = async->callback;
    route->queue = async->queue;
    route->port = queue_port( async->queue, XTaskQueuePort_Completion );
    route->origin = async->queue ? async->queue : (XTaskQueueHandle)default_task_queue();
    route->mode = completion_mode_for( async );
}

static void invoke_async_completion( struct x_pending_completion *item )
{
    struct x_task_queue *owner = item->owner;
    struct x_running_frame frame;

    if (item->wait_for_callback)
    {
        EnterCriticalSection( &wait_cs );
        item->callback_thread = GetCurrentThreadId();
        LeaveCriticalSection( &wait_cs );
    }
    enter_frame( &frame, owner );
    item->callback( item->async );
    leave_frame( &frame );
    if (item->wait_for_callback)
    {
        EnterCriticalSection( &wait_cs );
        list_remove( &item->wait_entry );
        InterlockedDecrement( &waited_callback_count );
        LeaveCriticalSection( &wait_cs );
        wake_waiters();
    }
    release_async_op( item->keep_alive );
    free( item );
    if (owner) queue_finish( owner );
}

static void CALLBACK async_completion_work( TP_CALLBACK_INSTANCE *instance, void *context )
{
    struct x_pending_completion *item = context;
    TRACE( "delivering completion for block %p (pool).\n", item->async );
    invoke_async_completion( item );
}

static DWORD WINAPI async_completion_thread( void *context )
{
    struct x_pending_completion *item = context;
    TRACE( "delivering completion for block %p (thread).\n", item->async );
    invoke_async_completion( item );
    return 0;
}

/* Write an outcome into a block without telling anyone. */
static void fill_async_result( XAsyncBlock *async, HRESULT hr, void *payload, SIZE_T result_size )
{
    async->internal[0] = XGR_ASYNC_MAGIC;
    async->internal[1] = (void *)(ULONG_PTR)hr;
    async->internal[2] = payload;
    async->internal[3] = (void *)result_size;
}

/* Record an outcome in the block without delivering the callback. Anything
 * polling the block -- XAsyncGetStatus, XAsyncGetResult -- sees it at once;
 * only the callback waits.
 *
 * The marker goes in last, with a full barrier, so a poller that sees it also
 * sees the HRESULT and payload rather than whatever was there before. After
 * that the block belongs to its caller again, and nothing here reads it. */
static void store_async_result( XAsyncBlock *async, HRESULT hr, void *payload, SIZE_T result_size )
{
    async->internal[1] = (void *)(ULONG_PTR)hr;
    async->internal[2] = payload;
    async->internal[3] = (void *)result_size;
    InterlockedExchangePointer( &async->internal[0], XGR_ASYNC_MAGIC );
    wake_waiters();
}

static struct x_pending_completion *prepare_completion( const struct x_completion_route *route,
                                                        struct x_async_op *keep_alive )
{
    struct x_pending_completion *item;

    if (!route->callback) return NULL;
    if (!(item = calloc( 1, sizeof(*item) ))) return NULL;
    item->async = route->async;
    item->callback = route->callback;
    item->keep_alive = keep_alive;
    item->port = route->port;
    item->origin = route->origin;
    if (keep_alive) InterlockedIncrement( &keep_alive->ref );
    if ((item->owner = (struct x_task_queue *)route->queue)) queue_hold( item->owner );
    return item;
}

static void dispatch_completion( struct x_pending_completion *item, XTaskQueueDispatchMode mode )
{
    /* A Manual queue is the title's to pump. Bedrock, though, creates Manual
     * queues, hands them to XSAPI and never calls XTaskQueueDispatch even once,
     * so everything parked on one is lost: across a run 42 Xbl* operations were
     * begun and not one completed, which is why the profile never loaded while
     * the HTTP calls under it all succeeded. Until a title shows it pumps, treat
     * Manual as thread-pool delivery rather than park a completion for ever. */

    /* Titles using a runtime-dispatched queue never call XTaskQueueDispatch at
     * all, so parking the completion on the pending list would strand it and
     * the title waits for an answer that never comes (Minecraft asks for its
     * game licence on such a queue and shuts down cleanly when it times out).
     * Hand those to the thread pool rather than running them inline, which
     * would re-enter the caller's own *Async() frame. */
    if (mode != XTaskQueueDispatchMode_Manual ||
        (!strict_manual_dispatch() && !InterlockedCompareExchange( &queue_ever_dispatched, 0, 0 )))
    {
        HANDLE thread;

        if (TrySubmitThreadpoolCallback( async_completion_work, item, NULL )) return;

        /* The pool refused the work. Falling back to the pending list would
         * strand this completion for ever, because a title on a runtime-
         * dispatched queue never calls XTaskQueueDispatch -- so spend a thread
         * on it instead. */
        if ((thread = CreateThread( NULL, 0, (LPTHREAD_START_ROUTINE)async_completion_thread, item, 0, NULL )))
        {
            CloseHandle( thread );
            return;
        }
        ERR( "could not deliver completion for asyncBlock %p.\n", item->async );
    }

    EnterCriticalSection( &pending_cs );
    list_add_tail( &pending_completions, &item->entry );
    InterlockedIncrement( &pending_count );
    if (!pending_event) pending_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    if (pending_event) SetEvent( pending_event );
    LeaveCriticalSection( &pending_cs );
    /* XAsyncGetStatus(wait) delivers these while it blocks. */
    wake_waiters();
}

static void deliver_completion( const struct x_completion_route *route, struct x_async_op *keep_alive )
{
    struct x_pending_completion *item = prepare_completion( route, keep_alive );
    if (item) dispatch_completion( item, route->mode );
}

static void CALLBACK invoke_termination( XAsyncBlock *async )
{
    struct x_terminate_request *request = CONTAINING_RECORD( async, struct x_terminate_request, async );
    request->callback( request->context );
    free( request );
}

/* A no-wait termination is completed on the queue's Completion port, not on
 * whichever Work callback happened to drain it. In particular, a Manual
 * completion cannot re-enter a title still setting up its shutdown state. The
 * terminated notification is not new work counted against the drained queue. */
static void deliver_termination( struct x_task_queue *queue, struct x_terminate_request *request )
{
    struct x_completion_route route = {0};
    route.async = &request->async;
    route.callback = invoke_termination;
    route.port = queue_port( (XTaskQueueHandle)queue, XTaskQueuePort_Completion );
    route.origin = (XTaskQueueHandle)queue;
    route.mode = queue->completion_mode;
    TRACE_(xqueue)( "queuing terminated callback on Completion port: queue %p mode %d context %p.\n",
                   queue, route.mode, request->context );
    deliver_completion( &route, NULL );
}

static BOOL WINAPI init_blocking_callbacks( INIT_ONCE *once, void *param, void **context )
{
    char value[2];

    *context = (void *)(ULONG_PTR)(GetEnvironmentVariableA( "XGR_WAIT_FOR_ASYNC_CALLBACKS", value,
                                 sizeof(value) ) == 1 && value[0] == '1' ? 4 : 0);
    return TRUE;
}

static void complete_and_deliver( const struct x_completion_route *route, HRESULT hr, void *payload, SIZE_T size )
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    struct x_pending_completion *item = NULL;
    void *enabled;

    if (route->callback && (route->mode == XTaskQueueDispatchMode_ThreadPool ||
                            route->mode == XTaskQueueDispatchMode_SerializedThreadPool))
    {
        InitOnceExecuteOnce( &once, init_blocking_callbacks, NULL, &enabled );
        if (enabled && (item = prepare_completion( route, NULL )))
        {
            /* Register the callback before a waiter can see the result. */
            item->wait_for_callback = TRUE;
            EnterCriticalSection( &wait_cs );
            list_add_head( &waited_callbacks, &item->wait_entry );
            InterlockedIncrement( &waited_callback_count );
            LeaveCriticalSection( &wait_cs );
        }
    }
    store_async_result( route->async, hr, payload, size );
    if (item) dispatch_completion( item, route->mode );
    else deliver_completion( route, NULL );
}

void xgr_async_complete_ex( XAsyncBlock *async, HRESULT hr, void *payload, SIZE_T result_size )
{
    struct x_completion_route route;

    if (!async) return;
    capture_completion_route( async, &route );
    complete_and_deliver( &route, hr, payload, result_size );
}

void xgr_async_complete( XAsyncBlock *async, HRESULT hr, void *payload )
{
    xgr_async_complete_ex( async, hr, payload, 0 );
}

/* Hand one queued completion to its callback. Returns TRUE if work was done. */
static BOOL deliver_one_completion( XTaskQueueHandle queue, XTaskQueuePort port, BOOL own_only )
{
    struct x_pending_completion *item = NULL;
    struct x_pending_completion *candidate;
    struct x_queue_port *target = strict_manual_dispatch() ? queue_port( queue, port ) : NULL;

    /* Titles poll this extremely hard -- millions of calls per session, from
     * several threads at once. Taking the lock every time makes the queue a
     * contention point and burns the cores the render thread needs, so check
     * the count first and only lock when there is actually something to hand
     * over. A stale read just means the item is picked up on the next poll. */
    if (!InterlockedCompareExchange( &pending_count, 0, 0 )) return FALSE;

    EnterCriticalSection( &pending_cs );
    LIST_FOR_EACH_ENTRY( candidate, &pending_completions, struct x_pending_completion, entry )
    {
        if (target && candidate->port != target) continue;
        if (own_only && candidate->origin != queue) continue;
        item = candidate;
        list_remove( &item->entry );
        InterlockedDecrement( &pending_count );
        break;
    }
    LeaveCriticalSection( &pending_cs );

    if (!item)
    {
        if (pending_event) ResetEvent( pending_event );
        return FALSE;
    }

    TRACE( "delivering completion for block %p (dispatch).\n", item->async );
    invoke_async_completion( item );
    return TRUE;
}

static HRESULT WINAPI x_threading_XTaskQueueCreate( IXThreadingImpl *iface, XTaskQueueDispatchMode workDispatchMode, XTaskQueueDispatchMode completionDispatchMode, XTaskQueueHandle *queue )
{
    struct x_task_queue *obj;

    TRACE( "iface %p, workDispatchMode %d, completionDispatchMode %d, queue %p.\n", iface, workDispatchMode, completionDispatchMode, queue );

    if (!queue) return E_INVALIDARG;
    if (!(obj = calloc( 1, sizeof(*obj) ))) return E_OUTOFMEMORY;

    obj->ref = 1;
    init_queue_ports( obj );
    obj->work_mode = workDispatchMode;
    obj->completion_mode = completionDispatchMode;

    *queue = (XTaskQueueHandle)obj;
    return S_OK;
}

static HRESULT WINAPI x_threading_XTaskQueueCreateComposite( IXThreadingImpl *iface, XTaskQueuePortHandle workPort, XTaskQueuePortHandle completionPort, XTaskQueueHandle *queue )
{
    struct x_task_queue *obj;

    TRACE( "iface %p, workPort %p, completionPort %p, queue %p.\n", iface, workPort, completionPort, queue );

    /* Ports are the queue itself here, so a composite is just another queue --
     * but it has to inherit its ports' dispatch modes, or a composite built
     * from thread-pool ports would look manually dispatched and its completions
     * would never be delivered. */
    if (!queue) return E_INVALIDARG;
    if (!(obj = calloc( 1, sizeof(*obj) ))) return E_OUTOFMEMORY;
    obj->ref = 1;
    init_queue_ports( obj );
    if (workPort)
    {
        struct x_queue_port *port = (struct x_queue_port *)workPort;
        obj->ports[XTaskQueuePort_Work] = port;
        obj->work_mode = port->kind == XTaskQueuePort_Work ? port->owner->work_mode : port->owner->completion_mode;
    }
    if (completionPort)
    {
        struct x_queue_port *port = (struct x_queue_port *)completionPort;
        obj->ports[XTaskQueuePort_Completion] = port;
        obj->completion_mode = port->kind == XTaskQueuePort_Work ? port->owner->work_mode : port->owner->completion_mode;
    }
    *queue = (XTaskQueueHandle)obj;
    return S_OK;
}

static HRESULT WINAPI x_threading_XTaskQueueGetPort( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, XTaskQueuePortHandle *portHandle )
{
    TRACE( "iface %p, queue %p, port %d, portHandle %p.\n", iface, queue, port, portHandle );

    /* Each endpoint retains its owning queue and dispatch mode. */
    if (!portHandle || (port != XTaskQueuePort_Work && port != XTaskQueuePort_Completion)) return E_INVALIDARG;
    if (!queue && !(queue = (XTaskQueueHandle)default_task_queue())) return E_OUTOFMEMORY;
    *portHandle = (XTaskQueuePortHandle)queue_port( queue, port );
    return S_OK;
}

static HRESULT WINAPI x_threading_XTaskQueueDuplicateHandle( IXThreadingImpl *iface, XTaskQueueHandle queueHandle, XTaskQueueHandle *duplicatedHandle )
{
    struct x_task_queue *obj = (struct x_task_queue *)queueHandle;

    TRACE( "iface %p, queueHandle %p, duplicatedHandle %p.\n", iface, queueHandle, duplicatedHandle );

    if (!obj || !duplicatedHandle) return E_INVALIDARG;
    InterlockedIncrement( &obj->ref );
    *duplicatedHandle = queueHandle;
    return S_OK;
}

static BOOLEAN WINAPI x_threading_XTaskQueueDispatch( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, UINT32 timeoutInMs )
{
    TRACE( "iface %p, queue %p, port %d, timeoutInMs %d.\n", iface, queue, port, timeoutInMs );

    InterlockedExchange( &queue_ever_dispatched, 1 );

    if (strict_manual_dispatch() && dispatch_manual_work( queue, port )) return TRUE;
    if (deliver_one_completion(queue, port, FALSE)) return TRUE;

    /* Callers pump this in a loop. Returning immediately turns that into a
     * busy-wait that saturates a core and starves the rest of the title, so
     * honour the timeout and block until something is queued. */
    /* Cap the wait hard. Callers pump this from threads that must keep running
     * (a title's main/render loop among them), so honouring a long timeout
     * stalls the frame; returning instantly instead turns the pump into a
     * busy-wait that saturates a core. A short bounded wait avoids both. */
    if (timeoutInMs)
    {
        ULONG wait = min( timeoutInMs, 2 );

        if (pending_event)
        {
            if (WaitForSingleObject( pending_event, wait ) == WAIT_OBJECT_0)
                return deliver_one_completion(queue, port, FALSE);
        }
        else Sleep( wait );
    }
    return FALSE;
}

static void WINAPI x_threading_XTaskQueueCloseHandle( IXThreadingImpl *iface, XTaskQueueHandle queue )
{
    struct x_task_queue *obj = (struct x_task_queue *)queue;

    TRACE( "iface %p, queue %p.\n", iface, queue );

    /* Retain queue objects and their embedded endpoints after the last close.
     * Port handles carry no reference, and composite queues share endpoints;
     * freeing the owner here would leave those handles dangling. */
    if (obj) InterlockedDecrement( &obj->ref );
}

struct x_manual_callback
{
    struct list entry;
    struct x_queue_port *port;
    struct x_task_queue *origin;
    ULONGLONG ready_at;
    void *context;
    XTaskQueueCallback *callback;
};

static struct list manual_callbacks = LIST_INIT( manual_callbacks );
static CRITICAL_SECTION manual_cs;
static CRITICAL_SECTION_DEBUG manual_cs_debug =
{
    0, 0, &manual_cs,
    { &manual_cs_debug.ProcessLocksList, &manual_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": manual_cs") }
};
static CRITICAL_SECTION manual_cs = { &manual_cs_debug, -1, 0, 0, 0, 0 };

/* A manual callback is the queue's work like any other: its queue counted it
 * when it was accepted, and it is counted until it returns -- not until it is
 * taken off the list -- so terminating the queue waits for a callback that a
 * title thread is inside right now. The frame makes a callback that
 * terminates its own queue and asks to wait not wait for itself. */
static void run_manual_callback( struct x_manual_callback *item, BOOL canceled )
{
    struct x_task_queue *origin = item->origin;
    struct x_running_frame frame;

    enter_frame( &frame, origin );
    item->callback( item->context, canceled );
    leave_frame( &frame );
    free( item );
    queue_finish( origin );
}

static BOOL dispatch_manual_work( XTaskQueueHandle queue, XTaskQueuePort port )
{
    struct x_queue_port *target = queue_port( queue, port );
    struct x_manual_callback *item, *found = NULL;
    EnterCriticalSection( &manual_cs );
    LIST_FOR_EACH_ENTRY( item, &manual_callbacks, struct x_manual_callback, entry )
    {
        if (item->port != target) continue;
        if (!item->origin->terminated && !item->port->owner->terminated &&
            GetTickCount64() < item->ready_at) continue;
        found = item;
        list_remove( &item->entry );
        break;
    }
    LeaveCriticalSection( &manual_cs );
    if (!found) return FALSE;
    run_manual_callback( found, found->origin->terminated || found->port->owner->terminated );
    return TRUE;
}

static HRESULT enqueue_manual_callback( struct x_task_queue *queue, XTaskQueuePort port,
                                        void *context, XTaskQueueCallback *callback, UINT32 delay_ms )
{
    struct x_manual_callback *item;
    if (!(item = calloc( 1, sizeof(*item) ))) return E_OUTOFMEMORY;
    item->port = queue_port( (XTaskQueueHandle)queue, port );
    item->origin = queue;
    item->ready_at = GetTickCount64() + delay_ms;
    item->context = context;
    item->callback = callback;
    /* Under the queue lock, as queue_submit does, so termination either sees
     * the item counted and listed -- and cancels it -- or refuses it. */
    EnterCriticalSection( &queue_work_cs );
    if (queue->terminated || item->port->owner->terminated)
    {
        LeaveCriticalSection( &queue_work_cs );
        free( item );
        return E_ABORT;
    }
    InterlockedIncrement( &queue->inflight );
    EnterCriticalSection( &manual_cs );
    list_add_tail( &manual_callbacks, &item->entry );
    LeaveCriticalSection( &manual_cs );
    LeaveCriticalSection( &queue_work_cs );
    return S_OK;
}

/* Termination owns callbacks submitted through this queue, even when its
 * endpoints are shared with a parent or another composite. */
static void cancel_manual_callbacks( struct x_task_queue *queue )
{
    struct x_manual_callback *item, *next;
    struct list drained;
    list_init( &drained );
    EnterCriticalSection( &manual_cs );
    LIST_FOR_EACH_ENTRY_SAFE( item, next, &manual_callbacks, struct x_manual_callback, entry )
    {
        if (item->origin != queue) continue;
        list_remove( &item->entry );
        list_add_tail( &drained, &item->entry );
    }
    LeaveCriticalSection( &manual_cs );
    while (!list_empty( &drained ))
    {
        item = LIST_ENTRY( list_head( &drained ), struct x_manual_callback, entry );
        list_remove( &item->entry );
        run_manual_callback( item, TRUE );
    }
}

/* A task queue port is a serial execution context: the GDK runs one callback
 * at a time on it, and a title dispatching a Manual queue itself necessarily
 * runs them one after another. We used to hand every callback straight to the
 * Win32 thread pool, so callbacks belonging to one queue ran concurrently on as
 * many threads as the pool felt like.
 *
 * libHttpClient does not survive that. Ten different threads were submitting to
 * a single queue during a server join; two of its callbacks ran at once, one
 * finished the HTTP call and freed it while the other was still inside it, and
 * the freed block was immediately reused for a response body -- a thread ended
 * up parked for ever on a std::mutex whose memory had become the text
 * {"users":[]}, still holding a second mutex that the game's main thread then
 * waited on. That is the multiplayer join freeze.
 *
 * So callbacks are queued per task queue and drained by a single runner. That
 * includes ThreadPool queues, which the GDK would run concurrently; see
 * parallel_threadpool_requested for the way out. Provider work does not go
 * through the runner (see schedule_async_work). */
struct x_queue_work
{
    struct list entry;
    struct x_task_queue *queue;     /* counted against this queue, or NULL */
    XTaskQueuePort port;
    void *context;
    XTaskQueueCallback *callback;
    PTP_TIMER timer;                /* serves the delay, if it has one */
    BOOL armed;                     /* listed on queue->delayed, its timer running */
    BOOL serial;                    /* joins the queue's runner rather than the pool */
};

static void kick_queue_runner( struct x_task_queue *queue );

static BOOL runs_serially( const struct x_task_queue *queue, XTaskQueuePort port )
{
    XTaskQueueDispatchMode mode = port == XTaskQueuePort_Work ? queue->work_mode : queue->completion_mode;
    return !(queue->parallel && mode == XTaskQueueDispatchMode_ThreadPool);
}

static BOOL queue_terminated( const struct x_task_queue *queue )
{
    BOOL terminated;

    if (!queue) return FALSE;
    EnterCriticalSection( &queue_work_cs );
    terminated = queue->terminated;
    LeaveCriticalSection( &queue_work_cs );
    return terminated;
}

/* `canceled` is the whole point of the flag: after a terminate, a callback
 * must be told it is being cancelled rather than run down its success path
 * against objects the title has already destroyed. */
static void run_queue_item( struct x_queue_work *item, BOOL canceled )
{
    struct x_task_queue *queue = item->queue;
    struct x_running_frame frame;

    enter_frame( &frame, queue );
    item->callback( item->context, canceled );
    leave_frame( &frame );
    /* Possibly from inside the timer's own callback, which is allowed: the
     * pool frees it once that callback has returned. */
    if (item->timer) CloseThreadpoolTimer( item->timer );
    free( item );
    if (queue) queue_finish( queue );
}

/* Runs one queue's callbacks, one at a time, until it runs dry. */
static void CALLBACK queue_runner( TP_CALLBACK_INSTANCE *instance, void *context )
{
    struct x_task_queue *queue = context;

    for (;;)
    {
        struct x_queue_work *item = NULL;
        struct list *head;
        BOOL canceled;

        EnterCriticalSection( &queue_work_cs );
        canceled = queue->terminated;
        if ((head = list_head( &queue->pending )))
        {
            item = LIST_ENTRY( head, struct x_queue_work, entry );
            list_remove( &item->entry );
        }
        else queue->running = FALSE;
        LeaveCriticalSection( &queue_work_cs );

        if (!item) return;
        run_queue_item( item, canceled );
    }
}

/* A thread entry point takes one argument; a thread-pool callback takes two.
 * Casting one to the other put the queue in `instance` and left `context` --
 * which is what the body actually uses -- holding whatever the ABI happened to
 * leave in the second register. The queue pointer was then garbage, and the
 * first thing the runner does is dereference it. */
static DWORD WINAPI queue_runner_thread( void *context )
{
    queue_runner( NULL, context );
    return 0;
}

static void kick_queue_runner( struct x_task_queue *queue )
{
    BOOL start = FALSE;

    EnterCriticalSection( &queue_work_cs );
    if (!queue->running && !list_empty( &queue->pending ))
        start = queue->running = TRUE;
    LeaveCriticalSection( &queue_work_cs );

    if (!start) return;

    if (TrySubmitThreadpoolCallback( queue_runner, queue, NULL )) return;

    /* The pool refused the work. Dropping it would strand every callback still
     * queued behind it, so spend a thread instead -- same reasoning as
     * run_off_pool, which cannot be used here because it takes a simple
     * callback and this one has to keep draining. */
    WARN( "thread pool refused a queue runner; using a thread of its own.\n" );
    {
        HANDLE thread = CreateThread( NULL, 0, queue_runner_thread, queue, 0, NULL );

        if (thread) CloseHandle( thread );
        else
        {
            EnterCriticalSection( &queue_work_cs );
            queue->running = FALSE;
            LeaveCriticalSection( &queue_work_cs );
            ERR( "queue %p has callbacks that cannot be run.\n", queue );
        }
    }
}

static void CALLBACK queue_item_thunk( TP_CALLBACK_INSTANCE *instance, void *context )
{
    struct x_queue_work *item = context;
    run_queue_item( item, queue_terminated( item->queue ) );
}

/* Its delay is up. The item is either still armed, and this callback takes it,
 * or termination has already claimed it: then termination runs it cancelled,
 * and waits for this callback to return before it does, so the item may be
 * read here but not after giving it up. */
static void CALLBACK queue_timer_cb( TP_CALLBACK_INSTANCE *instance, void *context, TP_TIMER *timer )
{
    struct x_queue_work *item = context;
    struct x_task_queue *queue = item->queue;

    if (queue)
    {
        BOOL mine, serial = item->serial;

        EnterCriticalSection( &queue_work_cs );
        if ((mine = item->armed))
        {
            item->armed = FALSE;
            list_remove( &item->entry );
            /* A delayed callback becomes ordinary queued work once its delay
             * is up, so it is still run in isolation. */
            if (serial) list_add_tail( &queue->pending, &item->entry );
        }
        LeaveCriticalSection( &queue_work_cs );

        if (!mine) return;
        if (serial)
        {
            kick_queue_runner( queue );
            return;
        }
    }
    run_queue_item( item, queue_terminated( queue ) );
}

static void arm_timer( PTP_TIMER timer, UINT32 delay_ms )
{
    LARGE_INTEGER due;
    FILETIME ft;

    due.QuadPart = -(LONGLONG)delay_ms * 10000; /* relative, 100ns units */
    ft.dwLowDateTime = due.u.LowPart;
    ft.dwHighDateTime = due.u.HighPart;
    SetThreadpoolTimer( timer, &ft, 0, 0 );
}

/* Accept a callback for a queue: counted against it until it has returned, and
 * refused once it is terminated. `queue` may be null only for work that runs
 * straight on the pool and belongs to no queue's lifetime.
 *
 * A delay is served by a pool timer, never by sleeping on a pool thread: a
 * title that retries hard (libHttpClient reschedules every failed request)
 * would park every worker, and then the completions it is waiting for cannot
 * get a thread at all -- which deadlocks the title outright. A pool timer holds
 * no thread while it waits, and termination can cancel it. */
static HRESULT queue_submit( struct x_task_queue *queue, XTaskQueuePort port, void *context,
                             XTaskQueueCallback *callback, UINT32 delay_ms, BOOL serial )
{
    struct x_queue_work *item;
    HRESULT hr;

    if (!(item = calloc( 1, sizeof(*item) ))) return E_OUTOFMEMORY;
    item->queue = queue;
    item->port = port;
    item->context = context;
    item->callback = callback;
    item->serial = serial && queue;

    if (delay_ms && !(item->timer = CreateThreadpoolTimer( queue_timer_cb, item, NULL )))
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        free( item );
        WARN( "no pool timer available for a %u ms delay.\n", delay_ms );
        return FAILED(hr) ? hr : E_OUTOFMEMORY;
    }

    if (queue)
    {
        EnterCriticalSection( &queue_work_cs );
        /* A terminated queue takes no more work. Accepting it is how a
         * callback ends up running after the title was told nothing more
         * would. */
        if (queue->terminated)
        {
            LeaveCriticalSection( &queue_work_cs );
            if (item->timer) CloseThreadpoolTimer( item->timer );
            free( item );
            return E_ABORT;
        }
        InterlockedIncrement( &queue->inflight );
        if (delay_ms)
        {
            /* Armed under the lock, so termination cannot claim the item
             * before its timer is set. */
            item->armed = TRUE;
            list_add_tail( &queue->delayed, &item->entry );
            arm_timer( item->timer, delay_ms );
        }
        else if (item->serial) list_add_tail( &queue->pending, &item->entry );
        LeaveCriticalSection( &queue_work_cs );
    }
    else if (delay_ms) arm_timer( item->timer, delay_ms );

    if (delay_ms) return S_OK;
    if (item->serial)
    {
        kick_queue_runner( queue );
        return S_OK;
    }

    if (TrySubmitThreadpoolCallback( queue_item_thunk, item, NULL )) return S_OK;
    WARN( "thread pool refused work; running it on a thread of its own.\n" );
    if (SUCCEEDED(hr = run_off_pool( queue_item_thunk, item ))) return S_OK;
    free( item );
    if (queue) queue_finish( queue );
    return hr;
}

static HRESULT enqueue_queue_work( struct x_task_queue *queue, XTaskQueuePort port, void *context,
                                   XTaskQueueCallback *callback, UINT32 delay_ms )
{
    XTaskQueueDispatchMode mode = port == XTaskQueuePort_Work ? queue->work_mode : queue->completion_mode;

    if (strict_manual_dispatch() && mode == XTaskQueueDispatchMode_Manual)
        return enqueue_manual_callback( queue, port, context, callback, delay_ms );
    return queue_submit( queue, port, context, callback, delay_ms, runs_serially( queue, port ) );
}

static HRESULT WINAPI x_threading_XTaskQueueSubmitCallback( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, void *callbackContext, XTaskQueueCallback *callback )
{
    struct x_task_queue *obj = (struct x_task_queue *)queue;

    TRACE( "iface %p, queue %p, port %d, callbackContext %p, callback %p.\n", iface, queue, port, callbackContext, callback );

    if (!callback || (port != XTaskQueuePort_Work && port != XTaskQueuePort_Completion)) return E_INVALIDARG;
    if (!obj) obj = default_task_queue();
    if (!obj) return E_OUTOFMEMORY;
    if (queue_terminated( obj )) return E_ABORT;

    /* Immediate means "run it on the thread that submitted it". */
    if ((port == XTaskQueuePort_Completion ? obj->completion_mode : obj->work_mode) == XTaskQueueDispatchMode_Immediate)
    {
        callback( callbackContext, FALSE );
        return S_OK;
    }
    return enqueue_queue_work( obj, port, callbackContext, callback, 0 );
}

/* A delayed callback is how callers implement retry backoff (libHttpClient
 * reschedules every failed request this way), so it has to actually fire --
 * and off the caller's thread, since the caller is mid-request. */
static HRESULT WINAPI x_threading_XTaskQueueSubmitDelayedCallback( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, UINT32 delayMs, void *callbackContext, XTaskQueueCallback *callback )
{
    struct x_task_queue *obj = (struct x_task_queue *)queue;

    TRACE( "iface %p, queue %p, port %d, delayMs %d, callbackContext %p, callback %p.\n", iface, queue, port, delayMs, callbackContext, callback );

    if (!callback || (port != XTaskQueuePort_Work && port != XTaskQueuePort_Completion)) return E_INVALIDARG;
    if (!obj) obj = default_task_queue();
    if (!obj) return E_OUTOFMEMORY;
    if (queue_terminated( obj )) return E_ABORT;
    return enqueue_queue_work( obj, port, callbackContext, callback, delayMs );
}

/* A waiter fires a task-queue callback when a wait handle becomes signalled.
 * This is how a title drives an event-based subsystem (e.g. GameInput's
 * dispatcher wait handle) from its own task queue instead of a busy loop. We
 * back it with the Win32 wait-thread pool: RegisterWaitForSingleObject invokes
 * our thunk when the handle signals, and we run the caller's callback. */
struct x_waiter
{
    HANDLE wait;
    void *context;
    XTaskQueueCallback *callback;
};

static void CALLBACK task_waiter_ready( void *param, BOOLEAN timer_fired )
{
    struct x_waiter *waiter = param;
    if (!timer_fired && waiter->callback) waiter->callback( waiter->context, FALSE );
}

static HRESULT WINAPI x_threading_XTaskQueueRegisterWaiter( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueuePort port, HANDLE waitHandle, void *callbackContext, XTaskQueueCallback *callback, XTaskQueueRegistrationToken *token )
{
    struct x_waiter *waiter;

    TRACE( "iface %p, queue %p, port %d, waitHandle %p, callbackContext %p, callback %p, token %p.\n",
           iface, queue, port, waitHandle, callbackContext, callback, token );

    if (!callback || !waitHandle) return E_INVALIDARG;
    if (!(waiter = calloc( 1, sizeof(*waiter) ))) return E_OUTOFMEMORY;
    waiter->context = callbackContext;
    waiter->callback = callback;

    if (!RegisterWaitForSingleObject( &waiter->wait, waitHandle, task_waiter_ready, waiter, INFINITE, WT_EXECUTEDEFAULT ))
    {
        HRESULT hr = HRESULT_FROM_WIN32( GetLastError() );
        free( waiter );
        return hr;
    }

    if (token) token->token = (UINT64)(UINT_PTR)waiter;
    return S_OK;
}

static void WINAPI x_threading_XTaskQueueUnregisterWaiter( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueueRegistrationToken token )
{
    struct x_waiter *waiter = (struct x_waiter *)(UINT_PTR)token.token;

    TRACE( "iface %p, queue %p, token %I64u.\n", iface, queue, token.token );

    if (waiter)
    {
        UnregisterWaitEx( waiter->wait, INVALID_HANDLE_VALUE ); /* wait for pending callbacks */
        free( waiter );
    }
}

/* Completions already parked for whoever pumps the queue, delivered now
 * rather than stranded. */
static void CALLBACK drain_terminated_queue( void *context, BOOLEAN canceled )
{
    struct x_task_queue *obj = context;

    if (strict_manual_dispatch()) cancel_manual_callbacks( obj );
    while (deliver_one_completion( (XTaskQueueHandle)obj, XTaskQueuePort_Work, strict_manual_dispatch() ) ||
           deliver_one_completion( (XTaskQueueHandle)obj, XTaskQueuePort_Completion, strict_manual_dispatch() ));
}

static HRESULT WINAPI x_threading_XTaskQueueTerminate( IXThreadingImpl *iface, XTaskQueueHandle queue, BOOLEAN wait, void *callbackContext, XTaskQueueTerminatedCallback *callback )
{
    struct x_task_queue *obj = (struct x_task_queue *)queue;
    struct x_terminate_request *request = NULL;
    struct x_queue_work *item, *next, *drain = NULL;
    struct list drained, timed;
    BOOL block;

    TRACE_(xqueue)( "iface %p, queue %p, wait %d, callbackContext %p, callback %p.\n", iface, queue, wait, callbackContext, callback );

    if (!obj) obj = default_task_queue();
    if (!obj)
    {
        if (callback) callback( callbackContext );
        return S_OK;
    }

    /* The terminated callback is the title's signal that nothing else will ever
     * be called back, and it acts on that: it destroys the objects those
     * callbacks were going to touch. So the queue stops accepting work, what it
     * holds is cancelled -- run with `canceled` set -- and the callback fires
     * only once everything it has accepted has returned (queue_finish).
     *
     * wait asks for all of that to have happened before this returns. Without
     * it nothing runs on the caller's thread: a caller holding a lock those
     * callbacks need would deadlock. A callback of this queue that asks to
     * wait would be waiting for itself, so it is answered as if it had not
     * asked; its terminated callback follows its own return. */
    block = wait && !running_on( obj );
    if (wait && !block) WARN( "queue %p is being terminated from its own callback; not waiting.\n", obj );
    if (callback && !block)
    {
        if (!(request = calloc( 1, sizeof(*request) ))) return E_OUTOFMEMORY;
        request->context = callbackContext;
        request->callback = callback;
    }
    if (!block && !(drain = calloc( 1, sizeof(*drain) )))
    {
        free( request );
        return E_OUTOFMEMORY;
    }

    list_init( &drained );
    list_init( &timed );

    EnterCriticalSection( &queue_work_cs );
    obj->terminated = TRUE;
    list_move_tail( &drained, &obj->pending );
    LIST_FOR_EACH_ENTRY_SAFE( item, next, &obj->delayed, struct x_queue_work, entry )
    {
        item->armed = FALSE;
        list_remove( &item->entry );
        list_add_tail( &timed, &item->entry );
    }
    /* Held for the rest of this call when it blocks, by the drain item when it
     * does not, so the queue cannot go quiet while cancellations are still
     * being handed out. */
    InterlockedIncrement( &obj->inflight );
    if (request) list_add_tail( &obj->terminate_requests, &request->entry );
    LeaveCriticalSection( &queue_work_cs );

    /* Delayed callbacks are cancelled now rather than when their timers would
     * have fired. A timer that has fired already finds its item claimed and
     * leaves it alone; waiting for it means it has stopped looking. */
    LIST_FOR_EACH_ENTRY_SAFE( item, next, &timed, struct x_queue_work, entry )
    {
        SetThreadpoolTimer( item->timer, NULL, 0, 0 );
        WaitForThreadpoolTimerCallbacks( item->timer, TRUE );
        list_remove( &item->entry );
        list_add_tail( &drained, &item->entry );
    }

    if (block)
    {
        /* Outside the lock: a cancelled callback may submit, close handles, or
         * do anything else that takes it. */
        while (!list_empty( &drained ))
        {
            item = LIST_ENTRY( list_head( &drained ), struct x_queue_work, entry );
            list_remove( &item->entry );
            run_queue_item( item, TRUE );
        }
        drain_terminated_queue( obj, TRUE );
        queue_finish( obj );
        wait_until_quiet( obj );
        if (callback) callback( callbackContext );
        return S_OK;
    }

    /* The queue's own runner delivers the cancellations, and the drain item
     * last, which also releases the count held above. */
    drain->queue = obj;
    drain->callback = drain_terminated_queue;
    drain->context = obj;
    drain->serial = TRUE;
    list_add_tail( &drained, &drain->entry );
    EnterCriticalSection( &queue_work_cs );
    list_move_tail( &obj->pending, &drained );
    LeaveCriticalSection( &queue_work_cs );
    kick_queue_runner( obj );
    return S_OK;
}

static HRESULT WINAPI x_threading_XTaskQueueRegisterMonitor( IXThreadingImpl *iface, XTaskQueueHandle queue, void *callbackContext, XTaskQueueMonitorCallback *callback, XTaskQueueRegistrationToken *token )
{
    TRACE( "iface %p, queue %p, callbackContext %p, callback %p, token %p.\n", iface, queue, callbackContext, callback, token );

    /* A monitor observes submissions for instrumentation; nothing here submits
     * out of band, so accept the registration and never invoke the callback. */
    if (token) token->token = 1;
    return S_OK;
}

static void WINAPI x_threading_XTaskQueueUnregisterMonitor( IXThreadingImpl *iface, XTaskQueueHandle queue, XTaskQueueRegistrationToken token )
{
    FIXME( "iface %p, queue %p, token %p stub!\n", iface, queue, &token );
}

static BOOLEAN WINAPI x_threading_XTaskQueueGetCurrentProcessTaskQueue( IXThreadingImpl *iface, XTaskQueueHandle *queue )
{
    struct x_task_queue *obj = default_task_queue();

    TRACE( "iface %p, queue %p.\n", iface, queue );

    if (!obj) return FALSE;
    /* The caller owns what it gets back and closes it when done, so this hands
     * out a reference. Without one, a title that installs its own queue would
     * free the default out from under whoever still held it. */
    InterlockedIncrement( &obj->ref );
    if (queue) *queue = (XTaskQueueHandle)obj;
    return TRUE;
}

static void WINAPI x_threading_XTaskQueueSetCurrentProcessTaskQueue( IXThreadingImpl *iface, XTaskQueueHandle queue )
{
    struct x_task_queue *obj = (struct x_task_queue *)queue, *old;

    TRACE( "iface %p, queue %p.\n", iface, queue );

    if (obj) InterlockedIncrement( &obj->ref );
    old = InterlockedExchangePointer( (void **)&process_queue, obj );
    if (old) InterlockedDecrement( &old->ref );   /* not freed -- see XTaskQueueCloseHandle */
}

static HRESULT WINAPI x_threading_XThreadSetTimeSensitive( IXThreadingImpl *iface, BOOLEAN isTimeSensitiveThread )
{
    TRACE( "iface %p, isTimeSensitiveThread %d.\n", iface, isTimeSensitiveThread );
    if (!TlsSetValue( tlsIndex, (void *)(UINT_PTR)isTimeSensitiveThread )) return HRESULT_FROM_WIN32( GetLastError() );
    return S_OK;
}

static void WINAPI x_threading_XThreadAssertNotTimeSensitive( IXThreadingImpl *iface )
{
    TRACE( "iface %p.\n", iface );
    if (TlsGetValue( tlsIndex )) DebugBreak();
}

static BOOLEAN WINAPI x_threading_XThreadIsTimeSensitive( IXThreadingImpl *iface )
{
    TRACE( "iface %p.\n", iface );
    return TlsGetValue( tlsIndex ) ? 1 : 0;
}

static const struct IXThreadingImplVtbl x_threading_vtbl =
{
    x_threading_QueryInterface,
    x_threading_AddRef,
    x_threading_Release,
    /* IXThreadingImpl methods */
    x_threading_XAsyncGetStatus,
    x_threading_XAsyncGetResultSize,
    x_threading_XAsyncCancel,
    x_threading_XAsyncRun,
    x_threading_XAsyncBegin,
    __PADDING__,
    x_threading_XAsyncSchedule,
    x_threading_XAsyncComplete,
    x_threading_XAsyncGetResult,
    x_threading_XTaskQueueCreate,
    x_threading_XTaskQueueCreateComposite,
    x_threading_XTaskQueueGetPort,
    x_threading_XTaskQueueDuplicateHandle,
    x_threading_XTaskQueueDispatch,
    x_threading_XTaskQueueCloseHandle,
    x_threading_XTaskQueueSubmitCallback,
    x_threading_XTaskQueueSubmitDelayedCallback,
    x_threading_XTaskQueueRegisterWaiter,
    x_threading_XTaskQueueUnregisterWaiter,
    x_threading_XTaskQueueTerminate,
    x_threading_XTaskQueueRegisterMonitor,
    x_threading_XTaskQueueUnregisterMonitor,
    x_threading_XTaskQueueGetCurrentProcessTaskQueue,
    x_threading_XTaskQueueSetCurrentProcessTaskQueue,
    x_threading_XThreadSetTimeSensitive,
    __PADDING__,
    x_threading_XThreadAssertNotTimeSensitive,
    x_threading_XThreadIsTimeSensitive
};

static struct x_threading x_threading =
{
    {&x_threading_vtbl},
    0,
};

IXThreadingImpl *x_threading_impl = &x_threading.IXThreadingImpl_iface;
