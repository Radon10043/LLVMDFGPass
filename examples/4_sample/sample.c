#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  char str[5] = "sad!";
  int ofst = 0;
  scanf("%d", &ofst);

  if (ofst > 0 && ofst < 19)
    str[0] -= ofst;
  else
    printf("After offset will not be alpha, stop.\n");

  if (strcmp(str, "bad!") == 0) { // ofst == 17
    printf("%s. Your string is bad!\n", str);
    abort();
  } else {
    printf("Your string is not bad!\n");
  }

  return 0;
}