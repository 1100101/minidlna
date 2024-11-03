#include <netinet/in.h>

int main()
{
    struct ip_mreq mreq;
    mreq.imr_interface.s_addr = 0;
    return 0;
}
