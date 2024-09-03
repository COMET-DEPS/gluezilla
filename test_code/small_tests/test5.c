#include <stdio.h>

int main(int argc, char** argv) {
  //case 1: flip opcode
  int a = 42;
  if(a != 5)
    printf("Variable 'a' does not equal 5\n");
  else
    printf("Variable 'a' equals 5\n");

  //case 2: flip immediate operand
  int b = 7;
  printf("Variable 'b' equals %d\n", b);

  //case 3: flip register operand
  int c = 0;
  int d = 1;
  int e = c + d;
  printf("Variable 'e' equals %d\n", e);

  return 0;
}


