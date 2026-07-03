#include "services/config_notify.h"

static volatile uint32_t s_generation = 0;

extern "C" void CONFIG_NOTIFY_Bump(void)
{
    s_generation = s_generation + 1u;
}

extern "C" uint32_t CONFIG_NOTIFY_Generation(void)
{
    return s_generation;
}
