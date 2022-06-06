#!/bin/bash

#============================================================================
# This is a test script for show command of pg_rman.
#============================================================================

# Load common rules
. sql/common.sh show

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
    cp ${PGDATA_PATH}/postgresql.conf ${PGDATA_PATH}/postgresql.conf_org
    cat << EOF >> ${PGDATA_PATH}/postgresql.conf
port = ${TEST_PGPORT}
logging_collector = on
wal_level = replica
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

echo '###### SHOW COMMAND TEST-0001 ######'
echo '###### Status DONE and OK ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0001-show.out.1 2>&1
if grep "DONE" ${TEST_BASE}/TEST-0001-show.out.1 > /dev/null ; then
     echo 'OK: DONE status is shown properly.'
else
     echo 'NG: DONE status is not shown.'
fi
pg_rman validate -B ${BACKUP_PATH} --quiet;echo $?
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0001-show.out.2 2>&1
if grep "OK" ${TEST_BASE}/TEST-0001-show.out.2 > /dev/null ; then
     echo 'OK: OK status is shown properly.'
else
     echo 'NG: OK status is not shown.'
fi
echo ''

echo '###### SHOW COMMAND TEST-0002 ######'
echo '###### Status RUNNING  ######'
init_catalog
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet & # run in background
sleep 1
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0002-show.out 2>&1
if grep "RUNNING" ${TEST_BASE}/TEST-0002-show.out > /dev/null ; then
     echo 'OK: RUNNING status is shown properly.'
else
     echo 'NG: RUNNING status is not shown.'
fi
wait # wait to finish the backup running in the background with '&'.
echo ''

echo '###### SHOW COMMAND TEST-0003 ######'
echo '###### Status CORRUPT ######'
init_catalog
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
echo 'remove a file from backup intentionally'
rm -f `find ${BACKUP_PATH} -name postgresql.conf`
pg_rman validate -B ${BACKUP_PATH} --quiet > /dev/null 2>&1;echo $?
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0003-show.out 2>&1
if grep "CORRUPT" ${TEST_BASE}/TEST-0003-show.out > /dev/null ; then
     echo 'OK: CORRUPT status is shown properly.'
else
     echo 'NG: CORRUPT status is not shown.'
fi
echo ''

echo '###### SHOW COMMAND TEST-0004 ######'
echo '###### Status DELETED ######'
init_catalog
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet > /dev/null 2>&1;echo $?
DELETE_DATE=`date +"%Y-%m-%d %H:%M:%S"`
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet > /dev/null 2>&1;echo $?
pg_rman delete ${DELETE_DATE} -B ${BACKUP_PATH} > /dev/null 2>&1;echo $?
pg_rman show -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0004-show.out 2>&1
pg_rman show -a -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0004-show-all.out 2>&1
if ! grep "DELETED" ${TEST_BASE}/TEST-0004-show.out > /dev/null && grep "DELETED" ${TEST_BASE}/TEST-0004-show-all.out > /dev/null ; then
     echo 'OK: DELETED status is shown properly.'
else
     echo 'NG: DELETED status is not shown.'
fi
echo ''

# clean up the temporal test data
pg_ctl stop -D ${PGDATA_PATH} -m immediate > /dev/null 2>&1
rm -fr ${PGDATA_PATH}
rm -fr ${BACKUP_PATH}
rm -fr ${ARCLOG_PATH}
rm -fr ${SRVLOG_PATH}
rm -fr ${TBLSPC_PATH}
