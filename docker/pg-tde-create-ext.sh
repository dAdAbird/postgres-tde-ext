#!/bin/bash

set -e

PG_PRIMARY=${PG_PRIMARY:-"false"}
REPL_PASS=${REPL_PASS:-"replpass"}

psql -c 'CREATE EXTENSION pg_tde;'
psql -d template1 -c 'CREATE EXTENSION pg_tde;'

if [ $PG_PRIMARY == "true" ] ; then
    psql -c "CREATE ROLE repl WITH REPLICATION PASSWORD '${REPL_PASS}' LOGIN;"
    echo "host replication repl 0.0.0.0/0 trust" >> ${PGDATA}/pg_hba.conf
else
    sleep 5
    rm -rf  ${PGDATA}/*
    pg_basebackup -h pg-primary -p 5432 -U repl -D ${PGDATA} -Fp -Xs -R
fi