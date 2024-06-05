/*-------------------------------------------------------------------------
 *
 * tde_master_key.c
 *      Deals with the tde master key configuration catalog
 *      routines.
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/catalog/tde_master_key.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/tde_master_key.h"
#include "common/pg_tde_shmem.h"
#include "storage/lwlock.h"
#include "storage/fd.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"
#include "utils/timestamp.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "pg_tde.h"
#include "access/pg_tde_xlog.h"
#include <sys/time.h>

#include "access/pg_tde_tdemap.h"
#include "catalog/tde_global_catalog.h"

typedef struct TdeMasterKeySharedState
{
    LWLock *Locks;
    int hashTrancheId;
    dshash_table_handle hashHandle;
    void *rawDsaArea; /* DSA area pointer */

} TdeMasterKeySharedState;

typedef struct TdeMasterKeylocalState
{
    TdeMasterKeySharedState *sharedMasterKeyState;
    dsa_area *dsa; /* local dsa area for backend attached to the
                    * dsa area created by postmaster at startup.
                    */
    dshash_table *sharedHash;
} TdeMasterKeylocalState;

/* parameter for the master key info shared hash */
static dshash_parameters master_key_dsh_params = {
    sizeof(Oid),
    sizeof(TDEMasterKey),
    dshash_memcmp, /* TODO use int compare instead */
    dshash_memhash};

TdeMasterKeylocalState masterKeyLocalState;

static void master_key_info_attach_shmem(void);
static Size initialize_shared_state(void *start_address);
static void initialize_objects_in_dsa_area(dsa_area *dsa, void *raw_dsa_area);
static Size cache_area_size(void);
static Size required_shared_mem_size(void);
static int  required_locks_count(void);
static void shared_memory_shutdown(int code, Datum arg);
static void master_key_startup_cleanup(int tde_tbl_count, void *arg);
static void clear_master_key_cache(Oid databaseId, Oid tablespaceId) ;
static inline dshash_table *get_master_key_Hash(void);
static TDEMasterKey *get_master_key_from_cache(Oid dbOid);
static void push_master_key_to_cache(TDEMasterKey *masterKey);

static const TDEShmemSetupRoutine master_key_info_shmem_routine = {
    .init_shared_state = initialize_shared_state,
    .init_dsa_area_objects = initialize_objects_in_dsa_area,
    .required_shared_mem_size = required_shared_mem_size,
    .required_locks_count = required_locks_count,
    .shmem_kill = shared_memory_shutdown
    };

void InitializeMasterKeyInfo(void)
{
    ereport(LOG, (errmsg("Initializing TDE master key info")));
    RegisterShmemRequest(&master_key_info_shmem_routine);
    on_ext_install(master_key_startup_cleanup, NULL);
}

LWLock *
tde_lwlock_mk_files(void)
{
    Assert(masterKeyLocalState.sharedMasterKeyState);

    return &masterKeyLocalState.sharedMasterKeyState->Locks[TDE_LWLOCK_MK_FILES];
}

LWLock *
tde_lwlock_mk_cache(void)
{
    Assert(masterKeyLocalState.sharedMasterKeyState);

    return &masterKeyLocalState.sharedMasterKeyState->Locks[TDE_LWLOCK_MK_CACHE];
}

static int
required_locks_count(void)
{
    return TDE_LWLOCK_COUNT;
}

static Size
cache_area_size(void)
{
    return MAXALIGN(8192 * 100); /* TODO: Probably get it from guc */
}

static Size
required_shared_mem_size(void)
{
    Size sz = cache_area_size();
    sz = add_size(sz, sizeof(TdeMasterKeySharedState));
    return MAXALIGN(sz);
}

/*
 * Initialize the shared area for Master key info.
 * This includes locks and cache area for master key info
 */

