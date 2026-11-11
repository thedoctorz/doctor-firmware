#pragma once
#include <stdlib.h>
#include "stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate string stream
 * @return Stream* 
 */
Stream* string_stream_alloc(void);

/**
 * Release the spare capacity a string stream keeps after growing. Writes grow the buffer
 * by half again each time it fills, so a stream that is written once and then kept, such as
 * a received signal held in a list, otherwise carries up to a third of its size unused
 * @param stream Stream instance, must be a string stream
 */
void string_stream_shrink_to_fit(Stream* stream);

#ifdef __cplusplus
}
#endif
