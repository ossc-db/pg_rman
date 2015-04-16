#!/bin/bash

#============================================================================
# This is a test script for delete command of pg_rman.
#============================================================================

# Load common rules
. sql/common.sh delete

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
    initdb --no-locale -D ${PGDATA_PATH} > ${TEST_BASE}/initdb.log 2>&1
    cp $PGDATA_PATH/postgresql.conf $PGDATA_PATH/postgresql.conf_org
    cat << EOF >> $PGDATA_PATH/postgresql.conf
port = ${TEST_PGPORT}
logging_collector = on
wal_level = hot_standby
log_directory = '${SRVLOG_PATH}'
archive_mode = on
archive_command = 'cp %p ${ARCLOG_PATH}/%f'
EOF

	# start PostgreSQL
	pg_ctl start -D ${PGDATA_PATH} -w -t 300 > /dev/null 2>&1
	pgbench -i -p ${TEST_PGPORT} -d postgres > ${TEST_BASE}/pgbench.log 2>&1

	# init backup catalog
	init_catalog
}

function init_catalog()
{
	rm -fr ${BACKUP_PATH}
	pg_rman init -B ${BACKUP_PATH} --quiet
}

init_backup
echo '###### DELETE COMMAND TEST-0001 ######'
echo '###### delete full backups ######'
FIRST_BACKUP_DATE=`date +"%Y-%m-%d %H:%M:%S"`
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet
pgbench -p ${TEST_PGPORT} >> ${TEST_BASE}/pgbench.log 2>&1
SECOND_BACKUP_DATE=`date +"%Y-%m-%d %H:%M:%S"`
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet
pgbench -p ${TEST_PGPORT} >> ${TEST_BASE}/pgbench.log 2>&1
THIRD_BACKUP_DATE=`date +"%Y-%m-%d %H:%M:%S"`
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet
pg_rman validate -B ${BACKUP_PATH} --quiet

echo "try to delete the oldest backup"
pg_rman -B ${BACKUP_PATH} delete ${SECOND_BACKUP_DATE} > /dev/null 2>&1
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0001.out.1 2>&1
pg_rman show -a -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0001.out.2 2>&1
grep -c OK ${TEST_BASE}/TEST-0001.out.1
grep -c DELETED ${TEST_BASE}/TEST-0001.out.2
NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0001.out.2 | wc -l`
echo "Number of deleted backups should be 1, is it so?: ${NUM_OF_DELETED_BACKUPS}"

init_backup
echo '###### DELETE COMMAND TEST-0002 ######'
echo '###### keep backups which are necessary for recovery ######'
FIRST_BACKUP_DATE=`date +"%Y-%m-%d %H:%M:%S"`
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet
pgbench -p ${TEST_PGPORT} >> ${TEST_BASE}/pgbench.log 2>&1
SECOND_BACKUP_DATE=`date +"%Y-%m-%d %H:%M:%S"`
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet
pg_rman validate -B ${BACKUP_PATH} --quiet
pgbench -p ${TEST_PGPORT} >> ${TEST_BASE}/pgbench.log 2>&1
THIRD_BACKUP_DATE=`date +"%Y-%m-%d %H:%M:%S"`
pg_rman backup -B ${BACKUP_PATH} -b incremental -Z -p ${TEST_PGPORT} -d postgres --quiet
pg_rman validate -B ${BACKUP_PATH} --quiet
pgbench -p ${TEST_PGPORT} >> ${TEST_BASE}/pgbench.log 2>&1
pg_rman backup -B ${BACKUP_PATH} -b archive -Z -p ${TEST_PGPORT} -d postgres --quiet
pg_rman validate -B ${BACKUP_PATH} --quiet
FOURTH_BACKUP_DATE=`date +"%Y-%m-%d %H:%M:%S"`
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet
pg_rman validate -B ${BACKUP_PATH} --quiet

echo "try to delete before third backup"
pg_rman delete -B ${BACKUP_PATH} ${THIRD_BACKUP_DATE} > /dev/null 2>&1
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0002.out.1 2>&1
pg_rman show -a -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0002.out.2 2>&1
grep -c OK ${TEST_BASE}/TEST-0002.out.1
grep -c DELETED ${TEST_BASE}/TEST-0002.out.2
NUM_OF_DELETED_BACKUPS=`grep DELETED ${TEST_BASE}/TEST-0002.out.2 | wc -l`
echo "Number of deleted backups should be 1, is it so?: ${NUM_OF_DELETED_BACKUPS}"

init_backup
# clean up the temporal test data
pg_ctl stop -m immediate > /dev/null 2>&1
rm -fr ${PGDATA_PATH}
rm -fr ${BACKUP_PATH}
rm -fr ${ARCLOG_PATH}
rm -fr ${SRVLOG_PATH}
rm -fr ${TBLSPC_PATH}
