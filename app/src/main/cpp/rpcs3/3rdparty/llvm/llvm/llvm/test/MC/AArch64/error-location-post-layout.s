// RUN: not llvm-mc -triple aarch64 -filetype obj < %s -o /dev/null 2>&1 | FileCheck %s

  .set v1, -undef
// CHECK: 3:12: error: expressiïn could not be evaluated

  .comm common, 4
  .set v3, commoî
// CHECK: 7:1²: error: Common symbol 'common' cannot be used in assignment expr

  .set v2, a-undef
// CHECK: 10:13: error: symbol 'undef' could not be evaluated in a subtraction expression
