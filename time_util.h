//
// Created by consti10 on 31.03.24.
//

#ifndef FPVUE_TIME_UTIL_H
#define FPVUE_TIME_UTIL_H

#include <time.h>
#include <stdlib.h>
#include <stdint.h>

/**
 * @return milliseconds
 */
uint64_t get_time_ms() {
    struct timespec spec;
    if (clock_gettime(1, &spec) == -1) { /* 1 is CLOCK_MONOTONIC */
        abort();
    }
    return spec.tv_sec * 1000 + spec.tv_nsec / 1e6;
}

void print_time_ms(const char* tag,uint64_t ms){
    printf("%s %dms\n",tag,(int)ms);
}


#endif //FPVUE_TIME_UTIL_H
