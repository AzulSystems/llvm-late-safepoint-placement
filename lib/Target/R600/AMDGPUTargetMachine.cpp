//===-- AMDGPUTargetMachine.cpp - TargetMachine for hw codegen targets-----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// \brief The AMDGPU target machine contains all of the hardware specific
/// information  needed to emit code for R600 and SI GPUs.
//
//===----------------------------------------------------------------------===//

#include "AMDGPUTargetMachine.h"
#include "AMDGPU.h"
#include "R600ISelLowering.h"
#include "R600InstrInfo.h"
#include "R600MachineScheduler.h"
#include "SIISelLowering.h"
#include "SIInstrInfo.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/CodeGen/MachineFunctionAnalysis.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/PassManager.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include <llvm/CodeGen/Passes.h>

using namespace llvm;

extern "C" void LLVMInitializeR600Target() {
  // Register the target
  RegisterTargetMachine<AMDGPUTargetMachine> X(TheAMDGPUTarget);
}

static ScheduleDAGInstrs *createR600MachineScheduler(MachineSchedContext *C) {
  return new ScheduleDAGMILive(C, make_unique<R600SchedStrategy>());
}

static MachineSchedRegistry
SchedCustomRegistry("r600", "Run R600's custom scheduler",
                    createR600MachineScheduler);

static std::string computeDataLayout(const AMDGPUSubtarget &ST) {
  std::string Ret = "e-p:32:32";

  if (ST.is64bit()) {
    // 32-bit private, local, and region pointers. 64-bit global and constant.
    Ret += "-p1:64:64-p2:64:64-p3:32:32-p4:64:64-p5:32:32-p24:64:64";
  }

  Ret += "-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256"
         "-v512:512-v1024:1024-v2048:2048-n32:64";

  return Ret;
}

AMDGPUTargetMachine::AMDGPUTargetMachine(const Target &T, StringRef TT,
    StringRef CPU, StringRef FS,
  TargetOptions Options,
  Reloc::Model RM, CodeModel::Model CM,
  CodeGenOpt::Level OptLevel
)
:
  LLVMTargetMachine(T, TT, CPU, FS, Options, RM, CM, OptLevel),
  Subtarget(TT, CPU, FS),
  Layout(computeDataLayout(Subtarget)),
  FrameLowering(TargetFrameLowering::StackGrowsUp,
                64 * 16 // Maximum stack alignment (long16)
               , 0),
  IntrinsicInfo(this),
  InstrItins(&Subtarget.getInstrItineraryData()) {
  // TLInfo uses InstrInfo so it must be initialized after.
  if (Subtarget.getGeneration() <= AMDGPUSubtarget::NORTHERN_ISLANDS) {
    TLInfo.reset(new R600TargetLowering(*this));
  } else {
    TLInfo.reset(new SITargetLowering(*this));
  }
  setRequiresStructuredCFG(true);
  initAsmInfo();
}

AMDGPUTargetMachine::~AMDGPUTargetMachine() {
}

namespace {
class AMDGPUPassConfig : public TargetPassConfig {
public:
  AMDGPUPassConfig(AMDGPUTargetMachine *TM, PassManagerBase &PM)
    : TargetPassConfig(TM, PM) {}

  AMDGPUTargetMachine &getAMDGPUTargetMachine() const {
    return getTM<AMDGPUTargetMachine>();
  }

  ScheduleDAGInstrs *
  createMachineScheduler(MachineSchedContext *C) const override {
    const AMDGPUSubtarget &ST = TM->getSubtarget<AMDGPUSubtarget>();
    if (ST.getGeneration() <= AMDGPUSubtarget::NORTHERN_ISLANDS)
      return createR600MachineScheduler(C);
    return nullptr;
  }

  virtual void addCodeGenPrepare();
  bool addPreISel() override;
  bool addInstSelector() override;
  bool addPreRegAlloc() override;
  bool addPostRegAlloc() override;
  bool addPreSched2() override;
  bool addPreEmitPass() override;
};
} // End of anonymous namespace

TargetPassConfig *AMDGPUTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new AMDGPUPassConfig(this, PM);
}