static Size
initialize_shared_state(void *start_address)
{
    TdeMasterKeySharedState *sharedState = (TdeMasterKeySharedState *)start_address;
    ereport(LOG, (errmsg("initializing shared state for master key")));
    masterKeyLocalState.dsa = NULL;
    masterKeyLocalState.sharedHash = NULL;

    sharedState->Locks = GetNewLWLock();
    masterKeyLocalState.sharedMasterKeyState = sharedState;
    return sizeof(TdeMasterKeySharedState);
}

void initialize_objects_in_dsa_area(dsa_area *dsa, void *raw_dsa_area)
{
    dshash_table *dsh;
    TdeMasterKeySharedState *sharedState = masterKeyLocalState.sharedMasterKeyState;

    ereport(LOG, (errmsg("initializing dsa area objects for master key")));

    Assert(sharedState != NULL);

    sharedState->rawDsaArea = raw_dsa_area;
    sharedState->hashTrancheId = LWLockNewTrancheId();
    master_key_dsh_params.tranche_id = sharedState->hashTrancheId;
    dsh = dshash_create(dsa, &master_key_dsh_params, 0);
    sharedState->hashHandle = dshash_get_hash_table_handle(dsh);
    dshash_detach(dsh);
}

/*
 * Attaches to the DSA to local backend
 */
static void
master_key_info_attach_shmem(void)
{
    MemoryContext oldcontext;

    if (masterKeyLocalState.dsa)
        return;

    /*
     * We want the dsa to remain valid throughout the lifecycle of this
     * process. so switch to TopMemoryContext before attaching
     */
    oldcontext = MemoryContextSwitchTo(TopMemoryContext);

    masterKeyLocalState.dsa = dsa_attach_in_place(masterKeyLocalState.sharedMasterKeyState->rawDsaArea,
                                                  NULL);

    /*
     * pin the attached area to keep the area attached until end of session or
     * explicit detach.
     */
    dsa_pin_mapping(masterKeyLocalState.dsa);

    master_key_dsh_params.tranche_id = masterKeyLocalState.sharedMasterKeyState->hashTrancheId;
    masterKeyLocalState.sharedHash = dshash_attach(masterKeyLocalState.dsa, &master_key_dsh_params,
                                                   masterKeyLocalState.sharedMasterKeyState->hashHandle, 0);
    MemoryContextSwitchTo(oldcontext);
}

static void
shared_memory_shutdown(int code, Datum arg)
{
    masterKeyLocalState.sharedMasterKeyState = NULL;
}

bool
save_master_key_info(TDEMasterKeyInfo *master_key_info)
{
    Assert(master_key_info != NULL);

    return pg_tde_save_master_key(master_key_info);
}

/*
 * Public interface to get the master key for the current database
 * If the master key is not present in the cache, it is loaded from
 * the keyring and stored in the cache.
 * When the master key is not set for the database. The function returns
 * throws an error.
 */
