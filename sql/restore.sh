#!/bin/bash

#============================================================================
# This is a test script for restore command of pg_rman.
#============================================================================

# Load common rules
. sql/common.sh restore

# Parameters exclusive to this test
SCALE=2
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
	if [ $? = "1" ]; then
    	echo "initdb did not succeed (--data-checksum not supported on this PostgreSQL version)."
    	echo "Aborting regression tests..."
    	exit
	fi
    cp ${PGDATA_PATH}/postgresql.conf ${PGDATA_PATH}/postgresql.conf_org
    cat << EOF >> ${PGDATA_PATH}/postgresql.conf
port = ${TEST_PGPORT}
logging_collector = on
wal_level = replica
log_directory = '${SRVLOG_PATH}'
log_filename = 'postgresql-%F_%H%M%S.log'
archive_mode = on
archive_command = 'cp %p ${ARCLOG_PATH}/%f'
max_wal_size = 512MB
EOF

    # start PostgreSQL
    pg_ctl start -D ${PGDATA_PATH} -w -t 300 > /dev/null 2>&1
	mkdir -p ${TBLSPC_PATH}/pgbench
	psql --no-psqlrc -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1 << EOF
CREATE TABLESPACE pgbench LOCATION '${TBLSPC_PATH}/pgbench';
CREATE DATABASE pgbench TABLESPACE = pgbench;
EOF

    pgbench -i -s $SCALE -p ${TEST_PGPORT} -d pgbench > ${TEST_BASE}/pgbench.log 2>&1

    # init backup catalog
    init_catalog
}

function init_catalog()
{
    rm -fr ${BACKUP_PATH}
    pg_rman init -B ${BACKUP_PATH} --quiet
}

function load_with_pgbench
{
	pgbench -p ${TEST_PGPORT} -T ${DURATION} -d pgbench > /dev/null 2>&1
}

function get_database_data
{
	psql -p ${TEST_PGPORT} --no-psqlrc -d pgbench -c "SELECT * FROM pgbench_branches;"
}

function server_is_running
{
	# if 1: running, 0: stopped
	pg_ctl status | grep "server is running" | wc -l
}

function start_postgres
{
	pg_ctl start -w -t 600 > /dev/null 2>&1
}

function stop_postgres
{
	pg_ctl stop -m fast >  /dev/null 2>&1
}

function pg_is_in_recovery
{
	psql -tA -p ${TEST_PGPORT} --no-psqlrc -d pgbench -c "SELECT pg_is_in_recovery();"
}

function full_backup
{
	pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
	pg_rman validate -B ${BACKUP_PATH} --quiet; echo $?
}

function recovery_target_action_test
{
	recovery_target_action=$1
	test_log=$2                   # ${TEST_BASE}/TEST-XXXX

	echo "recovery-target-action=$recovery_target_action"

	init_backup
	load_with_pgbench
	full_backup
	get_database_data > ${test_log}-before.out
	TARGET_TIME=`date +"%Y-%m-%d %H:%M:%S"`
	load_with_pgbench
	full_backup
	pg_ctl stop -m fast > /dev/null 2>&1
	pg_rman restore -B ${BACKUP_PATH} --recovery-target-time="${TARGET_TIME}" \
			 --recovery-target-action="${recovery_target_action}" --quiet;echo $?
	pg_ctl start -w -t 600 > /dev/null 2>&1
	sleep 5

	if [ $recovery_target_action = "shutdown" ]; then
		if [ `server_is_running` = "0" ]; then
			echo 'OK: server is stopped. recovery-target-action works well.'
		else
			echo 'NG: server is running. recovery-target-action does not work well.'
		fi
		return;
	fi

	# check the data records after shutdown check is done
	get_database_data > ${test_log}-after.out
	diff ${test_log}-before.out ${test_log}-after.out

	if [ `pg_is_in_recovery` = "f" ]; then
		if [ $recovery_target_action = "promote" ]; then
			echo 'OK: promoted. recovery-target-action works well.'
		else
			echo 'NG: promoted. recovery-target-action does not work well.'
		fi
	else
		if [ $recovery_target_action = "pause" ]; then
			echo 'OK: not promoted. recovery-target-action works well.'
		else
			echo 'NG: not promoted. recovery-target-action does not work well.'
		fi
	fi
}


