//===- TypedPointers.cpp - Symbol Rewriter -------------------------------===//
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
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Support/Process.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Utils/TypingPointers.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/SmallSet.h"
#include <map>
#include <set>

#define DEBUG_TYPE "type-pointers"

using namespace llvm;

namespace {
class PointersAnalyzer : public InstVisitor<PointersAnalyzer> {
private:
  bool Changed = false;
  std::map<Function*, bool>FunctionMap;
  std::vector<GlobalVariable*> GlobalList;
  std::map<GlobalVariable*, GlobalVariable*> GlobalMap;

  std::vector<PHINode*> PHIList;
  std::map<Constant*, Constant*>ConstantExpMap;

  std::map<Value*, Value*> AggregateMap;
  std::map<Value*, std::vector<Value*>> AggregateChain;

  //constant map, hold the global vairable and constant expression.
  //TODO: GlobalMap&ConstantMap should be Merged here
  std::map<Constant*, Constant*>ConstantMap;

  // Holds all new created values
  std::map<Value*, Value*>ValueMap;

public:
  PointersAnalyzer()=default;
  bool isChanged(){ return Changed; };

  void visitICmpInst(ICmpInst &I);
  void visitAllocaInst(AllocaInst &I);
  void visitLoadInst(LoadInst     &I);
  void visitStoreInst(StoreInst   &I);
  void visitAtomicCmpXchgInst(AtomicCmpXchgInst &I);
  void visitAtomicRMWInst(AtomicRMWInst &I);
  void visitGetElementPtrInst(GetElementPtrInst &I);
  //void visitPHINode(PHINode       &I);
  void visitPtrToIntInst(PtrToIntInst &I);
  void visitIntToPtrInst(IntToPtrInst &I);
  void visitBitCastInst(BitCastInst &I);
  void visitAddrSpaceCastInst(AddrSpaceCastInst &I);
  void visitSelectInst(SelectInst &I);
  //void visitVAArgInst(VAArgInst   &I);
  void visitExtractElementInst(ExtractElementInst &I);
  void visitInsertElementInst(InsertElementInst &I);
  //void visitShuffleVectorInst(ShuffleVectorInst &I);
  void visitExtractValueInst(ExtractValueInst &I);
  void visitInsertValueInst(InsertValueInst &I);

  void visitCallInst(CallInst &I);
  void visitReturnInst(ReturnInst &I);

  //void visitLandingPadInst(LandingPadInst &I);

  void visitInstruction(Instruction &I);

  void collectAggregates(Instruction &I);

  Type* processType(Type* Ty);
  StructType* processStructType(StructType* STy);
  ArrayType* processArrayType(ArrayType* ATy);
  VectorType* processVectorType(VectorType* ATy);
  void processNVVMReflect(Module &M);
  void processFunctions(Module &M);
  void processGlobals(Module &M);
  bool processFunction(Function* &F);
  bool processFunctionArguments(Function* &F);
  void processStructure(Value* SVal, StructType* NewTy=nullptr);
  void processVector(Value* VVal, VectorType* NewTy=nullptr);
  void processAggregates(Module &N);
  void processPHINode(PHINode &I);
  // format the constant expression and map with the typed expressions
  void processConstantExpression(ConstantExpr *CE, GlobalVariable* OldGV=nullptr,
                                 GlobalVariable* NewGV=nullptr);
  Constant* processConstantAggregate(ConstantAggregate *CA);

  Value* processPtr(IRBuilder<> &builder, Value* SrcPtr, Type* TypedPtrTy);

  void removeRedundentBitCast(Module &M);
  void removeDeadValue();
  void removeFakeCast(Module &M);

  bool runOnModule(Module &M);
};
}

PreservedAnalyses TypingPointers::run(Module &M, ModuleAnalysisManager &MAM) {
  bool MadeChange = false;
  PointersAnalyzer Analyzer;

  MadeChange = Analyzer.runOnModule(M);

  if (MadeChange)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}


bool PointersAnalyzer::runOnModule(Module& M) {

  for(Function &F : M.functions()) {
    FunctionMap[&F] = false;
  }

  processNVVMReflect(M);

  processGlobals(M);

  processAggregates(M);

  processFunctions(M);

  removeDeadValue();

  removeFakeCast(M);

  removeRedundentBitCast(M);

  LLVM_DEBUG(llvm::dbgs() << "final module: \n";
             M.print(llvm::dbgs(), nullptr);
             llvm::dbgs() << "\n"; );

  return Changed;
}


bool static isContainOpaquePointers(Type *Ty) {
  if(Ty->getNumContainedTypes() == 0) {
    return Ty->isOpaquePointerTy();
  } else {
    for(auto SubTy : Ty->subtypes()) {
      if(isContainOpaquePointers(SubTy))
        return true;
    }
    return false;
  }
}

void PointersAnalyzer::collectAggregates(Instruction &I) {
  if(InsertValueInst * InsertI = dyn_cast<InsertValueInst>(&I)) {
    if(isa<UndefValue>(InsertI->getAggregateOperand()) ||
       isa<PoisonValue>(InsertI->getAggregateOperand())) {
      if(isContainOpaquePointers(InsertI->getType())) {
        std::vector<Value*> InsertList;
        InsertList.push_back(InsertI);
        while(InsertI->hasOneUser()) {
          if(InsertValueInst* NextInsertI = dyn_cast<InsertValueInst>(*InsertI->user_begin())) {
            InsertI = NextInsertI;
            InsertList.push_back(InsertI);
          } else {
            break;
          }
        }
        if(AggregateChain.find(InsertI) == AggregateChain.end()) {
          AggregateChain.insert(std::make_pair(InsertI, InsertList));
        }
      }
    }
  } else if (InsertElementInst * InsertI = dyn_cast<InsertElementInst>(&I)) {
    if(isa<UndefValue>(InsertI->getOperand(0)) ||
       isa<PoisonValue>(InsertI->getOperand(0))) {
      if(isContainOpaquePointers(InsertI->getType())) {
        std::vector<Value*> InsertList;
        InsertList.push_back(InsertI);
        while(InsertI->hasOneUser()) {
          if(InsertElementInst* NextInsertI = dyn_cast<InsertElementInst>(*InsertI->user_begin())) {
            InsertI = NextInsertI;
            InsertList.push_back(InsertI);
          } else {
            break;
          }
        }
        if(AggregateChain.find(InsertI) == AggregateChain.end()) {
          AggregateChain.insert(std::make_pair(InsertI, InsertList));
        }
      }
    }
  }
}

void PointersAnalyzer::processNVVMReflect(Module &M) {
  if(Function *F = M.getFunction("__nvvm_reflect")) {
    if(F->getFunctionType()->getParamType(0)->isOpaquePointerTy()) {
        processFunction(F);
    }
  }
}

void PointersAnalyzer::processGlobals(Module &M) {
  for(auto &V : M.globals()) {
    GlobalList.push_back(&V);
    GlobalMap.insert(std::make_pair(&V, &V));
  }

  for(auto GVIter : GlobalMap) {
    GlobalVariable *GV = GVIter.first;
    if(GV->getType()->isOpaquePointerTy()) {
      Type* ValTy = GV->getValueType();
      Type* NewTy = PointerType::get(ValTy, GV->getType()->getPointerAddressSpace(), true);
      if(ValTy->isOpaquePointerTy()) {
        NewTy = PointerType::get(Type::getInt8Ty(M.getContext()),
                        GV->getType()->getPointerAddressSpace(), true);
      }
      LLVM_DEBUG(llvm::dbgs() << "global new type: \n";
                 NewTy->print(llvm::dbgs());
                 llvm::dbgs() << "\n"; );
      //Copy Global
      Constant *Initializer = nullptr;
      auto* ValueType = GV->getValueType();
      if(GV->hasInitializer()) {
        Initializer = GV->getInitializer();
        if(ConstantAggregate* CA = dyn_cast<ConstantAggregate>(Initializer)) {
          LLVM_DEBUG(llvm::dbgs() << "ConstantAggregate type";);
          Initializer = processConstantAggregate(CA);
        } else if (ConstantExpr* CE = dyn_cast<ConstantExpr>(Initializer)) {
          processConstantExpression(CE);
          Initializer = ConstantExpMap[CE];
        } else if (Initializer->getType()->isOpaquePointerTy()) {
          IRBuilder<> B(M.getContext());
          Type* NewTy = PointerType::get(Type::getInt8Ty(M.getContext()),
                        ValueType->getPointerAddressSpace(), true);
          Initializer = dyn_cast<Constant>(processPtr(B, Initializer, NewTy));
        }
        ValueType = Initializer->getType();
      }
      GlobalVariable *NewGV = new GlobalVariable(M, ValueType, GV->isConstant(),
                                GV->getLinkage(), Initializer, true, GV->getName(),
                                nullptr, GV->getThreadLocalMode(), GV->getType()->getAddressSpace());
      NewGV->copyAttributesFrom(GV);
      NewGV->takeName(GV);
      //NewGV->getComdat(GV->getComdat());

      LLVM_DEBUG(llvm::dbgs() << "Create new global: \n";
                 NewGV->print(llvm::dbgs());
                 llvm::dbgs() << "\n"; );
      GlobalMap[GV] = NewGV;

      Value::user_iterator Next;
      for(Value::user_iterator UserI = GV->user_begin(), UserE = GV->user_end();
                            UserI != UserE; UserI = Next) {
        Next = std::next(UserI);
        LLVM_DEBUG(llvm::dbgs() << "find use: \n";
                  UserI->print(llvm::dbgs());
                  llvm::dbgs() << "\n"; );
        if(Instruction* I = dyn_cast<Instruction>(*UserI)) {
          //global variable used in common instructions
          IRBuilder<> B(I);
          auto *NewCast = B.CreateBitCast(NewGV, GV->getType());
          UserI->replaceUsesOfWith(GV, NewCast);
        } else if (ConstantExpr* CE = dyn_cast<ConstantExpr>(*UserI)){
          //global variable used in constant expressions
          processConstantExpression(CE, GV, NewGV);
          Constant *NewCE = ConstantExpMap[CE];
          Constant *NewCast = ConstantExpr::getBitCast(NewCE, CE->getType());
          UserI->replaceAllUsesWith(NewCast);
        } else if (ConstantStruct *CStruct = dyn_cast<ConstantStruct>(*UserI)){
          //Create a new constantStructure and insert in the structure map
          //All the ptr in PHI should be formatted to i8*
          std::vector<Constant*> OpndList;
          std::vector<Type*> TypeList;
          unsigned Idx = 0;
          for(; Idx < CStruct->getNumOperands(); Idx++) {
            Constant *Opnd = CStruct->getOperand(Idx);
            if(Opnd->getType()->isOpaquePointerTy()) {
              Type* Int8Ptr = PointerType::get(Type::getInt8Ty(M.getContext()),
                                  Opnd->getType()->getPointerAddressSpace(), true);
              if(GlobalMap.find(dyn_cast<GlobalVariable>(Opnd)) != GlobalMap.end()) {
                Constant *NewOpnd = GlobalMap[dyn_cast<GlobalVariable>(Opnd)];
                if(NewOpnd->getType() != Int8Ptr) {
                  NewOpnd = ConstantExpr::getBitCast(NewOpnd, Int8Ptr);
                }
                TypeList.push_back(Int8Ptr);
                OpndList.push_back(NewOpnd);
              } else {
                // the global operand hasn't been ready, skip this operation.
                // the last global operand will finish the structure.
                // or the PHI will handle(onstant expression).
                break;
              }
            } else {
              TypeList.push_back(Opnd->getType());
              OpndList.push_back(Opnd);
            }
          }
          if(Idx == CStruct->getNumOperands()) {
            StructType *STy = StructType::get(M.getContext(), TypeList);
            Constant *NewCStruct = ConstantStruct::get(STy, OpndList);
            AggregateMap.insert(std::make_pair(CStruct, NewCStruct));
          }
        }
      }
    }
  }
}

