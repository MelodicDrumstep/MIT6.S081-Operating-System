#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

//I'm implementing the shell for sleep command
//usage : sleep n

int main(int argc, char *argv[])
{
  if(argc != 2)
  {
    fprintf(2, "usage: sleep n\n");
    exit(1);
  }
  sleep(atoi(argv[1]));
  exit(0);
}
