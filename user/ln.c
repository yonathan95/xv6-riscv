#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
  if (argc != 3 && (argc != 4 || (argc == 4 && !strcmp(argv[2], "-s"))))
  {
    exit(1);
  }
  if (argc == 3)
  {
    exit(link(argv[1], argv[2]));
  }
  else
  {
    exit(symlink(argv[2], argv[3]));
  }
}