void PointersAnalyzer::processAggregates(Module& M) {
  for(Function &F : M.functions()) {
    for (BasicBlock &BB : F) {
      for(Instruction &I : BB) {
        if(InsertValueInst *InsertI = dyn_cast<InsertValueInst>(&I)) {
          collectAggregates(*InsertI);
        } else if (InsertElementInst *InsertI = dyn_cast<InsertElementInst>(&I)) {
          collectAggregates(*InsertI);
        } else if(PHINode * PHI = dyn_cast<PHINode>(&I)) {
          if(isContainOpaquePointers(PHI->getType())) {
            PHIList.push_back(PHI);
          }
        }
      }
    }
  }
  for(auto AIter : AggregateChain) {
    if(AggregateMap.find(AIter.first) == AggregateMap.end()) {
      if(auto * InsertI = dyn_cast<InsertValueInst>(AIter.first)) {
        processStructure(AIter.first);
      } else if (auto * InsertI = dyn_cast<InsertElementInst>(AIter.first)) {
        processVector(AIter.first);
      }
    }
  }
  for(auto* PHI : PHIList) {
    processPHINode(*PHI);
  }
}

Value* PointersAnalyzer::processPtr(IRBuilder<> &builder, Value* SrcPtr, Type* TypedPtrTy) {
  Value *TypedSrc;
  //AggregateType includes struct and array types, but does not include vector types
  if(SrcPtr->getType()->isAggregateType()) {
    TypedSrc = builder.CreateIntrinsic(Intrinsic::fake_cast,
                      {TypedPtrTy, SrcPtr->getType()}, {SrcPtr});
  } else if(ConstantExpr *CE = dyn_cast<ConstantExpr>(SrcPtr)) {
    if(ConstantExpMap.find(CE) == ConstantExpMap.end()) {
      processConstantExpression(CE);
    }
    Value* NewSrcOpnd = ConstantExpMap[CE];
    if(TypedPtrTy != NewSrcOpnd->getType())
      TypedSrc = builder.CreateBitCast(NewSrcOpnd, TypedPtrTy);
    else
      TypedSrc = NewSrcOpnd;
  } else {
    TypedSrc = builder.CreateBitCast(SrcPtr, TypedPtrTy);
  }
  return TypedSrc;
}


void PointersAnalyzer::processConstantExpression(ConstantExpr *CE,
                                            GlobalVariable* OldGV,
                                            GlobalVariable* NewGV){
  if(ConstantExpMap.find(CE) != ConstantExpMap.end()) return;

  if (auto *GEP = dyn_cast<GEPOperator>(CE)) {
    // The new return type will be source element pointer type
    auto SETy = GEP->getSourceElementType();
    auto SEPtrTy = PointerType::get(SETy, GEP->getPointerAddressSpace(), true);
    Constant *SrcPtr = dyn_cast<Constant>(GEP->getPointerOperand());
    std::vector<Value*> Idx;
    for(auto It = GEP->idx_begin(); It != GEP->idx_end(); It++) {
      Idx.push_back(*It);
    }

    if(OldGV && NewGV) {
      // The GV should be replaced
      SrcPtr = NewGV;
    }

    if(SEPtrTy == SrcPtr->getType()) {
      //create new GEP constant expression
      // New GEP returns typed pointer
      Constant *NewGEP =
            ConstantExpr::getInBoundsGetElementPtr(SETy, SrcPtr, Idx, true, true);
      ConstantExpMap[CE] = NewGEP;
    } else {
      //create bitcast and new GEP constant expression
      Constant *PreCast = ConstantExpr::getBitCast(SrcPtr, SEPtrTy);
      Constant *NewGEP =
            ConstantExpr::getInBoundsGetElementPtr(SETy, PreCast, Idx, true, true);
      ConstantExpMap[CE] = NewGEP;
    }
  } else if(auto * BC = dyn_cast<BitCastOperator>(CE)) {
    //cast will be fold.
    Constant *SrcPtr = CE;
    //cast constant expression to i8*
    Type* Int8Ptr = PointerType::get(Type::getInt8Ty(CE->getContext()),
                                  BC->getType()->getPointerAddressSpace(), true);
    // This GlobalVariable path might not really hanppen
    if(OldGV && NewGV) {
      SrcPtr = NewGV;
    }

    Constant *NewCast = ConstantExpr::getBitCast(SrcPtr, Int8Ptr);
    ConstantExpMap[CE] = NewCast;

  } else if(auto *AC = dyn_cast<AddrSpaceCastOperator>(CE)) {
    Constant *SrcPtr = CE;
    // always cast to i8ptr
    if(OldGV && NewGV) {
      SrcPtr = NewGV;
    }
    Type* I8SrcPtr = PointerType::get(Type::getInt8Ty(CE->getContext()),
                                  AC->getSrcAddressSpace(), true);
    Type* I8DstPtr = PointerType::get(Type::getInt8Ty(CE->getContext()),
                                  AC->getDestAddressSpace(), true);
    if(SrcPtr->getType() != I8SrcPtr) {
      SrcPtr = ConstantExpr::getBitCast(SrcPtr, I8SrcPtr);
    }
    Constant *NewAC = ConstantExpr::getAddrSpaceCast(SrcPtr, I8DstPtr);
    ConstantExpMap[CE] = NewAC;

  } else if(auto * PTI= dyn_cast<PtrToIntOperator>(CE)) {
    report_fatal_error("PtrToIntOperator ConstantExpr type need to be handle");
  } else {
    report_fatal_error("Unknown ConstantExpr type need to be handle");
  }

}

Constant* PointersAnalyzer::processConstantAggregate(ConstantAggregate *CAG) {
  LLVMContext &Ctx = CAG->getContext();
  if(auto *CAY = dyn_cast<ConstantArray>(CAG)) {
    auto *ETy = CAY->getType()->getElementType();
    //array element type contains ptr
    if(isContainOpaquePointers(ETy)) {
      if(ETy->isOpaquePointerTy()) {
        Type* I8Ptr = PointerType::get(Type::getInt8Ty(Ctx),
                                  ETy->getPointerAddressSpace(), true);
        auto* NewATy = ArrayType::get(I8Ptr, CAY->getType()->getNumElements());
        SmallVector<Constant *, 8> Opnds;
        for(unsigned Idx = 0; Idx < CAY->getNumOperands(); Idx++) {
          Constant* Elt = CAY->getOperand(Idx);
          Constant* NewElt = ConstantExpr::getBitCast(Elt, I8Ptr);
          Opnds.push_back(NewElt);
        }
        return ConstantArray::get(NewATy, Opnds);
      } else if (auto STy = dyn_cast<StructType>(ETy)) {
        std::vector<Type*> TyList;
        for(auto SubTy : STy->subtypes()) {
          if(SubTy->isAggregateType()) {
            report_fatal_error("New nested isAggregateType need to handle");
          } else if(SubTy->isOpaquePointerTy()) {
            Type* Int8Ptr = PointerType::get(Type::getInt8Ty(Ctx),
                                  SubTy->getPointerAddressSpace(), true);
            TyList.push_back(Int8Ptr);
          } else {
            TyList.push_back(SubTy);
          }
        }
        auto* NewSTy = StructType::get(Ctx, TyList);
        auto* NewATy = ArrayType::get(NewSTy, CAY->getType()->getNumElements());
        SmallVector<Constant *, 8> Opnds;
        for(unsigned Idx = 0; Idx < CAY->getNumOperands(); Idx++) {
          ConstantStruct* Elt = dyn_cast<ConstantStruct>(CAY->getOperand(Idx));
          SmallVector<Constant *, 8> SOpnds;
          for(unsigned SI = 0; SI < Elt->getNumOperands(); SI++) {
            Constant* SElt = Elt->getOperand(SI);
            if(SElt->getType()->isOpaquePointerTy()) {
              Type* Int8Ptr = PointerType::get(Type::getInt8Ty(Ctx),
                                  SElt->getType()->getPointerAddressSpace(), true);
              if(auto* Func = dyn_cast<Function>(SElt)) {
                processFunction(Func);
                SElt = Func;
              }
              Constant* NewElt = ConstantExpr::getBitCast(SElt, Int8Ptr);
              SOpnds.push_back(NewElt);
            } else {
              SOpnds.push_back(SElt);
            }
          }
          auto* NewOpnd = ConstantStruct::get(NewSTy, SOpnds);
          Opnds.push_back(NewOpnd);
        }
        return ConstantArray::get(NewATy, Opnds);
      }
    }
  } else if (auto *CST = dyn_cast<ConstantStruct>(CAG)) {
    processStructure(CST);
    return dyn_cast<Constant>(AggregateMap[CST]);
  }
  return CAG;
}

