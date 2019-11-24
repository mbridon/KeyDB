/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include "cluster.h"
#include "atomicvar.h"
#include "aelocker.h"

#include <signal.h>
#include <ctype.h>

/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/

int keyIsExpired(redisDb *db, robj *key);
int expireIfNeeded(redisDb *db, robj *key, robj *o);

/* Update LFU when an object is accessed.
 * Firstly, decrement the counter if the decrement time is reached.
 * Then logarithmically increment the counter, and update the access time. */
void updateLFU(robj *val) {
    unsigned long counter = LFUDecrAndReturn(val);
    counter = LFULogIncr(counter);
    val->lru = (LFUGetTimeInMinutes()<<8) | counter;
}

void updateExpire(redisDb *db, sds key, robj *valOld, robj *valNew)
{
    serverAssert(valOld->FExpires());
    serverAssert(!valNew->FExpires());
    
    serverAssert(db->FKeyExpires((const char*)key));
    
    valNew->SetFExpires(true);
    valOld->SetFExpires(false);
    return;
}


/* Low level key lookup API, not actually called directly from commands
 * implementations that should instead rely on lookupKeyRead(),
 * lookupKeyWrite() and lookupKeyReadWithFlags(). */
static robj *lookupKey(redisDb *db, robj *key, int flags) {
    auto itr = db->find(key);
    if (itr) {
        robj *val = itr.val();
        /* Update the access time for the ageing algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. */
        if (!g_pserver->FRdbSaveInProgress() &&
            g_pserver->aof_child_pid == -1 &&
            !(flags & LOOKUP_NOTOUCH))
        {
            if (g_pserver->maxmemory_policy & MAXMEMORY_FLAG_LFU) {
                updateLFU(val);
            } else {
                val->lru = LRU_CLOCK();
            }
        }

        if (flags & LOOKUP_UPDATEMVCC) {
            val->mvcc_tstamp = getMvccTstamp();
            db->trackkey(key);
        }
        return val;
    } else {
        return NULL;
    }
}

/* Lookup a key for read operations, or return NULL if the key is not found
 * in the specified DB.
 *
 * As a side effect of calling this function:
 * 1. A key gets expired if it reached it's TTL.
 * 2. The key last access time is updated.
 * 3. The global keys hits/misses stats are updated (reported in INFO).
 * 4. If keyspace notifications are enabled, a "keymiss" notification is fired.
 *
 * This API should not be used when we write to the key after obtaining
 * the object linked to the key, but only for read only operations.
 *
 * Flags change the behavior of this command:
 *
 *  LOOKUP_NONE (or zero): no special flags are passed.
 *  LOOKUP_NOTOUCH: don't alter the last access time of the key.
 *
 * Note: this function also returns NULL if the key is logically expired
 * but still existing, in case this is a replica, since this API is called only
 * for read operations. Even if the key expiry is master-driven, we can
 * correctly report a key is expired on slaves even if the master is lagging
 * expiring our key via DELs in the replication link. */
robj_roptr lookupKeyReadWithFlags(redisDb *db, robj *key, int flags) {
    robj *val;
    serverAssert(GlobalLocksAcquired());

    if (expireIfNeeded(db,key) == 1) {
        /* Key expired. If we are in the context of a master, expireIfNeeded()
         * returns 0 only when the key does not exist at all, so it's safe
         * to return NULL ASAP. */
        if (listLength(g_pserver->masters) == 0) {
            g_pserver->stat_keyspace_misses++;
            notifyKeyspaceEvent(NOTIFY_KEY_MISS, "keymiss", key, db->id);
            return NULL;
        }

        /* However if we are in the context of a replica, expireIfNeeded() will
         * not really try to expire the key, it only returns information
         * about the "logical" status of the key: key expiring is up to the
         * master in order to have a consistent view of master's data set.
         *
         * However, if the command caller is not the master, and as additional
         * safety measure, the command invoked is a read-only command, we can
         * safely return NULL here, and provide a more consistent behavior
         * to clients accessign expired values in a read-only fashion, that
         * will say the key as non existing.
         *
         * Notably this covers GETs when slaves are used to scale reads. */
        if (serverTL->current_client &&
            !FActiveMaster(serverTL->current_client) &&
            serverTL->current_client->cmd &&
            serverTL->current_client->cmd->flags & CMD_READONLY)
        {
            g_pserver->stat_keyspace_misses++;
            notifyKeyspaceEvent(NOTIFY_KEY_MISS, "keymiss", key, db->id);
            return NULL;
        }
    }
    val = lookupKey(db,key,flags);
    if (val == NULL) {
        g_pserver->stat_keyspace_misses++;
        notifyKeyspaceEvent(NOTIFY_KEY_MISS, "keymiss", key, db->id);
    }
    else
        g_pserver->stat_keyspace_hits++;
    return val;
}

/* Like lookupKeyReadWithFlags(), but does not use any flag, which is the
 * common case. */
robj_roptr lookupKeyRead(redisDb *db, robj *key) {
    return lookupKeyReadWithFlags(db,key,LOOKUP_NONE);
}

/* Lookup a key for write operations, and as a side effect, if needed, expires
 * the key if its TTL is reached.
 *
 * Returns the linked value object if the key exists or NULL if the key
 * does not exist in the specified DB. */
robj *lookupKeyWrite(redisDb *db, robj *key) {
    robj *o = lookupKey(db,key,LOOKUP_UPDATEMVCC);
    if (expireIfNeeded(db,key))
        o = NULL;
    return o;
}

