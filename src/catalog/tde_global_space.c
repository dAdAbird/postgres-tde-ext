/*-------------------------------------------------------------------------
 *
 * tde_global_space.c
 *	  Global catalog key management
 *
 *
 * IDENTIFICATION
 *	  src/catalog/tde_global_space.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef PERCONA_FORK

#include "catalog/pg_tablespace_d.h"
#include "utils/memutils.h"
#include "common/logging.h"

#include "access/pg_tde_tdemap.h"
#include "catalog/tde_global_space.h"
#include "catalog/tde_keyring.h"

#include <openssl/rand.h>
#include <openssl/err.h>
#include <sys/time.h>

#define PRINCIPAL_KEY_DEFAULT_NAME	"tde-global-catalog-key"
#define KEYRING_DEFAULT_NAME "default_global_tablespace_keyring"

#define DefaultKeyProvider GetKeyProviderByName(KEYRING_DEFAULT_NAME, \
										GLOBAL_DATA_TDE_OID, GLOBALTABLESPACE_OID)

typedef enum
{
	TDE_INTERNAL_XLOG_KEY,

	/* must be last */
	TDE_INTERNAL_KEYS_COUNT
}			InternalKeyType;

/* Since for the global tablespace, we always keep the Internal key in the memory
 * and read it from disk only once during the server start, we need no cache for
 * the principal key.
 */
static RelKeyData * internal_keys_cache = NULL;
static void cache_internal_key(RelKeyData * ikey, InternalKeyType type);

#ifndef FRONTEND
static void init_keys(void);
static void init_default_keyring(void);
static TDEPrincipalKey * create_principal_key(const char *key_name,
											  GenericKeyring * keyring, Oid dbOid,
											  Oid spcOid);
#endif		/* FRONTEND */


void
TDEInitGlobalKeys(void)
{
#ifndef FRONTEND
	char		db_map_path[MAXPGPATH] = {0};

	pg_tde_set_db_file_paths(&GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID),
							 db_map_path, NULL);
	if (access(db_map_path, F_OK) == -1)
	{
		init_default_keyring();
		init_keys();
	}
	else
#endif		/* FRONTEND */
	{
		RelKeyData *ikey;

		ikey = pg_tde_get_key_from_file(&GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID));
		cache_internal_key(ikey, TDE_INTERNAL_XLOG_KEY);
	}
}

/*
 * Internal Key should be in the TopMemmoryContext because of SSL contexts. This
 * context is being initialized by OpenSSL with the pointer to the encryption
 * context which is valid only for the current backend. So new backends have to
 * inherit a cached key with NULL SSL connext and any changes to it have to remain
 * local ot the backend.
 * (see https://github.com/Percona-Lab/pg_tde/pull/214#discussion_r1648998317)
 */
static void
cache_internal_key(RelKeyData * ikey, InternalKeyType type)
{
	if (internal_keys_cache == NULL)
	{
#ifndef FRONTEND
	    MemoryContext oldcontext;

	    oldcontext = MemoryContextSwitchTo(TopMemoryContext);
#endif

		internal_keys_cache =
			(RelKeyData *) palloc(sizeof(RelKeyData) * TDE_INTERNAL_KEYS_COUNT);

#ifndef FRONTEND
    	MemoryContextSwitchTo(oldcontext);
#endif
	}
	memcpy(internal_keys_cache + type, ikey, sizeof(RelKeyData));
}


RelKeyData *
TDEGetGlobalInternalKey(Oid obj_id)
{
	InternalKeyType ktype;

	Assert(internal_keys_cache != NULL);
	switch (obj_id)
	{
		case XLOG_TDE_OID:
			ktype = TDE_INTERNAL_XLOG_KEY;
			break;
		default:
#ifndef FRONTEND
			elog(ERROR, "unknown internal key for Oid %u", obj_id);
#else
			pg_log_error("unknown internal key for Oid %u", obj_id);
#endif		/* FRONTEND */

	}
	return internal_keys_cache + ktype;
}

#ifndef FRONTEND

