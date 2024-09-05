#!/bin/bash

#============================================================================
# This is a test script for backup command from standby server of pg_rman.
#============================================================================

# Load common rules
. sql/common.sh backup_from_standby

# Extra parameters exclusive to this test
SBYDATA_PATH=${TEST_BASE}/standby_data
TEST_SBYPGPORT=54322
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
    pg_ctl stop -m immediate -D ${PGDATA_PATH} > /dev/null 2>&1
    pg_ctl stop -m immediate -D ${SBYDATA_PATH} > /dev/null 2>&1
    rm -fr ${PGDATA_PATH}
    rm -fr ${SBYDATA_PATH}
    rm -fr ${BACKUP_PATH}
    rm -fr ${ARCLOG_PATH}
    rm -fr ${SRVLOG_PATH}
    mkdir -p ${ARCLOG_PATH}
    mkdir -p ${SRVLOG_PATH}
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
wal_level = replica
max_wal_senders = 2
log_directory = '${SRVLOG_PATH}'
log_filename = 'postgresql-%F_%H%M%S-%p.log'
archive_mode = on
archive_command = 'cp %p ${ARCLOG_PATH}/%f'
max_wal_size = 512MB
EOF

	cat << EOF >> ${PGDATA_PATH}/pg_hba.conf
local   replication     postgres                                trust
host    replication     postgres        127.0.0.1/32            trust
host    replication     postgres        ::1/128                 trust
local   replication     $(whoami)                               trust
host    replication     $(whoami)       127.0.0.1/32            trust
host    replication     $(whoami)       ::1/128                 trust
EOF


    # start PostgreSQL
    pg_ctl start -D ${PGDATA_PATH} -w -t 300 > /dev/null 2>&1
	psql --no-psqlrc -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1 << EOF
CREATE DATABASE pgbench;
EOF

    pgbench -i -s $SCALE -p ${TEST_PGPORT} -d pgbench > ${TEST_BASE}/pgbench.log 2>&1

}

function init_catalog()
{
    rm -fr ${BACKUP_PATH}
    pg_rman init -B ${BACKUP_PATH} -D ${SBYDATA_PATH} -A ${ARCLOG_PATH} --quiet
}

function setup_standby()
{
	pg_basebackup -d "dbname=pgbench host=localhost port=${TEST_PGPORT}" -D ${SBYDATA_PATH}  --checkpoint=fast > /dev/null 2>&1
	cp ${SBYDATA_PATH}/postgresql.conf_org ${SBYDATA_PATH}/postgresql.conf
	cat >> ${SBYDATA_PATH}/postgresql.conf << EOF
port = ${TEST_SBYPGPORT}
hot_standby = on
logging_collector = on
wal_level = replica
EOF
	touch ${SBYDATA_PATH}/standby.signal

	cat >> ${SBYDATA_PATH}/postgresql.conf << EOF
restore_command = 'cp "${ARCLOG_PATH}/%f" "%p"'
primary_conninfo = 'port=${TEST_PGPORT} application_name=standby'
EOF

	cat >> ${PGDATA_PATH}/postgresql.conf << EOF
synchronous_standby_names = 'standby'
EOF
	pg_ctl -D ${PGDATA_PATH} reload > /dev/null 2>&1
	pg_ctl -D ${SBYDATA_PATH} start -w -t 600 > /dev/null 2>&1
}

function load_with_pgbench
{
	pgbench -p ${TEST_PGPORT} -T ${DURATION} -d pgbench > /dev/null 2>&1
}

function full_backup_from_standby
{
	pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres -D ${SBYDATA_PATH} --standby-host=localhost --standby-port=${TEST_SBYPGPORT} --quiet;echo $?
	pg_rman validate -B ${BACKUP_PATH} --quiet; echo $?
}

function incremental_backup_from_standby
{
	pg_rman backup -B ${BACKUP_PATH} -b incremental -Z -p ${TEST_PGPORT} -d postgres -D ${SBYDATA_PATH} --standby-host=localhost --standby-port=${TEST_SBYPGPORT} --quiet;echo $?
	pg_rman validate -B ${BACKUP_PATH} --quiet; echo $?
}

function get_database_data_from_primary
{
	psql -p ${TEST_PGPORT} --no-psqlrc -d pgbench -c "SELECT * FROM pgbench_branches;"
}

function get_database_data_from_standby
{
	psql -p ${TEST_SBYPGPORT} --no-psqlrc -d pgbench -c "SELECT * FROM pgbench_branches;"
}

init_backup
setup_standby
init_catalog
echo '###### BACKUP COMMAND FROM STANDBY SERVER TEST-0001 ######'
echo '###### full backup mode ######'
load_with_pgbench
get_database_data_from_primary > ${TEST_BASE}/TEST-0001-before.out
full_backup_from_standby
TARGET_XID=`psql -p ${TEST_PGPORT} --no-psqlrc -d pgbench -tAq -c "INSERT INTO pgbench_history VALUES (1) RETURNING(xmin);"`
load_with_pgbench
pg_ctl stop -m immediate -D ${PGDATA_PATH} > /dev/null 2>&1
cp ${PGDATA_PATH}/postgresql.conf ${TEST_BASE}/postgresql.conf
sleep 1
pg_rman restore -B ${BACKUP_PATH} -D ${PGDATA_PATH} --recovery-target-xid=${TARGET_XID} --quiet;echo $?
RMAN_RECOVERY_CONF=`tail -n 1 ${PGDATA_PATH}/postgresql.conf` # must be appended
cp ${TEST_BASE}/postgresql.conf ${PGDATA_PATH}/postgresql.conf
echo "${RMAN_RECOVERY_CONF}" >> ${PGDATA_PATH}/postgresql.conf
pg_ctl start -w -t 600 -D ${PGDATA_PATH} > /dev/null 2>&1
get_database_data_from_primary > ${TEST_BASE}/TEST-0001-after.out
diff ${TEST_BASE}/TEST-0001-before.out ${TEST_BASE}/TEST-0001-after.out
echo ''

