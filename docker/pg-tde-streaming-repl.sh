#!/bin/bash

set -e

PG_PRIMARY=${PG_PRIMARY:-"false"}
PG_REPLICATION=${PG_REPLICATION:-"false"}
REPL_PASS=${REPL_PASS:-"replpass"}

if [ !PG_REPLICATION = "true "] ; then
    exit 0
fi

if [ $PG_PRIMARY == "true" ] ; then
    psql -c "CREATE ROLE repl WITH REPLICATION PASSWORD '${REPL_PASS}' LOGIN;"
    echo "host replication repl 0.0.0.0/0 trust" >> ${PGDATA}/pg_hba.conf
else
    sleep 5
    rm -rf  ${PGDATA}/*
    pg_basebackup -h pg-primary -p 5432 -U repl -D ${PGDATA} -Fp -Xs -R
fi

# --- primary
# CREATE TABLE t1(a int, b text, PRIMARY KEY(a)) using pg_tde;
# CREATE PUBLICATION pub1 FOR TABLE t1;
# SELECT pg_create_logical_replication_slot('pub1_slot', 'tdeoutput');
# 
# --- standby
# CREATE SUBSCRIPTION sub1
# CONNECTION 'host=localhost dbname=vagrant application_name=sub1'
# PUBLICATION pub1
# WITH (create_slot=false, slot_name=pub1_slot);
# 
# ---  
# INSERT INTO t1 VALUES (1, 'one');