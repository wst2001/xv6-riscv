#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void get_primes(int rfd){
    int num, base_num;
    read(rfd, &base_num, 4);
    printf("prime %d\n", base_num);

    int created_pipe = 0;
    int fd[2];
    while (read(rfd, &num, 4) != 0){
        if (!created_pipe){
            created_pipe = 1;
            if (pipe(fd) < 0){
                printf("pipe\n");
                exit(-1);
            }
            int pid = fork();
            if (pid != 0){
                close(fd[0]);
            }
            else{
                close(fd[1]);
                get_primes(fd[0]);
                return;
            }
        }
        if (num % base_num != 0){
            write(fd[1], &num, 4);
        }
    }
    close(rfd);
    close(fd[1]);
    wait(0);

}

int main(int argc, char *argv[]){
    int fd[2];
    if (pipe(fd) < 0){
        printf("pipe\n");
        exit(-1);
    }
    int pid = fork();
    if (pid != 0){
        close(fd[0]);
        for (int i = 2; i <= 35; i ++){
            write(fd[1], &i, 4);
        }
        close(fd[1]);
        wait(0);
    }
    else{
        close(fd[1]);
        get_primes(fd[0]);
        close(fd[0]);
    }
    exit(0);
}