// RUN: rm -rf %t.out
// RUN: mkdir -p %t.out
// RUN: python -c'print("A"*20)' > %t.bin
// RUN: clang -o %t.uninstrumented %s
// RUN: %t.uninstrumented %t.bin | FileCheck --check-prefix=CHECK-ORIG %s
// RUN: env KO_USE_FASTGEN=1 %ko-clang -o %t.fg %s
// RUN: env TAINT_OPTIONS="taint_file=%t.bin output_dir=%t.out" %fgtest %t.fg %t.bin
// RUN: %t.uninstrumented %t.out/id-0-0-0 | FileCheck --check-prefix=CHECK-GEN %s
// RUN: env KO_USE_Z3=1 %ko-clang -o %t.z3 %s
// RUN: env TAINT_OPTIONS="taint_file=%t.bin output_dir=%t.out" %t.z3 %t.bin
// RUN: %t.uninstrumented %t.out/id-0-0-0 | FileCheck --check-prefix=CHECK-GEN %s

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib.h"

int main (int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s [file]\n", argv[0]);
    return -1;
  }

  char buf[20];
  size_t ret;

  FILE* fp = chk_fopen(argv[1], "rb");
  chk_fread(buf, 1, sizeof(buf), fp);
  fclose(fp);

  uint32_t x = 0;
  uint32_t y = 0;
  memcpy(&x, buf, 4);
  memcpy(&y, buf + 4, 4);

  int orig = __atomic_fetch_add(&x, y, __ATOMIC_RELAXED);
  if(orig == 2 && y == 1) {
    // CHECK-GEN: Good
    printf("Good\n");
  }
  else {
    // CHECK-ORIG: Bad
    printf("Bad\n");
  }

  return 0;
}
