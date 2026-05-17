# KVMapper - Windows native Makefile (mingw-w64)
#
# Targets:
#   make all     - build x64 exe + DLL + x86 DLL (default)
#   make x64     - x64 exe and DLL only
#   make x86     - x86 DLL only
#   make clean   - remove build artifacts
#
# Cross-compile from Linux via zig: use ./build-all.sh instead; this
# Makefile assumes a native mingw-w64 toolchain on Windows (the path
# users on a real Windows dev box take).

CC64    = gcc
CC32    = gcc -m32
WINDRES = windres

CFLAGS  = -O2 -Wall -Wextra -std=c99 -Iinclude -DUNICODE -D_UNICODE
LD_EXE  = -mwindows -lcomctl32 -lshell32 -luser32 -lgdi32 -lole32
LD_DLL  = -shared -luser32 -lkernel32

EXE_SRC  = src/main.c src/capture.c src/config.c src/shared_mem.c src/classifier.c src/icon_data.c
DLL_SRC  = src/hook/hook.c src/hook/inject.c

DIST     = dist
$(DIST):
	mkdir -p $(DIST)

all: $(DIST) $(DIST)/kvmapper.exe $(DIST)/kvmapper_hook.dll $(DIST)/kvmapper_hook_x86.dll
	cp app.manifest $(DIST)/kvmapper.exe.manifest

x64: $(DIST) $(DIST)/kvmapper.exe $(DIST)/kvmapper_hook.dll

x86: $(DIST) $(DIST)/kvmapper_hook_x86.dll

$(DIST)/res.o: app.rc app.manifest assets/tray.ico
	$(WINDRES) -O coff app.rc -o $@

$(DIST)/kvmapper.exe: $(EXE_SRC) $(DIST)/res.o include/mapping_defs.h
	$(CC64) $(CFLAGS) -o $@ $(EXE_SRC) $(DIST)/res.o $(LD_EXE)

$(DIST)/kvmapper_hook.dll: $(DLL_SRC) include/mapping_defs.h
	$(CC64) $(CFLAGS) -o $@ $(DLL_SRC) $(LD_DLL)

$(DIST)/kvmapper_hook_x86.dll: $(DLL_SRC) include/mapping_defs.h
	$(CC32) $(CFLAGS) -o $@ $(DLL_SRC) $(LD_DLL)

clean:
	rm -rf $(DIST) *.o *.obj *.res *.res.o

.PHONY: all x64 x86 clean
