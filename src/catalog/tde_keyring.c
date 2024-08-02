/*-------------------------------------------------------------------------
 *
 * tde_keyring.c
 *      Deals with the tde keyring configuration
 *      routines.
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/catalog/tde_keyring.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/pg_tde_xlog.h"
#include "catalog/tde_global_space.h"
#include "catalog/tde_keyring.h"
#include "catalog/tde_principal_key.h"
#include "access/skey.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/fmgroids.h"
#include "common/pg_tde_utils.h"
#include "miscadmin.h"
#include "unistd.h"
#include "utils/builtins.h"
#include "pg_tde.h"
#ifdef FRONTEND
#include "common/logging.h"
#include "common/file_perm.h"
#else
#include "access/heapam.h"
#include "common/pg_tde_shmem.h"
#include "funcapi.h"
#include "access/relscan.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "executor/spi.h"
#endif


#ifdef FRONTEND
#define LWLockAcquire(lock, mode) NULL
#define LWLockRelease(lock_files) NULL
#define LWLock void
#define tde_lwlock_mk_files() NULL
#define tde_lwlock_mk_cache() NULL
#define tde_provider_info_lock() NULL

/*
 * Error handling
 */
static void fe_errmsg(const char *fmt, ...);
static void fe_errhint(const char *fmt, ...);
static void fe_errdetail(const char *fmt, ...);

int fe_error_level = 0;

#define errmsg(...) fe_errmsg(__VA_ARGS__)
#define errhint(...) fe_errhint(__VA_ARGS__)
#define errdetail(...) fe_errdetail(__VA_ARGS__)

#define errcode_for_file_access() NULL
#define errcode(e) NULL

#define elog(elevel, ...) pgtde_elog(elevel, __VA_ARGS__)
#define pgtde_elog(elevel, ...) \
	do {							\
		fe_error_level = elevel;	\
		fe_errmsg(__VA_ARGS__);		\
	} while(0)

#define ereport(elevel, ...) pgtde_ereport(elevel, __VA_ARGS__)
#define pgtde_ereport(elevel, ...)		\
	do {							\
		fe_error_level = elevel;	\
		__VA_ARGS__;				\
	} while(0)

void
fe_errmsg(const char *fmt, ...)
{
	va_list		ap;

	va_start(ap, fmt);

	if (fe_error_level >= ERROR)
		pg_log_error(fmt, ap);
	else if (fe_error_level >= WARNING)
		pg_log_warning(fmt, ap);
	else if (fe_error_level >= LOG)
		pg_log_info(fmt, ap);
	else
		pg_log_debug(fmt, ap);

	va_end(ap);
}

void
fe_errhint(const char *fmt, ...)
{
	va_list		ap;

	va_start(ap, fmt);

	if (fe_error_level >= ERROR)
		pg_log_error_hint(fmt, ap);
	else if (fe_error_level >= WARNING)
		pg_log_warning_hint(fmt, ap);
	else if (fe_error_level >= LOG)
		pg_log_info_hint(fmt, ap);
	else
		pg_log_debug_hint(fmt, ap);

	va_end(ap);
}
void
fe_errdetail(const char *fmt, ...)
{
	va_list		ap;

	va_start(ap, fmt);

	if (fe_error_level >= ERROR)
		pg_log_error_detail(fmt, ap);
	else if (fe_error_level >= WARNING)
		pg_log_warning_detail(fmt, ap);
	else if (fe_error_level >= LOG)
		pg_log_info_detail(fmt, ap);
	else
		pg_log_debug_detail(fmt, ap);

	va_end(ap);
}

char *
pg_tde_get_tde_file_dir(Oid dbOid, Oid spcOid)
{
	/* `dbOid` is set to a value for the XLog keys caching but GetDatabasePath() 
	 * expects it (`dbOid`) to be `0` if this is a global space.
	 */
	if (spcOid == GLOBALTABLESPACE_OID)
		return pstrdup("global");
	return GetDatabasePath(dbOid, spcOid);
}

#define BasicOpenFile(fname, flags) open(fname, flags, PG_FILE_MODE_OWNER)

#endif		/* FRONTEND */