TDEMasterKey *
GetMasterKey(Oid dbOid, Oid spcOid, GenericKeyring *keyring)
{
    TDEMasterKey *masterKey = NULL;
    TDEMasterKeyInfo *masterKeyInfo = NULL;
    const keyInfo *keyInfo = NULL;
    KeyringReturnCodes keyring_ret;
    LWLock *lock_files = tde_lwlock_mk_files();
    LWLock *lock_cache = tde_lwlock_mk_cache();

    LWLockAcquire(lock_cache, LW_SHARED);
        /* Global catalog has its own cache */
    if (spcOid == GLOBALTABLESPACE_OID)
        masterKey = TDEGetGlCatKeyFromCache();
    else 
        masterKey = get_master_key_from_cache(dbOid);
    LWLockRelease(lock_cache);

    if (masterKey)
        return masterKey;

    /*
     * We should hold an exclusive lock here to ensure that a valid master key, if found, is added
     * to the cache without any interference.
     */
    LWLockAcquire(lock_files, LW_SHARED);
    LWLockAcquire(lock_cache, LW_EXCLUSIVE);

    /* Global catalog has its own cache */
    if (spcOid == GLOBALTABLESPACE_OID)
        masterKey = TDEGetGlCatKeyFromCache();
    else 
        masterKey = get_master_key_from_cache(dbOid);

    if (masterKey)
    {
        LWLockRelease(lock_cache);
        LWLockRelease(lock_files);
        return masterKey;
    }

    /* Master key not present in cache. Load from the keyring */
    masterKeyInfo = pg_tde_get_master_key(dbOid, spcOid);
    if (masterKeyInfo == NULL)
    {
        LWLockRelease(lock_cache);
        LWLockRelease(lock_files);

        ereport(PANIC,
                (errmsg("Master key does not exists for the database %u/%u", dbOid, spcOid),
                 errhint("Use set_master_key interface to set the master key")));
        return NULL;
    }

    if (keyring == NULL)
    {
        keyring = GetKeyProviderByID(masterKeyInfo->keyringId);
        if (keyring == NULL)
        {
            LWLockRelease(lock_cache);
            LWLockRelease(lock_files);

            ereport(ERROR,
                    (errmsg("Key provider with ID:\"%d\" does not exists", masterKeyInfo->keyringId)));
            return NULL;
        }
    }

    keyInfo = KeyringGetKey(keyring, masterKeyInfo->keyId.versioned_name, false, &keyring_ret);
    if (keyInfo == NULL)
    {
        LWLockRelease(lock_cache);
        LWLockRelease(lock_files);

        ereport(ERROR,
                (errmsg("failed to retrieve master key \"%s\" from keyring.", masterKeyInfo->keyId.versioned_name)));
        return NULL;
    }

    masterKey = palloc(sizeof(TDEMasterKey));

    memcpy(&masterKey->keyInfo, masterKeyInfo, sizeof(masterKey->keyInfo));
    memcpy(masterKey->keyData, keyInfo->data.data, keyInfo->data.len);
    masterKey->keyLength = keyInfo->data.len;

    Assert(dbOid == masterKey->keyInfo.databaseId);
    if (spcOid == GLOBALTABLESPACE_OID)
        TDEPutGlCatKeyInCache(masterKey);
    else 
        push_master_key_to_cache(masterKey);

    /* Release the exclusive locks here */
    LWLockRelease(lock_cache);
    LWLockRelease(lock_files);

    if (masterKeyInfo)
        pfree(masterKeyInfo);

    return masterKey;
}

/*
 * SetMasterkey:
 * We need to ensure that only one master key is set for a database.
 * To do that we take a little help from cache. Before setting the
 * master key we take an exclusive lock on the cache entry for the
 * database.
 * After acquiring the exclusive lock we check for the entry again
 * to make sure if some other caller has not added a master key for
 * same database while we were waiting for the lock.
 */