init_backup

echo '###### RESTORE COMMAND TEST-0001 ######'
echo '###### recovery to latest from full backup ######'
pgbench -p ${TEST_PGPORT} -d pgbench > /dev/null 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0001-before.out
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_ctl stop -m immediate > /dev/null 2>&1
pg_rman restore -B ${BACKUP_PATH} --quiet;echo $?
pg_ctl start -w -t 600 > /dev/null 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0001-after.out
diff ${TEST_BASE}/TEST-0001-before.out ${TEST_BASE}/TEST-0001-after.out
echo ''

echo '###### RESTORE COMMAND TEST-0002 ######'
echo '###### recovery to latest from full + incremental backups ######'
init_backup
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pgbench -p ${TEST_PGPORT} -d pgbench > /dev/null 2>&1
pg_rman backup -B ${BACKUP_PATH} -b incremental -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0002-before.out
pg_ctl stop -m immediate > /dev/null 2>&1
pg_rman restore -B ${BACKUP_PATH} --quiet;echo $?
pg_ctl start -w -t 600 > /dev/null 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0002-after.out
diff ${TEST_BASE}/TEST-0002-before.out ${TEST_BASE}/TEST-0002-after.out
echo ''

echo '###### RESTORE COMMAND TEST-0003 ######'
echo '###### recovery to latest from compressed full backup ######'
init_backup
pgbench -p ${TEST_PGPORT} -d pgbench > /dev/null 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0003-before.out
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_ctl stop -m immediate > /dev/null 2>&1
pg_rman restore -B ${BACKUP_PATH} --quiet;echo $?
pg_ctl start -w -t 600 > /dev/null 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0003-after.out
diff ${TEST_BASE}/TEST-0003-before.out ${TEST_BASE}/TEST-0003-after.out
echo ''

echo '###### RESTORE COMMAND TEST-0004 ######'
echo '###### recovery to latest from full + archivelog backups ######'
init_backup
pgbench -p ${TEST_PGPORT} -d pgbench > /dev/null 2>&1
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pgbench -p ${TEST_PGPORT} -d pgbench > /dev/null 2>&1
pg_rman backup -B ${BACKUP_PATH} -b archive -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0004-before.out
sleep 1
pg_ctl stop -m immediate > /dev/null 2>&1
pg_rman restore -B ${BACKUP_PATH} --quiet;echo $?
pg_ctl start -w -t 600 > /dev/null 2>&1
sleep 1
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0004-after.out
diff ${TEST_BASE}/TEST-0004-before.out ${TEST_BASE}/TEST-0004-after.out
echo ''

echo '###### RESTORE COMMAND TEST-0005 ######'
echo '###### recovery to target timeline ######'
init_backup
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0005-before.out
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
TARGET_TLI=`pg_controldata | grep " TimeLineID:" | awk '{print $4}'`
pg_ctl stop -m immediate > /dev/null 2>&1
pg_rman restore -B ${BACKUP_PATH} --quiet;echo $?
pg_ctl start -w -t 600 > /dev/null 2>&1
pgbench -p ${TEST_PGPORT} -d pgbench > /dev/null 2>&1
pg_ctl stop -m immediate > /dev/null 2>&1
pg_rman restore -B ${BACKUP_PATH} --recovery-target-timeline=${TARGET_TLI} --quiet;echo $?
echo "checking recovery.conf..."
TARGET_TLI_IN_RECOVERY_CONF=`grep "recovery_target_timeline = " ${PGDATA_PATH}/recovery.conf | awk '{print $3}' | sed -e "s/'//g"`
if [ ${TARGET_TLI} = ${TARGET_TLI_IN_RECOVERY_CONF} ]; then
	echo 'OK: recovery.conf has the given target timeline.'
