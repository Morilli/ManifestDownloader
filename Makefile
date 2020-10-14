CC := gcc
ifdef DEBUG
	_DEBUG := -DDEBUG
endif
CFLAGS := -std=gnu18 -g -Wall -Wextra -pedantic -Os -flto -ffunction-sections -fdata-sections $(_DEBUG)
LDFLAGS := -Wl,--gc-sections
target := ManifestDownloader

ifeq ($(OS),Windows_NT)
    LDFLAGS := $(LDFLAGS) pcre2/libpcre2_mingw.a zstd/libzstd_mingw.a bearssl/libbearssl_mingw.a -lws2_32 -static
    target := $(target).exe
else
    LDFLAGS := $(LDFLAGS) -pthread pcre2/libpcre2_linux.a zstd/libzstd_linux.a bearssl/libbearssl_linux.a
endif

all: $(target)
strip: LDFLAGS := $(LDFLAGS) -s
strip: all

object_files = general_utils.o rman.o socket_utils.o download.o main.o sha/sha2.o bearssl/digicert_certificates.o

general_utils.o: general_utils.h defs.h
rman.o: rman.h defs.h list.h
socket_utils.o: socket_utils.h defs.h list.h rman.h
download.o: download.h defs.h general_utils.h list.h rman.h socket_utils.h
main.o: download.h defs.h general_utils.h list.h rman.h socket_utils.h

$(target): $(object_files)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@


clean:
	rm -f ManifestDownloader ManifestDownloader.exe $(object_files)
