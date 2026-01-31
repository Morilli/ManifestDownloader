ifdef DEBUG
    _DEBUG := -DDEBUG
endif
CFLAGS := -std=gnu18 -g -Wall -Wextra -pedantic -Os -flto $(_DEBUG) -Iwolfssl -DWOLFSSL_USE_OPTIONS_H
LDFLAGS := -Wl,--gc-sections
ifneq ($(findstring clang,$(CC)),)
    # lld is required with clang to support flto-compiled object files (bitcode)
	LDFLAGS += -fuse-ld=lld
endif
target := ManifestDownloader

ifeq ($(OS),Windows_NT)
    SUFFIX := _mingw
    LDFLAGS += -lws2_32 -lcrypt32 -Wl,-Bstatic -lpthread
    target := $(target).exe
else
    SUFFIX := _linux
    LDFLAGS += -pthread
endif

all: $(target)
strip: LDFLAGS := $(LDFLAGS) -s
strip: all

.prerequisites_built$(SUFFIX):
ifeq ($(wildcard ./.prerequisites_built$(SUFFIX)),)
	$(MAKE) -C zstd libzstd.a MOREFLAGS="-flto" ZSTD_LIB_MINIFY=1 && \
	mv zstd/libzstd.a libs/libzstd$(SUFFIX).a

	cmake --build pcre2/build > /dev/null 2>&1 || (mkdir -p pcre2/build && rm -rf pcre2/build/* && \
	cmake -S pcre2 -B pcre2/build -G Ninja -DPCRE2_BUILD_TESTS=OFF -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_SUPPORT_UNICODE=OFF -DCMAKE_BUILD_TYPE=MinSizeRel && \
	cmake --build pcre2/build) && \
	cp pcre2/build/interface/pcre2.h libs/ && \
	mv pcre2/build/libpcre2-8.a libs/libpcre2$(SUFFIX).a

	cmake --build wolfssl/build > /dev/null 2>&1 || (mkdir -p wolfssl/build && rm -rf wolfssl/build/* && \
	cmake -S wolfssl -B wolfssl/build -G Ninja -DWOLFSSL_EXAMPLES=no -DWOLFSSL_CRYPT_TESTS=no -DBUILD_SHARED_LIBS=OFF -DWOLFSSL_DH=no \
	    -DWOLFSSL_SHA224=no -DWOLFSSL_SHA3=no -DCMAKE_C_FLAGS="-Os -flto -DNO_WOLFSSL_SERVER" -DCMAKE_BUILD_TYPE=MinSizeRel && \
	cmake --build wolfssl/build) && \
	cp wolfssl/build/wolfssl/options.h wolfssl/wolfssl/ && \
	mv wolfssl/build/libwolfssl.a libs/libwolfssl$(SUFFIX).a

	cmake --build BLAKE3/c/build > /dev/null 2>&1 || (mkdir -p BLAKE3/c/build && rm -rf BLAKE3/c/build/* && \
	cmake -S BLAKE3/c -B BLAKE3/c/build -G Ninja -DCMAKE_BUILD_TYPE=Release && \
	cmake --build BLAKE3/c/build) && \
	mv BLAKE3/c/build/libblake3.a libs/libblake3$(SUFFIX).a

	test -f ./libs/libzstd$(SUFFIX).a && test -f ./libs/libpcre2$(SUFFIX).a && test -f ./libs/libwolfssl$(SUFFIX).a && test -f ./libs/libblake3$(SUFFIX).a && \
	touch .prerequisites_built$(SUFFIX)
endif

object_files = general_utils.o rman.o socket_utils.o download.o main.o sha/sha256.o sha/sha256-x86.o
lib_files = libs/libzstd$(SUFFIX).a libs/libpcre2$(SUFFIX).a libs/libwolfssl$(SUFFIX).a libs/libblake3$(SUFFIX).a

general_utils.o: general_utils.h defs.h
rman.o: rman.h defs.h list.h
socket_utils.o: socket_utils.h defs.h list.h rman.h
download.o: download.h defs.h general_utils.h list.h rman.h socket_utils.h
main.o: download.h defs.h general_utils.h list.h rman.h socket_utils.h
sha/sha256-x86.o: CFLAGS += -O3 -msha -msse4

$(object_files): | .prerequisites_built$(SUFFIX)

$(target): $(object_files)
	$(CC) $(CFLAGS) $^ $(lib_files) $(LDFLAGS) -o $@


clean:
	rm -f $(target) $(object_files)

clean-all: clean
	rm -f .prerequisites_built$(SUFFIX) $(lib_files)
	rm -rf pcre2/build
	rm -rf BLAKE3/c/build
	rm -rf wolfssl/build
	$(MAKE) -C zstd clean ZSTD_LIB_MINIFY=1
