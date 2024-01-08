/*-------------------------------------------------------------------------
 *
 * pg_tde_tdemap.c
 *	  tde relation fork manager code
 *
 *
 * IDENTIFICATION
 *	  src/access/pg_tde_tdemap.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/pg_tde_tdemap.h"
#include "transam/pg_tde_xact_handler.h"
#include "storage/fd.h"
#include "utils/wait_event.h"
#include "utils/memutils.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"

#include "access/pg_tde_tdemap.h"
#include "encryption/enc_aes.h"
#include "keyring/keyring_api.h"

#include <openssl/rand.h>
#include <openssl/err.h>

#include "pg_tde_defines.h"

/* TODO: should be a user defined */ 
static const char *MasterKeyName = "master-key";

static inline char* pg_tde_get_key_file_path(const RelFileLocator *newrlocator);
static void put_keys_into_map(Oid rel_id, RelKeysData *keys);
static void pg_tde_write_key_fork(const RelFileLocator *rlocator, InternalKey *key, InternalKey *keyEnc, const char *MasterKeyName);
static void pg_tde_xlog_create_fork(XLogReaderState *record);
static void pg_tde_crypt_internal_key(InternalKey *in, InternalKey *out);

void
pg_tde_delete_key_fork(Relation rel)
{
	/* TODO: delete related internal keys from cache */
	char    *key_file_path = pg_tde_get_key_file_path(&rel->rd_locator);
    if (!key_file_path)
	{
        ereport(ERROR,
                (errmsg("failed to get key file path")));
	}
	RegisterFileForDeletion(key_file_path, true);
	pfree(key_file_path);
}

/*
 * Creates a relation fork file relfilenode.tde that contains the
 * encryption key for the relation.
 */
void
pg_tde_create_key_fork(const RelFileLocator *newrlocator, Relation rel)
{
	InternalKey int_key = {0};
	InternalKey int_key_enc = {0};

	if (!RAND_bytes(int_key.key, INTERNAL_KEY_LEN))
	{
		ereport(FATAL,
				(errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not generate internal key for relation \"%s\": %s",
                		RelationGetRelationName(rel), ERR_error_string(ERR_get_error(), NULL))));
	}

	pg_tde_crypt_internal_key(&int_key, &int_key_enc);

	uint8 mklen = (uint8) strlen(MasterKeyName);
	/* XLOG internal keys */
	XLogBeginInsert();
	XLogRegisterData((char *) newrlocator, sizeof(RelFileLocator));
	XLogRegisterData((char *) &mklen, sizeof(uint8));
	XLogRegisterData((char *) MasterKeyName, strlen(MasterKeyName)+1);
	XLogRegisterData((char *) &int_key_enc, sizeof(InternalKey));
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_CREATE_FORK);

	/* TODO: should DB crash after sending XLog, secondaries would create a fork
	 * file but the relation won't be created either on primary or secondaries.
	 * Hence, the *.tde file will remain as garbage on secondaries.
	 */

	pg_tde_write_key_fork(newrlocator, &int_key, &int_key_enc, MasterKeyName);
}

void
pg_tde_write_key_fork(const RelFileLocator *rlocator, InternalKey *key, InternalKey *keyEnc, const char *MasterKeyName)
{
	char		*key_file_path;
	File		file = -1;
	RelKeysData *data;
	unsigned char        dataEnc[1024];
	RelKeysData *encData = dataEnc;
	size_t 		sz;

	key_file_path = pg_tde_get_key_file_path(rlocator);
	if (!key_file_path)
		ereport(ERROR,
				(errmsg("failed to get key file path")));

	file = PathNameOpenFile(key_file_path, O_RDWR | O_CREAT | PG_BINARY);
	if (file < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				errmsg("could not open tde key file \"%s\": %m",
						key_file_path)));

	/* Allocate in TopMemoryContext and don't pfree sice we add it to
	 * the cache as well */
	data = (RelKeysData *) MemoryContextAlloc(TopMemoryContext, SizeOfRelKeysData(1));

	strcpy(data->master_key_name, MasterKeyName);
	data->internal_key[0] = *key;
	data->internal_keys_len = 1;

	sz = SizeOfRelKeysData(data->internal_keys_len);
	
	memcpy(dataEnc, data, sz);
	memcpy(dataEnc + SizeOfRelKeysDataHeader, keyEnc, INTERNAL_KEY_LEN);

