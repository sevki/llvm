//===- InlineFunction.cpp - Code to perform function inlining -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements inlining of a function into a call site, resolving
// parameters and the return value as appropriate.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/EHPersonalities.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Support/CommandLine.h"
#include <algorithm>

using namespace llvm;

static cl::opt<bool>
EnableNoAliasConversion("enable-noalias-to-md-conversion", cl::init(true),
  cl::Hidden,
  cl::desc("Convert noalias attributes to metadata during inlining."));

static cl::opt<bool>
PreserveAlignmentAssumptions("preserve-alignment-assumptions-during-inlining",
  cl::init(true), cl::Hidden,
  cl::desc("Convert align attributes to assumptions during inlining."));

bool llvm::InlineFunction(CallInst *CI, InlineFunctionInfo &IFI,
                          AAResults *CalleeAAR, bool InsertLifetime) {
  return InlineFunction(CallSite(CI), IFI, CalleeAAR, InsertLifetime);
}
bool llvm::InlineFunction(InvokeInst *II, InlineFunctionInfo &IFI,
                          AAResults *CalleeAAR, bool InsertLifetime) {
  return InlineFunction(CallSite(II), IFI, CalleeAAR, InsertLifetime);
}

namespace {
  /// A class for recording information about inlining a landing pad.
  class LandingPadInliningInfo {
    BasicBlock *OuterResumeDest; ///< Destination of the invoke's unwind.
    BasicBlock *InnerResumeDest; ///< Destination for the callee's resume.
    LandingPadInst *CallerLPad;  ///< LandingPadInst associated with the invoke.
    PHINode *InnerEHValuesPHI;   ///< PHI for EH values from landingpad insts.
    SmallVector<Value*, 8> UnwindDestPHIValues;

  public:
    LandingPadInliningInfo(InvokeInst *II)
      : OuterResumeDest(II->getUnwindDest()), InnerResumeDest(nullptr),
        CallerLPad(nullptr), InnerEHValuesPHI(nullptr) {
      // If there are PHI nodes in the unwind destination block, we need to keep
      // track of which values came into them from the invoke before removing
      // the edge from this block.
      llvm::BasicBlock *InvokeBB = II->getParent();
      BasicBlock::iterator I = OuterResumeDest->begin();
      for (; isa<PHINode>(I); ++I) {
        // Save the value to use for this edge.
        PHINode *PHI = cast<PHINode>(I);
        UnwindDestPHIValues.push_back(PHI->getIncomingValueForBlock(InvokeBB));
      }

      CallerLPad = cast<LandingPadInst>(I);
    }

    /// The outer unwind destination is the target of
    /// unwind edges introduced for calls within the inlined function.
    BasicBlock *getOuterResumeDest() const {
      return OuterResumeDest;
    }

    BasicBlock *getInnerResumeDest();

    LandingPadInst *getLandingPadInst() const { return CallerLPad; }

    /// Forward the 'resume' instruction to the caller's landing pad block.
    /// When the landing pad block has only one predecessor, this is
    /// a simple branch. When there is more than one predecessor, we need to
    /// split the landing pad block after the landingpad instruction and jump
    /// to there.
    void forwardResume(ResumeInst *RI,
                       SmallPtrSetImpl<LandingPadInst*> &InlinedLPads);

    /// Add incoming-PHI values to the unwind destination block for the given
    /// basic block, using the values for the original invoke's source block.
    void addIncomingPHIValuesFor(BasicBlock *BB) const {
      addIncomingPHIValuesForInto(BB, OuterResumeDest);
    }

    void addIncomingPHIValuesForInto(BasicBlock *src, BasicBlock *dest) const {
      BasicBlock::iterator I = dest->begin();
      for (unsigned i = 0, e = UnwindDestPHIValues.size(); i != e; ++i, ++I) {
        PHINode *phi = cast<PHINode>(I);
        phi->addIncoming(UnwindDestPHIValues[i], src);
      }
    }
  };
} // anonymous namespace

/// Get or create a target for the branch from ResumeInsts.
BasicBlock *LandingPadInliningInfo::getInnerResumeDest() {
  if (InnerResumeDest) return InnerResumeDest;

  // Split the landing pad.
  BasicBlock::iterator SplitPoint = ++CallerLPad->getIterator();
  InnerResumeDest =
    OuterResumeDest->splitBasicBlock(SplitPoint,
                                     OuterResumeDest->getName() + ".body");

  // The number of incoming edges we expect to the inner landing pad.
  const unsigned PHICapacity = 2;

  // Create corresponding new PHIs for all the PHIs in the outer landing pad.
  Instruction *InsertPoint = &InnerResumeDest->front();
  BasicBlock::iterator I = OuterResumeDest->begin();
  for (unsigned i = 0, e = UnwindDestPHIValues.size(); i != e; ++i, ++I) {
    PHINode *OuterPHI = cast<PHINode>(I);
    PHINode *InnerPHI = PHINode::Create(OuterPHI->getType(), PHICapacity,
                                        OuterPHI->getName() + ".lpad-body",
                                        InsertPoint);
    OuterPHI->replaceAllUsesWith(InnerPHI);
    InnerPHI->addIncoming(OuterPHI, OuterResumeDest);
  }

  // Create a PHI for the exception values.
  InnerEHValuesPHI = PHINode::Create(CallerLPad->getType(), PHICapacity,
                                     "eh.lpad-body", InsertPoint);
  CallerLPad->replaceAllUsesWith(InnerEHValuesPHI);
  InnerEHValuesPHI->addIncoming(CallerLPad, OuterResumeDest);

  // All done.
  return InnerResumeDest;
}

/// Forward the 'resume' instruction to the caller's landing pad block.
/// When the landing pad block has only one predecessor, this is a simple
/// branch. When there is more than one predecessor, we need to split the
/// landing pad block after the landingpad instruction and jump to there.
void LandingPadInliningInfo::forwardResume(
    ResumeInst *RI, SmallPtrSetImpl<LandingPadInst *> &InlinedLPads) {
  BasicBlock *Dest = getInnerResumeDest();
  BasicBlock *Src = RI->getParent();

  BranchInst::Create(Dest, Src);

  // Update the PHIs in the destination. They were inserted in an order which
  // makes this work.
  addIncomingPHIValuesForInto(Src, Dest);

  InnerEHValuesPHI->addIncoming(RI->getOperand(0), Src);
  RI->eraseFromParent();
}

/// Helper for getUnwindDestToken/getUnwindDestTokenHelper.
static Value *getParentPad(Value *EHPad) {
  if (auto *FPI = dyn_cast<FuncletPadInst>(EHPad))
    return FPI->getParentPad();
  return cast<CatchSwitchInst>(EHPad)->getParentPad();
}

typedef DenseMap<Instruction *, Value *> UnwindDestMemoTy;

/// Helper for getUnwindDestToken that does the descendant-ward part of
/// the search.
static Value *getUnwindDestTokenHelper(Instruction *EHPad,
                                       UnwindDestMemoTy &MemoMap) {
  SmallVector<Instruction *, 8> Worklist(1, EHPad);

  while (!Worklist.empty()) {
    Instruction *CurrentPad = Worklist.pop_back_val();
    // We only put pads on the worklist that aren't in the MemoMap.  When
    // we find an unwind dest for a pad we may update its ancestors, but
    // the queue only ever contains uncles/great-uncles/etc. of CurrentPad,
    // so they should never get updated while queued on the worklist.
    assert(!MemoMap.count(CurrentPad));
    Value *UnwindDestToken = nullptr;
    if (auto *CatchSwitch = dyn_cast<CatchSwitchInst>(CurrentPad)) {
      if (CatchSwitch->hasUnwindDest()) {
        UnwindDestToken = CatchSwitch->getUnwindDest()->getFirstNonPHI();
      } else {
        // Catchswitch doesn't have a 'nounwind' variant, and one might be
        // annotated as "unwinds to caller" when really it's nounwind (see
        // e.g. SimplifyCFGOpt::SimplifyUnreachable), so we can't infer the
        // parent's unwind dest from this.  We can check its catchpads'
        // descendants, since they might include a cleanuppad with an
        // "unwinds to caller" cleanupret, which can be trusted.
        for (auto HI = CatchSwitch->handler_begin(),
                  HE = CatchSwitch->handler_end();
             HI != HE && !UnwindDestToken; ++HI) {
          BasicBlock *HandlerBlock = *HI;
          auto *CatchPad = cast<CatchPadInst>(HandlerBlock->getFirstNonPHI());
          for (User *Child : CatchPad->users()) {
            // Intentionally ignore invokes here -- since the catchswitch is
            // marked "unwind to caller", it would be a verifier error if it
            // contained an invoke which unwinds out of it, so any invoke we'd
            // encounter must unwind to some child of the catch.
            if (!isa<CleanupPadInst>(Child) && !isa<CatchSwitchInst>(Child))
              continue;

            Instruction *ChildPad = cast<Instruction>(Child);
            auto Memo = MemoMap.find(ChildPad);
            if (Memo == MemoMap.end()) {
              // Haven't figure out this child pad yet; queue it.
              Worklist.push_back(ChildPad);
              continue;
            }
            // We've already checked this child, but might have found that
            // it offers no proof either way.
            Value *ChildUnwindDestToken = Memo->second;
            if (!ChildUnwindDestToken)
              continue;
            // We already know the child's unwind dest, which can either
            // be ConstantTokenNone to indicate unwind to caller, or can
            // be another child of the catchpad.  Only the former indicates
            // the unwind dest of the catchswitch.
            if (isa<ConstantTokenNone>(ChildUnwindDestToken)) {
              UnwindDestToken = ChildUnwindDestToken;
              break;
            }
            assert(getParentPad(ChildUnwindDestToken) == CatchPad);
          }
        }
      }
    } else {
      auto *CleanupPad = cast<CleanupPadInst>(CurrentPad);
      for (User *U : CleanupPad->users()) {
        if (auto *CleanupRet = dyn_cast<CleanupReturnInst>(U)) {
          if (BasicBlock *RetUnwindDest = CleanupRet->getUnwindDest())
            UnwindDestToken = RetUnwindDest->getFirstNonPHI();
          else
            UnwindDestToken = ConstantTokenNone::get(CleanupPad->getContext());
          break;
        }
        Value *ChildUnwindDestToken;
        if (auto *Invoke = dyn_cast<InvokeInst>(U)) {
          ChildUnwindDestToken = Invoke->getUnwindDest()->getFirstNonPHI();
        } else if (isa<CleanupPadInst>(U) || isa<CatchSwitchInst>(U)) {
          Instruction *ChildPad = cast<Instruction>(U);
          auto Memo = MemoMap.find(ChildPad);
          if (Memo == MemoMap.end()) {
            // Haven't resolved this child yet; queue it and keep searching.
            Worklist.push_back(ChildPad);
            continue;
          }
          // We've checked this child, but still need to ignore it if it
          // had no proof either way.
          ChildUnwindDestToken = Memo->second;
          if (!ChildUnwindDestToken)
            continue;
        } else {
          // Not a relevant user of the cleanuppad
          continue;
        }
        // In a well-formed program, the child/invoke must either unwind to
        // an(other) child of the cleanup, or exit the cleanup.  In the
        // first case, continue searching.
        if (isa<Instruction>(ChildUnwindDestToken) &&
            getParentPad(ChildUnwindDestToken) == CleanupPad)
          continue;
        UnwindDestToken = ChildUnwindDestToken;
        break;
      }
    }
    // If we haven't found an unwind dest for CurrentPad, we may have queued its
    // children, so move on to the next in the worklist.
    if (!UnwindDestToken)
      continue;

    // Now we know that CurrentPad unwinds to UnwindDestToken.  It also exits
    // any ancestors of CurrentPad up to but not including UnwindDestToken's
    // parent pad.  Record this in the memo map, and check to see if the
    // original EHPad being queried is one of the ones exited.
    Value *UnwindParent;
    if (auto *UnwindPad = dyn_cast<Instruction>(UnwindDestToken))
      UnwindParent = getParentPad(UnwindPad);
    else
      UnwindParent = nullptr;
    bool ExitedOriginalPad = false;
    for (Instruction *ExitedPad = CurrentPad;
         ExitedPad && ExitedPad != UnwindParent;
         ExitedPad = dyn_cast<Instruction>(getParentPad(ExitedPad))) {
      // Skip over catchpads since they just follow their catchswitches.
      if (isa<CatchPadInst>(ExitedPad))
        continue;
      MemoMap[ExitedPad] = UnwindDestToken;
      ExitedOriginalPad |= (ExitedPad == EHPad);
    }

    if (ExitedOriginalPad)
      return UnwindDestToken;

    // Continue the search.
  }

  // No definitive information is contained within this funclet.
  return nullptr;
}

