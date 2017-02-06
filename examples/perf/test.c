#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>


int main(){

    char test[1024] ={0,};
    char buf[1024] ={0,};
    int i = 100;
    
    sprintf(test,"./%d", i);
    mkdir(test,0755);
    chdir(test);

    getcwd(buf, 1024);
    printf("%s\n",buf);
}
