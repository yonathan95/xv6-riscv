#include "kernel/types.h"
#include "user.h"
#include "kernel/fcntl.h"

#define TOTAL_FILE_SIZE (1<<20)
#define BLOCK_SIZE (1<<10)
#define DIRECT_BLOCKS 12
#define INDIRECT_BLOCKS 256
#define DINDIRECT_BLOCKS (10 * TOTAL_FILE_SIZE)/(BLOCK_SIZE)-DIRECT_BLOCKS-INDIRECT_BLOCKS


char buffer[BLOCK_SIZE];

int main(int argc, char** argv){
    int fd = open("test.txt",O_CREATE | O_RDWR);

    for(int i=0;i<DIRECT_BLOCKS;++i){
        if(write(fd,buffer,BLOCK_SIZE)<0){
            exit(1);
        }
    }
    fprintf(2,"Finished writing 12KB (direct)\n");

    for(int i=0;i<INDIRECT_BLOCKS;++i){
        if(write(fd,buffer,BLOCK_SIZE)<0){
            exit(1);
        }
    }
    fprintf(2,"Finished writing 268KB (single indirect)\n");

    for(int i=0;i<DINDIRECT_BLOCKS;++i){
        if(write(fd,buffer,BLOCK_SIZE)<0){
            exit(1);
        }
    }
    fprintf(2,"Finished writing 10MB\n");
    exit(1);
}