PG_FUNCTION_INFO_V1(pg_tde_add_key_provider_internal);
Datum pg_tde_add_key_provider_internal(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tde_list_all_key_providers);
Datum pg_tde_list_all_key_providers(PG_FUNCTION_ARGS);

#define PG_TDE_KEYRING_FILENAME "pg_tde_keyrings"
/*
 * These token must be exactly same as defined in
 * pg_tde_add_key_provider_vault_v2 SQL interface
 */
#define VAULTV2_KEYRING_TOKEN_KEY "token"
#define VAULTV2_KEYRING_URL_KEY "url"
#define VAULTV2_KEYRING_MOUNT_PATH_KEY "mountPath"
#define VAULTV2_KEYRING_CA_PATH_KEY "caPath"

/*
 * These token must be exactly same as defined in
 * pg_tde_add_key_provider_file SQL interface
 */
#define FILE_KEYRING_PATH_KEY "path"
#define FILE_KEYRING_TYPE_KEY "type"
#define PG_TDE_LIST_PROVIDERS_COLS 4

typedef enum ProviderScanType
{
	PROVIDER_SCAN_BY_NAME,
	PROVIDER_SCAN_BY_ID,
	PROVIDER_SCAN_BY_TYPE,
	PROVIDER_SCAN_ALL
} ProviderScanType;

static List *scan_key_provider_file(ProviderScanType scanType, void *scanKey, Oid dbOid, Oid spcOid);

static FileKeyring *load_file_keyring_provider_options(Datum keyring_options);
static GenericKeyring *load_keyring_provider_options(ProviderType provider_type, Datum keyring_options);
static VaultV2Keyring *load_vaultV2_keyring_provider_options(Datum keyring_options);
static void debug_print_kerying(GenericKeyring *keyring);
static char *get_keyring_infofile_path(char *resPath, Oid dbOid, Oid spcOid);
static const char *get_keyring_provider_typename(ProviderType p_type);
static bool fetch_next_key_provider(int fd, off_t* curr_pos, KeyringProvideRecord *provider);
static GenericKeyring *load_keyring_provider_from_record(KeyringProvideRecord *provider);

#ifndef FRONTEND
static void key_provider_startup_cleanup(int tde_tbl_count, XLogExtensionInstall *ext_info, bool redo, void *arg);
static uint32 write_key_provider_info(KeyringProvideRecord *provider, 
									Oid database_id, Oid tablespace_id,
									off_t position, bool redo, bool recovery);

static Size initialize_shared_state(void *start_address);
static Size required_shared_mem_size(void);

typedef struct TdeKeyProviderInfoSharedState
{
	LWLock *Locks;
} TdeKeyProviderInfoSharedState;

TdeKeyProviderInfoSharedState*	sharedPrincipalKeyState = NULL; /* Lives in shared state */

static const TDEShmemSetupRoutine key_provider_info_shmem_routine = {
	.init_shared_state = initialize_shared_state,
	.init_dsa_area_objects = NULL,
	.required_shared_mem_size = required_shared_mem_size,
	.shmem_kill = NULL
	};

static Size
required_shared_mem_size(void)
{
	return MAXALIGN(sizeof(TdeKeyProviderInfoSharedState));
}

static Size
initialize_shared_state(void *start_address)
{
	sharedPrincipalKeyState = (TdeKeyProviderInfoSharedState *)start_address;
	sharedPrincipalKeyState->Locks = GetLWLocks();
	return sizeof(TdeKeyProviderInfoSharedState);
}

static inline LWLock *
tde_provider_info_lock(void)
{
	Assert(sharedPrincipalKeyState);
	return &sharedPrincipalKeyState->Locks[TDE_LWLOCK_PI_FILES];
}

void InitializeKeyProviderInfo(void)
{
	ereport(LOG, (errmsg("initializing TDE key provider info")));
	RegisterShmemRequest(&key_provider_info_shmem_routine);
	on_ext_install(key_provider_startup_cleanup, NULL);
}
static void
key_provider_startup_cleanup(int tde_tbl_count, XLogExtensionInstall *ext_info, bool redo, void *arg)
{

	if (tde_tbl_count > 0)
	{
		ereport(WARNING,
				(errmsg("failed to perform initialization. database already has %d TDE tables", tde_tbl_count)));
		return;
	}
	cleanup_key_provider_info(ext_info->database_id, ext_info->tablespace_id);
}

