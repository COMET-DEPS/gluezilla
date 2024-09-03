//DBL workaround to get these CLI options in different LLVM components

#include "llvm/Support/CommandLine.h"

using namespace llvm;

enum DBLModeT {
  Baseline, Offsets, DBL
};

extern cl::opt<DBLModeT> DBLMode;
extern cl::opt<std::string> TargetOffsetsPath;
extern cl::opt<std::string> VictimAddressesPath;
extern cl::opt<std::string> CompilerOutputPath;
extern cl::opt<std::string> CompilationID;
