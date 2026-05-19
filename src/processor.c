#include <stdio.h>

int main () {

  int *T;

  T=(int *)"\01\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";
  printf ("\n\n\nCPC4X processor test:\n");
  printf ("---------------------\n");

  if(*T==1) {
     printf("\nThis machine is high-endian. The lowest significant byte comes first:\n\n");
     printf("Please edit %cMakefile.admin%c or %cMakefile.user%c and set the variable\n", 34,34,34,34);
     printf("PROCESSOR = -DLSB_FIRST\n\n\n\n");
  }
  else {
     printf("\nThis machine is low-endian. The most significant byte comes first:\n\n");
     printf("Please edit %cMakefile.admin%c or %cMakefile.user%c and set the variable\n", 34,34,34,34);
     printf("PROCESSOR = -DMSB_FIRST\n\n\n\n");
  }
  return 0;
}

