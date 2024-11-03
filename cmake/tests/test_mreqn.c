#include <netinet/in.h>

int main()
{
    struct ip_mreqn mreq;
    mreq.imr_address.s_addr = 0;
}
