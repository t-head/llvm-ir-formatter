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
#include <map>
#include <set>

#define DEBUG_TYPE "type-pointers"

using namespace llvm;

namespace {
class PointersAnalyzer : public InstVisitor<PointersAnalyzer> {
private:
  bool Changed = false;
  std::vector<Function*> FunctionList;
  std::map<Function*, bool>FunctionMap;
  std::vector<GlobalVariable*> GlobalList;
  std::map<GlobalVariable*, GlobalVariable*> GlobalMap;
  std::map<Value*, Value*> StructMap;
  std::map<Value*, std::vector<Value*>> StructChain;
  std::vector<PHINode*> PHIList;
  std::map<Constant*, Constant*>ConstantExpMap;

  //constant map, hold the global vairable and constant expression.
  //TODO: GlobalMap&ConstantMap should be Merged here
  std::map<Constant*, Constant*>ConstantMap;

  // Holds all new created values
  std::map<Value*, Value*>ValueMap;

public:
  PointersAnalyzer()=default;
  bool isChanged(){ return Changed; };

/*
  void visitAllocaInst(AllocaInst &I);
  void visitGetElementPtrInst(GetElementPtrInst &I);
  void visitCallInst(CallInst &I);
  void visitLoadInst(LoadInst &I);
  void visitStoreInst(StoreInst &I);
  //void visitInstruction(Instruction &I);
  void visitPHINode(PHINode &I);
  void visitAddrSpaceCastInst(AddrSpaceCastInst &I);
  void visitSelectInst(SelectInst &I);
*/

  void visitICmpInst(ICmpInst &I);
  void visitAllocaInst(AllocaInst &I);
  void visitLoadInst(LoadInst     &I);
  void visitStoreInst(StoreInst   &I);
  void visitAtomicCmpXchgInst(AtomicCmpXchgInst &I);
  void visitAtomicRMWInst(AtomicRMWInst &I);
  void visitGetElementPtrInst(GetElementPtrInst &I);
  void visitPHINode(PHINode       &I);
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

  void visitInstruction(Instruction &I);


  void collectStruct(Instruction &I);
  void preparations(Module &M);
  //bool isContainOpaquePointers(Type *Ty);

  void processNVVMReflect(Module &M);
  void processFunctions(Module &M);
  void processGlobals(Module &M);
  bool processFunction(Function* &F);
  bool processFunctionArguments(Function* &F);
  void processStructure(Value* SVal, StructType* NewTy=nullptr);
  void processStructures();
  void processPHI();
  // format the constant expression and map with the typed expressions
  void processConstantExpression(ConstantExpr *CE, GlobalVariable* OldGV=nullptr,
                                 GlobalVariable* NewGV=nullptr);
  Constant* processConstantAggregate(ConstantAggregate *CA);
  void processVector(Value* VecVal);

  Value* processPtr(IRBuilder<> &builder, Value* SrcPtr, Type* TypedPtrTy);

  void removeRedundentBitCast(Module &M);
  void removeDeadValue();

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

  preparations(M);

  processNVVMReflect(M);

  processGlobals(M);

  processStructures();

  //processPHI();

  processFunctions(M);

