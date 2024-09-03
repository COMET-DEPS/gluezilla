# BUILD
git clone https://github.com/coreutils/coreutils.git
cd coreutils
git checkout v9.0
./bootstrap
CC=/abs/path/to/compiler_build/bin/clang CFLAGS=-flto ./configure
make -j10
git apply ../<the_program>.patch
# note: ignore "warning: unable to rmdir 'gnulib': Directory not empty"
# in Makefile: add `src/remove.c` to src_cp_SOURCES, add `src/remove.$(OBJEXT)` to src_cp_OBJECTS (TODO add to patch)
# --- 1. build for target offset selection ---
# in Makefile: add to LDFLAGS (i do this here bcs i cannot find how to disable the compiler checks performed by the configure script) `-flto -fuse-ld=/abs/path/to/compiler_build/bin/ld.lld -L/abs/path/to/libloader.so_dir -lloader -Wl,-T,/abs/path/to/compiler/linker_script.ld,-rpath=.,-mllvm,-TO=../target_offsets.toml,-mllvm,-VA=../victim_addresses.txt,-mllvm,-dbl_output=/abs/path/to/output_dir,-mllvm,-dbl_mode=offsets`
make ./src/<the_program>
objdump -drwCs src/<the_program> > <the_program>.dump
# get targets and put in ../target_offsets.toml (also make sure ../victim_addresses.txt exists)
# --- 2. build final binary ---
# in MakeFile: change `-dbl_mode=offsets` to `-dbl_mode=dbl` in LDFLAGS
make src/<the_program>
# generate attack_config.toml