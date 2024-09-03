//DBL
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/DBLCLIArgs.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"

using namespace llvm;

//This pass renames the main function to `old_main` and inserts a new main
//fn that calls the RH loader followed by a call to the `old_main` fn

//build an empty function with only a return instruction
void createRHStub(FunctionCallee& Callee, LLVMContext& Context) {
  Function* Fn = dyn_cast<Function>(Callee.getCallee());
  assert(Fn);
  BasicBlock* BB = BasicBlock::Create(Context, "", Fn);
  ReturnInst::Create(Context, BB);
}

void instrumentModule(Module& M) {
  if(DBLMode == Baseline) return;

  errs() << "DBL Pass runs\n";

  LLVMContext& Context = M.getContext();
  for(Function& F: M) {
    if(F.getName() == "main") {
      Type* VoidTy = Type::getVoidTy(Context);
      Type* Int32Ty = Type::getInt32Ty(Context);
      FunctionType* MainFnType = F.getFunctionType();
      FunctionType* RHFnType = FunctionType::get(VoidTy, false);

      //rename the main function and create a new main function
      F.setName("old_main");
      M.getOrInsertFunction("main", MainFnType);
      Function* NewMainFn = M.getFunction("main");
      assert(NewMainFn && "main fn not created");
      BasicBlock* NewMainBB = BasicBlock::Create(Context, "", NewMainFn);
      IRBuilder<> IRB(NewMainBB);

      //call the RH loader
      FunctionCallee RHFn = M.getOrInsertFunction("do_the_thing", RHFnType);
      //in 'offsets' mode, we insert an empty loader function so we don't have
      //to link in the loader and the 'offsets' binary is still functional
      if(DBLMode == Offsets) createRHStub(RHFn, Context);
      IRB.CreateCall(RHFn);

      //call old_main with the original arguments
      SmallVector<Value*, 2> Args;
      for(auto* I = NewMainFn->arg_begin(); I != NewMainFn->arg_end(); I++)
        Args.push_back(I);
      IRB.CreateCall(F.getFunctionType(), &F, Args);

      //return
      Value* Ret = ConstantInt::get(Int32Ty, 0);
      IRB.CreateRet(Ret);
      break;
    }
  }
}

struct DBLPass : public ModulePass {
  static char ID;
  DBLPass() : ModulePass(ID) {
    initializeDBLPassPass(*PassRegistry::getPassRegistry());
  }
  bool runOnModule(Module &M) override {
    instrumentModule(M);
    return true;
  }
};

INITIALIZE_PASS(DBLPass, "DBL pass", "DBL pass", false, false)

ModulePass* llvm::createDBLPassPass() {
  return new DBLPass();
}

char DBLPass::ID = 0;