/// Given an EH pad, find where it unwinds.  If it unwinds to an EH pad,
/// return that pad instruction.  If it unwinds to caller, return
/// ConstantTokenNone.  If it does not have a definitive unwind destination,
/// return nullptr.
///
/// This routine gets invoked for calls in funclets in inlinees when inlining
/// an invoke.  Since many funclets don't have calls inside them, it's queried
/// on-demand rather than building a map of pads to unwind dests up front.
/// Determining a funclet's unwind dest may require recursively searching its
/// descendants, and also ancestors and cousins if the descendants don't provide
/// an answer.  Since most funclets will have their unwind dest immediately
/// available as the unwind dest of a catchswitch or cleanupret, this routine
/// searches top-down from the given pad and then up. To avoid worst-case
/// quadratic run-time given that approach, it uses a memo map to avoid
/// re-processing funclet trees.  The callers that rewrite the IR as they go
/// take advantage of this, for correctness, by checking/forcing rewritten
/// pads' entries to match the original callee view.
static Value *getUnwindDestToken(Instruction *EHPad,
                                 UnwindDestMemoTy &MemoMap) {
  // Catchpads unwind to the same place as their catchswitch;
  // redirct any queries on catchpads so the code below can
  // deal with just catchswitches and cleanuppads.
  if (auto *CPI = dyn_cast<CatchPadInst>(EHPad))
    EHPad = CPI->getCatchSwitch();

  // Check if we've already determined the unwind dest for this pad.
  auto Memo = MemoMap.find(EHPad);
  if (Memo != MemoMap.end())
    return Memo->second;

  // Search EHPad and, if necessary, its descendants.
  Value *UnwindDestToken = getUnwindDestTokenHelper(EHPad, MemoMap);
  assert((UnwindDestToken == nullptr) != (MemoMap.count(EHPad) != 0));
  if (UnwindDestToken)
    return UnwindDestToken;

  // No information is available for this EHPad from itself or any of its
  // descendants.  An unwind all the way out to a pad in the caller would
  // need also to agree with the unwind dest of the parent funclet, so
  // search up the chain to try to find a funclet with information.  Put
  // null entries in the memo map to avoid re-processing as we go up.
  MemoMap[EHPad] = nullptr;
  Instruction *LastUselessPad = EHPad;
  Value *AncestorToken;
  for (AncestorToken = getParentPad(EHPad);
       auto *AncestorPad = dyn_cast<Instruction>(AncestorToken);
       AncestorToken = getParentPad(AncestorToken)) {
    // Skip over catchpads since they just follow their catchswitches.
    if (isa<CatchPadInst>(AncestorPad))
      continue;
    assert(!MemoMap.count(AncestorPad) || MemoMap[AncestorPad]);
    auto AncestorMemo = MemoMap.find(AncestorPad);
    if (AncestorMemo == MemoMap.end()) {
      UnwindDestToken = getUnwindDestTokenHelper(AncestorPad, MemoMap);
    } else {
      UnwindDestToken = AncestorMemo->second;
    }
    if (UnwindDestToken)
      break;
    LastUselessPad = AncestorPad;
  }

  // Since the whole tree under LastUselessPad has no information, it all must
  // match UnwindDestToken; record that to avoid repeating the search.
  SmallVector<Instruction *, 8> Worklist(1, LastUselessPad);
  while (!Worklist.empty()) {
    Instruction *UselessPad = Worklist.pop_back_val();
    assert(!MemoMap.count(UselessPad) || MemoMap[UselessPad] == nullptr);
    MemoMap[UselessPad] = UnwindDestToken;
    if (auto *CatchSwitch = dyn_cast<CatchSwitchInst>(UselessPad)) {
      for (BasicBlock *HandlerBlock : CatchSwitch->handlers())
        for (User *U : HandlerBlock->getFirstNonPHI()->users())
          if (isa<CatchSwitchInst>(U) || isa<CleanupPadInst>(U))
            Worklist.push_back(cast<Instruction>(U));
    } else {
      assert(isa<CleanupPadInst>(UselessPad));
      for (User *U : UselessPad->users())
        if (isa<CatchSwitchInst>(U) || isa<CleanupPadInst>(U))
          Worklist.push_back(cast<Instruction>(U));
    }
  }

  return UnwindDestToken;
}

/// When we inline a basic block into an invoke,
/// we have to turn all of the calls that can throw into invokes.
/// This function analyze BB to see if there are any calls, and if so,
/// it rewrites them to be invokes that jump to InvokeDest and fills in the PHI
/// nodes in that block with the values specified in InvokeDestPHIValues.
static BasicBlock *HandleCallsInBlockInlinedThroughInvoke(
    BasicBlock *BB, BasicBlock *UnwindEdge,
    UnwindDestMemoTy *FuncletUnwindMap = nullptr) {
  for (BasicBlock::iterator BBI = BB->begin(), E = BB->end(); BBI != E; ) {
    Instruction *I = &*BBI++;

    // We only need to check for function calls: inlined invoke
    // instructions require no special handling.
    CallInst *CI = dyn_cast<CallInst>(I);

    if (!CI || CI->doesNotThrow() || isa<InlineAsm>(CI->getCalledValue()))
      continue;

    if (auto FuncletBundle = CI->getOperandBundle(LLVMContext::OB_funclet)) {
      // This call is nested inside a funclet.  If that funclet has an unwind
      // destination within the inlinee, then unwinding out of this call would
      // be UB.  Rewriting this call to an invoke which targets the inlined
      // invoke's unwind dest would give the call's parent funclet multiple
      // unwind destinations, which is something that subsequent EH table
      // generation can't handle and that the veirifer rejects.  So when we
      // see such a call, leave it as a call.
      auto *FuncletPad = cast<Instruction>(FuncletBundle->Inputs[0]);
      Value *UnwindDestToken =
          getUnwindDestToken(FuncletPad, *FuncletUnwindMap);
      if (UnwindDestToken && !isa<ConstantTokenNone>(UnwindDestToken))
        continue;
#ifndef NDEBUG
      Instruction *MemoKey;
      if (auto *CatchPad = dyn_cast<CatchPadInst>(FuncletPad))
        MemoKey = CatchPad->getCatchSwitch();
      else
        MemoKey = FuncletPad;
      assert(FuncletUnwindMap->count(MemoKey) &&
             (*FuncletUnwindMap)[MemoKey] == UnwindDestToken &&
             "must get memoized to avoid confusing later searches");
#endif // NDEBUG
    }

    // Convert this function call into an invoke instruction.  First, split the
    // basic block.
    BasicBlock *Split =
        BB->splitBasicBlock(CI->getIterator(), CI->getName() + ".noexc");

    // Delete the unconditional branch inserted by splitBasicBlock
    BB->getInstList().pop_back();

    // Create the new invoke instruction.
    SmallVector<Value*, 8> InvokeArgs(CI->arg_begin(), CI->arg_end());
    SmallVector<OperandBundleDef, 1> OpBundles;

    CI->getOperandBundlesAsDefs(OpBundles);

    // Note: we're round tripping operand bundles through memory here, and that
    // can potentially be avoided with a cleverer API design that we do not have
    // as of this time.

    InvokeInst *II =
        InvokeInst::Create(CI->getCalledValue(), Split, UnwindEdge, InvokeArgs,
                           OpBundles, CI->getName(), BB);
    II->setDebugLoc(CI->getDebugLoc());
    II->setCallingConv(CI->getCallingConv());
    II->setAttributes(CI->getAttributes());
    
    // Make sure that anything using the call now uses the invoke!  This also
    // updates the CallGraph if present, because it uses a WeakVH.
    CI->replaceAllUsesWith(II);

    // Delete the original call
    Split->getInstList().pop_front();
    return BB;
  }
  return nullptr;
}

