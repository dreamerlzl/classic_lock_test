#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <pthread.h>
#include <cstdint>
#include <atomic>
#include <sstream>
#include <unistd.h>
// #include <vector>
#include <fstream>
#include "hrtimer_x86.h"

#define MAX_THREAD 32
#define RANDOM_COUNT 1000
#define IBM
#ifdef IBM
    #include <time.h>
#else
    #include "hrtimer_x86.h"
#endif
// #define DEBUG

// required
int counter;
int thread_num;
int I;
int mode; // mode = 1 means experiment 1, mode = 2 means experiment 2
int repeat;

// for fetch and increment
static std::atomic<int> atom_counter(0);

// thread-related 
pthread_t * threads;
pthread_attr_t * attr;
int *** count_per_thread;
int * final;

// barrier
int arrive_count;
pthread_mutex_t lock;

//affinity 
int num_cpu;
cpu_set_t cpuset;

//utility
void barrier(int);
void join();
void issue(void * (* func) (void *));
int random(int from, int to); // [from, to)

// experiment 1 
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

#define ROUND 9
double * elapsed[ROUND];
typedef int(*funcp)();
funcp count_method[ROUND] = {no_sync, with_mutex, tas, tatas, tatas_backoff, ticket, mcs, fai, local};
char * round_name[] = {"no synchronization", "pthread mutex", "test and set", "test and test and set", 
    "test and test and set with backoff", "ticket lock", "MCS lock", "fetch and increment", "local"};

int local()
{
    int my_count = 0;
    while(my_count < I)
        ++my_count;
    return my_count;
}

class Qnode
{
  public:
    bool waiting;
    Qnode * next;
    // std::atomic<Qnode *> next;
    Qnode()
    {
        waiting = false; 
        next = NULL;
        // next.store(NULL, std::memory_order_release);
    }
};

std:: atomic<Qnode *> mcs_l(NULL);

void mcs_lock(Qnode * me)
{
    me->waiting = true;
    me->next = NULL;
    Qnode * pre = mcs_l.exchange(me, std::memory_order_relaxed);
    // printf("%ld enter with pre: %ld\n", me, pre);
    if(pre != NULL)
    {
        pre->next = me;
        // pre->next.store(me, std::memory_order_release);
        while(me->waiting);
    }
}