TDEMasterKey *
set_master_key_with_keyring(const char *key_name, GenericKeyring *keyring,
                            Oid dbOid, Oid spcOid, bool ensure_new_key)
{
    TDEMasterKey *masterKey = NULL;
    LWLock *lock_files = tde_lwlock_mk_files();
    LWLock *lock_cache = tde_lwlock_mk_cache();
    bool is_dup_key = false;

    /*
     * Try to get master key from cache.
     */
    LWLockAcquire(lock_files, LW_EXCLUSIVE);
    LWLockAcquire(lock_cache, LW_EXCLUSIVE);

    masterKey = get_master_key_from_cache(dbOid);
    is_dup_key = (masterKey != NULL);

    /*  TODO: Add the key in the cache? */
    if (is_dup_key == false)
        is_dup_key = (pg_tde_get_master_key(dbOid, spcOid) != NULL);

    if (is_dup_key == false)
    {
        const keyInfo *keyInfo = NULL;

        masterKey = palloc(sizeof(TDEMasterKey));
        masterKey->keyInfo.databaseId = dbOid;
        masterKey->keyInfo.tablespaceId = spcOid;
        masterKey->keyInfo.keyId.version = DEFAULT_MASTER_KEY_VERSION;
        masterKey->keyInfo.keyringId = keyring->key_id;
        strncpy(masterKey->keyInfo.keyId.name, key_name, TDE_KEY_NAME_LEN);
        gettimeofday(&masterKey->keyInfo.creationTime, NULL);

        keyInfo = load_latest_versioned_key_name(&masterKey->keyInfo, keyring, ensure_new_key);

        if (keyInfo == NULL)
            keyInfo = KeyringGenerateNewKeyAndStore(keyring, masterKey->keyInfo.keyId.versioned_name, INTERNAL_KEY_LEN, false);

        if (keyInfo == NULL)
        {
            LWLockRelease(lock_cache);
            LWLockRelease(lock_files);

            ereport(ERROR,
                    (errmsg("failed to retrieve master key")));
        }

        masterKey->keyLength = keyInfo->data.len;
        memcpy(masterKey->keyData, keyInfo->data.data, keyInfo->data.len);

        save_master_key_info(&masterKey->keyInfo);

        /* XLog the new key*/
        XLogBeginInsert();
	    XLogRegisterData((char *) &masterKey->keyInfo, sizeof(TDEMasterKeyInfo));
	    XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ADD_MASTER_KEY);

        push_master_key_to_cache(masterKey);
    }

    LWLockRelease(lock_cache);
    LWLockRelease(lock_files);

    if (is_dup_key)
    {
        /*
         * Seems like just before we got the lock, the key was installed by some other caller
         * Throw an error and mover no
         */

        ereport(ERROR,
            (errcode(ERRCODE_DUPLICATE_OBJECT),
                 errmsg("Master key already exists for the database"),
                 errhint("Use rotate_key interface to change the master key")));
    }

    return masterKey;
}

bool
SetMasterKey(const char *key_name, const char *provider_name, bool ensure_new_key)
{
    TDEMasterKey *master_key = set_master_key_with_keyring(key_name, 
                                        GetKeyProviderByName(provider_name), 
                                        MyDatabaseId, MyDatabaseTableSpace, 
                                        ensure_new_key);

    return (master_key != NULL);
}

bool
RotateMasterKey(const char *new_key_name, const char *new_provider_name, bool ensure_new_key)
{
    TDEMasterKey *master_key = GetMasterKey(MyDatabaseId, MyDatabaseTableSpace, NULL);
    TDEMasterKey new_master_key;
    const keyInfo *keyInfo = NULL;
    GenericKeyring *keyring;

    /*
     * Let's set everything the same as the older master key and
     * update only the required attributes.
     * */
    memcpy(&new_master_key, master_key, sizeof(TDEMasterKey));

    if (new_key_name == NULL)
    {
        new_master_key.keyInfo.keyId.version++;
    }
    else
    {
        strncpy(new_master_key.keyInfo.keyId.name, new_key_name, sizeof(new_master_key.keyInfo.keyId.name));
        new_master_key.keyInfo.keyId.version = DEFAULT_MASTER_KEY_VERSION;

        if (new_provider_name != NULL)
        {
            new_master_key.keyInfo.keyringId = GetKeyProviderByName(new_provider_name)->key_id;
        }
    }

    /* We need a valid keyring structure */
    keyring = GetKeyProviderByID(new_master_key.keyInfo.keyringId);

    keyInfo = load_latest_versioned_key_name(&new_master_key.keyInfo, keyring, ensure_new_key);

    if (keyInfo == NULL)
        keyInfo = KeyringGenerateNewKeyAndStore(keyring, new_master_key.keyInfo.keyId.versioned_name, INTERNAL_KEY_LEN, false);

    if (keyInfo == NULL)
    {
        ereport(ERROR,
                (errmsg("Failed to generate new key name")));
    }

    new_master_key.keyLength = keyInfo->data.len;
    memcpy(new_master_key.keyData, keyInfo->data.data, keyInfo->data.len);
    clear_master_key_cache(MyDatabaseId, MyDatabaseTableSpace);
    return pg_tde_perform_rotate_key(master_key, &new_master_key);
}

