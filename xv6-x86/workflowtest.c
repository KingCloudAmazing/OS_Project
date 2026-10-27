
#include "types.h"
#include "stat.h"
#include "user.h"

int
main(void)
{
  printf(1, "setworkflow(1, 200) = %d\n", setworkflow(1, 200));
  printf(1, "setworkflow(2, 150) = %d\n", setworkflow(2, 150));

  exit();
}

