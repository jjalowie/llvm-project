//===- GlobalOpt.cpp - Optimize Global Variables --------------------------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This pass transforms simple global variables that never have their address
// taken.  If obviously true, it marks read/write globals as constant, deletes
// variables only stored to, etc.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "globalopt"
#include "llvm/Transforms/IPO.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include <set>
#include <algorithm>
using namespace llvm;

namespace {
  Statistic<> NumMarked   ("globalopt", "Number of globals marked constant");
  Statistic<> NumSRA      ("globalopt", "Number of aggregate globals broken "
                           "into scalars");
  Statistic<> NumSubstitute("globalopt",
                        "Number of globals with initializers stored into them");
  Statistic<> NumDeleted  ("globalopt", "Number of globals deleted");
  Statistic<> NumFnDeleted("globalopt", "Number of functions deleted");
  Statistic<> NumGlobUses ("globalopt", "Number of global uses devirtualized");

  struct GlobalOpt : public ModulePass {
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<TargetData>();
    }
    
    bool runOnModule(Module &M);

  private:
    bool ProcessInternalGlobal(GlobalVariable *GV, Module::giterator &GVI);
  };

  RegisterOpt<GlobalOpt> X("globalopt", "Global Variable Optimizer");
}

ModulePass *llvm::createGlobalOptimizerPass() { return new GlobalOpt(); }

/// GlobalStatus - As we analyze each global, keep track of some information
/// about it.  If we find out that the address of the global is taken, none of
/// this info will be accurate.
struct GlobalStatus {
  /// isLoaded - True if the global is ever loaded.  If the global isn't ever
  /// loaded it can be deleted.
  bool isLoaded;

  /// StoredType - Keep track of what stores to the global look like.
  ///
  enum StoredType {
    /// NotStored - There is no store to this global.  It can thus be marked
    /// constant.
    NotStored,

    /// isInitializerStored - This global is stored to, but the only thing
    /// stored is the constant it was initialized with.  This is only tracked
    /// for scalar globals.
    isInitializerStored,

    /// isStoredOnce - This global is stored to, but only its initializer and
    /// one other value is ever stored to it.  If this global isStoredOnce, we
    /// track the value stored to it in StoredOnceValue below.  This is only
    /// tracked for scalar globals.
    isStoredOnce,

    /// isStored - This global is stored to by multiple values or something else
    /// that we cannot track.
    isStored
  } StoredType;

  /// StoredOnceValue - If only one value (besides the initializer constant) is
  /// ever stored to this global, keep track of what value it is.
  Value *StoredOnceValue;

  /// isNotSuitableForSRA - Keep track of whether any SRA preventing users of
  /// the global exist.  Such users include GEP instruction with variable
  /// indexes, and non-gep/load/store users like constant expr casts.
  bool isNotSuitableForSRA;

  GlobalStatus() : isLoaded(false), StoredType(NotStored), StoredOnceValue(0),
                   isNotSuitableForSRA(false) {}
};



/// ConstantIsDead - Return true if the specified constant is (transitively)
/// dead.  The constant may be used by other constants (e.g. constant arrays and
/// constant exprs) as long as they are dead, but it cannot be used by anything
/// else.
static bool ConstantIsDead(Constant *C) {
  if (isa<GlobalValue>(C)) return false;

  for (Value::use_iterator UI = C->use_begin(), E = C->use_end(); UI != E; ++UI)
    if (Constant *CU = dyn_cast<Constant>(*UI)) {
      if (!ConstantIsDead(CU)) return false;
    } else
      return false;
  return true;
}