void mcs_release(Qnode * me)
{
    // Qnode * succ = me->next.load(std::memory_order_acquire);
    Qnode * succ = me->next;
    Qnode * original_me = me;
    if(succ == NULL)
    {
        if(mcs_l.compare_exchange_strong(me, NULL, std::memory_order_relaxed))
        {
            // printf("%ld leaves\n", me);
            return;
        }
        me = original_me; // if CAS fails, me will have the value of mcs_l
        // succ = me->next.load(std::memory_order_acquire);
        succ = me->next;
        int base = 1;
        while(succ == NULL)
        {
            // succ = me->next.load(std::memory_order_acquire);
            succ = me->next;
            pause(base);
            base <<= 1;
            // printf("%ld next %ld\n", me, succ);
        }
    }
    succ->waiting = false;
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

        TicketLock():now_serving(0)
        {
            base = 1;
            next_ticket.store(0, std::memory_order_release);
        }

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
    int my_count = 0;
    static TicketLock ticket_lock;
    while(counter < I)
    {
        ticket_lock.acquire();
        if(counter < I)
        {
            ++counter;
            ++my_count;
        }
        ticket_lock.release();
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

// experiment 2
// std:: vector<double> exp2_time[2];

// // 1000 tas locks
// std::atomic_flag tas_lock[RANDOM_COUNT];
// int random_counter[RANDOM_COUNT];

// int exp2_tas()
// {
//     int my_count = 0;
//     for(; my_count < I; ++my_count)
//     {
//         int sample = random(0, RANDOM_COUNT);
//         while(tas_lock[sample].test_and_set(std::memory_order_acquire));
//         ++random_counter[sample];
//         tas_lock[sample].clear(std::memory_order_release);
//     }
//     return my_count;
// }

// funcp exp2_method[] = {exp2_tas};

// void * exp2(void * id)
// {
//     int pid = reinterpret_cast<intptr_t>(id);
//     #ifndef IBM
//         double start;
//     #else
//         struct timespec start, end;
//     #endif
    
//     for(int n = 0; n < repeat; ++n)
//     for(int r = 0; r < 1; ++r)
//     {   
//         barrier(pid);
//         if(pid == 0)
//         {
//             #ifndef IBM
//                 start = gethrtime_x86();
//             #else 
//                 clock_gettime(CLOCK_MONOTONIC, &start);
//             #endif
//         }
//         barrier(pid);
        
//         (*exp2_method[r])();

//         barrier(pid);
//         if(pid == 0)
//         {
//             #ifndef IBM
//                 exp2_time[r].push_back(gethrtime_x86() - start);
//             #else
//                 clock_gettime(CLOCK_MONOTONIC, &end);
//                 exp2_time[r].push_back((end.tv_nsec - start.tv_nsec)/1000000000.0 + end.tv_sec - start.tv_sec);
//             #endif
//         }
//     }
// }

void * exp1(void * id)
{
    int pid = reinterpret_cast<intptr_t>(id), r;
    #ifndef IBM
        double start, end;
    #else
        struct timespec start, end;
    #endif

    for(int n = 0; n < repeat; ++n)
    {
        barrier(pid);
        for(r = 0; r < ROUND-1; ++r)
        {
            if(pid == 0)
            {
                counter = 0;
                #ifndef IBM
                    start = gethrtime_x86();
                #else 
                    clock_gettime(CLOCK_MONOTONIC, &start);
                #endif
            }
            
            barrier(pid);

            count_per_thread[pid][r][n] = (*count_method[r])();

            barrier(pid);
            if(pid == 0)
            {
                #ifndef IBM
                    end = gethrtime_x86(); 
                    elapsed[r][n] = end-start;
                #else
                    clock_gettime(CLOCK_MONOTONIC, &end);
                    elapsed[r][n] = end.tv_sec - start.tv_sec;
                    elapsed[r][n] += (end.tv_nsec - start.tv_nsec)/1000000000.0;
                #endif
                final[r] = counter;
            }
        }

        // privatization 
        if(pid == 0)
        {
            #ifndef IBM
                start = gethrtime_x86();
            #else 
                clock_gettime(CLOCK_MONOTONIC, &start);
            #endif
        }
        
        barrier(pid);

        count_per_thread[pid][r][n] = (*count_method[r])();

        barrier(pid);
        if(pid == 0)
        {
            int sum = 0;
            for(int k = 0; k < thread_num; ++k)
                sum += count_per_thread[k][r][n];
            #ifndef IBM
                end = gethrtime_x86(); 
                elapsed[r][n] = end-start;
            #else
                clock_gettime(CLOCK_MONOTONIC, &end);
                elapsed[r][n] = end.tv_sec - start.tv_sec;
                elapsed[r][n] += (end.tv_nsec - start.tv_nsec)/1000000000.0;
            #endif
            final[r] = sum;
        }
    }

    pthread_exit(NULL);
}

int random(int from, int to)
{
    return rand() % (to - from) + from;
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
    // num_cpu = (num_cpu < num_thread) ? num_cpu: num_thread;
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
    // set the random seed
    srand((unsigned) time(0));

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

    count_per_thread = (int ***) malloc(sizeof(int **) * thread_num);
    for(int k = 0; k < thread_num; ++k)
    {
        count_per_thread[k] = (int **) malloc(sizeof(int *) * ROUND);
        for(int r = 0; r < ROUND; ++r)
        {    
            count_per_thread[k][r] = (int *) malloc(sizeof(int) * repeat);
            for(int n = 0; n < repeat; ++n)
                count_per_thread[k][r][n] = 0;
        }
    }

    final = (int *) malloc(sizeof(int) * ROUND);

    for(int r = 0; r < ROUND; ++r)
    {
        elapsed[r] = (double *)malloc(sizeof(double) * repeat);
        if(elapsed[r] == NULL)
        {
            fprintf(stderr, "memory for storing time count alloc fails\n");
            exit(1);
        }
    }
    // init 1000 tas locks for expr2
    // for(int i = 0; i < RANDOM_COUNT; ++i)
    // {   
    //     random_counter[i] = 0; 
    //     tas_lock[i].clear(std::memory_order_relaxed);
    // }
}

void clean()
{
    free(threads);
    free(attr);
    for(int k = 0; k < thread_num; ++k)
    {
        for(int r = 0; r < ROUND;++r)
            free(count_per_thread[k][r]);
        free(count_per_thread[k]);
    }
    free(count_per_thread);
    free(final);
}

void output()
{
    if(mode == 1)
    {
        using namespace std;
        stringstream s1;
        s1 << "thread_count_" << thread_num << ".txt";
        ofstream output(s1.str());
        if(!output.is_open())
        {
            fprintf(stderr, "output file thread_count_%d.txt open fails", thread_num);
            exit(1);
        }

        final[ROUND-2] = atom_counter; // fetch and increment
        for(int r = 0; r < ROUND; ++r)
        {
            double time_this_round = 0.0;
            for(int n = 0; n < repeat; ++n)
                time_this_round += elapsed[r][n];
            time_this_round /= repeat;
            printf("%lf %d\n", time_this_round, final[r]);
            for(int k = 0; k < thread_num; ++k)
            {
                int thread_count = 0;
                for(int n = 0; n < repeat; ++n)
                    thread_count += count_per_thread[k][r][n];
                thread_count /= repeat;
                output << thread_count << endl;
            }
            output << endl << endl;
            // printf("round %d %s elapsed time: %lf\n", r, round_name[r], elapsed[r]);
            // printf("final value of the counter: %d\n", final[r]);
            // for(int k = 0; k < thread_num; ++k)
            //     printf("thread %d counts %d times\n", k, count_per_thread[k][r]);
            // printf("\n");
        }
    }
    // else
    // {   
    //     for(int n = 0;n < repeat; ++n)
    //     {
    //         printf("%lf %lf\n", exp2_time[0][n], exp2_time[1][n]);
    //         // for(int i = 0; i < RANDOM_COUNT;++i)
    //         //     printf("%d: %d\n",i, random_counter[i]);
    //     }
    // }
}

int main(int argc, char ** argv)
{
    // std::atomic<bool> test;
    // std::cout << test.is_lock_free() << std::endl;

    // default setting
    num_cpu = 4;
    thread_num = 4;
    I = 10000;
    mode = 1;
    repeat = 1;
    int rc;
    while((rc = getopt(argc, argv, "t:i:c:m:n:")) != -1)
    {
        switch(rc)
        {
            case 't':
                thread_num = atoi(optarg);
                break;
            case 'i':
                I = atoi(optarg);
                break;
            case 'c':
                num_cpu = atoi(optarg);
                break;
            case 'm':
                // mode = atoi(optarg);
                break;
            case 'n':
                repeat = atoi(optarg);
                break;
            case '?':
                fprintf(stderr, "usage: -t [thread number] -i [i] -c [cpu number]\n");
                exit(1);
            default:    
                abort();
        }
    }

    init(); // init all shared structures
    
    set_attr(); // set affinity, scope, etc.
    
    // if(mode == 1)
    //     issue(exp1); 
    // else
    //     issue(exp2);
    issue(exp1);
    join();

    #ifndef DEBUG
    output();
    #endif 

    clean();
}