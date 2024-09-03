# Introduction
This organization contains the implementation of GlueZilla.
In GlueZilla, we protect a program against unauthorized deployment and reverse engineering by associating every software instance to a single hardware instance using the rowhammer effect.
You can find more details in our [paper](https://link.springer.com/chapter/10.1007/978-3-031-64171-8_22).

GlueZilla consists of multiple components:
- **gluezilla-templater** in a [separate repo](https://github.com/COMET-DEPS/gluezilla-templater): the templater tests the DRAM module for rowhammer-susceptible cells using different rowhammer patterns.
- **gluezilla-compiler** in the [`compiler` directory](compiler): the compiler compiles the protected program and matches the preselected junction bits with a rowhammer-susceptible cell discoved by the templater.
Because the bit flips distribution in a DRAM module is rather sparse, the final binary has a custom binary layout to make sure every junction bit lands in a rowhammer-susceptible cell.
- **gluezilla-loader** in the [`loader` directory](loader): the loader component loads the protected binary in physical memory according to the loader map produced by the gluezilla-compiler.
This component is built using the reusable code in the [`rowhammer_tools` directory](rowhammer_tools).
This directory also contains some small supporting tools for rowhammer-related experiments.
- Some overarching scripts, tests, and documentation:
    - The [`scripts` directory](scripts) contains several helper scripts to generate input files, convert between formats, etc.
    At the top of each script is a comment about its usage.
    - The [`docs` directory](docs) contains examples and explanations for most configuration and input files.
    - The [`test_code` directory](test_code) contains the setup for our evaluation on `coreutils` and `SPEC`.

The remaining of this README applies to this repo only.
See the [gluezilla-templater repo](https://github.com/COMET-DEPS/gluezilla-templater) for more info about the templater component.

# Terminology & Conventions
- "victim XXXX" refers to the XXXX containing the rowhammer-susceptible location in physical memory, e.g. a victim frame contains a victim byte, which contains a victim bit that we can flip using rowhammer.
- "target XXXX" refers to the XXXX containing the bit we want to get flipped in the binary, e.g. a target binary contains a target page, which contains a target byte, which contains a target bit. In our paper, we refer to this as the "junction bit".
- "page" is a page in virtual memory.
- "page frame" is a page frame in physical memory.
- The bit flip direction is usually indicated by '+' for a 0->1 flip and '-' for a 1->0 flip.
- Bit and byte indices start at 0 with the least significant bit/byte.

# Build
## Prerequisites
- Tested on Fedora 40
- Build dependencies:
    - `cmake, ninja, clang, binutils-dev, llvm`
    - A [Rust toolchain](https://www.rust-lang.org/) is required for the `loader` component (`Stable` channel is sufficient)
- Run-time dependencies: `decode-dimms, dmidecode, msr-tools`
- Python libraries for the scripts (python3): `pyelftools, pysqlite3, numpy, capstone, tomlkit==0.11.6, pathlib`

## Build
1. Build the compiler by running `./build.sh` inside the `compiler` directory.
1. Build the supporting rowhammer tools by running `cargo build --release` inside the `rowhammer_tools` directory.
1. Build the loader by running `cargo build --release` inside the `loader` directory.
This creates the `libloader.so` library in `target/release` against which the protected program is linked.

# Workflow
**Note:** to deduce noise factors, you should run these tools pinned (e.g., using `taskset -c <core_id>`) to an isolated core (e.g., using `isolcpus=<core_id>` in `/etc/default/grub` (remake grub config!)), disable SMT (e.g., in BIOS), and set a fixed core frequency (e.g., using `/sys` pseudo-file interface).
[This document](workflow_visualization.pdf) visualizes the workflow (credits to Mathéo Vergnolle).

1. Template the memory and create a `victim_addresses.txt` file.
    - Using the [gluezilla-templater](https://github.com/COMET-DEPS/gluezilla-templater):
        1. Run the templater to create an SQLite database with the discoved vulnerable locations.
        1. Create/modify the filter query `query.sql` and run `./db2victim_addresses.py` (from inside the `scripts` directory).

    - Using the small `templater` tool in `rowhammer_tools`:
        1. Simply copy the `target/release/templater` binary to your rowhammer machine
        1. Create a `templater_config.toml` file and a`dram_info.toml` or `dram_config.toml` file in the same directory (see the examples in [`docs`](docs))
        1. `sudo ./templater <id>`
        1. Use `json2victim_addresses.py` to filter the template by the number of times a flip occurred, and to create the `victim_addresses.txt` file

1. Compile the source code using the gluezilla-compiler (`compiler/compiler_build/bin/clang`) with the following compiler options:

        -flto (put in CFLAGS and LDFLAGS)
        -fuse-ld=</abs/path/to/compiler_build/bin/ld.lld>
        -L</abs/path/to/libloader.so_dir> -lloader
        -Wl,-T,</abs/path/to/compiler/linker_script.ld>,-rpath=<path/to/runtime/libloader.so_dir>,-mllvm,-VA=<path/to/victim_addresses.txt>,-mllvm,-TO=<path/to/target_offsets.toml>,-mllvm,-dbl_output=<path/to/output_dir/for/compiler_info>,-mllvm,-dbl_mode=offsets

    The `offsets` mode (-dbl_mode=offsets) compiles the provided source code **without** modifying the layout.
    It does however have some DBL effects (e.g. it separates the code into special sections, changes some parts of the compilation process,...).

    **Notes:**
    - `VA` = Victim Addresses ; `TO` = Target Offsets
    - The `VA`, `TO`, `dbl_output` options are ignored in the `offsets` mode.
    You can drop them for now.
    - There is also a `baseline` mode to compile with no DBL effects, used to create a benchmark baseline.
    For a clean comparison with the `dbl` mode version, you should still compile the baseline with LTO and use lld with the custom linker script though.
    When no `-dbl_mode` option is provided, it defaults to `baseline`.
    - You can add `,-mllvm,-debug-only=dbl_trace` to the linker flags to print debug info at compile time.
    - You can add the `,-mllvm,-dbl_id=<ID>` option to add an ID to the output files created by the compiler.
    This ID is also passed on by the `generate_attack_config.py` script.

1. Disassemble the resulting binary with

        objdump -drwC binary_name > binary_name.dump

    to select the target bits (paper: junction bits) you want to flip.
    Those targets should be put per section in a `target_offsets.toml` file (see the [example](docs/example_target_offsets.toml)).
    **This is the only file you have to create by hand.**
    For now, only bitflips in the `.dbl_text` section are supported!

1. Recompile the program but change the `-dbl_mode` option to `dbl` and make sure the `VA`, `TO` and `dbl_output` are set correctly this time.
In this step, the compiler will emit the final binary with the required custom layout along with loader info in the `compiler_output.txt` file in the directory given by `dbl_output` (make sure this directory exists).

    **Note:** other compiler options you added on your own (opt level, ...), should be the same for the run in `offsets` mode and `dbl` mode!

1. Run `generate_attack_config.py` to generate an `attack_config.toml` file (paper: the loader map) for the loader component.

1. Copy the created binary, `libloader.so` and `attack_config.toml` to your associated rowhammer machine for which the RowHammer template database was created.
`libloader.so` should be in the assigned `rpath`, and `attack_config.toml` should be in the same directory as the created binary.

1. Run the target binary and enjoy your bit flips.

    Control the log level of the `loader` component at run time with the RUST_LOG environment variable.
    Default is `info`.

        sudo RUST_LOG=trace ./binary


# Evaluation
## SPEC CPU 2017
We measure the run-time overhead introduced by the custom binary layout when using a large number of target bits.
We do not need to flip these bits at run time to measure the layout overhead.
We, therefore, use `generate_random_target_offsets.py` in the scripts directory to generate random target bits for a binary compiled in `offsets` mode.
By not flipping the random target bits, we keep the binary functional.

See the [SPEC documentation](https://www.spec.org/cpu2017/Docs/) and the [provided SPEC config file](test_code/SPEC2017/config/test1.cfg) for build and run info.
Create a `DBL` directory in the SPEC root directory containing the `target_offsets_<benchmark_name_EXEBASE>.toml` files, the `victim_addresses.txt` file and `libloader.so`.

To run the benchmark programs on a machine without flipping bits, see the `Debugging` section below to create versions without the `loader` component.

**Notes**:
- We do not support C++ benchmarks with exceptions.
- Gluezilla is only tested on the following benchmarks: 500.perlbench_r, 502.gcc_r, 505.mcf_r, 525.x264_r, 531.deepsjeng, 557.xz_r, 508.namd_r, 519.lbm_r, 538.imagick_r, 544.nab_r.
- The x264 benchmark has 2 extra binaries: imagevalidate_525 and ldecod_r.
These are used for setup and post-validation and are not part of the actual performance benchmark.
So it doesn't matter whether they run with a DBL config or not.

## GNU coreutils
Used for functionality evaluation.
This directory contains patches for the `cp` and `ls` programs.
Each patch transforms the source code into its unintentional form.
This form's binary representation differs only in a small number of bits from the intended form.
At startup time, these bits are flipped and the intended behavior is reconstructed on the associated machine.
See the README.txt for build instructions.

# Debugging
- Do not compile with debug info! The debug info could/will be wrong and it will confuse you!
- I advise you to not use a debugger on the fully rowhammered programs.
If you want to debug the emitted program, create a version that does not rely on the `loader` component to load the binary:
    - Run `toggle_segment_type.py` to change the segment type to `PT_LOAD` instead of `PT_NULL` (or directly change it in `linker_script.ld`)
    - Set the environment variable `RH_TEST` to an arbitrary value.
    This deactivates the `loader` component.