ProviderType
get_keyring_provider_from_typename(char *provider_type)
{
	if (provider_type == NULL)
		return UNKNOWN_KEY_PROVIDER;

	if (strcmp(FILE_KEYRING_TYPE, provider_type) == 0)
		return FILE_KEY_PROVIDER;
	if (strcmp(VAULTV2_KEYRING_TYPE, provider_type) == 0)
		return VAULT_V2_KEY_PROVIDER;
	return UNKNOWN_KEY_PROVIDER;
}

static const char *
get_keyring_provider_typename(ProviderType p_type)
{
	switch (p_type)
	{
	case FILE_KEY_PROVIDER:
		return FILE_KEYRING_TYPE;
	case VAULT_V2_KEY_PROVIDER:
		return VAULTV2_KEYRING_TYPE;
	default:
		break;
	}
	return NULL;
}

static GenericKeyring *load_keyring_provider_from_record(KeyringProvideRecord *provider)
{
	Datum option_datum;
	GenericKeyring *keyring = NULL;

	option_datum = CStringGetTextDatum(provider->options);

	keyring = load_keyring_provider_options(provider->provider_type, option_datum);
	if (keyring)
	{
		keyring->key_id = provider->provider_id;
		strncpy(keyring->provider_name, provider->provider_name, sizeof(keyring->provider_name));
		keyring->type = provider->provider_type;
		strncpy(keyring->options, provider->options, sizeof(keyring->options));
		debug_print_kerying(keyring);
	}
	return keyring;
}

List *
GetAllKeyringProviders(Oid dbOid, Oid spcOid)
{
	return scan_key_provider_file(PROVIDER_SCAN_ALL, NULL, dbOid, spcOid);
}

GenericKeyring *
GetKeyProviderByName(const char *provider_name, Oid dbOid, Oid spcOid)
{
	GenericKeyring *keyring = NULL;
	List *providers = scan_key_provider_file(PROVIDER_SCAN_BY_NAME, (void*)provider_name, dbOid, spcOid);
	if (providers != NIL)
	{
		keyring = (GenericKeyring *)linitial(providers);
		list_free(providers);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("key provider \"%s\" does not exists", provider_name),
				 errhint("Use pg_tde_add_key_provider interface to create the key provider")));
	}
	return keyring;
}

static uint32
write_key_provider_info(KeyringProvideRecord *provider, Oid database_id, 
					Oid tablespace_id, off_t position, bool redo, bool recovery)
{
	off_t bytes_written = 0;
	off_t curr_pos = 0;
	int fd;
	int max_provider_id = 0;
	char kp_info_path[MAXPGPATH] = {0};
	KeyringProvideRecord existing_provider;

	Assert(provider != NULL);

	get_keyring_infofile_path(kp_info_path, database_id, tablespace_id);

	LWLockAcquire(tde_provider_info_lock(), LW_EXCLUSIVE);

	fd = BasicOpenFile(kp_info_path, O_CREAT | O_RDWR | PG_BINARY);
	if (fd < 0)
	{
		LWLockRelease(tde_provider_info_lock());
		ereport(ERROR,
			(errcode_for_file_access(),
				errmsg("could not open tde file \"%s\": %m", kp_info_path)));
	}
	if (!redo)
	{
		/* we also need to verify the name conflict and generate the next provider ID */
		while (fetch_next_key_provider(fd, &curr_pos, &existing_provider))
		{
			if (strcmp(existing_provider.provider_name, provider->provider_name) == 0)
			{
				close(fd);
				LWLockRelease(tde_provider_info_lock());
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						errmsg("key provider \"%s\" already exists", provider->provider_name)));
			}
			if (max_provider_id < existing_provider.provider_id)
				max_provider_id = existing_provider.provider_id;
		}
		provider->provider_id = max_provider_id + 1;
		curr_pos = lseek(fd, 0, SEEK_END);

		/* emit the xlog here. So that we can handle partial file write errors
		 * but cannot make new WAL entries during recovery.
		 */
		if (!recovery)
		{
			KeyringProviderXLRecord xlrec;

			xlrec.database_id = database_id;
			xlrec.tablespace_id = tablespace_id;
			xlrec.offset_in_file = curr_pos;
			memcpy(&xlrec.provider, provider, sizeof(KeyringProvideRecord));

			XLogBeginInsert();
			XLogRegisterData((char *)&xlrec, sizeof(KeyringProviderXLRecord));
			XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ADD_KEY_PROVIDER_KEY);
		}
	}
	else
	{
		/* we are performing redo, just go to the position received from the
		 * xlog and write the record there.
		 * No need to verify the name conflict and generate the provider ID
		 */
		curr_pos = lseek(fd, position, SEEK_SET);
	}
	/*
	 * All good, Just add a new provider
	 */
	bytes_written = pg_pwrite(fd, provider, sizeof(KeyringProvideRecord), curr_pos);
	if (bytes_written != sizeof(KeyringProvideRecord))
	{
		close(fd);
		LWLockRelease(tde_provider_info_lock());
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("key provider info file \"%s\" can't be written: %m",
						kp_info_path)));
	}
	if (pg_fsync(fd) != 0)
	{
		close(fd);
		LWLockRelease(tde_provider_info_lock());
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						kp_info_path)));
	}
	close(fd);
	LWLockRelease(tde_provider_info_lock());
	return provider->provider_id;
}