void PointersAnalyzer::processVector(Value* VVal, VectorType* NewTy) {
  LLVMContext &Ctx = VVal->getContext();
  VectorType * VTy = dyn_cast<VectorType>(VVal->getType());
  if(NewTy == nullptr) {
    NewTy = dyn_cast<VectorType>(processType(VTy));
  }
  if(ConstantVector *CVector = dyn_cast<ConstantVector>(VVal)) {
    report_fatal_error("Need to implement constant vector");
  } else {
    Value* NewAgg = llvm::PoisonValue::get(NewTy);
    for(Value* InsertVal : AggregateChain[VVal]) {
      IRBuilder<> B(dyn_cast<Instruction>(InsertVal));
      InsertElementInst *InsertI = dyn_cast<InsertElementInst>(InsertVal);
      Value *IstValue = InsertI->getOperand(1);
      Value *Index = InsertI->getOperand(2);
      if(IstValue->getType()->isAggregateType()) {
        report_fatal_error("unhandled aggregate elements in vector");
      } else {
        IstValue = processPtr(B, IstValue, NewTy->getElementType());
      }
      NewAgg = B.CreateInsertElement(NewAgg, IstValue, Index);
    }
    AggregateMap.insert(std::make_pair(VVal, NewAgg));

    Value::user_iterator Next;
    bool OtherUse = false;
    for(Value::user_iterator UserI = VVal->user_begin(), UserE = VVal->user_end();
                          UserI != UserE; UserI = Next) {
      Next = std::next(UserI);
      IRBuilder<> B(dyn_cast<Instruction>(*UserI));
      if(ExtractElementInst* ExtractI = dyn_cast<ExtractElementInst>(*UserI)) {
        Value *NewExtract = B.CreateExtractElement(NewAgg, ExtractI->getIndexOperand());
        if(NewExtract->getType() != ExtractI->getType()) {
          NewExtract = processPtr(B, NewExtract, ExtractI->getType());
        }
        ExtractI->replaceAllUsesWith(NewExtract);
        ExtractI->eraseFromParent();
      } else if (PtrToIntInst* PtrCast = dyn_cast<PtrToIntInst>(*UserI)) {
        auto *NewCast = B.CreatePtrToInt(NewAgg, PtrCast->getType());
        PtrCast->replaceAllUsesWith(NewCast);
        PtrCast->eraseFromParent();
      } else {
        OtherUse = true;
      }
    }
    if(OtherUse) {
      IRBuilder<> B(dyn_cast<Instruction>(VVal));
      Value *NewVal = processPtr(B, NewAgg, VVal->getType());
      VVal->replaceAllUsesWith(NewVal);
    }
  }
}

void PointersAnalyzer::processStructure(Value* SVal, StructType* NewTy) {
  LLVMContext &Ctx = SVal->getContext();
  StructType * STy = dyn_cast<StructType>(SVal->getType());
  if(NewTy == nullptr) {
    NewTy = dyn_cast<StructType>(processType(STy));
  }
  if(ConstantStruct *CStruct = dyn_cast<ConstantStruct>(SVal)) {
    //Create a new constantStructure and insert in the structure map
    std::vector<Constant*> OpndList;
    for(unsigned Idx = 0; Idx < CStruct->getNumOperands(); Idx++) {
      Constant *Opnd = CStruct->getOperand(Idx);
      if(Opnd->getType() != NewTy->getElementType(Idx)) {
        Constant *NewOpnd;
        if(ConstantExpr* CE = dyn_cast<ConstantExpr>(Opnd)) {
          processConstantExpression(CE);
          // new constant expression is i8*
          NewOpnd = ConstantExpr::getBitCast(ConstantExpMap[CE], NewTy->getElementType(Idx));
        } else {
          NewOpnd = ConstantExpr::getBitCast(Opnd, NewTy->getElementType(Idx));
        }
        OpndList.push_back(NewOpnd);
      } else {
        OpndList.push_back(Opnd);
      }
    }
    Constant *NewCStruct = ConstantStruct::get(NewTy, OpndList);
    AggregateMap.insert(std::make_pair(CStruct, NewCStruct));
  } else if (AggregateChain.find(SVal) != AggregateChain.end()) {
    Value* NewAgg = llvm::PoisonValue::get(NewTy);
    for(Value* InsertVal : AggregateChain[SVal]) {
      InsertValueInst *InsertI = dyn_cast<InsertValueInst>(InsertVal);
      IRBuilder<> B(dyn_cast<Instruction>(InsertVal));
      auto Indices = InsertI->getIndices();
      Value *IstValue = InsertI->getInsertedValueOperand();
      if(STy->getElementType(Indices[0]) != NewTy->getElementType(Indices[0])) {
        IstValue = processPtr(B, IstValue, NewTy->getElementType(Indices[0]));
      }
      NewAgg = B.CreateInsertValue(NewAgg, IstValue, Indices);
    }
    AggregateMap.insert(std::make_pair(SVal, NewAgg));
    Value::user_iterator Next;
    bool OtherUse = false;
    for(Value::user_iterator UserI = SVal->user_begin(), UserE = SVal->user_end();
                          UserI != UserE; UserI = Next) {
      Next = std::next(UserI);
      IRBuilder<> B(dyn_cast<Instruction>(*UserI));
      if(ExtractValueInst* ExtractI = dyn_cast<ExtractValueInst>(*UserI)) {
        Value *NewExtract = B.CreateExtractValue(NewAgg, ExtractI->getIndices());
        if(NewExtract->getType() != ExtractI->getType()) {
          NewExtract = processPtr(B, NewExtract, ExtractI->getType());
        }
        ExtractI->replaceAllUsesWith(NewExtract);
        ExtractI->eraseFromParent();
      } else {
        OtherUse = true;
      }
    }
    if(OtherUse) {
      IRBuilder<> B(dyn_cast<Instruction>(SVal));
      Value *NewVal = processPtr(B, NewAgg, SVal->getType());
      SVal->replaceAllUsesWith(NewVal);
    }
  }
}
#if 0
void PointersAnalyzer::processPHI() {
  for(auto PHII : PHIList) {
    Type *PHITy = PHII->getType();
    if(PHITy->isAggregateType()) {
      //structure type
      if(AggregateMap.find(PHII->getOperand(0)) == AggregateMap.end()) {
        processStructure(PHII->getOperand(0));
      }

      Type *NewPHITy = AggregateMap[PHII->getOperand(0)]->getType();
      IRBuilder<> B(dyn_cast<Instruction>(PHII));
      auto NewPHI = B.CreatePHI(NewPHITy, PHII->getNumIncomingValues());

      for(unsigned Idx = 0; Idx <PHII->getNumIncomingValues(); Idx++) {
        Value *Opnd = PHII->getIncomingValue(Idx);
        if(AggregateMap.find(Opnd) == AggregateMap.end()) {
          //The constant expression structure will be resolved here.
          processStructure(Opnd);
        }
        Value * NewOpnd = AggregateMap[Opnd];
        NewPHI->addIncoming(NewOpnd, PHII->getIncomingBlock(Idx));
      }
      Value::user_iterator Next;
      for(Value::user_iterator UserI = PHII->user_begin(), UserE = PHII->user_end();
                            UserI != UserE; UserI = Next) {
        Next = std::next(UserI);
        if(ExtractValueInst* ExtractI = dyn_cast<ExtractValueInst>(*UserI)) {
          IRBuilder<> B(dyn_cast<Instruction>(*UserI));
          Value *NewExtract = B.CreateExtractValue(NewPHI, ExtractI->getIndices());
          if(NewExtract->getType() != ExtractI->getType()) {
            NewExtract = B.CreateBitCast(NewExtract, ExtractI->getType());
          }
          ExtractI->replaceAllUsesWith(NewExtract);
          ExtractI->eraseFromParent();
        } else {
          report_fatal_error("Unresolved PHI node use");
        }
      }
      // remove PHI and its creation chain
      if(PHII->user_empty()) {
        PHII->eraseFromParent();
      } else {
        report_fatal_error("Unresolved PHI Node");
      }
    } else {
      //ptr type, need to insert bitcast right behind the operand value.
      LLVMContext &Ctx = PHII->getContext();
      IRBuilder<> B(dyn_cast<Instruction>(PHII));
      Type* Int8Ptr = PointerType::get(Type::getInt8Ty(Ctx), PHII->getType()->getPointerAddressSpace(), true);
      auto NewPHI = B.CreatePHI(Int8Ptr, PHII->getNumIncomingValues());
      for(unsigned Idx = 0; Idx <PHII->getNumIncomingValues(); Idx++) {
        Value *Opnd = PHII->getIncomingValue(Idx);
        Value * NewOpnd = nullptr;
        if(Instruction* NormI = dyn_cast<Instruction>(Opnd)) {
          IRBuilder<> OpndB(dyn_cast<Instruction>(NormI));
          OpndB.SetInsertPoint(NormI->getNextNode());
          NewOpnd = OpndB.CreateBitCast(Opnd, Int8Ptr);
        } else if(ConstantExpr *CE = dyn_cast<ConstantExpr>(Opnd)) {
          if(ConstantExpMap.find(CE) == ConstantExpMap.end()) {
            processConstantExpression(CE);
          }
          NewOpnd = ConstantExpMap[CE];
        } else {
          report_fatal_error("Unresolved PHI node value");
        }
        NewPHI->addIncoming(NewOpnd, PHII->getIncomingBlock(Idx));
      }
      Value * PostCast = B.CreateBitCast(NewPHI, PHII->getType());
      PHII->replaceAllUsesWith(PostCast);
      PHII->eraseFromParent();
    }
  }
}
#endif
static bool checkConstantContained(Constant* CT, Constant* CVal) {
  //self contained must have ptr type inside
  if(isContainOpaquePointers(CT->getType())) {
    if(CT == CVal) return true;
    if(ConstantAggregate* CA = dyn_cast<ConstantAggregate>(CT)) {
    for(unsigned Idx = 0; Idx < CA->getNumOperands(); Idx++) {
      Constant *Opnd = CA->getOperand(Idx);
      if(checkConstantContained(Opnd, CVal)) {
        return true;
      }
      }
    } else if (ConstantExpr* CE = dyn_cast<ConstantExpr>(CT)) {
    for(unsigned Idx = 0; Idx < CE->getNumOperands(); Idx++) {
      Constant *Opnd = CE->getOperand(Idx);
      if(checkConstantContained(Opnd, CVal)) {
        return true;
      }
      }
    }
  }
  return false;
}