else
	echo 'NG: recovery.conf does not have the given target timeline.'
fi
pg_ctl start -w -t 600 > /dev/null 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0005-after.out
diff ${TEST_BASE}/TEST-0005-before.out ${TEST_BASE}/TEST-0005-after.out
echo ''

echo '###### RESTORE COMMAND TEST-0006 ######'
echo '###### recovery to target time ######'
init_backup
pgbench -p ${TEST_PGPORT} -d pgbench > /dev/null 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0006-before.out
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
TARGET_TIME=`date +"%Y-%m-%d %H:%M:%S"`
pgbench -p ${TEST_PGPORT} -d pgbench > /dev/null 2>&1
pg_ctl stop -m immediate > /dev/null 2>&1
pg_rman restore -B ${BACKUP_PATH} --recovery-target-time="${TARGET_TIME}" --quiet;echo $?
pg_ctl start -w -t 600 > /dev/null 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0006-after.out
diff ${TEST_BASE}/TEST-0006-before.out ${TEST_BASE}/TEST-0006-after.out
echo ''

echo '###### RESTORE COMMAND TEST-0007 ######'
echo '###### recovery to target XID ######'
init_backup
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "CREATE TABLE tbl0007 (a text);" > /dev/null 2>&1
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pgbench -p ${TEST_PGPORT} pgbench > /dev/null 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0007-before.out
TARGET_XID=`psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -tAq -c "INSERT INTO tbl0007 VALUES ('inserted') RETURNING (xmin);"`
pgbench -p ${TEST_PGPORT} -d pgbench > /dev/null 2>&1
pg_ctl stop -m fast > /dev/null 2>&1
pg_rman restore -B ${BACKUP_PATH} --recovery-target-xid="${TARGET_XID}" --quiet;echo $?
pg_ctl start -w -t 600 > /dev/null 2>&1
sleep 1
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0007-after.out
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM tbl0007;" > ${TEST_BASE}/TEST-0007-tbl.dump
diff ${TEST_BASE}/TEST-0007-before.out ${TEST_BASE}/TEST-0007-after.out
if grep "inserted" ${TEST_BASE}/TEST-0007-tbl.dump > /dev/null ; then
	echo 'OK: recovery-target-xid options works well.'
else
	echo 'NG: recovery-target-xid options does not work well.'
fi
echo ''

echo '###### RESTORE COMMAND TEST-0008 ######'
echo '###### recovery with target inclusive false ######'
init_backup
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "CREATE TABLE tbl0008 (a text);" > /dev/null 2>&1
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pgbench -p ${TEST_PGPORT} pgbench > /dev/null 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0008-before.out
TARGET_XID=`psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -tAq -c "INSERT INTO tbl0008 VALUES ('inserted') RETURNING (xmin);"`
pgbench -p ${TEST_PGPORT} -d pgbench > /dev/null 2>&1
pg_ctl stop -m fast > /dev/null 2>&1
pg_rman restore -B ${BACKUP_PATH} --recovery-target-xid="${TARGET_XID}" --recovery-target-inclusive=false --quiet;echo $?
pg_ctl start -w -t 600 > /dev/null 2>&1
sleep 1
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0008-after.out
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM tbl0008;" > ${TEST_BASE}/TEST-0008-tbl.dump
diff ${TEST_BASE}/TEST-0008-before.out ${TEST_BASE}/TEST-0008-after.out
if grep "inserted" ${TEST_BASE}/TEST-0008-tbl.dump > /dev/null ; then
	echo 'NG: recovery-target-inclusive=false does not work well.'
else
	echo 'OK: recovery-target-inclusive=false works well.'
