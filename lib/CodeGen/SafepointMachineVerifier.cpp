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
#include "llvm/CodeGen/StackMaps.h"
#include "llvm/Support/CommandLine.h"
using namespace llvm;

/// This option is used for writting test cases.  Instead of
/// crashing the program when verification fails, report a
/// message to the console (for FileCheck usage) and continue
/// execution as if nothing happened.
static cl::opt<bool> PrintOnly(
    "safepoint-machineInstr-verifier-print-only",
    cl::init(false));

namespace {
  class SafepointMachineVerifier : public MachineFunctionPass {
    virtual bool runOnMachineFunction(MachineFunction &MF);

    const MachineRegisterInfo *MRI;
    const TargetInstrInfo *TII;

  public:
    static char ID; // Pass identification, replacement for typeid
    SafepointMachineVerifier() : MachineFunctionPass(ID) {
     initializeSafepointMachineVerifierPass(*PassRegistry::getPassRegistry());
    }

    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<MachineDominatorTree>();
      AU.addPreserved<MachineDominatorTree>();
      MachineFunctionPass::getAnalysisUsage(AU);
    }

  private:
    void addTransativeClosure(unsigned reg, std::set<unsigned>& invalid) {
      if (invalid.find(reg) != invalid.end()) {
        return;
      }

      // Physical registers are non-SSA form that the verifier can not handle.
      // We believe the implementation to be conservative (i.e. not to report
      // false positives) and the set of potential errors missed by this are
      // small due to the fact that values passed to physical registers are
      // unlikely passed to other vregs (i.e. values passed to calling
      // convention physical registers are only directly used by calls) and
      // values passed from physical registers are unlikely passed twice to
      // different vregs (i.e. rax pass the return value to only one vreg).
      if (TargetRegisterInfo::isPhysicalRegister(reg)) {
        return;
      }

      invalid.insert(reg);

      for (MachineRegisterInfo::use_iterator I = MRI->use_begin(reg),
             E = MRI->use_end(); I!=E; I++) {

        MachineOperand& Use = *I;
        MachineInstr *UseMI = Use.getParent();
        if (UseMI->isCopy()) {
          assert(UseMI->getOperand(0).isReg() && UseMI->getOperand(1).isReg() && "copy must be between two registers");
          if (UseMI->getOperand(0).getReg() == reg) {
            addTransativeClosure(UseMI->getOperand(1).getReg(), invalid);
          } else {
            addTransativeClosure(UseMI->getOperand(0).getReg(), invalid);
          }
        }
      }
    }

    unsigned getGCStartIndex(MachineInstr* MI) {
      StatepointOpers statepoint = StatepointOpers(MI);
      assert(MI->getOperand(statepoint.getStackNumIdx()).isImm()
          && MI->getOperand(statepoint.getLocalNumIdx()).isImm()
          && MI->getOperand(statepoint.getMonitorNumIdx()).isImm()
          && "deopt numbers must be immediate");
      unsigned num_stack =
          MI->getOperand(statepoint.getStackNumIdx()).getImm();
      unsigned num_local =
          MI->getOperand(statepoint.getLocalNumIdx()).getImm();
      unsigned num_monitor =
          MI->getOperand(statepoint.getMonitorNumIdx()).getImm();
      unsigned gc_start = statepoint.getMonitorNumIdx() + 1;

      // skip deopt values
      for (unsigned i = 0; i < num_stack + num_local + num_monitor; i++) {
        if (i < num_stack + num_local) {
          // first skip StackMaps::ConstantOp and deopt value type int
          // this does not apply to monitors
          gc_start += 2;
        }
        MachineOperand& MO = MI->getOperand(gc_start);

        // frame index value is lowered to 4 operands as below
        // MIB.addImm(StackMaps::IndirectMemRefOp);
        // MIB.addImm(MF.getFrameInfo()->getObjectSize(MO.getIndex()));
        // MIB.addFrameIndex(MO.getIndex());
        // MIB.addImm(0);
        // Refer to TargetLoweringBase.cpp for more details

        // This assert need to be changed if we change the fact that
        // all vmstate values are spilled
        assert(MO.isImm() && MO.getImm() == StackMaps::IndirectMemRefOp
            && "VMState values are all spilled and must be frame indexes");
        gc_start += 4;
      }

      return gc_start;
    }
  };

  struct bb_exit_state {
    std::set<unsigned> _invalid;
  };
}
char SafepointMachineVerifier::ID = 0;
char &llvm::SafepointMachineVerifierID = SafepointMachineVerifier::ID;