static bool isSelfContained(GlobalVariable* GVal) {
  if(GVal->hasInitializer()){
    auto* Initializer = GVal->getInitializer();
    return checkConstantContained(Initializer, GVal);
  }
  return false;
}


void PointersAnalyzer::removeDeadValue() {
  for(auto SIter : AggregateMap) {
    auto SVal = SIter.first;
    // structure has inserting instruction chain
    if(AggregateChain.find(SVal) != AggregateChain.end()) {
      auto SChain = AggregateChain[SVal];
      for(unsigned Idx = SChain.size()-1; Idx != -1U; Idx--) {
        Instruction *IsrtVal = dyn_cast<Instruction>(SChain[Idx]);
        if(IsrtVal->user_empty()) {
          IsrtVal->eraseFromParent();
        } else {
          Value::user_iterator Next;
          for(Value::user_iterator UserI = IsrtVal->user_begin(), UserE = IsrtVal->user_end();
                            UserI != UserE; UserI = Next) {
            Next = std::next(UserI);
            UserI->print(llvm::errs());
            llvm::errs() << "\n";
          }
          report_fatal_error("Unresolved structure chain value");
        }
      }
    } else {
      if(SVal->user_empty()) {
        if(Constant * CSVal = dyn_cast<Constant>(SVal)) {
          CSVal->destroyConstant();
        } else {
          report_fatal_error("Unresolved strutrue type");
        }
      } else {
        // might be used in some GV
        // example: @take_self_address = linkonce global { i32, ptr } { i32 0, ptr @take_self_address }
        //report_fatal_error("Unresolved structure use");
      }
    }
  }

  for(auto GV : GlobalList) {
    GV->removeDeadConstantUsers();
    //currently the reflect constant string still use mutatetype
    if(GV->getType()->isOpaquePointerTy()) {
      if(GV->user_empty()) {
        GV->eraseFromParent();
      } else {
        //check self contained
        if(GV->hasInitializer()){
          auto* Initializer = GV->getInitializer();
          Value::user_iterator Next;
          for(Value::user_iterator UserI = GV->user_begin(), UserE = GV->user_end();
                                UserI != UserE; UserI = Next) {
            Next = std::next(UserI);
            if(!checkConstantContained(Initializer, dyn_cast<Constant>(*UserI))) {
              report_fatal_error("Unresolved Global Vairable");
            }
          }
          GV->eraseFromParent();
        } else {
          report_fatal_error("Unresolved Global Vairable");
        }
      }
    }
  }

}


void PointersAnalyzer::visitICmpInst(ICmpInst &I) {
  Value *LHS = I.getOperand(0);
  Value *RHS = I.getOperand(1);
  if(LHS->getType()->isOpaquePointerTy()) {
    IRBuilder<> B(&I);
    auto NewLHSTy = PointerType::get(B.getInt8Ty(), LHS->getType()->getPointerAddressSpace(), true);
    Value *NewLHS = B.CreateBitCast(LHS, NewLHSTy);
    auto NewRHSTy = PointerType::get(B.getInt8Ty(), RHS->getType()->getPointerAddressSpace(), true);
    Value *NewRHS = B.CreateBitCast(LHS, NewRHSTy);
    Value *NewCmp = B.CreateICmp(I.getPredicate(), NewLHS, NewRHS);
    I.replaceAllUsesWith(NewCmp);
    I.eraseFromParent();
  }
}

void PointersAnalyzer::visitAllocaInst(AllocaInst &I) {
  // for case %8 = alloca ptr, align 8, might only happend in llvm13 bc test
  if(I.getAllocatedType()->isOpaquePointerTy()) {
    IRBuilder<> B(&I);
    Type *NewTy = PointerType::get(B.getInt8Ty(), I.getAllocatedType()->getPointerAddressSpace(), true);
    Type *RetTy = PointerType::get(NewTy, I.getAllocatedType()->getPointerAddressSpace(), true);
    Value *NewAlloc = B.CreateAlloca(NewTy, I.getAddressSpace(), I.getArraySize());
    NewAlloc->mutateType(RetTy);
    Value *TransTemp = B.CreateBitCast(NewAlloc, I.getType());
    I.replaceAllUsesWith(TransTemp);
    I.eraseFromParent();

  }else if(isContainOpaquePointers(I.getAllocatedType())) {
    LLVMContext &Ctx = I.getContext();
    IRBuilder<> B(&I);
    // alloc a structure type
    if(StructType * STy = dyn_cast<StructType>(I.getAllocatedType())) {
      std::vector<Type*> TyList;
      for(auto SubTy : STy->subtypes()) {
        if(SubTy->isAggregateType()) {
          report_fatal_error("New nested isAggregateType need to handle");
        } else if(SubTy->isOpaquePointerTy()) {
          Type* Int8Ptr = PointerType::get(Type::getInt8Ty(Ctx), SubTy->getPointerAddressSpace(), true);
          TyList.push_back(Int8Ptr);
        } else {
          TyList.push_back(SubTy);
        }
      }
      auto NewTy =  StructType::get(Ctx, TyList);
      Type *RetTy = PointerType::get(NewTy, I.getAddressSpace(), true);
      Value *NewAlloc = B.CreateAlloca(NewTy, I.getAddressSpace(), I.getArraySize());
      NewAlloc->mutateType(RetTy);
      Value* TransTemp = B.CreateBitCast(NewAlloc, I.getType());
      I.replaceAllUsesWith(TransTemp);
      I.eraseFromParent();
    } else {
      report_fatal_error("New nested isAggregateType need to handle");
    }
  }else if(I.getType()->isOpaquePointerTy()) {
    Type *DataTy = I.getAllocatedType();
    Type *DataPtrTy = PointerType::get(DataTy, I.getAddressSpace(), true);
    IRBuilder<> B(&I);
    Value *NewAlloc = B.CreateAlloca(I.getAllocatedType(), I.getAddressSpace(), I.getArraySize());
    NewAlloc->mutateType(DataPtrTy);
    Value *TransTemp = B.CreateBitCast(NewAlloc, I.getType());
    I.replaceAllUsesWith(TransTemp);
    I.eraseFromParent();
  }

}

// The point to ptr should not be happended in the really opaque pointer IR
// Point to ptr only happend in llvm13 bc test

static Type* processPointerType(Type *OpndTy, Type *TypedPtrTy, unsigned AddressSpace) {
  if(!OpndTy->isPointerTy()) return nullptr;

  if(OpndTy->isOpaquePointerTy()) {
    return TypedPtrTy;
  } else {
    Type * OpndElemTy = OpndTy->getNonOpaquePointerElementType();
    Type * NewElemTy = processPointerType(OpndElemTy, TypedPtrTy, AddressSpace);
    if(NewElemTy == nullptr) return nullptr;
    return PointerType::get(NewElemTy, AddressSpace, true);
  }

}

static Value* processOperand(IRBuilder<> &B, Value *Opnd, Type *TypedPtrTy) {
  if(!Opnd->getType()->isPointerTy()) return nullptr;
  PointerType * OpndTy = dyn_cast<PointerType>(Opnd->getType());
  if(TypedPtrTy == nullptr) {
    TypedPtrTy = PointerType::get(B.getInt8Ty(), Opnd->getType()->getPointerAddressSpace(), true);
  }
  Type *NewPtrTy = processPointerType(OpndTy, TypedPtrTy, Opnd->getType()->getPointerAddressSpace());
  if(NewPtrTy != nullptr) {
    Value *TypedSrc = B.CreateBitCast(Opnd, NewPtrTy);
    return TypedSrc;
  } else {
    return nullptr;
  }
}

