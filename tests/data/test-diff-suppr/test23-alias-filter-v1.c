/*
  Compile this file by doing:

  gcc -g -Wall -shared -Wl,--version-script=test23-alias-filter-version-script -o libtest23-alias-filter-v1.so test23-alias-filter-v1.c
 */

struct S
{
  char inserted_member;
  int m0;
};

typedef struct S S;

void
__private_function(S* s)
{
  if (s)
    ;
}

asm(".symver __private_function,__private_function@PRIVATE_1.0");

void
function1(S* s) __attribute__ ((alias("__private_function")));

asm(".symver function1,function1@VERSION_1.0");

void
function2(S* s) __attribute__ ((alias("__private_function")));

asm(".symver function2,function2@VERSION_1.0");