#if TDE_FORK_DEBUG
	ereport(DEBUG2,
		(errmsg("encrypted internal_key: %s", tde_sprint_key(&encData->internal_key[0]))));
#endif

	if (FileWrite(file, dataEnc, sz, 0, WAIT_EVENT_DATA_FILE_WRITE) != sz)
    	ereport(FATAL,
				(errcode_for_file_access(),
                errmsg("could not write key data to file \"%s\": %m",
                		key_file_path)));

	/* Register the file for delete in case transaction Aborts */
	RegisterFileForDeletion(key_file_path, false);

	/* Add to the cache */
	put_keys_into_map(rlocator->relNumber, data);

	pfree(key_file_path);
	FileClose(file);
}

/*
 * Reads tde keys for the relation fork file.
 */
RelKeysData *
pg_tde_get_keys_from_fork(const RelFileLocator *rlocator)
{
	char		*key_file_path;
	File		file = -1;
	Size		sz;
	int			nbytes;
	RelKeysData *keys;

    key_file_path = pg_tde_get_key_file_path(rlocator);
    if (!key_file_path)
	{
        ereport(ERROR,
                (errmsg("failed to get key file path")));
	}

	file = PathNameOpenFile(key_file_path, O_RDONLY | PG_BINARY);
	if (file < 0)
	{
		ereport(FATAL,
                (errcode_for_file_access(),
                errmsg("could not open tde key file \"%s\": %m",
						key_file_path)));
	}
	
	sz = (Size) FileSize(file);
	keys = (RelKeysData *) MemoryContextAlloc(TopMemoryContext, sz);

	nbytes = FileRead(file, keys, sz, 0, WAIT_EVENT_DATA_FILE_READ);
	if (nbytes < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				errmsg("could not read key data file \"%s\": %m",
						key_file_path)));
	else if (nbytes < SizeOfRelKeysData(1) || 
				(nbytes - SizeOfRelKeysDataHeader) % sizeof(InternalKey) != 0)
	{
		ereport(FATAL,
				(errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("corrupted key data in file \"%s\"",
						key_file_path)));
	}

#if TDE_FORK_DEBUG
	for (Size i = 0; i < keys->internal_keys_len; i++) 
		ereport(DEBUG2,
			(errmsg("encrypted fork file keys: [%lu] %s: %s", i+1, keys->master_key_name, tde_sprint_key(&keys->internal_key[i]))));
#endif

	pg_tde_crypt_internal_key(&keys->internal_key, &keys->internal_key);

#if TDE_FORK_DEBUG
	for (Size i = 0; i < keys->internal_keys_len; i++) 
		ereport(DEBUG2,
			(errmsg("fork file keys: [%lu] %s: %s", i+1, keys->master_key_name, tde_sprint_key(&keys->internal_key[i]))));
#endif

	pfree(key_file_path);
	/* For now just close the key file.*/
	FileClose(file);

	return keys;
}

/* Head of the keys cache (linked list) */
RelKeys *tde_rel_keys_map = NULL;

/*
 * Returns TDE keys for a given relation.
 * First it looks in a cache. If nothing found in the cache, it reads data from
 * the tde fork file and populates cache.
 */
RelKeysData *
GetRelationKeys(RelFileLocator rel)
{
	RelKeys		*curr;
	RelKeysData *keys;

	Oid rel_id = rel.relNumber;
	for (curr = tde_rel_keys_map; curr != NULL; curr = curr->next)
	{
		if (curr->rel_id == rel_id) {
#if TDE_FORK_DEBUG
			ereport(DEBUG2,
					(errmsg("TDE: cache hit, \"%s\" %s | (%d)",
							curr->keys->master_key_name,
							tde_sprint_key(&curr->keys->internal_key[0]), 
							rel_id)));
#endif
			return curr->keys;
		}
	}

	keys = pg_tde_get_keys_from_fork(&rel);

	put_keys_into_map(rel.relNumber, keys);

	return keys;
}

static void
put_keys_into_map(Oid rel_id, RelKeysData *keys) {
	RelKeys		*new;
	RelKeys		*prev = NULL;

	new = (RelKeys *) MemoryContextAlloc(TopMemoryContext, sizeof(RelKeys));
	new->rel_id = rel_id;
	new->keys = keys;
	new->next = NULL; 

	if (prev == NULL)
		tde_rel_keys_map = new;
	else
		prev->next = new;
}