/// AnalyzeGlobal - Look at all uses of the global and fill in the GlobalStatus
/// structure.  If the global has its address taken, return true to indicate we
/// can't do anything with it.
///
static bool AnalyzeGlobal(Value *V, GlobalStatus &GS,
                          std::set<PHINode*> &PHIUsers) {
  for (Value::use_iterator UI = V->use_begin(), E = V->use_end(); UI != E; ++UI)
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(*UI)) {
      if (AnalyzeGlobal(CE, GS, PHIUsers)) return true;
      if (CE->getOpcode() != Instruction::GetElementPtr)
        GS.isNotSuitableForSRA = true;
      else if (!GS.isNotSuitableForSRA) {
        // Check to see if this ConstantExpr GEP is SRA'able.  In particular, we
        // don't like < 3 operand CE's, and we don't like non-constant integer
        // indices.
        if (CE->getNumOperands() < 3 || !CE->getOperand(1)->isNullValue())
          GS.isNotSuitableForSRA = true;
        else {
          for (unsigned i = 1, e = CE->getNumOperands(); i != e; ++i)
            if (!isa<ConstantInt>(CE->getOperand(i))) {
              GS.isNotSuitableForSRA = true;
              break;
            }
        }
      }

    } else if (Instruction *I = dyn_cast<Instruction>(*UI)) {
      if (isa<LoadInst>(I)) {
        GS.isLoaded = true;
      } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
        // Don't allow a store OF the address, only stores TO the address.
        if (SI->getOperand(0) == V) return true;

        // If this is a direct store to the global (i.e., the global is a scalar
        // value, not an aggregate), keep more specific information about
        // stores.
        if (GS.StoredType != GlobalStatus::isStored)
          if (GlobalVariable *GV = dyn_cast<GlobalVariable>(SI->getOperand(1))){
            Value *StoredVal = SI->getOperand(0);
            if (StoredVal == GV->getInitializer()) {
              if (GS.StoredType < GlobalStatus::isInitializerStored)
                GS.StoredType = GlobalStatus::isInitializerStored;
            } else if (isa<LoadInst>(StoredVal) &&
                       cast<LoadInst>(StoredVal)->getOperand(0) == GV) {
              // G = G
              if (GS.StoredType < GlobalStatus::isInitializerStored)
                GS.StoredType = GlobalStatus::isInitializerStored;
            } else if (GS.StoredType < GlobalStatus::isStoredOnce) {
              GS.StoredType = GlobalStatus::isStoredOnce;
              GS.StoredOnceValue = StoredVal;
            } else if (GS.StoredType == GlobalStatus::isStoredOnce &&
                       GS.StoredOnceValue == StoredVal) {
              // noop.
            } else {
              GS.StoredType = GlobalStatus::isStored;
            }
          } else {
            GS.StoredType = GlobalStatus::isStored;
          }
      } else if (I->getOpcode() == Instruction::GetElementPtr) {
        if (AnalyzeGlobal(I, GS, PHIUsers)) return true;

        // If the first two indices are constants, this can be SRA'd.
        if (isa<GlobalVariable>(I->getOperand(0))) {
          if (I->getNumOperands() < 3 || !isa<Constant>(I->getOperand(1)) ||
              !cast<Constant>(I->getOperand(1))->isNullValue() || 
              !isa<ConstantInt>(I->getOperand(2)))
            GS.isNotSuitableForSRA = true;
        } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(I->getOperand(0))){
          if (CE->getOpcode() != Instruction::GetElementPtr ||
              CE->getNumOperands() < 3 || I->getNumOperands() < 2 ||
              !isa<Constant>(I->getOperand(0)) ||
              !cast<Constant>(I->getOperand(0))->isNullValue())
            GS.isNotSuitableForSRA = true;
        } else {
          GS.isNotSuitableForSRA = true;
        }
      } else if (I->getOpcode() == Instruction::Select) {
        if (AnalyzeGlobal(I, GS, PHIUsers)) return true;
        GS.isNotSuitableForSRA = true;
      } else if (PHINode *PN = dyn_cast<PHINode>(I)) {
        // PHI nodes we can check just like select or GEP instructions, but we
        // have to be careful about infinite recursion.
        if (PHIUsers.insert(PN).second)  // Not already visited.
          if (AnalyzeGlobal(I, GS, PHIUsers)) return true;
        GS.isNotSuitableForSRA = true;
      } else if (isa<SetCondInst>(I)) {
        GS.isNotSuitableForSRA = true;
      } else {
        return true;  // Any other non-load instruction might take address!
      }
    } else if (Constant *C = dyn_cast<Constant>(*UI)) {
      // We might have a dead and dangling constant hanging off of here.
      if (!ConstantIsDead(C))
        return true;
    } else {
      // Otherwise must be a global or some other user.
      return true;
    }

  return false;
}

static Constant *getAggregateConstantElement(Constant *Agg, Constant *Idx) {
  ConstantInt *CI = dyn_cast<ConstantInt>(Idx);
  if (!CI) return 0;
  uint64_t IdxV = CI->getRawValue();

  if (ConstantStruct *CS = dyn_cast<ConstantStruct>(Agg)) {
    if (IdxV < CS->getNumOperands()) return CS->getOperand(IdxV);
  } else if (ConstantArray *CA = dyn_cast<ConstantArray>(Agg)) {
    if (IdxV < CA->getNumOperands()) return CA->getOperand(IdxV);
  } else if (ConstantPacked *CP = dyn_cast<ConstantPacked>(Agg)) {
    if (IdxV < CP->getNumOperands()) return CP->getOperand(IdxV);
  } else if (isa<ConstantAggregateZero>(Agg)) {
    if (const StructType *STy = dyn_cast<StructType>(Agg->getType())) {
      if (IdxV < STy->getNumElements())
        return Constant::getNullValue(STy->getElementType(IdxV));
    } else if (const SequentialType *STy =
               dyn_cast<SequentialType>(Agg->getType())) {
      return Constant::getNullValue(STy->getElementType());
    }
  } else if (isa<UndefValue>(Agg)) {
    if (const StructType *STy = dyn_cast<StructType>(Agg->getType())) {
      if (IdxV < STy->getNumElements())
        return UndefValue::get(STy->getElementType(IdxV));
    } else if (const SequentialType *STy =
               dyn_cast<SequentialType>(Agg->getType())) {
      return UndefValue::get(STy->getElementType());
    }
  }
  return 0;
}

