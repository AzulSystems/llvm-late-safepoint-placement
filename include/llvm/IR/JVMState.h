//===-- JVMState.h - Helpers for Abstract Virtual Machine States-*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_JVM_STATE_H
#define LLVM_IR_JVM_STATE_H

#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"

namespace llvm {

bool isJVMState(const Value *);
bool isJVMStateAnchorInstruction(const Value *);

#ifndef NDEBUG
void assertJVMStateSanity(const CallInst *V);
#endif

// This representes an *opaque* identifier for a JVM type.  LLVM must
// not rely on being able to interpret what the return value of
// coerceToInt() means.
class OpaqueJVMTypeID {
 public:
  OpaqueJVMTypeID(const OpaqueJVMTypeID& other)
      : integerValue(other.integerValue) { }

  int coerceToInt() const { return integerValue; }

 private:
  int integerValue;
  explicit OpaqueJVMTypeID(int value) : integerValue(value) { }

  template<typename ValueTy, typename CallInstTy> friend class JVMStateBase;
  template<typename InstructionTy, typename ValueTy, typename CallSiteTy>
  friend class StatepointBase;
};

template<typename ValueTy, typename CallInstTy>
class JVMStateBase {
 public:
  explicit JVMStateBase(ValueTy *V) {
    assert(isJVMState(V) && "contract!");
    jvmState = cast<CallInst>(V);
#ifndef NDEBUG
    assertJVMStateSanity(jvmState);
#endif
    jvmsCallerVMState = jvmState->getArgOperand(callerVMStateIndex);
    jvmsBCI = static_cast<int>(
        cast<ConstantInt>(jvmState->getArgOperand(bciIndex))->getSExtValue());
    jvmsNumStackElements = static_cast<int>(
        cast<ConstantInt>(jvmState->getArgOperand(numStackElementsIndex))->getSExtValue());
    jvmsNumLocals = static_cast<int>(
        cast<ConstantInt>(jvmState->getArgOperand(numLocalsIndex))->getSExtValue());
    jvmsNumMonitors = static_cast<int>(
        cast<ConstantInt>(jvmState->getArgOperand(numMonitorIndex))->getSExtValue());
  }

  const static int callerVMStateIndex = 0;
  const static int bciIndex = 1;
  const static int numStackElementsIndex = 2;
  const static int numLocalsIndex = 3;
  const static int numMonitorIndex = 4;

  static int headerEndOffset() { return 5; }

  Value* callerVMState() const { return jvmsCallerVMState; }
  int bci() const { return jvmsBCI; }
  int numStackElements() const { return jvmsNumStackElements; }
  int numLocals() const { return jvmsNumLocals; }
  int numMonitors() const { return jvmsNumMonitors; }

  ValueTy *stackElementAt(int i) {
    assert(i >= 0 && i < numStackElements() &&
           "index out of bounds in stackElementAt");
    return jvmState->getArgOperand(headerEndOffset() + 2 * i + 1);
  }

  OpaqueJVMTypeID stackElementTypeAt(int i) {
    assert(i >= 0 && i < numStackElements() &&
           "index out of bounds in stackElementAt");
    int tyInt = cast<ConstantInt>(
        jvmState->getArgOperand(headerEndOffset() + 2 * i))->getSExtValue();
    return OpaqueJVMTypeID(tyInt);
  }

  ValueTy *localAt(int i) {
    assert(i >= 0 && i < numLocals() && "index out of bounds in localAt");
    return jvmState->getArgOperand(stackEndOffset() + 2 * i + 1);
  }

  OpaqueJVMTypeID localTypeAt(int i) {
    assert(i >= 0 && i < numLocals() && "index out of bounds in localAt");
    int tyInt = cast<ConstantInt>(
        jvmState->getArgOperand(stackEndOffset() + 2 * i))->getSExtValue();
    return OpaqueJVMTypeID(tyInt);
  }

  ValueTy *monitorAt(int i) {
    assert(i >= 0 && i < numMonitors() && "index out of bounds in monitorAt");
    return jvmState->getArgOperand(localsEndOffset() + i);
  }

 private:
  CallInstTy *jvmState;
  Value* jvmsCallerVMState;
  int jvmsBCI;
  int jvmsNumStackElements;
  int jvmsNumLocals;
  int jvmsNumMonitors;

  int stackEndOffset() const {
    return headerEndOffset() + 2 * numStackElements();
  }

  int localsEndOffset() const {
    return headerEndOffset() + 2 * numStackElements() + 2 * numLocals();
  }
};

struct JVMState : public JVMStateBase<Value, CallInst> {
  explicit JVMState(Value *V) : JVMStateBase<Value, CallInst>(V) { }
};

struct ImmutableJVMState : public JVMStateBase<const Value, const CallInst> {
  explicit ImmutableJVMState(const Value *V)
      : JVMStateBase<const Value, const CallInst>(V) { }
};

}

#endif
