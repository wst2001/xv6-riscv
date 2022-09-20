#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

char * readline(){
    char* buf = malloc(1000);
    char* p = buf;
    char ch;
    while (read(0, &ch, 1)){
        *p = ch;
        if (ch == '\n' || ch == '\0'){
            *p = '\0';
            break;
        }
        p ++;
    }
    if (p == buf){
        free(buf);
        return 0;
    }
    return buf;
}

int split(char* command, int nargc, char ** nargv){
    
    char* cmd = malloc(100);
    char* p = command;
    char* q = cmd;
    while (*p != '\0'){
        if (*p == ' '){
            nargv[nargc] = cmd;
            cmd = malloc(100);
            q = cmd;
            nargc ++;
            p ++;
            continue;
        }
        *q = *p;
        p ++;
        q ++;

    }
    nargv[nargc] = cmd;
    nargc ++;
    return nargc;
}



int
main(int argc, char *argv[]){
    if (argc < 2){
        printf("Usage: xarg [command]\n");
        exit(-1);
    }
    char * command;
    int nargc = argc-1;
    char ** nargv = malloc(100);
    for (int i = 1; i < argc; i ++){
        nargv[i-1] = argv[i];
    }
    while((command = readline())){
        nargc = split(command, nargc, nargv);
        
    }
    int pid = fork();
    if (pid == 0){
        exec(nargv[0], nargv);
    }
    else{
        wait(0);
    }

    exit(0);
}