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

function init_database()
{
    rm -rf ${PGDATA_PATH}
    rm -fr ${ARCLOG_PATH}
    rm -fr ${SRVLOG_PATH}
    rm -fr ${TBLSPC_PATH}
    mkdir -p ${ARCLOG_PATH}
    mkdir -p ${SRVLOG_PATH}
    mkdir -p ${TBLSPC_PATH}

    # create new database cluster
    initdb ${USE_DATA_CHECKSUM} --no-locale -D ${PGDATA_PATH} > ${TEST_BASE}/initdb.log 2>&1
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
    pg_ctl start -D ${PGDATA_PATH} -w -t 300 > ${TEST_BASE}/pg_ctl.log 2>&1
	mkdir -p ${TBLSPC_PATH}/pgbench
	psql --no-psqlrc -p ${TEST_PGPORT} -d postgres > /dev/null 2>&1 << EOF
CREATE TABLESPACE pgbench LOCATION '${TBLSPC_PATH}/pgbench';
CREATE DATABASE pgbench TABLESPACE = pgbench;
EOF

    pgbench -i -s ${SCALE} -p ${TEST_PGPORT} -d pgbench > ${TEST_BASE}/pgbench.log 2>&1

}

function init_catalog()
{
    rm -fr ${BACKUP_PATH}
    pg_rman init -B ${BACKUP_PATH} --quiet
}

cleanup
init_database
init_catalog

echo '###### BACKUP COMMAND TEST-0001 ######'
echo '###### full backup mode ######'
pg_rman backup -B ${BACKUP_PATH} -b full -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0001.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0001.log

echo '###### BACKUP COMMAND TEST-0002 ######'
echo '###### incremental backup mode ######'
pg_rman backup -B ${BACKUP_PATH} -b incremental -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0002.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0002.log

echo '###### BACKUP COMMAND TEST-0003 ######'
echo '###### archive backup mode ######'
pg_rman backup -B ${BACKUP_PATH} -b archive -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0003.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0003.log

echo '###### BACKUP COMMAND TEST-0004 ######'
echo '###### full backup with server log ######'
init_catalog
pg_rman backup -B ${BACKUP_PATH} -b full -s -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0004.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0004.log

# If a differential backup was taken, the size of third backup is same as the second one and the size is bigger (XXMB).
# 16kB is the base backup size if nothing was changed in the database cluster.
echo '###### BACKUP COMMAND TEST-0005 ######'
echo '###### Make sure that pg_rman does not take a differential backup, but a incremental backup ######'
init_catalog
pg_rman backup -B ${BACKUP_PATH} -b full -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
psql -p ${TEST_PGPORT} -d postgres -c 'create table test (c1 int);'
psql -p ${TEST_PGPORT} -d postgres -c 'insert into test values(generate_series(1,1000000));' 
pg_rman backup -B ${BACKUP_PATH} -b incremental -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman backup -B ${BACKUP_PATH} -b incremental -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0012.log 2>&1
cat ${TEST_BASE}/TEST-0012.log  | head -n 4 | tail -n 1 | awk '{print $5, $6, $13}' 

echo '###### BACKUP COMMAND TEST-0006 ######'
echo '###### full backup with compression ######'
init_catalog
pg_rman backup -B ${BACKUP_PATH} -b full -s -Z -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0005.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0005.log
grep OK ${TEST_BASE}/TEST-0005.log | grep -c true

echo '###### BACKUP COMMAND TEST-0007 ######'
echo '###### full backup with smooth checkpoint ######'
init_catalog
pg_rman backup -B ${BACKUP_PATH} -b full -s -C -p ${TEST_PGPORT} -d postgres --quiet;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0006.log 2>&1
grep -c OK ${TEST_BASE}/TEST-0006.log

echo '###### BACKUP COMMAND TEST-0008 ######'
echo '###### switch backup mode from incremental to full ######'
init_catalog
echo 'incremental backup without validated full backup'
pg_rman backup -B ${BACKUP_PATH} -b incremental -s -Z -p ${TEST_PGPORT} -d postgres;echo $?
sleep 1
echo 'incremental backup in the same situation but with --full-backup-on-error option'
pg_rman backup -B ${BACKUP_PATH} -b incremental -F -s -Z -p ${TEST_PGPORT} -d postgres;echo $?
pg_rman validate -B ${BACKUP_PATH} --quiet
pg_rman show detail -B ${BACKUP_PATH} > ${TEST_BASE}/TEST-0010.log 2>&1
grep OK ${TEST_BASE}/TEST-0010.log | grep FULL | wc -l
grep ERROR ${TEST_BASE}/TEST-0010.log | grep INCR | wc -l

echo '###### BACKUP COMMAND TEST-0009 ######'
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

echo '###### BACKUP COMMAND TEST-0010 ######'
echo '###### failure in backup with different system identifier database ######'
init_catalog
pg_ctl stop -m immediate > /dev/null 2>&1
init_database
pg_rman backup -B ${BACKUP_PATH} -b full -p ${TEST_PGPORT} -d postgres --quiet;echo $?


# cleanup
## clean up the temporal test data
pg_ctl stop -m immediate > /dev/null 2>&1
rm -fr ${PGDATA_PATH}
rm -fr ${BACKUP_PATH}
rm -fr ${ARCLOG_PATH}
rm -fr ${SRVLOG_PATH}
rm -fr ${TBLSPC_PATH}
