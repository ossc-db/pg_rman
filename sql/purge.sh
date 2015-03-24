#!/bin/bash

#============================================================================
# This is a test script for purge command of pg_rman.
#============================================================================

# Load common rules
. sql/common.sh purge

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
echo '###### PURGE COMMAND TEST-0001 ######'
echo '###### purge DELETED backups sucessfully ######'

FIRST_BACKUP_DATE=`date +"%Y-%m-%d %H:%M:%S"`
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet
pgbench -p ${TEST_PGPORT} >> ${TEST_BASE}/pgbench.log 2>&1
SECOND_BACKUP_DATE=`date +"%Y-%m-%d %H:%M:%S"`
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet
pgbench -p ${TEST_PGPORT} >> ${TEST_BASE}/pgbench.log 2>&1
THRID_BACKUP_DATE=`date +"%Y-%m-%d %H:%M:%S"`
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet
pg_rman validate -B ${BACKUP_PATH} --quiet

echo "delete the oldest backup"
pg_rman -B ${BACKUP_PATH} delete ${SECOND_BACKUP_DATE} --quiet
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0001.out.1 2>&1
pg_rman show -a -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0001.out.2 2>&1

echo "Now, test purge command"
pg_rman purge -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0001.out.3 2>&1
pg_rman show -a -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0001.out.4 2>&1

NUM_OF_PURGED_BACKUPS=`diff ${TEST_BASE}/TEST-0001.out.2 ${TEST_BASE}/TEST-0001.out.4 | grep DELETED | wc -l`
echo "Number of purged backups: ${NUM_OF_PURGED_BACKUPS}"

init_backup
echo '###### PURGE COMMAND TEST-0002 ######'
echo '###### purge DELETED backups with check option ######'

FIRST_BACKUP_DATE=`date +"%Y-%m-%d %H:%M:%S"`
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet
pgbench -p ${TEST_PGPORT} >> ${TEST_BASE}/pgbench.log 2>&1
SECOND_BACKUP_DATE=`date +"%Y-%m-%d %H:%M:%S"`
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet
pgbench -p ${TEST_PGPORT} >> ${TEST_BASE}/pgbench.log 2>&1
THRID_BACKUP_DATE=`date +"%Y-%m-%d %H:%M:%S"`
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet
pg_rman validate -B ${BACKUP_PATH} --quiet

echo "delete the oldest backup"
pg_rman -B ${BACKUP_PATH} delete ${SECOND_BACKUP_DATE} --quiet
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0002.out.1 2>&1
pg_rman show -a -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0002.out.2 2>&1

echo "Now, test purge command with check option"
pg_rman purge -B ${BACKUP_PATH} --check > ${TEST_BASE}/TEST-0002.out.3 2>&1
pg_rman show -a -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0002.out.4 2>&1

NUM_OF_PURGED_BACKUPS=`diff ${TEST_BASE}/TEST-0002.out.2 ${TEST_BASE}/TEST-0002.out.4 | grep DELETED | wc -l`
echo "Number of purged backups: ${NUM_OF_PURGED_BACKUPS}"

# clean up the temporal test data
pg_ctl stop -m immediate > /dev/null 2>&1
rm -fr ${PGDATA_PATH}
rm -fr ${BACKUP_PATH}
rm -fr ${ARCLOG_PATH}
rm -fr ${SRVLOG_PATH}
rm -fr ${TBLSPC_PATH}
