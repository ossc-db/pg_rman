#!/bin/bash

#============================================================================
# This is a test script for archived WAL and serverlog files management of pg_rman.
#============================================================================

# Load common rules
. sql/common.sh arc_srv_log_management

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
	DEFAULT_LOG_DIRECTORY="FALSE";
	while [ $# -gt 0 ]; do
		case "$1" in
			--default-log_directory)
				DEFAULT_LOG_DIRECTORY="TRUE"
			;;
		esac
		shift
	done

	# cleanup environment
	cleanup

	# create new database cluster
	initdb --no-locale -D ${PGDATA_PATH} > ${TEST_BASE}/initdb.log 2>&1
	cp $PGDATA_PATH/postgresql.conf $PGDATA_PATH/postgresql.conf_org
	cat << EOF >> $PGDATA_PATH/postgresql.conf
port = ${TEST_PGPORT}
logging_collector = on
wal_level = hot_standby
archive_mode = on
archive_command = 'cp %p ${ARCLOG_PATH}/%f'
EOF

	if [ "$DEFAULT_LOG_DIRECTORY" = "FALSE" ]; then
		cat << EOF >> $PGDATA_PATH/postgresql.conf
log_directory = '${SRVLOG_PATH}'
EOF
	fi

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

function create_dummy_files()
{
	for i in `seq 1 $3`
	do
		touch $1/$2_$i
	done
}

