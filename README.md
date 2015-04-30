pg_rman
=======
pg_rman is an online backup and restore tool for PostgreSQL.

The goal of the pg_rman project is providing a method for
online backup and PITR as easy as pg_dump. Also, it maintains
a backup catalog per database cluster. Users can maintain old
backups including archive logs with one command.

Branches
--------
There are several branches on pg_rman in order to work with
different PostgreSQL server versions without introducing
server version check code blocks.
Please choose branch with PostgreSQL version you use.

* master : branch for latest PostgreSQL develop version
* REL9_4_STABLE : branch for PostgreSQL 9.4
* REL9_3_STABLE : branch for PostgreSQL 9.3
* REL9_2_STABLE : branch for PostgreSQL 9.2
* pre-9.2 : branch for PostgreSQL 9.1 and before

How to use
----------
See doc/index.html.

How to build and install from source code
-----------------------------------------
Change directory into top directory of pg_rman sorce codes and
run the below commands.

````
 $ make
 # make install
````

How to run regression tests
---------------------------
Start PostgreSQL server and run the below command.

````
 $ make installcheck
````

Bug report
----------

https://sourceforge.net/p/pg-rman/tickets/




