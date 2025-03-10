#include "server.h"
#include "bio.h"
#include "functions.h"
#include "cluster.h"
#include "module.h"

#include <stdatomic.h>

static _Atomic size_t lazyfree_objects = 0;
static _Atomic size_t lazyfreed_objects = 0;

/* Release objects from the lazyfree thread. It's just decrRefCount()
 * updating the count of objects to release. */
void lazyfreeFreeObject(void *args[]) {
    robj *o = (robj *)args[0];
    decrRefCount(o);
    atomic_fetch_sub_explicit(&lazyfree_objects, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&lazyfreed_objects, 1, memory_order_relaxed);
}

/* Release a database from the lazyfree thread. The 'db' pointer is the
 * database which was substituted with a fresh one in the main thread
 * when the database was logically deleted. */
void lazyfreeFreeDatabase(void *args[]) {
    kvstore *da1 = args[0];
    kvstore *da2 = args[1];

    size_t numkeys = kvstoreSize(da1);
    kvstoreRelease(da1);
    kvstoreRelease(da2);
    atomic_fetch_sub_explicit(&lazyfree_objects, numkeys, memory_order_relaxed);
    atomic_fetch_add_explicit(&lazyfreed_objects, numkeys, memory_order_relaxed);
}

/* Release the key tracking table. */
void lazyFreeTrackingTable(void *args[]) {
    rax *rt = args[0];
    size_t len = rt->numele;
    freeTrackingRadixTree(rt);
    atomic_fetch_sub_explicit(&lazyfree_objects, len, memory_order_relaxed);
    atomic_fetch_add_explicit(&lazyfreed_objects, len, memory_order_relaxed);
}

/* Release the error stats rax tree. */
void lazyFreeErrors(void *args[]) {
    rax *errors = args[0];
    size_t len = errors->numele;
    raxFreeWithCallback(errors, zfree);
    atomic_fetch_sub_explicit(&lazyfree_objects, len, memory_order_relaxed);
    atomic_fetch_add_explicit(&lazyfreed_objects, len, memory_order_relaxed);
}

/* Release the eval scripts data structures. */
void lazyFreeEvalScripts(void *args[]) {
    dict *scripts = args[0];
    list *scripts_lru_list = args[1];
    list *engine_callbacks = args[2];
    long long len = dictSize(scripts);
    freeEvalScripts(scripts, scripts_lru_list, engine_callbacks);
    atomic_fetch_sub_explicit(&lazyfree_objects, len, memory_order_relaxed);
    atomic_fetch_add_explicit(&lazyfreed_objects, len, memory_order_relaxed);
}

/* Release the functions ctx. */
void lazyFreeFunctionsCtx(void *args[]) {
    functionsLibCtx *functions_lib_ctx = args[0];
    size_t len = functionsLibCtxFunctionsLen(functions_lib_ctx);
    functionsLibCtxFree(functions_lib_ctx);
    atomic_fetch_sub_explicit(&lazyfree_objects, len, memory_order_relaxed);
    atomic_fetch_add_explicit(&lazyfreed_objects, len, memory_order_relaxed);
}

/* Release replication backlog referencing memory. */
void lazyFreeReplicationBacklogRefMem(void *args[]) {
    list *blocks = args[0];
    rax *index = args[1];
    long long len = listLength(blocks);
    len += raxSize(index);
    listRelease(blocks);
    raxFree(index);
    atomic_fetch_sub_explicit(&lazyfree_objects, len, memory_order_relaxed);
    atomic_fetch_add_explicit(&lazyfreed_objects, len, memory_order_relaxed);
}

/* Return the number of currently pending objects to free. */
size_t lazyfreeGetPendingObjectsCount(void) {
    size_t aux = atomic_load_explicit(&lazyfree_objects, memory_order_relaxed);
    return aux;
}