function change_files_timestamp()
{
	TIMESTAMP=`date +"%Y%m%d%H%M" -d "$1 days ago"`
	for file in $2/*; do
		touch -t ${TIMESTAMP} ${file}
	done
}

function test_keep_srvlog_files()
{
	SRVLOG_PATH_ARG=$1

	create_dummy_files ${SRVLOG_PATH_ARG} logfile 3
	pg_rman backup -B ${BACKUP_PATH} -b full -Z -s -p ${TEST_PGPORT} -d postgres --quiet;echo $?
	pg_rman validate -B ${BACKUP_PATH} --quiet
	NUM_OF_SRVLOG_FILES_BEFORE=`ls ${SRVLOG_PATH_ARG} | grep -v backup | wc -l`
	echo "Number of existing server log files: ${NUM_OF_SRVLOG_FILES_BEFORE}"
	echo "do --keep-srvlog-files=3"
	#ls -l ${SRVLOG_PATH_ARG}
	pg_rman backup -B ${BACKUP_PATH} -b arc -Z -s --keep-srvlog-files=3 -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1;echo $?
	# FOR DEBUG
	#pg_rman backup -B ${BACKUP_PATH} -b arc -Z -s --keep-srvlog-files=3 -p ${TEST_PGPORT} -d postgres --debug;echo $?
	NUM_OF_SRVLOG_FILES_AFTER=`ls ${SRVLOG_PATH_ARG} | grep -v backup | wc -l`
	echo "Number of remaining server log files: ${NUM_OF_SRVLOG_FILES_AFTER}"
	#ls -l ${SRVLOG_PATH_ARG}
}

function test_keep_srvlog_days()
{
	SRVLOG_PATH_ARG=$1

	create_dummy_files ${SRVLOG_PATH_ARG} old_logfile 3
	change_files_timestamp 2 ${SRVLOG_PATH_ARG}
	create_dummy_files ${SRVLOG_PATH_ARG} new_logfile 3
	pg_rman backup -B ${BACKUP_PATH} -b full -Z -s -p ${TEST_PGPORT} -d postgres --quiet;echo $?
	pg_rman validate -B ${BACKUP_PATH} --quiet
	NUM_OF_SRVLOG_FILES_BEFORE=`ls ${SRVLOG_PATH_ARG} | grep -v backup | wc -l`
	echo "Number of existing server log files: ${NUM_OF_SRVLOG_FILES_BEFORE}"
	echo "do --keep-srvlog-days=1"
	#ls -l ${SRVLOG_PATH_ARG}
	pg_rman backup -B ${BACKUP_PATH} -b arc -Z -s --keep-srvlog-days=1 -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1;echo $?
	# FOR DEBUG
	#pg_rman backup -B ${BACKUP_PATH} -b arc -Z -s --keep-srvlog-days=1 -p ${TEST_PGPORT} -d postgres --debug;echo $?
	NUM_OF_SRVLOG_FILES_AFTER=`ls ${SRVLOG_PATH_ARG} | grep -v backup | wc -l`
	echo "Number of remaining server log files: ${NUM_OF_SRVLOG_FILES_AFTER}"
	#ls -l ${SRVLOG_PATH_ARG}
}

function test_keep_srvlog_files_and_keep_srvlog_days_together()
{
	SRVLOG_PATH_ARG=$1

	create_dummy_files ${SRVLOG_PATH_ARG} old_logfile 3
	change_files_timestamp 2 ${SRVLOG_PATH_ARG}
	create_dummy_files ${SRVLOG_PATH_ARG} new_logfile 3
	pg_rman backup -B ${BACKUP_PATH} -b full -Z -s -p ${TEST_PGPORT} -d postgres --quiet;echo $?
	pg_rman validate -B ${BACKUP_PATH} --quiet
	NUM_OF_SRVLOG_FILES_BEFORE=`ls ${SRVLOG_PATH_ARG} | grep -v backup | wc -l`
	echo "Number of existing server log files: ${NUM_OF_SRVLOG_FILES_BEFORE}"
	echo "do --keep-srvlog-files=4 AND --keep-srvlog-days=1"
	#ls -l ${SRVLOG_PATH_ARG}
	pg_rman backup -B ${BACKUP_PATH} -b arc -Z -s --keep-srvlog-files=4 --keep-srvlog-days=1 -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1;echo $?
	# FOR DEBUG
	#pg_rman backup -B ${BACKUP_PATH} -b arc -Z -s --keep-srvlog-files=4 --keep-srvlog-days=1 -p ${TEST_PGPORT} -d postgres --debug;echo $?
	NUM_OF_SRVLOG_FILES_AFTER=`ls ${SRVLOG_PATH_ARG} | grep -v backup | wc -l`
	echo "Number of remaining server log files: ${NUM_OF_SRVLOG_FILES_AFTER}"
	#ls -l ${SRVLOG_PATH_ARG}
}

init_backup

echo '###### LOG FILE MANAGEMENT TEST-0001 ######'
echo '###### keep-arclog-files only ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman backup -B ${BACKUP_PATH} -b inc -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
NUM_OF_ARCLOG_FILES_BEFORE=`ls ${ARCLOG_PATH} | grep -v backup | wc -l`
echo "Number of existing archivelog files: ${NUM_OF_ARCLOG_FILES_BEFORE}"
echo "do --keep-arclog-files=3"
#ls -l ${ARCLOG_PATH}
pg_rman backup -B ${BACKUP_PATH} -b inc -Z --keep-arclog-files=3 -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1; echo $?
# FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b inc -Z --keep-arclog-files=3 -p ${TEST_PGPORT} -d postgres --debug; echo $?
NUM_OF_ARCLOG_FILES_AFTER=`ls ${ARCLOG_PATH} | grep -v backup | wc -l`
echo "Number of remaining archivelog files: ${NUM_OF_ARCLOG_FILES_AFTER}"
#ls -l ${ARCLOG_PATH}

echo '###### LOG FILE MANAGEMENT TEST-0002 ######'
echo '###### keep-arclog-days only ######'
change_files_timestamp 2 ${ARCLOG_PATH}
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
#ls -l ${ARCLOG_PATH}
NUM_OF_ARCLOG_FILES_BEFORE=`ls ${ARCLOG_PATH} | grep -v backup | wc -l`
echo "Number of existing archivelog files: ${NUM_OF_ARCLOG_FILES_BEFORE}"
echo "do --keep-arclog-days=1"
pg_rman backup -B ${BACKUP_PATH} -b inc -Z --keep-arclog-days=1 -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1; echo $?
# FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b inc -Z --keep-arclog-days=1 -p ${TEST_PGPORT} -d postgres --debug; echo $?
NUM_OF_ARCLOG_FILES_AFTER=`ls ${ARCLOG_PATH} | grep -v backup | wc -l`
echo "Number of remaining archivelog files: ${NUM_OF_ARCLOG_FILES_AFTER}"
#ls -l ${ARCLOG_PATH}


echo '###### LOG FILE MANAGEMENT TEST-0003 ######'
echo '###### keep-arclog-files and keep-arclog-days together ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
change_files_timestamp 2 ${ARCLOG_PATH}
pg_rman backup -B ${BACKUP_PATH} -b inc -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman backup -B ${BACKUP_PATH} -b inc -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
NUM_OF_ARCLOG_FILES_BEFORE=`ls ${ARCLOG_PATH} | grep -v backup | wc -l`
echo "Number of existing archivelog files: ${NUM_OF_ARCLOG_FILES_BEFORE}"
echo "do --keep-arclog-files=3 AND --keep-arclog-days=1"
#ls -l ${ARCLOG_PATH}
pg_rman backup -B ${BACKUP_PATH} -b inc -Z --keep-arclog-files=3 --keep-arclog-days=1 -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1; echo $?
# FOR DEBUG
#pg_rman backup -B ${BACKUP_PATH} -b inc -Z --keep-arclog-files=3 --keep-arclog-days=1 -p ${TEST_PGPORT} -d postgres --debug; echo $?
NUM_OF_ARCLOG_FILES_AFTER=`ls ${ARCLOG_PATH} | grep -v backup | wc -l`
echo "Number of remaining archivelog files: ${NUM_OF_ARCLOG_FILES_AFTER}"
#ls -l ${ARCLOG_PATH}

init_catalog

echo '###### LOG FILE MANAGEMENT TEST-0004 ######'
echo '###### keep-srvlog-files only ######'
test_keep_srvlog_files ${SRVLOG_PATH}

init_backup

echo '###### LOG FILE MANAGEMENT TEST-0005 ######'
echo '###### keep-srvlog-days only ######'
test_keep_srvlog_days ${SRVLOG_PATH}

init_backup

echo '###### LOG FILE MANAGEMENT TEST-0006 ######'
echo '###### keep-srvlog-files and keep-srvlog-days together ######'
test_keep_srvlog_files_and_keep_srvlog_days_together ${SRVLOG_PATH}

# Test with default log directory from TEST-0007 to TEST-0009
init_backup --default-log_directory
DEFAULT_SRVLOG_PATH=${PGDATA_PATH}/`get_guc_value log_directory`

echo '###### LOG FILE MANAGEMENT TEST-0007 ######'
echo '###### keep-srvlog-files only with default "log_directory" option ######'
test_keep_srvlog_files $DEFAULT_SRVLOG_PATH

init_backup --default-log_directory

echo '###### LOG FILE MANAGEMENT TEST-0008 ######'
echo '###### keep-srvlog-days only with default "log_directory" option ######'
test_keep_srvlog_days $DEFAULT_SRVLOG_PATH

init_backup --default-log_directory

echo '###### LOG FILE MANAGEMENT TEST-0009 ######'
echo '###### keep-srvlog-files and keep-srvlog-days together with default "log_directory" option ######'
test_keep_srvlog_files_and_keep_srvlog_days_together $DEFAULT_SRVLOG_PATH


# clean up the temporal test data
pg_ctl stop -m immediate > /dev/null 2>&1
rm -fr ${PGDATA_PATH}
rm -fr ${BACKUP_PATH}
rm -fr ${ARCLOG_PATH}
rm -fr ${SRVLOG_PATH}
rm -fr ${TBLSPC_PATH}