const char *
tde_sprint_key(InternalKey *k)
{
	static char buf[256];
	int 	i;

	for (i = 0; i < sizeof(k->key); i++)
		sprintf(buf+i, "%02X", k->key[i]);

	sprintf(buf+i, "[%lu, %lu]", k->start_loc, k->end_loc);

	return buf;
}

const char *
tde_sprint_masterkey(const keyData *k)
{
	static char buf[256];
	int 	i;

	for (i = 0; i < k->len; i++)
		sprintf(buf+i, "%02X", k->data[i]);

	return buf;
}

/* returns the palloc'd key (TDE relation fork) file path */
static inline char*
pg_tde_get_key_file_path(const RelFileLocator *newrlocator)
{
    char        *rel_file_path;
    char        *key_file_path = NULL;

    /* We get a relation name for MAIN fork and manually append the
     * .tde postfix to the file name
     */
    rel_file_path = relpathperm(*newrlocator, MAIN_FORKNUM);
    if (rel_file_path)
    {
        key_file_path = psprintf("%s."TDE_FORK_EXT, rel_file_path);
        pfree(rel_file_path);
    }
    return key_file_path;
}

void 
pg_tde_crypt_internal_key(InternalKey *in, InternalKey *out)
{
	const keyInfo 	*master_key_info;
	unsigned char	dataDec[1024];
	int				encsz;
	// TODO: use proper iv stored in the file!
	unsigned char iv[INTERNAL_KEY_LEN] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	master_key_info = keyringGetLatestKey(MasterKeyName);
	if(master_key_info == NULL)
	{
		master_key_info = keyringGenerateKey(MasterKeyName, INTERNAL_KEY_LEN);
	}
	if(master_key_info == NULL)
	{
		ereport(ERROR,
				(errmsg("failed to retrieve master key")));
	}
	AesDecrypt(master_key_info->data.data, iv, (unsigned char*) in, INTERNAL_KEY_LEN, dataDec, &encsz);

	memcpy(out, dataDec, INTERNAL_KEY_LEN);
}

/* 
 * TDE fork XLog 
 */
void
pg_tde_rmgr_redo(XLogReaderState *record)
{
	uint8	info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_TDE_CREATE_FORK:
			pg_tde_xlog_create_fork(record);
			break;
		default:
			elog(PANIC, "pg_tde_redo: unknown op code %u", info);
	}
}

void
pg_tde_rmgr_desc(StringInfo buf, XLogReaderState *record)
{
	uint8			info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char			*rec = XLogRecGetData(record);
	RelFileLocator	rlocator;

	if (info == XLOG_TDE_CREATE_FORK)
	{
		memcpy(&rlocator, rec, sizeof(RelFileLocator));
		appendStringInfo(buf, "create tde fork for relation %u/%u", rlocator.dbOid, rlocator.relNumber);
	}
}

const char *
pg_tde_rmgr_identify(uint8 info)
{
	if ((info & ~XLR_INFO_MASK) == XLOG_TDE_CREATE_FORK)
		return "TDE_CREATE_FORK";

	return NULL;
}

static void
pg_tde_xlog_create_fork(XLogReaderState *record)
{
	char			*rec = XLogRecGetData(record);
	RelFileLocator	rlocator;
	InternalKey 	int_key = {0};
	InternalKey 	int_key_enc = {0};
	uint8 			mklen;
	char			mkname[MASTER_KEY_NAME_LEN];

	if (XLogRecGetDataLen(record) < sizeof(InternalKey)+sizeof(RelFileLocator))
	{
		ereport(FATAL,
				(errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("corrupted XLOG_TDE_CREATE_FORK data")));
	}


	/* Format [RelFileLocator][MasterKeyNameLen][MasterKeyName][InternalKey] */
	memcpy(&rlocator, rec, sizeof(RelFileLocator));
	memcpy(&mklen, rec+sizeof(RelFileLocator), sizeof(mklen));
	memcpy(&mkname, rec+sizeof(RelFileLocator)+sizeof(mklen), mklen+1);
	memcpy(&int_key_enc, rec+sizeof(RelFileLocator)+sizeof(mklen)+1+mklen, sizeof(InternalKey));

	pg_tde_crypt_internal_key(&int_key_enc, &int_key);

#if TDE_FORK_DEBUG
	ereport(DEBUG2,
		(errmsg("xlog internal_key: %s", tde_sprint_key(&int_key))));
#endif

	pg_tde_write_key_fork(&rlocator, &int_key, &int_key_enc, mkname);	
}