/* Return the number of objects that have been freed. */
size_t lazyfreeGetFreedObjectsCount(void) {
    size_t aux = atomic_load_explicit(&lazyfreed_objects, memory_order_relaxed);
    return aux;
}

void lazyfreeResetStats(void) {
    atomic_store_explicit(&lazyfreed_objects, 0, memory_order_relaxed);
}

/* Return the amount of work needed in order to free an object.
 * The return value is not always the actual number of allocations the
 * object is composed of, but a number proportional to it.
 *
 * For strings the function always returns 1.
 *
 * For aggregated objects represented by hash tables or other data structures
 * the function just returns the number of elements the object is composed of.
 *
 * Objects composed of single allocations are always reported as having a
 * single item even if they are actually logical composed of multiple
 * elements.
 *
 * For lists the function returns the number of elements in the quicklist
 * representing the list. */
size_t lazyfreeGetFreeEffort(robj *key, robj *obj, int dbid) {
    if (obj->type == OBJ_LIST && obj->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *ql = obj->ptr;
        return ql->len;
    } else if (obj->type == OBJ_SET && obj->encoding == OBJ_ENCODING_HASHTABLE) {
        hashtable *ht = obj->ptr;
        return hashtableSize(ht);
    } else if (obj->type == OBJ_ZSET && obj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = obj->ptr;
        return zs->zsl->length;
    } else if (obj->type == OBJ_HASH && obj->encoding == OBJ_ENCODING_HASHTABLE) {
        hashtable *ht = obj->ptr;
        return hashtableSize(ht);
    } else if (obj->type == OBJ_STREAM) {
        size_t effort = 0;
        stream *s = obj->ptr;

        /* Make a best effort estimate to maintain constant runtime. Every macro
         * node in the Stream is one allocation. */
        effort += s->rax->numnodes;

        /* Every consumer group is an allocation and so are the entries in its
         * PEL. We use size of the first group's PEL as an estimate for all
         * others. */
        if (s->cgroups && raxSize(s->cgroups)) {
            raxIterator ri;
            streamCG *cg;
            raxStart(&ri, s->cgroups);
            raxSeek(&ri, "^", NULL, 0);
            /* There must be at least one group so the following should always
             * work. */
            serverAssert(raxNext(&ri));
            cg = ri.data;
            effort += raxSize(s->cgroups) * (1 + raxSize(cg->pel));
            raxStop(&ri);
        }
        return effort;
    } else if (obj->type == OBJ_MODULE) {
        size_t effort = moduleGetFreeEffort(key, obj, dbid);
        /* If the module's free_effort returns 0, we will use asynchronous free
         * memory by default. */
        return effort == 0 ? ULONG_MAX : effort;
    } else {
        return 1; /* Everything else is a single allocation. */
    }
}

/* If there are enough allocations to free the value object asynchronously, it
 * may be put into a lazy free list instead of being freed synchronously. The
 * lazy free list will be reclaimed in a different bio.c thread. If the value is
 * composed of a few allocations, to free in a lazy way is actually just
 * slower... So under a certain limit we just free the object synchronously. */
#define LAZYFREE_THRESHOLD 64

/* Free an object, if the object is huge enough, free it in async way. */
void freeObjAsync(robj *key, robj *obj, int dbid) {
    size_t free_effort = lazyfreeGetFreeEffort(key, obj, dbid);
    /* Note that if the object is shared, to reclaim it now it is not
     * possible. This rarely happens, however sometimes the implementation
     * of parts of the server core may call incrRefCount() to protect
     * objects, and then call dbDelete(). */
    if (free_effort > LAZYFREE_THRESHOLD && obj->refcount == 1) {
        atomic_fetch_add_explicit(&lazyfree_objects, 1, memory_order_relaxed);
        bioCreateLazyFreeJob(lazyfreeFreeObject, 1, obj);
    } else {
        decrRefCount(obj);
    }
}

/* Empty a DB asynchronously. What the function does actually is to
 * create a new empty set of hash tables and scheduling the old ones for
 * lazy freeing. */