  removeDeadValue();

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


void PointersAnalyzer::preparations(Module &M) {
  for(auto &V : M.globals()) {
    GlobalList.push_back(&V);
  }

  for(auto &V : M.globals()) {
    GlobalMap.insert(std::make_pair(&V, &V));
  }

  for(Function &F : M.functions()) {
      FunctionList.push_back(&F);
      FunctionMap[&F] = false;
      for (BasicBlock &BB : F) {
        for(Instruction &I : BB) {
          if(InsertValueInst *InsertI = dyn_cast<InsertValueInst>(&I)) {
            collectStruct(*InsertI);
          } else if(PHINode * PHI = dyn_cast<PHINode>(&I)) {
            if(isContainOpaquePointers(PHI->getType())) {
              PHIList.push_back(PHI);
            }
          }
        }
      }
  }
}

void PointersAnalyzer::collectStruct(Instruction &I) {
  if(InsertValueInst * InsertI = dyn_cast<InsertValueInst>(&I)) {
    //suppose all insert start from undef
    //create the stucture map that capture the structur and its inserting chain
    if(isa<UndefValue>(InsertI->getAggregateOperand())) {
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
        if(StructChain.find(InsertI) == StructChain.end()) {
          StructChain.insert(std::make_pair(InsertI, InsertList));
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
  for(auto GVIter : GlobalMap) {
    GlobalVariable *GV = GVIter.first;
    if(GV->getType()->isOpaquePointerTy()) {
      Type* NewTy = PointerType::get(GV->getValueType(), GV->getType()->getPointerAddressSpace(), true);
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
            StructMap.insert(std::make_pair(CStruct, NewCStruct));
          }
        }
      }
    }
  }
}

void PointersAnalyzer::processStructures() {
  for(auto SIter : StructChain) {
    if(StructMap.find(SIter.first) != StructMap.end()) {
      //SVal have been processed before
      //need to check if all use have been replaced
      ;
    } else {
      processStructure(SIter.first);
    }
  }
}

Value* PointersAnalyzer::processPtr(IRBuilder<> &builder, Value* SrcPtr, Type* TypedPtrTy) {
  Value *TypedSrc;
  if(ConstantExpr *CE = dyn_cast<ConstantExpr>(SrcPtr)) {
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
    return CAG;
  }
}

void PointersAnalyzer::processStructure(Value* SVal, StructType* NewTy) {
  LLVMContext &Ctx = SVal->getContext();
  StructType * STy = dyn_cast<StructType>(SVal->getType());
  if(NewTy == nullptr) {
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
    NewTy = StructType::get(Ctx, TyList);
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
    StructMap.insert(std::make_pair(CStruct, NewCStruct));
  } else {
    Value* NewAgg = llvm::PoisonValue::get(NewTy);
    for(Value* InsertVal : StructChain[SVal]) {
      InsertValueInst *InsertI = dyn_cast<InsertValueInst>(InsertVal);
      IRBuilder<> B(dyn_cast<Instruction>(InsertVal));
      auto Indices = InsertI->getIndices();
      Value *IstValue = InsertI->getInsertedValueOperand();
      if(STy->getElementType(Indices[0]) != NewTy->getElementType(Indices[0])) {
        if(Instruction* NormI = dyn_cast<Instruction>(IstValue)) {
          // the inserted value is a normal instrution value
          IstValue = B.CreateBitCast(IstValue, NewTy->getElementType(Indices[0]));
        } else if(ConstantExpr *CE = dyn_cast<ConstantExpr>(IstValue)) {
          if(ConstantExpMap.find(CE) == ConstantExpMap.end()) {
            processConstantExpression(CE);
          }
          IstValue = ConstantExpMap[CE];

        } else {
          report_fatal_error("Unknow type of value inserted in structure");
        }
      }
      NewAgg = B.CreateInsertValue(NewAgg, IstValue, Indices);
    }
    StructMap.insert(std::make_pair(SVal, NewAgg));
    Value::user_iterator Next;
    for(Value::user_iterator UserI = SVal->user_begin(), UserE = SVal->user_end();
                          UserI != UserE; UserI = Next) {
      Next = std::next(UserI);
      if(ExtractValueInst* ExtractI = dyn_cast<ExtractValueInst>(*UserI)) {
        IRBuilder<> B(dyn_cast<Instruction>(*UserI));
        Value *NewExtract = B.CreateExtractValue(NewAgg, ExtractI->getIndices());
        if(NewExtract->getType() != ExtractI->getType()) {
          NewExtract = B.CreateBitCast(NewExtract, ExtractI->getType());
        }
        ExtractI->replaceAllUsesWith(NewExtract);
        ExtractI->eraseFromParent();
      }
    }
  }
}

void PointersAnalyzer::processPHI() {
  for(auto PHII : PHIList) {
    Type *PHITy = PHII->getType();
    if(PHITy->isAggregateType()) {
      //structure type
      if(StructMap.find(PHII->getOperand(0)) == StructMap.end()) {
        processStructure(PHII->getOperand(0));
      }

      Type *NewPHITy = StructMap[PHII->getOperand(0)]->getType();
      IRBuilder<> B(dyn_cast<Instruction>(PHII));
      auto NewPHI = B.CreatePHI(NewPHITy, PHII->getNumIncomingValues());

      for(unsigned Idx = 0; Idx <PHII->getNumIncomingValues(); Idx++) {
        Value *Opnd = PHII->getIncomingValue(Idx);
        if(StructMap.find(Opnd) == StructMap.end()) {
          //The constant expression structure will be resolved here.
          processStructure(Opnd);
        }
        Value * NewOpnd = StructMap[Opnd];
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
}

void PointersAnalyzer::removeDeadValue() {
  for(auto SIter : StructMap) {
    auto SVal = SIter.first;
    // structure has inserting instruction chain
    if(StructChain.find(SVal) != StructChain.end()) {
      auto SChain = StructChain[SVal];
      for(unsigned Idx = SChain.size()-1; Idx != -1U; Idx--) {
        Instruction *IsrtVal = dyn_cast<Instruction>(SChain[Idx]);
        if(IsrtVal->user_empty()) {
          IsrtVal->eraseFromParent();
        } else {
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
        report_fatal_error("Unresolved structure use");
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
        report_fatal_error("Unresolved Global Vairable");
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
  LLVMContext &Ctx = I.getContext();
  IRBuilder<> B(&I);
  Module *M = I.getParent()->getParent()->getParent();
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


void PointersAnalyzer::visitPHINode(PHINode &I) {
  PHINode *PHII = &I;
  Type *PHITy = PHII->getType();
  if(!isContainOpaquePointers(PHITy))
    return;

  if(PHITy->isAggregateType()) {
    //structure type
    if(StructMap.find(PHII->getOperand(0)) == StructMap.end()) {
      processStructure(PHII->getOperand(0));
    }

    Type *NewPHITy = StructMap[PHII->getOperand(0)]->getType();
    IRBuilder<> B(dyn_cast<Instruction>(PHII));
    auto NewPHI = B.CreatePHI(NewPHITy, PHII->getNumIncomingValues());

    for(unsigned Idx = 0; Idx <PHII->getNumIncomingValues(); Idx++) {
      Value *Opnd = PHII->getIncomingValue(Idx);
      if(StructMap.find(Opnd) == StructMap.end()) {
        //The constant expression structure will be resolved here.
        processStructure(Opnd);
      }
      Value * NewOpnd = StructMap[Opnd];
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
  if(isContainOpaquePointers(I.getType())) {
    report_fatal_error("Unresolved Vector Insert Elements");
  }
}
void PointersAnalyzer::visitExtractValueInst(ExtractValueInst &I) {
  // All the ptr related ExtractValues should be handled in the Structure
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
  LLVMContext &Ctx = I.getContext();
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
  LLVMContext &Ctx = I.getContext();
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
     if(RetVal->getType()->isOpaquePointerTy()) {
       //The function return type has been changed in processFuncArguments
       IRBuilder<> B(&I);
       Type *Int8Ptr = PointerType::get(B.getInt8Ty(), RetVal->getType()->getPointerAddressSpace(), true);
       Value *NewVal = B.CreateBitCast(RetVal, Int8Ptr);
       Value *NewRet = B.CreateRet(NewVal);
       I.replaceAllUsesWith(NewRet);
       I.eraseFromParent();
     }
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
    if(ParamTy->isOpaquePointerTy()) {
      auto TypedPtrTy = PointerType::get(B.getInt8Ty(), ParamTy->getPointerAddressSpace(), true);
      ParamsTys.push_back(TypedPtrTy);
      ParamChange |= true;
    } else {
      ParamsTys.push_back(ParamTy);
    }
  }

  Type *ResTy = FuncTy->getReturnType();
  if(ResTy->isOpaquePointerTy()) {
    auto TypedPtrTy = PointerType::get(B.getInt8Ty(), ResTy->getPointerAddressSpace(), true);
    ResTy = TypedPtrTy;
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
          Value* CastParam = B.CreateBitCast(NewArg, OldArg->getType());
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
            Value* TransTemp = B.CreateBitCast(NewCI, CI->getType());
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