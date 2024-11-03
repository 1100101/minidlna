#include <sys/types.h>
#ifdef IS_DIRENT
#include <dirent.h>
#elif IS_SYS_NDIR
#include <sys/ndir.h>
#elif IS_SYS_DIR
#include <sys/dir.h>
#elif IS_NDIR
#include <ndir.h>
#endif

int
main (void)
{
    if ((DIR *) 0)
        return 0;
    return 0;
}
