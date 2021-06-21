pg_rman
=======
pg_rman is an online backup and restore tool for PostgreSQL.

The goal of the pg_rman project is to provide a method for online backup
and PITR that is as easy as pg_dump. Also, it maintains a backup catalog
per database cluster. Users can maintain old backups including archive
logs with one command.

Branches
--------
There are several branches within pg_rman repository in order to work with
different PostgreSQL server versions without introducing server version
check code blocks.  Please choose a branch to match the PostgreSQL version
you will be building pg_rman against.

* master : branch for PostgreSQL 14beta [![Test](https://github.com/ossc-db/pg_rman/actions/workflows/build.yml/badge.svg?branch=master&event=push)](https://github.com/ossc-db/pg_rman/actions/workflows/build.yml)
* REL_13_STABLE : branch for PostgreSQL 13 [![Test](https://github.com/ossc-db/pg_rman/actions/workflows/build.yml/badge.svg?branch=REL_13_STABLE&event=push)](https://github.com/ossc-db/pg_rman/actions/workflows/build.yml)
* REL_12_STABLE : branch for PostgreSQL 12 [![Test](https://github.com/ossc-db/pg_rman/actions/workflows/build.yml/badge.svg?branch=REL_12_STABLE&event=push)](https://github.com/ossc-db/pg_rman/actions/workflows/build.yml)
* REL_11_STABLE : branch for PostgreSQL 11 [![Test](https://github.com/ossc-db/pg_rman/actions/workflows/build.yml/badge.svg?branch=REL_11_STABLE&event=push)](https://github.com/ossc-db/pg_rman/actions/workflows/build.yml)
* REL_10_STABLE : branch for PostgreSQL 10 [![Test](https://github.com/ossc-db/pg_rman/actions/workflows/build.yml/badge.svg?branch=REL_10_STABLE&event=push)](https://github.com/ossc-db/pg_rman/actions/workflows/build.yml)
* REL9_6_STABLE : branch for PostgreSQL 9.6 [![Test](https://github.com/ossc-db/pg_rman/actions/workflows/build.yml/badge.svg?branch=REL9_6_STABLE&event=push)](https://github.com/ossc-db/pg_rman/actions/workflows/build.yml)

How to use
----------

To take an online backup, use the `backup` command:

````
$ pg_rman backup --backup-mode=full --with-serverlog
INFO: copying database files
INFO: copying archived WAL files
INFO: copying server log files
INFO: backup complete
INFO: Please execute 'pg_rman validate' to verify the files are correctly copied.
````

To list all the backups taken so far, use the `show` command:

````
$ pg_rman show
 =====================================================================
 StartTime            EndTime              Mode    Size   TLI  Status
 =====================================================================
 2015-03-27 14:59:47  2015-03-27 14:59:49  FULL  3404kB     3  OK
 2015-03-27 14:59:19  2015-03-27 14:59:20  ARCH    26kB     3  OK
 2015-03-27 14:59:00  2015-03-27 14:59:01  ARCH    26kB     3  OK
 2015-03-27 14:58:46  2015-03-27 14:58:48  FULL  3516kB     3  OK
 2015-03-27 11:43:31  2015-03-27 11:43:32  INCR    54kB     1  OK
 2015-03-27 11:43:19  2015-03-27 11:43:20  INCR    69kB     1  OK
 2015-03-27 11:43:04  2015-03-27 11:43:05  INCR   151kB     1  OK
 2015-03-27 11:42:56  2015-03-27 11:42:56  INCR    96kB     1  OK
 2015-03-27 11:34:55  2015-03-27 11:34:58  FULL  5312kB     1  OK
````

To restore from a backup, use the `restore` command.  Up to PostgreSQL11, note that pg_rman itself generates the `recovery.conf` file required to perform PostgreSQL PITR.

````
$ pg_ctl stop -m immediate
$ pg_rman restore
$ cat $PGDATA/recovery.conf
# recovery.conf generated by pg_rman 1.3.12
restore_command = 'cp /home/postgres/arclog/%f %p'
recovery_target_timeline = '1'
$ pg_ctl start
````

After to PostgreSQL12, note that pg_rman itself added PostgreSQL PITR related options to `postgresql.conf` file and generates the `recovery.signal` file in sub directory of PGBASE

To see more options to use with each command, run `pg_rman --help`.

Also, see the documentation for detailed usage:

http://ossc-db.github.io/pg_rman/index.html

How to build and install from source code
-----------------------------------------
Go to the top directory of pg_rman source tree and run the following commands:

````
 $ make
 # make install
````

The following packages need to be installed as a prerequisite.

* zlib-devel


How to run regression tests
---------------------------
Start PostgreSQL server and run the below command.

````
 $ make installcheck
````
