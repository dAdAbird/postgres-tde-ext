
// #include "pg_tde_fe.h"
// #include "fmgr.h"
#include "keyring/keyring.h"
#include "keyring/keyring_config.h"
#include "keyring/keyring_api.h"

void keyringInitialize(void)
{
	keyringRegisterVariables();
	keyringInitializeCache();
}

// #ifdef FRONTEND

// /*
//  * These are for invocation of a specifically named function with a
//  * directly-computed parameter list.  Note that neither arguments nor result
//  * are allowed to be NULL.  Also, the function cannot be one that needs to
//  * look at FmgrInfo, since there won't be any.
//  * 
//  * Copy of funcs in PG src/backend/utils/fmgr/fmgr.c
//  */
// Datum
// DirectFunctionCall1Coll(PGFunction func, Oid collation, Datum arg1)
// {
// 	LOCAL_FCINFO(fcinfo, 1);
// 	Datum		result;

// 	InitFunctionCallInfoData(*fcinfo, NULL, 1, collation, NULL, NULL);

// 	fcinfo->args[0].value = arg1;
// 	fcinfo->args[0].isnull = false;

// 	result = (*func) (fcinfo);

// 	/* Check for null result, since caller is clearly not expecting one */
// 	if (fcinfo->isnull)
// 		elog(ERROR, "function %p returned NULL", (void *) func);

// 	return result;
// }

// Datum
// DirectFunctionCall2Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2)
// {
// 	LOCAL_FCINFO(fcinfo, 2);
// 	Datum		result;

// 	InitFunctionCallInfoData(*fcinfo, NULL, 2, collation, NULL, NULL);

// 	fcinfo->args[0].value = arg1;
// 	fcinfo->args[0].isnull = false;
// 	fcinfo->args[1].value = arg2;
// 	fcinfo->args[1].isnull = false;

// 	result = (*func) (fcinfo);

// 	/* Check for null result, since caller is clearly not expecting one */
// 	if (fcinfo->isnull)
// 		elog(ERROR, "function %p returned NULL", (void *) func);

// 	return result;
// }


// #endif /* FRONTEND */