/// If we inlined an invoke site, we need to convert calls
/// in the body of the inlined function into invokes.
///
/// II is the invoke instruction being inlined.  FirstNewBlock is the first
/// block of the inlined code (the last block is the end of the function),
/// and InlineCodeInfo is information about the code that got inlined.
static void HandleInlinedLandingPad(InvokeInst *II, BasicBlock *FirstNewBlock,
                                    ClonedCodeInfo &InlinedCodeInfo) {
  BasicBlock *InvokeDest = II->getUnwindDest();

  Function *Caller = FirstNewBlock->getParent();

  // The inlined code is currently at the end of the function, scan from the
  // start of the inlined code to its end, checking for stuff we need to
  // rewrite.
  LandingPadInliningInfo Invoke(II);

  // Get all of the inlined landing pad instructions.
  SmallPtrSet<LandingPadInst*, 16> InlinedLPads;
  for (Function::iterator I = FirstNewBlock->getIterator(), E = Caller->end();
       I != E; ++I)
    if (InvokeInst *II = dyn_cast<InvokeInst>(I->getTerminator()))
      InlinedLPads.insert(II->getLandingPadInst());

  // Append the clauses from the outer landing pad instruction into the inlined
  // landing pad instructions.
  LandingPadInst *OuterLPad = Invoke.getLandingPadInst();
  for (LandingPadInst *InlinedLPad : InlinedLPads) {
    unsigned OuterNum = OuterLPad->getNumClauses();
    InlinedLPad->reserveClauses(OuterNum);
    for (unsigned OuterIdx = 0; OuterIdx != OuterNum; ++OuterIdx)
      InlinedLPad->addClause(OuterLPad->getClause(OuterIdx));
    if (OuterLPad->isCleanup())
      InlinedLPad->setCleanup(true);
  }

  for (Function::iterator BB = FirstNewBlock->getIterator(), E = Caller->end();
       BB != E; ++BB) {
    if (InlinedCodeInfo.ContainsCalls)
      if (BasicBlock *NewBB = HandleCallsInBlockInlinedThroughInvoke(
              &*BB, Invoke.getOuterResumeDest()))
        // Update any PHI nodes in the exceptional block to indicate that there
        // is now a new entry in them.
        Invoke.addIncomingPHIValuesFor(NewBB);

    // Forward any resumes that are remaining here.
    if (ResumeInst *RI = dyn_cast<ResumeInst>(BB->getTerminator()))
      Invoke.forwardResume(RI, InlinedLPads);
  }

  // Now that everything is happy, we have one final detail.  The PHI nodes in
  // the exception destination block still have entries due to the original
  // invoke instruction. Eliminate these entries (which might even delete the
  // PHI node) now.
  InvokeDest->removePredecessor(II->getParent());
}

/// If we inlined an invoke site, we need to convert calls
/// in the body of the inlined function into invokes.
///
/// II is the invoke instruction being inlined.  FirstNewBlock is the first
/// block of the inlined code (the last block is the end of the function),
/// and InlineCodeInfo is information about the code that got inlined.
static void HandleInlinedEHPad(InvokeInst *II, BasicBlock *FirstNewBlock,
                               ClonedCodeInfo &InlinedCodeInfo) {
  BasicBlock *UnwindDest = II->getUnwindDest();
  Function *Caller = FirstNewBlock->getParent();

  assert(UnwindDest->getFirstNonPHI()->isEHPad() && "unexpected BasicBlock!");

  // If there are PHI nodes in the unwind destination block, we need to keep
  // track of which values came into them from the invoke before removing the
  // edge from this block.
  SmallVector<Value *, 8> UnwindDestPHIValues;
  llvm::BasicBlock *InvokeBB = II->getParent();
  for (Instruction &I : *UnwindDest) {
    // Save the value to use for this edge.
    PHINode *PHI = dyn_cast<PHINode>(&I);
    if (!PHI)
      break;
    UnwindDestPHIValues.push_back(PHI->getIncomingValueForBlock(InvokeBB));
  }

  // Add incoming-PHI values to the unwind destination block for the given basic
  // block, using the values for the original invoke's source block.
  auto UpdatePHINodes = [&](BasicBlock *Src) {
    BasicBlock::iterator I = UnwindDest->begin();
    for (Value *V : UnwindDestPHIValues) {
      PHINode *PHI = cast<PHINode>(I);
      PHI->addIncoming(V, Src);
      ++I;
    }
  };

  // This connects all the instructions which 'unwind to caller' to the invoke
  // destination.
  UnwindDestMemoTy FuncletUnwindMap;
  for (Function::iterator BB = FirstNewBlock->getIterator(), E = Caller->end();
       BB != E; ++BB) {
    if (auto *CRI = dyn_cast<CleanupReturnInst>(BB->getTerminator())) {
      if (CRI->unwindsToCaller()) {
        auto *CleanupPad = CRI->getCleanupPad();
        CleanupReturnInst::Create(CleanupPad, UnwindDest, CRI);
        CRI->eraseFromParent();
        UpdatePHINodes(&*BB);
        // Finding a cleanupret with an unwind destination would confuse
        // subsequent calls to getUnwindDestToken, so map the cleanuppad
        // to short-circuit any such calls and recognize this as an "unwind
        // to caller" cleanup.
        assert(!FuncletUnwindMap.count(CleanupPad) ||
               isa<ConstantTokenNone>(FuncletUnwindMap[CleanupPad]));
        FuncletUnwindMap[CleanupPad] =
            ConstantTokenNone::get(Caller->getContext());
      }
    }

    Instruction *I = BB->getFirstNonPHI();
    if (!I->isEHPad())
      continue;

    Instruction *Replacement = nullptr;
    if (auto *CatchSwitch = dyn_cast<CatchSwitchInst>(I)) {
      if (CatchSwitch->unwindsToCaller()) {
        Value *UnwindDestToken;
        if (auto *ParentPad =
                dyn_cast<Instruction>(CatchSwitch->getParentPad())) {
          // This catchswitch is nested inside another funclet.  If that
          // funclet has an unwind destination within the inlinee, then
          // unwinding out of this catchswitch would be UB.  Rewriting this
          // catchswitch to unwind to the inlined invoke's unwind dest would
          // give the parent funclet multiple unwind destinations, which is
          // something that subsequent EH table generation can't handle and
          // that the veirifer rejects.  So when we see such a call, leave it
          // as "unwind to caller".
          UnwindDestToken = getUnwindDestToken(ParentPad, FuncletUnwindMap);
          if (UnwindDestToken && !isa<ConstantTokenNone>(UnwindDestToken))
            continue;
        } else {
          // This catchswitch has no parent to inherit constraints from, and
          // none of its descendants can have an unwind edge that exits it and
          // targets another funclet in the inlinee.  It may or may not have a
          // descendant that definitively has an unwind to caller.  In either
          // case, we'll have to assume that any unwinds out of it may need to
          // be routed to the caller, so treat it as though it has a definitive
          // unwind to caller.
          UnwindDestToken = ConstantTokenNone::get(Caller->getContext());
        }
        auto *NewCatchSwitch = CatchSwitchInst::Create(
            CatchSwitch->getParentPad(), UnwindDest,
            CatchSwitch->getNumHandlers(), CatchSwitch->getName(),
            CatchSwitch);
        for (BasicBlock *PadBB : CatchSwitch->handlers())
          NewCatchSwitch->addHandler(PadBB);
        // Propagate info for the old catchswitch over to the new one in
        // the unwind map.  This also serves to short-circuit any subsequent
        // checks for the unwind dest of this catchswitch, which would get
        // confused if they found the outer handler in the callee.
        FuncletUnwindMap[NewCatchSwitch] = UnwindDestToken;
        Replacement = NewCatchSwitch;
      }
    } else if (!isa<FuncletPadInst>(I)) {
      llvm_unreachable("unexpected EHPad!");
    }

    if (Replacement) {
      Replacement->takeName(I);
      I->replaceAllUsesWith(Replacement);
      I->eraseFromParent();
      UpdatePHINodes(&*BB);
    }
  }

  if (InlinedCodeInfo.ContainsCalls)
    for (Function::iterator BB = FirstNewBlock->getIterator(),
                            E = Caller->end();
         BB != E; ++BB)
      if (BasicBlock *NewBB = HandleCallsInBlockInlinedThroughInvoke(
              &*BB, UnwindDest, &FuncletUnwindMap))
        // Update any PHI nodes in the exceptional block to indicate that there
        // is now a new entry in them.
        UpdatePHINodes(NewBB);

  // Now that everything is happy, we have one final detail.  The PHI nodes in
  // the exception destination block still have entries due to the original
  // invoke instruction. Eliminate these entries (which might even delete the
  // PHI node) now.
  UnwindDest->removePredecessor(InvokeBB);
}

/// When inlining a function that contains noalias scope metadata,
/// this metadata needs to be cloned so that the inlined blocks
/// have different "unqiue scopes" at every call site. Were this not done, then
/// aliasing scopes from a function inlined into a caller multiple times could
/// not be differentiated (and this would lead to miscompiles because the
/// non-aliasing property communicated by the metadata could have
/// call-site-specific control dependencies).
static void CloneAliasScopeMetadata(CallSite CS, ValueToValueMapTy &VMap) {
  const Function *CalledFunc = CS.getCalledFunction();
  SetVector<const MDNode *> MD;

  // Note: We could only clone the metadata if it is already used in the
  // caller. I'm omitting that check here because it might confuse
  // inter-procedural alias analysis passes. We can revisit this if it becomes
  // an efficiency or overhead problem.

  for (Function::const_iterator I = CalledFunc->begin(), IE = CalledFunc->end();
       I != IE; ++I)
    for (BasicBlock::const_iterator J = I->begin(), JE = I->end(); J != JE; ++J) {
      if (const MDNode *M = J->getMetadata(LLVMContext::MD_alias_scope))
        MD.insert(M);
      if (const MDNode *M = J->getMetadata(LLVMContext::MD_noalias))
        MD.insert(M);
    }

  if (MD.empty())
    return;

  // Walk the existing metadata, adding the complete (perhaps cyclic) chain to
  // the set.
  SmallVector<const Metadata *, 16> Queue(MD.begin(), MD.end());
  while (!Queue.empty()) {
    const MDNode *M = cast<MDNode>(Queue.pop_back_val());
    for (unsigned i = 0, ie = M->getNumOperands(); i != ie; ++i)
      if (const MDNode *M1 = dyn_cast<MDNode>(M->getOperand(i)))
        if (MD.insert(M1))
          Queue.push_back(M1);
  }

  // Now we have a complete set of all metadata in the chains used to specify
  // the noalias scopes and the lists of those scopes.
  SmallVector<TempMDTuple, 16> DummyNodes;
  DenseMap<const MDNode *, TrackingMDNodeRef> MDMap;
  for (SetVector<const MDNode *>::iterator I = MD.begin(), IE = MD.end();
       I != IE; ++I) {
    DummyNodes.push_back(MDTuple::getTemporary(CalledFunc->getContext(), None));
    MDMap[*I].reset(DummyNodes.back().get());
  }

  // Create new metadata nodes to replace the dummy nodes, replacing old
  // metadata references with either a dummy node or an already-created new
  // node.
  for (SetVector<const MDNode *>::iterator I = MD.begin(), IE = MD.end();
       I != IE; ++I) {
    SmallVector<Metadata *, 4> NewOps;
    for (unsigned i = 0, ie = (*I)->getNumOperands(); i != ie; ++i) {
      const Metadata *V = (*I)->getOperand(i);
      if (const MDNode *M = dyn_cast<MDNode>(V))
        NewOps.push_back(MDMap[M]);
      else
        NewOps.push_back(const_cast<Metadata *>(V));
    }

    MDNode *NewM = MDNode::get(CalledFunc->getContext(), NewOps);
    MDTuple *TempM = cast<MDTuple>(MDMap[*I]);
    assert(TempM->isTemporary() && "Expected temporary node");

    TempM->replaceAllUsesWith(NewM);
  }

  // Now replace the metadata in the new inlined instructions with the
  // repacements from the map.
  for (ValueToValueMapTy::iterator VMI = VMap.begin(), VMIE = VMap.end();
       VMI != VMIE; ++VMI) {
    if (!VMI->second)
      continue;

    Instruction *NI = dyn_cast<Instruction>(VMI->second);
    if (!NI)
      continue;

    if (MDNode *M = NI->getMetadata(LLVMContext::MD_alias_scope)) {
      MDNode *NewMD = MDMap[M];
      // If the call site also had alias scope metadata (a list of scopes to
      // which instructions inside it might belong), propagate those scopes to
      // the inlined instructions.
      if (MDNode *CSM =
              CS.getInstruction()->getMetadata(LLVMContext::MD_alias_scope))
        NewMD = MDNode::concatenate(NewMD, CSM);
      NI->setMetadata(LLVMContext::MD_alias_scope, NewMD);
    } else if (NI->mayReadOrWriteMemory()) {
      if (MDNode *M =
              CS.getInstruction()->getMetadata(LLVMContext::MD_alias_scope))
        NI->setMetadata(LLVMContext::MD_alias_scope, M);
    }

    if (MDNode *M = NI->getMetadata(LLVMContext::MD_noalias)) {
      MDNode *NewMD = MDMap[M];
      // If the call site also had noalias metadata (a list of scopes with
      // which instructions inside it don't alias), propagate those scopes to
      // the inlined instructions.
      if (MDNode *CSM =
              CS.getInstruction()->getMetadata(LLVMContext::MD_noalias))
        NewMD = MDNode::concatenate(NewMD, CSM);
      NI->setMetadata(LLVMContext::MD_noalias, NewMD);
    } else if (NI->mayReadOrWriteMemory()) {
      if (MDNode *M = CS.getInstruction()->getMetadata(LLVMContext::MD_noalias))
        NI->setMetadata(LLVMContext::MD_noalias, M);
    }
  }
}

