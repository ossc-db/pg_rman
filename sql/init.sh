#!/bin/bash

#============================================================================
# This is a test script for init command of pg_rman.
#============================================================================

BASE_PATH=`pwd`
TEST_BASE=${BASE_PATH}/results/init
PGDATA_PATH=${TEST_BASE}/data
BACKUP_PATH=${TEST_BASE}/backup
ARCLOG_PATH=${TEST_BASE}/arclog
SRVLOG_PATH=${TEST_BASE}/srvlog

## setup environment
export PGDATA=${PGDATA_PATH}
unset PGUSER
unset PGPORT
unset PGDATABASE
unset COMPRESS_DATA
unset BACKUP_MODE
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

## clean and create database cluster
pg_ctl stop -m immediate > /dev/null 2>&1
rm -fr ${PGDATA}
rm -fr ${BACKUP_PATH}
rm -fr ${ARCLOG_PATH} && mkdir -p ${ARCLOG_PATH}
rm -fr ${SRVLOG_PATH} && mkdir -p ${SRVLOG_PATH}

initdb --no-locale > /dev/null 2>&1
cp ${PGDATA}/postgresql.conf ${PGDATA}/postgresql.conf_org
cat << EOF >> ${PGDATA}/postgresql.conf
wal_level = hot_standby
archive_mode = on
archive_command = 'cp "%p" "${ARCLOG_PATH}/%f"'
EOF

echo '###### INIT COMMAND TEST-0001 ######'
echo '###### success with archive_command ######'
pg_rman -B ${BACKUP_PATH} init --quiet;echo $?
find results/init/backup | xargs ls -Fd | sort

echo '###### INIT COMMAND TEST-0002 ######'
echo '###### success with archive_command and log_directory ######'
rm -rf ${BACKUP_PATH}
cp ${PGDATA_PATH}/postgresql.conf_org ${PGDATA_PATH}/postgresql.conf
cat << EOF >> ${PGDATA}/postgresql.conf
wal_level = hot_standby
archive_mode = on
archive_command = 'cp "%p" "${ARCLOG_PATH}/%f"'
log_directory = '${SRVLOG_PATH}'
EOF
pg_rman -B ${BACKUP_PATH} init --quiet;echo $?
find results/init/backup | xargs ls -Fd | sort

echo '###### INIT COMMAND TEST-0003 ######'
echo '###### success without archive_command ######'
rm -rf ${BACKUP_PATH}
cp ${PGDATA_PATH}/postgresql.conf_org ${PGDATA_PATH}/postgresql.conf
cat << EOF >> ${PGDATA}/postgresql.conf
wal_level = hot_standby
archive_mode = on
log_directory = '${SRVLOG_PATH}'
EOF
pg_rman -B ${BACKUP_PATH} init --quiet;echo $?
find results/init/backup | xargs ls -Fd | sort

echo '###### INIT COMMAND TEST-0004 ######'
echo '###### failure with backup catalog already existed ######'
pg_rman -B ${BACKUP_PATH} init;echo $?
echo ''

echo '###### INIT COMMAND TEST-0005 ######'
echo '###### failure with backup catalog shoud be given as absolute path ######'
rm -rf ${BACKUP_PATH}
pg_rman --backup-path=resuts/init/backup init;echo $?
echo ''


## clean up the temporal test data
pg_ctl stop -m immediate > /dev/null 2>&1
rm -fr ${PGDATA}
rm -fr ${BACKUP_PATH}
rm -fr ${ARCLOG_PATH} 
rm -fr ${SRVLOG_PATH} 