static Constant *TraverseGEPInitializer(User *GEP, Constant *Init) {
  if (GEP->getNumOperands() == 1 ||
      !isa<Constant>(GEP->getOperand(1)) ||
      !cast<Constant>(GEP->getOperand(1))->isNullValue())
    return 0;

  for (unsigned i = 2, e = GEP->getNumOperands(); i != e; ++i) {
    ConstantInt *Idx = dyn_cast<ConstantInt>(GEP->getOperand(i));
    if (!Idx) return 0;
    Init = getAggregateConstantElement(Init, Idx);
    if (Init == 0) return 0;
  }
  return Init;
}

/// CleanupConstantGlobalUsers - We just marked GV constant.  Loop over all
/// users of the global, cleaning up the obvious ones.  This is largely just a
/// quick scan over the use list to clean up the easy and obvious cruft.  This
/// returns true if it made a change.
static bool CleanupConstantGlobalUsers(Value *V, Constant *Init) {
  bool Changed = false;
  for (Value::use_iterator UI = V->use_begin(), E = V->use_end(); UI != E;) {
    User *U = *UI++;
    
    if (LoadInst *LI = dyn_cast<LoadInst>(U)) {
      // Replace the load with the initializer.
      LI->replaceAllUsesWith(Init);
      LI->eraseFromParent();
      Changed = true;
    } else if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
      // Store must be unreachable or storing Init into the global.
      SI->eraseFromParent();
      Changed = true;
    } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(U)) {
      if (CE->getOpcode() == Instruction::GetElementPtr) {
        if (Constant *SubInit = TraverseGEPInitializer(CE, Init))
          Changed |= CleanupConstantGlobalUsers(CE, SubInit);
        if (CE->use_empty()) {
          CE->destroyConstant();
          Changed = true;
        }
      }
    } else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(U)) {
      if (Constant *SubInit = TraverseGEPInitializer(GEP, Init))
        Changed |= CleanupConstantGlobalUsers(GEP, SubInit);
      else {
        // If this GEP has variable indexes, we should still be able to delete
        // any stores through it.
        for (Value::use_iterator GUI = GEP->use_begin(), E = GEP->use_end();
             GUI != E;)
          if (StoreInst *SI = dyn_cast<StoreInst>(*GUI++)) {
            SI->eraseFromParent();
            Changed = true;
          }
      }

      if (GEP->use_empty()) {
        GEP->eraseFromParent();
        Changed = true;
      }
    } else if (Constant *C = dyn_cast<Constant>(U)) {
      // If we have a chain of dead constantexprs or other things dangling from
      // us, and if they are all dead, nuke them without remorse.
      if (ConstantIsDead(C)) {
        C->destroyConstant();
        // This could have incalidated UI, start over from scratch.x
        CleanupConstantGlobalUsers(V, Init);
        return true;
      }
    }
  }
  return Changed;
}

