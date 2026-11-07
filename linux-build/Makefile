CROSS   := i686-w64-mingw32-
CC      := $(CROSS)g++
CFLAGS  := -Os -s -static-libgcc -static-libstdc++
OBJ     := papytelemapp_renamed2.obj
COMPAT  := msvc_compat.cpp msvc_stubs.s
LIBS    := -lwinmm

all: tsample.exe tsample_csv.exe

tsample.exe: tsample.cpp $(OBJ) $(COMPAT)
	$(CC) $(CFLAGS) -o $@ $< $(OBJ) $(COMPAT) $(LIBS)

tsample_csv.exe: tsample.cpp $(OBJ) $(COMPAT)
	$(CC) $(CFLAGS) -DFORMAT_CSV -o $@ $< $(OBJ) $(COMPAT) $(LIBS)

static: tsample_static.exe tsample_csv_static.exe

tsample_static.exe: tsample.cpp $(OBJ) $(COMPAT)
	$(CC) $(CFLAGS) -static -o $@ $< $(OBJ) $(COMPAT) $(LIBS)

tsample_csv_static.exe: tsample.cpp $(OBJ) $(COMPAT)
	$(CC) $(CFLAGS) -static -DFORMAT_CSV -o $@ $< $(OBJ) $(COMPAT) $(LIBS)

.PHONY: regenerate-obj
# Regenerate renamed obj from the original MSVC .obj
# Requires: original papytelemapp.obj in this directory + redef.txt
regenerate-obj:
	# Drop the MSVC .drectve section: it carries MSVC /DEFAULTLIB directives
	# and is not NUL-terminated, which makes MinGW's linker warn about a
	# "corrupt .drectve at end of def file".  MinGW does not need it.
	$(CROSS)objcopy --remove-section=.drectve \
		--redefine-syms=redef.txt papytelemapp.obj papytelemapp_renamed2.obj

clean:
	rm -f tsample.exe tsample_csv.exe tsample_static.exe tsample_csv_static.exe
