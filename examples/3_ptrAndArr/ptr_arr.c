#include <stdio.h>

void ptrFunc() {
  int a = 10, b = 5;
  int *p = &a;
  *p += b;
  *p = 20;
}

void arrFunc() {
  int arr1[10] = {0}, arr2[7] = {0};
  arr1[5] = 1;
  arr2[5] = arr1[5];
}

int main(int argc, char **argv) {
  return 0;
}