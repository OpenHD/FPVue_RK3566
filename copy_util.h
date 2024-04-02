//
// Created by consti10 on 02.04.24.
//

#ifndef FPVUE_COPY_UTIL_H
#define FPVUE_COPY_UTIL_H


#include <pthread.h>
#include <assert.h>
#include <string.h>

struct memcpy_args_t {
    void* src;
    void* dst;
    int len;
};
void* memcpy_data_function(void* args_uncast){
    struct memcpy_args_t* args=(struct memcpy_args_t*)args_uncast;
    memcpy(args->dst,args->src,args->len);
}

void memcpy_threaded(void* dest,void* src, int len,int n_threads){
    pthread_t threads[100];
    int consumed=0;
    int chunck=len/(n_threads);
    for(int i=0;i<n_threads-1;i++){
        struct memcpy_args_t memcpyArgs;
        memcpyArgs.src=src+consumed;
        memcpyArgs.dst=dest+consumed;
        memcpyArgs.len=chunck;
        int iret1 = pthread_create( &threads[i], NULL, &memcpy_data_function, (void*) &memcpyArgs);
        assert(iret1==0);
        consumed+=chunck;
    }
    // copy remaining (nth-thread is self)
    memcpy(dest+consumed,src+consumed,len-consumed);
    for(int i=0;i<n_threads-1;i++){
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