/*
 * Save the key provider info to the file
 */
uint32
save_new_key_provider_info(KeyringProvideRecord* provider, Oid databaseId, Oid tablespaceId, bool recovery)
{
	return write_key_provider_info(provider, databaseId, tablespaceId, 0, false, recovery);
}

uint32
redo_key_provider_info(KeyringProviderXLRecord* xlrec)
{
	return write_key_provider_info(&xlrec->provider, xlrec->database_id, xlrec->tablespace_id, xlrec->offset_in_file, true, false);
}

void
cleanup_key_provider_info(Oid databaseId, Oid tablespaceId)
{
	/* Remove the key provider info file */
	char kp_info_path[MAXPGPATH] = {0};

	get_keyring_infofile_path(kp_info_path, databaseId, tablespaceId);
	PathNameDeleteTemporaryFile(kp_info_path, false);
}

Datum
pg_tde_add_key_provider_internal(PG_FUNCTION_ARGS)
{
	char *provider_type = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char *provider_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char *options = text_to_cstring(PG_GETARG_TEXT_PP(2));
	bool is_global = PG_GETARG_BOOL(3);
	KeyringProvideRecord provider;
	Oid dbOid = MyDatabaseId;
	Oid spcOid = MyDatabaseTableSpace;

	if (is_global)
	{
		dbOid = GLOBAL_DATA_TDE_OID;
		spcOid = GLOBALTABLESPACE_OID;
	}

	strncpy(provider.options, options, sizeof(provider.options));
	strncpy(provider.provider_name, provider_name, sizeof(provider.provider_name));
	provider.provider_type = get_keyring_provider_from_typename(provider_type);
	save_new_key_provider_info(&provider, dbOid, spcOid, false);

	PG_RETURN_INT32(provider.provider_id);
}

Datum
pg_tde_list_all_key_providers(PG_FUNCTION_ARGS)
{
	List* all_providers = GetAllKeyringProviders(MyDatabaseId, MyDatabaseTableSpace);
	ListCell *lc;
	Tuplestorestate *tupstore;
	TupleDesc tupdesc;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_tde_list_all_key_providers: set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_tde_list_all_key_providers: materialize mode required, but it is not allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "pg_tde_list_all_key_providers: return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	foreach (lc, all_providers)
	{
		Datum values[PG_TDE_LIST_PROVIDERS_COLS] = {0};
		bool nulls[PG_TDE_LIST_PROVIDERS_COLS] = {0};
		GenericKeyring *keyring = (GenericKeyring *)lfirst(lc);
		int i = 0;

		values[i++] = Int32GetDatum(keyring->key_id);
		values[i++] = CStringGetTextDatum(keyring->provider_name);
		values[i++] = CStringGetTextDatum(get_keyring_provider_typename(keyring->type));
		values[i++] = CStringGetTextDatum(keyring->options);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);

		debug_print_kerying(keyring);
	}
	list_free_deep(all_providers);
	return (Datum)0;
}
#endif		/* FRONTEND */