void PointersAnalyzer::visitLoadInst(LoadInst &I) {
  // case %18 = load ptr, ptr* %9, align 8, which might only appeared in llvm13 bc test
  // case %18 = load i8*, i8** %9, align 8
  if(I.getType()->isOpaquePointerTy()) {
    IRBuilder<> B(&I);
    Value *OldPtr = I.getPointerOperand();
    Type *NewValTy = PointerType::get(B.getInt8Ty(), OldPtr->getType()->getPointerAddressSpace(), true);
    Type *NewPtrTy = PointerType::get(NewValTy, OldPtr->getType()->getPointerAddressSpace(), true);
    Value *NewPtr = processPtr(B, OldPtr, NewPtrTy);
    LoadInst *NewLoad = B.CreateLoad(NewValTy, NewPtr, I.isVolatile());

    NewLoad->setAlignment(I.getAlign());
    NewLoad->setAtomic(I.getOrdering(), I.getSyncScopeID());

    Value *PostCast = B.CreateBitCast(NewLoad, I.getType());
    I.replaceAllUsesWith(PostCast);
    I.eraseFromParent();

  } else {
    Value *Src = I.getPointerOperand();
    if(Src->getType()->isOpaquePointerTy()) {
      IRBuilder<> B(&I);
      Type *DataTy = I.getType();
      Type *DataPtrTy = PointerType::get(DataTy, I.getPointerAddressSpace(), true);
      Value *TypedSrc = processPtr(B, Src, DataPtrTy);
      I.setOperand(0, TypedSrc);
      Changed |= true;
    }
  }
}

void PointersAnalyzer::visitStoreInst(StoreInst &I) {
  if(I.getValueOperand()->getType()->isOpaquePointerTy()) {
    // case store ptr %8, ptr* %9, align 8
    // store i8* %8, i8** %9, align 8
    IRBuilder<>B(&I);
    Value *OldVal = I.getValueOperand();
    Value *OldPtr = I.getPointerOperand();
    Type *NewValTy = PointerType::get(B.getInt8Ty(), OldVal->getType()->getPointerAddressSpace(), true);
    Type *NewPtrTy = PointerType::get(NewValTy, OldVal->getType()->getPointerAddressSpace(), true);
    Value *NewVal = B.CreateBitCast(OldVal, NewValTy);
    Value *NewPtr = B.CreateBitCast(OldPtr, NewPtrTy);
    StoreInst *NewStore = B.CreateStore(NewVal, NewPtr, I.isVolatile());
    NewStore->setAlignment(I.getAlign());
    NewStore->setAtomic(I.getOrdering(), I.getSyncScopeID());
    I.eraseFromParent();

  } else {
    Value *Dst = I.getPointerOperand();
    if(Dst->getType()->isOpaquePointerTy()) {
      Type *DataTy = I.getValueOperand()->getType();
      Type *DataPtrTy = PointerType::get(DataTy, I.getPointerAddressSpace(), true);
      IRBuilder<>B(&I);
      Value *TypedDst = processPtr(B, Dst, DataPtrTy);
      I.setOperand(1, TypedDst);
      Changed |= true;
    }
  }
}

void PointersAnalyzer::visitPtrToIntInst(PtrToIntInst &I) {
  LLVMContext &Ctx = I.getContext();
  IRBuilder<> B(&I);
  Value *SrcOp = I.getPointerOperand();
  if(SrcOp->getType()->isOpaquePointerTy()) {
    Type* Int8Ptr = PointerType::get(Type::getInt8Ty(Ctx), SrcOp->getType()->getPointerAddressSpace(), true);
    Value *PreCast = B.CreateBitCast(SrcOp, Int8Ptr);
    Value *NewInst = B.CreatePtrToInt(PreCast, I.getType());
    I.replaceAllUsesWith(NewInst);
    I.eraseFromParent();
  }

}
void PointersAnalyzer::visitIntToPtrInst(IntToPtrInst &I) {
  LLVMContext &Ctx = I.getContext();
  IRBuilder<> B(&I);
  if(I.getType()->isOpaquePointerTy()) {
    Type* DestInt8Ptr = PointerType::get(Type::getInt8Ty(Ctx), I.getType()->getPointerAddressSpace(), true);
    Value *NewInst = B.CreateIntToPtr(I.getOperand(0), DestInt8Ptr);
    Value *PostCast = B.CreateBitCast(NewInst, I.getType());
    I.replaceAllUsesWith(PostCast);
    I.eraseFromParent();
  }
}
void PointersAnalyzer::visitBitCastInst(BitCastInst &I) {
  //All the ptr related Bitcasts wil be handled in function
  // removeRedundentBitCast
  //Instructions will be skipped at this monment
  return;
}

void PointersAnalyzer::visitAddrSpaceCastInst(AddrSpaceCastInst &I) {
  LLVMContext &Ctx = I.getContext();
  IRBuilder<> B(&I);
  Value *SrcOp = I.getPointerOperand();
  if(SrcOp->getType()->isOpaquePointerTy()) {
    Type* Int8Ptr = PointerType::get(Type::getInt8Ty(Ctx), SrcOp->getType()->getPointerAddressSpace(), true);
    Type* DestInt8Ptr = PointerType::get(Type::getInt8Ty(Ctx), I.getType()->getPointerAddressSpace(), true);
    Value *PreCast = B.CreateBitCast(SrcOp, Int8Ptr);
    Value *NewCast = B.CreateAddrSpaceCast(PreCast, DestInt8Ptr);
    Value *PostCast = B.CreateBitCast(NewCast, I.getType());
    I.replaceAllUsesWith(PostCast);
    I.eraseFromParent();
  }

}

void PointersAnalyzer::visitGetElementPtrInst(GetElementPtrInst &I) {
  LLVMContext &Ctx = I.getContext();
  auto SrcPtr = I.getOperand(0);
  IRBuilder<> B(&I);
  auto BB = I.getParent();

  if(SrcPtr->getType()->isOpaquePointerTy()) {
    Type* SrcTy = I.getSourceElementType();

    //%152 = getelementptr ptr addrspace(1), ptr addrspace(3) @global_smem, i64 %56, !dbg !35
    if(SrcTy->isOpaquePointerTy()) {
      //%152 = getelementptr i8* addrspace(1), i8** @global_smem, i64 %56, !dbg !35
      SrcTy = PointerType::get(Type::getInt8Ty(Ctx), SrcTy->getPointerAddressSpace(), true);
    } else if (isContainOpaquePointers(SrcTy)) {
      // structure type
      // %13 = getelementptr inbounds { i32, i32, i32, i32, ptr, i32 }, ptr %12, i64 0, i32 1
      if(StructType * STy = dyn_cast<StructType>(SrcTy)) {
        std::vector<Type*> TyList;
        for(auto SubTy : STy->subtypes()) {
          if(SubTy->isAggregateType()) {
            report_fatal_error("New nested isAggregateType need to handle");
          } else if(SubTy->isOpaquePointerTy()) {
            Type* Int8Ptr = PointerType::get(Type::getInt8Ty(Ctx), SubTy->getPointerAddressSpace(), true);
            TyList.push_back(Int8Ptr);
          } else {
            TyList.push_back(SubTy);
          }
        }
        SrcTy =  StructType::get(Ctx, TyList);
      } else {
        report_fatal_error("New isAggregateType need to handle");
      }
    }

    LLVM_DEBUG(llvm::dbgs() << "Meet GEP: \n";
               I.print(llvm::dbgs());
               llvm::dbgs() << "\n"; );

    auto TypedPtrTy = PointerType::get(SrcTy, SrcPtr->getType()->getPointerAddressSpace(), true);
    Value* TypedSrc = processPtr(B, SrcPtr, TypedPtrTy);

    std::vector<Value*>Idx;
    for(auto Iter = I.idx_begin(); Iter != I.idx_end(); Iter++) {
      Idx.push_back(*Iter);
    }

    Value *NewGEP = B.CreateGEP(SrcTy, TypedSrc, Idx, "", I.isInBounds());
    //using mutateType introduce risk
    Type *GEPRetTy = GetElementPtrInst::getGEPReturnType(SrcTy, TypedSrc, Idx, true);
    NewGEP->mutateType(GEPRetTy);

    Value* TransTemp = B.CreateBitCast(NewGEP, SrcPtr->getType());

    I.replaceAllUsesWith(TransTemp);
    I.eraseFromParent();

    LLVM_DEBUG(llvm::dbgs() << "New GEP: \n";
               NewGEP->print(llvm::dbgs());
               llvm::dbgs() << "\nGEP return type: ";
               GEPRetTy->print(llvm::dbgs());
               llvm::dbgs() << "\n";);
    Changed |= true;
  }

}


void PointersAnalyzer::visitCallInst(CallInst &I) {
  IRBuilder<> B(&I);
  if(CallInst *CI = dyn_cast<CallInst>(&I)) {
    const Value * op = CI->getCalledOperand();
    if(const InlineAsm *IA = dyn_cast<InlineAsm>(op)) {
      for(unsigned Idx = 0; Idx < CI->arg_size(); Idx++) {
        Value* Arg = CI->getArgOperand(Idx);
        if(Arg->getType()->isOpaquePointerTy()) {
          auto TypedPtrTy = PointerType::get(B.getInt8Ty(), Arg->getType()->getPointerAddressSpace(), true);
          Value *TypedSrc = B.CreateBitCast(Arg, TypedPtrTy);
          CI->setArgOperand(Idx, TypedSrc);
          Changed |= true;
        }
      }
    }
  }
}


