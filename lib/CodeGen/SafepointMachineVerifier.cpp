// Run a sanity check on the Machine instructions to ensure that Safepoints -
// if they've been inserted - were inserted correctly.  In particular, look
// for use of non-relocated values after a safepoint.

#define DEBUG_TYPE "codegen-sp-verifier"

#include "llvm/CodeGen/Passes.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
using namespace llvm;

namespace {
class SafepointMachineVerifier : public MachineFunctionPass {
  bool runOnMachineFunction(MachineFunction &MF) override;

  const MachineRegisterInfo *MRI;
  const TargetInstrInfo *TII;

public:
  static char ID; // Pass identification, replacement for typeid
  SafepointMachineVerifier() : MachineFunctionPass(ID) {
    initializeSafepointMachineVerifierPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineDominatorTree>();
    AU.addPreserved<MachineDominatorTree>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

private:
  void addTransativeClosure(unsigned reg, std::set<unsigned> &invalid) {
    if (invalid.find(reg) != invalid.end()) {
      return;
    }
    invalid.insert(reg);

    for (const MachineOperand &Use : MRI->use_operands(reg)) {
      const MachineInstr *UseMI = Use.getParent();
      if (UseMI->isCopy()) {
        assert(UseMI->getOperand(0).isReg() && UseMI->getOperand(1).isReg() &&
               "copy must be between two registers");
        if (UseMI->getOperand(0).getReg() == reg) {
          addTransativeClosure(UseMI->getOperand(1).getReg(), invalid);
        } else {
          addTransativeClosure(UseMI->getOperand(0).getReg(), invalid);
        }
      }
    }
  }
};

struct bb_exit_state {
  std::set<unsigned> _invalid;
};
}
char SafepointMachineVerifier::ID = 0;
char &llvm::SafepointMachineVerifierID = SafepointMachineVerifier::ID;

INITIALIZE_PASS_BEGIN(SafepointMachineVerifier,
                      "verify-safepoint-machineinstrs", "", false, true)
INITIALIZE_PASS_END(SafepointMachineVerifier, "verify-safepoint-machineinstrs",
                    "", false, true)

FunctionPass *llvm::createSafepointMachineVerifierPass() {
  return new SafepointMachineVerifier();
}

bool SafepointMachineVerifier::runOnMachineFunction(MachineFunction &MF) {
  MRI = &MF.getRegInfo();
  TII = MF.getTarget().getInstrInfo();

  // There is no need to keep a frame index and vreg pair for each basic block
  // and we dont care about joint them.
  // Consider the following examples:
  // mov vreg1, stack0            mov vreg2, stack0
  // statepoint stack0            statepoint stack0
  //                \              /
  //               statepoint stack0
  // and
  // mov vreg1, stack0            mov vreg2, stack0
  // statepoint stack0            statepoint stack0
  // mov stack0, vreg3            mov stack0, vreg4
  //                \              /
  //               vreg5 = phi vreg3, vreg4
  //               mov vreg5, stack0
  //               statepoint stack0
  // Those should be identical machine instructions based on how smart llvm is
  // to fold the phi node.
  // Either case we dont need to care about joining the frame index and vreg
  // pairs from the incoming blocks,
  // and we will always record the correct invalid vregs. In the first example,
  // we could use either pair from
  // the left incoming block or right incoming block, it dosn't matter because
  // we will record both vreg1
  // and vreg2 in the invalid list. In the second example, the pair is updated
  // by vreg5 from both incoming
  // block, and will record vreg5 in the invalid list.
  std::map<int, unsigned> frameIndexVRegPair;
  std::map<MachineBasicBlock *, bb_exit_state> state;
  std::vector<MachineBasicBlock *> worklist;

  // We put basic blocks those contain spills (instead of statepoint) into the
  // worklist because we need to record
  // them first. As spills dominate statepoint, we should not miss any
  // statepoint. Statepoints those have no spills
  // must be reachable from a statepoint which has spills.
  for (MachineBasicBlock &MBB : MF) {
    for (const MachineInstr &MI : MBB) {
      if (MI.mayStore()) {
        worklist.push_back(&MBB);
        break;
      }
    }
  }

  // iterate until the invalid states stablize, checking on every iteration.
  while (!worklist.empty()) {
    MachineBasicBlock *currentBlock = worklist.back();
    worklist.pop_back();

    std::set<unsigned> validByDef;

    for (MachineBasicBlock::iterator itr = currentBlock->begin(),
                                     end = currentBlock->getFirstNonPHI();
         itr != end; itr++) {
      MachineInstr *inst = &*itr;

      assert(inst->isPHI() && "must be phi");
      for (unsigned i = 1; i < inst->getNumOperands(); i = i + 2) {
        if (inst->getOperand(i).isReg()) {
          unsigned reg = inst->getOperand(i).getReg();
          assert(i + 1 < inst->getNumOperands() &&
                 inst->getOperand(i + 1).isMBB() &&
                 "Could not find incoming basic block");
          MachineBasicBlock *incomingBB = inst->getOperand(i + 1).getMBB();
          // here we dont need to care about RelocationPHIEscapes like the IR
          // verifer
          // because those unused relocation phis should be cleaned up by the
          // optimizer
          assert(state[incomingBB]._invalid.find(reg) ==
                     state[incomingBB]._invalid.end() &&
                 "use of invalid unrelocated machine value after safepoint!");
        }
      }
      assert(inst->getOperand(0).isReg() && "phi shuold be assigned to a reg");
      validByDef.insert(inst->getOperand(0).getReg());
    }

    std::set<unsigned> invalid;
    // join all incoming invalid list from its predecessors
    for (MachineBasicBlock *Pred : currentBlock->predecessors()) {
      bb_exit_state exit = state[Pred];
      invalid.insert(exit._invalid.begin(), exit._invalid.end());
    }

    // if an invalid value reaches its def, it should be removed from the list
    for (unsigned val : validByDef) {
      std::set<unsigned>::iterator invalid_itr = invalid.find(val);
      if (invalid_itr != invalid.end()) {
        invalid.erase(invalid_itr);
      }
    }

    // now scan the rest of the machine instruction in the block
    for (MachineBasicBlock::iterator itr = currentBlock->getFirstNonPHI(),
                                     end = currentBlock->end();
         itr != end; itr++) {
      MachineInstr *inst = &*itr;

      if (inst->getNumOperands() > 0) {
        // first remove new def from the invalid list
        for (const MachineOperand &Operand : inst->operands()) {
          if (Operand.isReg() && Operand.isDef() &&
              invalid.find(Operand.getReg()) != invalid.end()) {
            invalid.erase(invalid.find(Operand.getReg()));
          }
        }

        // check whether any operand is invalid
        if (inst->getOpcode() != TargetOpcode::STATEPOINT) {
          for (unsigned i = 1; i < inst->getNumOperands(); i++) {
            assert(
                !inst->getOperand(i).isReg() || !inst->getOperand(i).isUse() ||
                invalid.find(inst->getOperand(i).getReg()) == invalid.end() &&
                    "use of invalid unrelocated value after safepoint!");
          }
        } else {
          // only check call argument for statepoint because vmstate is not
          // updated properly (a known bug)
          assert(inst->getOperand(0).isImm() &&
                 "number of call args must be immediate");
          int num_arg = inst->getOperand(0).getImm();
          for (int i = 2; i < 2 + num_arg; i++) {
            assert(
                !inst->getOperand(i).isReg() || !inst->getOperand(i).isUse() ||
                invalid.find(inst->getOperand(i).getReg()) == invalid.end() &&
                    "use of invalid unrelocated value after safepoint!");
          }
        }

        // track gc value from spills
        int FI;
        unsigned spilledReg = TII->isStoreToStackSlot(inst, FI);
        if (spilledReg != 0) {
          frameIndexVRegPair[FI] = spilledReg;
        }

        if (inst->getOpcode() == TargetOpcode::STATEPOINT) {
          // Statepoint operands:
          // <num call arguments>, <call target>, [call arguments],
          // <StackMaps::ConstantOp>, <flags>, [vm state and gc values]
          // therefore vm and gc values starts at 4 + num_arg
          assert(inst->getOperand(0).isImm() &&
                 "number of call args must be immediate");
          int num_arg = inst->getOperand(0).getImm();
          for (unsigned i = 4 + num_arg, e = inst->getNumOperands(); i != e;
               i++) {
            MachineOperand &MO = inst->getOperand(i);
            if (MO.isFI() && frameIndexVRegPair.find(MO.getIndex()) !=
                                 frameIndexVRegPair.end()) {
              // We cannot assert there is always a match, because we could
              // spill a 0 (null value) to the stack and that stack does not
              // have a match vreg
              addTransativeClosure(frameIndexVRegPair[MO.getIndex()], invalid);
            }
          }
        }
      }

      if (state[currentBlock]._invalid != invalid) {
        state[currentBlock]._invalid = invalid;
        for (MachineBasicBlock *Succ : currentBlock->successors()) {
          worklist.push_back(Succ);
        }
      }
    }
  }
  return false;
}
