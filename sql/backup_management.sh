#!/bin/bash

#============================================================================
# This is a test script for backup management of pg_rman.
#============================================================================

# Load common rules
. sql/common.sh backup_management

# Extra parameters exclusive to this test
SCALE=1
DURATION=10
USE_DATA_CHECKSUM=""

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

function create_dummy_backup()
{

	YEAR_STRING=`date +"%Y" -d "$1 days ago"`
	MONTH_STRING=`date +"%m" -d "$1 days ago"`
	DAY_STRING=`date +"%d" -d "$1 days ago"`
		
	DUMMY_PATH=${BACKUP_PATH}/${YEAR_STRING}${MONTH_STRING}${DAY_STRING}/101010
	mkdir -p ${DUMMY_PATH}
    cat << EOF > ${DUMMY_PATH}/backup.ini
# configuration
BACKUP_MODE=$2
FULL_BACKUP_ON_ERROR=false
WITH_SERVERLOG=true
COMPRESS_DATA=true
# result
TIMELINEID=99
START_LSN=1/03000028
STOP_LSN=1/030000f8
START_TIME='${YEAR_STRING}-${MONTH_STRING}-${DAY_STRING} 10:10:10'
END_TIME='${YEAR_STRING}-${MONTH_STRING}-${DAY_STRING} 10:11:10'
RECOVERY_XID=3645
RECOVERY_TIME='${YEAR_STRING}-${MONTH_STRING}-${DAY_STRING} 10:11:08'
TOTAL_DATA_BYTES=9999999
READ_DATA_BYTES=9999999
READ_ARCLOG_BYTES=9999999
WRITE_BYTES=9999999
BLOCK_SIZE=8192
XLOG_BLOCK_SIZE=8192
STATUS=$3
EOF
}

init_backup

echo '###### BACKUP MANAGEMENT TEST-0001 ######'
echo '###### keep generations for full backup ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0001-before.log 2>&1
grep OK ${TEST_BASE}/TEST-0001-before.log | sed -e 's@[^-]@@g' | wc -c
pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=1 -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
#FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=1 -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail --show-all -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0001-after.log 2>&1
NUM_OF_FULL_BACKUPS_AFTER=`grep OK ${TEST_BASE}/TEST-0001-after.log | grep FULL | wc -l`
echo "Number of remaining full backups validated: ${NUM_OF_FULL_BACKUPS_AFTER}"
NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0001-after.log | wc -l`
echo "Number of deleted backups : ${NUM_OF_DELETED_BACKUPS}"
grep OK ${TEST_BASE}/TEST-0001-after.log | sed -e 's@[^-]@@g' | wc -c

init_catalog

echo '###### BACKUP MANAGEMENT TEST-0002 ######'
echo '###### keep generations for full and incremental backup ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman backup -B ${BACKUP_PATH} -b inc -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0002-before.log 2>&1
grep OK ${TEST_BASE}/TEST-0002-before.log | sed -e 's@[^-]@@g' | wc -c
pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=1 -Z -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1;echo $?
# FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=1 -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail --show-all -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0002-after.log 2>&1
NUM_OF_FULL_BACKUPS_AFTER=`grep OK ${TEST_BASE}/TEST-0002-after.log | grep FULL | wc -l`
echo "Number of remaining full backups validated: ${NUM_OF_FULL_BACKUPS_AFTER}"
NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0002-after.log | wc -l`
echo "Number of deleted backups : ${NUM_OF_DELETED_BACKUPS}"
grep OK ${TEST_BASE}/TEST-0002-after.log | sed -e 's@[^-]@@g' | wc -c

init_catalog