static void
init_default_keyring(void)
{
	if (GetAllKeyringProviders(GLOBAL_DATA_TDE_OID, GLOBALTABLESPACE_OID) == NIL)
	{
		static KeyringProvideRecord provider =
		{
			.provider_name = KEYRING_DEFAULT_NAME,
				.provider_type = FILE_KEY_PROVIDER,
				.options =
				"{"
				"\"type\": \"file\","
				" \"path\": \"pg_tde_default_keyring_CHANGE_AND_REMOVE_IT\""	/* TODO: not sure about
																				 * the location */
				"}"
		};

		/*
		 * TODO: should we remove it automaticaly on
		 * pg_tde_rotate_principal_key() ?
		 */
		save_new_key_provider_info(&provider, GLOBAL_DATA_TDE_OID, GLOBALTABLESPACE_OID, true);
		elog(INFO,
			 "default keyring has been created for the global tablespace (WAL)."
			 " Change it with pg_tde_add_key_provider_* and run pg_tde_rotate_principal_key."
			);
	}
}

/*
 * Create and store global space keys (principal and internal) and cache the
 * internal key.
 *
 * This function has to be run during the cluster start only, so no locks here.
 */
static void
init_keys(void)
{
	InternalKey int_key;
	RelKeyData *rel_key_data;
	RelKeyData *enc_rel_key_data;
	RelFileLocator *rlocator;
	TDEPrincipalKey *mkey;

	mkey = create_principal_key(PRINCIPAL_KEY_DEFAULT_NAME,
								DefaultKeyProvider,
								GLOBAL_DATA_TDE_OID, GLOBALTABLESPACE_OID);

	memset(&int_key, 0, sizeof(InternalKey));

	/* Create and store an internal key for XLog */
	if (!RAND_bytes(int_key.key, INTERNAL_KEY_LEN))
	{
		ereport(FATAL,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate internal key for \"WAL\": %s",
						ERR_error_string(ERR_get_error(), NULL))));
	}

	rlocator = &GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID);
	rel_key_data = tde_create_rel_key(rlocator->relNumber, &int_key, &mkey->keyInfo);
	enc_rel_key_data = tde_encrypt_rel_key(mkey, rel_key_data, rlocator);
	pg_tde_write_key_map_entry(rlocator, enc_rel_key_data, &mkey->keyInfo);

	cache_internal_key(rel_key_data, TDE_INTERNAL_XLOG_KEY);
	pfree(rel_key_data);
	pfree(mkey);
}

/*
 * Substantially simplified version of set_principal_key_with_keyring() as during
 * recovery (server start):
 * - we can't insert XLog records;
 * - no need for locks;
 * - we run this func only once, during the first server start and always create
 *   a new key with the default keyring, hence no need to try to load the key
 *   first.
 */
static TDEPrincipalKey *
create_principal_key(const char *key_name, GenericKeyring * keyring,
					 Oid dbOid, Oid spcOid)
{
	TDEPrincipalKey *principalKey;
	keyInfo    *keyInfo = NULL;

	principalKey = palloc(sizeof(TDEPrincipalKey));
	principalKey->keyInfo.databaseId = dbOid;
	principalKey->keyInfo.tablespaceId = spcOid;
	principalKey->keyInfo.keyId.version = DEFAULT_PRINCIPAL_KEY_VERSION;
	principalKey->keyInfo.keyringId = keyring->key_id;
	strncpy(principalKey->keyInfo.keyId.name, key_name, TDE_KEY_NAME_LEN);
	snprintf(principalKey->keyInfo.keyId.versioned_name, TDE_KEY_NAME_LEN,
			"%s_%d", principalKey->keyInfo.keyId.name, principalKey->keyInfo.keyId.version);
	gettimeofday(&principalKey->keyInfo.creationTime, NULL);

	keyInfo = KeyringGenerateNewKeyAndStore(keyring, principalKey->keyInfo.keyId.versioned_name, INTERNAL_KEY_LEN, false);

	if (keyInfo == NULL)
	{
		ereport(ERROR,
				(errmsg("failed to generate principal key")));
	}

	principalKey->keyLength = keyInfo->data.len;
	memcpy(principalKey->keyData, keyInfo->data.data, keyInfo->data.len);

	return principalKey;
}
#endif		/* FRONTEND */

#endif							/* PERCONA_FORK */