/// If the inlined function has noalias arguments,
/// then add new alias scopes for each noalias argument, tag the mapped noalias
/// parameters with noalias metadata specifying the new scope, and tag all
/// non-derived loads, stores and memory intrinsics with the new alias scopes.
static void AddAliasScopeMetadata(CallSite CS, ValueToValueMapTy &VMap,
                                  const DataLayout &DL, AAResults *CalleeAAR) {
  if (!EnableNoAliasConversion)
    return;

  const Function *CalledFunc = CS.getCalledFunction();
  SmallVector<const Argument *, 4> NoAliasArgs;

  for (const Argument &I : CalledFunc->args()) {
    if (I.hasNoAliasAttr() && !I.hasNUses(0))
      NoAliasArgs.push_back(&I);
  }

  if (NoAliasArgs.empty())
    return;

  // To do a good job, if a noalias variable is captured, we need to know if
  // the capture point dominates the particular use we're considering.
  DominatorTree DT;
  DT.recalculate(const_cast<Function&>(*CalledFunc));

  // noalias indicates that pointer values based on the argument do not alias
  // pointer values which are not based on it. So we add a new "scope" for each
  // noalias function argument. Accesses using pointers based on that argument
  // become part of that alias scope, accesses using pointers not based on that
  // argument are tagged as noalias with that scope.

  DenseMap<const Argument *, MDNode *> NewScopes;
  MDBuilder MDB(CalledFunc->getContext());

  // Create a new scope domain for this function.
  MDNode *NewDomain =
    MDB.createAnonymousAliasScopeDomain(CalledFunc->getName());
  for (unsigned i = 0, e = NoAliasArgs.size(); i != e; ++i) {
    const Argument *A = NoAliasArgs[i];

    std::string Name = CalledFunc->getName();
    if (A->hasName()) {
      Name += ": %";
      Name += A->getName();
    } else {
      Name += ": argument ";
      Name += utostr(i);
    }

    // Note: We always create a new anonymous root here. This is true regardless
    // of the linkage of the callee because the aliasing "scope" is not just a
    // property of the callee, but also all control dependencies in the caller.
    MDNode *NewScope = MDB.createAnonymousAliasScope(NewDomain, Name);
    NewScopes.insert(std::make_pair(A, NewScope));
  }

  // Iterate over all new instructions in the map; for all memory-access
  // instructions, add the alias scope metadata.
  for (ValueToValueMapTy::iterator VMI = VMap.begin(), VMIE = VMap.end();
       VMI != VMIE; ++VMI) {
    if (const Instruction *I = dyn_cast<Instruction>(VMI->first)) {
      if (!VMI->second)
        continue;

      Instruction *NI = dyn_cast<Instruction>(VMI->second);
      if (!NI)
        continue;

      bool IsArgMemOnlyCall = false, IsFuncCall = false;
      SmallVector<const Value *, 2> PtrArgs;

      if (const LoadInst *LI = dyn_cast<LoadInst>(I))
        PtrArgs.push_back(LI->getPointerOperand());
      else if (const StoreInst *SI = dyn_cast<StoreInst>(I))
        PtrArgs.push_back(SI->getPointerOperand());
      else if (const VAArgInst *VAAI = dyn_cast<VAArgInst>(I))
        PtrArgs.push_back(VAAI->getPointerOperand());
      else if (const AtomicCmpXchgInst *CXI = dyn_cast<AtomicCmpXchgInst>(I))
        PtrArgs.push_back(CXI->getPointerOperand());
      else if (const AtomicRMWInst *RMWI = dyn_cast<AtomicRMWInst>(I))
        PtrArgs.push_back(RMWI->getPointerOperand());
      else if (ImmutableCallSite ICS = ImmutableCallSite(I)) {
        // If we know that the call does not access memory, then we'll still
        // know that about the inlined clone of this call site, and we don't
        // need to add metadata.
        if (ICS.doesNotAccessMemory())
          continue;

        IsFuncCall = true;
        if (CalleeAAR) {
          FunctionModRefBehavior MRB = CalleeAAR->getModRefBehavior(ICS);
          if (MRB == FMRB_OnlyAccessesArgumentPointees ||
              MRB == FMRB_OnlyReadsArgumentPointees)
            IsArgMemOnlyCall = true;
        }

        for (ImmutableCallSite::arg_iterator AI = ICS.arg_begin(),
             AE = ICS.arg_end(); AI != AE; ++AI) {
          // We need to check the underlying objects of all arguments, not just
          // the pointer arguments, because we might be passing pointers as
          // integers, etc.
          // However, if we know that the call only accesses pointer arguments,
          // then we only need to check the pointer arguments.
          if (IsArgMemOnlyCall && !(*AI)->getType()->isPointerTy())
            continue;

          PtrArgs.push_back(*AI);
        }
      }

      // If we found no pointers, then this instruction is not suitable for
      // pairing with an instruction to receive aliasing metadata.
      // However, if this is a call, this we might just alias with none of the
      // noalias arguments.
      if (PtrArgs.empty() && !IsFuncCall)
        continue;

      // It is possible that there is only one underlying object, but you
      // need to go through several PHIs to see it, and thus could be
      // repeated in the Objects list.
      SmallPtrSet<const Value *, 4> ObjSet;
      SmallVector<Metadata *, 4> Scopes, NoAliases;

      SmallSetVector<const Argument *, 4> NAPtrArgs;
      for (unsigned i = 0, ie = PtrArgs.size(); i != ie; ++i) {
        SmallVector<Value *, 4> Objects;
        GetUnderlyingObjects(const_cast<Value*>(PtrArgs[i]),
                             Objects, DL, /* LI = */ nullptr);

        for (Value *O : Objects)
          ObjSet.insert(O);
      }

      // Figure out if we're derived from anything that is not a noalias
      // argument.
      bool CanDeriveViaCapture = false, UsesAliasingPtr = false;
      for (const Value *V : ObjSet) {
        // Is this value a constant that cannot be derived from any pointer
        // value (we need to exclude constant expressions, for example, that
        // are formed from arithmetic on global symbols).
        bool IsNonPtrConst = isa<ConstantInt>(V) || isa<ConstantFP>(V) ||
                             isa<ConstantPointerNull>(V) ||
                             isa<ConstantDataVector>(V) || isa<UndefValue>(V);
        if (IsNonPtrConst)
          continue;

        // If this is anything other than a noalias argument, then we cannot
        // completely describe the aliasing properties using alias.scope
        // metadata (and, thus, won't add any).
        if (const Argument *A = dyn_cast<Argument>(V)) {
          if (!A->hasNoAliasAttr())
            UsesAliasingPtr = true;
        } else {
          UsesAliasingPtr = true;
        }

        // If this is not some identified function-local object (which cannot
        // directly alias a noalias argument), or some other argument (which,
        // by definition, also cannot alias a noalias argument), then we could
        // alias a noalias argument that has been captured).
        if (!isa<Argument>(V) &&
            !isIdentifiedFunctionLocal(const_cast<Value*>(V)))
          CanDeriveViaCapture = true;
      }

      // A function call can always get captured noalias pointers (via other
      // parameters, globals, etc.).
      if (IsFuncCall && !IsArgMemOnlyCall)
        CanDeriveViaCapture = true;

      // First, we want to figure out all of the sets with which we definitely
      // don't alias. Iterate over all noalias set, and add those for which:
      //   1. The noalias argument is not in the set of objects from which we
      //      definitely derive.
      //   2. The noalias argument has not yet been captured.
      // An arbitrary function that might load pointers could see captured
      // noalias arguments via other noalias arguments or globals, and so we
      // must always check for prior capture.
      for (const Argument *A : NoAliasArgs) {
        if (!ObjSet.count(A) && (!CanDeriveViaCapture ||
                                 // It might be tempting to skip the
                                 // PointerMayBeCapturedBefore check if
                                 // A->hasNoCaptureAttr() is true, but this is
                                 // incorrect because nocapture only guarantees
                                 // that no copies outlive the function, not
                                 // that the value cannot be locally captured.
                                 !PointerMayBeCapturedBefore(A,
                                   /* ReturnCaptures */ false,
                                   /* StoreCaptures */ false, I, &DT)))
          NoAliases.push_back(NewScopes[A]);
      }

      if (!NoAliases.empty())
        NI->setMetadata(LLVMContext::MD_noalias,
                        MDNode::concatenate(
                            NI->getMetadata(LLVMContext::MD_noalias),
                            MDNode::get(CalledFunc->getContext(), NoAliases)));

      // Next, we want to figure out all of the sets to which we might belong.
      // We might belong to a set if the noalias argument is in the set of
      // underlying objects. If there is some non-noalias argument in our list
      // of underlying objects, then we cannot add a scope because the fact
      // that some access does not alias with any set of our noalias arguments
      // cannot itself guarantee that it does not alias with this access
      // (because there is some pointer of unknown origin involved and the
      // other access might also depend on this pointer). We also cannot add
      // scopes to arbitrary functions unless we know they don't access any
      // non-parameter pointer-values.
      bool CanAddScopes = !UsesAliasingPtr;
      if (CanAddScopes && IsFuncCall)
        CanAddScopes = IsArgMemOnlyCall;

      if (CanAddScopes)
        for (const Argument *A : NoAliasArgs) {
          if (ObjSet.count(A))
            Scopes.push_back(NewScopes[A]);
        }

      if (!Scopes.empty())
        NI->setMetadata(
            LLVMContext::MD_alias_scope,
            MDNode::concatenate(NI->getMetadata(LLVMContext::MD_alias_scope),
                                MDNode::get(CalledFunc->getContext(), Scopes)));
    }
  }
}

