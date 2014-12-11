#!/bin/bash

#============================================================================
# This is a test script for backup command of pg_rman.
#============================================================================

BASE_PATH=`pwd`
TEST_BASE=${BASE_PATH}/results/backup
PGDATA_PATH=${TEST_BASE}/data
BACKUP_PATH=${TEST_BASE}/backup
ARCLOG_PATH=${TEST_BASE}/arclog
SRVLOG_PATH=${TEST_BASE}/srvlog
TBLSPC_PATH=${TEST_BASE}/tblspc
TEST_PGPORT=54321
SCALE=1
DURATION=10
USE_DATA_CHECKSUM=""

# Clear environment variables used by pg_rman except $PGDATA.
# List of environment variables is defined in catalog.c.
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
unset HARD_COPY


## command line option handling for this script 
while [ $# -gt 0 ]; do
	case $1 in
		"-d")
			DURATION=`expr $2 + 0`
			if [ $? -ne 0 ]; then
				echo "invalid duration"
				exit 1
			fi
			shift 2
			;;
		"-s")
			SCALE=`expr $2 + 0`
			if [ $? -ne 0 ]; then
				echo "invalid scale"
				exit 1
			fi
			shift 2
			;;
		"--with-checksum")
			USE_DATA_CHECKSUM="--data-checksum"
			shift
			;;
		*)
			shift
			;;
	esac
done

# Check presence of pgbench command and initialize environment
which pgbench > /dev/null 2>&1
ERR_NUM=$?
if [ $ERR_NUM != 0 ]
then
    echo "pgbench is not installed in this environment."
    echo "It is needed in PATH for those regression tests."
    exit 1
fi

function cleanup()
{
    # cleanup environment
    pg_ctl stop -m immediate > /dev/null 2>&1
    rm -fr ${PGDATA_PATH}
    rm -fr ${BACKUP_PATH}
    rm -fr ${ARCLOG_PATH}
    rm -fr ${SRVLOG_PATH}
    rm -fr ${TBLSPC_PATH}
    mkdir -p ${ARCLOG_PATH}
    mkdir -p ${SRVLOG_PATH}
    mkdir -p ${TBLSPC_PATH}
}

function init_backup()
{
    # cleanup environment
    cleanup

    # create new database cluster
    initdb ${USE_DATA_CHECKSUM} --no-locale -D ${PGDATA_PATH} > ${TEST_BASE}/initdb.log 2>&1
    cp ${PGDATA_PATH}/postgresql.conf ${PGDATA_PATH}/postgresql.conf_org
    cat << EOF >> ${PGDATA_PATH}/postgresql.conf
port = ${TEST_PGPORT}
logging_collector = on
wal_level = hot_standby
log_directory = '${SRVLOG_PATH}'
log_filename = 'postgresql-%F_%H%M%S.log'
archive_mode = on
archive_command = 'cp %p ${ARCLOG_PATH}/%f'
checkpoint_segments = 10
EOF

    # start PostgreSQL
    pg_ctl start -D ${PGDATA_PATH} -w -t 300 > /dev/null 2>&1
	mkdir -p ${TBLSPC_PATH}/pgbench	
	psql --no-psqlrc -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1 << EOF
CREATE TABLESPACE pgbench LOCATION '${TBLSPC_PATH}/pgbench';
CREATE DATABASE pgbench TABLESPACE = pgbench;
EOF

    pgbench -i -s ${SCALE} -p ${TEST_PGPORT} -d pgbench > ${TEST_BASE}/pgbench.log 2>&1

    # init backup catalog
    init_catalog
}

function init_catalog()
{
    rm -fr ${BACKUP_PATH}
    pg_rman init -B ${BACKUP_PATH} --quiet
}

init_backup

echo '###### BACKUP COMMAND TEST-0001 ######'
echo '###### full backup mode ######'
pg_rman backup -B ${BACKUP_PATH} -b full -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0001.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0001.log
grep OK ${TEST_BASE}/TEST-0001.log | sed -e 's@[^-]@@g' | wc -c


echo '###### BACKUP COMMAND TEST-0002 ######'
echo '###### incremental backup mode ######'
pg_rman backup -B ${BACKUP_PATH} -b incremental -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0002.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0002.log
grep OK ${TEST_BASE}/TEST-0002.log | sed -e 's@[^-]@@g' | wc -c

echo '###### BACKUP COMMAND TEST-0003 ######'
echo '###### archive backup mode ######'
pg_rman backup -B ${BACKUP_PATH} -b archive -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0003.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0003.log
grep OK ${TEST_BASE}/TEST-0003.log | sed -e 's@[^-]@@g' | wc -c

echo '###### BACKUP COMMAND TEST-0004 ######'
echo '###### full backup with server log ######'
init_catalog
pg_rman backup -B ${BACKUP_PATH} -b full -s -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0004.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0004.log
grep OK ${TEST_BASE}/TEST-0004.log | sed -e 's@[^-]@@g' | wc -c

echo '###### BACKUP COMMAND TEST-0005 ######'
echo '###### full backup with compression ######'
init_catalog
pg_rman backup -B ${BACKUP_PATH} -b full -s -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0005.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0005.log
grep OK ${TEST_BASE}/TEST-0005.log | sed -e 's@[^-]@@g' | wc -c

