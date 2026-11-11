AUTOMAKE_OPTIONS = dejagnu
EXTRA_DIST = kui.base config msys-stdout.cpp \
    native-windows.exp native-windows-ui.exp native-windows-startup.exp \
    pty-regression.py native-windows.txt source-dialog.exp source-list-fixture.py \
    source-dialog.py source-dialog.txt suspend.py suspend.exp suspend.txt
DEJATOOL = kui.base

check_PROGRAMS = source-list
TESTS = source-list
source_list_SOURCES = source-list.cpp
source_list_CPPFLAGS = -I$(top_srcdir)/lib/tgdb
source_list_CXXFLAGS = -std=c++11
source_list_LDADD = $(top_builddir)/lib/tgdb/libtgdb.a