/// SRAGlobal - Perform scalar replacement of aggregates on the specified global
/// variable.  This opens the door for other optimizations by exposing the
/// behavior of the program in a more fine-grained way.  We have determined that
/// this transformation is safe already.  We return the first global variable we
/// insert so that the caller can reprocess it.
static GlobalVariable *SRAGlobal(GlobalVariable *GV) {
  assert(GV->hasInternalLinkage() && !GV->isConstant());
  Constant *Init = GV->getInitializer();
  const Type *Ty = Init->getType();
  
  std::vector<GlobalVariable*> NewGlobals;
  Module::GlobalListType &Globals = GV->getParent()->getGlobalList();

  if (const StructType *STy = dyn_cast<StructType>(Ty)) {
    NewGlobals.reserve(STy->getNumElements());
    for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i) {
      Constant *In = getAggregateConstantElement(Init,
                                            ConstantUInt::get(Type::UIntTy, i));
      assert(In && "Couldn't get element of initializer?");
      GlobalVariable *NGV = new GlobalVariable(STy->getElementType(i), false,
                                               GlobalVariable::InternalLinkage,
                                               In, GV->getName()+"."+utostr(i));
      Globals.insert(GV, NGV);
      NewGlobals.push_back(NGV);
    }
  } else if (const SequentialType *STy = dyn_cast<SequentialType>(Ty)) {
    unsigned NumElements = 0;
    if (const ArrayType *ATy = dyn_cast<ArrayType>(STy))
      NumElements = ATy->getNumElements();
    else if (const PackedType *PTy = dyn_cast<PackedType>(STy))
      NumElements = PTy->getNumElements();
    else
      assert(0 && "Unknown aggregate sequential type!");

    if (NumElements > 16 && GV->use_size() > 16) return 0; // It's not worth it.
    NewGlobals.reserve(NumElements);
    for (unsigned i = 0, e = NumElements; i != e; ++i) {
      Constant *In = getAggregateConstantElement(Init,
                                            ConstantUInt::get(Type::UIntTy, i));
      assert(In && "Couldn't get element of initializer?");

      GlobalVariable *NGV = new GlobalVariable(STy->getElementType(), false,
                                               GlobalVariable::InternalLinkage,
                                               In, GV->getName()+"."+utostr(i));
      Globals.insert(GV, NGV);
      NewGlobals.push_back(NGV);
    }
  }

  if (NewGlobals.empty())
    return 0;

  DEBUG(std::cerr << "PERFORMING GLOBAL SRA ON: " << *GV);

  Constant *NullInt = Constant::getNullValue(Type::IntTy);

  // Loop over all of the uses of the global, replacing the constantexpr geps,
  // with smaller constantexpr geps or direct references.
  while (!GV->use_empty()) {
    User *GEP = GV->use_back();
    assert(((isa<ConstantExpr>(GEP) &&
             cast<ConstantExpr>(GEP)->getOpcode()==Instruction::GetElementPtr)||
            isa<GetElementPtrInst>(GEP)) && "NonGEP CE's are not SRAable!");
             
    // Ignore the 1th operand, which has to be zero or else the program is quite
    // broken (undefined).  Get the 2nd operand, which is the structure or array
    // index.
    unsigned Val = cast<ConstantInt>(GEP->getOperand(2))->getRawValue();
    if (Val >= NewGlobals.size()) Val = 0; // Out of bound array access.

    Value *NewPtr = NewGlobals[Val];

    // Form a shorter GEP if needed.
    if (GEP->getNumOperands() > 3)
      if (ConstantExpr *CE = dyn_cast<ConstantExpr>(GEP)) {
        std::vector<Constant*> Idxs;
        Idxs.push_back(NullInt);
        for (unsigned i = 3, e = CE->getNumOperands(); i != e; ++i)
          Idxs.push_back(CE->getOperand(i));
        NewPtr = ConstantExpr::getGetElementPtr(cast<Constant>(NewPtr), Idxs);
      } else {
        GetElementPtrInst *GEPI = cast<GetElementPtrInst>(GEP);
        std::vector<Value*> Idxs;
        Idxs.push_back(NullInt);
        for (unsigned i = 3, e = GEPI->getNumOperands(); i != e; ++i)
          Idxs.push_back(GEPI->getOperand(i));
        NewPtr = new GetElementPtrInst(NewPtr, Idxs,
                                       GEPI->getName()+"."+utostr(Val), GEPI);
      }
    GEP->replaceAllUsesWith(NewPtr);

    if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(GEP))
      GEPI->eraseFromParent();
    else
      cast<ConstantExpr>(GEP)->destroyConstant();
  }

  // Delete the old global, now that it is dead.
  Globals.erase(GV);
  ++NumSRA;

  // Loop over the new globals array deleting any globals that are obviously
  // dead.  This can arise due to scalarization of a structure or an array that
  // has elements that are dead.
  unsigned FirstGlobal = 0;
  for (unsigned i = 0, e = NewGlobals.size(); i != e; ++i)
    if (NewGlobals[i]->use_empty()) {
      Globals.erase(NewGlobals[i]);
      if (FirstGlobal == i) ++FirstGlobal;
    }

  return FirstGlobal != NewGlobals.size() ? NewGlobals[FirstGlobal] : 0;
}

