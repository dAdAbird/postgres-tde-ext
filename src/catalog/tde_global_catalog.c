/*-------------------------------------------------------------------------
 *
 * tde_global_catalog.c
 *	  Global catalog key management
 *
 *
 * IDENTIFICATION
 *	  src/catalog/tde_global_catalog.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/pg_tde_tdemap.h"
#include "catalog/tde_global_catalog.h"
#include "catalog/tde_keyring.h"
#include "catalog/tde_master_key.h"

typedef enum
{
	TDE_GCAT_KEY_XLOG,

	/* must be last */
	TDE_GCAT_KEYS_COUNT
} GlobalCatalogKeyTypes;

typedef struct EncryptionStateData
{
	GenericKeyring *keyring;
	/* TODO: locking */
	TDEMasterKey master_keys[TDE_GCAT_KEYS_COUNT];
} EncryptionStateData;

static EncryptionStateData *EncryptionState = NULL;

/* GUC */
static char *KRingProviderType = NULL;
static char *KRingProviderFilePath = NULL;

static void create_gl_catalog_key(void);
static void init_keyring(void);

void
TDEGlCatInitGUC(void)
{
	DefineCustomStringVariable("pg_tde.global_keyring_type",
							   "Keyring type for XLog",
							   NULL,
							   &KRingProviderType,
							   NULL,
							   PGC_POSTMASTER,
							   0,	/* no flags required */
							   NULL,
							   NULL,
							   NULL
		);
	DefineCustomStringVariable("pg_tde.global_keyring_file_path",
							   "Keyring file options for XLog",
							   NULL,
							   &KRingProviderFilePath,
							   NULL,
							   PGC_POSTMASTER,
							   0,	/* no flags required */
							   NULL,
							   NULL,
							   NULL
		);
}


Size
TDEGlCatEncStateSize()
{
	Size size;

	size = sizeof(EncryptionStateData);
	size = add_size(size, sizeof(KeyringProviders));

	return MAXALIGN(size);
}

void
TDEGlCatShmemInit(void)
{
	bool	foundBuf;
	char	*allocptr;
	char	db_map_path[MAXPGPATH] = {0};

	EncryptionState = (EncryptionStateData *)
			ShmemInitStruct("TDE XLog Encryption State",
									XLogEncStateSize(), &foundBuf);

	allocptr = ((char *) EncryptionState) + MAXALIGN(sizeof(EncryptionStateData));
	EncryptionState->keyring = allocptr;
	memset(EncryptionState->keyring, 0, sizeof(KeyringProviders));
	memset(EncryptionState->master_keys, 0, TDEMasterKey * TDE_GCAT_KEYS_COUNT);
	
	init_keyring();

	pg_tde_set_db_file_paths(&GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID),
							db_map_path, NULL);
	if (access(db_map_path, F_OK) == -1)
	{
		create_gl_catalog_key();
	}
}

static void
init_keyring(void)
{
	EncryptionState->keyring->type = get_keyring_provider_from_typename(KRingProviderType);
	switch (EncryptionState->keyring->type)
	{
		case FILE_KEY_PROVIDER:
			FileKeyring *kring = (FileKeyring *) EncryptionState->keyring;
			strncpy(kring->file_name, KRingProviderFilePath, sizeof(kring->file_name));
			break;
	}
}

static void
create_gl_catalog_key(void)
{
	InternalKey		int_key;
	RelKeyData		*rel_key_data;
	RelKeyData		*enc_rel_key_data;
	RelFileLocator	*rlocator = &GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID);
	TDEMasterKey 	*master_key;

    master_key = create_key("xlog-master-key", EncryptionState->keyring, 
									rlocator->dbOid, rlocator->spcOid, false);

	memset(&int_key, 0, sizeof(InternalKey));

	if (!RAND_bytes(int_key.key, INTERNAL_KEY_LEN))
	{
		ereport(FATAL,
				(errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not generate internal key for \"WAL\": %s",
                		ERR_error_string(ERR_get_error(), NULL))));
	}

	rel_key_data = tde_create_rel_key(rlocator->relNumber, &int_key, &master_key->keyInfo);
	enc_rel_key_data = tde_encrypt_rel_key(master_key, rel_key_data, rlocator);

	pg_tde_write_key_map_entry(rlocator, enc_rel_key_data, &master_key->keyInfo);

	push_key_to_cache(master_key);
}

static TDEMasterKey *
get_key_from_cache(void)
{
	TDEMasterKey *mkey;
	
	mkey = &EncryptionState->master_keys[TDE_GCAT_KEY_XLOG];
	if (mkey->keyLength == 0)
		return NULL;

	return mkey;
}

static inline void
push_key_to_cache(TDEMasterKey *mkey)
{
	memcpy(EncryptionState->master_keys + TDE_GCAT_KEY_XLOG, mkey, sizeof(TDEMasterKey));
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
static TDEMasterKey *
create_key(const char *key_name, GenericKeyring *keyring,
                            Oid dbOid, Oid spcOid, bool ensure_new_key)
{
	TDEMasterKey *masterKey;

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
		ereport(ERROR,
				(errmsg("failed to retrieve master key")));
	}

	masterKey->keyLength = keyInfo->data.len;
	memcpy(masterKey->keyData, keyInfo->data.data, keyInfo->data.len);

	return masterKey;
}