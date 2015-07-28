#!/bin/bash

#============================================================================
# This is a test script for backup command of pg_rman.
#============================================================================

# Load common rules
. sql/common.sh backup

# Extra parameters exclusive to this test
SCALE=1
DURATION=10
USE_DATA_CHECKSUM=""

# command line option handling for this script
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
max_wal_size = 512MB
EOF

    # start PostgreSQL
    pg_ctl start -D ${PGDATA_PATH} -w -t 300 > ${TEST_BASE}/pg_ctl.log 2>&1
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
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0001.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0001.log
grep OK ${TEST_BASE}/TEST-0001.log | sed -e 's@[^-]@@g' | wc -c


echo '###### BACKUP COMMAND TEST-0002 ######'
echo '###### incremental backup mode ######'
pg_rman backup -B ${BACKUP_PATH} -b incremental -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0002.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0002.log
grep OK ${TEST_BASE}/TEST-0002.log | sed -e 's@[^-]@@g' | wc -c

echo '###### BACKUP COMMAND TEST-0003 ######'
echo '###### archive backup mode ######'
pg_rman backup -B ${BACKUP_PATH} -b archive -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0003.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0003.log
grep OK ${TEST_BASE}/TEST-0003.log | sed -e 's@[^-]@@g' | wc -c

echo '###### BACKUP COMMAND TEST-0004 ######'
echo '###### full backup with server log ######'
init_catalog
pg_rman backup -B ${BACKUP_PATH} -b full -s -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0004.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0004.log
grep OK ${TEST_BASE}/TEST-0004.log | sed -e 's@[^-]@@g' | wc -c

echo '###### BACKUP COMMAND TEST-0005 ######'
echo '###### full backup with compression ######'
init_catalog
pg_rman backup -B ${BACKUP_PATH} -b full -s -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0005.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0005.log
grep OK ${TEST_BASE}/TEST-0005.log | grep -c true
grep OK ${TEST_BASE}/TEST-0005.log | sed -e 's@[^-]@@g' | wc -c

echo '###### BACKUP COMMAND TEST-0006 ######'
echo '###### full backup with smooth checkpoint ######'
init_catalog
pg_rman backup -B ${BACKUP_PATH} -b full -s -C -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0006.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0006.log
grep OK ${TEST_BASE}/TEST-0006.log | sed -e 's@[^-]@@g' | wc -c

#echo '###### BACKUP COMMAND TEST-0007 ######'
#echo '###### full backup with keep-data-generations and keep-data-days ######'
# test not here, but backup_management test sets
#init_catalog
#pg_rman backup -B ${BACKUP_PATH} -b full -s -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
#pg_rman backup -B ${BACKUP_PATH} -b full -s -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
#pg_rman backup -B ${BACKUP_PATH} -b full -s -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
#pg_rman validate -B ${BACKUP_PATH} --quiet
#pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0007-before.log 2>&1
#NUM_OF_FULL_BACKUPS_BEFORE=`grep OK ${TEST_BASE}/TEST-0007-before.log | grep FULL | wc -l`
#if [ ${NUM_OF_FULL_BACKUPS_BEFORE} -gt 2 ] ; then
#	echo "The number of existing full backups validated is greater than 2."
#	echo "OK. Let's try to test --keep-data-generations=1."
#else
#	echo "The number of existing full backups validated is not greater than 2."
#	echo "NG. There was something wrong in preparation of this test."
#fi
## The actual value of NUM_OF_FULL_BACKUPS_BEFORE can vary on env, so commented out as default.
##echo "Number of existing full backups validated: ${NUM_OF_FULL_BACKUPS_BEFORE}"
#grep OK ${TEST_BASE}/TEST-0007-before.log | sed -e 's@[^-]@@g' | wc -c
#pg_rman backup -B ${BACKUP_PATH} -b full -s --keep-data-days=-1 --keep-data-generations=1 -p ${TEST_PGPORT} -d postgres --quiet;echo $?
#pg_rman validate -B ${BACKUP_PATH} --quiet
#pg_rman show detail --show-all -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0007-after.log 2>&1
#NUM_OF_FULL_BACKUPS_AFTER=`grep OK ${TEST_BASE}/TEST-0007-after.log | grep FULL | wc -l`
#echo "Number of remaining full backups validated: ${NUM_OF_FULL_BACKUPS_AFTER}"
#NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0007-after.log | wc -l`
#echo "Number of deleted backups : ${NUM_OF_DELETED_BACKUPS}"
#grep OK ${TEST_BASE}/TEST-0007-after.log | sed -e 's@[^-]@@g' | wc -c