//===----------------------------------------------------------------------===//
// AMDGPU Analysis Pass Setup
//===----------------------------------------------------------------------===//

void AMDGPUTargetMachine::addAnalysisPasses(PassManagerBase &PM) {
  // Add first the target-independent BasicTTI pass, then our AMDGPU pass. This
  // allows the AMDGPU pass to delegate to the target independent layer when
  // appropriate.
  PM.add(createBasicTargetTransformInfoPass(this));
  PM.add(createAMDGPUTargetTransformInfoPass(this));
}

void AMDGPUPassConfig::addCodeGenPrepare() {
  const AMDGPUSubtarget &ST = TM->getSubtarget<AMDGPUSubtarget>();
  if (ST.isPromoteAllocaEnabled()) {
    addPass(createAMDGPUPromoteAlloca(ST));
    addPass(createSROAPass());
  }

  TargetPassConfig::addCodeGenPrepare();
}

bool
AMDGPUPassConfig::addPreISel() {
  const AMDGPUSubtarget &ST = TM->getSubtarget<AMDGPUSubtarget>();
  addPass(createFlattenCFGPass());
  if (ST.IsIRStructurizerEnabled())
    addPass(createStructurizeCFGPass());
  if (ST.getGeneration() >= AMDGPUSubtarget::SOUTHERN_ISLANDS) {
    addPass(createSinkingPass());
    addPass(createSITypeRewriter());
    addPass(createSIAnnotateControlFlowPass());
  } else {
    addPass(createR600TextureIntrinsicsReplacer());
  }
  return false;
}

bool AMDGPUPassConfig::addInstSelector() {
  addPass(createAMDGPUISelDag(getAMDGPUTargetMachine()));
  addPass(createSILowerI1CopiesPass());
  return false;
}

bool AMDGPUPassConfig::addPreRegAlloc() {
  const AMDGPUSubtarget &ST = TM->getSubtarget<AMDGPUSubtarget>();

  if (ST.getGeneration() <= AMDGPUSubtarget::NORTHERN_ISLANDS) {
    addPass(createR600VectorRegMerger(*TM));
  } else {
    addPass(createSIFixSGPRCopiesPass(*TM));
    // SIFixSGPRCopies can generate a lot of duplicate instructions,
    // so we need to run MachineCSE afterwards.
    addPass(&MachineCSEID);
    initializeSIFixSGPRLiveRangesPass(*PassRegistry::getPassRegistry());
    insertPass(&RegisterCoalescerID, &SIFixSGPRLiveRangesID);
  }
  return false;
}

bool AMDGPUPassConfig::addPostRegAlloc() {
  const AMDGPUSubtarget &ST = TM->getSubtarget<AMDGPUSubtarget>();

  if (ST.getGeneration() > AMDGPUSubtarget::NORTHERN_ISLANDS) {
    addPass(createSIInsertWaits(*TM));
  }
  return false;
}

bool AMDGPUPassConfig::addPreSched2() {
  const AMDGPUSubtarget &ST = TM->getSubtarget<AMDGPUSubtarget>();

  if (ST.getGeneration() <= AMDGPUSubtarget::NORTHERN_ISLANDS)
    addPass(createR600EmitClauseMarkers());
  if (ST.isIfCvtEnabled())
    addPass(&IfConverterID);
  if (ST.getGeneration() <= AMDGPUSubtarget::NORTHERN_ISLANDS)
    addPass(createR600ClauseMergePass(*TM));
  return false;
}

bool AMDGPUPassConfig::addPreEmitPass() {
  const AMDGPUSubtarget &ST = TM->getSubtarget<AMDGPUSubtarget>();
  if (ST.getGeneration() <= AMDGPUSubtarget::NORTHERN_ISLANDS) {
    addPass(createAMDGPUCFGStructurizerPass());
    addPass(createR600ExpandSpecialInstrsPass(*TM));
    addPass(&FinalizeMachineBundlesID);
    addPass(createR600Packetizer(*TM));
    addPass(createR600ControlFlowFinalizer(*TM));
  } else {
    addPass(createSILowerControlFlowPass(*TM));
  }

  return false;
}
