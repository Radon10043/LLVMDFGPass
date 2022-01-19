#include <stdio.h>
void func2() {
  printf("hello world");
}

void func() {
  func2();
}

int rnAdd(int a, int b) {
  return a + b;
}

int main(int argc, char **argv) {
  func();
  int a = 1, b = 2, c = 3;
  int d = 4, e = 5, f = 6;
  a = b + c;
  d = e + f;
  return 0;
}

int func3() {
  return 3;
}