//void PointersAnalyzer::visitPHINode(PHINode &I) {
void PointersAnalyzer::processPHINode(PHINode &I) {
  PHINode *PHII = &I;
  Type *PHITy = PHII->getType();
  if(!isContainOpaquePointers(PHITy))
    return;

  if(PHITy->isAggregateType()) {
    //structure type
    if(AggregateMap.find(PHII->getOperand(0)) == AggregateMap.end()) {
      processStructure(PHII->getOperand(0));
    }
    Type *NewPHITy;
    if(AggregateMap.find(PHII->getOperand(0)) != AggregateMap.end()) {
      NewPHITy = AggregateMap[PHII->getOperand(0)]->getType();
    } else {
      NewPHITy = processType(PHII->getOperand(0)->getType());
    }

    IRBuilder<> B(dyn_cast<Instruction>(PHII));
    auto NewPHI = B.CreatePHI(NewPHITy, PHII->getNumIncomingValues());

    for(unsigned Idx = 0; Idx <PHII->getNumIncomingValues(); Idx++) {
      Value *Opnd = PHII->getIncomingValue(Idx);
      if(AggregateMap.find(Opnd) == AggregateMap.end()) {
        //The constant expression structure will be resolved here.
        processStructure(Opnd);
      }
      Value * NewOpnd;
      if(AggregateMap.find(Opnd) != AggregateMap.end()) {
        NewOpnd = AggregateMap[Opnd];
      } else {
        NewOpnd = processPtr(B, Opnd, NewPHITy);
      }
      NewPHI->addIncoming(NewOpnd, PHII->getIncomingBlock(Idx));
    }
    Value::user_iterator Next;
    for(Value::user_iterator UserI = PHII->user_begin(), UserE = PHII->user_end();
                          UserI != UserE; UserI = Next) {
      Next = std::next(UserI);
      if(ExtractValueInst* ExtractI = dyn_cast<ExtractValueInst>(*UserI)) {
        IRBuilder<> B(dyn_cast<Instruction>(*UserI));
        Value *NewExtract = B.CreateExtractValue(NewPHI, ExtractI->getIndices());
        if(NewExtract->getType() != ExtractI->getType()) {
          NewExtract = B.CreateBitCast(NewExtract, ExtractI->getType());
        }
        ExtractI->replaceAllUsesWith(NewExtract);
        ExtractI->eraseFromParent();
      } else {
        report_fatal_error("Unresolved PHI node use");
      }
    }
    // remove PHI and its creation chain
    if(PHII->user_empty()) {
      PHII->eraseFromParent();
    } else {
      report_fatal_error("Unresolved PHI Node");
    }
  } else {
    //ptr type, need to insert bitcast right behind the operand value.
    LLVMContext &Ctx = PHII->getContext();
    IRBuilder<> B(dyn_cast<Instruction>(PHII));
    Type* Int8Ptr = PointerType::get(Type::getInt8Ty(Ctx), PHII->getType()->getPointerAddressSpace(), true);
    auto NewPHI = B.CreatePHI(Int8Ptr, PHII->getNumIncomingValues());
    for(unsigned Idx = 0; Idx <PHII->getNumIncomingValues(); Idx++) {
      Value *Opnd = PHII->getIncomingValue(Idx);
      Value * NewOpnd = nullptr;
      if(Instruction* NormI = dyn_cast<Instruction>(Opnd)) {
        IRBuilder<> OpndB(dyn_cast<Instruction>(NormI));
        OpndB.SetInsertPoint(NormI->getNextNode());
        NewOpnd = OpndB.CreateBitCast(Opnd, Int8Ptr);
      } else if(ConstantExpr *CE = dyn_cast<ConstantExpr>(Opnd)) {
        if(ConstantExpMap.find(CE) == ConstantExpMap.end()) {
          processConstantExpression(CE);
        }
        NewOpnd = ConstantExpMap[CE];
      } else {
        report_fatal_error("Unresolved PHI node value");
      }
      NewPHI->addIncoming(NewOpnd, PHII->getIncomingBlock(Idx));
    }
    Value * PostCast = B.CreateBitCast(NewPHI, PHII->getType());
    PHII->replaceAllUsesWith(PostCast);
    PHII->eraseFromParent();

    //report_fatal_error("Unresolved PHI node");
  }

}

void PointersAnalyzer::visitExtractElementInst(ExtractElementInst &I) {
  // TODO:
  // All the ptr related ExtractElement should be handled in the Vector
  if(I.getType()->isOpaquePointerTy()) {
    report_fatal_error("Unresolved Vector Extract Elements");
  }
}
void PointersAnalyzer::visitInsertElementInst(InsertElementInst &I) {
  // TODO:
  // All the ptr related InsertElement should be handled in the funtion
  // Vector&removeDeadValue
  return;
}
void PointersAnalyzer::visitExtractValueInst(ExtractValueInst &I) {
  // All the ptr related ExtractValues should be handled in the Structure
  Value *SVal = I.getAggregateOperand();
  if(isContainOpaquePointers(SVal->getType())) {
    IRBuilder<> B(&I);
    if(CallInst* SInt = dyn_cast<CallInst>(SVal)) {
      if(SInt->getIntrinsicID() == Intrinsic::fake_cast) {
        Value *OVal = SInt->getOperand(0);
        Value *NewExtract = B.CreateExtractValue(OVal, I.getIndices());
        Function *FDecl = SInt->getCalledFunction();
        if(NewExtract->getType() != I.getType()) {
          NewExtract = processPtr(B, NewExtract, I.getType());
        }
        I.replaceAllUsesWith(NewExtract);
        I.eraseFromParent();
        if(SInt->use_empty()) {
          SInt->eraseFromParent();
        }
        if(FDecl->use_empty()) {
          FDecl->eraseFromParent();
        }
      }
    }
  }
  return;
}
void PointersAnalyzer::visitInsertValueInst(InsertValueInst &I) {
  // All the ptr related InsertValues should be handled in the Structure&removeDeadValue
  return;
}
void PointersAnalyzer::visitInstruction(Instruction &I) {
  // Check if there are unhandled instuctions
  for(unsigned Idx = 0; Idx < I.getNumOperands(); Idx++) {
    Value* opnd = I.getOperand(Idx);
    if(isContainOpaquePointers(opnd->getType())) {
      I.print(llvm::errs());
      report_fatal_error("Instruction contain opaque pointer need to be handled");
    }

  }
}

/*
void PointersAnalyzer::visitLandingPadInst(LandingPadInst &I) {
  LLVMContext &Ctx = I.getContext();
  IRBuilder<> B(&I);
  if(isContainOpaquePointers(I.getType())) {
    Type* NewTy = processType(I.getType());
    SmallVector<Constant*> Clause;
    for(unsigned Idx = 0; Idx < I.getNumClauses(); Idx ++) {
      auto* CT = I.getClause(Idx);
      auto* NewCT = dyn_cast<Constant>(processPtr(B, CT, NewTy));
      Clause.push_back(NewCT);
    }
    auto* NewInst = B.CreateLandingPad(NewTy, I.getNumClauses());
    for(unsigned Idx = 0; Idx < I.getNumClauses(); Idx ++) {
      NewInst->addClause(Clause[Idx]);
    }
    auto* PostCast = processPtr(B, NewInst, I.getType());
    I.replaceAllUsesWith(PostCast);
    I.eraseFromParent();
  }
}
*/

void PointersAnalyzer::visitSelectInst(SelectInst &I) {
  LLVMContext &Ctx = I.getContext();
  IRBuilder<> B(&I);
  Value* Condition = I.getCondition();
  Value* TrueVal = I.getTrueValue();
  Value* FalseVal = I.getFalseValue();
  if(!I.getType()->isOpaquePointerTy())
    return;
  if(TrueVal->getType()->isOpaquePointerTy()) {
    Type* Int8Ptr = PointerType::get(Type::getInt8Ty(Ctx), TrueVal->getType()->getPointerAddressSpace(), true);
    TrueVal = B.CreateBitCast(TrueVal, Int8Ptr);
  } else {
    report_fatal_error("Unconsistent select operand");
  }
  if(FalseVal->getType()->isOpaquePointerTy()) {
    Type* Int8Ptr = PointerType::get(Type::getInt8Ty(Ctx), FalseVal->getType()->getPointerAddressSpace(), true);
    FalseVal = B.CreateBitCast(FalseVal, Int8Ptr);
  } else {
    report_fatal_error("Unconsistent select operand");
  }
  Type* Int8Ptr = PointerType::get(Type::getInt8Ty(Ctx), I.getType()->getPointerAddressSpace(), true);
  Value* NewSel = B.CreateSelect(Condition, TrueVal, FalseVal);
  Value* PostCast = B.CreateBitCast(NewSel, I.getType());
  I.replaceAllUsesWith(PostCast);
  I.eraseFromParent();
}

