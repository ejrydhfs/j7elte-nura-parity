#include <stdlib.h>
static int f(int x){ int *p = 0; *p = x; return *p; }
int main(void){ return f(1); }