robj_roptr lookupKeyReadOrReply(client *c, robj *key, robj *reply) {
    robj_roptr o = lookupKeyRead(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

bool dbAddCore(redisDb *db, robj *key, robj *val) {
    serverAssert(!val->FExpires());
    sds copy = sdsdup(szFromObj(key));
    bool fInserted = db->insert(copy, val);
    if (g_pserver->fActiveReplica)
        val->mvcc_tstamp = key->mvcc_tstamp = getMvccTstamp();

    if (fInserted)
    {
        if (val->type == OBJ_LIST ||
            val->type == OBJ_ZSET)
            signalKeyAsReady(db, key);
        if (g_pserver->cluster_enabled) slotToKeyAdd(key);
    }
    else
    {
        sdsfree(copy);
    }

    return fInserted;
}

/* Add the key to the DB. It's up to the caller to increment the reference
 * counter of the value if needed.
 *
 * The program is aborted if the key already exists. */
void dbAdd(redisDb *db, robj *key, robj *val)
{
    bool fInserted = dbAddCore(db, key, val);
    serverAssertWithInfo(NULL,key,fInserted);
}

void redisDb::dbOverwriteCore(redisDb::iter itr, robj *key, robj *val, bool fUpdateMvcc, bool fRemoveExpire)
{
    robj *old = itr.val();

    if (old->FExpires()) {
        if (fRemoveExpire) {
            ::removeExpire(this, key);
        }
        else {
            if (val->getrefcount(std::memory_order_relaxed) == OBJ_SHARED_REFCOUNT)
                val = dupStringObject(val);
            ::updateExpire(this, itr.key(), old, val);
        }
    }

    if (g_pserver->maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        val->lru = old->lru;
    }
    if (fUpdateMvcc) {
        if (val->getrefcount(std::memory_order_relaxed) == OBJ_SHARED_REFCOUNT)
            val = dupStringObject(val);
        val->mvcc_tstamp = getMvccTstamp();
    }

    if (g_pserver->lazyfree_lazy_server_del)
        freeObjAsync(itr.val());
    else
        decrRefCount(itr.val());

    updateValue(itr, val);
}

/* Overwrite an existing key with a new value. Incrementing the reference
 * count of the new value is up to the caller.
 * This function does not modify the expire time of the existing key.
 *
 * The program is aborted if the key was not already present. */
void dbOverwrite(redisDb *db, robj *key, robj *val) {
    auto itr = db->find(key);

    serverAssertWithInfo(NULL,key,itr != nullptr);
    db->dbOverwriteCore(itr, key, val, !!g_pserver->fActiveReplica, false);
}

/* Insert a key, handling duplicate keys according to fReplace */
int dbMerge(redisDb *db, robj *key, robj *val, int fReplace)
{
    if (fReplace)
    {
        auto itr = db->find(key);
        if (itr == nullptr)
            return (dbAddCore(db, key, val) == true);

        robj *old = itr.val();
        if (old->mvcc_tstamp <= val->mvcc_tstamp)
        {
            db->dbOverwriteCore(itr, key, val, false, true);
            return true;
        }
        
        return false;
    }
    else
    {
        return (dbAddCore(db, key, val) == true);
    }
}

/* High level Set operation. This function can be used in order to set
 * a key, whatever it was existing or not, to a new object.
 *
 * 1) The ref count of the value object is incremented.
 * 2) clients WATCHing for the destination key notified.
 * 3) The expire time of the key is reset (the key is made persistent).
 *
 * All the new keys in the database should be created via this interface. */
void setKey(redisDb *db, robj *key, robj *val) {
    auto itr = db->find(key);
    if (itr == NULL) {
        dbAdd(db,key,val);
    } else {
        db->dbOverwriteCore(itr,key,val,!!g_pserver->fActiveReplica,true);
    }
    incrRefCount(val);
    signalModifiedKey(db,key);
}

int dbExists(redisDb *db, robj *key) {
    return (db->find(key) != nullptr);
}

/* Return a random key, in form of a Redis object.
 * If there are no keys, NULL is returned.
 *
 * The function makes sure to return keys not already expired. */
robj *dbRandomKey(redisDb *db) {
    int maxtries = 100;
    bool allvolatile = db->expireSize() == db->size();

    while(1) {
        sds key;
        robj *keyobj;

        auto itr = db->random();
        if (itr == nullptr) return NULL;

        key = itr.key();
        keyobj = createStringObject(key,sdslen(key));

        if (itr.val()->FExpires())
        {
            if (allvolatile && listLength(g_pserver->masters) && --maxtries == 0) {
                /* If the DB is composed only of keys with an expire set,
                    * it could happen that all the keys are already logically
                    * expired in the replica, so the function cannot stop because
                    * expireIfNeeded() is false, nor it can stop because
                    * dictGetRandomKey() returns NULL (there are keys to return).
                    * To prevent the infinite loop we do some tries, but if there
                    * are the conditions for an infinite loop, eventually we
                    * return a key name that may be already expired. */
                return keyobj;
            }
        }
            
        if (itr.val()->FExpires())
        {
             if (expireIfNeeded(db,keyobj)) {
                decrRefCount(keyobj);
                continue; /* search for another key. This expired. */
             }
        }
        
        return keyobj;
    }
}

bool redisDbPersistentData::syncDelete(robj *key)
{
    /* Deleting an entry from the expires dict will not free the sds of
     * the key, because it is shared with the main dictionary. */

    auto itr = find(szFromObj(key));
    trackkey(szFromObj(key));
    if (itr != nullptr && itr.val()->FExpires())
        removeExpire(key, itr);
    if (dictDelete(m_pdict,ptrFromObj(key)) == DICT_OK) {
        if (m_pdbSnapshot != nullptr)
            dictAdd(m_pdictTombstone, sdsdup(szFromObj(key)), nullptr);
        if (g_pserver->cluster_enabled) slotToKeyDel(key);
        return 1;
    } else {
        return 0;
    }
}

/* Delete a key, value, and associated expiration entry if any, from the DB */
int dbSyncDelete(redisDb *db, robj *key) {
    return db->syncDelete(key);
}

/* This is a wrapper whose behavior depends on the Redis lazy free
 * configuration. Deletes the key synchronously or asynchronously. */
int dbDelete(redisDb *db, robj *key) {
    return g_pserver->lazyfree_lazy_server_del ? dbAsyncDelete(db,key) :
                                             dbSyncDelete(db,key);
}

/* Prepare the string object stored at 'key' to be modified destructively
 * to implement commands like SETBIT or APPEND.
 *
 * An object is usually ready to be modified unless one of the two conditions
 * are true:
 *
 * 1) The object 'o' is shared (refcount > 1), we don't want to affect
 *    other users.
 * 2) The object encoding is not "RAW".
 *
 * If the object is found in one of the above conditions (or both) by the
 * function, an unshared / not-encoded copy of the string object is stored
 * at 'key' in the specified 'db'. Otherwise the object 'o' itself is
 * returned.
 *
 * USAGE:
 *
 * The object 'o' is what the caller already obtained by looking up 'key'
 * in 'db', the usage pattern looks like this:
 *
 * o = lookupKeyWrite(db,key);
 * if (checkType(c,o,OBJ_STRING)) return;
 * o = dbUnshareStringValue(db,key,o);
 *
 * At this point the caller is ready to modify the object, for example
 * using an sdscat() call to append some data, or anything else.
 */
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o) {
    serverAssert(o->type == OBJ_STRING);
    if (o->getrefcount(std::memory_order_relaxed) != 1 || o->encoding != OBJ_ENCODING_RAW) {
        robj *decoded = getDecodedObject(o);
        o = createRawStringObject(szFromObj(decoded), sdslen(szFromObj(decoded)));
        decrRefCount(decoded);
        dbOverwrite(db,key,o);
    }
    return o;
}

/* Remove all keys from all the databases in a Redis DB
 * If callback is given the function is called from time to time to
 * signal that work is in progress.
 *
 * The dbnum can be -1 if all the DBs should be flushed, or the specified
 * DB number if we want to flush only a single Redis database number.
 *
 * Flags are be EMPTYDB_NO_FLAGS if no special flags are specified or
 * EMPTYDB_ASYNC if we want the memory to be freed in a different thread
 * and the function to return ASAP.
 *
 * On success the fuction returns the number of keys removed from the
 * database(s). Otherwise -1 is returned in the specific case the
 * DB number is out of range, and errno is set to EINVAL. */
long long emptyDb(int dbnum, int flags, void(callback)(void*)) {
    int async = (flags & EMPTYDB_ASYNC);
    long long removed = 0;

    if (dbnum < -1 || dbnum >= cserver.dbnum) {
        errno = EINVAL;
        return -1;
    }

    int startdb, enddb;
    if (dbnum == -1) {
        startdb = 0;
        enddb = cserver.dbnum-1;
    } else {
        startdb = enddb = dbnum;
    }

    for (int j = startdb; j <= enddb; j++) {
        removed += g_pserver->db[j].clear(!!async, callback); 
    }
    if (g_pserver->cluster_enabled) {
        if (async) {
            slotToKeyFlushAsync();
        } else {
            slotToKeyFlush();
        }
    }
    if (dbnum == -1) flushSlaveKeysWithExpireList();
    return removed;
}

int selectDb(client *c, int id) {
    if (id < 0 || id >= cserver.dbnum)
        return C_ERR;
    c->db = &g_pserver->db[id];
    return C_OK;
}

/*-----------------------------------------------------------------------------
 * Hooks for key space changes.
 *
 * Every time a key in the database is modified the function
 * signalModifiedKey() is called.
 *
 * Every time a DB is flushed the function signalFlushDb() is called.
 *----------------------------------------------------------------------------*/

void signalModifiedKey(redisDb *db, robj *key) {
    touchWatchedKey(db,key);
    if (g_pserver->tracking_clients) trackingInvalidateKey(key);
}

void signalFlushedDb(int dbid) {
    touchWatchedKeysOnFlush(dbid);
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space
 *----------------------------------------------------------------------------*/

/* Return the set of flags to use for the emptyDb() call for FLUSHALL
 * and FLUSHDB commands.
 *
 * Currently the command just attempts to parse the "ASYNC" option. It
 * also checks if the command arity is wrong.
 *
 * On success C_OK is returned and the flags are stored in *flags, otherwise
 * C_ERR is returned and the function sends an error to the client. */
int getFlushCommandFlags(client *c, int *flags) {
    /* Parse the optional ASYNC option. */
    if (c->argc > 1) {
        if (c->argc > 2 || strcasecmp(szFromObj(c->argv[1]),"async")) {
            addReply(c,shared.syntaxerr);
            return C_ERR;
        }
        *flags = EMPTYDB_ASYNC;
    } else {
        *flags = EMPTYDB_NO_FLAGS;
    }
    return C_OK;
}

/* FLUSHDB [ASYNC]
 *
 * Flushes the currently SELECTed Redis DB. */
void flushdbCommand(client *c) {
    int flags;

    if (getFlushCommandFlags(c,&flags) == C_ERR) return;
    signalFlushedDb(c->db->id);
    g_pserver->dirty += emptyDb(c->db->id,flags,NULL);
    addReply(c,shared.ok);
}

/* FLUSHALL [ASYNC]
 *
 * Flushes the whole server data set. */
void flushallCommand(client *c) {
    int flags;

    if (getFlushCommandFlags(c,&flags) == C_ERR) return;
    signalFlushedDb(-1);
    g_pserver->dirty += emptyDb(-1,flags,NULL);
    addReply(c,shared.ok);
    if (g_pserver->FRdbSaveInProgress()) killRDBChild();
    if (g_pserver->saveparamslen > 0) {
        /* Normally rdbSave() will reset dirty, but we don't want this here
         * as otherwise FLUSHALL will not be replicated nor put into the AOF. */
        int saved_dirty = g_pserver->dirty;
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        rdbSave(nullptr, rsiptr);
        g_pserver->dirty = saved_dirty;
    }
    g_pserver->dirty++;
}

/* This command implements DEL and LAZYDEL. */
void delGenericCommand(client *c, int lazy) {
    int numdel = 0, j;

    for (j = 1; j < c->argc; j++) {
        expireIfNeeded(c->db,c->argv[j]);
        int deleted  = lazy ? dbAsyncDelete(c->db,c->argv[j]) :
                              dbSyncDelete(c->db,c->argv[j]);
        if (deleted) {
            signalModifiedKey(c->db,c->argv[j]);
            notifyKeyspaceEvent(NOTIFY_GENERIC,
                "del",c->argv[j],c->db->id);
            g_pserver->dirty++;
            numdel++;
        }
    }
    addReplyLongLong(c,numdel);
}

void delCommand(client *c) {
    delGenericCommand(c,0);
}

void unlinkCommand(client *c) {
    delGenericCommand(c,1);
}

/* EXISTS key1 key2 ... key_N.
 * Return value is the number of keys existing. */
void existsCommand(client *c) {
    long long count = 0;
    int j;

    for (j = 1; j < c->argc; j++) {
        if (lookupKeyRead(c->db,c->argv[j])) count++;
    }
    addReplyLongLong(c,count);
}

void selectCommand(client *c) {
    long id;

    if (getLongFromObjectOrReply(c, c->argv[1], &id,
        "invalid DB index") != C_OK)
        return;

    if (g_pserver->cluster_enabled && id != 0) {
        addReplyError(c,"SELECT is not allowed in cluster mode");
        return;
    }
    if (selectDb(c,id) == C_ERR) {
        addReplyError(c,"DB index is out of range");
    } else {
        addReply(c,shared.ok);
    }
}

void randomkeyCommand(client *c) {
    robj *key;

    if ((key = dbRandomKey(c->db)) == NULL) {
        addReplyNull(c);
        return;
    }

    addReplyBulk(c,key);
    decrRefCount(key);
}


bool redisDbPersistentData::iterate(std::function<bool(const char*, robj*)> fn)
{
    dictIterator *di = dictGetSafeIterator(m_pdict);
    dictEntry *de = nullptr;
    bool fResult = true;
    while((de = dictNext(di)) != nullptr)
    {
        ensure((const char*)dictGetKey(de), &de);
        if (!fn((const char*)dictGetKey(de), (robj*)dictGetVal(de)))
        {
            fResult = false;
            break;
        }
    }
    dictReleaseIterator(di);

    if (fResult && m_pdbSnapshot != nullptr)
    {
        fResult = m_pdbSnapshot->iterate([&](const char *key){
            // Before passing off to the user we need to make sure it's not already in the
            //  the current set, and not deleted
            dictEntry *deCurrent = dictFind(m_pdict, key);
            if (deCurrent != nullptr)
                return true;
            dictEntry *deTombstone = dictFind(m_pdictTombstone, key);
            if (deTombstone != nullptr)
                return true;

            // Alright it's a key in the use keyspace, lets ensure it and then pass it off
            ensure(key);
            deCurrent = dictFind(m_pdict, key);
            return fn(key, (robj*)dictGetVal(deCurrent));
        });
    }
    
    return fResult;
}

bool redisDbPersistentData::iterate_threadsafe(std::function<bool(const char*, robj_roptr o)> fn) const
{
    dictIterator *di = dictGetIterator(m_pdict);
    dictEntry *de = nullptr;
    bool fResult = true;

    while((de = dictNext(di)) != nullptr)
    {
        if (!fn((const char*)dictGetKey(de), (robj*)dictGetVal(de)))
        {
            fResult = false;
            break;
        }
    }
    dictReleaseIterator(di);

    if (fResult && m_pdbSnapshot != nullptr)
    {
        fResult = m_pdbSnapshot->iterate_threadsafe([&](const char *key, robj_roptr o){
            // Before passing off to the user we need to make sure it's not already in the
            //  the current set, and not deleted
            dictEntry *deCurrent = dictFind(m_pdict, key);
            if (deCurrent != nullptr)
                return true;
            dictEntry *deTombstone = dictFind(m_pdictTombstone, key);
            if (deTombstone != nullptr)
                return true;

            // Alright it's a key in the use keyspace, lets ensure it and then pass it off
            return fn(key, o);
        });
    }

    return fResult;
}

bool redisDbPersistentData::iterate(std::function<bool(const char*)> fn) const
{
    dictIterator *di = dictGetIterator(m_pdict);
    dictEntry *de = nullptr;
    bool fResult = true;
    while((de = dictNext(di)) != nullptr)
    {
        if (!fn((const char*)dictGetKey(de)))
        {
            fResult = false;
            break;
        }
    }
    dictReleaseIterator(di);
    
    if (fResult && m_pdbSnapshot != nullptr)
    {
        fResult = m_pdbSnapshot->iterate([&](const char *key){
            // Before passing off to the user we need to make sure it's not already in the
            //  the current set, and not deleted
            dictEntry *deCurrent = dictFind(m_pdict, key);
            if (deCurrent != nullptr)
                return true;
            dictEntry *deTombstone = dictFind(m_pdictTombstone, key);
            if (deTombstone != nullptr)
                return true;

            // Alright it's a key in the use keyspace
            return fn(key);
        });
    }
    
    return fResult;
}

client *createFakeClient(void);
void freeFakeClient(client *);
void keysCommandCore(client *cIn, const redisDbPersistentData *db, sds pattern)
{
    int plen = sdslen(pattern), allkeys;
    unsigned long numkeys = 0;

    client *c = createFakeClient();
    c->flags |= CLIENT_FORCE_REPLY;

    void *replylen = addReplyDeferredLen(c);

    allkeys = (pattern[0] == '*' && pattern[1] == '\0');
    db->iterate([&](const char *key)->bool {
        robj *keyobj;

        if (allkeys || stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            keyobj = createStringObject(key,sdslen(key));
            if (!keyIsExpired(c->db,keyobj)) {
                addReplyBulk(c,keyobj);
                numkeys++;
            }
            decrRefCount(keyobj);
        }
        return !(cIn->flags.load(std::memory_order_relaxed) & CLIENT_CLOSE_ASAP);
    });
    
    setDeferredArrayLen(c,replylen,numkeys);

    aeAcquireLock();
    addReplyProtoAsync(cIn, c->buf, c->bufpos);
    listIter li;
    listNode *ln;
    listRewind(c->reply, &li);
    while ((ln = listNext(&li)) != nullptr)
    {
        clientReplyBlock *block = (clientReplyBlock*)listNodeValue(ln);
        addReplyProtoAsync(cIn, block->buf(), block->used);
    }
    aeReleaseLock();
    freeFakeClient(c);
}

int prepareClientToWrite(client *c, bool fAsync);
void keysCommand(client *c) {
    sds pattern = szFromObj(c->argv[1]);

    const redisDbPersistentData *snapshot = nullptr;
    if (!(c->flags & (CLIENT_MULTI | CLIENT_BLOCKED)))
        snapshot = c->db->createSnapshot(c->mvccCheckpoint);
    if (snapshot != nullptr)
    {
        sds patternCopy = sdsdup(pattern);
        aeEventLoop *el = serverTL->el;
        blockClient(c, BLOCKED_ASYNC);
        redisDb *db = c->db;
        g_pserver->asyncworkqueue->AddWorkFunction([el, c, db, patternCopy, snapshot]{
            keysCommandCore(c, snapshot, patternCopy);
            sdsfree(patternCopy);
            aePostFunction(el, [c, db, snapshot]{
                aeReleaseLock();    // we need to lock with coordination of the client

                std::unique_lock<decltype(c->lock)> lock(c->lock);
                AeLocker locker;
                locker.arm(c);

                unblockClient(c);
                db->endSnapshot(snapshot);
                aeAcquireLock();
            });
        });
    }
    else
    {
        keysCommandCore(c, c->db, pattern);
    }
}

/* This callback is used by scanGenericCommand in order to collect elements
 * returned by the dictionary iterator into a list. */
void scanCallback(void *privdata, const dictEntry *de) {
    void **pd = (void**) privdata;
    list *keys = (list*)pd[0];
    robj *o = (robj*)pd[1];
    robj *key, *val = NULL;

    if (o == NULL) {
        sds sdskey = (sds)dictGetKey(de);
        key = createStringObject(sdskey, sdslen(sdskey));
    } else if (o->type == OBJ_SET) {
        sds keysds = (sds)dictGetKey(de);
        key = createStringObject(keysds,sdslen(keysds));
    } else if (o->type == OBJ_HASH) {
        sds sdskey = (sds)dictGetKey(de);
        sds sdsval = (sds)dictGetVal(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObject(sdsval,sdslen(sdsval));
    } else if (o->type == OBJ_ZSET) {
        sds sdskey = (sds)dictGetKey(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObjectFromLongDouble(*(double*)dictGetVal(de),0);
    } else {
        serverPanic("Type not handled in SCAN callback.");
    }

    listAddNodeTail(keys, key);
    if (val) listAddNodeTail(keys, val);
}

/* Try to parse a SCAN cursor stored at object 'o':
 * if the cursor is valid, store it as unsigned integer into *cursor and
 * returns C_OK. Otherwise return C_ERR and send an error to the
 * client. */
int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor) {
    char *eptr;

    /* Use strtoul() because we need an *unsigned* long, so
     * getLongLongFromObject() does not cover the whole cursor space. */
    errno = 0;
    *cursor = strtoul(szFromObj(o), &eptr, 10);
    if (isspace(((char*)ptrFromObj(o))[0]) || eptr[0] != '\0' || errno == ERANGE)
    {
        addReplyError(c, "invalid cursor");
        return C_ERR;
    }
    return C_OK;
}

/* This command implements SCAN, HSCAN and SSCAN commands.
 * If object 'o' is passed, then it must be a Hash, Set or Zset object, otherwise
 * if 'o' is NULL the command will operate on the dictionary associated with
 * the current database.
 *
 * When 'o' is not NULL the function assumes that the first argument in
 * the client arguments vector is a key so it skips it before iterating
 * in order to parse options.
 *
 * In the case of a Hash object the function returns both the field and value
 * of every element on the Hash. */
void scanGenericCommand(client *c, robj_roptr o, unsigned long cursor) {
    int i, j;
    list *keys = listCreate();
    listNode *node, *nextnode;
    long count = 10;
    sds pat = NULL;
    sds type = NULL;
    int patlen = 0, use_pattern = 0;
    dict *ht;

    /* Object must be NULL (to iterate keys names), or the type of the object
     * must be Set, Sorted Set, or Hash. */
    serverAssert(o == nullptr || o->type == OBJ_SET || o->type == OBJ_HASH ||
                o->type == OBJ_ZSET);

    /* Set i to the first option argument. The previous one is the cursor. */
    i = (o == nullptr) ? 2 : 3; /* Skip the key argument if needed. */

    /* Step 1: Parse options. */
    while (i < c->argc) {
        j = c->argc - i;
        if (!strcasecmp(szFromObj(c->argv[i]), "count") && j >= 2) {
            if (getLongFromObjectOrReply(c, c->argv[i+1], &count, NULL)
                != C_OK)
            {
                goto cleanup;
            }

            if (count < 1) {
                addReply(c,shared.syntaxerr);
                goto cleanup;
            }

            i += 2;
        } else if (!strcasecmp(szFromObj(c->argv[i]), "match") && j >= 2) {
            pat = szFromObj(c->argv[i+1]);
            patlen = sdslen(pat);

            /* The pattern always matches if it is exactly "*", so it is
             * equivalent to disabling it. */
            use_pattern = !(pat[0] == '*' && patlen == 1);

            i += 2;
        } else if (!strcasecmp(szFromObj(c->argv[i]), "type") && o == nullptr && j >= 2) {
            /* SCAN for a particular type only applies to the db dict */
            type = szFromObj(c->argv[i+1]);
            i+= 2;
        } else {
            addReply(c,shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Step 2: Iterate the collection.
     *
     * Note that if the object is encoded with a ziplist, intset, or any other
     * representation that is not a hash table, we are sure that it is also
     * composed of a small number of elements. So to avoid taking state we
     * just return everything inside the object in a single call, setting the
     * cursor to zero to signal the end of the iteration. */

    /* Handle the case of a hash table. */
    ht = NULL;
    if (o == nullptr) {
        ht = c->db->dictUnsafeKeyOnly();
    } else if (o->type == OBJ_SET && o->encoding == OBJ_ENCODING_HT) {
        ht = (dict*)ptrFromObj(o);
    } else if (o->type == OBJ_HASH && o->encoding == OBJ_ENCODING_HT) {
        ht = (dict*)ptrFromObj(o);
        count *= 2; /* We return key / value for this type. */
    } else if (o->type == OBJ_ZSET && o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = (zset*)ptrFromObj(o);
        ht = zs->pdict;
        count *= 2; /* We return key / value for this type. */
    }

    if (ht) {
        void *privdata[2];
        /* We set the max number of iterations to ten times the specified
         * COUNT, so if the hash table is in a pathological state (very
         * sparsely populated) we avoid to block too much time at the cost
         * of returning no or very few elements. */
        long maxiterations = count*10;

        /* We pass two pointers to the callback: the list to which it will
         * add new elements, and the object containing the dictionary so that
         * it is possible to fetch more data in a type-dependent way. */
        privdata[0] = keys;
        privdata[1] = o.unsafe_robjcast();
        do {
            cursor = dictScan(ht, cursor, scanCallback, NULL, privdata);
        } while (cursor &&
              maxiterations-- &&
              listLength(keys) < (unsigned long)count);
    } else if (o->type == OBJ_SET) {
        int pos = 0;
        int64_t ll;

        while(intsetGet((intset*)ptrFromObj(o),pos++,&ll))
            listAddNodeTail(keys,createStringObjectFromLongLong(ll));
        cursor = 0;
    } else if (o->type == OBJ_HASH || o->type == OBJ_ZSET) {
        unsigned char *p = ziplistIndex((unsigned char*)ptrFromObj(o),0);
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;

        while(p) {
            ziplistGet(p,&vstr,&vlen,&vll);
            listAddNodeTail(keys,
                (vstr != NULL) ? createStringObject((char*)vstr,vlen) :
                                 createStringObjectFromLongLong(vll));
            p = ziplistNext((unsigned char*)ptrFromObj(o),p);
        }
        cursor = 0;
    } else {
        serverPanic("Not handled encoding in SCAN.");
    }

    /* Step 3: Filter elements. */
    node = listFirst(keys);
    while (node) {
        robj *kobj = (robj*)listNodeValue(node);
        nextnode = listNextNode(node);
        int filter = 0;

        /* Filter element if it does not match the pattern. */
        if (!filter && use_pattern) {
            if (sdsEncodedObject(kobj)) {
                if (!stringmatchlen(pat, patlen, szFromObj(kobj), sdslen(szFromObj(kobj)), 0))
                    filter = 1;
            } else {
                char buf[LONG_STR_SIZE];
                int len;

                serverAssert(kobj->encoding == OBJ_ENCODING_INT);
                len = ll2string(buf,sizeof(buf),(long)ptrFromObj(kobj));
                if (!stringmatchlen(pat, patlen, buf, len, 0)) filter = 1;
            }
        }

        /* Filter an element if it isn't the type we want. */
        if (!filter && o == nullptr && type){
            robj_roptr typecheck = lookupKeyReadWithFlags(c->db, kobj, LOOKUP_NOTOUCH);
            const char* typeT = getObjectTypeName(typecheck);
            if (strcasecmp((char*) type, typeT)) filter = 1;
        }

        /* Filter element if it is an expired key. */
        if (!filter && o == nullptr && expireIfNeeded(c->db, kobj)) filter = 1;

        /* Remove the element and its associted value if needed. */
        if (filter) {
            decrRefCount(kobj);
            listDelNode(keys, node);
        }

        /* If this is a hash or a sorted set, we have a flat list of
         * key-value elements, so if this element was filtered, remove the
         * value, or skip it if it was not filtered: we only match keys. */
        if (o && (o->type == OBJ_ZSET || o->type == OBJ_HASH)) {
            node = nextnode;
            nextnode = listNextNode(node);
            if (filter) {
                kobj = (robj*)listNodeValue(node);
                decrRefCount(kobj);
                listDelNode(keys, node);
            }
        }
        node = nextnode;
    }

    /* Step 4: Reply to the client. */
    addReplyArrayLen(c, 2);
    addReplyBulkLongLong(c,cursor);

    addReplyArrayLen(c, listLength(keys));
    while ((node = listFirst(keys)) != NULL) {
        robj *kobj = (robj*)listNodeValue(node);
        addReplyBulk(c, kobj);
        decrRefCount(kobj);
        listDelNode(keys, node);
    }

cleanup:
    listSetFreeMethod(keys,decrRefCountVoid);
    listRelease(keys);
}

/* The SCAN command completely relies on scanGenericCommand. */
void scanCommand(client *c) {
    unsigned long cursor;
    if (parseScanCursorOrReply(c,c->argv[1],&cursor) == C_ERR) return;
    scanGenericCommand(c,nullptr,cursor);
}

void dbsizeCommand(client *c) {
    addReplyLongLong(c,c->db->size());
}

void lastsaveCommand(client *c) {
    addReplyLongLong(c,g_pserver->lastsave);
}

const char* getObjectTypeName(robj_roptr o) {
    const char* type;
    if (o == nullptr) {
        type = "none";
    } else {
        switch(o->type) {
        case OBJ_STRING: type = "string"; break;
        case OBJ_LIST: type = "list"; break;
        case OBJ_SET: type = "set"; break;
        case OBJ_ZSET: type = "zset"; break;
        case OBJ_HASH: type = "hash"; break;
        case OBJ_STREAM: type = "stream"; break;
        case OBJ_MODULE: {
            moduleValue *mv = (moduleValue*)ptrFromObj(o);
            type = mv->type->name;
        }; break;
        default: type = "unknown"; break;
        }
    }
    return type;
}

void typeCommand(client *c) {
    robj_roptr o = lookupKeyReadWithFlags(c->db,c->argv[1],LOOKUP_NOTOUCH);
    addReplyStatus(c, getObjectTypeName(o));
}

void shutdownCommand(client *c) {
    int flags = 0;

    if (c->argc > 2) {
        addReply(c,shared.syntaxerr);
        return;
    } else if (c->argc == 2) {
        if (!strcasecmp(szFromObj(c->argv[1]),"nosave")) {
            flags |= SHUTDOWN_NOSAVE;
        } else if (!strcasecmp(szFromObj(c->argv[1]),"save")) {
            flags |= SHUTDOWN_SAVE;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }
    /* When SHUTDOWN is called while the server is loading a dataset in
     * memory we need to make sure no attempt is performed to save
     * the dataset on shutdown (otherwise it could overwrite the current DB
     * with half-read data).
     *
     * Also when in Sentinel mode clear the SAVE flag and force NOSAVE. */
    if (g_pserver->loading || g_pserver->sentinel_mode)
        flags = (flags & ~SHUTDOWN_SAVE) | SHUTDOWN_NOSAVE;
    if (prepareForShutdown(flags) == C_OK) exit(0);
    addReplyError(c,"Errors trying to SHUTDOWN. Check logs.");
}

void renameGenericCommand(client *c, int nx) {
    robj *o;
    int samekey = 0;

    /* When source and dest key is the same, no operation is performed,
     * if the key exists, however we still return an error on unexisting key. */
    if (sdscmp(szFromObj(c->argv[1]),szFromObj(c->argv[2])) == 0) samekey = 1;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;

    if (samekey) {
        addReply(c,nx ? shared.czero : shared.ok);
        return;
    }

    incrRefCount(o);

    std::unique_ptr<expireEntry> spexpire;

    {   // scope pexpireOld since it will be invalid soon
    expireEntry *pexpireOld = c->db->getExpire(c->argv[1]);
    if (pexpireOld != nullptr)
        spexpire = std::make_unique<expireEntry>(std::move(*pexpireOld));
    }

    if (lookupKeyWrite(c->db,c->argv[2]) != NULL) {
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        /* Overwrite: delete the old key before creating the new one
         * with the same name. */
        dbDelete(c->db,c->argv[2]);
    }
    dbDelete(c->db,c->argv[1]);
    dbAdd(c->db,c->argv[2],o);
    if (spexpire != nullptr) 
        setExpire(c,c->db,c->argv[2],std::move(*spexpire));
    signalModifiedKey(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[2]);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_from",
        c->argv[1],c->db->id);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_to",
        c->argv[2],c->db->id);
    g_pserver->dirty++;
    addReply(c,nx ? shared.cone : shared.ok);
}

void renameCommand(client *c) {
    renameGenericCommand(c,0);
}

void renamenxCommand(client *c) {
    renameGenericCommand(c,1);
}

void moveCommand(client *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid;
    long long dbid;

    if (g_pserver->cluster_enabled) {
        addReplyError(c,"MOVE is not allowed in cluster mode");
        return;
    }

    /* Obtain source and target DB pointers */
    src = c->db;
    srcid = c->db->id;

    if (getLongLongFromObject(c->argv[2],&dbid) == C_ERR ||
        dbid < INT_MIN || dbid > INT_MAX ||
        selectDb(c,dbid) == C_ERR)
    {
        addReply(c,shared.outofrangeerr);
        return;
    }
    dst = c->db;
    selectDb(c,srcid); /* Back to the source DB */

    /* If the user is moving using as target the same
     * DB as the source DB it is probably an error. */
    if (src == dst) {
        addReply(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (!o) {
        addReply(c,shared.czero);
        return;
    }

    std::unique_ptr<expireEntry> spexpire;
    {   // scope pexpireOld
    expireEntry *pexpireOld = c->db->getExpire(c->argv[1]);
    if (pexpireOld != nullptr)
        spexpire = std::make_unique<expireEntry>(std::move(*pexpireOld));
    }
    if (o->FExpires())
        removeExpire(c->db,c->argv[1]);
    serverAssert(!o->FExpires());
    incrRefCount(o);
    dbDelete(src,c->argv[1]);
    g_pserver->dirty++;

    /* Return zero if the key already exists in the target DB */
    if (lookupKeyWrite(dst,c->argv[1]) != NULL) {
        addReply(c,shared.czero);
        return;
    }
    dbAdd(dst,c->argv[1],o);
    if (spexpire != nullptr) setExpire(c,dst,c->argv[1],std::move(*spexpire));

    addReply(c,shared.cone);
}

/* Helper function for dbSwapDatabases(): scans the list of keys that have
 * one or more blocked clients for B[LR]POP or other blocking commands
 * and signal the keys as ready if they are of the right type. See the comment
 * where the function is used for more info. */
void scanDatabaseForReadyLists(redisDb *db) {
    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(db->blocking_keys);
    while((de = dictNext(di)) != NULL) {
        robj *key = (robj*)dictGetKey(de);
        robj *value = lookupKey(db,key,LOOKUP_NOTOUCH);
        if (value && (value->type == OBJ_LIST ||
                      value->type == OBJ_STREAM ||
                      value->type == OBJ_ZSET))
            signalKeyAsReady(db, key);
    }
    dictReleaseIterator(di);
}

/* Swap two databases at runtime so that all clients will magically see
 * the new database even if already connected. Note that the client
 * structure c->db points to a given DB, so we need to be smarter and
 * swap the underlying referenced structures, otherwise we would need
 * to fix all the references to the Redis DB structure.
 *
 * Returns C_ERR if at least one of the DB ids are out of range, otherwise
 * C_OK is returned. */
int dbSwapDatabases(int id1, int id2) {
    if (id1 < 0 || id1 >= cserver.dbnum ||
        id2 < 0 || id2 >= cserver.dbnum) return C_ERR;
    if (id1 == id2) return C_OK;
    redisDb aux; 
    memcpy(&aux, &g_pserver->db[id1], sizeof(redisDb));
    redisDb *db1 = &g_pserver->db[id1], *db2 = &g_pserver->db[id2];

    /* Swap hash tables. Note that we don't swap blocking_keys,
     * ready_keys and watched_keys, since we want clients to
     * remain in the same DB they were. */
    redisDbPersistentData::swap(db1, db2);
    db1->avg_ttl = db2->avg_ttl;
    db1->last_expire_set = db2->last_expire_set;
    db1->expireitr = db2->expireitr;

    db2->avg_ttl = aux.avg_ttl;
    db2->last_expire_set = aux.last_expire_set;
    db2->expireitr = aux.expireitr;

    /* Now we need to handle clients blocked on lists: as an effect
     * of swapping the two DBs, a client that was waiting for list
     * X in a given DB, may now actually be unblocked if X happens
     * to exist in the new version of the DB, after the swap.
     *
     * However normally we only do this check for efficiency reasons
     * in dbAdd() when a list is created. So here we need to rescan
     * the list of clients blocked on lists and signal lists as ready
     * if needed. */
    scanDatabaseForReadyLists(db1);
    scanDatabaseForReadyLists(db2);
    return C_OK;
}

/* SWAPDB db1 db2 */
void swapdbCommand(client *c) {
    long id1, id2;

    /* Not allowed in cluster mode: we have just DB 0 there. */
    if (g_pserver->cluster_enabled) {
        addReplyError(c,"SWAPDB is not allowed in cluster mode");
        return;
    }

    /* Get the two DBs indexes. */
    if (getLongFromObjectOrReply(c, c->argv[1], &id1,
        "invalid first DB index") != C_OK)
        return;

    if (getLongFromObjectOrReply(c, c->argv[2], &id2,
        "invalid second DB index") != C_OK)
        return;

    /* Swap... */
    if (dbSwapDatabases(id1,id2) == C_ERR) {
        addReplyError(c,"DB index is out of range");
        return;
    } else {
        g_pserver->dirty++;
        addReply(c,shared.ok);
    }
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/
int removeExpire(redisDb *db, robj *key) {
    auto itr = db->find(key);
    return db->removeExpire(key, itr);
}
int redisDbPersistentData::removeExpire(robj *key, dict_iter itr) {
    /* An expire may only be removed if there is a corresponding entry in the
     * main dict. Otherwise, the key will never be freed. */
    serverAssertWithInfo(NULL,key,itr != nullptr);

    robj *val = itr.val();
    if (!val->FExpires())
        return 0;

    trackkey(key);
    auto itrExpire = m_setexpire->find(itr.key());
    serverAssert(itrExpire != m_setexpire->end());
    serverAssert(itrExpire->key() == itr.key());
    m_setexpire->erase(itrExpire);
    val->SetFExpires(false);
    return 1;
}

int redisDbPersistentData::removeSubkeyExpire(robj *key, robj *subkey) {
    auto de = find(szFromObj(key));
    serverAssertWithInfo(NULL,key,de != nullptr);
    
    robj *val = de.val();
    if (!val->FExpires())
        return 0;
    
    auto itr = m_setexpire->find(de.key());
    serverAssert(itr != m_setexpire->end());
    serverAssert(itr->key() == de.key());
    if (!itr->FFat())
        return 0;

    int found = 0;
    for (auto subitr : *itr)
    {
        if (subitr.subkey() == nullptr)
            continue;
        if (sdscmp((sds)subitr.subkey(), szFromObj(subkey)) == 0)
        {
            itr->erase(subitr);
            found = 1;
            break;
        }
    }

    if (itr->pfatentry()->size() == 0)
        this->removeExpire(key, de);

    return found;
}

/* Set an expire to the specified key. If the expire is set in the context
 * of an user calling a command 'c' is the client, otherwise 'c' is set
 * to NULL. The 'when' parameter is the absolute unix time in milliseconds
 * after which the key will no longer be considered valid. */
void setExpire(client *c, redisDb *db, robj *key, robj *subkey, long long when) {
    serverAssert(GlobalLocksAcquired());

    /* Update TTL stats (exponential moving average) */
    /*  Note: We never have to update this on expiry since we reduce it by the current elapsed time here */
    long long now = g_pserver->mstime;
    db->avg_ttl -= (now - db->last_expire_set); // reduce the TTL by the time that has elapsed
    if (db->expireSize() == 0)
        db->avg_ttl = 0;
    else
        db->avg_ttl -= db->avg_ttl / db->expireSize(); // slide one entry out the window
    if (db->avg_ttl < 0)
        db->avg_ttl = 0;    // TTLs are never negative
    db->avg_ttl += (double)(when-now) / (db->expireSize()+1);    // add the new entry
    db->last_expire_set = now;

    /* Update the expire set */
    db->setExpire(key, subkey, when);

    int writable_slave = listLength(g_pserver->masters) && g_pserver->repl_slave_ro == 0;
    if (c && writable_slave && !(c->flags & CLIENT_MASTER))
        rememberSlaveKeyWithExpire(db,key);
}

void setExpire(client *c, redisDb *db, robj *key, expireEntry &&e)
{
    serverAssert(GlobalLocksAcquired());

    /* Reuse the sds from the main dict in the expire dict */
    auto kde = db->find(key);
    serverAssertWithInfo(NULL,key,kde != NULL);

    if (kde.val()->getrefcount(std::memory_order_relaxed) == OBJ_SHARED_REFCOUNT)
    {
        // shared objects cannot have the expire bit set, create a real object
        db->updateValue(kde, dupStringObject(kde.val()));
    }

    if (kde.val()->FExpires())
        removeExpire(db, key);

    e.setKeyUnsafe(kde.key());
    db->setExpire(std::move(e));
    kde.val()->SetFExpires(true);


    int writable_slave = listLength(g_pserver->masters) && g_pserver->repl_slave_ro == 0;
    if (c && writable_slave && !(c->flags & CLIENT_MASTER))
        rememberSlaveKeyWithExpire(db,key);
}

/* Return the expire time of the specified key, or null if no expire
 * is associated with this key (i.e. the key is non volatile) */
expireEntry *redisDbPersistentData::getExpire(robj_roptr key) {
    /* No expire? return ASAP */
    if (expireSize() == 0)
        return nullptr;

    auto itr = find(szFromObj(key));
    if (itr == nullptr)
        return nullptr;
    if (!itr.val()->FExpires())
        return nullptr;

    auto itrExpire = findExpire(itr.key());
    return itrExpire.operator->();
}

const expireEntry *redisDbPersistentData::getExpire(robj_roptr key) const
{
    return const_cast<redisDbPersistentData*>(this)->getExpire(key);
}

/* Propagate expires into slaves and the AOF file.
 * When a key expires in the master, a DEL operation for this key is sent
 * to all the slaves and the AOF file if enabled.
 *
 * This way the key expiry is centralized in one place, and since both
 * AOF and the master->replica link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against expiring
 * keys. */
void propagateExpire(redisDb *db, robj *key, int lazy) {
    serverAssert(GlobalLocksAcquired());
    robj *argv[2];

    argv[0] = lazy ? shared.unlink : shared.del;
    argv[1] = key;
    incrRefCount(argv[0]);
    incrRefCount(argv[1]);

    if (g_pserver->aof_state != AOF_OFF)
        feedAppendOnlyFile(cserver.delCommand,db->id,argv,2);
    // Active replicas do their own expiries, do not propogate
    if (!g_pserver->fActiveReplica)
        replicationFeedSlaves(g_pserver->slaves,db->id,argv,2);

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

/* Check if the key is expired. Note, this does not check subexpires */
int keyIsExpired(redisDb *db, robj *key) {
    expireEntry *pexpire = db->getExpire(key);

    if (pexpire == nullptr) return 0; /* No expire for this key */

    /* Don't expire anything while loading. It will be done later. */
    if (g_pserver->loading) return 0;

    long long when = -1;
    for (auto &exp : *pexpire)
    {
        if (exp.subkey() == nullptr)
        {
            when = exp.when();
            break;
        }
    }

    if (when == -1)
        return 0;

    /* If we are in the context of a Lua script, we pretend that time is
     * blocked to when the Lua script started. This way a key can expire
     * only the first time it is accessed and not in the middle of the
     * script execution, making propagation to slaves / AOF consistent.
     * See issue #1525 on Github for more information. */
    mstime_t now = g_pserver->lua_caller ? g_pserver->lua_time_start : mstime();

    return now > when;
}

/* This function is called when we are going to perform some operation
 * in a given key, but such key may be already logically expired even if
 * it still exists in the database. The main way this function is called
 * is via lookupKey*() family of functions.
 *
 * The behavior of the function depends on the replication role of the
 * instance, because replica instances do not expire keys, they wait
 * for DELs from the master for consistency matters. However even
 * slaves will try to have a coherent return value for the function,
 * so that read commands executed in the replica side will be able to
 * behave like if the key is expired even if still present (because the
 * master has yet to propagate the DEL).
 *
 * In masters as a side effect of finding a key which is expired, such
 * key will be evicted from the database. Also this may trigger the
 * propagation of a DEL/UNLINK command in AOF / replication stream.
 *
 * The return value of the function is 0 if the key is still valid,
 * otherwise the function returns 1 if the key is expired. */
int expireIfNeeded(redisDb *db, robj *key) {
    if (!keyIsExpired(db,key)) return 0;

    /* If we are running in the context of a replica, instead of
     * evicting the expired key from the database, we return ASAP:
     * the replica key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time. */
    if (listLength(g_pserver->masters) && !g_pserver->fActiveReplica) return 1;

    /* Delete the key */
    g_pserver->stat_expiredkeys++;
    propagateExpire(db,key,g_pserver->lazyfree_lazy_expire);
    notifyKeyspaceEvent(NOTIFY_EXPIRED,
        "expired",key,db->id);
    return g_pserver->lazyfree_lazy_expire ? dbAsyncDelete(db,key) :
                                         dbSyncDelete(db,key);
}

/* -----------------------------------------------------------------------------
 * API to get key arguments from commands
 * ---------------------------------------------------------------------------*/

/* The base case is to use the keys position as given in the command table
 * (firstkey, lastkey, step). */
int *getKeysUsingCommandTable(struct redisCommand *cmd,robj **argv, int argc, int *numkeys) {
    int j, i = 0, last, *keys;
    UNUSED(argv);

    if (cmd->firstkey == 0) {
        *numkeys = 0;
        return NULL;
    }

    last = cmd->lastkey;
    if (last < 0) last = argc+last;
    keys = (int*)zmalloc(sizeof(int)*((last - cmd->firstkey)+1), MALLOC_SHARED);
    for (j = cmd->firstkey; j <= last; j += cmd->keystep) {
        if (j >= argc) {
            /* Modules commands, and standard commands with a not fixed number
             * of arguments (negative arity parameter) do not have dispatch
             * time arity checks, so we need to handle the case where the user
             * passed an invalid number of arguments here. In this case we
             * return no keys and expect the command implementation to report
             * an arity or syntax error. */
            if (cmd->flags & CMD_MODULE || cmd->arity < 0) {
                zfree(keys);
                *numkeys = 0;
                return NULL;
            } else {
                serverPanic("Redis built-in command declared keys positions not matching the arity requirements.");
            }
        }
        keys[i++] = j;
    }
    *numkeys = i;
    return keys;
}

/* Return all the arguments that are keys in the command passed via argc / argv.
 *
 * The command returns the positions of all the key arguments inside the array,
 * so the actual return value is an heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0].
 *
 * This function uses the command table if a command-specific helper function
 * is not required, otherwise it calls the command-specific function. */
int *getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    if (cmd->flags & CMD_MODULE_GETKEYS) {
        return moduleGetCommandKeysViaAPI(cmd,argv,argc,numkeys);
    } else if (!(cmd->flags & CMD_MODULE) && cmd->getkeys_proc) {
        return cmd->getkeys_proc(cmd,argv,argc,numkeys);
    } else {
        return getKeysUsingCommandTable(cmd,argv,argc,numkeys);
    }
}

/* Free the result of getKeysFromCommand. */
void getKeysFreeResult(int *result) {
    zfree(result);
}

/* Helper function to extract keys from following commands:
 * ZUNIONSTORE <destkey> <num-keys> <key> <key> ... <key> <options>
 * ZINTERSTORE <destkey> <num-keys> <key> <key> ... <key> <options> */
int *zunionInterGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    UNUSED(cmd);

    num = atoi(szFromObj(argv[2]));
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    if (num < 1 || num > (argc-3)) {
        *numkeys = 0;
        return NULL;
    }

    /* Keys in z{union,inter}store come from two places:
     * argv[1] = storage key,
     * argv[3...n] = keys to intersect */
    keys = (int*)zmalloc(sizeof(int)*(num+1), MALLOC_SHARED);

    /* Add all key positions for argv[3...n] to keys[] */
    for (i = 0; i < num; i++) keys[i] = 3+i;

    /* Finally add the argv[1] key position (the storage key target). */
    keys[num] = 1;
    *numkeys = num+1;  /* Total keys = {union,inter} keys + storage key */
    return keys;
}

/* Helper function to extract keys from the following commands:
 * EVAL <script> <num-keys> <key> <key> ... <key> [more stuff]
 * EVALSHA <script> <num-keys> <key> <key> ... <key> [more stuff] */
int *evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    UNUSED(cmd);

    num = atoi(szFromObj(argv[2]));
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    if (num <= 0 || num > (argc-3)) {
        *numkeys = 0;
        return NULL;
    }

    keys = (int*)zmalloc(sizeof(int)*num, MALLOC_SHARED);
    *numkeys = num;

    /* Add all key positions for argv[3...n] to keys[] */
    for (i = 0; i < num; i++) keys[i] = 3+i;

    return keys;
}

/* Helper function to extract keys from the SORT command.
 *
 * SORT <sort-key> ... STORE <store-key> ...
 *
 * The first argument of SORT is always a key, however a list of options
 * follow in SQL-alike style. Here we parse just the minimum in order to
 * correctly identify keys in the "STORE" option. */
int *sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, j, num, *keys, found_store = 0;
    UNUSED(cmd);

    num = 0;
    keys = (int*)zmalloc(sizeof(int)*2, MALLOC_SHARED); /* Alloc 2 places for the worst case. */

    keys[num++] = 1; /* <sort-key> is always present. */

    /* Search for STORE option. By default we consider options to don't
     * have arguments, so if we find an unknown option name we scan the
     * next. However there are options with 1 or 2 arguments, so we
     * provide a list here in order to skip the right number of args. */
    struct {
        const char *name;
        int skip;
    } skiplist[] = {
        {"limit", 2},
        {"get", 1},
        {"by", 1},
        {NULL, 0} /* End of elements. */
    };

    for (i = 2; i < argc; i++) {
        for (j = 0; skiplist[j].name != NULL; j++) {
            if (!strcasecmp(szFromObj(argv[i]),skiplist[j].name)) {
                i += skiplist[j].skip;
                break;
            } else if (!strcasecmp(szFromObj(argv[i]),"store") && i+1 < argc) {
                /* Note: we don't increment "num" here and continue the loop
                 * to be sure to process the *last* "STORE" option if multiple
                 * ones are provided. This is same behavior as SORT. */
                found_store = 1;
                keys[num] = i+1; /* <store-key> */
                break;
            }
        }
    }
    *numkeys = num + found_store;
    return keys;
}

int *migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, first, *keys;
    UNUSED(cmd);

    /* Assume the obvious form. */
    first = 3;
    num = 1;

    /* But check for the extended one with the KEYS option. */
    if (argc > 6) {
        for (i = 6; i < argc; i++) {
            if (!strcasecmp(szFromObj(argv[i]),"keys") &&
                sdslen(szFromObj(argv[3])) == 0)
            {
                first = i+1;
                num = argc-first;
                break;
            }
        }
    }

    keys = (int*)zmalloc(sizeof(int)*num, MALLOC_SHARED);
    for (i = 0; i < num; i++) keys[i] = first+i;
    *numkeys = num;
    return keys;
}

/* Helper function to extract keys from following commands:
 * GEORADIUS key x y radius unit [WITHDIST] [WITHHASH] [WITHCOORD] [ASC|DESC]
 *                             [COUNT count] [STORE key] [STOREDIST key]
 * GEORADIUSBYMEMBER key member radius unit ... options ... */
int *georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    UNUSED(cmd);

    /* Check for the presence of the stored key in the command */
    int stored_key = -1;
    for (i = 5; i < argc; i++) {
        char *arg = szFromObj(argv[i]);
        /* For the case when user specifies both "store" and "storedist" options, the
         * second key specified would override the first key. This behavior is kept
         * the same as in georadiusCommand method.
         */
        if ((!strcasecmp(arg, "store") || !strcasecmp(arg, "storedist")) && ((i+1) < argc)) {
            stored_key = i+1;
            i++;
        }
    }
    num = 1 + (stored_key == -1 ? 0 : 1);

    /* Keys in the command come from two places:
     * argv[1] = key,
     * argv[5...n] = stored key if present
     */
    keys = (int*)zmalloc(sizeof(int) * num, MALLOC_SHARED);

    /* Add all key positions to keys[] */
    keys[0] = 1;
    if(num > 1) {
         keys[1] = stored_key;
    }
    *numkeys = num;
    return keys;
}

/* XREAD [BLOCK <milliseconds>] [COUNT <count>] [GROUP <groupname> <ttl>]
 *       STREAMS key_1 key_2 ... key_N ID_1 ID_2 ... ID_N */
int *xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num = 0, *keys;
    UNUSED(cmd);

    /* We need to parse the options of the command in order to seek the first
     * "STREAMS" string which is actually the option. This is needed because
     * "STREAMS" could also be the name of the consumer group and even the
     * name of the stream key. */
    int streams_pos = -1;
    for (i = 1; i < argc; i++) {
        char *arg = szFromObj(argv[i]);
        if (!strcasecmp(arg, "block")) {
            i++; /* Skip option argument. */
        } else if (!strcasecmp(arg, "count")) {
            i++; /* Skip option argument. */
        } else if (!strcasecmp(arg, "group")) {
            i += 2; /* Skip option argument. */
        } else if (!strcasecmp(arg, "noack")) {
            /* Nothing to do. */
        } else if (!strcasecmp(arg, "streams")) {
            streams_pos = i;
            break;
        } else {
            break; /* Syntax error. */
        }
    }
    if (streams_pos != -1) num = argc - streams_pos - 1;

    /* Syntax error. */
    if (streams_pos == -1 || num == 0 || num % 2 != 0) {
        *numkeys = 0;
        return NULL;
    }
    num /= 2; /* We have half the keys as there are arguments because
                 there are also the IDs, one per key. */

    keys = (int*)zmalloc(sizeof(int) * num, MALLOC_SHARED);
    for (i = streams_pos+1; i < argc-num; i++) keys[i-streams_pos-1] = i;
    *numkeys = num;
    return keys;
}

/* Slot to Key API. This is used by Redis Cluster in order to obtain in
 * a fast way a key that belongs to a specified hash slot. This is useful
 * while rehashing the cluster and in other conditions when we need to
 * understand if we have keys for a given hash slot. */
void slotToKeyUpdateKey(robj *key, int add) {
    size_t keylen = sdslen(szFromObj(key));
    unsigned int hashslot = keyHashSlot(szFromObj(key),keylen);
    unsigned char buf[64];
    unsigned char *indexed = buf;

    g_pserver->cluster->slots_keys_count[hashslot] += add ? 1 : -1;
    if (keylen+2 > 64) indexed = (unsigned char*)zmalloc(keylen+2, MALLOC_SHARED);
    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    memcpy(indexed+2,ptrFromObj(key),keylen);
    if (add) {
        raxInsert(g_pserver->cluster->slots_to_keys,indexed,keylen+2,NULL,NULL);
    } else {
        raxRemove(g_pserver->cluster->slots_to_keys,indexed,keylen+2,NULL);
    }
    if (indexed != buf) zfree(indexed);
}

void slotToKeyAdd(robj *key) {
    slotToKeyUpdateKey(key,1);
}

void slotToKeyDel(robj *key) {
    slotToKeyUpdateKey(key,0);
}

void slotToKeyFlush(void) {
    raxFree(g_pserver->cluster->slots_to_keys);
    g_pserver->cluster->slots_to_keys = raxNew();
    memset(g_pserver->cluster->slots_keys_count,0,
           sizeof(g_pserver->cluster->slots_keys_count));
}

/* Pupulate the specified array of objects with keys in the specified slot.
 * New objects are returned to represent keys, it's up to the caller to
 * decrement the reference count to release the keys names. */
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count) {
    raxIterator iter;
    int j = 0;
    unsigned char indexed[2];

    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    raxStart(&iter,g_pserver->cluster->slots_to_keys);
    raxSeek(&iter,">=",indexed,2);
    while(count-- && raxNext(&iter)) {
        if (iter.key[0] != indexed[0] || iter.key[1] != indexed[1]) break;
        keys[j++] = createStringObject((char*)iter.key+2,iter.key_len-2);
    }
    raxStop(&iter);
    return j;
}

/* Remove all the keys in the specified hash slot.
 * The number of removed items is returned. */
unsigned int delKeysInSlot(unsigned int hashslot) {
    raxIterator iter;
    int j = 0;
    unsigned char indexed[2];

    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    raxStart(&iter,g_pserver->cluster->slots_to_keys);
    while(g_pserver->cluster->slots_keys_count[hashslot]) {
        raxSeek(&iter,">=",indexed,2);
        raxNext(&iter);

        robj *key = createStringObject((char*)iter.key+2,iter.key_len-2);
        dbDelete(&g_pserver->db[0],key);
        decrRefCount(key);
        j++;
    }
    raxStop(&iter);
    return j;
}

unsigned int countKeysInSlot(unsigned int hashslot) {
    return g_pserver->cluster->slots_keys_count[hashslot];
}

void redisDbPersistentData::initialize()
{
    m_pdbSnapshot = nullptr;
    m_pdict = dictCreate(&dbDictType,this);
    m_pdictTombstone = dictCreate(&dbDictType,this);
    m_setexpire = new(MALLOC_LOCAL) expireset();
    m_fAllChanged = false;
    m_fTrackingChanges = 0;
}

void redisDb::initialize(int id)
{
    redisDbPersistentData::initialize();
    this->expireitr = setexpire()->end();
    this->blocking_keys = dictCreate(&keylistDictType,NULL);
    this->ready_keys = dictCreate(&objectKeyPointerValueDictType,NULL);
    this->watched_keys = dictCreate(&keylistDictType,NULL);
    this->id = id;
    this->avg_ttl = 0;
    this->last_expire_set = 0;
    this->defrag_later = listCreate();
}

bool redisDbPersistentData::insert(char *key, robj *o)
{
    int res = dictAdd(m_pdict, key, o);
    if (res == DICT_OK)
        trackkey(key);
    return (res == DICT_OK);
}

void redisDbPersistentData::tryResize()
{
    if (htNeedsResize(m_pdict))
        dictResize(m_pdict);
}

size_t redisDb::clear(bool fAsync, void(callback)(void*))
{
    size_t removed = size();
    if (fAsync) {
        redisDbPersistentData::emptyDbAsync();
    } else {
        redisDbPersistentData::clear(callback);
    }
    expireitr = setexpire()->end();
    return removed;
}

void redisDbPersistentData::clear(void(callback)(void*))
{
    dictEmpty(m_pdict,callback);
    if (m_fTrackingChanges)
        m_fAllChanged = true;
    delete m_setexpire;
    m_setexpire = new (MALLOC_LOCAL) expireset();
    if (m_pstorage != nullptr)
        m_pstorage->clear();
    m_pdbSnapshot = nullptr;
}

/* static */ void redisDbPersistentData::swap(redisDbPersistentData *db1, redisDbPersistentData *db2)
{
    redisDbPersistentData aux = std::move(*db1);
    db1->m_pdict = db2->m_pdict;
    db1->m_fTrackingChanges = db2->m_fTrackingChanges;
    db1->m_fAllChanged = db2->m_fAllChanged;
    db1->m_setexpire = db2->m_setexpire;
    db1->m_pstorage = db2->m_pstorage;
    db1->m_pdbSnapshot = db2->m_pdbSnapshot;
    db1->m_spdbSnapshotHOLDER = std::move(db2->m_spdbSnapshotHOLDER);

    db2->m_pdict = aux.m_pdict;
    db2->m_fTrackingChanges = aux.m_fTrackingChanges;
    db2->m_fAllChanged = aux.m_fAllChanged;
    db2->m_setexpire = aux.m_setexpire;
    db2->m_pstorage = aux.m_pstorage;
    db2->m_pdbSnapshot = aux.m_pdbSnapshot;
    db2->m_spdbSnapshotHOLDER = std::move(aux.m_spdbSnapshotHOLDER);

    db1->m_pdict->privdata = static_cast<redisDbPersistentData*>(db1);
    db2->m_pdict->privdata = static_cast<redisDbPersistentData*>(db2);
}

void redisDbPersistentData::setExpire(robj *key, robj *subkey, long long when)
{
    /* Reuse the sds from the main dict in the expire dict */
    dictEntry *kde = dictFind(m_pdict,ptrFromObj(key));
    serverAssertWithInfo(NULL,key,kde != NULL);
    trackkey(key);

    if (((robj*)dictGetVal(kde))->getrefcount(std::memory_order_relaxed) == OBJ_SHARED_REFCOUNT)
    {
        // shared objects cannot have the expire bit set, create a real object
        dictSetVal(m_pdict, kde, dupStringObject((robj*)dictGetVal(kde)));
    }

    const char *szSubKey = (subkey != nullptr) ? szFromObj(subkey) : nullptr;
    if (((robj*)dictGetVal(kde))->FExpires()) {
        auto itr = m_setexpire->find((sds)dictGetKey(kde));
        serverAssert(itr != m_setexpire->end());
        expireEntry eNew(std::move(*itr));
        eNew.update(szSubKey, when);
        m_setexpire->erase(itr);
        m_setexpire->insert(eNew);
    }
    else
    {
        expireEntry e((sds)dictGetKey(kde), szSubKey, when);
        ((robj*)dictGetVal(kde))->SetFExpires(true);
        m_setexpire->insert(e);
    }
}

void redisDbPersistentData::setExpire(expireEntry &&e)
{
    trackkey(e.key());
    m_setexpire->insert(e);
}

bool redisDb::FKeyExpires(const char *key)
{
    return setexpireUnsafe()->find(key) != setexpire()->end();
}

void redisDbPersistentData::updateValue(dict_iter itr, robj *val)
{
    trackkey(itr.key());
    dictSetVal(m_pdict, itr.de, val);
}

void redisDbPersistentData::ensure(const char *key)
{
    dictEntry *de = dictFind(m_pdict, key);
    ensure(key, &de);
}

void redisDbPersistentData::ensure(const char *sdsKey, dictEntry **pde)
{
    serverAssert(sdsKey != nullptr);
    if (*pde == nullptr && m_pdbSnapshot != nullptr)
    {
        dictEntry *deTombstone = dictFind(m_pdictTombstone, sdsKey);
        if (deTombstone == nullptr)
        {
            auto itr = m_pdbSnapshot->find_threadsafe(sdsKey);
            if (itr == m_pdbSnapshot->end())
                return; // not found
            if (itr.val()->getrefcount(std::memory_order_relaxed) == OBJ_SHARED_REFCOUNT)
            {
                dictAdd(m_pdict, sdsdup(sdsKey), itr.val());
            }
            else
            {
                sds strT = serializeStoredObject(itr.val());
                robj *objNew = deserializeStoredObject(strT, sdslen(strT));
                sdsfree(strT);
                dictAdd(m_pdict, sdsdup(sdsKey), objNew);
                serverAssert(objNew->getrefcount(std::memory_order_relaxed) == 1);
                serverAssert(objNew->mvcc_tstamp == itr.val()->mvcc_tstamp);
            }
            *pde = dictFind(m_pdict, sdsKey);
        }
    }
    else if (*pde != nullptr && dictGetVal(*pde) == nullptr)
    {
        serverAssert(m_pstorage != nullptr);
        sds key = (sds)dictGetKey(*pde);
        m_pstorage->retrieve(key, sdslen(key), true, [&](const char *, size_t, const void *data, size_t cb){
            robj *o = deserializeStoredObject(data, cb);
            serverAssert(o != nullptr);
            dictSetVal(m_pdict, *pde, o);
        });
    }
}

void redisDbPersistentData::storeKey(const char *szKey, size_t cchKey, robj *o)
{
    sds temp = serializeStoredObject(o);
    m_pstorage->insert(szKey, cchKey, temp, sdslen(temp));
    sdsfree(temp);
}

void redisDbPersistentData::storeDatabase()
{
    dictIterator *di = dictGetIterator(m_pdict);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        sds key = (sds)dictGetKey(de);
        robj *o = (robj*)dictGetVal(de);
        storeKey(key, sdslen(key), o);
    }
    dictReleaseIterator(di);
}

