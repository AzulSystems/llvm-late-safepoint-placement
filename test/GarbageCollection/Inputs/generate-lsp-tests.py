# This is a helper script to generate the various individual tests for Late
# Safepoint Placement.  This script should be run any time you add a new 
# lsp-*.cpp file or remove an existing one and before invoking llvm-lit.
# Run it as so: python ./test/GarbageCollection/Inputs/generate-lsp-test.py 

# For each input cpp file, we generate the following variants
# (clang -O0) vs (clang -O3)
# (-place-call-safepoints vs -place-backedge-safepoints vs both)
# (opt -O3 vs opt -O0)
# llc (asm file) vs llc (obj file)

import glob

def write_test(name, clangO, lsp, optO, llcmode):
    assert clangO in range(0,4)
    assert lsp in ["call", "loop", "both"]
    assert optO in range(0,4)
    assert llcmode in ["asm", "obj"]

    lspmap = {"call" : "-place-call-safepoints", 
              "loop" : "-place-backedge-safepoints", 
              "both" : "-place-call-safepoints -place-backedge-safepoints"}

    optOs = "-O%d" % optO
    if optO == 0:
        optOs = ""
    lines = ["; RUN: %%p/../Inputs/clang-proxy.sh %%p/../Inputs/%s.cpp -S -emit-llvm -o %%t -O%d" % (name,clangO),
             "; RUN: llvm-link %t %p/../Inputs/lsp-library.ll -S -o %t",
             "; RUN: opt %%t -S -o %%t %s %s" % (lspmap[lsp] + " -spp-all-functions", optOs),
             "; RUN: llc -filetype=%s < %%t" % llcmode,
             "; RUN: llc < %t | FileCheck %s",
             "",
             "; CHECK: StackMaps"]
    fullname = "./test/GarbageCollection/lsp-integ/%s-O%d-%s-O%d-%s.ll" % (name, clangO, lsp, optO, llcmode)
    with open(fullname, 'w') as testfile:
        testfile.write("\n".join(lines))


# TODO: actually use the glob and split out the test files
for f in glob.glob("./test/GarbageCollection/Inputs/*.cpp"):
    # remove prefix and extension
    name = f[len("./test/GarbageCollection/Inputs/"):]
    name = name[:len(name)-4]
    print "Writing out test variants for %s from %s" % (name, f)
    for clangO3 in [0, 3]:
        for optlsp in ["call", "loop", "both"]:
            for optO3 in [0, 3]:
                for llcmode in ["asm", "obj"]:
                    write_test(name, clangO3, optlsp, optO3, llcmode);
                    pass