GenericKeyring *
GetKeyProviderByID(int provider_id, Oid dbOid, Oid spcOid)
{
	GenericKeyring *keyring = NULL;
	List *providers = scan_key_provider_file(PROVIDER_SCAN_BY_ID, &provider_id, dbOid, spcOid);
	if (providers != NIL)
	{
		keyring = (GenericKeyring *)linitial(providers);
		list_free(providers);
	}
	return keyring;
}


/*
	* Scan the key provider info file and can also apply filter based on scanType
	*/
static List *scan_key_provider_file(ProviderScanType scanType, void *scanKey, Oid dbOid, Oid spcOid)
{
	off_t curr_pos = 0;
	int fd;
	char kp_info_path[MAXPGPATH] = {0};
	KeyringProvideRecord provider;
	List *providers_list = NIL;

	if (scanType != PROVIDER_SCAN_ALL)
		Assert(scanKey != NULL);

	get_keyring_infofile_path(kp_info_path, dbOid, spcOid);

	LWLockAcquire(tde_provider_info_lock(), LW_SHARED);

	fd = BasicOpenFile(kp_info_path, PG_BINARY);
	if (fd < 0)
	{
		LWLockRelease(tde_provider_info_lock());
		ereport(DEBUG2,
			(errcode_for_file_access(),
				errmsg("could not open tde file \"%s\": %m", kp_info_path)));
		return NIL;
	}
	while (fetch_next_key_provider(fd, &curr_pos, &provider))
	{
		bool match = false;
		ereport(DEBUG2,
			(errmsg("read key provider ID=%d %s", provider.provider_id, provider.provider_name)));

		if (scanType == PROVIDER_SCAN_BY_NAME)
		{
			if (strcasecmp(provider.provider_name, (char*)scanKey) == 0)
				match = true;
		}
		else if (scanType == PROVIDER_SCAN_BY_ID)
		{
			if (provider.provider_id == *(int *)scanKey)
				match = true;
		}
		else if (scanType == PROVIDER_SCAN_BY_TYPE)
		{
			if (provider.provider_type == *(ProviderType*)scanKey)
				match = true;
		}
		else if (scanType == PROVIDER_SCAN_ALL)
			match = true;

		if (match)
		{
			GenericKeyring *keyring = load_keyring_provider_from_record(&provider);
			if (keyring)
			{
				providers_list = lappend(providers_list, keyring);
			}
		}
	}
	close(fd);
	LWLockRelease(tde_provider_info_lock());
	return providers_list;
}

static char*
get_keyring_infofile_path(char* resPath, Oid dbOid, Oid spcOid)
{
	char *db_path = pg_tde_get_tde_file_dir(dbOid, spcOid);
	Assert(db_path != NULL);
	join_path_components(resPath, db_path, PG_TDE_KEYRING_FILENAME);
	pfree(db_path);
	return resPath;
}


/*
 * Fetch the next key provider from the file and update the curr_pos
*/
static bool
fetch_next_key_provider(int fd, off_t* curr_pos, KeyringProvideRecord *provider)
{
	off_t bytes_read = 0;

	Assert(provider != NULL);
	Assert(fd >= 0);

	bytes_read = pg_pread(fd, provider, sizeof(KeyringProvideRecord), *curr_pos);
	*curr_pos += bytes_read;

	if (bytes_read == 0)
		return false;
	if (bytes_read != sizeof(KeyringProvideRecord))
	{
		close(fd);
		/* Corrupt file */
		ereport(ERROR,
				(errcode_for_file_access(),
					errmsg("key provider info file is corrupted: %m"),
					errdetail("invalid key provider record size %lld expected %lu", bytes_read, sizeof(KeyringProvideRecord) )));
	}
	return true;
}


