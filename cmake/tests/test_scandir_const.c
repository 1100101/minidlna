#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>

int main()
{
    int filter(struct dirent *d);
    struct dirent **ptr = NULL;
    char *name = NULL;
    (void)scandir(name, &ptr, filter, alphasort);
    return 0;
}