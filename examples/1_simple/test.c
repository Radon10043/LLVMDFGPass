#include <stdio.h>

void func();
void func1(int p);
void func2(int p1, int p2);

void func() {
  printf("This is a func.\n");
}

void func1(int p) {
  int v1 = p;
  int v2 = 3;
  v1 = v1 + v2;
}

void func2(int p1, int p2) {
  int v1 = p1 + 1;
  int v2 = p2 + 2;
}

int main(int argc, char **argv) {
  int a = 1, b = 2, c = 3;
  int d = 4, e = 5, f = 6;
  a = b + c;
  d = e + f;

  if (a) {
    b = 20;
  } else {
    b = -20;
  }

  if (d) {
    f = 10;
  } else {
    f = -10;
  }

  func();
  func1(a);
  func2(b, f);
  return 0;
}