void emptyDbAsync(serverDb *db) {
    int slot_count_bits = 0;
    int flags = KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND;
    if (server.cluster_enabled) {
        slot_count_bits = CLUSTER_SLOT_MASK_BITS;
        flags |= KVSTORE_FREE_EMPTY_HASHTABLES;
    }
    kvstore *oldkeys = db->keys, *oldexpires = db->expires;
    db->keys = kvstoreCreate(&kvstoreKeysHashtableType, slot_count_bits, flags);
    db->expires = kvstoreCreate(&kvstoreExpiresHashtableType, slot_count_bits, flags);
    atomic_fetch_add_explicit(&lazyfree_objects, kvstoreSize(oldkeys), memory_order_relaxed);
    bioCreateLazyFreeJob(lazyfreeFreeDatabase, 2, oldkeys, oldexpires);
}

/* Free the key tracking table.
 * If the table is huge enough, free it in async way. */
void freeTrackingRadixTreeAsync(rax *tracking) {
    /* Because this rax has only keys and no values so we use numnodes. */
    if (tracking->numnodes > LAZYFREE_THRESHOLD) {
        atomic_fetch_add_explicit(&lazyfree_objects, tracking->numele, memory_order_relaxed);
        bioCreateLazyFreeJob(lazyFreeTrackingTable, 1, tracking);
    } else {
        freeTrackingRadixTree(tracking);
    }
}

/* Free the error stats rax tree.
 * If the rax tree is huge enough, free it in async way. */
void freeErrorsRadixTreeAsync(rax *errors) {
    /* Because this rax has only keys and no values so we use numnodes. */
    if (errors->numnodes > LAZYFREE_THRESHOLD) {
        atomic_fetch_add_explicit(&lazyfree_objects, errors->numele, memory_order_relaxed);
        bioCreateLazyFreeJob(lazyFreeErrors, 1, errors);
    } else {
        raxFreeWithCallback(errors, zfree);
    }
}

/* Free scripts dict, and lru list, if the dict is huge enough, free them in
 * async way.
 * Close lua interpreter, if there are a lot of lua scripts, close it in async way. */
void freeEvalScriptsAsync(dict *scripts, list *scripts_lru_list, list *engine_callbacks) {
    if (dictSize(scripts) > LAZYFREE_THRESHOLD) {
        atomic_fetch_add_explicit(&lazyfree_objects, dictSize(scripts), memory_order_relaxed);
        bioCreateLazyFreeJob(lazyFreeEvalScripts, 3, scripts, scripts_lru_list, engine_callbacks);
    } else {
        freeEvalScripts(scripts, scripts_lru_list, engine_callbacks);
    }
}

/* Free functions ctx, if the functions ctx contains enough functions, free it in async way. */
void freeFunctionsAsync(functionsLibCtx *functions_lib_ctx) {
    if (functionsLibCtxFunctionsLen(functions_lib_ctx) > LAZYFREE_THRESHOLD) {
        atomic_fetch_add_explicit(&lazyfree_objects, functionsLibCtxFunctionsLen(functions_lib_ctx),
                                  memory_order_relaxed);
        bioCreateLazyFreeJob(lazyFreeFunctionsCtx, 1, functions_lib_ctx);
    } else {
        functionsLibCtxFree(functions_lib_ctx);
    }
}

/* Free replication backlog referencing buffer blocks and rax index. */
void freeReplicationBacklogRefMemAsync(list *blocks, rax *index) {
    if (listLength(blocks) > LAZYFREE_THRESHOLD || raxSize(index) > LAZYFREE_THRESHOLD) {
        atomic_fetch_add_explicit(&lazyfree_objects, listLength(blocks) + raxSize(index), memory_order_relaxed);
        bioCreateLazyFreeJob(lazyFreeReplicationBacklogRefMem, 2, blocks, index);
    } else {
        listRelease(blocks);
        raxFree(index);
    }
}
