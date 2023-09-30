#pragma once

#include "bridge_config.h"

#if REFRESH_INTERVAL != 0
#define PKT_PROCESS_BATCHING
#endif

#ifdef SCALABILITY_PROFILER 
#define DCHAIN_EXP_LEGACY
#endif

#ifdef SCALABILITY_PROFILER 
#if REFRESH_INTERVAL == 3000000000LL
#undef PKT_PROCESS_BATCHING
#endif
#endif
