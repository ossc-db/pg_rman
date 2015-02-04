#!/bin/bash

#============================================================================
# Common setup rules for all tests
#============================================================================

TEST_NAME=$1

# Unset environment variables usable by both Postgres and pg_rman
unset PGUSER
unset PGPORT
unset PGDATABASE
unset COMPRESS_DATA
unset BACKUP_MODE
unset ARCLOG_PATH
unset BACKUP_PATH
unset SRVLOG_PATH
unset WITH_SERVLOG
unset SMOOTH_CHECKPOINT
unset KEEP_DATA_GENERATIONS
unset KEEP_DATA_DAYS
unset KEEP_ARCLOG_FILES
unset KEEP_ARCLOG_DAYS
unset KEEP_SRVLOG_FILES
unset KEEP_SRVLOG_DAYS
unset RECOVERY_TARGET_TIME
unset RECOVERY_TARGET_XID
unset RECOVERY_TARGET_INCLUSIVE
unset RECOVERY_TARGET_TIMELINE
unset HARD_COPY

# Data locations
BASE_PATH=`pwd`
TEST_BASE=${BASE_PATH}/results/${TEST_NAME}
PGDATA_PATH=${TEST_BASE}/data
BACKUP_PATH=${TEST_BASE}/backup
ARCLOG_PATH=${TEST_BASE}/arclog
SRVLOG_PATH=${TEST_BASE}/srvlog
TBLSPC_PATH=${TEST_BASE}/tblspc
TEST_PGPORT=54321
export PGDATA=${PGDATA_PATH}
