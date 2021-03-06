CC=clang
CXX=clang++

CURRENT_DIR=$(shell pwd)
TBB_LIBDIR=$(wildcard $(CURRENT_DIR)/oneTBB/build/linux_intel64_*_release/)
MALLOC_LIBDIR=$(CURRENT_DIR)/mimalloc/out/release

CPPFLAGS=-g -IoneTBB/include -pthread -std=c++20 -Wno-deprecated-volatile \
         -Wno-switch -O2
LDFLAGS=-L$(TBB_LIBDIR) -Wl,-rpath=$(TBB_LIBDIR) \
        -L$(MALLOC_LIBDIR) -Wl,-rpath=$(MALLOC_LIBDIR)
LIBS=-lcrypto -pthread -ltbb -lmimalloc -lz
OBJS=main.o object_file.o input_sections.o output_chunks.o mapfile.o perf.o \
     linker_script.o archive_file.o output_file.o subprocess.o gc_sections.o \
     icf.o symbols.o commandline.o

mold: $(OBJS)
	$(CXX) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

$(OBJS): mold.h elf.h Makefile

submodules:
	$(MAKE) -C oneTBB
	mkdir -p mimalloc/out/release
	(cd mimalloc/out/release; cmake ../..)
	$(MAKE) -C mimalloc/out/release

test: mold
	for i in test/*.sh; do $$i || exit 1; done

clean:
	rm -f *.o *~ mold

.PHONY: intel_tbb test clean
