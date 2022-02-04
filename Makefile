ifdef DEBUG
    _DEBUG := -DDEBUG
endif
CFLAGS := -std=gnu18 -g -Wall -Wextra -pedantic -Os -flto $(_DEBUG)
LDFLAGS := -Wl,--gc-sections
target := ManifestDownloader

ifeq ($(OS),Windows_NT)
    SUFFIX := _mingw
    CONF := Mingw
    LDFLAGS += -lws2_32 -static
    target := $(target).exe
else
    SUFFIX := _linux
    CONF := Unix
    LDFLAGS += -pthread
endif

ifneq ($(findstring mingw32-make,$(MAKE)),)
    CMAKE_GENERATOR := MinGW Makefiles
else
    CMAKE_GENERATOR := Unix Makefiles
endif

all: $(target)
strip: LDFLAGS := $(LDFLAGS) -s
strip: all

.prerequisites_built$(SUFFIX):
ifeq ($(wildcard ./.prerequisites_built$(SUFFIX)),)
	$(MAKE) -C zstd libzstd.a MOREFLAGS="-flto" ZSTD_LIB_MINIFY=1 && \
	mv zstd/libzstd.a libs/libzstd$(SUFFIX).a

	cmake --build pcre2/build > /dev/null 2>&1 || (mkdir -p pcre2/build && rm -rf pcre2/build/* && \
	cmake -S pcre2 -B pcre2/build -G "$(CMAKE_GENERATOR)" -DPCRE2_BUILD_TESTS=OFF -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_SUPPORT_UNICODE=OFF -DCMAKE_C_FLAGS="-Os" && \
	cmake --build pcre2/build) && \
	mv pcre2/build/libpcre2-8.a libs/libpcre2$(SUFFIX).a

	$(MAKE) -C BearSSL CONF=$(CONF)

	test -f ./libs/libzstd$(SUFFIX).a && test -f ./libs/libpcre2$(SUFFIX).a && test -f ./libs/libbearssl$(SUFFIX).a && \
	touch .prerequisites_built$(SUFFIX)
endif

object_files = general_utils.o rman.o socket_utils.o download.o main.o sha/sha256.o sha/sha256-x86.o BearSSL/root_certificates.o
lib_files = libs/libzstd$(SUFFIX).a libs/libpcre2$(SUFFIX).a libs/libbearssl$(SUFFIX).a

general_utils.o: general_utils.h defs.h
rman.o: rman.h defs.h list.h
socket_utils.o: socket_utils.h defs.h list.h rman.h BearSSL/trust_anchors.h
download.o: download.h defs.h general_utils.h list.h rman.h socket_utils.h BearSSL/trust_anchors.h
main.o: download.h defs.h general_utils.h list.h rman.h socket_utils.h
sha/sha256-x86.o: CFLAGS += -O3 -msha -msse4

$(target): .prerequisites_built$(SUFFIX) $(object_files)
	$(CC) $(CFLAGS) $^ $(lib_files) $(LDFLAGS) -o $@


clean:
	rm -f $(target) $(object_files)

clean-all: clean
	rm -f .prerequisites_built$(SUFFIX) $(lib_files)
	rm -rf pcre2/build
	$(MAKE) -C BearSSL clean CONF=$(CONF)
	$(MAKE) -C zstd clean ZSTD_LIB_MINIFY=1