echo '###### BACKUP COMMAND TEST-0006 ######'
echo '###### full backup with smooth checkpoint ######'
init_catalog
pg_rman backup -B ${BACKUP_PATH} -b full -s -C -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0006.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0006.log
grep OK ${TEST_BASE}/TEST-0006.log | sed -e 's@[^-]@@g' | wc -c

echo '###### BACKUP COMMAND TEST-0007 ######'
echo '###### full backup with keep-data-generations and keep-data-days ######'
init_catalog
pg_rman backup -B ${BACKUP_PATH} -b full -s -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman backup -B ${BACKUP_PATH} -b full -s -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman backup -B ${BACKUP_PATH} -b full -s -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0007-before.log 2>&1
NUM_OF_FULL_BACKUPS_BEFORE=`grep OK ${TEST_BASE}/TEST-0007-before.log | wc -l`
if [ ${NUM_OF_FULL_BACKUPS_BEFORE} -gt 2 ] ; then
	echo "The number of existing full backups validated is greater than 2."
	echo "OK. Let's try to test --keep-data-generations=1."
else
	echo "The number of existing full backups validated is not greater than 2."
	echo "NG. There was something wrong in preparation of this test."
fi
# The actual value of NUM_OF_FULL_BACKUPS_BEFORE can vary on env, so commented out as default.
#echo "Number of existing full backups validated: ${NUM_OF_FULL_BACKUPS_BEFORE}"
grep OK ${TEST_BASE}/TEST-0007-before.log | sed -e 's@[^-]@@g' | wc -c
pg_rman backup -B ${BACKUP_PATH} -b full -s --keep-data-days=-1 --keep-data-generations=1 -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0007-after.log 2>&1
NUM_OF_FULL_BACKUPS_AFTER=`grep OK ${TEST_BASE}/TEST-0007-after.log | wc -l`
echo "Number of remaining full backups validated: ${NUM_OF_FULL_BACKUPS_AFTER}"
grep OK ${TEST_BASE}/TEST-0007-after.log | sed -e 's@[^-]@@g' | wc -c

echo '###### BACKUP COMMAND TEST-0008 ######'
echo '###### full backup with keep-arclog-files and keep-arclog-days ######'
init_catalog
NUM_OF_ARCLOG_FILES_BEFORE=`ls ${ARCLOG_PATH} | grep -v backup | wc -l`
if [ ${NUM_OF_ARCLOG_FILES_BEFORE} -gt 2 ] ; then
	echo "The number of existing archive log files already backuped is greater than 2."
	echo "OK. Let's try to test --keep-arclog-files=2."
else
	echo "The number of existing archive log files already backuped is not greater than 2."
	echo "NG. There was something wrong in preparation of this test."
fi
# The actual value of NUM_OF_ARCLOG_FILES_BEFORE can vary on env, so commented out as default.
# echo "Number of existing archivelog files: ${NUM_OF_ARCLOG_FILES_BEFORE}"
pg_rman backup -B ${BACKUP_PATH} -b full -s --keep-arclog-days=-2 --keep-arclog-files=2 -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0008.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0008.log
grep OK ${TEST_BASE}/TEST-0008.log | sed -e 's@[^-]@@g' | wc -c
NUM_OF_ARCLOG_FILES_AFTER=`ls ${ARCLOG_PATH} | grep -v backup | wc -l`
echo "Number of remaining archivelog files: ${NUM_OF_ARCLOG_FILES_AFTER}"

echo '###### BACKUP COMMAND TEST-0009 ######'
echo '###### full backup with keep-srvlog-files and keep-srvlog-days ######'
init_catalog
pg_ctl restart > /dev/null 2>&1
sleep 2
pg_ctl restart > /dev/null 2>&1
sleep 2
pg_ctl restart > /dev/null 2>&1
sleep 2
NUM_OF_SRVLOG_FILES_BEFORE=`ls ${SRVLOG_PATH} | wc -l`
if [ ${NUM_OF_SRVLOG_FILES_BEFORE} -gt 1 ] ; then
	echo "The number of existing server log files already backuped is greater than 1."
	echo "OK. Let's try to test --keep-srvlog-files=1."
else
	echo "The number of existing server log files already backuped is not greater than 1."
	echo "NG. There was something wrong in preparation of this test."
fi
# The actual value of NUM_OF_SRVLOG_FILES_BEFORE can vary on env, so commented out as default.
#echo "Number of existing server log files: ${NUM_OF_SRVLOG_FILES_BEFORE}"
pg_rman backup -B ${BACKUP_PATH} -b full -s --keep-srvlog-days=-1 --keep-srvlog-files=1 -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0009.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0009.log
grep OK ${TEST_BASE}/TEST-0009.log | sed -e 's@[^-]@@g' | wc -c
NUM_OF_SRVLOG_FILES_AFTER=`ls ${SRVLOG_PATH} | wc -l`
echo "Number of remaining serverlog files: ${NUM_OF_SRVLOG_FILES_AFTER}"

## cleanup
# clean up the temporal test data
pg_ctl stop -m immediate > /dev/null 2>&1
rm -fr ${PGDATA_PATH}
rm -fr ${BACKUP_PATH}
rm -fr ${ARCLOG_PATH}
rm -fr ${SRVLOG_PATH}
rm -fr ${TBLSPC_PATH}
