#!/bin/bash

set -e

psql -c 'CREATE EXTENSION pg_tde;'
psql -d template1 -c 'CREATE EXTENSION pg_tde;'
