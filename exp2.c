#define _GNU_SOURCE

#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include "hle-emulation.h"

#define IBM
#ifdef IBM
    #include <time.h>
#else
    #include "hrtimer_x86.h"
#endif

#define MAX_THREAD 32
#define RANDOM_COUNT 1000

//required
int I, num_cpu, num_thread, repeat;
double * exp2_time[2];

//thread
pthread_t * threads;
pthread_attr_t * attr;

//aff
cpu_set_t cpuset;

// utility
void barrier(int);
void join();
void issue(void * (* func) (void *));
int my_random(int from, int to); // [from, to)

int exp2_tas();
int exp2_hle();

typedef int(*funcp)();
funcp exp2_method[] = {exp2_tas, exp2_hle};

// 1000 tas locks
int tas_lock[RANDOM_COUNT];
int random_counter[RANDOM_COUNT];

int exp2_tas()
{
    int my_count = 0;
    for(; my_count < I; ++my_count)
    {
        int sample = my_random(0, RANDOM_COUNT);
        while(__atomic_test_and_set(&tas_lock[sample], __ATOMIC_ACQUIRE));
        ++random_counter[sample];
        __atomic_clear(&tas_lock[sample], __ATOMIC_RELEASE);
    }
    return my_count;
}

int my_random(int from, int to)
{
    return rand() % (to - from) + from;
}

int random_counter[RANDOM_COUNT];
static volatile int lock[RANDOM_COUNT];

int exp2_hle()
{
    int my_count = 0;
    for(; my_count < I; ++my_count)
    {
        int sample = my_random(0, RANDOM_COUNT);
        while (__hle_acquire_test_and_set1(&lock[sample]) == 1)
        {
            while (lock[sample])
                    _mm_pause();
        }
        ++random_counter[sample];
        __hle_release_clear1(&lock[sample]);
    }
    return my_count;
}

void * exp2 (void * id)
{
    int pid = (int) id;
    #ifndef IBM
        double start, end;
    #else
        struct timespec start, end;
    #endif

    for(int n = 0;n < repeat;++n)
    for(int r = 0;r < 2;++r)
    {
        barrier(pid);
        if(pid == 0)
        {
            #ifndef IBM
                start = gethrtime_x86();
            #else 
                clock_gettime(CLOCK_MONOTONIC, &start);
            #endif
        }
        barrier(pid);
        
        (*exp2_method[r])();

        barrier(pid);
        if(pid == 0)
        {
            #ifndef IBM
                exp2_time[r][n] = gethrtime_x86() - start;
            #else
                clock_gettime(CLOCK_MONOTONIC, &end);
                exp2_time[r][n] = (end.tv_nsec - start.tv_nsec)/1000000000.0 + end.tv_sec - start.tv_sec;
            #endif
        }
    }
}

pthread_mutex_t barrier_lock;
pthread_cond_t barrier_cond;
void barrier(int tid)
{
    static int count = 0;
    pthread_mutex_lock(&barrier_lock);
    if(count == num_thread - 1)
    {
        count = 0;
        pthread_cond_broadcast(&barrier_cond);
        // printf("%d enters and leaves!\n", tid);
    }
    else
    {
        // printf("%d enters\n", tid);
        ++count;
        while(pthread_cond_wait(&barrier_cond, &barrier_lock));
        // printf("%d leaves!\n", tid);
    }
    pthread_mutex_unlock(&barrier_lock);
}

// void barrier(int tid)
// {
//     // static volatile unsigned long count = 0;
//     static volatile int count = 0;
//     static volatile unsigned int sense = 0;
//     static volatile unsigned int thread_sense[MAX_THREAD] = {0};

