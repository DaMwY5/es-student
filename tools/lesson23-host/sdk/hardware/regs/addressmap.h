#pragma once
#include <stdint.h>
extern uint32_t host_chip_id;
#define SYSINFO_BASE ((uintptr_t)&host_chip_id)