/*
 * Rotate keys on a standby.
 */
bool
xl_tde_perform_rotate_key(XLogMasterKeyRotate *xlrec)
{
    bool ret;

    ret = pg_tde_write_map_keydata_files(xlrec->map_size, xlrec->buff, xlrec->keydata_size, &xlrec->buff[xlrec->map_size]);
    clear_master_key_cache(MyDatabaseId, MyDatabaseTableSpace);

	return ret;
}

/*
* Load the latest versioned key name for the master key
* If ensure_new_key is true, then we will keep on incrementing the version number
* till we get a key name that is not present in the keyring
*/
keyInfo *
load_latest_versioned_key_name(TDEMasterKeyInfo *mastere_key_info, GenericKeyring *keyring, bool ensure_new_key)
{
    KeyringReturnCodes kr_ret;
    keyInfo *keyInfo = NULL;
    int base_version = mastere_key_info->keyId.version;
    Assert(mastere_key_info != NULL);
    Assert(keyring != NULL);
    Assert(strlen(mastere_key_info->keyId.name) > 0);
    /* Start with the passed in version number
     * We expect the name and the version number are already properly initialized
     * and contain the correct values
     */
    snprintf(mastere_key_info->keyId.versioned_name, TDE_KEY_NAME_LEN,
             "%s_%d", mastere_key_info->keyId.name, mastere_key_info->keyId.version);

    while (true)
    {
        keyInfo = KeyringGetKey(keyring, mastere_key_info->keyId.versioned_name, false, &kr_ret);
        /* vault-v2 returns 404 (KEYRING_CODE_RESOURCE_NOT_AVAILABLE) when key is not found */
        if (kr_ret != KEYRING_CODE_SUCCESS && kr_ret != KEYRING_CODE_RESOURCE_NOT_AVAILABLE)
        {
            ereport(ERROR,
                (errmsg("failed to retrieve master key from keyring provider :\"%s\"", keyring->provider_name),
                    errdetail("Error code: %d", kr_ret)));
        }
        if (keyInfo == NULL)
        {
            if (ensure_new_key == false)
            {
                /*
                 * If ensure_key is false and we are not at the base version,
                 * We should return the last existent version.
                 */
                if (base_version < mastere_key_info->keyId.version)
                {
                    /* Not optimal but keep the things simple */
                    mastere_key_info->keyId.version -= 1;
                    snprintf(mastere_key_info->keyId.versioned_name, TDE_KEY_NAME_LEN,
                             "%s_%d", mastere_key_info->keyId.name, mastere_key_info->keyId.version);
                    keyInfo = KeyringGetKey(keyring, mastere_key_info->keyId.versioned_name, false, &kr_ret);
                }
            }
            return keyInfo;
        }

        mastere_key_info->keyId.version++;
        snprintf(mastere_key_info->keyId.versioned_name, TDE_KEY_NAME_LEN, "%s_%d", mastere_key_info->keyId.name, mastere_key_info->keyId.version);

        /*
         * Not really required. Just to break the infinite loop in case the key provider is not behaving sane.
         */
        if (mastere_key_info->keyId.version > MAX_MASTER_KEY_VERSION_NUM)
        {
            ereport(ERROR,
                    (errmsg("failed to retrieve master key. %d versions already exist", MAX_MASTER_KEY_VERSION_NUM)));
        }
    }
    return NULL; /* Just to keep compiler quite */
}
/*
 * Returns the provider ID of the keyring that holds the master key
 * Return InvalidOid if the master key is not set for the database
 */