//     thread_sense[tid] = !thread_sense[tid];
//     if (__atomic_fetch_add(&count, 1, __ATOMIC_RELEASE) == num_thread - 1) {
//         count = 0;
//         printf("%d enters and leaves!\n", tid);
//         sense = !sense;
//     } else {
//         printf("%d enters\n", tid);
//         int wait = 0;
//         while (sense != thread_sense[tid])
//         {
//             ++wait;
//             if(wait % 1000000 == 0)
//                 printf("%d waiting!\n", tid);
//         }   /* spin */
//         printf("%d leaves!\n", tid);
//     }
// }

void init()
{
    // set the random seed
    srand((unsigned) time(0));

    threads = (pthread_t *)malloc(sizeof(pthread_t) * num_thread);
    if(threads == NULL)
    {
        fprintf(stderr, "thread alloc fails\n");
        exit(1);
    }

    attr = (pthread_attr_t *)malloc(sizeof(pthread_attr_t) * num_thread);
    if(attr == NULL)
    {
        fprintf(stderr, "attr alloc fails\n");
        exit(1);
    }

    for(int k = 0; k < 2;++k)
    {
        exp2_time[k] = (double *)malloc(sizeof(double) * repeat);
        if(exp2_time[k] == NULL)
        {
            fprintf(stderr, "hle alloc fails\n");
            exit(1);
        }
    }

    pthread_mutex_init(&barrier_lock, NULL);
    pthread_cond_init(&barrier_cond, NULL);
}

void clean()
{
    free(threads);
    free(attr);
    for(int k = 0; k < 2;++k)
        free(exp2_time[k]);
}

void set_attr()
{
    pthread_attr_t attr[num_thread];
    for(int k = 0; k < num_thread; ++k)
        pthread_attr_init(&attr[k]);
    // num_cpu = (num_cpu < num_thread) ? num_cpu: num_thread;
    int thread_per_cpu = num_thread / num_cpu;
    int num_ot_cpu = num_thread - thread_per_cpu * num_cpu;

    for (int i = 0, j = 0; i < num_cpu; ++i)
    {
        for(int k = 0; k < thread_per_cpu + (i < num_ot_cpu); ++k, ++j)
        {
            CPU_ZERO(&cpuset);
            CPU_SET(i, &cpuset);
            pthread_attr_setaffinity_np(&attr[j], sizeof(cpu_set_t), &cpuset);
            pthread_attr_setscope(&attr[j], PTHREAD_SCOPE_SYSTEM);
        }
    }
}

void issue(void * (* func) (void *))
{
    for(int k = 0;k < num_thread;++k)
        pthread_create(&threads[k], &attr[k], func, (void *) k);
}

void join()
{
    for(int k = 0;k < num_thread;++k)
    {
        pthread_join(threads[k], NULL);
    }
}

void output()
{
    double tas_time = 0.0, hle_time = 0.0;
    // printf("repeat %d times\n", repeat);
    for(int n = 0; n < repeat; ++n)
    {
        tas_time += exp2_time[0][n];
        hle_time += exp2_time[1][n];
        //printf("%lf %lf\n", exp2_time[0][n], exp2_time[1][n]);
    }
    tas_time /= repeat;
    hle_time /= repeat;
    printf("%lf %lf\n", tas_time, hle_time);
}

int main(int argc, char ** argv)
{
    // default setting
    num_cpu = 4;
    num_thread = 4;
    I = 10000;
    repeat = 1;
    int rc;
    while((rc = getopt(argc, argv, "t:i:c:n:")) != -1)
    {
        switch(rc)
        {
            case 't':
                num_thread = atoi(optarg);
                break;
            case 'i':
                I = atoi(optarg);
                break;
            case 'c':
                num_cpu = atoi(optarg);
                break;
            case 'n':
                repeat = atoi(optarg);
                // printf("to repeat %d times\n", repeat);
                break;
            case '?':
                fprintf(stderr, "usage: -t [thread number] -i [i] -c [cpu number]\n");
                exit(1);
            default:    
                abort();
        }
    }

    init();
    set_attr();

    issue(exp2);
    join();

    output();
    clean();
}