#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  char ch0 = 'g', ch1 = 'o', ch2 = 'o', ch3 = 'd';
  int ofst0 = 0, ofst1 = 0, ofst2 = 0, ofst3;

  scanf("%d,%d,%d,%d", &ofst0, &ofst1, &ofst2, &ofst3); // 5,14,11,67

  if (ch0 - ofst0 >= 'a' && ch0 - ofst0 <= 'z')
    ch0 -= ofst0;
  if (ch1 - ofst1 >= 'a' && ch1 - ofst1 <= 'z')
    ch1 -= ofst1;
  if (ch2 - ofst2 >= 'a' && ch2 - ofst2 <= 'z')
    ch2 -= ofst2;
  if (ch3 - ofst3 >= 0 && ch3 - ofst3 <= 127)
    ch3 -= ofst3;

  char str[5] = {ch0,ch1,ch2,ch3};

  if (strcmp(str, "bad!") == 0) {
    printf("Your string is bad!\n");
  } else {
    printf("You string is not bad!\n");
  }

  return 0;
}