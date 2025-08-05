//===- TransMemoryAttr.cpp - Symbol Rewriter -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/IntrinsicsNVPTX.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Support/Process.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Utils/TransMemoryAttr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/SmallSet.h"

#include <map>
#include <set>

#define DEBUG_TYPE "trans-mem-attr"

using namespace llvm;

// enum LocationMod {
//     ArgMem = 1,
//     InaccessibleMem = 2,
//     AnyWhere = 4
//   };

// enum AccessMod {
//     read = 8,
//     Write = 16,
// };

template <typename T> void replacement(T *Ptr) {
  static_assert(std::is_same_v<T, CallInst> || std::is_same_v<T, Function>,
                "transInsMemAttr only supports CallInst* or Function*");
  auto ME = Ptr->getMemoryEffects();
  if (ME == MemoryEffects::none()) {
    Ptr->addFnAttr(Attribute::ReadNone);
  } else if (ME != MemoryEffects::unknown()) {
    // add location mod attr
    if (ME.onlyAccessesArgPointees()) {
      Ptr->addFnAttr(Attribute::ArgMemOnly);
    } else if (ME.onlyAccessesInaccessibleMem()) {
      Ptr->addFnAttr(Attribute::InaccessibleMemOnly);
    } else if (ME.onlyAccessesInaccessibleOrArgMem()) {
      Ptr->addFnAttr(Attribute::InaccessibleMemOrArgMemOnly);
    }
    // add acceess mod attr
    if (ME.onlyReadsMemory()) {
      Ptr->addFnAttr(Attribute::ReadOnly);
    } else if (ME.onlyWritesMemory()) {
      Ptr->addFnAttr(Attribute::WriteOnly);
    }
  } else {
    assert((ME == MemoryEffects::unknown()) && "unexpected memory access mod");
  }
  Ptr->removeFnAttr(Attribute::AttrKind::Memory);
}

bool transInsMemAttr(Instruction *I) {
  CallInst *CI = dyn_cast<CallInst>(I);
  if (CI && CI->hasFnAttr(Attribute::AttrKind::Memory)) {
    LLVM_DEBUG(llvm::dbgs() << "CI: "; CI->print(llvm::dbgs());
               llvm::dbgs() << "\n";);
    replacement(CI);
    return true;
  }
  return false;
}

bool transFnMemAttr(Function *F) {
  if (F->hasFnAttribute(Attribute::AttrKind::Memory)) {
    LLVM_DEBUG(llvm::dbgs() << "Function: " << F->getName() << "\n";);
    replacement(F);
    return true;
  }
  return false;
}

PreservedAnalyses TransMemoryAttr::run(Module &M, ModuleAnalysisManager &MAM) {
  bool MadeChange = false;
  for (Function &F : M.functions()) {
    MadeChange |= transFnMemAttr(&F);
  }
  for (Function &F : M.functions()) {
    // MadeChange |= transFnMemAttr(&F);
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        MadeChange |= transInsMemAttr(&I);
      }
    }
  }

  if (MadeChange)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