/// AllUsesOfValueWillTrapIfNull - Return true if all users of the specified
/// value will trap if the value is dynamically null.
static bool AllUsesOfValueWillTrapIfNull(Value *V) {
  for (Value::use_iterator UI = V->use_begin(), E = V->use_end(); UI != E; ++UI)
    if (isa<LoadInst>(*UI)) {
      // Will trap.
    } else if (StoreInst *SI = dyn_cast<StoreInst>(*UI)) {
      if (SI->getOperand(0) == V) {
        //std::cerr << "NONTRAPPING USE: " << **UI;
        return false;  // Storing the value.
      }
    } else if (CallInst *CI = dyn_cast<CallInst>(*UI)) {
      if (CI->getOperand(0) != V) {
        //std::cerr << "NONTRAPPING USE: " << **UI;
        return false;  // Not calling the ptr
      }
    } else if (InvokeInst *II = dyn_cast<InvokeInst>(*UI)) {
      if (II->getOperand(0) != V) {
        //std::cerr << "NONTRAPPING USE: " << **UI;
        return false;  // Not calling the ptr
      }
    } else if (CastInst *CI = dyn_cast<CastInst>(*UI)) {
      if (!AllUsesOfValueWillTrapIfNull(CI)) return false;
    } else if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(*UI)) {
      if (!AllUsesOfValueWillTrapIfNull(GEPI)) return false;
    } else if (isa<SetCondInst>(*UI) && 
               isa<ConstantPointerNull>(UI->getOperand(1))) {
      // Ignore setcc X, null
    } else {
      //std::cerr << "NONTRAPPING USE: " << **UI;
      return false;
    }
  return true;
}

/// AllUsesOfLoadedValueWillTrapIfNull - Return true if all uses of any loads
/// from GV will trap if the loaded value is null.  Note that this also permits
/// comparisons of the loaded value against null, as a special case.
static bool AllUsesOfLoadedValueWillTrapIfNull(GlobalVariable *GV) {
  for (Value::use_iterator UI = GV->use_begin(), E = GV->use_end(); UI!=E; ++UI)
    if (LoadInst *LI = dyn_cast<LoadInst>(*UI)) {
      if (!AllUsesOfValueWillTrapIfNull(LI))
        return false;
    } else if (isa<StoreInst>(*UI)) {
      // Ignore stores to the global.
    } else {
      // We don't know or understand this user, bail out.
      //std::cerr << "UNKNOWN USER OF GLOBAL!: " << **UI;
      return false;
    }

  return true;
}

static bool OptimizeAwayTrappingUsesOfValue(Value *V, Constant *NewV) {
  bool Changed = false;
  for (Value::use_iterator UI = V->use_begin(), E = V->use_end(); UI != E; ) {
    Instruction *I = cast<Instruction>(*UI++);
    if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
      LI->setOperand(0, NewV);
      Changed = true;
    } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
      if (SI->getOperand(1) == V) {
        SI->setOperand(1, NewV);
        Changed = true;
      }
    } else if (isa<CallInst>(I) || isa<InvokeInst>(I)) {
      if (I->getOperand(0) == V) {
        // Calling through the pointer!  Turn into a direct call, but be careful
        // that the pointer is not also being passed as an argument.
        I->setOperand(0, NewV);
        Changed = true;
        bool PassedAsArg = false;
        for (unsigned i = 1, e = I->getNumOperands(); i != e; ++i)
          if (I->getOperand(i) == V) {
            PassedAsArg = true;
            I->setOperand(i, NewV);
          }

        if (PassedAsArg) {
          // Being passed as an argument also.  Be careful to not invalidate UI!
          UI = V->use_begin();
        }
      }
    } else if (CastInst *CI = dyn_cast<CastInst>(I)) {
      Changed |= OptimizeAwayTrappingUsesOfValue(CI,
                                    ConstantExpr::getCast(NewV, CI->getType()));
      if (CI->use_empty()) {
        Changed = true;
        CI->eraseFromParent();
      }
    } else if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(I)) {
      // Should handle GEP here.
      std::vector<Constant*> Indices;
      Indices.reserve(GEPI->getNumOperands()-1);
      for (unsigned i = 1, e = GEPI->getNumOperands(); i != e; ++i)
        if (Constant *C = dyn_cast<Constant>(GEPI->getOperand(i)))
          Indices.push_back(C);
        else
          break;
      if (Indices.size() == GEPI->getNumOperands()-1)
        Changed |= OptimizeAwayTrappingUsesOfValue(GEPI,
                                ConstantExpr::getGetElementPtr(NewV, Indices));
      if (GEPI->use_empty()) {
        Changed = true;
        GEPI->eraseFromParent();
      }
    }
  }

  return Changed;
}


