// Kernel taint tracking test: reading an untrusted file taints a process,
// taint follows fork() and pipes, and tainted processes cannot unlink().
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

static void
check(int ok, char *what)
{
  printf(1, "%s: %s\n", ok ? "PASS" : "FAIL", what);
}

static void
readfile(char *path)
{
  char buf[128];
  int fd = open(path, O_RDONLY);

  while(read(fd, buf, sizeof(buf)) > 0)
    ;
  close(fd);
}

static int
mkfile(char *path)
{
  int fd = open(path, O_CREATE | O_RDWR);

  if(fd < 0)
    return -1;
  close(fd);
  return 0;
}

int
main(void)
{
  int p[2];
  char c;

  check(mkfile("scratch1") == 0 && unlink("scratch1") == 0, "untainted process may unlink");
  check(untrust("inbox.txt") == 0, "inbox.txt marked untrusted");
  readfile("report.txt");
  check(mkfile("scratch1") == 0 && unlink("scratch1") == 0, "reading a trusted file does not taint");

  pipe(p);
  if(fork() == 0){
    readfile("inbox.txt");
    check(unlink("important.txt") < 0, "tainted process cannot unlink important.txt");
    check(open("report.txt", O_RDONLY) >= 0, "tainted process can still read files");
    check(mkfile("scratch2") == 0, "tainted process can still create files");
    if(fork() == 0){
      check(unlink("important.txt") < 0, "taint is inherited across fork");
      exit();
    }
    wait();
    write(p[1], "x", 1);
    exit();
  }
  wait();
  if(fork() == 0){
    read(p[0], &c, 1);
    check(unlink("important.txt") < 0, "taint propagates through a pipe");
    exit();
  }
  wait();
  check(open("important.txt", O_RDONLY) >= 0, "important.txt still exists");
  check(unlink("scratch2") == 0, "untainted parent may still unlink");
  printf(1, "tainttest done\n");
  exit();
}
