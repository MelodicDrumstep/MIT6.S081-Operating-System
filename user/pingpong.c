#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

/*
Write a program that uses UNIX system calls to ''ping-pong'' a byte
between two processes over a pair of pipes, one for each direction. 
The parent should send a byte to the child; 
the child should print "<pid>: received ping", where <pid> is its process ID,
write the byte on the pipe to the parent, and exit; 
the parent should read the byte from the child, print "<pid>: received pong", and exit. 
Your solution should be in the file user/pingpong.c.
*/

int main(int argc, char *argv[])
{
    char buf[1];
    int p1[2]; // parent -> child
    int p2[2]; // child -> parent

    if(argc != 1)
    {
        fprintf(2, "usage: pingpong\n");
        exit(1);
    }

    if(pipe(p1) < 0 || pipe(p2) < 0)
    {
        fprintf(2, "pipe error\n");
    }

    
    if(fork() == 0) // This is the child process
    {
        close(p1[1]);
        close(p2[0]);

        if(read(p1[0], buf, 1) != 1)
        {
            fprintf(2, "child: read error\n");
            exit(1);
        }
        printf("%d: received ping\n", getpid());

        // Write to parent
        write(p2[1], "a", 1);
        close(p1[0]);
        close(p2[1]);
        exit(0);
    }

    close(p1[0]);
    close(p2[1]);

    // Write to child
    write(p1[1], "a", 1);

    wait(0);

    // Read from child
    if(read(p2[0], buf, 1) != 1)
    {
        fprintf(2, "parent: read error\n");
        exit(1);
    }
    printf("%d: received pong\n", getpid());

    close(p1[1]);
    close(p2[0]);
    exit(0);
}
