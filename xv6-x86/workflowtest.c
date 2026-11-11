
#include "types.h"
#include "stat.h"
#include "user.h"

int
main(void)
{
  printf(1, "setworkflow(1, 200) = %d\n", setworkflow(1, 200));
  printf(1, "setworkflow(2, 150) = %d\n", setworkflow(2, 150));
  printf(1, "setworkflow(-1, 100) = %d (expect -1)\n", setworkflow(-1, 100));
  printf(1, "setworkflow(3, -5) = %d (expect -1)\n", setworkflow(3, -5));

  exit();
}

