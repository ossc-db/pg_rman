#!/bin/bash

#============================================================================
# This is a test script for options of pg_rman.
#============================================================================

# Load common rules
. sql/common.sh option

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

cleanup

echo '###### COMMAND OPTION TEST-0001 ######'
echo '###### help option ######'
pg_rman --help;echo $?
echo ''

echo '###### COMMAND OPTION TEST-0002 ######'
echo '###### version option ######'
pg_rman --version;echo $?
echo ''

echo '###### COMMAND OPTION TEST-0003 ######'
echo '###### backup command failure without backup path option ######'
pg_rman backup -A ${ARCLOG_PATH} -b full -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0004 ######'
echo '###### backup command failure without arclog path option ######'
pg_rman backup -B ${BACKUP_PATH} -b full -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0005 ######'
echo '###### backup command failure without srvlog path option ######'
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -b full -s -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0006 ######'
echo '###### backup command failure without backup mode option ######'
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0007 ######'
echo '###### backup command failure with invalid backup mode option ######'
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -b bad -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0008 ######'
echo '###### delete failure without DATE ######'
pg_rman delete -B ${BACKUP_PATH};echo $?
echo ''

init_backup

echo '###### COMMAND OPTION TEST-0009 ######'
echo '###### syntax error in pg_rman.ini ######'
echo " = INFINITE" >> ${BACKUP_PATH}/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0010 ######'
echo '###### invalid value in pg_rman.ini ######'
init_catalog
echo "BACKUP_MODE=" >> ${BACKUP_PATH}/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0011 ######'
echo '###### invalid value in pg_rman.ini ######'
init_catalog
echo "COMPRESS_DATA=FOO" >> ${BACKUP_PATH}/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -b full -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0012 ######'
echo '###### invalid value in pg_rman.ini ######'
init_catalog
echo "KEEP_ARCLOG_FILES=TRUE" >> ${BACKUP_PATH}/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -b full -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0013 ######'
echo '###### invalid value in pg_rman.ini ######'
init_catalog
echo "KEEP_ARCLOG_DAYS=TRUE" >> ${BACKUP_PATH}/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -b full -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0014 ######'
echo '###### invalid value in pg_rman.ini ######'
init_catalog
echo "KEEP_SRVLOG_FILES=TRUE" >> ${BACKUP_PATH}/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -b full -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0015 ######'
echo '###### invalid value in pg_rman.ini ######'
init_catalog
echo "KEEP_SRVLOG_DAYS=TRUE" >> ${BACKUP_PATH}/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -b full -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0016 ######'
echo '###### invalid value in pg_rman.ini ######'
init_catalog
echo "KEEP_DATA_GENERATIONS=TRUE" >> ${BACKUP_PATH}/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -b full -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0017 ######'
echo '###### invalid value in pg_rman.ini ######'
init_catalog
echo "KEEP_DATA_GENERATIONS=0" >> ${BACKUP_PATH}/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -b full -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0018 ######'
echo '###### invalid value in pg_rman.ini ######'
init_catalog
echo "KEEP_SRVLOG_FILES=0" >> ${BACKUP_PATH}/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -b full -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0019 ######'
echo '###### invalid value in pg_rman.ini ######'
init_catalog
echo "KEEP_SRVLOG_DAYS=0" >> ${BACKUP_PATH}/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -b full -p ${TEST_PGPORT};echo $?
echo ''


echo '###### COMMAND OPTION TEST-0020 ######'
echo '###### invalid value in pg_rman.ini ######'
init_catalog
echo "SMOOTH_CHECKPOINT=FOO" >> ${BACKUP_PATH}/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -b full -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0021 ######'
echo '###### invalid value in pg_rman.ini ######'
init_catalog
echo "WITH_SERVERLOG=FOO" >> ${BACKUP_PATH}/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -b full -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0022 ######'
echo '###### invalid value in pg_rman.ini ######'
init_catalog
echo "HARD_COPY=FOO" >> ${BACKUP_PATH}/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -b full -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0023 ######'
echo '###### invalid option in pg_rman.ini ######'
init_catalog
echo "TIMELINEID=1" >> ${BACKUP_PATH}/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -b full -p ${TEST_PGPORT};echo $?
echo ''

echo '###### COMMAND OPTION TEST-0024 ######'
echo '###### check priority of several pg_rman.ini files ######'
init_catalog
mkdir -p ${BACKUP_PATH}/conf_path_a
echo "BACKUP_MODE=ENV_PATH" > ${BACKUP_PATH}/pg_rman.ini
echo "BACKUP_MODE=ENV_PATH_A" > ${BACKUP_PATH}/conf_path_a/pg_rman.ini
pg_rman backup -B ${BACKUP_PATH} -A ${ARCLOG_PATH} -p ${TEST_PGPORT};echo $?
echo ''

# clean up the temporal test data
pg_ctl stop -m immediate > /dev/null 2>&1
rm -fr ${PGDATA_PATH}
rm -fr ${BACKUP_PATH}
rm -fr ${ARCLOG_PATH}
rm -fr ${SRVLOG_PATH}
rm -fr ${TBLSPC_PATH}