void redisDbPersistentData::processChanges()
{
    --m_fTrackingChanges;
    serverAssert(m_fTrackingChanges >= 0);

    if (m_pstorage != nullptr)
    {
        if (m_fTrackingChanges == 0)
        {
            if (m_fAllChanged)
            {
                m_pstorage->clear();
                storeDatabase();
            }
            else
            {
                for (auto &str : m_setchanged)
                {
                    sds sdsKey = sdsnewlen(str.data(), str.size());
                    robj *o = find(sdsKey);
                    if (o != nullptr)
                    {
                        storeKey(str.data(), str.size(), o);
                    }
                    else
                    {
                        m_pstorage->erase(str.data(), str.size());
                    }
                    sdsfree(sdsKey);
                }
            }
        }
    }
    m_setchanged.clear();
}

const redisDbPersistentData *redisDbPersistentData::createSnapshot(uint64_t mvccCheckpoint)
{
    serverAssert(GlobalLocksAcquired());
    serverAssert(m_refCount == 0);  // do not call this on a snapshot
    bool fNested = false;
    if (m_spdbSnapshotHOLDER != nullptr)
    {
        if (mvccCheckpoint <= m_spdbSnapshotHOLDER->mvccCheckpoint)
        {
            m_spdbSnapshotHOLDER->m_refCount++;
            return m_spdbSnapshotHOLDER.get();
        }
        serverLog(LL_WARNING, "Nested snapshot created");
        fNested = true;
    }
    auto spdb = std::unique_ptr<redisDbPersistentData>(new (MALLOC_LOCAL) redisDbPersistentData());
    
    spdb->m_fAllChanged = false;
    spdb->m_fTrackingChanges = 0;
    spdb->m_pdict = m_pdict;
    spdb->m_pdict->iterators++;
    spdb->m_pdictTombstone = m_pdictTombstone;
    spdb->m_spdbSnapshotHOLDER = std::move(m_spdbSnapshotHOLDER);
    spdb->m_pdbSnapshot = m_pdbSnapshot;
    spdb->m_refCount = 1;
    if (m_setexpire != nullptr)
        spdb->m_setexpire = m_setexpire;

    m_pdict = dictCreate(&dbDictType,this);
    m_pdictTombstone = dictCreate(&dbDictType, this);
    m_setexpire = new (MALLOC_LOCAL) expireset();
    
    serverAssert(spdb->m_pdict->iterators == 1);

    m_spdbSnapshotHOLDER = std::move(spdb);
    m_pdbSnapshot = m_spdbSnapshotHOLDER.get();

    // Finally we need to take a ref on all our children snapshots.  This ensures they aren't free'd before we are
    redisDbPersistentData *pdbSnapshotNext = m_pdbSnapshot->m_spdbSnapshotHOLDER.get();
    while (pdbSnapshotNext != nullptr)
    {
        pdbSnapshotNext->m_refCount++;
        pdbSnapshotNext = pdbSnapshotNext->m_spdbSnapshotHOLDER.get();
    }

    return m_pdbSnapshot;
}

