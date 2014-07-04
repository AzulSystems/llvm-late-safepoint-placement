This branch contains a proof of concept implementation of proposed support for precise relocating garbage collection in LLVM.  This code is not yet in a form suitable for submission to the upstream LLVM repository or other production use, but we welcome feedback on approach or implementation.

Over the next few weeks to months, we will be splitting out isolated pieces of these changes for upstream inclusion with LLVM, polishing the remaining implementation, and incorporating feedback on design/implementation.  Our long term goal is to merge nearly everything in this repository into the main LLVM development branch.  

If you're interested in experimenting with this code in your own compiler, please feel free.  Patches are welcome, but we will be somewhat selective about applying them due to merging costs.  

Overview:
- The IR level definitions for safepoints (statepoint, gc_relocate, and gc_result) can be found in:
    include/llvm/IR/Intrinsics.td
    include/llvm/IR/Statepoint.h
    lib/IR/Statepoint.cpp
    lib/IR/SafepointIRVerifier.cpp
- A pass to perform safepoint insertion in (nearly) arbitrary LLVM IR can be found in:
    lib/Transform/Scalar/SafepointPlacementPass.cpp
- The backend (x86 only) representation (STATEPOINT) can be found in: 
    include/llvm/Target/TargetOpcodes.h
    include/llvm/Target/Target.td
    lib/CodeGen/SelectionDAG/SelectionDAG/SelectionDAGBuilder.cpp
    ... various other files in Target/X86/..., search for TargetOpcodes::STATEPOINT
    lib/Target/X86/X86MCInstLower.cpp
- The interface for consumption by your runtime can be found here:
    lib/CodeGen/StackMaps.cpp (which generates a section in the in memory object file, the same approach as STACKMAP and PATCHPOINT)
    (this code supports MCJIT or static compilation, but not the older JIT framework)
- A collection of unit and integration tests can be found in:
    tests/GarbageCollection

The most recent merge into this public branch was performed using a baseline of July 1st, 2014.  

You can see the entirety of the changes by running the following command:
git diff pristine...master

Known Issues:
- Handling for safepoints at invokes (rather than calls) is incomplete.  
- The repository also includes other pieces of functionality - mostly because trying to separate them was in-feasible - which are not part of the garbage collection functionality.  In particular, the code around abstract VM states for deoptimization is fairly rough and is likely to be revised heavily.
- Only configure/make is supported, not CMake.  You must pass "--enable-shared" to configure.


Questions, suggestions, and other feedback can be sent to either Philip Reames (listmail @ full name.com ) or Sanjoy Das ( sanjoy @ azul systems com).