/// OptimizeAwayTrappingUsesOfLoads - The specified global has only one non-null
/// value stored into it.  If there are uses of the loaded value that would trap
/// if the loaded value is dynamically null, then we know that they cannot be
/// reachable with a null optimize away the load.
static bool OptimizeAwayTrappingUsesOfLoads(GlobalVariable *GV, Constant *LV) {
  std::vector<LoadInst*> Loads;
  bool Changed = false;

  // Replace all uses of loads with uses of uses of the stored value.
  for (Value::use_iterator GUI = GV->use_begin(), E = GV->use_end();
       GUI != E; ++GUI)
    if (LoadInst *LI = dyn_cast<LoadInst>(*GUI)) {
      Loads.push_back(LI);
      Changed |= OptimizeAwayTrappingUsesOfValue(LI, LV);
    } else {
      assert(isa<StoreInst>(*GUI) && "Only expect load and stores!");
    }

  if (Changed) {
    DEBUG(std::cerr << "OPTIMIZED LOADS FROM STORED ONCE POINTER: " << *GV);
    ++NumGlobUses;
  }

  // Delete all of the loads we can, keeping track of whether we nuked them all!
  bool AllLoadsGone = true;
  while (!Loads.empty()) {
    LoadInst *L = Loads.back();
    if (L->use_empty()) {
      L->eraseFromParent();
      Changed = true;
    } else {
      AllLoadsGone = false;
    }
    Loads.pop_back();
  }

  // If we nuked all of the loads, then none of the stores are needed either,
  // nor is the global.
  if (AllLoadsGone) {
    DEBUG(std::cerr << "  *** GLOBAL NOW DEAD!\n");
    CleanupConstantGlobalUsers(GV, 0);
    if (GV->use_empty()) {
      GV->eraseFromParent();
      ++NumDeleted;
    }
    Changed = true;
  }
  return Changed;
}

/// ConstantPropUsersOf - Walk the use list of V, constant folding all of the
/// instructions that are foldable.
static void ConstantPropUsersOf(Value *V) {
  for (Value::use_iterator UI = V->use_begin(), E = V->use_end(); UI != E; )
    if (Instruction *I = dyn_cast<Instruction>(*UI++))
      if (Constant *NewC = ConstantFoldInstruction(I)) {
        I->replaceAllUsesWith(NewC);

        // Back up UI to avoid invalidating it!
        bool AtBegin = false;
        if (UI == V->use_begin())
          AtBegin = true;
        else
          --UI;
        I->eraseFromParent();
        if (AtBegin)
          UI = V->use_begin();
        else
          ++UI;
      }
}

/// OptimizeGlobalAddressOfMalloc - This function takes the specified global
/// variable, and transforms the program as if it always contained the result of
/// the specified malloc.  Because it is always the result of the specified
/// malloc, there is no reason to actually DO the malloc.  Instead, turn the
/// malloc into a global, and any laods of GV as uses of the new global.
static GlobalVariable *OptimizeGlobalAddressOfMalloc(GlobalVariable *GV,
                                                     MallocInst *MI) {
  DEBUG(std::cerr << "PROMOTING MALLOC GLOBAL: " << *GV << "  MALLOC = " <<*MI);
  ConstantInt *NElements = cast<ConstantInt>(MI->getArraySize());

  if (NElements->getRawValue() != 1) {
    // If we have an array allocation, transform it to a single element
    // allocation to make the code below simpler.
    Type *NewTy = ArrayType::get(MI->getAllocatedType(),
                                 NElements->getRawValue());
    MallocInst *NewMI =
      new MallocInst(NewTy, Constant::getNullValue(Type::UIntTy),
                     MI->getName(), MI);
    std::vector<Value*> Indices;
    Indices.push_back(Constant::getNullValue(Type::IntTy));
    Indices.push_back(Indices[0]);
    Value *NewGEP = new GetElementPtrInst(NewMI, Indices,
                                          NewMI->getName()+".el0", MI);
    MI->replaceAllUsesWith(NewGEP);
    MI->eraseFromParent();
    MI = NewMI;
  }
  
  // Create the new global variable.  The contents of the malloc'd memory is
  // undefined, so initialize with an undef value.
  Constant *Init = UndefValue::get(MI->getAllocatedType());
  GlobalVariable *NewGV = new GlobalVariable(MI->getAllocatedType(), false,
                                             GlobalValue::InternalLinkage, Init,
                                             GV->getName()+".body");
  GV->getParent()->getGlobalList().insert(GV, NewGV);
  
  // Anything that used the malloc now uses the global directly.
  MI->replaceAllUsesWith(NewGV);

  Constant *RepValue = NewGV;
  if (NewGV->getType() != GV->getType()->getElementType())
    RepValue = ConstantExpr::getCast(RepValue, GV->getType()->getElementType());

  // If there is a comparison against null, we will insert a global bool to
  // keep track of whether the global was initialized yet or not.
  GlobalVariable *InitBool = 
    new GlobalVariable(Type::BoolTy, false, GlobalValue::InternalLinkage, 
                       ConstantBool::False, GV->getName()+".init");
  bool InitBoolUsed = false;

  // Loop over all uses of GV, processing them in turn.
  std::vector<StoreInst*> Stores;
  while (!GV->use_empty())
    if (LoadInst *LI = dyn_cast<LoadInst>(GV->use_back())) {
      while (!LI->use_empty()) {
        // FIXME: the iterator should expose a getUse() method.
        Use &LoadUse = *(const iplist<Use>::iterator&)LI->use_begin();
        if (!isa<SetCondInst>(LoadUse.getUser()))
          LoadUse = RepValue;
        else {
          // Replace the setcc X, 0 with a use of the bool value.
          SetCondInst *SCI = cast<SetCondInst>(LoadUse.getUser());
          Value *LV = new LoadInst(InitBool, InitBool->getName()+".val", SCI);
          InitBoolUsed = true;
          switch (SCI->getOpcode()) {
          default: assert(0 && "Unknown opcode!");
          case Instruction::SetLT:
            LV = ConstantBool::False;   // X < null -> always false
            break;
          case Instruction::SetEQ:
          case Instruction::SetLE:
            LV = BinaryOperator::createNot(LV, "notinit", SCI);
            break;
          case Instruction::SetNE:
          case Instruction::SetGE:
          case Instruction::SetGT:
            break;  // no change.
          }
          SCI->replaceAllUsesWith(LV);
          SCI->eraseFromParent();
        }
      }
      LI->eraseFromParent();
    } else {
      StoreInst *SI = cast<StoreInst>(GV->use_back());
      // The global is initialized when the store to it occurs.
      new StoreInst(ConstantBool::True, InitBool, SI);
      SI->eraseFromParent();
    }

  // If the initialization boolean was used, insert it, otherwise delete it.
  if (!InitBoolUsed) {
    while (!InitBool->use_empty())  // Delete initializations
      cast<Instruction>(InitBool->use_back())->eraseFromParent();
    delete InitBool;
  } else
    GV->getParent()->getGlobalList().insert(GV, InitBool);


  // Now the GV is dead, nuke it and the malloc.
  GV->eraseFromParent();
  MI->eraseFromParent();

  // To further other optimizations, loop over all users of NewGV and try to
  // constant prop them.  This will promote GEP instructions with constant
  // indices into GEP constant-exprs, which will allow global-opt to hack on it.
  ConstantPropUsersOf(NewGV);
  if (RepValue != NewGV)
    ConstantPropUsersOf(RepValue);

  return NewGV;
}

