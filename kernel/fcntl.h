#define O_RDONLY   0x000
#define O_WRONLY   0x001
#define O_RDWR     0x002
#define O_CREATE   0x200
#define O_TRUNC    0x400
#define O_NOFOLLOW 0x800
// If "open" system call is enabledd with O_NOFOLLOW
// I will let "open" open the symlink (and not follow the symlink)

#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_SHARED      0x01
// MAP_SHARED means that modifications to the mapped memory should
// Always be written back to file
// It's OK if process that map the same MAP_SHARED files DO NOT
// shared physical pages
#define MAP_PRIVATE     0x02
// MAP_PRIVATE means that we should not write back on modifying
