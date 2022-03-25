/*
 * @Author: Radon
 * @Date: 2022-03-25 12:42:40
 * @LastEditors: Radon
 * @LastEditTime: 2022-03-25 15:42:11
 * @Description: Hi, say something
 */
#include <stdio.h>

void ptrAddOne(int *p) {
  (*p)++;
}

void addOne(int y) {
  y++;
}

void arrTest(int* arr) {
  arr[0]++;
}

int main(int argc, char **argv) {
  int x = 0, y = 0;
  scanf("%d", &x);
  int *p = &x;
  int arr[5] = {0};
  ptrAddOne(p);
  ptrAddOne(&x);
  arrTest(arr);
  addOne(y);
  return 0;
}