void redisDbPersistentData::recursiveFreeSnapshots(redisDbPersistentData *psnapshot)
{
    std::vector<redisDbPersistentData*> m_stackSnapshots;
    // gather a stack of snapshots, we do this so we can free them in reverse
    
    // Note: we don't touch the incoming psnapshot since the parent is free'ing that one
    while ((psnapshot = psnapshot->m_spdbSnapshotHOLDER.get()) != nullptr)
    {
        m_stackSnapshots.push_back(psnapshot);
    }

    for (auto itr = m_stackSnapshots.rbegin(); itr != m_stackSnapshots.rend(); ++itr)
    {
        endSnapshot(*itr);
    }
}

void redisDbPersistentData::endSnapshot(const redisDbPersistentData *psnapshot)
{
    // Note: This function is dependent on GlobalLocksAcquried(), but rdb background saving has a weird case where
    //  a seperate thread holds the lock for it.  Yes that's pretty crazy and should be fixed somehow...

    if (m_spdbSnapshotHOLDER.get() != psnapshot)
    {
        serverAssert(m_spdbSnapshotHOLDER != nullptr);
        m_spdbSnapshotHOLDER->endSnapshot(psnapshot);
        return;
    }

    // Alright we're ready to be free'd, but first dump all the refs on our child snapshots
    if (m_spdbSnapshotHOLDER->m_refCount == 1)
        recursiveFreeSnapshots(m_spdbSnapshotHOLDER.get());

    m_spdbSnapshotHOLDER->m_refCount--;
    if (m_spdbSnapshotHOLDER->m_refCount > 0)
        return;
    serverAssert(m_spdbSnapshotHOLDER->m_refCount == 0);
    serverAssert((m_refCount == 0 && m_pdict->iterators == 0) || (m_refCount != 0 && m_pdict->iterators == 1));

    serverAssert(m_spdbSnapshotHOLDER->m_pdict->iterators == 1);  // All iterators should have been free'd except the fake one from createSnapshot
    if (m_refCount == 0)
        m_spdbSnapshotHOLDER->m_pdict->iterators--;

    if (m_pdbSnapshot == nullptr)
    {
        // the database was cleared so we don't need to recover the snapshot
        dictEmpty(m_pdictTombstone, nullptr);
        m_spdbSnapshotHOLDER = std::move(m_spdbSnapshotHOLDER->m_spdbSnapshotHOLDER);
        return;
    }

    // Stage 1 Loop through all the tracked deletes and remove them from the snapshot DB
    dictIterator *di = dictGetIterator(m_pdictTombstone);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL)
    {
        dictEntry *deSnapshot = dictFind(m_spdbSnapshotHOLDER->m_pdict, dictGetKey(de));
        if (deSnapshot == nullptr)
            continue;   // sometimes we delete things that were never in the snapshot
        
        robj *obj = (robj*)dictGetVal(deSnapshot);
        const char *key = (const char*)dictGetKey(deSnapshot);
        if (obj == nullptr || obj->FExpires())
        {
            auto itrExpire = m_spdbSnapshotHOLDER->m_setexpire->find(key);
            if (itrExpire != m_spdbSnapshotHOLDER->m_setexpire->end())
            {
                m_spdbSnapshotHOLDER->m_setexpire->erase(itrExpire);  // Note: normally we would have to set obj::fexpire false but we're deleting it anyways...
            }
        }
        dictDelete(m_spdbSnapshotHOLDER->m_pdict, key);
    }
    dictReleaseIterator(di);
    dictEmpty(m_pdictTombstone, nullptr);

    // Stage 2 Move all new keys to the snapshot DB
    di = dictGetIterator(m_pdict);
    while ((de = dictNext(di)) != NULL)
    {
        dictEntry *deExisting = dictFind(m_spdbSnapshotHOLDER->m_pdict, (const char*)dictGetKey(de));
        if (deExisting != nullptr)
        {
            decrRefCount((robj*)dictGetVal(deExisting));
            dictSetVal(m_spdbSnapshotHOLDER->m_pdict, deExisting, dictGetVal(de));
        }
        else
        {
            dictAdd(m_spdbSnapshotHOLDER->m_pdict, sdsdup((sds)dictGetKey(de)), dictGetVal(de));
        }
        incrRefCount((robj*)dictGetVal(de));
    }
    dictReleaseIterator(di);
    
    // Stage 3 swap the databases with the snapshot
    std::swap(m_pdict, m_spdbSnapshotHOLDER->m_pdict);

    // Stage 4 merge all expires
    // TODO
    std::swap(m_setexpire, m_spdbSnapshotHOLDER->m_setexpire);
    
    // Finally free the snapshot
    if (m_pdbSnapshot != nullptr && m_spdbSnapshotHOLDER->m_pdbSnapshot != nullptr)
    {
        m_pdbSnapshot = m_spdbSnapshotHOLDER->m_pdbSnapshot;
        m_spdbSnapshotHOLDER->m_pdbSnapshot = nullptr;
    }
    else
    {
        m_pdbSnapshot = nullptr;
    }

    // Fixup the about to free'd snapshots iterator count so the dtor doesn't complain
    if (m_refCount)
        m_spdbSnapshotHOLDER->m_pdict->iterators--;

    m_spdbSnapshotHOLDER = std::move(m_spdbSnapshotHOLDER->m_spdbSnapshotHOLDER);
    serverAssert(m_spdbSnapshotHOLDER != nullptr || m_pdbSnapshot == nullptr);
    serverAssert(m_pdbSnapshot == m_spdbSnapshotHOLDER.get() || m_pdbSnapshot == nullptr);
    serverAssert((m_refCount == 0 && m_pdict->iterators == 0) || (m_refCount != 0 && m_pdict->iterators == 1));
}

