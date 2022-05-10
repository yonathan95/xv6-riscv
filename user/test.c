#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"

int
main(int argc, char *argv[]){
    int status;
    int pid = fork();
    for(int i = 0; i < 10;i++){
        kill(pid);
        pid = fork();
        sleep(i * 2);
        fprintf(2,"cpu: %d\n", get_cpu());
    }

    if (pid != 0){
        sleep(1);
        kill(pid);
    }
    else{
        sleep(10);
    }
    wait(&status);
    fprintf(2,"done\n");
    exit(0);
}