
extern bool unknown_condition();

// Only used to force call safepoints at interesting points
extern void some_function();
extern void* my_alloc(unsigned);
extern void* my_alloc2(unsigned);
extern void* my_realloc(void*, unsigned);

/// This function simply exists to see what the resulting assembly looks like
/// when there are more live gc objects than registers (Note: X86_64 has lots of registers!)
void* lots_of_live_objs_call(void* a, void* b, void* c, void* d, void* e,
                             void* f, void* g, void* h, void* i, void* j,
                             void* k, void* l, void* m, void* n, void* o,
                             void* p, void* q, void* r, void* s, void* t) {
  some_function(); //force a safepoint
#define RET_IF_NULL(exp) if(exp) return exp
  RET_IF_NULL(a);
  RET_IF_NULL(b);
  RET_IF_NULL(c);
  RET_IF_NULL(d);
  RET_IF_NULL(e);
  RET_IF_NULL(f);
  RET_IF_NULL(g);
  RET_IF_NULL(h);
  RET_IF_NULL(j);
  RET_IF_NULL(k);
  RET_IF_NULL(l);
  RET_IF_NULL(m);
  RET_IF_NULL(n);
  RET_IF_NULL(o);
  RET_IF_NULL(p);
  RET_IF_NULL(q);
  RET_IF_NULL(r);
  RET_IF_NULL(s);
  RET_IF_NULL(t);
  return a;
#undef RET_IF_NULL
}
// Same as previous, but on a loop backegge
void* lots_of_live_objs_loop(void* a, void* b, void* c, void* d, void* e,
                             void* f, void* g, void* h, void* i, void* j,
                             void* k, void* l, void* m, void* n, void* o,
                             void* p, void* q, void* r, void* s, void* t) {
  while( unknown_condition() ) {}
#define RET_IF_NULL(exp) if(exp) return exp
  RET_IF_NULL(a);
  RET_IF_NULL(b);
  RET_IF_NULL(c);
  RET_IF_NULL(d);
  RET_IF_NULL(e);
  RET_IF_NULL(f);
  RET_IF_NULL(g);
  RET_IF_NULL(h);
  RET_IF_NULL(j);
  RET_IF_NULL(k);
  RET_IF_NULL(l);
  RET_IF_NULL(m);
  RET_IF_NULL(n);
  RET_IF_NULL(o);
  RET_IF_NULL(p);
  RET_IF_NULL(q);
  RET_IF_NULL(r);
  RET_IF_NULL(s);
  RET_IF_NULL(t);
  return a;
#undef RET_IF_NULL
}

extern void fake_use(...);

// This routine is trying to trick the compiler into rematerializing a use of
// unrelocated 'a' without relocating the 'a' or it's derived pointer.  This
// was maainly relevent when we had the dual use issue with gc.relocate using
// pointer arguments, but it's good to protect against going forward too.
void* cheap_recompute(char* a) {
  char* a1 = a+1;
  char* a2 = a+2;
  char* a3 = a+3;
  char* a4 = a+4;
  char* a5 = a+5;
  char* a6 = a+6;
  char* a7 = a+7;
  char* a8 = a+8;
  char* a9 = a+9;
  char* a10 = a+10;
  char* a11 = a+11;
  char* a12 = a+12;
  char* a13 = a+13;
  char* a14 = a+14;
  char* a15 = a+15;
  int* i = (int*)a;
  int* i1 = i-1;
  int* i2 = i-2;
  int* i3 = i-3;
  int* i4 = i-4;
  int* i5 = i-5;
  some_function();
  fake_use(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, i, i1, i2, i3, i4, i5);
  if( *a15 == '4') {
    return i3;
  }
  if( *a2 == '\0' ) {
    return a5;
  }
  return 0;
}
