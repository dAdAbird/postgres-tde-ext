/*-------------------------------------------------------------------------
 *
 * tde_global_catalog.h
 *	  Global catalog key management
 *
 * src/include/catalog/tde_global_catalog.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TDE_GLOBAL_CATALOG_H
#define TDE_GLOBAL_CATALOG_H

#include "postgres.h"


/* 
 * Needed for glogbal data (WAL etc) keys identification in caches and storage.
 * We take IDs the oid type operators, so there is no overlap with the "real"
 * catalog object possible.
 */
#define GLOBAL_DATA_TDE_OID	607 /* Global objects fake "db" */
#define XLOG_TDE_OID        608

#define GLOBAL_SPACE_RLOCATOR(_obj_oid) (RelFileLocator) {GLOBALTABLESPACE_OID, 0, _obj_oid}

extern void TDEGlCatInitGUC(void);
extern Size TDEGlCatEncStateSize();
extern void TDEGlCatShmemInit(void);

#endif /*TDE_GLOBAL_CATALOG_H*/