Oid
GetMasterKeyProviderId(void)
{
    TDEMasterKey *masterKey = NULL;
    TDEMasterKeyInfo *masterKeyInfo = NULL;
    Oid keyringId = InvalidOid;
    Oid dbOid = MyDatabaseId;
    LWLock *lock_files = tde_lwlock_mk_files();
    LWLock *lock_cache = tde_lwlock_mk_cache();

    LWLockAcquire(lock_files, LW_SHARED);
    LWLockAcquire(lock_cache, LW_SHARED);

    masterKey = get_master_key_from_cache(dbOid);
    if (masterKey)
    {
        keyringId = masterKey->keyInfo.keyringId;
    }
    {
        /* Master key not present in cache. Try Loading it from the info file */
        masterKeyInfo = pg_tde_get_master_key(dbOid, MyDatabaseTableSpace);
        if (masterKeyInfo)
        {
            keyringId = masterKeyInfo->keyringId;
            pfree(masterKeyInfo);
        }
    }

    LWLockRelease(lock_cache);
    LWLockRelease(lock_files);

    return keyringId;
}

/*
 * ------------------------------
 * Master key cache realted stuff
 */

static inline dshash_table *
get_master_key_Hash(void)
{
    master_key_info_attach_shmem();
    return masterKeyLocalState.sharedHash;
}

/*
 * Gets the master key for current database from cache
 */
static TDEMasterKey *
get_master_key_from_cache(Oid dbOid)
{
    TDEMasterKey *cacheEntry = NULL;

    cacheEntry = (TDEMasterKey *)dshash_find(get_master_key_Hash(),
                                             &dbOid, false);
    if (cacheEntry)
        dshash_release_lock(get_master_key_Hash(), cacheEntry);

    return cacheEntry;
}

/*
 * Push the master key for current database to the shared memory cache.
 * TODO: Add eviction policy
 * For now we just keep pushing the master keys to the cache and do not have
 * any eviction policy. We have one master key for a database, so at max,
 * we could have as many entries in the cache as the number of databases.
 * Which in practice would not be a huge number, but still we need to have
 * some eviction policy in place. Moreover, we need to have some mechanism to
 * remove the cache entry when the database is dropped.
 */
static void
push_master_key_to_cache(TDEMasterKey *masterKey)
{
    TDEMasterKey *cacheEntry = NULL;
    Oid databaseId = MyDatabaseId;
    bool found = false;
    cacheEntry = dshash_find_or_insert(get_master_key_Hash(),
                                       &databaseId, &found);
    if (!found)
        memcpy(cacheEntry, masterKey, sizeof(TDEMasterKey));
    dshash_release_lock(get_master_key_Hash(), cacheEntry);
}

/*
 * Cleanup the master key cache entry for the current database.
 * This function is a hack to handle the situation if the
 * extension was dropped from the database and had created the
 * master key info file and cache entry in its previous encarnation.
 * We need to remove the cache entry and the master key info file
 * at the time of extension creation to start fresh again.
 * Idelly we should have a mechanism to remove these when the extension
 * but unfortunately we do not have any such mechanism in PG.
*/
static void
master_key_startup_cleanup(int tde_tbl_count, void* arg)
{
    XLogMasterKeyCleanup xlrec;

    if (tde_tbl_count > 0)
    {
        ereport(WARNING,
                (errmsg("Failed to perform initialization. database already has %d TDE tables", tde_tbl_count)));
        return;
    }

    cleanup_master_key_info(MyDatabaseId, MyDatabaseTableSpace);

    /* XLog the key cleanup */
    xlrec.databaseId = MyDatabaseId;
    xlrec.tablespaceId = MyDatabaseTableSpace;
    XLogBeginInsert();
    XLogRegisterData((char *) &xlrec, sizeof(TDEMasterKeyInfo));
    XLogInsert(RM_TDERMGR_ID, XLOG_TDE_CLEAN_MASTER_KEY);
}

void
cleanup_master_key_info(Oid databaseId, Oid tablespaceId)
{
    clear_master_key_cache(databaseId, tablespaceId);
    /*
        * TODO: Although should never happen. Still verify if any table in the
        * database is using tde
        */

    /* Remove the tde files */
    pg_tde_delete_tde_files(databaseId, tablespaceId);
}