echo '###### BACKUP MANAGEMENT TEST-0003 ######'
echo '###### keep generations for full and archive backup ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman backup -B ${BACKUP_PATH} -b arc -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0003-before.log 2>&1
grep OK ${TEST_BASE}/TEST-0003-before.log | sed -e 's@[^-]@@g' | wc -c
pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=1 -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
# FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=1 -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail --show-all -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0003-after.log 2>&1
NUM_OF_FULL_BACKUPS_AFTER=`grep OK ${TEST_BASE}/TEST-0003-after.log | grep FULL | wc -l`
echo "Number of remaining full backups validated: ${NUM_OF_FULL_BACKUPS_AFTER}"
NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0003-after.log | wc -l`
echo "Number of deleted backups : ${NUM_OF_DELETED_BACKUPS}"
grep OK ${TEST_BASE}/TEST-0003-after.log | sed -e 's@[^-]@@g' | wc -c

init_catalog

echo '###### BACKUP MANAGEMENT TEST-0004 ######'
echo '###### keep generations for full and error backup ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
rm -f `find ${BACKUP_PATH} -name postgresql.conf`
pg_rman validate -B ${BACKUP_PATH} --quiet > /dev/null 2>&1;echo $?
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0004-before.log 2>&1
grep OK ${TEST_BASE}/TEST-0004-before.log | sed -e 's@[^-]@@g' | wc -c
pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=1 -Z -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1;echo $?
# FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=1 -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail --show-all -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0004-after.log 2>&1
NUM_OF_FULL_BACKUPS_AFTER=`grep OK ${TEST_BASE}/TEST-0004-after.log | grep FULL | wc -l`
echo "Number of remaining full backups validated: ${NUM_OF_FULL_BACKUPS_AFTER}"
NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0004-after.log | wc -l`
echo "Number of deleted backups : ${NUM_OF_DELETED_BACKUPS}"
grep OK ${TEST_BASE}/TEST-0004-after.log | sed -e 's@[^-]@@g' | wc -c

init_catalog

echo '###### BACKUP MANAGEMENT TEST-0005 ######'
echo '###### keep days for full backup ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
create_dummy_backup 1 FULL OK
create_dummy_backup 2 FULL OK
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0005-before.log 2>&1
grep OK ${TEST_BASE}/TEST-0005-before.log | sed -e 's@[^-]@@g' | wc -c
pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-days=1 -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
# FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-days=1 -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail --show-all -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0005-after.log 2>&1
NUM_OF_FULL_BACKUPS_AFTER=`grep OK ${TEST_BASE}/TEST-0005-after.log | grep FULL | wc -l`
echo "Number of remaining full backups validated: ${NUM_OF_FULL_BACKUPS_AFTER}"
NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0005-after.log | wc -l`
echo "Number of deleted backups : ${NUM_OF_DELETED_BACKUPS}"
grep OK ${TEST_BASE}/TEST-0005-after.log | sed -e 's@[^-]@@g' | wc -c

init_catalog

echo '###### BACKUP MANAGEMENT TEST-0006 ######'
echo '###### keep days for full and incremental backup ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
create_dummy_backup 1 INCREMENTAL OK
create_dummy_backup 2 FULL OK
create_dummy_backup 3 FULL OK
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0006-before.log 2>&1
grep OK ${TEST_BASE}/TEST-0006-before.log | sed -e 's@[^-]@@g' | wc -c
pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-days=1 -Z -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1;echo $?
# FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-days=1 -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail --show-all -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0006-after.log 2>&1
NUM_OF_FULL_BACKUPS_AFTER=`grep OK ${TEST_BASE}/TEST-0006-after.log | grep FULL | wc -l`
echo "Number of remaining full backups validated: ${NUM_OF_FULL_BACKUPS_AFTER}"
NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0006-after.log | wc -l`
echo "Number of deleted backups : ${NUM_OF_DELETED_BACKUPS}"
grep OK ${TEST_BASE}/TEST-0006-after.log | sed -e 's@[^-]@@g' | wc -c

init_catalog