#echo '###### BACKUP COMMAND TEST-0008 ######'
#echo '###### full backup with keep-arclog-files and keep-arclog-days ######'
# test not here, but arc_srv_log_management test sets
#init_catalog
#NUM_OF_ARCLOG_FILES_BEFORE=`ls ${ARCLOG_PATH} | grep -v backup | wc -l`
#if [ ${NUM_OF_ARCLOG_FILES_BEFORE} -gt 2 ] ; then
#	echo "The number of existing archive log files already backuped is greater than 2."
#	echo "OK. Let's try to test --keep-arclog-files=2."
#else
#	echo "The number of existing archive log files already backuped is not greater than 2."
#	echo "NG. There was something wrong in preparation of this test."
#fi
## The actual value of NUM_OF_ARCLOG_FILES_BEFORE can vary on env, so commented out as default.
## echo "Number of existing archivelog files: ${NUM_OF_ARCLOG_FILES_BEFORE}"
#pg_rman backup -B ${BACKUP_PATH} -b full -s --keep-arclog-days=-2 --keep-arclog-files=2 -p ${TEST_PGPORT} -d postgres --quiet;echo $?
#pg_rman validate -B ${BACKUP_PATH} --quiet
#pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0008.log 2>&1
#grep -c OK ${TEST_BASE}/TEST-0008.log
#grep OK ${TEST_BASE}/TEST-0008.log | sed -e 's@[^-]@@g' | wc -c
#NUM_OF_ARCLOG_FILES_AFTER=`ls ${ARCLOG_PATH} | grep -v backup | wc -l`
#echo "Number of remaining archivelog files: ${NUM_OF_ARCLOG_FILES_AFTER}"
#
#echo '###### BACKUP COMMAND TEST-0009 ######'
#echo '###### full backup with keep-srvlog-files and keep-srvlog-days ######'
# test not here, but arc_srv_log_management test sets
#init_catalog
#pg_ctl restart > /dev/null 2>&1
#sleep 2
#pg_ctl restart > /dev/null 2>&1
#sleep 2
#pg_ctl restart > /dev/null 2>&1
#sleep 2
#NUM_OF_SRVLOG_FILES_BEFORE=`ls ${SRVLOG_PATH} | wc -l`
#if [ ${NUM_OF_SRVLOG_FILES_BEFORE} -gt 1 ] ; then
#	echo "The number of existing server log files already backuped is greater than 1."
#	echo "OK. Let's try to test --keep-srvlog-files=1."
#else
#	echo "The number of existing server log files already backuped is not greater than 1."
#	echo "NG. There was something wrong in preparation of this test."
#fi
## The actual value of NUM_OF_SRVLOG_FILES_BEFORE can vary on env, so commented out as default.
##echo "Number of existing server log files: ${NUM_OF_SRVLOG_FILES_BEFORE}"
#pg_rman backup -B ${BACKUP_PATH} -b full -s --keep-srvlog-days=-1 --keep-srvlog-files=1 -p ${TEST_PGPORT} -d postgres --quiet;echo $?
#pg_rman validate -B ${BACKUP_PATH} --quiet
#pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0009.log 2>&1
#grep -c OK ${TEST_BASE}/TEST-0009.log
#grep OK ${TEST_BASE}/TEST-0009.log | sed -e 's@[^-]@@g' | wc -c
#NUM_OF_SRVLOG_FILES_AFTER=`ls ${SRVLOG_PATH} | wc -l`
#echo "Number of remaining serverlog files: ${NUM_OF_SRVLOG_FILES_AFTER}"

echo '###### BACKUP COMMAND TEST-0010 ######'
echo '###### switch backup mode from incremental to full ######'
init_catalog
echo 'incremental backup without validated full backup'
pg_rman backup -B ${BACKUP_PATH} -b incremental -s -Z -p ${TEST_PGPORT} -d postgres;echo $?
echo 'incremental backup in the same situation but with --full-backup-on-error option'
pg_rman backup -B ${BACKUP_PATH} -b incremental -F -s -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0010.log 2>&1
grep OK ${TEST_BASE}/TEST-0010.log | grep FULL | wc -l
grep ERROR ${TEST_BASE}/TEST-0010.log | grep INCR | wc -l

echo '###### BACKUP COMMAND TEST-0011 ######'
echo '###### switch backup mode from archive to full ######'
init_catalog
echo 'archive backup without validated full backup'
pg_rman backup -B ${BACKUP_PATH} -b archive -s -Z -p ${TEST_PGPORT} -d postgres;echo $?
sleep 1
echo 'archive backup in the same situation but with --full-backup-on-error option'
pg_rman backup -B ${BACKUP_PATH} -b archive -F -s -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0011.log 2>&1
grep OK ${TEST_BASE}/TEST-0011.log | grep FULL | wc -l
grep ERROR ${TEST_BASE}/TEST-0011.log | grep ARCH | wc -l

# cleanup
## clean up the temporal test data
pg_ctl stop -m immediate > /dev/null 2>&1
rm -fr ${PGDATA_PATH}
rm -fr ${BACKUP_PATH}
rm -fr ${ARCLOG_PATH}
rm -fr ${SRVLOG_PATH}
rm -fr ${TBLSPC_PATH}