static void
clear_master_key_cache(Oid databaseId, Oid tablespaceId)
{
    TDEMasterKey *cache_entry;

    /* Start with deleting the cache entry for the database */
    cache_entry = (TDEMasterKey *)dshash_find(get_master_key_Hash(),
                                              &databaseId, true);
    if (cache_entry)
    {
        dshash_delete_entry(get_master_key_Hash(), cache_entry);
    }
}

/*
 * SQL interface to set master key
 */
PG_FUNCTION_INFO_V1(pg_tde_set_master_key);
Datum pg_tde_set_master_key(PG_FUNCTION_ARGS);

Datum pg_tde_set_master_key(PG_FUNCTION_ARGS)
{
    char *master_key_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *provider_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
    bool ensure_new_key = PG_GETARG_BOOL(2);
	bool ret;

    ereport(LOG, (errmsg("Setting master key [%s : %s] for the database", master_key_name, provider_name)));
    ret = SetMasterKey(master_key_name, provider_name, ensure_new_key);
    PG_RETURN_BOOL(ret);
}

/*
 * SQL interface for key rotation
 */
PG_FUNCTION_INFO_V1(pg_tde_rotate_key);
Datum
pg_tde_rotate_key(PG_FUNCTION_ARGS)
{
    char *new_master_key_name = NULL;
    char *new_provider_name =  NULL;
    bool ensure_new_key;
    bool ret;

    if (!PG_ARGISNULL(0))
        new_master_key_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    if (!PG_ARGISNULL(1))
        new_provider_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
    ensure_new_key = PG_GETARG_BOOL(2);


    ereport(LOG, (errmsg("Rotating master key to [%s : %s] for the database", new_master_key_name, new_provider_name)));
    ret = RotateMasterKey(new_master_key_name, new_provider_name, ensure_new_key);
    PG_RETURN_BOOL(ret);
}

PG_FUNCTION_INFO_V1(pg_tde_master_key_info);
Datum pg_tde_master_key_info(PG_FUNCTION_ARGS)
{
    TupleDesc tupdesc;
    Datum values[6];
    bool isnull[6];
    HeapTuple tuple;
    Datum result;
    TDEMasterKey *master_key;
    TimestampTz ts;
    GenericKeyring *keyring;

    /* Build a tuple descriptor for our result type */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("function returning record called in context that cannot accept type record")));

    master_key = GetMasterKey(MyDatabaseId, MyDatabaseTableSpace, NULL);
    if (master_key == NULL)
        PG_RETURN_NULL();

    keyring = GetKeyProviderByID(master_key->keyInfo.keyringId);

    /* Initialize the values and null flags */

    /* TEXT: Master key name */
    values[0] = CStringGetTextDatum(master_key->keyInfo.keyId.name);
    isnull[0] = false;
    /* TEXT: Keyring provider name */
    if (keyring)
    {
        values[1] = CStringGetTextDatum(keyring->provider_name);
        isnull[1] = false;
    }
    else
        isnull[1] = true;

    /* INTEGERT:  key provider id */
    values[2] = Int32GetDatum(master_key->keyInfo.keyringId);
    isnull[2] = false;

    /* TEXT: Master key versioned name */
    values[3] = CStringGetTextDatum(master_key->keyInfo.keyId.versioned_name);
    isnull[3] = false;
    /* INTEGERT: Master key version */
    values[4] = Int32GetDatum(master_key->keyInfo.keyId.version);
    isnull[4] = false;
    /* TIMESTAMP TZ: Master key creation time */
    ts = (TimestampTz)master_key->keyInfo.creationTime.tv_sec - ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
    ts = (ts * USECS_PER_SEC) + master_key->keyInfo.creationTime.tv_usec;
    values[5] = TimestampTzGetDatum(ts);
    isnull[5] = false;

    /* Form the tuple */
    tuple = heap_form_tuple(tupdesc, values, isnull);

    /* Make the tuple into a datum */
    result = HeapTupleGetDatum(tuple);

    PG_RETURN_DATUM(result);
}
