# GlueZilla-Loader
This component produces a library `libloader.so` that is linked to the protected binary and contains a function that loads the `PT_NULL` segment of the target binary. 
While doing so, it puts the virtual pages in the right physical page frames based on the info in the `attack_config.toml` file (paper: the loader map). 
The `loader` continuously allocates pages until all required victim and aggressor page frames are found. 
The allocated pages are locked in memory so they are not swapped out to disk. 
Some required page frames might be held by another process or by the OS and will not get swapped out. 
In this case, the `loader` will keep allocating until the OS kills it eventually when it's running low on memory. 
When this keeps happening, you can either wait for a few minutes, run some other programs in between or disable(+enable) the swap memory. 
These tricks usually move some memory around.

The loader requires a temporary file `/tmp/RAM_SWAP_ID` to be present containing an arbitrary integer ID for logging purposes.

The loader and related rowhammer tools have the ability to insert arbitrary code (called garbage in the code) in between the hammer accesses to slow the accesses down.
The repo contains one garbage sequence of sequential add instructions.
Beware that, without sequencing measures like data dependencies, out-of-order execution can move the instructions around.
This feature is not essential to GlueZilla.