/// If the inlined function has non-byval align arguments, then
/// add @llvm.assume-based alignment assumptions to preserve this information.
static void AddAlignmentAssumptions(CallSite CS, InlineFunctionInfo &IFI) {
  if (!PreserveAlignmentAssumptions)
    return;
  auto &DL = CS.getCaller()->getParent()->getDataLayout();

  // To avoid inserting redundant assumptions, we should check for assumptions
  // already in the caller. To do this, we might need a DT of the caller.
  DominatorTree DT;
  bool DTCalculated = false;

  Function *CalledFunc = CS.getCalledFunction();
  for (Function::arg_iterator I = CalledFunc->arg_begin(),
                              E = CalledFunc->arg_end();
       I != E; ++I) {
    unsigned Align = I->getType()->isPointerTy() ? I->getParamAlignment() : 0;
    if (Align && !I->hasByValOrInAllocaAttr() && !I->hasNUses(0)) {
      if (!DTCalculated) {
        DT.recalculate(const_cast<Function&>(*CS.getInstruction()->getParent()
                                               ->getParent()));
        DTCalculated = true;
      }

      // If we can already prove the asserted alignment in the context of the
      // caller, then don't bother inserting the assumption.
      Value *Arg = CS.getArgument(I->getArgNo());
      if (getKnownAlignment(Arg, DL, CS.getInstruction(),
                            &IFI.ACT->getAssumptionCache(*CS.getCaller()),
                            &DT) >= Align)
        continue;

      IRBuilder<>(CS.getInstruction())
          .CreateAlignmentAssumption(DL, Arg, Align);
    }
  }
}

/// Once we have cloned code over from a callee into the caller,
/// update the specified callgraph to reflect the changes we made.
/// Note that it's possible that not all code was copied over, so only
/// some edges of the callgraph may remain.
static void UpdateCallGraphAfterInlining(CallSite CS,
                                         Function::iterator FirstNewBlock,
                                         ValueToValueMapTy &VMap,
                                         InlineFunctionInfo &IFI) {
  CallGraph &CG = *IFI.CG;
  const Function *Caller = CS.getInstruction()->getParent()->getParent();
  const Function *Callee = CS.getCalledFunction();
  CallGraphNode *CalleeNode = CG[Callee];
  CallGraphNode *CallerNode = CG[Caller];

  // Since we inlined some uninlined call sites in the callee into the caller,
  // add edges from the caller to all of the callees of the callee.
  CallGraphNode::iterator I = CalleeNode->begin(), E = CalleeNode->end();

  // Consider the case where CalleeNode == CallerNode.
  CallGraphNode::CalledFunctionsVector CallCache;
  if (CalleeNode == CallerNode) {
    CallCache.assign(I, E);
    I = CallCache.begin();
    E = CallCache.end();
  }

  for (; I != E; ++I) {
    const Value *OrigCall = I->first;

    ValueToValueMapTy::iterator VMI = VMap.find(OrigCall);
    // Only copy the edge if the call was inlined!
    if (VMI == VMap.end() || VMI->second == nullptr)
      continue;
    
    // If the call was inlined, but then constant folded, there is no edge to
    // add.  Check for this case.
    Instruction *NewCall = dyn_cast<Instruction>(VMI->second);
    if (!NewCall)
      continue;

    // We do not treat intrinsic calls like real function calls because we
    // expect them to become inline code; do not add an edge for an intrinsic.
    CallSite CS = CallSite(NewCall);
    if (CS && CS.getCalledFunction() && CS.getCalledFunction()->isIntrinsic())
      continue;
    
    // Remember that this call site got inlined for the client of
    // InlineFunction.
    IFI.InlinedCalls.push_back(NewCall);

    // It's possible that inlining the callsite will cause it to go from an
    // indirect to a direct call by resolving a function pointer.  If this
    // happens, set the callee of the new call site to a more precise
    // destination.  This can also happen if the call graph node of the caller
    // was just unnecessarily imprecise.
    if (!I->second->getFunction())
      if (Function *F = CallSite(NewCall).getCalledFunction()) {
        // Indirect call site resolved to direct call.
        CallerNode->addCalledFunction(CallSite(NewCall), CG[F]);

        continue;
      }

    CallerNode->addCalledFunction(CallSite(NewCall), I->second);
  }
  
  // Update the call graph by deleting the edge from Callee to Caller.  We must
  // do this after the loop above in case Caller and Callee are the same.
  CallerNode->removeCallEdgeFor(CS);
}

static void HandleByValArgumentInit(Value *Dst, Value *Src, Module *M,
                                    BasicBlock *InsertBlock,
                                    InlineFunctionInfo &IFI) {
  Type *AggTy = cast<PointerType>(Src->getType())->getElementType();
  IRBuilder<> Builder(InsertBlock, InsertBlock->begin());

  Value *Size = Builder.getInt64(M->getDataLayout().getTypeStoreSize(AggTy));

  // Always generate a memcpy of alignment 1 here because we don't know
  // the alignment of the src pointer.  Other optimizations can infer
  // better alignment.
  Builder.CreateMemCpy(Dst, Src, Size, /*Align=*/1);
}

/// When inlining a call site that has a byval argument,
/// we have to make the implicit memcpy explicit by adding it.
static Value *HandleByValArgument(Value *Arg, Instruction *TheCall,
                                  const Function *CalledFunc,
                                  InlineFunctionInfo &IFI,
                                  unsigned ByValAlignment) {
  PointerType *ArgTy = cast<PointerType>(Arg->getType());
  Type *AggTy = ArgTy->getElementType();

  Function *Caller = TheCall->getParent()->getParent();

  // If the called function is readonly, then it could not mutate the caller's
  // copy of the byval'd memory.  In this case, it is safe to elide the copy and
  // temporary.
  if (CalledFunc->onlyReadsMemory()) {
    // If the byval argument has a specified alignment that is greater than the
    // passed in pointer, then we either have to round up the input pointer or
    // give up on this transformation.
    if (ByValAlignment <= 1)  // 0 = unspecified, 1 = no particular alignment.
      return Arg;

    const DataLayout &DL = Caller->getParent()->getDataLayout();

    // If the pointer is already known to be sufficiently aligned, or if we can
    // round it up to a larger alignment, then we don't need a temporary.
    if (getOrEnforceKnownAlignment(Arg, ByValAlignment, DL, TheCall,
                                   &IFI.ACT->getAssumptionCache(*Caller)) >=
        ByValAlignment)
      return Arg;
    
    // Otherwise, we have to make a memcpy to get a safe alignment.  This is bad
    // for code quality, but rarely happens and is required for correctness.
  }

  // Create the alloca.  If we have DataLayout, use nice alignment.
  unsigned Align =
      Caller->getParent()->getDataLayout().getPrefTypeAlignment(AggTy);

  // If the byval had an alignment specified, we *must* use at least that
  // alignment, as it is required by the byval argument (and uses of the
  // pointer inside the callee).
  Align = std::max(Align, ByValAlignment);
  
  Value *NewAlloca = new AllocaInst(AggTy, nullptr, Align, Arg->getName(), 
                                    &*Caller->begin()->begin());
  IFI.StaticAllocas.push_back(cast<AllocaInst>(NewAlloca));
  
  // Uses of the argument in the function should use our new alloca
  // instead.
  return NewAlloca;
}

// Check whether this Value is used by a lifetime intrinsic.
static bool isUsedByLifetimeMarker(Value *V) {
  for (User *U : V->users()) {
    if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(U)) {
      switch (II->getIntrinsicID()) {
      default: break;
      case Intrinsic::lifetime_start:
      case Intrinsic::lifetime_end:
        return true;
      }
    }
  }
  return false;
}

// Check whether the given alloca already has
// lifetime.start or lifetime.end intrinsics.
static bool hasLifetimeMarkers(AllocaInst *AI) {
  Type *Ty = AI->getType();
  Type *Int8PtrTy = Type::getInt8PtrTy(Ty->getContext(),
                                       Ty->getPointerAddressSpace());
  if (Ty == Int8PtrTy)
    return isUsedByLifetimeMarker(AI);

  // Do a scan to find all the casts to i8*.
  for (User *U : AI->users()) {
    if (U->getType() != Int8PtrTy) continue;
    if (U->stripPointerCasts() != AI) continue;
    if (isUsedByLifetimeMarker(U))
      return true;
  }
  return false;
}

/// Rebuild the entire inlined-at chain for this instruction so that the top of
/// the chain now is inlined-at the new call site.
static DebugLoc
updateInlinedAtInfo(DebugLoc DL, DILocation *InlinedAtNode, LLVMContext &Ctx,
                    DenseMap<const DILocation *, DILocation *> &IANodes) {
  SmallVector<DILocation *, 3> InlinedAtLocations;
  DILocation *Last = InlinedAtNode;
  DILocation *CurInlinedAt = DL;

  // Gather all the inlined-at nodes
  while (DILocation *IA = CurInlinedAt->getInlinedAt()) {
    // Skip any we've already built nodes for
    if (DILocation *Found = IANodes[IA]) {
      Last = Found;
      break;
    }

    InlinedAtLocations.push_back(IA);
    CurInlinedAt = IA;
  }

  // Starting from the top, rebuild the nodes to point to the new inlined-at
  // location (then rebuilding the rest of the chain behind it) and update the
  // map of already-constructed inlined-at nodes.
  for (const DILocation *MD : make_range(InlinedAtLocations.rbegin(),
                                         InlinedAtLocations.rend())) {
    Last = IANodes[MD] = DILocation::getDistinct(
        Ctx, MD->getLine(), MD->getColumn(), MD->getScope(), Last);
  }

  // And finally create the normal location for this instruction, referring to
  // the new inlined-at chain.
  return DebugLoc::get(DL.getLine(), DL.getCol(), DL.getScope(), Last);
}