// OptimizeOnceStoredGlobal - Try to optimize globals based on the knowledge
// that only one value (besides its initializer) is ever stored to the global.
static bool OptimizeOnceStoredGlobal(GlobalVariable *GV, Value *StoredOnceVal,
                                     Module::giterator &GVI, TargetData &TD) {
  if (CastInst *CI = dyn_cast<CastInst>(StoredOnceVal))
    StoredOnceVal = CI->getOperand(0);
  else if (GetElementPtrInst *GEPI =dyn_cast<GetElementPtrInst>(StoredOnceVal)){
    // "getelementptr Ptr, 0, 0, 0" is really just a cast.
    bool IsJustACast = true;
    for (unsigned i = 1, e = GEPI->getNumOperands(); i != e; ++i)
      if (!isa<Constant>(GEPI->getOperand(i)) ||
          !cast<Constant>(GEPI->getOperand(i))->isNullValue()) {
        IsJustACast = false;
        break;
      }
    if (IsJustACast)
      StoredOnceVal = GEPI->getOperand(0);
  }

  // If we are dealing with a pointer global that is initialized to null and
  // only has one (non-null) value stored into it, then we can optimize any
  // users of the loaded value (often calls and loads) that would trap if the
  // value was null.
  if (isa<PointerType>(GV->getInitializer()->getType()) &&
      GV->getInitializer()->isNullValue()) {
    if (Constant *SOVC = dyn_cast<Constant>(StoredOnceVal)) {
      if (GV->getInitializer()->getType() != SOVC->getType())
        SOVC = ConstantExpr::getCast(SOVC, GV->getInitializer()->getType());
      
      // Optimize away any trapping uses of the loaded value.
      if (OptimizeAwayTrappingUsesOfLoads(GV, SOVC))
        return true;
    } else if (MallocInst *MI = dyn_cast<MallocInst>(StoredOnceVal)) {
      // If we have a global that is only initialized with a fixed size malloc,
      // and if all users of the malloc trap, and if the malloc'd address is not
      // put anywhere else, transform the program to use global memory instead
      // of malloc'd memory.  This eliminates dynamic allocation (good) and
      // exposes the resultant global to further GlobalOpt (even better).  Note
      // that we restrict this transformation to only working on small
      // allocations (2048 bytes currently), as we don't want to introduce a 16M
      // global or something.
      if (ConstantInt *NElements = dyn_cast<ConstantInt>(MI->getArraySize()))
        if (MI->getAllocatedType()->isSized() &&
            NElements->getRawValue()*
                     TD.getTypeSize(MI->getAllocatedType()) < 2048 &&
            AllUsesOfLoadedValueWillTrapIfNull(GV)) {
          // FIXME: do more correctness checking to make sure the result of the
          // malloc isn't squirrelled away somewhere.
          GVI = OptimizeGlobalAddressOfMalloc(GV, MI);
          return true;
        }
    }
  }

  return false;
}

