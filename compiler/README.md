# GlueZilla-Compiler
The compiler compiles the protected program and matches the preselected junction bits (target bits) with a rowhammer-susceptible cell discoved by the templater. 
Because the bit flips distribution in a DRAM module is rather sparse, the final binary has a custom binary layout to make sure every target bit in the code lands in a rowhammer-susceptible cell.
The mapping of which virtual page has to reside in which physical page frame is emitted in a `compiler_output.txt` file. 
This file is later read by the `generate_attack_config.py` script to generate the loader map. 

The compiler performs two assembly rounds: 
- ROUND 1: in the first round it runs a normal binary code emission to find which instructions contain the target bytes specified in `target_offsets.toml` and tag the corresponding C++ objects.
- ROUND 2: in the second round, it discards the emitted binary code of the first round and restarts the code emission.
This time the compiler moves the code pieces around and adds the right padding to construct the required layout.

For the moment, only the `.text` section can contain target bits (so no data sections like `.rodata`). 
The compiler renames the original `main` function from the source code to `old_main` and moves it to the newly created `.dbl_text` section along with all other code. 
A new `main` function is placed in the original `.text` section that starts the `loader` followed by a call to the `old_main` function.

The linker script puts the section that has the modified layout (only the `.dbl_text` section for now) in a `PT_NULL` segment in the binary. 
This means it will not be loaded by the system loader when the target program starts. 
It is instead the `loader` component that will load this segment.
The `.text` section (containing the new `main` function) is still in a `PT_LOAD` segment.
This ensures there is still a code segment loaded by the system loader which can launch the `loader` component.

The bulk of the changes are made in [/lib/MC/MCAssembler.cpp](llvm-11.1.0.src/lib/MC/MCAssembler.cpp), [/lib/MC/DBLSolve/DBLSolveFF.cpp](llvm-11.1.0.src/lib/MC/DBLSolve/DBLSolveFF.cpp) and [/lib/CodeGen/DBL.cpp](llvm-11.1.0.src/lib/CodeGen/DBL.cpp).
The entry point to the layouting functionality is in the [`MCAssembler::Finish()`](llvm-11.1.0.src/lib/MC/MCAssembler.cpp#L1439) function in [MCAssembler.cpp](llvm-11.1.0.src/lib/MC/MCAssembler.cpp).

## LTO
The layouting logic in the compiler backend should have a complete view of all code containing targets.
This is the case only when all targets are either in the same compilation unit (module) or if all modules are first merged together, for example, when LTO is enabled. 
We, therefore, run all our compilations with LTO enabled.
ThinLTO is not supported because this still generates separate object files for each module which get linked by the native (non-LTO) linker (the same as in non-LTO compilation). 
The layouting logic checks whether all targets are found.
These checks will fail in non-LTO and ThinLTO builds where targets are in different modules.

You have to use LLD (included in the compiler directory under `tools`) as the linker. 
GNU Gold also works with LTO but it looks like not all LLVM backend code runs with Gold, including some of the code required for the layouting.

## Code
All changes in the gluezilla-compiler are marked with

    //DBL

