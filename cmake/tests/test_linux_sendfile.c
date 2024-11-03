#include <sys/types.h>
#include <sys/sendfile.h>

int main()
{
    int tofd = 0, fromfd = 0;
    off_t offset;
    size_t total = 0;
    ssize_t nwritten = sendfile(tofd, fromfd, &offset, total);
    return nwritten;
}