echo '###### BACKUP MANAGEMENT TEST-0007 ######'
echo '###### keep days for full and archive backup ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
create_dummy_backup 1 ARCHIVE OK
create_dummy_backup 2 FULL OK
create_dummy_backup 3 FULL OK
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0007-before.log 2>&1
grep OK ${TEST_BASE}/TEST-0007-before.log | sed -e 's@[^-]@@g' | wc -c
pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-days=1 -Z -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1;echo $?
# FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-days=1 -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail --show-all -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0007-after.log 2>&1
NUM_OF_FULL_BACKUPS_AFTER=`grep OK ${TEST_BASE}/TEST-0007-after.log | grep FULL | wc -l`
echo "Number of remaining full backups validated: ${NUM_OF_FULL_BACKUPS_AFTER}"
NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0007-after.log | wc -l`
echo "Number of deleted backups : ${NUM_OF_DELETED_BACKUPS}"
grep OK ${TEST_BASE}/TEST-0007-after.log | sed -e 's@[^-]@@g' | wc -c

init_catalog

echo '###### BACKUP MANAGEMENT TEST-0008 ######'
echo '###### keep days for full and error backup ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
create_dummy_backup 1 FULL ERROR
create_dummy_backup 2 FULL OK
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0008-before.log 2>&1
grep OK ${TEST_BASE}/TEST-0008-before.log | sed -e 's@[^-]@@g' | wc -c
pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-days=1 -Z -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1;echo $?
# FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-days=1 -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail --show-all -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0008-after.log 2>&1
NUM_OF_FULL_BACKUPS_AFTER=`grep OK ${TEST_BASE}/TEST-0008-after.log | grep FULL | wc -l`
echo "Number of remaining full backups validated: ${NUM_OF_FULL_BACKUPS_AFTER}"
NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0008-after.log | wc -l`
echo "Number of deleted backups : ${NUM_OF_DELETED_BACKUPS}"
grep OK ${TEST_BASE}/TEST-0008-after.log | sed -e 's@[^-]@@g' | wc -c

init_catalog

echo '###### BACKUP MANAGEMENT TEST-0009 ######'
echo '###### keep generations and days together with full backup ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
create_dummy_backup 1 FULL OK
create_dummy_backup 2 FULL OK
create_dummy_backup 3 FULL OK
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0009-before.log 2>&1
grep OK ${TEST_BASE}/TEST-0009-before.log | sed -e 's@[^-]@@g' | wc -c
echo "keep data generation : 1, keep data days : 2"
pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=1 --keep-data-days=2 -Z -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1;echo $?
# FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=1 --keep-data-days=2 -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail --show-all -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0009-after.log 2>&1
NUM_OF_FULL_BACKUPS_AFTER=`grep OK ${TEST_BASE}/TEST-0009-after.log | grep FULL | wc -l`
echo "Number of remaining full backups validated: ${NUM_OF_FULL_BACKUPS_AFTER}"
NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0009-after.log | wc -l`
echo "Number of deleted backups : ${NUM_OF_DELETED_BACKUPS}"
grep OK ${TEST_BASE}/TEST-0009-after.log | sed -e 's@[^-]@@g' | wc -c

init_catalog

echo '###### BACKUP MANAGEMENT TEST-0010 ######'
echo '###### keep generations and days together with full backup ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
create_dummy_backup 1 FULL OK
create_dummy_backup 2 FULL OK
create_dummy_backup 3 FULL OK
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0010-before.log 2>&1
grep OK ${TEST_BASE}/TEST-0010-before.log | sed -e 's@[^-]@@g' | wc -c
echo "keep data generation : 3, keep data days : 1"
pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=3 --keep-data-days=1 -Z -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1;echo $?
# FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=3 --keep-data-days=1 -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail --show-all -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0010-after.log 2>&1
NUM_OF_FULL_BACKUPS_AFTER=`grep OK ${TEST_BASE}/TEST-0010-after.log | grep FULL | wc -l`
echo "Number of remaining full backups validated: ${NUM_OF_FULL_BACKUPS_AFTER}"
NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0010-after.log | wc -l`
echo "Number of deleted backups : ${NUM_OF_DELETED_BACKUPS}"
grep OK ${TEST_BASE}/TEST-0010-after.log | sed -e 's@[^-]@@g' | wc -c

