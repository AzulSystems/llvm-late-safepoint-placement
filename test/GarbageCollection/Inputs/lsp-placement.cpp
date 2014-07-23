/* This file contains a bunch of test which were used in the early development
   of LSP.  Given this is standard C code, you'll need to process the resulting
   IR with -spp-all-functions
 */


extern bool unknown_condition();

int counted_loop_purevec(int a[]) {
  int sum = 0;
  for(int i = 0; i < 4*12; i++) {
    sum += a[i];
  }
  return sum;
}
int counted_loop_impurevec(int a[]) {
  int sum = 0;
  for(int i = 0; i < 4*12+2; i++) {
    sum += a[i];
  }
  return sum;
}

int finite_loop(int a[], int len) {
  int sum = 0;
  for(int i = 0; i < len; i++) {
    sum += a[i];
  }
  return sum;
}
int blocked_finite_loop(int a[], int len) {
  int sum = 0; 
  for(int i = 0; i < len; i++) {
    int end = (i+28) > len ? len : (i+28);
    // There may be a off by one here, don't really care
    for( ; i < end; i++) {
      sum += a[i];
    }
  }
  return sum;
}

int uncounted_loop() {
  int cnt = 0;
  while( unknown_condition() ) {
    cnt++;
  }
  return cnt;
}

int uncounted_loop_with_obj(int a[]) {
  int sum = 0;
  int i = 0;
  while( unknown_condition() ) {
    sum += a[i++];
  }
  return sum;
}

int uncounted_loop_live_after(int a[]) {
  int cnt = 0;
  while( unknown_condition() ) {
    cnt++;
  }
  a[0] = 0;
  return cnt;
}

// Only used to force call safepoints at interesting points
extern void some_function();
extern void* my_alloc(unsigned);
extern void* my_alloc2(unsigned);
extern void* my_realloc(void*, unsigned);

void* conflict_base_phi(bool which) {
  void* a;
  if(which) {
    a = my_alloc(4);
  } else {
    a = my_alloc2(4);
  }
  //need to use two different functions to force a phi node here
  some_function();
  return a;
}

void* conflict_base_phi_loop(int cnt) {
  void* a = 0;
  while(cnt) {
    // force a phi cycle in the loop
    a = my_realloc(a, 4);
    cnt--;
  }
  some_function();
  return a;
}

/// Uses arguments only (not instructions)
int* conflict_base_args(int a[], int b[], bool which) {
  some_function();
  return which ? a : b; //gives a select
}
void* conflict_base(bool which) {
  void* a = my_alloc(4);
  void* b = my_alloc(4);
  some_function();
  return which ? a : b; //gives a select
}

void* conflict_base_select_loop(int cnt, bool which) {
  void* a; //undef
  while(cnt) {
    // force a phi cycle in the loop
    // with a select contained
    void* b = my_realloc(a, 4);
    a = unknown_condition() ? b : 0;
    cnt--;
  }
  some_function();
  // force a select
  return which ? a : 0;
}

// avoid varags here due to parsing error in LL files?  WTF
void fake_use(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h);


// Has multiple uses of a after a safepoint.  This triggers a problematic case
// in the folding code where it only folds a stack ref into a use IFF there's a
// single use.
void* multiple_use(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
  some_function();
  // Note: There need to be enough objs here to saturate the callee saved
  // registers and force spilling, but no so many to run into the VR > regs issue.
  fake_use(a,b,c,d,e,f,g,h);
  fake_use(a,b,c,d,e,f,g,h);
  return a;
}



void* conditional_safepoint(void* a, bool b) {
  if( b ) {
    some_function();
  }
  return a;
}


// If the type of the call comes from a bitcast, we need to preserve that type
// when inserting safepoints.
typedef void (*VoidVoidFnTy)(void);
void function_pointer_test(void* fp) {
  ((VoidVoidFnTy)fp)();
}

// These two aren't actually fully implemented yet.  All the changes through
// opt have been made, but there's a bit more work required in
// SelectionDAGBuilder once Igor's cleanup changes go in.  If you enable these,
// you should trip over related asserts in SelectionDAGBuilder::visitInvoke
#if 0

// Since the result can differ based on whether an exception is thrown, Clang
// will emit an invoke rather than a call.  This is the simplest case with
// neither gc_relocates or gc_results following in the normal dest block
bool invoke_test() {
  try {
    some_function();
    return true;
  } catch( int& i ) {
    return false;
  }
}


// These test phi insertion when the safepoint statles two basic blocks

// There will be gc_relocate in the normal block, but NOT a gc_result
bool invoke_test_relocate(void* a) {
  try {
    some_function();
    return a ? true : false;
  } catch( int& i ) {
    return false;
  }
}

// There will be an gc_result in the normal block, but NOT a gc_relocate
bool invoke_test_result() {
  try {
    bool b = unknown_condition();
    return b;
  } catch( int& i ) {
    return false;
  }
}

// This is a contrived case to show that the normal basic block can be
// reachable along a second path and thus contain phis.  As a result, we
// may need to insert another basic block when doing invoke insertion.
bool invoke_test_merged_normal(bool unrelated) {
  bool rval = true;
  try {
    bool b = unknown_condition();
    rval = b;
  } catch( int& i ) {
    rval = false;
  }
  // We should get a shared return block with a phi on the return value here.
  // The 'b' value above will be one of the phi inputs, the other will be
  // 'false' from the catch block.  
  return rval;
}

#endif


