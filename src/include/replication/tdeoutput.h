/*-------------------------------------------------------------------------
 *
 * tdeoutput.h
 *		Logical Replication output plugin
 *
 * Copyright (c) 2015-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/replication/tdeoutput.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TDEOUTPUT_H
#define TDEOUTPUT_H

#include "postgres.h"
#include "nodes/pg_list.h"

typedef struct TDEOutputData
{
	MemoryContext context;		/* private memory context for transient
								 * allocations */
	MemoryContext cachectx;		/* private memory context for cache data */

	/* client-supplied info: */
	uint32		protocol_version;
	List	   *publication_names;
	List	   *publications;
	bool		binary;
	char		streaming;
	bool		messages;
	bool		two_phase;
	char	   *origin;
} TDEOutputData;

#endif							/* TDEOUTPUT_H */
