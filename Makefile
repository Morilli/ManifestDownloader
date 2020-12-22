ifdef DEBUG
    _DEBUG := -DDEBUG
endif
CFLAGS := -std=gnu18 -g -Wall -Wextra -pedantic -Os -flto -ffunction-sections -fdata-sections $(_DEBUG)
LDFLAGS := -Wl,--gc-sections
target := ManifestDownloader

ifeq ($(OS),Windows_NT)
    SUFFIX := _mingw
    CONF := mingw
    LDFLAGS += -lws2_32 -static
    target := $(target).exe
else
    SUFFIX := _linux
    CONF := unix
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

	mkdir -p pcre2/build && cd pcre2/build && rm -rf * && \
	cmake .. -G "$(CMAKE_GENERATOR)" -DPCRE2_BUILD_TESTS=OFF -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_SUPPORT_UNICODE=OFF -DCMAKE_C_FLAGS="-Os" && \
	cmake --build . && \
	mv libpcre2-8.a ../../libs/libpcre2$(SUFFIX).a

	$(MAKE) -C bearssl CONF=$(CONF)

	test -f ./libs/libzstd$(SUFFIX).a && test -f ./libs/libpcre2$(SUFFIX).a && test -f ./libs/libbearssl$(SUFFIX).a && \
	touch .prerequisites_built$(SUFFIX)
endif

object_files = general_utils.o rman.o socket_utils.o download.o main.o sha/sha2.o bearssl/digicert_certificates.o
lib_files = libs/libzstd$(SUFFIX).a libs/libpcre2$(SUFFIX).a libs/libbearssl$(SUFFIX).a

general_utils.o: general_utils.h defs.h
rman.o: rman.h defs.h list.h
socket_utils.o: socket_utils.h defs.h list.h rman.h
download.o: download.h defs.h general_utils.h list.h rman.h socket_utils.h
main.o: download.h defs.h general_utils.h list.h rman.h socket_utils.h

$(target): .prerequisites_built$(SUFFIX) $(object_files)
	$(CC) $(CFLAGS) $^ $(lib_files) $(LDFLAGS) -o $@


clean:
	rm -f $(target) $(object_files)

clean-all: clean
	rm -f .prerequisites_built$(SUFFIX) $(lib_files)
	rm -rf pcre2/build
	$(MAKE) -C BearSSL clean CONF=$(CONF)
	$(MAKE) -C zstd clean ZSTD_LIB_MINIFY=1
