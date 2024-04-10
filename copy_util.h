//
// Created by consti10 on 02.04.24.
//

#ifndef FPVUE_COPY_UTIL_H
#define FPVUE_COPY_UTIL_H


#include <pthread.h>
#include <assert.h>
#include <string.h>


#include <arm_neon.h>

void memcpy_neon_8(uint8_t* region2, const uint8_t* region1, size_t length){
    assert(length % 8 == 0);

    uint8x8_t in;

    for (const uint8_t *end = region1 + length; region1 < end; region1 += 8, region2 += 8) {
        in = vld1_u8(region1);
        vst1_u8(region1, out);
    }
}


void memcpy_neon_aligned(void* dst, const void * src, size_t length){
    int len_fast=length % 8;
    memcpy_neon_8((uint8_t*)dst,(const uint8_t*)src,len_fast);
    int len_slow=length-len_fast;
    if(len_slow>0){
        memcpy(dst+len_fast,src+len_fast,len_slow);
    }
}


extern "C"{
void *mempcpy(void * __restrict s1, const void * __restrict s2, size_t n);
};

void simple_memcpy (char *dst, const char *src, size_t n)
{
    char *ret = dst;
    while (n--)
        *dst++ = *src++;
}

struct memcpy_args_t {
    void* src;
    void* dst;
    int len;
};
void* memcpy_data_function(void* args_uncast){
    struct memcpy_args_t* args=(struct memcpy_args_t*)args_uncast;
    mempcpy(args->dst,args->src,args->len);
    //memmove(args->dst,args->src,args->len);
    //simple_memcpy(args->dst,args->src,args->len);
    return NULL;
}

void memcpy_threaded(void* dest,void* src, int len,int n_threads){
    pthread_t threads[100];
    struct memcpy_args_t memcpyArgs[100];
    int consumed=0;
    int chunck=len/(n_threads);
    for(int i=0;i<n_threads;i++){
        memcpyArgs[i].src=src+consumed;
        memcpyArgs[i].dst=dest+consumed;
        int this_thread_len;
        if(i==n_threads-1){
            // might not be even
            this_thread_len=len-consumed;
        }else{
            this_thread_len=chunck;
        }
        memcpyArgs[i].len=this_thread_len;
        int iret1 = pthread_create( &threads[i], NULL, &memcpy_data_function, (void*) &memcpyArgs[i]);
        assert(iret1==0);
        consumed+=this_thread_len;
    }
    assert(consumed==len);
    for(int i=0;i<n_threads;i++){
        pthread_join(threads[i], NULL);
    }
    /*pthread_t thread1;
    struct memcpy_args_t memcpyArgs;
    int len_first=len / 2;
    int len_second=len-len_first;
    memcpyArgs.src=src;
    memcpyArgs.dst=dest;
    memcpyArgs.len=len_first;
    int iret1 = pthread_create( &thread1, NULL, &memcpy_data_function, (void*) &memcpyArgs);
    assert(iret1==0);
    memcpy(dest+len_first,src+len_first,len_second);
    pthread_join(thread1, NULL);*/
}

#endif //FPVUE_COPY_UTIL_H