redisDbPersistentData::~redisDbPersistentData()
{
    serverAssert(m_spdbSnapshotHOLDER == nullptr);
    serverAssert(m_pdbSnapshot == nullptr);
    serverAssert(m_refCount == 0);
    serverAssert(m_pdict->iterators == 0);
    dictRelease(m_pdict);
    if (m_pdictTombstone)
        dictRelease(m_pdictTombstone);
    delete m_setexpire;
}

dict_iter redisDbPersistentData::random()
{
    if (size() == 0)
        return dict_iter(nullptr);
    if (m_pdbSnapshot != nullptr && m_pdbSnapshot->size() > 0)
    {
        dict_iter iter(nullptr);
        double pctInSnapshot = (double)m_pdbSnapshot->size() / (size() + m_pdbSnapshot->size());
        double randval = (double)rand()/RAND_MAX;
        if (randval <= pctInSnapshot)
        {
            iter = m_pdbSnapshot->random_threadsafe();
            ensure(iter.key());
            dictEntry *de = dictFind(m_pdict, iter.key());
            return dict_iter(de);
        }
    }
    dictEntry *de = dictGetRandomKey(m_pdict);
    if (de != nullptr)
        ensure((const char*)dictGetKey(de), &de);
    return dict_iter(de);
}

dict_iter redisDbPersistentData::random_threadsafe() const
{
    if (size() == 0)
        return dict_iter(nullptr);
    if (m_pdbSnapshot != nullptr && m_pdbSnapshot->size() > 0)
    {
        dict_iter iter(nullptr);
        double pctInSnapshot = (double)m_pdbSnapshot->size() / (size() + m_pdbSnapshot->size());
        double randval = (double)rand()/RAND_MAX;
        if (randval <= pctInSnapshot)
        {
            return m_pdbSnapshot->random_threadsafe();
        }
    }
    serverAssert(dictSize(m_pdict) > 0);
    dictEntry *de = dictGetRandomKey(m_pdict);
    return dict_iter(de);
}