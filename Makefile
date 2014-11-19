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
	pgut/pgut.c \
	pgut/pgut-port.c
OBJS = $(SRCS:.c=.o)
# pg_crc.c and are copied from PostgreSQL source tree.

# XXX for debug, add -g and disable optimization
PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport)

REGRESS = option init show_validate backup_restore backup_restore_checksum

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
		sed -i 's/^wal_level/#wal_level/g' sql/backup_restore.sh; \
	 fi

clean: myclean

myclean:
	-@sed -i 's/^#wal_level/wal_level/g' sql/backup_restore.sh;
