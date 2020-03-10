CC := gcc
CFLAGS := -std=gnu18 -g -Wall -Wextra -pedantic -Os -flto# -DDEBUG
LDFLAGS := -l:libzstd.a

all: LDFLAGS := $(LDFLAGS) -pthread
all: ManifestDownloader

mingw: LDFLAGS := $(LDFLAGS) -static -lWs2_32
mingw: CFLAGS := $(CFLAGS) -D__USE_MINGW_ANSI_STDIO
mingw: ManifestDownloader.exe

object_files = general_utils.o rman.o socket_utils.o main.o

general_utils.o: general_utils.c general_utils.h
rman.o: rman.c rman.h defs.h general_utils.h
socket_utils.o: socket_utils.c socket_utils.h rman.h defs.h general_utils.h
main.o: rman.h socket_utils.h defs.h general_utils.h

ManifestDownloader ManifestDownloader.exe: general_utils.o rman.o socket_utils.o main.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@


clean:
	rm -f ManifestDownloader ManifestDownloader.exe $(object_files)