INITIALIZE_PASS_BEGIN(SafepointMachineVerifier, "verify-safepoint-machineinstrs",
                "", false, true)
INITIALIZE_PASS_END(SafepointMachineVerifier, "verify-safepoint-machineinstrs",
                "", false, true)

FunctionPass *llvm::createSafepointMachineVerifierPass() {
  return new SafepointMachineVerifier();
}

bool SafepointMachineVerifier::runOnMachineFunction(MachineFunction &MF) {
  MRI = &MF.getRegInfo();
  TII = MF.getTarget().getInstrInfo();

  // There is no need to keep a frame index and vreg pair for each basic block and we dont care about joint them.
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
  // Those should be identical machine instructions based on how smart llvm is to fold the phi node.
  // Either case we dont need to care about joining the frame index and vreg pairs from the incoming blocks,
  // and we will always record the correct invalid vregs. In the first example, we could use either pair from
  // the left incoming block or right incoming block, it dosn't matter because we will record both vreg1
  // and vreg2 in the invalid list. In the second example, the pair is updated by vreg5 from both incoming
  // block, and will record vreg5 in the invalid list.
  std::map<int, unsigned> frameIndexVRegPair;
  std::map<MachineBasicBlock*, bb_exit_state> state;
  std::vector<MachineBasicBlock*> worklist;

  // We put basic blocks those contain spills (instead of statepoint) into the worklist because we need to record
  // them first. As spills dominate statepoint, we should not miss any statepoint. Statepoints those have no spills
  // must be reachable from a statepoint which has spills.
  for (MachineFunction::iterator I = MF.begin(), E = MF.end();
      I != E; I++) {
    MachineBasicBlock* MBB = &*I;
    for (MachineBasicBlock::iterator MII = MBB->begin(),
             MIE = MBB->end(); MII != MIE; MII++) {
      MachineInstr* MI = &*MII;
      if (MI->mayStore()) {
        worklist.push_back(MBB);
        break;
      }
    }
  }

  // iterate until the invalid states stablize, checking on every iteration.
  while (!worklist.empty()) {
    MachineBasicBlock* currentBlock = worklist.back();
    worklist.pop_back();

    std::set<unsigned> validByDef;

    for (MachineBasicBlock::iterator itr = currentBlock->begin(), end = currentBlock->getFirstNonPHI();
        itr != end; itr++) {
      MachineInstr* inst = &*itr;

      assert(inst->isPHI() && "must be phi");
      for (unsigned i = 1; i < inst->getNumOperands(); i = i + 2) {
        if (inst->getOperand(i).isReg()) {
          unsigned reg = inst->getOperand(i).getReg();
          assert(i+1 < inst->getNumOperands() && inst->getOperand(i+1).isMBB() && "Could not find incoming basic block");
          MachineBasicBlock* incomingBB = inst->getOperand(i+1).getMBB();
          // here we dont need to care about RelocationPHIEscapes like the IR verifer
          // because those unused relocation phis should be cleaned up by the optimizer
          if (state[incomingBB]._invalid.find(reg) != state[incomingBB]._invalid.end()) {
            errs() << "Illegal use of unrelocated machine value after safepoint found!\n";
            errs() << "MachineInstr: ";
            inst->dump();
            if (!PrintOnly)
              assert(0 && "use of invalid unrelocated machine value after safepoint!");
          }
        }
      }
      assert(inst->getOperand(0).isReg() && inst->getOperand(0).isDef()
          && "phi shuold be assigned to a reg");
      validByDef.insert(inst->getOperand(0).getReg());
    }

    std::set<unsigned> invalid;
    // join all incoming invalid list from its predecessors
    for (MachineBasicBlock::pred_iterator PI = currentBlock->pred_begin(), E = currentBlock->pred_end(); PI != E; ++PI) {
      MachineBasicBlock* Pred = *PI;
      bb_exit_state exit = state[Pred];
      invalid.insert(exit._invalid.begin(), exit._invalid.end());
    }

    // if an invalid value reaches its def, it should be removed from the list
    for(std::set<unsigned>::iterator itr = validByDef.begin(), end = validByDef.end();
        itr != end; itr++) {
      std::set<unsigned>::iterator invalid_itr = invalid.find(*itr);
      if( invalid_itr != invalid.end() ) {
        invalid.erase(invalid_itr);
      }
    }

    // now scan the rest of the machine instruction in the block
    for (MachineBasicBlock::iterator itr = currentBlock->getFirstNonPHI(), end = currentBlock->end(); itr != end; itr++) {
      MachineInstr* inst = &*itr;

      if (inst->getNumOperands() > 0) {
        // first remove new def from the invalid list
        for (unsigned i = 0; i < inst->getNumOperands(); i++) {
          if (inst->getOperand(i).isReg() && inst->getOperand(i).isDef() && invalid.find(inst->getOperand(i).getReg()) != invalid.end()) {
            invalid.erase(invalid.find(inst->getOperand(i).getReg()));
          }
        }

        // check whether any operand is invalid
        if (inst->getOpcode() != TargetOpcode::STATEPOINT) {
          for (unsigned i = 1; i < inst->getNumOperands(); i++) {
            if (!(!inst->getOperand(i).isReg() || !inst->getOperand(i).isUse()
                || invalid.find(inst->getOperand(i).getReg()) == invalid.end())) {
              errs() << "Illegal use of unrelocated machine value after safepoint found!\n";
              errs() << "MachineInstr: ";
              inst->dump();
              if (!PrintOnly)
                assert(0 && "use of invalid unrelocated value after safepoint!");
            }
          }
        } else {
          // only check call argument for statepoint because vmstate is not updated properly (a known bug)
          assert(inst->getOperand(0).isImm() && "number of call args must be immediate");
          int num_arg = inst->getOperand(0).getImm();
          for (int i = 2; i < 2 + num_arg; i++) {
            if (!(!inst->getOperand(i).isReg() || !inst->getOperand(i).isUse()
                || invalid.find(inst->getOperand(i).getReg()) == invalid.end())) {
              errs() << "Illegal use of unrelocated machine value after safepoint found!\n";
              errs() << "MachineInstr: ";
              inst->dump();
              if (!PrintOnly)
                assert(0 && "use of invalid unrelocated value after safepoint!");
            }
          }
        }

        // track gc value from spills
        int FI;
        unsigned spilledReg = TII->isStoreToStackSlot(inst, FI);
        if (spilledReg != 0) {
          frameIndexVRegPair[FI] = spilledReg;
        }

        if (inst->getOpcode() == TargetOpcode::STATEPOINT) {
          unsigned gc_start = getGCStartIndex(inst);

          for (unsigned i = gc_start, e = inst->getNumOperands(); i != e; i++) {
            MachineOperand& MO = inst->getOperand(i);
            if (MO.isFI() && frameIndexVRegPair.find(MO.getIndex()) != frameIndexVRegPair.end()) {
              // We cannot assert there is always a match, because we could
              // spill a 0 (null value) to the stack and that stack does not
              // have a match vreg
              addTransativeClosure(frameIndexVRegPair[MO.getIndex()], invalid);
            }
          }

          // after each statepoint, the frameIndex2vreg pair need to be cleared
          // otherwise the previous pair relation might give us wrong
          // information. eg. at statepoint1, we spill a vmstate value vreg1 to
          // frame1 and record it into frameIndexVRegPair, and then at
          // statepoint2, we spill a null (constant 0) gc value to frame1.
          // Since we only record spill of registers, frame1 is not updated by
          // the null spill and still paired with vreg1. We would end up
          // recording vreg1 as a relocated gc value which is wrong.
          frameIndexVRegPair.clear();

        }
      }
    }

    if (state[currentBlock]._invalid != invalid) {
      state[currentBlock]._invalid = invalid;
      for (MachineBasicBlock::succ_iterator PI = currentBlock->succ_begin(),
          E = currentBlock->succ_end(); PI != E; ++PI) {
        MachineBasicBlock *Succ = *PI;
        worklist.push_back(Succ);
      }
    }
  }
  return false;
}