void PointersAnalyzer::visitAtomicCmpXchgInst(AtomicCmpXchgInst &I) {
  IRBuilder<> B(&I);
  Value *PtrOpnd = I.getPointerOperand();
  Type *PtrOpndTy = PtrOpnd->getType();
  if(PtrOpndTy->isOpaquePointerTy()) {
    Value *CompValue = I.getCompareOperand();
    Value *NewValue = I.getNewValOperand();
    Type *NewPtrTy = PointerType::get(CompValue->getType(), PtrOpndTy->getPointerAddressSpace(), true);
    Value *PreCast = B.CreateBitCast(PtrOpnd, NewPtrTy);
    Value *NewInst = B.CreateAtomicCmpXchg(PreCast, CompValue, NewValue, I.getAlign(),
                       I.getSuccessOrdering(), I.getFailureOrdering(), I.getSyncScopeID());
    I.replaceAllUsesWith(NewInst);
    I.eraseFromParent();
  }
}
void PointersAnalyzer::visitAtomicRMWInst(AtomicRMWInst &I) {
  IRBuilder<> B(&I);
  Value *PtrOpnd = I.getPointerOperand();
  Type *PtrOpndTy = PtrOpnd->getType();
  if(PtrOpndTy->isOpaquePointerTy()) {
    Value *SrcValue = I.getValOperand();
    Type *NewPtrTy = PointerType::get(SrcValue->getType(), PtrOpndTy->getPointerAddressSpace(), true);
    Value *PreCast = B.CreateBitCast(PtrOpnd, NewPtrTy);
    Value *NewInst = B.CreateAtomicRMW(I.getOperation(), PreCast, SrcValue, I.getAlign(),
                                      I.getOrdering(), I.getSyncScopeID());
    I.replaceAllUsesWith(NewInst);
    I.eraseFromParent();
  }
}

void PointersAnalyzer::visitReturnInst(ReturnInst &I) {
  Value *RetVal = I.getReturnValue();
  if(RetVal!= nullptr) {
    if(isContainOpaquePointers(RetVal->getType())) {
      IRBuilder<> B(&I);
      Type* RetTy = I.getFunction()->getReturnType();
      Value *NewVal = processPtr(B, RetVal, RetTy);
      Value *NewRet = B.CreateRet(NewVal);
      I.replaceAllUsesWith(NewRet);
      I.eraseFromParent();
    }
  }
}

StructType* PointersAnalyzer::processStructType(StructType* STy) {
  std::vector<Type*> TyList;
  LLVMContext &Ctx = STy->getContext();
  for(auto SubTy : STy->subtypes()) {
    if(isContainOpaquePointers(SubTy)) {
      auto* NewSubTy = processType(SubTy);
      TyList.push_back(NewSubTy);
    } else {
      TyList.push_back(SubTy);
    }
  }
  auto* NewTy = StructType::get(Ctx, TyList);
  return NewTy;
}

ArrayType* PointersAnalyzer::processArrayType(ArrayType* ATy) {
  Type* EltTy = ATy->getElementType();
  auto* NewEltTy = processType(EltTy);
  auto* NewTy = ArrayType::get(NewEltTy, ATy->getNumElements());
  return NewTy;
}

VectorType* PointersAnalyzer::processVectorType(VectorType* VTy) {
  Type* EltTy = VTy->getElementType();
  auto* NewEltTy = processType(EltTy);
  auto NewTy = VectorType::get(NewEltTy, VTy->getElementCount());
  return NewTy;
}

Type* PointersAnalyzer::processType(Type* Ty) {
  IRBuilder<> B(Ty->getContext());
  if(Ty->isOpaquePointerTy()) {
    auto TypedPtrTy = PointerType::get(B.getInt8Ty(), Ty->getPointerAddressSpace(), true);
    return TypedPtrTy;
  } else if (StructType *STy = dyn_cast<StructType>(Ty)) {
    StructType *NewSTy = processStructType(STy);
    return NewSTy;
  } else if (ArrayType *ATy = dyn_cast<ArrayType>(Ty)) {
    ArrayType *NewATy = processArrayType(ATy);
    return NewATy;
  } else if (VectorType *VTy = dyn_cast<VectorType>(Ty)) {
    VectorType *NewVTy = processVectorType(VTy);
    return NewVTy;
  } else {
    report_fatal_error("New nested isAggregateType need to handle");
  }
}


bool PointersAnalyzer::processFunctionArguments(Function* &F) {
  IRBuilder<> B(F->getContext());
  FunctionType* FuncTy = F->getFunctionType();
  std::vector<llvm::Type *> ParamsTys;
  // all function need change because of potential function reference(function pointer)
  bool needChange = true;
  bool ParamChange = false;
  bool RetChange = false;
  bool DeleteFunc = true;
  ConstantAsMetadata *MD = llvm::ConstantAsMetadata::getIfExists(F);
  for(unsigned Idx = 0; Idx < FuncTy->getNumParams(); Idx++) {
    Type* ParamTy = FuncTy->getParamType(Idx);
    if(isContainOpaquePointers(ParamTy)) {
      Type* NewTy = processType(ParamTy);
      ParamsTys.push_back(NewTy);
      ParamChange |= true;
    } else {
      ParamsTys.push_back(ParamTy);
    }
  }

  Type *ResTy = FuncTy->getReturnType();
  if(isContainOpaquePointers(ResTy)) {
    ResTy = processType(ResTy);
    RetChange = true;
  }
  if(needChange) {
    FunctionType* NewFTy = FunctionType::get(ResTy, ParamsTys, FuncTy->isVarArg());
    LLVM_DEBUG(llvm::dbgs() << "new function type: ";
               NewFTy->print(llvm::dbgs());
               llvm::dbgs() << "\n"; );

    Function *NewFn = llvm::Function::Create(NewFTy, F->getLinkage(), F->getAddressSpace(), true);
    NewFn->copyAttributesFrom(F);
    NewFn->setComdat(F->getComdat());
    F->getParent()->getFunctionList().insert(F->getIterator(), NewFn);
    NewFn->takeName(F);

    LLVM_DEBUG(llvm::dbgs() << "new function: ";
               NewFn->print(llvm::dbgs());
               llvm::dbgs() << "\n"; );

    if(!F->isDeclaration()) {
      NewFn->splice(NewFn->begin(), F);
      auto I = NewFn->begin()->begin();
      for(unsigned Idx = 0; Idx < NewFn->arg_size(); Idx++) {
        Value *OldArg = F->getArg(Idx);
        Value *NewArg = NewFn->getArg(Idx);
        if(OldArg->getType() != NewArg->getType()) {
          IRBuilder<> B(&*I);
          Value* CastParam;
          if(OldArg->getType()->isAggregateType()) {
            CastParam = B.CreateIntrinsic(Intrinsic::fake_cast,
              {OldArg->getType(), NewArg->getType()}, NewArg);
          } else {
            CastParam = processPtr(B, NewArg, OldArg->getType());
          }
          //Value* CastParam = B.CreateBitCast(NewArg, OldArg->getType());
          OldArg->replaceAllUsesWith(CastParam);
        } else {
          OldArg->replaceAllUsesWith(NewArg);
        }
      }
    }

    if(NewFn->getName() == "__nvvm_reflect") {
      LLVMContext &Ctx = F->getContext();
      Value::user_iterator Next;
      for(Value::user_iterator UserI = F->user_begin(), UserE = F->user_end();
                          UserI != UserE; UserI = Next) {
        Next = std::next(UserI);
        if(CallInst *CI = dyn_cast<CallInst>(*UserI)) {
          IRBuilder<> B(CI);
          Value* Arg = CI->getArgOperand(0);
          GlobalVariable* GV = dyn_cast<GlobalVariable>(Arg);
          auto TypedPtrTy = PointerType::get(GV->getValueType(), Arg->getType()->getPointerAddressSpace(), true);
          if(Arg->getType()->isOpaquePointerTy())
            Arg->mutateType(TypedPtrTy);

          Value *Idx[2] = {
          ConstantInt::get(Type::getInt32Ty(Ctx), 0),
          ConstantInt::get(Type::getInt32Ty(Ctx), 0)};
          //in the legacy version, the reflect is like 
          //%1 = call i32 @__nvvm_reflect(i8* getelementptr inbounds ([11 x i8], [11 x i8]* @.str, i32 0, i32 0))
          auto TypedTy = GV->getValueType();
          auto RetTy = PointerType::get(B.getInt8Ty(), Arg->getType()->getPointerAddressSpace(), true);
          Constant *GEP =
                    ConstantExpr::getInBoundsGetElementPtr(TypedTy, dyn_cast<Constant>(Arg), Idx, true, true);

          LLVM_DEBUG(llvm::dbgs() << "create reflect GEP: ";
                     GEP->print(llvm::dbgs());
                     llvm::dbgs() << "\n"; );

          CallInst *NewCI = B.CreateCall(NewFn, GEP);
          CI->replaceAllUsesWith(NewCI);
          CI->eraseFromParent();
        } else {
          UserI->replaceUsesOfWith(F, NewFn);
        }
      }
    } else {
      Value::user_iterator Next;
      for(Value::user_iterator UserI = F->user_begin(), UserE = F->user_end();
                          UserI != UserE; UserI = Next) {
        Next = std::next(UserI);
        if(CallBase *CI = dyn_cast<CallBase>(*UserI)) {
          IRBuilder<> B(CI);
          std::vector<Value *> ArgList;

          for(unsigned Idx = 0; Idx < CI->arg_size(); Idx++) {
            Value * Arg = CI->getArgOperand(Idx);
            if(Arg->getType()->isOpaquePointerTy()) {
              auto TypedPtrTy = PointerType::get(B.getInt8Ty(), Arg->getType()->getPointerAddressSpace(), true);
              Value *NewArg = B.CreateBitCast(Arg, TypedPtrTy);
              ArgList.push_back(NewArg);
            } else {
              ArgList.push_back(Arg);
            }
          }

          SmallVector<OperandBundleDef, 1> OpBundles;
          CI->getOperandBundlesAsDefs(OpBundles);

          CallBase *NewCI = nullptr;
          if (InvokeInst *II = dyn_cast<InvokeInst>(CI)) {
            NewCI = InvokeInst::Create(NewFn, II->getNormalDest(), II->getUnwindDest(),
                                      ArgList, OpBundles, "", CI);
          } else {
            NewCI = CallInst::Create(NewFn, ArgList, OpBundles, "", CI);
            cast<CallInst>(NewCI)->setTailCallKind(
                cast<CallInst>(NewCI)->getTailCallKind());
          }
          NewCI->setCallingConv(CI->getCallingConv());
          NewCI->setAttributes(CI->getAttributes());
          NewCI->copyMetadata(*CI, {LLVMContext::MD_prof, LLVMContext::MD_dbg});

          if(RetChange) {
            Value* TransTemp;
            if(CI->getType()->isAggregateType()) {
              TransTemp = B.CreateIntrinsic(Intrinsic::fake_cast,
                          {CI->getType(), NewCI->getType()}, NewCI);
            } else {
              TransTemp = processPtr(B, NewCI, CI->getType());
            }
            //TransTemp = B.CreateBitCast(NewCI, CI->getType());
            CI->replaceAllUsesWith(TransTemp);
          } else {
            CI->replaceAllUsesWith(NewCI);
          }
          CI->eraseFromParent();
        } else if (Constant* CF = dyn_cast<Constant>(*UserI)) {
          DeleteFunc = false;
        } else {
          // Fix up any BlockAddresses that refer to the function.
          UserI->replaceUsesOfWith(F, ConstantExpr::getBitCast(NewFn, F->getType()));
        }
      }
      // Clone metadatas from the old function, including debug info descriptor.
      SmallVector<std::pair<unsigned, MDNode *>, 1> MDs;
      F->getAllMetadata(MDs);
      for (auto MD : MDs)
        NewFn->addMetadata(MD.first, *MD.second);

      // Delete the bitcast that we just created, so that NF does not
      // appear to be address-taken.
      NewFn->removeDeadConstantUsers();
    }

    if(MD) {
      //ConstantAsMetadata * NewMD = llvm::ConstantAsMetadata::get(NewFn);
      //llvm::ConstantAsMetadata::handleRAUW(MD, NewMD);
      MD->handleRAUW(F, NewFn);
      //MD->replaceAllUsesWith(NewMD);
    }

    if(DeleteFunc)
      F->eraseFromParent();
    F = NewFn;

    Changed |= true;
    return true;
  }
  return false;
}


