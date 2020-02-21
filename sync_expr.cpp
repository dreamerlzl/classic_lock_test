#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <pthread.h>
#include <cstdint>
#include <atomic>
#include "hrtimer_x86.h"

#define MAX_THREAD 32
// #define DEBUG

int counter;
static std::atomic<int> atom_counter(0);
int thread_num, num_cpu;
int I;
pthread_t * threads;
pthread_attr_t * attr;
int ** count_per_thread;
int * final;

int arrive_count;
pthread_mutex_t lock;
cpu_set_t cpuset;

void barrier(int);
void join();
void issue(void * (* func) (void *));

int no_sync();
int with_mutex();
int tas();
int tatas();
int tatas_backoff();
int ticket();
int mcs();
int fai();
int local();
void pause(int);

#define ROUND 8
double elapsed[ROUND];
typedef int(*funcp)();
funcp count_method[ROUND] = {no_sync, with_mutex, tas, tatas, tatas_backoff, ticket, mcs, fai};
char * round_name[] = {"no synchronization", "pthread mutex", "test and set", "test and test and set", 
    "test and test and set with backoff", "ticket lock", "MCS lock", "fetch and increment"};

class Qnode
{
  public:
    std::atomic<bool> waiting;
    std::atomic<Qnode *> next;
    Qnode(){waiting.store(false, std::memory_order_release); next.store(NULL, std::memory_order_release);}
};

std:: atomic<Qnode *> mcs_l(NULL);

void mcs_lock(Qnode * me)
{
    me->waiting.store(true, std::memory_order_acquire);
    Qnode * pre = mcs_l.exchange(me, std::memory_order_acq_rel);
    // printf("%ld enter with pre: %ld\n", me, pre);
    if(pre != NULL)
    {
        pre->next.store(me, std::memory_order_release);
        while(me->waiting.load(std::memory_order_acquire));
    }
}

void mcs_release(Qnode * me)
{
    Qnode * succ = me->next.load(std::memory_order_acquire);
    Qnode * original_me = me;
    int base = 1;
    if(succ == NULL)
    {
        if(mcs_l.compare_exchange_strong(me, NULL, std::memory_order_acq_rel))
        {
            // printf("%ld leaves\n", me);
            return;
        }
        me = original_me; // if CAS fails, me will have the value of mcs_l
        succ = me->next.load(std::memory_order_acquire);
        while(succ == NULL)
        {
            succ = me->next.load(std::memory_order_acquire);
            pause(base);
            base <<= 1;
            // printf("%ld next %ld\n", me, succ);
        }
    }
    succ->waiting.store(false, std::memory_order_release);
    // printf("%ld leaves\n", me);
}

int mcs()
{
    int my_count = 0;
    Qnode me = Qnode();
    while(counter < I)
    {
        mcs_lock(&me);
        // printf("%d\n", counter);
        if(counter < I)
        {
            ++counter;
            ++my_count;
        }
        mcs_release(&me);
    }
    return my_count;
}

class TicketLock
{
    public:
        int now_serving;
        int base;
        std::atomic<int> next_ticket;

        TicketLock(int b):now_serving(0), base(b){next_ticket.store(0, std::memory_order_release);}

        void acquire()
        {
            int my_ticket = next_ticket.fetch_add(1, std::memory_order_acquire);
            while(true)
            {
                if(now_serving == my_ticket)
                    return;
                pause(my_ticket - now_serving);
            }
        }

        void release()
        {
            ++now_serving;
        }
};

int ticket()
{
    static TicketLock lock = TicketLock(1);
    int my_count = 0;

    while(counter < I)
    {
        lock.acquire();
        if(counter < I)
        {
            ++counter;
            ++my_count;
        }
        lock.release();
    }

    return my_count;
}

void pause(int delay)
{
    for(int k = 0; k < delay; ++k);
}

int tatas_backoff()
{
    static std::atomic<bool> lock;
    int my_count = 0, delay;
    while(counter < I)
    {
        // while(lock.test_and_set(std::memory_order_acquire))
        //     while(lock.test(std::memory_order_acquire)); // sadly not supported by the g++ compiler on our cycle machine
        delay = 1;
        while(lock.exchange(true, std::memory_order_acquire))
            while(lock.load(std::memory_order_acquire))
            {
                pause(delay);
                delay <<= 1;
            }

        if(counter < I)
        {
            ++my_count;
            ++counter;
        }
        // lock.clear(std::memory_order_release);
        lock.store(false, std::memory_order_release);
    }
    return my_count;
}

int tatas()
{
    // static std::atomic_flag lock(false);
    static std::atomic<bool> lock;
    int my_count = 0;
    while(counter < I)
    {
        // while(lock.test_and_set(std::memory_order_acquire))
        //     while(lock.test(std::memory_order_acquire));
        while(lock.exchange(true, std::memory_order_acquire))
            while(lock.load(std::memory_order_acquire));
        if(counter < I)
        {
            ++my_count;
            ++counter;
        }
        // lock.clear(std::memory_order_release);
        lock.store(false, std::memory_order_release);
    }
    return my_count;
}

int tas()
{
    static std::atomic_flag lock(false);
    int my_count = 0;
    while(counter < I)
    {
        while(lock.test_and_set(std::memory_order_acquire));
        if(counter < I)
        {
            ++counter;
            ++my_count;
        }
        lock.clear(std::memory_order_release);
    }
    return my_count;
}