static GenericKeyring *
load_keyring_provider_from_record(KeyringProvideRecord *provider)
{
	Datum option_datum;
	GenericKeyring *keyring = NULL;

	option_datum = CStringGetTextDatum(provider->options);

	keyring = load_keyring_provider_options(provider->provider_type, option_datum);
	if (keyring)
	{
		keyring->key_id = provider->provider_id;
		strncpy(keyring->provider_name, provider->provider_name, sizeof(keyring->provider_name));
		keyring->type = provider->provider_type;
		strncpy(keyring->options, provider->options, sizeof(keyring->options));
		debug_print_kerying(keyring);
	}
	return keyring;
}


static GenericKeyring *
load_keyring_provider_options(ProviderType provider_type, Datum keyring_options)
{
	switch (provider_type)
	{
	case FILE_KEY_PROVIDER:
		return (GenericKeyring *)load_file_keyring_provider_options(keyring_options);
		break;
	case VAULT_V2_KEY_PROVIDER:
		return (GenericKeyring *)load_vaultV2_keyring_provider_options(keyring_options);
		break;
	default:
		break;
	}
	return NULL;
}

static FileKeyring *
load_file_keyring_provider_options(Datum keyring_options)
{
	const char* file_path = extract_json_option_value(keyring_options, FILE_KEYRING_PATH_KEY);
	FileKeyring *file_keyring = palloc0(sizeof(FileKeyring));
	
	if(file_path == NULL)
	{
		ereport(DEBUG2,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("file path is missing in the keyring options")));
		return NULL;
	}

	file_keyring->keyring.type = FILE_KEY_PROVIDER;
	strncpy(file_keyring->file_name, file_path, sizeof(file_keyring->file_name));
	return file_keyring;
}

static VaultV2Keyring *
load_vaultV2_keyring_provider_options(Datum keyring_options)
{
	VaultV2Keyring *vaultV2_keyring = palloc0(sizeof(VaultV2Keyring));
	const char* token = extract_json_option_value(keyring_options, VAULTV2_KEYRING_TOKEN_KEY);
	const char* url = extract_json_option_value(keyring_options, VAULTV2_KEYRING_URL_KEY);
	const char* mount_path = extract_json_option_value(keyring_options, VAULTV2_KEYRING_MOUNT_PATH_KEY);
	const char* ca_path = extract_json_option_value(keyring_options, VAULTV2_KEYRING_CA_PATH_KEY);

	if(token == NULL || url == NULL || mount_path == NULL)
	{
		/* TODO: report error */
		return NULL;
	}

	vaultV2_keyring->keyring.type = VAULT_V2_KEY_PROVIDER;
	strncpy(vaultV2_keyring->vault_token, token, sizeof(vaultV2_keyring->vault_token));
	strncpy(vaultV2_keyring->vault_url, url, sizeof(vaultV2_keyring->vault_url));
	strncpy(vaultV2_keyring->vault_mount_path, mount_path, sizeof(vaultV2_keyring->vault_mount_path));
	strncpy(vaultV2_keyring->vault_ca_path, ca_path ? ca_path : "", sizeof(vaultV2_keyring->vault_ca_path));
	return vaultV2_keyring;
}

static void
debug_print_kerying(GenericKeyring *keyring)
{
	int debug_level = DEBUG2;
	elog(debug_level, "Keyring type: %d", keyring->type);
	elog(debug_level, "Keyring name: %s", keyring->provider_name);
	elog(debug_level, "Keyring id: %d", keyring->key_id);
	switch (keyring->type)
	{
	case FILE_KEY_PROVIDER:
		elog(debug_level, "File Keyring Path: %s", ((FileKeyring *)keyring)->file_name);
		break;
	case VAULT_V2_KEY_PROVIDER:
		elog(debug_level, "Vault Keyring Token: %s", ((VaultV2Keyring *)keyring)->vault_token);
		elog(debug_level, "Vault Keyring URL: %s", ((VaultV2Keyring *)keyring)->vault_url);
		elog(debug_level, "Vault Keyring Mount Path: %s", ((VaultV2Keyring *)keyring)->vault_mount_path);
		elog(debug_level, "Vault Keyring CA Path: %s", ((VaultV2Keyring *)keyring)->vault_ca_path);
		break;
	case UNKNOWN_KEY_PROVIDER:
		elog(debug_level, "Unknown Keyring ");
		break;
	}
}