void PointersAnalyzer::removeRedundentBitCast(Module &M){
  for(Function &F : M) {
    std::vector<BitCastInst*> BCListToPtr;
    std::vector<BitCastInst*> BCListFromPtr;
    for(BasicBlock &BB : F) {
      BasicBlock::iterator Next;
      for (BasicBlock::iterator I = BB.begin(), E = BB.end(); 
                                    I != E; I = Next) {
        Next = std::next(I);
        if(BitCastInst * BI = dyn_cast<BitCastInst>(&*I)) {
          if(BI->getOperand(0)->getType()->isOpaquePointerTy()) {
            BCListFromPtr.push_back(BI);
          } else if (BI->getType()->isOpaquePointerTy()) {
            BCListToPtr.push_back(BI);
          }
        }
      }
    }

    //find the pair that
    // bitcast typedptr to ptr
    // bitcast ptr to typedptr
    for(auto BIT : BCListToPtr) {
      LLVM_DEBUG(llvm::dbgs() << "find bitcast to ptr: ";
                  BIT->print(llvm::dbgs());
                  llvm::dbgs() << "\n"; );
      Value *Src = BIT->getOperand(0);
      Type *SrcTy = Src->getType();

      Value::user_iterator Next;
      for(Value::user_iterator UserI = BIT->user_begin(), UserE = BIT->user_end();
                            UserI != UserE; UserI = Next) {
        Next = std::next(UserI);
        if(BitCastInst *BI = dyn_cast<BitCastInst>(*UserI)) {
          IRBuilder<> B(BI);
          if(BI->getType() == SrcTy) {
            LLVM_DEBUG(llvm::dbgs() << "eliminate reversed bitcast: ";
                        BI->print(llvm::dbgs());
                        llvm::dbgs() << "\n"; );
            BI->replaceAllUsesWith(Src);
            BI->eraseFromParent();
          } else {
            LLVM_DEBUG(llvm::dbgs() << "merge bitcast: ";
                        BI->print(llvm::dbgs());
                        llvm::dbgs() << "\n"; );
            //merge bitcast
            Value* TransNew = B.CreateBitCast(Src, BI->getType());
            BI->replaceAllUsesWith(TransNew);
            BI->eraseFromParent();
          }
        }
      }
      if(BIT->use_empty()) {
        BIT->eraseFromParent();
      }
    }
  }
}

void PointersAnalyzer::removeFakeCast(Module &M) {
  SmallSet<Function*, 8> IntriscsDecl;
  for(Function &F : M) {
    std::vector<CallInst*> CastListToPtr;
    std::vector<CallInst*> CastListFromPtr;
    for(BasicBlock &BB : F) {
      BasicBlock::iterator Next;
      for (BasicBlock::iterator I = BB.begin(), E = BB.end();
                                    I != E; I = Next) {
        Next = std::next(I);
        if(CallInst* SInt = dyn_cast<CallInst>(&*I)) {
          if(SInt->getIntrinsicID() == Intrinsic::fake_cast) {
            IntriscsDecl.insert(SInt->getCalledFunction());
            if(isContainOpaquePointers(SInt->getType())) {
              CastListToPtr.push_back(SInt);
            } else if (isContainOpaquePointers(SInt->getOperand(0)->getType())) {
              CastListFromPtr.push_back(SInt);
            } else  {
              report_fatal_error("error fake cast");
            }
          }
        }
      }
    }
    for(auto* CIT : CastListToPtr) {
      Value *Src = CIT->getOperand(0);
      Type *SrcTy = Src->getType();

      Value::user_iterator Next;
      for(Value::user_iterator UserI = CIT->user_begin(), UserE = CIT->user_end();
                            UserI != UserE; UserI = Next) {
        Next = std::next(UserI);
        if(CallInst* SInt = dyn_cast<CallInst>(*UserI)) {
          if(SInt->getIntrinsicID() == Intrinsic::fake_cast) {
            if(SInt->getType() == SrcTy) {
              SInt->replaceAllUsesWith(Src);
              SInt->eraseFromParent();
            } else {
              report_fatal_error("error fake cast");
            }
          }
        }
      }
      if(CIT->use_empty()) {
        CIT->eraseFromParent();
      }
    }
  }
  for(auto *F : IntriscsDecl) {
    if(F->use_empty()) {
      F->eraseFromParent();
    }
  }
}

bool PointersAnalyzer::processFunction(Function* &F) {
  bool ret = false;
  if(FunctionMap.find(F) != FunctionMap.end() && FunctionMap[F] == false && F) {
    FunctionMap[F] = true;
    ret = processFunctionArguments(F);
    if(F->isDeclaration()) return ret;
    //process function body
    for (BasicBlock &BB : *F) {
      BasicBlock::iterator Next;
      for (BasicBlock::iterator I = BB.begin(), E = BB.end(); 
                                    I != E; I = Next) {
        Next = std::next(I);
        visit(*I);
      }
    }
  }
  return ret;
}


void PointersAnalyzer::processFunctions(Module& M) {
  for (auto &Iter : FunctionMap) {
    Function *F = Iter.first;
    processFunction(F);
  }
}


#define DEBUG_TYPE "verify-pointers"

namespace {
class VerifyAnalyzer:public InstVisitor<VerifyAnalyzer> {

public:
  VerifyAnalyzer()=default;
  void visitInstruction(Instruction &I);
  void visitModule(Module &M);
  void visitFunction(Function &F);
  bool runOnModule(Module &M);
};
}

bool VerifyAnalyzer::runOnModule(Module &M) {
  visit(M);
  return false;
}


void VerifyAnalyzer::visitFunction(Function &F) {
  FunctionType* FuncTy = F.getFunctionType();
  for(auto *EltTy : FuncTy->params()) {
    if(isContainOpaquePointers(EltTy)) {
      F.print(llvm::errs());
      llvm::errs() << "\n";
      report_fatal_error("Function contain opaque pointer need to be handled");
    }
  }
}

void VerifyAnalyzer::visitModule(Module &M) {
  for(auto &GV : M.globals()) {
    if(GV.getType()->isOpaquePointerTy()) {
      GV.print(llvm::errs());
      llvm::errs() << "\n";
      report_fatal_error("Global Variable contain opaque pointer need to be handled");
    }
    if(GV.hasInitializer()) {
      auto* Initializer = GV.getInitializer();
      if(ConstantAggregate* CA = dyn_cast<ConstantAggregate>(Initializer)) {
        if(isContainOpaquePointers(CA->getType())) {
          GV.print(llvm::errs());
          llvm::errs() << "\n";
          report_fatal_error("Global Variable contain opaque pointer need to be handled");
        }
      }
    }
  }
}

void VerifyAnalyzer::visitInstruction(Instruction &I) {

  LLVM_DEBUG(llvm::dbgs() << "Verify instruction: ";
              I.print(llvm::dbgs());
              llvm::dbgs() << "\n"; );

  // Check if there are unhandled instuctions
  for(unsigned Idx = 0; Idx < I.getNumOperands(); Idx++) {
    Value* opnd = I.getOperand(Idx);

    // keep inlineAsm type, because inlineAsm will be only used once
    if(const InlineAsm *IA = dyn_cast<InlineAsm>(opnd))
      continue;

    if(isContainOpaquePointers(opnd->getType())) {
      I.print(llvm::errs());
      llvm::errs() << "\n";
      report_fatal_error("Instruction contain opaque pointer need to be handled");
    }
  }
}


PreservedAnalyses VerifyPointers::run(Module &M, ModuleAnalysisManager &MAM) {
  VerifyAnalyzer Analyzer;

  Analyzer.runOnModule(M);

  return PreservedAnalyses::all();
}