fi
echo ''

echo '###### RESTORE COMMAND TEST-0009 ######'
echo '###### recovery with target action pause ######'
recovery_target_action_test pause "${TEST_BASE}/TEST-0009"
echo ''

echo '###### RESTORE COMMAND TEST-0010 ######'
echo '###### recovery with target action promote ######'
recovery_target_action_test promote "${TEST_BASE}/TEST-0010"
echo ''

echo '###### RESTORE COMMAND TEST-0011 ######'
echo '###### recovery with target action shutdown ######'
recovery_target_action_test shutdown "${TEST_BASE}/TEST-0011"
echo ''

echo '###### RESTORE COMMAND TEST-0012 ######'
echo '###### recovery with hard-copy option ######'
init_backup
pgbench -p ${TEST_PGPORT} -d pgbench > /dev/null 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d pgbench -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0012-before.out
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_ctl stop -m immediate > /dev/null 2>&1
pg_rman restore -B ${BACKUP_PATH} --hard-copy --quiet;echo $?
FLAGS=0
for FILENAME in `ls ${ARCLOG_PATH}`
do
	if [ -L ${ARCLOG_PATH}/${FILENAME} ]; then
		echo 'NG: hard-copy option does not work well.' ${FILENAME} 'is a symbolic link.'
		FLAGS=`expr ${FLAGS} + 1`
	fi
done
if [ ${FLAGS} -eq 0 ]; then
	echo 'OK: hard-copy option works well.'
else
	echo 'NG: hard-copy option does not work well.'
fi
echo ''

echo '###### RESTORE COMMAND TEST-0013 ######'
echo '###### recovery from incremental backup after database creation ######'
init_backup
pg_ctl start -w -t 600 > /dev/null 2>&1
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
createdb db0013 -p ${TEST_PGPORT}
pgbench -i -s $SCALE -d db0013 -p ${TEST_PGPORT} > ${TEST_BASE}/TEST-0013-db0013-init.out 2>&1
pg_rman backup -B ${BACKUP_PATH} -b incremental -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pgbench -p ${TEST_PGPORT} -d db0013 >> ${TEST_BASE}/TEST-0013-db0013-init.out 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d db0013 -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0013-before.out
pg_ctl stop -m fast > /dev/null 2>&1
pg_rman restore -B ${BACKUP_PATH} --quiet;echo $?
pg_ctl start -w -t 600 > /dev/null 2>&1
sleep 1
psql --no-psqlrc -p ${TEST_PGPORT} -d db0013 -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0013-after.out
diff ${TEST_BASE}/TEST-0013-before.out ${TEST_BASE}/TEST-0013-after.out
echo ''

echo '###### RESTORE COMMAND TEST-0014 ######'
echo '###### vacuum shrinks a page between full and incremental backups ######'
init_backup
pg_ctl start -w -t 600 > /dev/null 2>&1
createdb db0014 -p ${TEST_PGPORT}
psql --no-psqlrc -p ${TEST_PGPORT} -d db0014 -c "CREATE TABLE t0014(i int,j int,k varchar);" > /dev/null 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d db0014 -c "INSERT INTO t0014 (i,j,k) select generate_series(1,1000),1, repeat('a', 10);" > /dev/null 2>&1
pg_rman backup -B ${BACKUP_PATH} -b full -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
psql --no-psqlrc -p ${TEST_PGPORT} -d db0014 -c "DELETE FROM t0014 WHERE i > 10;" > /dev/null 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d db0014 -c "VACUUM t0014;" > /dev/null 2>&1
pg_rman backup -B ${BACKUP_PATH} -b incremental -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
psql --no-psqlrc -p ${TEST_PGPORT} -d db0014 -c "SELECT * FROM t0014;" > ${TEST_BASE}/TEST-0014-before.out
pg_ctl stop -m fast > /dev/null 2>&1
pg_rman restore -B ${BACKUP_PATH} --quiet;echo $?
pg_ctl start -w -t 600 > /dev/null 2>&1
sleep 1
psql --no-psqlrc -p ${TEST_PGPORT} -d db0014 -c "SELECT * FROM t0014;" > ${TEST_BASE}/TEST-0014-after.out
diff ${TEST_BASE}/TEST-0014-before.out ${TEST_BASE}/TEST-0014-after.out
echo ''

