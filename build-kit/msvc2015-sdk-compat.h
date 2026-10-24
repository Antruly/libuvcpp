#pragma once
/* Windows SDK 10.0.26100's UCRT uses _mm_loadu_si64 in wchar.h.
   VS2015 (19.00) only has _mm_loadl_epi64; both load 64 bits. */
#if defined(_MSC_VER) && _MSC_VER < 1920
#include <intrin.h>
#ifndef _mm_loadu_si64
#define _mm_loadu_si64(p) _mm_loadl_epi64((const __m128i *)(p))
#endif
#endif