init_backup
setup_standby
init_catalog
echo '###### BACKUP COMMAND FROM STANDBY SERVER TEST-0002 ######'
echo '###### full + incremental backup mode ######'
load_with_pgbench
full_backup_from_standby
load_with_pgbench
incremental_backup_from_standby
get_database_data_from_primary > ${TEST_BASE}/TEST-0002-before.out
TARGET_XID=`psql -p ${TEST_PGPORT} --no-psqlrc -d pgbench -tAq -c "INSERT INTO pgbench_history VALUES (1) RETURNING(xmin);"`
load_with_pgbench
pg_ctl stop -m immediate -D ${PGDATA_PATH} > /dev/null 2>&1
cp ${PGDATA_PATH}/postgresql.conf ${TEST_BASE}/postgresql.conf
sleep 1
pg_rman restore -B ${BACKUP_PATH} -D ${PGDATA_PATH} --recovery-target-xid=${TARGET_XID} --quiet;echo $?
RMAN_RECOVERY_CONF=`tail -n 1 ${PGDATA_PATH}/postgresql.conf` # must be appended
cp ${TEST_BASE}/postgresql.conf ${PGDATA_PATH}/postgresql.conf
echo "${RMAN_RECOVERY_CONF}" >> ${PGDATA_PATH}/postgresql.conf
pg_ctl start -w -t 600 -D ${PGDATA_PATH} > /dev/null 2>&1
get_database_data_from_primary > ${TEST_BASE}/TEST-0002-after.out
diff ${TEST_BASE}/TEST-0002-before.out ${TEST_BASE}/TEST-0002-after.out
echo ''

# check to start as primary (stand-alone) if the recovery target is latest
init_backup
setup_standby
init_catalog
echo '###### BACKUP COMMAND FROM STANDBY SERVER TEST-0003 ######'
load_with_pgbench
get_database_data_from_primary > ${TEST_BASE}/TEST-0003-before.out
full_backup_from_standby
# Don't load_with_pgbench again. It makes the *.out result difference because
# restoring to the latest point using archived wal including the one generated
# by the second load.
pg_ctl stop -m immediate -D ${PGDATA_PATH} > /dev/null 2>&1
pg_rman restore -B ${BACKUP_PATH} -D ${PGDATA_PATH} --quiet;echo $?
sed -i -e "s/^port = .*$/port = ${TEST_PGPORT}/" ${PGDATA_PATH}/postgresql.conf
pg_ctl start -w -t 600 -D ${PGDATA_PATH} > /dev/null 2>&1
get_database_data_from_primary > ${TEST_BASE}/TEST-0003-after.out
diff ${TEST_BASE}/TEST-0003-before.out ${TEST_BASE}/TEST-0003-after.out
echo ''
echo '###### must be primary even if the recovery target is latest ######'
sleep 3  # wait until finishing recovery
psql -p ${TEST_PGPORT} --no-psqlrc -d pgbench -c "SELECT pg_is_in_recovery();"

# check to start as standby if a user configure it manually
init_backup
setup_standby
init_catalog
echo '###### BACKUP COMMAND FROM STANDBY SERVER TEST-0004 ######'
load_with_pgbench
full_backup_from_standby
load_with_pgbench  # this data will be replicated after restoring
get_database_data_from_primary > ${TEST_BASE}/TEST-0004-before.out
pg_ctl stop -m immediate -D ${SBYDATA_PATH} > /dev/null 2>&1
pg_rman restore -B ${BACKUP_PATH} -D ${SBYDATA_PATH} --quiet;echo $?
touch ${SBYDATA_PATH}/standby.signal  # configure for standby server
pg_ctl start -w -t 600 -D ${SBYDATA_PATH} > /dev/null 2>&1
echo ''
echo '###### must be standby and synchronized with the primary server ######'
sleep 3  # wait until finishing to sync
psql -p ${TEST_SBYPGPORT} --no-psqlrc -d pgbench -c "SELECT status FROM pg_stat_wal_receiver;"
get_database_data_from_standby > ${TEST_BASE}/TEST-0004-after.out
diff ${TEST_BASE}/TEST-0004-before.out ${TEST_BASE}/TEST-0004-after.out

# check to fail if "--standby-host" and "--standby-port" are not specified
init_backup
setup_standby
init_catalog
echo '###### BACKUP COMMAND FROM STANDBY SERVER TEST-0005 ######'
pg_rman backup -B ${BACKUP_PATH} -b full -Z -p ${TEST_PGPORT} -d postgres -D ${SBYDATA_PATH}

# clean up the temporal test data
pg_ctl stop -m immediate -D ${PGDATA_PATH} > /dev/null 2>&1
pg_ctl stop -m immediate -D ${SBYDATA_PATH} > /dev/null 2>&1
rm -fr ${PGDATA_PATH}
rm -rf ${SBYDATA_PATH}
rm -fr ${BACKUP_PATH}
rm -fr ${ARCLOG_PATH}
rm -fr ${SRVLOG_PATH}

