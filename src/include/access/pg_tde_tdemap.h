/*-------------------------------------------------------------------------
 *
 * pg_tde_tdemap.h
 *	  TDE relation fork manapulation.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_MAP_H
#define PG_TDE_MAP_H

#include "utils/rel.h"
#include "storage/relfilelocator.h"
#include "access/xlog_internal.h"

#define TDE_FORK_EXT "tde"

#define INTERNAL_KEY_LEN 16
typedef struct InternalKey
{
    uint8   key[INTERNAL_KEY_LEN];
    /* a start and end range of the key
     * (start_loc == 0 && end_loc == 0) -> the key is for the whole file
     */
    Size    start_loc; 
    Size    end_loc;
	void*   ctx; // TODO: shouldn't be here / written to the disk
} InternalKey;

#define MASTER_KEY_NAME_LEN 256
typedef struct RelKeysData
{
    char        master_key_name[MASTER_KEY_NAME_LEN];
    Size        internal_keys_len;
    InternalKey internal_key[FLEXIBLE_ARRAY_MEMBER];
} RelKeysData;

#define SizeOfRelKeysDataHeader offsetof(RelKeysData, internal_key)
#define SizeOfRelKeysData(keys_num) \
    (SizeOfRelKeysDataHeader + sizeof(InternalKey) * keys_num)

/* Relation keys cache.
 * 
 * TODO: For now it is just a linked list. Data can only be added w/o any
 * ability to remove or change it. Also consider usage of more efficient data
 * struct (hash map) in the shared memory(?) - currently allocated in the
 * TopMemoryContext of the process. 
 */
typedef struct RelKeys
{
    Oid     rel_id;
    RelKeysData    *keys;
    struct RelKeys *next;
} RelKeys;

extern void pg_tde_delete_key_fork(Relation rel);
extern void pg_tde_create_key_fork(const RelFileLocator *newrlocator, Relation rel);
extern RelKeysData *pg_tde_get_keys_from_fork(const RelFileLocator *rlocator);
extern RelKeysData *GetRelationKeys(RelFileLocator rel);
const char * tde_sprint_key(InternalKey *k);

/* XLOG stuff */
#define XLOG_TDE_CREATE_FORK 0x00
#define RM_TDERMGRS_ID          RM_EXPERIMENTAL_ID
#define RM_TDERMGRS_NAME        "test_pg_tde_custom_rmgrs"

void            pg_tde_rmgrs_redo(XLogReaderState *record);
void            pg_tde_rmgrs_desc(StringInfo buf, XLogReaderState *record);
const char *    pg_tde_rmgrs_identify(uint8 info);

static const RmgrData  pg_tde_rmgr = {
	.rm_name = RM_TDERMGRS_NAME,
	.rm_redo = pg_tde_rmgrs_redo,
	.rm_desc = pg_tde_rmgrs_desc,
	.rm_identify = pg_tde_rmgrs_identify
};

#endif                            /* PG_TDE_MAP_H */