init_catalog

echo '###### BACKUP MANAGEMENT TEST-0011 ######'
echo '###### keep generations and days together with full and incremental backup ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
create_dummy_backup 1 FULL OK
create_dummy_backup 2 INCREMENTAL OK
create_dummy_backup 3 FULL OK
create_dummy_backup 4 FULL OK
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0011-before.log 2>&1
grep OK ${TEST_BASE}/TEST-0011-before.log | sed -e 's@[^-]@@g' | wc -c
echo "keep data generation : 2, keep data days : 2"
pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=2 --keep-data-days=2 -Z -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1;echo $?
# FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=2 --keep-data-days=2 -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail --show-all -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0011-after.log 2>&1
NUM_OF_FULL_BACKUPS_AFTER=`grep OK ${TEST_BASE}/TEST-0011-after.log | grep FULL | wc -l`
echo "Number of remaining full backups validated: ${NUM_OF_FULL_BACKUPS_AFTER}"
NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0011-after.log | wc -l`
echo "Number of deleted backups : ${NUM_OF_DELETED_BACKUPS}"
grep OK ${TEST_BASE}/TEST-0011-after.log | sed -e 's@[^-]@@g' | wc -c

init_catalog

echo '###### BACKUP MANAGEMENT TEST-0012 ######'
echo '###### keep generations and days together with full and archive backup ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
create_dummy_backup 1 FULL OK
create_dummy_backup 2 ARCHIVE OK
create_dummy_backup 3 FULL OK
create_dummy_backup 4 FULL OK
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0012-before.log 2>&1
grep OK ${TEST_BASE}/TEST-0012-before.log | sed -e 's@[^-]@@g' | wc -c
echo "keep data generation : 2, keep data days : 2"
pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=2 --keep-data-days=2 -Z -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1;echo $?
# FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=2 --keep-data-days=2 -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail --show-all -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0012-after.log 2>&1
NUM_OF_FULL_BACKUPS_AFTER=`grep OK ${TEST_BASE}/TEST-0012-after.log | grep FULL | wc -l`
echo "Number of remaining full backups validated: ${NUM_OF_FULL_BACKUPS_AFTER}"
NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0012-after.log | wc -l`
echo "Number of deleted backups : ${NUM_OF_DELETED_BACKUPS}"
grep OK ${TEST_BASE}/TEST-0012-after.log | sed -e 's@[^-]@@g' | wc -c

init_catalog

echo '###### BACKUP MANAGEMENT TEST-0013 ######'
echo '###### keep generations and days together with full and error backup ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
create_dummy_backup 1 FULL OK
create_dummy_backup 2 FULL ERROR
create_dummy_backup 3 FULL OK
create_dummy_backup 4 FULL OK
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0013-before.log 2>&1
grep OK ${TEST_BASE}/TEST-0013-before.log | sed -e 's@[^-]@@g' | wc -c
echo "keep data generation : 2, keep data days : 2"
pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=2 --keep-data-days=2 -Z -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1;echo $?
# FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b full --keep-data-generations=2 --keep-data-days=2 -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail --show-all -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0013-after.log 2>&1
NUM_OF_FULL_BACKUPS_AFTER=`grep OK ${TEST_BASE}/TEST-0013-after.log | grep FULL | wc -l`
echo "Number of remaining full backups validated: ${NUM_OF_FULL_BACKUPS_AFTER}"
NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0013-after.log | wc -l`
echo "Number of deleted backups : ${NUM_OF_DELETED_BACKUPS}"
grep OK ${TEST_BASE}/TEST-0013-after.log | sed -e 's@[^-]@@g' | wc -c

# cleanup
## clean up the temporal test data
pg_ctl stop -m immediate > /dev/null 2>&1
rm -fr ${PGDATA_PATH}
rm -fr ${BACKUP_PATH}
rm -fr ${ARCLOG_PATH}
rm -fr ${SRVLOG_PATH}
rm -fr ${TBLSPC_PATH}