/// Update inlined instructions' line numbers to
/// to encode location where these instructions are inlined.
static void fixupLineNumbers(Function *Fn, Function::iterator FI,
                             Instruction *TheCall) {
  DebugLoc TheCallDL = TheCall->getDebugLoc();
  if (!TheCallDL)
    return;

  auto &Ctx = Fn->getContext();
  DILocation *InlinedAtNode = TheCallDL;

  // Create a unique call site, not to be confused with any other call from the
  // same location.
  InlinedAtNode = DILocation::getDistinct(
      Ctx, InlinedAtNode->getLine(), InlinedAtNode->getColumn(),
      InlinedAtNode->getScope(), InlinedAtNode->getInlinedAt());

  // Cache the inlined-at nodes as they're built so they are reused, without
  // this every instruction's inlined-at chain would become distinct from each
  // other.
  DenseMap<const DILocation *, DILocation *> IANodes;

  for (; FI != Fn->end(); ++FI) {
    for (BasicBlock::iterator BI = FI->begin(), BE = FI->end();
         BI != BE; ++BI) {
      DebugLoc DL = BI->getDebugLoc();
      if (!DL) {
        // If the inlined instruction has no line number, make it look as if it
        // originates from the call location. This is important for
        // ((__always_inline__, __nodebug__)) functions which must use caller
        // location for all instructions in their function body.

        // Don't update static allocas, as they may get moved later.
        if (auto *AI = dyn_cast<AllocaInst>(BI))
          if (isa<Constant>(AI->getArraySize()))
            continue;

        BI->setDebugLoc(TheCallDL);
      } else {
        BI->setDebugLoc(updateInlinedAtInfo(DL, InlinedAtNode, BI->getContext(), IANodes));
      }
    }
  }
}

