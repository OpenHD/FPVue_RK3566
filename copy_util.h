//
// Created by consti10 on 02.04.24.
//

#ifndef FPVUE_COPY_UTIL_H
#define FPVUE_COPY_UTIL_H


#include <pthread.h>
#include <assert.h>
#include <string.h>

/*
 * sizeof(word) MUST BE A POWER OF TWO
 * SO THAT wmask BELOW IS ALL ONES
 */
typedef	int word;		/* "word" used for optimal copy speed */

#define	wsize	sizeof(word)
#define	wmask	(wsize - 1)

/*
 * Copy a block of memory, handling overlap.
 * This is the routine that actually implements
 * (the portable versions of) bcopy, memcpy, and memmove.
 */

void * memcpy_apple(void *dst0, const void *src0, size_t length)
{
    char *dst =(char*) dst0;
    const char *src = (const char *)src0;
    size_t t;

    if (length == 0 || dst == src)		/* nothing to do */
        goto done;

    /*
     * Macros: loop-t-times; and loop-t-times, t>0
     */
#define	TLOOP(s) if (t) TLOOP1(s)
#define	TLOOP1(s) do { s; } while (--t)

    if ((unsigned long)dst < (unsigned long)src) {
        /*
         * Copy forward.
         */
        t = (uintptr_t)src;	/* only need low bits */
        if ((t | (uintptr_t)dst) & wmask) {
            /*
             * Try to align operands.  This cannot be done
             * unless the low bits match.
             */
            if ((t ^ (uintptr_t)dst) & wmask || length < wsize)
                t = length;
            else
                t = wsize - (t & wmask);
            length -= t;
            TLOOP1(*dst++ = *src++);
        }
        /*
         * Copy whole words, then mop up any trailing bytes.
         */
        t = length / wsize;
        TLOOP(*(word *)dst = *(word *)src; src += wsize; dst += wsize);
        t = length & wmask;
        TLOOP(*dst++ = *src++);
    } else {
        /*
         * Copy backwards.  Otherwise essentially the same.
         * Alignment works as before, except that it takes
         * (t&wmask) bytes to align, not wsize-(t&wmask).
         */
        src += length;
        dst += length;
        t = (uintptr_t)src;
        if ((t | (uintptr_t)dst) & wmask) {
            if ((t ^ (uintptr_t)dst) & wmask || length <= wsize)
                t = length;
            else
                t &= wmask;
            length -= t;
            TLOOP1(*--dst = *--src);
        }
        t = length / wsize;
        TLOOP(src -= wsize; dst -= wsize; *(word *)dst = *(word *)src);
        t = length & wmask;
        TLOOP(*--dst = *--src);
    }
    done:
    return (dst0);
}


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
    //memcpy(args->dst,args->src,args->len);
    memcpy_apple(args->dst,args->src,args->len);
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