echo '###### RESTORE COMMAND TEST-0015 ######'
echo '###### vacuum shrinks a page between full and incremental backups(compressed) ######'
init_backup
pg_ctl start -w -t 600 > /dev/null 2>&1
createdb db0015 -p ${TEST_PGPORT}
psql --no-psqlrc -p ${TEST_PGPORT} -d db0015 -c "CREATE TABLE t0015(i int,j int,k varchar);" > /dev/null 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d db0015 -c "INSERT INTO t0015 (i,j,k) select generate_series(1,1000),1, repeat('a', 10);" > /dev/null 2>&1
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
psql --no-psqlrc -p ${TEST_PGPORT} -d db0015 -c "DELETE FROM t0015 WHERE i > 10;" > /dev/null 2>&1
psql --no-psqlrc -p ${TEST_PGPORT} -d db0015 -c "VACUUM t0015;" > /dev/null 2>&1
pg_rman backup -B ${BACKUP_PATH} -b incremental -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
psql --no-psqlrc -p ${TEST_PGPORT} -d db0015 -c "SELECT * FROM t0015;" > ${TEST_BASE}/TEST-0015-before.out
pg_ctl stop -m fast > /dev/null 2>&1
pg_rman restore -B ${BACKUP_PATH} --quiet;echo $?
pg_ctl start -w -t 600 > /dev/null 2>&1
sleep 1
psql --no-psqlrc -p ${TEST_PGPORT} -d db0015 -c "SELECT * FROM t0015;" > ${TEST_BASE}/TEST-0015-after.out
diff ${TEST_BASE}/TEST-0015-before.out ${TEST_BASE}/TEST-0015-after.out
echo ''

echo '###### RESTORE COMMAND TEST-0018 ######'
echo '###### check to work even if the path of tablespace has $PGDATA ######'
init_backup
start_postgres

TBLSPC_PATH_HAS_PGDATA_PATH=${PGDATA_PATH}_test_tbl/test
TEST_DB=test

mkdir -p ${TBLSPC_PATH_HAS_PGDATA_PATH}
psql --no-psqlrc -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1 << EOF
CREATE TABLESPACE ${TEST_DB} LOCATION '${TBLSPC_PATH_HAS_PGDATA_PATH}';
CREATE DATABASE ${TEST_DB} TABLESPACE = ${TEST_DB};
EOF

pgbench -p ${TEST_PGPORT} -i -s 10 -d ${TEST_DB} > /dev/null 2>&1
psql -p ${TEST_PGPORT} --no-psqlrc -d ${TEST_DB} -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0018-before.out

pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
stop_postgres
pg_rman restore -B ${BACKUP_PATH} --quiet;echo $?
start_postgres
sleep 1

psql -p ${TEST_PGPORT} --no-psqlrc -d ${TEST_DB} -c "SELECT * FROM pgbench_branches;" > ${TEST_BASE}/TEST-0018-after.out
diff ${TEST_BASE}/TEST-0018-before.out ${TEST_BASE}/TEST-0018-after.out

stop_postgres
rm -r ${TBLSPC_PATH_HAS_PGDATA_PATH}
echo ''

# clean up the temporal test data
pg_ctl stop -m immediate > /dev/null 2>&1
rm -fr ${PGDATA_PATH}
rm -fr ${BACKUP_PATH}
rm -fr ${ARCLOG_PATH}
rm -fr ${SRVLOG_PATH}
rm -fr ${TBLSPC_PATH}