/// This function inlines the called function into the basic block of the
/// caller. This returns false if it is not possible to inline this call.
/// The program is still in a well defined state if this occurs though.
///
/// Note that this only does one level of inlining.  For example, if the
/// instruction 'call B' is inlined, and 'B' calls 'C', then the call to 'C' now
/// exists in the instruction stream.  Similarly this will inline a recursive
/// function by one level.
bool llvm::InlineFunction(CallSite CS, InlineFunctionInfo &IFI,
                          AAResults *CalleeAAR, bool InsertLifetime) {
  Instruction *TheCall = CS.getInstruction();
  assert(TheCall->getParent() && TheCall->getParent()->getParent() &&
         "Instruction not in function!");

  // If IFI has any state in it, zap it before we fill it in.
  IFI.reset();
  
  const Function *CalledFunc = CS.getCalledFunction();
  if (!CalledFunc ||              // Can't inline external function or indirect
      CalledFunc->isDeclaration() || // call, or call to a vararg function!
      CalledFunc->getFunctionType()->isVarArg()) return false;

  // The inliner does not know how to inline through calls with operand bundles
  // in general ...
  if (CS.hasOperandBundles()) {
    for (int i = 0, e = CS.getNumOperandBundles(); i != e; ++i) {
      uint32_t Tag = CS.getOperandBundleAt(i).getTagID();
      // ... but it knows how to inline through "deopt" operand bundles ...
      if (Tag == LLVMContext::OB_deopt)
        continue;
      // ... and "funclet" operand bundles.
      if (Tag == LLVMContext::OB_funclet)
        continue;

      return false;
    }
  }

  // If the call to the callee cannot throw, set the 'nounwind' flag on any
  // calls that we inline.
  bool MarkNoUnwind = CS.doesNotThrow();

  BasicBlock *OrigBB = TheCall->getParent();
  Function *Caller = OrigBB->getParent();

  // GC poses two hazards to inlining, which only occur when the callee has GC:
  //  1. If the caller has no GC, then the callee's GC must be propagated to the
  //     caller.
  //  2. If the caller has a differing GC, it is invalid to inline.
  if (CalledFunc->hasGC()) {
    if (!Caller->hasGC())
      Caller->setGC(CalledFunc->getGC());
    else if (CalledFunc->getGC() != Caller->getGC())
      return false;
  }

  // Get the personality function from the callee if it contains a landing pad.
  Constant *CalledPersonality =
      CalledFunc->hasPersonalityFn()
          ? CalledFunc->getPersonalityFn()->stripPointerCasts()
          : nullptr;

  // Find the personality function used by the landing pads of the caller. If it
  // exists, then check to see that it matches the personality function used in
  // the callee.
  Constant *CallerPersonality =
      Caller->hasPersonalityFn()
          ? Caller->getPersonalityFn()->stripPointerCasts()
          : nullptr;
  if (CalledPersonality) {
    if (!CallerPersonality)
      Caller->setPersonalityFn(CalledPersonality);
    // If the personality functions match, then we can perform the
    // inlining. Otherwise, we can't inline.
    // TODO: This isn't 100% true. Some personality functions are proper
    //       supersets of others and can be used in place of the other.
    else if (CalledPersonality != CallerPersonality)
      return false;
  }

  // We need to figure out which funclet the callsite was in so that we may
  // properly nest the callee.
  Instruction *CallSiteEHPad = nullptr;
  if (CallerPersonality) {
    EHPersonality Personality = classifyEHPersonality(CallerPersonality);
    if (isFuncletEHPersonality(Personality)) {
      Optional<OperandBundleUse> ParentFunclet =
          CS.getOperandBundle(LLVMContext::OB_funclet);
      if (ParentFunclet)
        CallSiteEHPad = cast<FuncletPadInst>(ParentFunclet->Inputs.front());

      // OK, the inlining site is legal.  What about the target function?

      if (CallSiteEHPad) {
        if (Personality == EHPersonality::MSVC_CXX) {
          // The MSVC personality cannot tolerate catches getting inlined into
          // cleanup funclets.
          if (isa<CleanupPadInst>(CallSiteEHPad)) {
            // Ok, the call site is within a cleanuppad.  Let's check the callee
            // for catchpads.
            for (const BasicBlock &CalledBB : *CalledFunc) {
              if (isa<CatchSwitchInst>(CalledBB.getFirstNonPHI()))
                return false;
            }
          }
        } else if (isAsynchronousEHPersonality(Personality)) {
          // SEH is even less tolerant, there may not be any sort of exceptional
          // funclet in the callee.
          for (const BasicBlock &CalledBB : *CalledFunc) {
            if (CalledBB.isEHPad())
              return false;
          }
        }
      }
    }
  }

  // Determine if we are dealing with a call in an EHPad which does not unwind
  // to caller.
  bool EHPadForCallUnwindsLocally = false;
  if (CallSiteEHPad && CS.isCall()) {
    UnwindDestMemoTy FuncletUnwindMap;
    Value *CallSiteUnwindDestToken =
        getUnwindDestToken(CallSiteEHPad, FuncletUnwindMap);

    EHPadForCallUnwindsLocally =
        CallSiteUnwindDestToken &&
        !isa<ConstantTokenNone>(CallSiteUnwindDestToken);
  }

  // Get an iterator to the last basic block in the function, which will have
  // the new function inlined after it.
  Function::iterator LastBlock = --Caller->end();

  // Make sure to capture all of the return instructions from the cloned
  // function.
  SmallVector<ReturnInst*, 8> Returns;
  ClonedCodeInfo InlinedFunctionInfo;
  Function::iterator FirstNewBlock;

  { // Scope to destroy VMap after cloning.
    ValueToValueMapTy VMap;
    // Keep a list of pair (dst, src) to emit byval initializations.
    SmallVector<std::pair<Value*, Value*>, 4> ByValInit;

    auto &DL = Caller->getParent()->getDataLayout();

    assert(CalledFunc->arg_size() == CS.arg_size() &&
           "No varargs calls can be inlined!");

    // Calculate the vector of arguments to pass into the function cloner, which
    // matches up the formal to the actual argument values.
    CallSite::arg_iterator AI = CS.arg_begin();
    unsigned ArgNo = 0;
    for (Function::const_arg_iterator I = CalledFunc->arg_begin(),
         E = CalledFunc->arg_end(); I != E; ++I, ++AI, ++ArgNo) {
      Value *ActualArg = *AI;

      // When byval arguments actually inlined, we need to make the copy implied
      // by them explicit.  However, we don't do this if the callee is readonly
      // or readnone, because the copy would be unneeded: the callee doesn't
      // modify the struct.
      if (CS.isByValArgument(ArgNo)) {
        ActualArg = HandleByValArgument(ActualArg, TheCall, CalledFunc, IFI,
                                        CalledFunc->getParamAlignment(ArgNo+1));
        if (ActualArg != *AI)
          ByValInit.push_back(std::make_pair(ActualArg, (Value*) *AI));
      }

      VMap[&*I] = ActualArg;
    }

    // Add alignment assumptions if necessary. We do this before the inlined
    // instructions are actually cloned into the caller so that we can easily
    // check what will be known at the start of the inlined code.
    AddAlignmentAssumptions(CS, IFI);

    // We want the inliner to prune the code as it copies.  We would LOVE to
    // have no dead or constant instructions leftover after inlining occurs
    // (which can happen, e.g., because an argument was constant), but we'll be
    // happy with whatever the cloner can do.
    CloneAndPruneFunctionInto(Caller, CalledFunc, VMap,
                              /*ModuleLevelChanges=*/false, Returns, ".i",
                              &InlinedFunctionInfo, TheCall);

    // Remember the first block that is newly cloned over.
    FirstNewBlock = LastBlock; ++FirstNewBlock;

    // Inject byval arguments initialization.
    for (std::pair<Value*, Value*> &Init : ByValInit)
      HandleByValArgumentInit(Init.first, Init.second, Caller->getParent(),
                              &*FirstNewBlock, IFI);

    Optional<OperandBundleUse> ParentDeopt =
        CS.getOperandBundle(LLVMContext::OB_deopt);
    if (ParentDeopt) {
      SmallVector<OperandBundleDef, 2> OpDefs;

      for (auto &VH : InlinedFunctionInfo.OperandBundleCallSites) {
        Instruction *I = dyn_cast_or_null<Instruction>(VH);
        if (!I) continue;  // instruction was DCE'd or RAUW'ed to undef

        OpDefs.clear();

        CallSite ICS(I);
        OpDefs.reserve(ICS.getNumOperandBundles());

        for (unsigned i = 0, e = ICS.getNumOperandBundles(); i < e; ++i) {
          auto ChildOB = ICS.getOperandBundleAt(i);
          if (ChildOB.getTagID() != LLVMContext::OB_deopt) {
            // If the inlined call has other operand bundles, let them be
            OpDefs.emplace_back(ChildOB);
            continue;
          }

          // It may be useful to separate this logic (of handling operand
          // bundles) out to a separate "policy" component if this gets crowded.
          // Prepend the parent's deoptimization continuation to the newly
          // inlined call's deoptimization continuation.
          std::vector<Value *> MergedDeoptArgs;
          MergedDeoptArgs.reserve(ParentDeopt->Inputs.size() +
                                  ChildOB.Inputs.size());

          MergedDeoptArgs.insert(MergedDeoptArgs.end(),
                                 ParentDeopt->Inputs.begin(),
                                 ParentDeopt->Inputs.end());
          MergedDeoptArgs.insert(MergedDeoptArgs.end(), ChildOB.Inputs.begin(),
                                 ChildOB.Inputs.end());

          OpDefs.emplace_back("deopt", std::move(MergedDeoptArgs));
        }

        Instruction *NewI = nullptr;
        if (isa<CallInst>(I))
          NewI = CallInst::Create(cast<CallInst>(I), OpDefs, I);
        else
          NewI = InvokeInst::Create(cast<InvokeInst>(I), OpDefs, I);

        // Note: the RAUW does the appropriate fixup in VMap, so we need to do
        // this even if the call returns void.
        I->replaceAllUsesWith(NewI);

        VH = nullptr;
        I->eraseFromParent();
      }
    }

    // Update the callgraph if requested.
    if (IFI.CG)
      UpdateCallGraphAfterInlining(CS, FirstNewBlock, VMap, IFI);

    // Update inlined instructions' line number information.
    fixupLineNumbers(Caller, FirstNewBlock, TheCall);

    // Clone existing noalias metadata if necessary.
    CloneAliasScopeMetadata(CS, VMap);

    // Add noalias metadata if necessary.
    AddAliasScopeMetadata(CS, VMap, DL, CalleeAAR);

    // FIXME: We could register any cloned assumptions instead of clearing the
    // whole function's cache.
    if (IFI.ACT)
      IFI.ACT->getAssumptionCache(*Caller).clear();
  }

  // If there are any alloca instructions in the block that used to be the entry
  // block for the callee, move them to the entry block of the caller.  First
  // calculate which instruction they should be inserted before.  We insert the
  // instructions at the end of the current alloca list.
  {
    BasicBlock::iterator InsertPoint = Caller->begin()->begin();
    for (BasicBlock::iterator I = FirstNewBlock->begin(),
         E = FirstNewBlock->end(); I != E; ) {
      AllocaInst *AI = dyn_cast<AllocaInst>(I++);
      if (!AI) continue;
      
      // If the alloca is now dead, remove it.  This often occurs due to code
      // specialization.
      if (AI->use_empty()) {
        AI->eraseFromParent();
        continue;
      }

      if (!isa<Constant>(AI->getArraySize()))
        continue;
      
      // Keep track of the static allocas that we inline into the caller.
      IFI.StaticAllocas.push_back(AI);
      
      // Scan for the block of allocas that we can move over, and move them
      // all at once.
      while (isa<AllocaInst>(I) &&
             isa<Constant>(cast<AllocaInst>(I)->getArraySize())) {
        IFI.StaticAllocas.push_back(cast<AllocaInst>(I));
        ++I;
      }

      // Transfer all of the allocas over in a block.  Using splice means
      // that the instructions aren't removed from the symbol table, then
      // reinserted.
      Caller->getEntryBlock().getInstList().splice(
          InsertPoint, FirstNewBlock->getInstList(), AI->getIterator(), I);
    }
    // Move any dbg.declares describing the allocas into the entry basic block.
    DIBuilder DIB(*Caller->getParent());
    for (auto &AI : IFI.StaticAllocas)
      replaceDbgDeclareForAlloca(AI, AI, DIB, /*Deref=*/false);
  }

  bool InlinedMustTailCalls = false;
  if (InlinedFunctionInfo.ContainsCalls) {
    CallInst::TailCallKind CallSiteTailKind = CallInst::TCK_None;
    if (CallInst *CI = dyn_cast<CallInst>(TheCall))
      CallSiteTailKind = CI->getTailCallKind();

    for (Function::iterator BB = FirstNewBlock, E = Caller->end(); BB != E;
         ++BB) {
      for (Instruction &I : *BB) {
        CallInst *CI = dyn_cast<CallInst>(&I);
        if (!CI)
          continue;

        // We need to reduce the strength of any inlined tail calls.  For
        // musttail, we have to avoid introducing potential unbounded stack
        // growth.  For example, if functions 'f' and 'g' are mutually recursive
        // with musttail, we can inline 'g' into 'f' so long as we preserve
        // musttail on the cloned call to 'f'.  If either the inlined call site
        // or the cloned call site is *not* musttail, the program already has
        // one frame of stack growth, so it's safe to remove musttail.  Here is
        // a table of example transformations:
        //
        //    f -> musttail g -> musttail f  ==>  f -> musttail f
        //    f -> musttail g ->     tail f  ==>  f ->     tail f
        //    f ->          g -> musttail f  ==>  f ->          f
        //    f ->          g ->     tail f  ==>  f ->          f
        CallInst::TailCallKind ChildTCK = CI->getTailCallKind();
        ChildTCK = std::min(CallSiteTailKind, ChildTCK);
        CI->setTailCallKind(ChildTCK);
        InlinedMustTailCalls |= CI->isMustTailCall();

        // Calls inlined through a 'nounwind' call site should be marked
        // 'nounwind'.
        if (MarkNoUnwind)
          CI->setDoesNotThrow();
      }
    }
  }

  // Leave lifetime markers for the static alloca's, scoping them to the
  // function we just inlined.
  if (InsertLifetime && !IFI.StaticAllocas.empty()) {
    IRBuilder<> builder(&FirstNewBlock->front());
    for (unsigned ai = 0, ae = IFI.StaticAllocas.size(); ai != ae; ++ai) {
      AllocaInst *AI = IFI.StaticAllocas[ai];

      // If the alloca is already scoped to something smaller than the whole
      // function then there's no need to add redundant, less accurate markers.
      if (hasLifetimeMarkers(AI))
        continue;

      // Try to determine the size of the allocation.
      ConstantInt *AllocaSize = nullptr;
      if (ConstantInt *AIArraySize =
          dyn_cast<ConstantInt>(AI->getArraySize())) {
        auto &DL = Caller->getParent()->getDataLayout();
        Type *AllocaType = AI->getAllocatedType();
        uint64_t AllocaTypeSize = DL.getTypeAllocSize(AllocaType);
        uint64_t AllocaArraySize = AIArraySize->getLimitedValue();

        // Don't add markers for zero-sized allocas.
        if (AllocaArraySize == 0)
          continue;

        // Check that array size doesn't saturate uint64_t and doesn't
        // overflow when it's multiplied by type size.
        if (AllocaArraySize != ~0ULL &&
            UINT64_MAX / AllocaArraySize >= AllocaTypeSize) {
          AllocaSize = ConstantInt::get(Type::getInt64Ty(AI->getContext()),
                                        AllocaArraySize * AllocaTypeSize);
        }
      }

      builder.CreateLifetimeStart(AI, AllocaSize);
      for (ReturnInst *RI : Returns) {
        // Don't insert llvm.lifetime.end calls between a musttail call and a
        // return.  The return kills all local allocas.
        if (InlinedMustTailCalls &&
            RI->getParent()->getTerminatingMustTailCall())
          continue;
        IRBuilder<>(RI).CreateLifetimeEnd(AI, AllocaSize);
      }
    }
  }

  // If the inlined code contained dynamic alloca instructions, wrap the inlined
  // code with llvm.stacksave/llvm.stackrestore intrinsics.
  if (InlinedFunctionInfo.ContainsDynamicAllocas) {
    Module *M = Caller->getParent();
    // Get the two intrinsics we care about.
    Function *StackSave = Intrinsic::getDeclaration(M, Intrinsic::stacksave);
    Function *StackRestore=Intrinsic::getDeclaration(M,Intrinsic::stackrestore);

    // Insert the llvm.stacksave.
    CallInst *SavedPtr = IRBuilder<>(&*FirstNewBlock, FirstNewBlock->begin())
                             .CreateCall(StackSave, {}, "savedstack");

    // Insert a call to llvm.stackrestore before any return instructions in the
    // inlined function.
    for (ReturnInst *RI : Returns) {
      // Don't insert llvm.stackrestore calls between a musttail call and a
      // return.  The return will restore the stack pointer.
      if (InlinedMustTailCalls && RI->getParent()->getTerminatingMustTailCall())
        continue;
      IRBuilder<>(RI).CreateCall(StackRestore, SavedPtr);
    }
  }

  // If we are inlining for an invoke instruction, we must make sure to rewrite
  // any call instructions into invoke instructions.  This is sensitive to which
  // funclet pads were top-level in the inlinee, so must be done before
  // rewriting the "parent pad" links.
  if (auto *II = dyn_cast<InvokeInst>(TheCall)) {
    BasicBlock *UnwindDest = II->getUnwindDest();
    Instruction *FirstNonPHI = UnwindDest->getFirstNonPHI();
    if (isa<LandingPadInst>(FirstNonPHI)) {
      HandleInlinedLandingPad(II, &*FirstNewBlock, InlinedFunctionInfo);
    } else {
      HandleInlinedEHPad(II, &*FirstNewBlock, InlinedFunctionInfo);
    }
  }

  // Update the lexical scopes of the new funclets and callsites.
  // Anything that had 'none' as its parent is now nested inside the callsite's
  // EHPad.

  if (CallSiteEHPad) {
    for (Function::iterator BB = FirstNewBlock->getIterator(),
                            E = Caller->end();
         BB != E; ++BB) {
      // Add bundle operands to any top-level call sites.
      SmallVector<OperandBundleDef, 1> OpBundles;
      for (BasicBlock::iterator BBI = BB->begin(), E = BB->end(); BBI != E;) {
        Instruction *I = &*BBI++;
        CallSite CS(I);
        if (!CS)
          continue;

        // Skip call sites which are nounwind intrinsics.
        auto *CalledFn =
            dyn_cast<Function>(CS.getCalledValue()->stripPointerCasts());
        if (CalledFn && CalledFn->isIntrinsic() && CS.doesNotThrow())
          continue;

        // Skip call sites which already have a "funclet" bundle.
        if (CS.getOperandBundle(LLVMContext::OB_funclet))
          continue;

        CS.getOperandBundlesAsDefs(OpBundles);
        OpBundles.emplace_back("funclet", CallSiteEHPad);

        Instruction *NewInst;
        if (CS.isCall())
          NewInst = CallInst::Create(cast<CallInst>(I), OpBundles, I);
        else
          NewInst = InvokeInst::Create(cast<InvokeInst>(I), OpBundles, I);
        NewInst->setDebugLoc(I->getDebugLoc());
        NewInst->takeName(I);
        I->replaceAllUsesWith(NewInst);
        I->eraseFromParent();

        OpBundles.clear();
      }

      // It is problematic if the inlinee has a cleanupret which unwinds to
      // caller and we inline it into a call site which doesn't unwind but into
      // an EH pad that does.  Such an edge must be dynamically unreachable.
      // As such, we replace the cleanupret with unreachable.
      if (auto *CleanupRet = dyn_cast<CleanupReturnInst>(BB->getTerminator()))
        if (CleanupRet->unwindsToCaller() && EHPadForCallUnwindsLocally)
          changeToUnreachable(CleanupRet, /*UseLLVMTrap=*/false);

      Instruction *I = BB->getFirstNonPHI();
      if (!I->isEHPad())
        continue;

      if (auto *CatchSwitch = dyn_cast<CatchSwitchInst>(I)) {
        if (isa<ConstantTokenNone>(CatchSwitch->getParentPad()))
          CatchSwitch->setParentPad(CallSiteEHPad);
      } else {
        auto *FPI = cast<FuncletPadInst>(I);
        if (isa<ConstantTokenNone>(FPI->getParentPad()))
          FPI->setParentPad(CallSiteEHPad);
      }
    }
  }

  // Handle any inlined musttail call sites.  In order for a new call site to be
  // musttail, the source of the clone and the inlined call site must have been
  // musttail.  Therefore it's safe to return without merging control into the
  // phi below.
  if (InlinedMustTailCalls) {
    // Check if we need to bitcast the result of any musttail calls.
    Type *NewRetTy = Caller->getReturnType();
    bool NeedBitCast = !TheCall->use_empty() && TheCall->getType() != NewRetTy;

    // Handle the returns preceded by musttail calls separately.
    SmallVector<ReturnInst *, 8> NormalReturns;
    for (ReturnInst *RI : Returns) {
      CallInst *ReturnedMustTail =
          RI->getParent()->getTerminatingMustTailCall();
      if (!ReturnedMustTail) {
        NormalReturns.push_back(RI);
        continue;
      }
      if (!NeedBitCast)
        continue;

      // Delete the old return and any preceding bitcast.
      BasicBlock *CurBB = RI->getParent();
      auto *OldCast = dyn_cast_or_null<BitCastInst>(RI->getReturnValue());
      RI->eraseFromParent();
      if (OldCast)
        OldCast->eraseFromParent();

      // Insert a new bitcast and return with the right type.
      IRBuilder<> Builder(CurBB);
      Builder.CreateRet(Builder.CreateBitCast(ReturnedMustTail, NewRetTy));
    }

    // Leave behind the normal returns so we can merge control flow.
    std::swap(Returns, NormalReturns);
  }

  // If we cloned in _exactly one_ basic block, and if that block ends in a
  // return instruction, we splice the body of the inlined callee directly into
  // the calling basic block.
  if (Returns.size() == 1 && std::distance(FirstNewBlock, Caller->end()) == 1) {
    // Move all of the instructions right before the call.
    OrigBB->getInstList().splice(TheCall->getIterator(),
                                 FirstNewBlock->getInstList(),
                                 FirstNewBlock->begin(), FirstNewBlock->end());
    // Remove the cloned basic block.
    Caller->getBasicBlockList().pop_back();

    // If the call site was an invoke instruction, add a branch to the normal
    // destination.
    if (InvokeInst *II = dyn_cast<InvokeInst>(TheCall)) {
      BranchInst *NewBr = BranchInst::Create(II->getNormalDest(), TheCall);
      NewBr->setDebugLoc(Returns[0]->getDebugLoc());
    }

    // If the return instruction returned a value, replace uses of the call with
    // uses of the returned value.
    if (!TheCall->use_empty()) {
      ReturnInst *R = Returns[0];
      if (TheCall == R->getReturnValue())
        TheCall->replaceAllUsesWith(UndefValue::get(TheCall->getType()));
      else
        TheCall->replaceAllUsesWith(R->getReturnValue());
    }
    // Since we are now done with the Call/Invoke, we can delete it.
    TheCall->eraseFromParent();

    // Since we are now done with the return instruction, delete it also.
    Returns[0]->eraseFromParent();

    // We are now done with the inlining.
    return true;
  }

  // Otherwise, we have the normal case, of more than one block to inline or
  // multiple return sites.

  // We want to clone the entire callee function into the hole between the
  // "starter" and "ender" blocks.  How we accomplish this depends on whether
  // this is an invoke instruction or a call instruction.
  BasicBlock *AfterCallBB;
  BranchInst *CreatedBranchToNormalDest = nullptr;
  if (InvokeInst *II = dyn_cast<InvokeInst>(TheCall)) {

    // Add an unconditional branch to make this look like the CallInst case...
    CreatedBranchToNormalDest = BranchInst::Create(II->getNormalDest(), TheCall);

    // Split the basic block.  This guarantees that no PHI nodes will have to be
    // updated due to new incoming edges, and make the invoke case more
    // symmetric to the call case.
    AfterCallBB =
        OrigBB->splitBasicBlock(CreatedBranchToNormalDest->getIterator(),
                                CalledFunc->getName() + ".exit");

  } else {  // It's a call
    // If this is a call instruction, we need to split the basic block that
    // the call lives in.
    //
    AfterCallBB = OrigBB->splitBasicBlock(TheCall->getIterator(),
                                          CalledFunc->getName() + ".exit");
  }

  // Change the branch that used to go to AfterCallBB to branch to the first
  // basic block of the inlined function.
  //
  TerminatorInst *Br = OrigBB->getTerminator();
  assert(Br && Br->getOpcode() == Instruction::Br &&
         "splitBasicBlock broken!");
  Br->setOperand(0, &*FirstNewBlock);

  // Now that the function is correct, make it a little bit nicer.  In
  // particular, move the basic blocks inserted from the end of the function
  // into the space made by splitting the source basic block.
  Caller->getBasicBlockList().splice(AfterCallBB->getIterator(),
                                     Caller->getBasicBlockList(), FirstNewBlock,
                                     Caller->end());

  // Handle all of the return instructions that we just cloned in, and eliminate
  // any users of the original call/invoke instruction.
  Type *RTy = CalledFunc->getReturnType();

  PHINode *PHI = nullptr;
  if (Returns.size() > 1) {
    // The PHI node should go at the front of the new basic block to merge all
    // possible incoming values.
    if (!TheCall->use_empty()) {
      PHI = PHINode::Create(RTy, Returns.size(), TheCall->getName(),
                            &AfterCallBB->front());
      // Anything that used the result of the function call should now use the
      // PHI node as their operand.
      TheCall->replaceAllUsesWith(PHI);
    }

    // Loop over all of the return instructions adding entries to the PHI node
    // as appropriate.
    if (PHI) {
      for (unsigned i = 0, e = Returns.size(); i != e; ++i) {
        ReturnInst *RI = Returns[i];
        assert(RI->getReturnValue()->getType() == PHI->getType() &&
               "Ret value not consistent in function!");
        PHI->addIncoming(RI->getReturnValue(), RI->getParent());
      }
    }

    // Add a branch to the merge points and remove return instructions.
    DebugLoc Loc;
    for (unsigned i = 0, e = Returns.size(); i != e; ++i) {
      ReturnInst *RI = Returns[i];
      BranchInst* BI = BranchInst::Create(AfterCallBB, RI);
      Loc = RI->getDebugLoc();
      BI->setDebugLoc(Loc);
      RI->eraseFromParent();
    }
    // We need to set the debug location to *somewhere* inside the
    // inlined function. The line number may be nonsensical, but the
    // instruction will at least be associated with the right
    // function.
    if (CreatedBranchToNormalDest)
      CreatedBranchToNormalDest->setDebugLoc(Loc);
  } else if (!Returns.empty()) {
    // Otherwise, if there is exactly one return value, just replace anything
    // using the return value of the call with the computed value.
    if (!TheCall->use_empty()) {
      if (TheCall == Returns[0]->getReturnValue())
        TheCall->replaceAllUsesWith(UndefValue::get(TheCall->getType()));
      else
        TheCall->replaceAllUsesWith(Returns[0]->getReturnValue());
    }

    // Update PHI nodes that use the ReturnBB to use the AfterCallBB.
    BasicBlock *ReturnBB = Returns[0]->getParent();
    ReturnBB->replaceAllUsesWith(AfterCallBB);

    // Splice the code from the return block into the block that it will return
    // to, which contains the code that was after the call.
    AfterCallBB->getInstList().splice(AfterCallBB->begin(),
                                      ReturnBB->getInstList());

    if (CreatedBranchToNormalDest)
      CreatedBranchToNormalDest->setDebugLoc(Returns[0]->getDebugLoc());

    // Delete the return instruction now and empty ReturnBB now.
    Returns[0]->eraseFromParent();
    ReturnBB->eraseFromParent();
  } else if (!TheCall->use_empty()) {
    // No returns, but something is using the return value of the call.  Just
    // nuke the result.
    TheCall->replaceAllUsesWith(UndefValue::get(TheCall->getType()));
  }

  // Since we are now done with the Call/Invoke, we can delete it.
  TheCall->eraseFromParent();

  // If we inlined any musttail calls and the original return is now
  // unreachable, delete it.  It can only contain a bitcast and ret.
  if (InlinedMustTailCalls && pred_begin(AfterCallBB) == pred_end(AfterCallBB))
    AfterCallBB->eraseFromParent();

  // We should always be able to fold the entry block of the function into the
  // single predecessor of the block...
  assert(cast<BranchInst>(Br)->isUnconditional() && "splitBasicBlock broken!");
  BasicBlock *CalleeEntry = cast<BranchInst>(Br)->getSuccessor(0);

  // Splice the code entry block into calling block, right before the
  // unconditional branch.
  CalleeEntry->replaceAllUsesWith(OrigBB);  // Update PHI nodes
  OrigBB->getInstList().splice(Br->getIterator(), CalleeEntry->getInstList());

  // Remove the unconditional branch.
  OrigBB->getInstList().erase(Br);

  // Now we can remove the CalleeEntry block, which is now empty.
  Caller->getBasicBlockList().erase(CalleeEntry);

  // If we inserted a phi node, check to see if it has a single value (e.g. all
  // the entries are the same or undef).  If so, remove the PHI so it doesn't
  // block other optimizations.
  if (PHI) {
    auto &DL = Caller->getParent()->getDataLayout();
    if (Value *V = SimplifyInstruction(PHI, DL, nullptr, nullptr,
                                       &IFI.ACT->getAssumptionCache(*Caller))) {
      PHI->replaceAllUsesWith(V);
      PHI->eraseFromParent();
    }
  }

  return true;
}
