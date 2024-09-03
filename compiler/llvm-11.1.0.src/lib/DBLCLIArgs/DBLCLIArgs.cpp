//DBL workaround to get these CLI options in different LLVM components

#include "llvm/Support/CommandLine.h"
#include "llvm/DBLCLIArgs.h"

using namespace llvm;


cl::opt<DBLModeT> DBLMode("dbl_mode", cl::desc("dbl mode"),
                         cl::values(
                           clEnumValN(Baseline, "baseline", ""),
                           clEnumValN(Offsets, "offsets", ""),
                           clEnumValN(DBL, "dbl", "")),
                         cl::init(DBLModeT::Baseline));
cl::opt<std::string> TargetOffsetsPath("TO",
                                cl::desc("target offsets file path"),
                                cl::value_desc("file path"), cl::init(""));
cl::opt<std::string> VictimAddressesPath("VA",
                                cl::desc("victim addresses file path"),
                                cl::value_desc("file path"), cl::init(""));
cl::opt<std::string> CompilerOutputPath("dbl_output",
                                cl::desc("dbl output file path"),
                                cl::value_desc("file path"), cl::init(""));
cl::opt<std::string> CompilationID("dbl_id",
                            cl::desc("an ID to tag the output file with"),
                            cl::value_desc("dbl_id"), cl::init(""));

