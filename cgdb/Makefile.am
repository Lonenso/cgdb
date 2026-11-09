DEFS += -DPKGDATADIR=\"$(pkgdatadir)\" -DTOPBUILDDIR=\"$(abs_top_builddir)\"

AM_CXXFLAGS = \
    -I$(top_srcdir)/lib/kui \
    -I$(top_srcdir)/lib/rline \
    -I$(top_srcdir)/lib/util \
    -I$(top_srcdir)/lib/tgdb \
    -I$(top_srcdir)/lib/tokenizer \
    -I$(top_srcdir)/lib/vterm \
    -std=c++11

bin_PROGRAMS = cgdb

cgdb_LDFLAGS = \
    -L$(top_builddir)/lib/kui \
    -L$(top_builddir)/lib/rline \
    -L$(top_builddir)/lib/tokenizer \
    -L$(top_builddir)/lib/util \
    -L$(top_builddir)/lib/vterm \
    -L$(top_builddir)/lib/tgdb

cgdb_LDADD = \
    $(top_builddir)/lib/tgdb/libtgdb.a \
    $(top_builddir)/lib/tokenizer/libtokenizer.a \
    $(top_builddir)/lib/kui/libkui.a \
    $(top_builddir)/lib/rline/librline.a \
    $(top_builddir)/lib/vterm/libcgdbvterm.a \
    $(top_builddir)/lib/util/libcgdbutil.a

noinst_HEADERS = \
    command_lexer.h

cgdb_SOURCES = \
    cgdb.cpp \
    cgdb.h \
    cgdbrc.cpp \
    cgdbrc.h \
    command_lexer.lpp \
    filedlg.cpp \
    filedlg.h \
    highlight.cpp \
    highlight.h \
    highlight_groups.cpp \
    highlight_groups.h \
    interface.cpp \
    interface.h \
    logo.cpp \
    logo.h \
    scroller.cpp \
    scroller.h \
    vterminal.cpp \
    vterminal.h \
    sources.cpp \
    sources.h \
    usage.cpp \
    usage.h

# This executable must not link to msys-2.0.dll: crossing the native process
# boundary is what activates the console before GDB creates its second UI.
EXTRA_DIST = console-host.c
if MSYS_CONSOLE_HOST
nodist_bin_SCRIPTS = cgdb-console-host.exe
all-local: cgdb-console-host.exe
cgdb-console-host.exe: $(srcdir)/console-host.c $(top_srcdir)/lib/util/native_console_protocol.h
	PATH="$(dir $(NATIVE_CC)):$$PATH" $(NATIVE_CC) -O2 -Wall -Wextra -municode -o $@ $<
CLEANFILES = cgdb-console-host.exe
endif
