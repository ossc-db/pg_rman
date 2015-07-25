#
# pg_rman: Makefile
#
#  Portions Copyright (c) 2008-2015, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
#
PROGRAM = pg_rman
SRCS = \
	backup.c \
	catalog.c \
	data.c \
	delete.c \
	dir.c \
	init.c \
	parray.c \
	pg_rman.c \
	restore.c \
	show.c \
	util.c \
	validate.c \
	xlog.c \
	controlfile.c \
	pgsql_src/pg_ctl.c \
	pgsql_src/pg_crc.c \
	pgut/pgut.c \
	pgut/pgut-port.c
OBJS = $(SRCS:.c=.o)
# pg_crc.c and are copied from PostgreSQL source tree.

# XXX for debug, add -g and disable optimization
PG_CPPFLAGS = -I$(libpq_srcdir) -lm
PG_LIBS = $(libpq_pgport)

REGRESS = init option show delete purge backup backup_management restore backup_from_standby

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# remove dependency to libxml2 and libxslt
LIBS := $(filter-out -lxml2, $(LIBS))
LIBS := $(filter-out -lxslt, $(LIBS))

$(OBJS): pg_rman.h

installcheck: myinstallcheck

myinstallcheck:
	@if [ `expr "$(MAJORVERSION) < 9.0" | bc` -eq 1 ]; \
	 then \
		sed -i 's/^wal_level/#wal_level/g' sql/init.sh; \
		sed -i 's/^wal_level/#wal_level/g' sql/option.sh; \
		sed -i 's/^wal_level/#wal_level/g' sql/show.sh; \
		sed -i 's/^wal_level/#wal_level/g' sql/delete.sh; \
		sed -i 's/^wal_level/#wal_level/g' sql/purge.sh; \
		sed -i 's/^wal_level/#wal_level/g' sql/backup.sh; \
		sed -i 's/^wal_level/#wal_level/g' sql/backup_management.sh; \
		sed -i 's/^wal_level/#wal_level/g' sql/restore.sh; \
		sed -i 's/^wal_level/#wal_level/g' sql/backup_from_standby.sh; \
	 fi
	@if [ `expr "$(MAJORVERSION) < 9.1" | bc` -eq 1 ]; \
	 then \
		sed -i 's/^synchronous_standby_names/#synchronous_standby_names/g' sql/backup_from_standby.sh; \
	 fi

clean: myclean

myclean:
	-@sed -i 's/^#wal_level/wal_level/g' sql/init.sh;
	-@sed -i 's/^#wal_level/wal_level/g' sql/option.sh;
	-@sed -i 's/^#wal_level/wal_level/g' sql/show.sh;
	-@sed -i 's/^#wal_level/wal_level/g' sql/delete.sh;
	-@sed -i 's/^#wal_level/wal_level/g' sql/purge.sh;
	-@sed -i 's/^#wal_level/wal_level/g' sql/backup.sh;
	-@sed -i 's/^#wal_level/wal_level/g' sql/backup_management.sh;
	-@sed -i 's/^#wal_level/wal_level/g' sql/restore.sh;
	-@sed -i 's/^#wal_level/wal_level/g' sql/backup_from_standby.sh;
	-@sed -i 's/^#synchronous_standby_names/synchronous_standby_names/g' sql/backup_from_standby.sh;