int local()
{
    int my_count = 0;
    while(my_count++ < I);
    return my_count;
}

int fai()
{
    int my_count = 0;
    while(atom_counter.fetch_add(1, std::memory_order_acq_rel) < I-1)
    {
        ++my_count;
    }
    return my_count;
}

int with_mutex()
{
    int my_count = 0;
    while(counter < I)
    {
        pthread_mutex_lock(&lock);
        if(counter < I) // without this the final sum will be larger than I 
        {
            ++counter;
            ++my_count;
        }
        pthread_mutex_unlock(&lock);
    }
    return my_count;
}

int no_sync ()
{
    int my_count = 0;
    while(counter < I)
    {
        ++counter;
        ++my_count;
    }
    return my_count;
}

void * expr(void * id)
{
    int pid = reinterpret_cast<intptr_t>(id);
    double start, end;
    barrier(pid);
    for(int r = 0; r < ROUND; ++r)
    {
        if(pid == 0)
        {
            counter = 0;
            start = gethrtime_x86();
        }
        
        barrier(pid);

        count_per_thread[pid][r] = (*count_method[r])();

        barrier(pid);
        if(pid == 0)
        {
            end = gethrtime_x86(); 
            elapsed[r] = end-start;
            final[r] = counter;
        }
    }
    pthread_exit(NULL);
}

void barrier(int tid)
{
    // static volatile unsigned long count = 0;
    static std:: atomic<int> count(0);
    static volatile unsigned int sense = 0;
    static volatile unsigned int thread_sense[MAX_THREAD] = {0};

    thread_sense[tid] = !thread_sense[tid];
    if (count.fetch_add(1, std::memory_order_relaxed) == thread_num - 1) {
        count = 0;
        sense = !sense;
    } else {
        while (sense != thread_sense[tid]);     /* spin */
    }
}

void issue(void * (* func) (void *))
{
    for(int k = 0;k < thread_num;++k)
        pthread_create(&threads[k], &attr[k], func, reinterpret_cast<void *>(k));
}

void join()
{
    for(int k = 0;k < thread_num;++k)
    {
        pthread_join(threads[k], NULL);
    }
}

void set_attr()
{
    int num_thread = thread_num;
    pthread_attr_t attr[num_thread];
    for(int k = 0; k < num_thread; ++k)
        pthread_attr_init(&attr[k]);
    num_cpu = (num_cpu < num_thread) ? num_cpu: num_thread;
    int thread_per_cpu = num_thread / num_cpu;
    int num_ot_cpu = num_thread - thread_per_cpu * num_cpu;

    for (int i = 0, j = 0; i < num_cpu; ++i)
    {
        for(int k = 0; k < thread_per_cpu + (i < num_ot_cpu); ++k, ++j)
        {
            // printf("thread id: %d cpu: %d\n", j, i);
            CPU_ZERO(&cpuset);
            CPU_SET(i, &cpuset);
            pthread_attr_setaffinity_np(&attr[j], sizeof(cpu_set_t), &cpuset);
            pthread_attr_setscope(&attr[j], PTHREAD_SCOPE_SYSTEM);
        }
    }
}

void init()
{
    threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_num);
    if(threads == NULL)
    {
        fprintf(stderr, "thread alloc fails\n");
        exit(1);
    }

    attr = (pthread_attr_t *)malloc(sizeof(pthread_attr_t) * thread_num);
    if(attr == NULL)
    {
        fprintf(stderr, "attr alloc fails\n");
        exit(1);
    }

    // init mutex lock
    pthread_mutex_init(&lock, NULL);

    count_per_thread = (int **) malloc(sizeof(int *) * thread_num);
    for(int k = 0; k < thread_num; ++k)
    {
        count_per_thread[k] = (int *) malloc(sizeof(int) * ROUND);
        for(int j = 0; j < ROUND; ++j)
            count_per_thread[k][j] = 0;
    }

    final = (int *) malloc(sizeof(int) * ROUND);
}

void clean()
{
    free(threads);
    free(attr);
    for(int k = 0; k < thread_num; ++k)
    {
        free(count_per_thread[k]);
    }
    free(count_per_thread);
    free(final);
}

void output()
{
    final[ROUND-1] = atom_counter; // fetch and increment
    for(int r = 0; r < ROUND; ++r)
    {
        printf("round %d %s elapsed time: %lf\n", r, round_name[r], elapsed[r]);
        printf("final value of the counter: %d\n", final[r]);
        for(int k = 0; k < thread_num; ++k)
            printf("thread %d counts %d times\n", k, count_per_thread[k][r]);
        printf("\n");
    }
}

int main(int argc, char * argv[])
{
    // std::atomic<bool> test;
    // std::cout << test.is_lock_free() << std::endl;
    if(argc != 7)
    {
        // -m [mode, 0 for total i times 1 for t * i times]
        printf("usage: %s -t [number of threads] -i [i] -c [number of processors]\n", argv[0]);
        exit(1);
    }

    thread_num = atoi(argv[2]);
    I = atoi(argv[4]);
    // mode = atoi(argv[6]);
    num_cpu = atoi(argv[6]);

    init(); // init all shared structures
    
    set_attr(); // set affinity, scope, etc.
    
    // begin testing
    issue(expr); 
    join();

    #ifndef DEBUG
    output();
    #endif 

    clean();
}