/// ProcessInternalGlobal - Analyze the specified global variable and optimize
/// it if possible.  If we make a change, return true.
bool GlobalOpt::ProcessInternalGlobal(GlobalVariable *GV,
                                      Module::giterator &GVI) {
  std::set<PHINode*> PHIUsers;
  GlobalStatus GS;
  PHIUsers.clear();
  GV->removeDeadConstantUsers();

  if (GV->use_empty()) {
    DEBUG(std::cerr << "GLOBAL DEAD: " << *GV);
    GV->eraseFromParent();
    ++NumDeleted;
    return true;
  }

  if (!AnalyzeGlobal(GV, GS, PHIUsers)) {
    // If the global is never loaded (but may be stored to), it is dead.
    // Delete it now.
    if (!GS.isLoaded) {
      DEBUG(std::cerr << "GLOBAL NEVER LOADED: " << *GV);

      // Delete any stores we can find to the global.  We may not be able to
      // make it completely dead though.
      bool Changed = CleanupConstantGlobalUsers(GV, GV->getInitializer());

      // If the global is dead now, delete it.
      if (GV->use_empty()) {
        GV->eraseFromParent();
        ++NumDeleted;
        Changed = true;
      }
      return Changed;
          
    } else if (GS.StoredType <= GlobalStatus::isInitializerStored) {
      DEBUG(std::cerr << "MARKING CONSTANT: " << *GV);
      GV->setConstant(true);
          
      // Clean up any obviously simplifiable users now.
      CleanupConstantGlobalUsers(GV, GV->getInitializer());
          
      // If the global is dead now, just nuke it.
      if (GV->use_empty()) {
        DEBUG(std::cerr << "   *** Marking constant allowed us to simplify "
              "all users and delete global!\n");
        GV->eraseFromParent();
        ++NumDeleted;
      }
          
      ++NumMarked;
      return true;
    } else if (!GS.isNotSuitableForSRA &&
               !GV->getInitializer()->getType()->isFirstClassType()) {
      if (GlobalVariable *FirstNewGV = SRAGlobal(GV)) {
        GVI = FirstNewGV;  // Don't skip the newly produced globals!
        return true;
      }
    } else if (GS.StoredType == GlobalStatus::isStoredOnce) {
      // If the initial value for the global was an undef value, and if only one
      // other value was stored into it, we can just change the initializer to
      // be an undef value, then delete all stores to the global.  This allows
      // us to mark it constant.
      if (isa<UndefValue>(GV->getInitializer()) &&
          isa<Constant>(GS.StoredOnceValue)) {
        // Change the initial value here.
        GV->setInitializer(cast<Constant>(GS.StoredOnceValue));
        
        // Clean up any obviously simplifiable users now.
        CleanupConstantGlobalUsers(GV, GV->getInitializer());

        if (GV->use_empty()) {
          DEBUG(std::cerr << "   *** Substituting initializer allowed us to "
                "simplify all users and delete global!\n");
          GV->eraseFromParent();
          ++NumDeleted;
        } else {
          GVI = GV;
        }
        ++NumSubstitute;
        return true;
      }

      // Try to optimize globals based on the knowledge that only one value
      // (besides its initializer) is ever stored to the global.
      if (OptimizeOnceStoredGlobal(GV, GS.StoredOnceValue, GVI,
                                   getAnalysis<TargetData>()))
        return true;
    }
  }
  return false;
}


bool GlobalOpt::runOnModule(Module &M) {
  bool Changed = false;

  // As a prepass, delete functions that are trivially dead.
  bool LocalChange = true;
  while (LocalChange) {
    LocalChange = false;
    for (Module::iterator FI = M.begin(), E = M.end(); FI != E; ) {
      Function *F = FI++;
      F->removeDeadConstantUsers();
      if (F->use_empty() && (F->hasInternalLinkage() ||
                             F->hasLinkOnceLinkage())) {
        M.getFunctionList().erase(F);
        LocalChange = true;
        ++NumFnDeleted;
      }
    }
    Changed |= LocalChange;
  }

  LocalChange = true;
  while (LocalChange) {
    LocalChange = false;
    for (Module::giterator GVI = M.gbegin(), E = M.gend(); GVI != E;) {
      GlobalVariable *GV = GVI++;
      if (!GV->isConstant() && GV->hasInternalLinkage() &&
          GV->hasInitializer())
        LocalChange |= ProcessInternalGlobal(GV, GVI);
    }
    Changed |= LocalChange;
  }
  return Changed;
}
