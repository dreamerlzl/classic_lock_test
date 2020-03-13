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

// #define IBM

static const int MAX_THREAD = 128;
#ifdef IBM
static const int MAX_CPU = 160;
static const int threads_per_core = 8;
#else
static const int MAX_CPU = 72;
static const int threads_per_core = 2;
#endif

#ifdef IBM
    #include <time.h>
    #include <ppu_intrinsics.h>
#else
    #include "hrtimer_x86.h"
    #include <immintrin.h>
#endif

static int debug = 0;
static int same_socket = 0;
static int mode = 0; // mode == 1 means each thread counts I times
// required
static volatile int counter;
static int thread_num;
static int I;
static int repeat;

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

// maximum backoff time for backoff
static const int max_delay = (2<<14);

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

static const int ROUND = 9;
double * elapsed[ROUND];
typedef int(*funcp)();
funcp count_method[ROUND] = {no_sync, with_mutex, tas, tatas, tatas_backoff, ticket, mcs, fai, local};
// char * round_name[] = {"no synchronization", "pthread mutex", "test and set", "test and test and set", 
//     "test and test and set with backoff", "ticket lock", "MCS lock", "fetch and increment", "local"};

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
    Qnode * pre = mcs_l.exchange(me, std::memory_order_release);
    // printf("%ld enter with pre: %ld\n", me, pre);
    if(pre != NULL)
    {
        pre->next = me;
        // pre->next.store(me, std::memory_order_release);
        #ifdef IBM
        int base = 1;
        #endif
        while(me->waiting)
        {
            #ifndef IBM
            _mm_pause();
            #else
            // pause(std::min(2<<12, base));
            // base <<= 1;
            #endif
        }    
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
        #ifdef IBM
        int base = 1;
        #endif
        while(succ == NULL)
        {
            // succ = me->next.load(std::memory_order_acquire);
            succ = me->next;
            #ifdef IBM
            pause(std::min(base, 1024));
            base <<= 1;
            #else
            _mm_pause();
            #endif
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
    if(mode)
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
    else
        while(my_count < I)
        {
            mcs_lock(&me);
            #ifdef IBM
                __lwsync();
            #endif
            ++counter;
            ++my_count;
            #ifdef IBM
                __lwsync();
            #endif
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
            next_ticket.store(0, std::memory_order_relaxed);
        }

        int acquire()
        {
            int my_ticket = next_ticket.fetch_add(1, std::memory_order_relaxed);
            while(now_serving != my_ticket)
            {
                #ifdef IBM
                pause(my_ticket - now_serving);
                #else
                _mm_pause();
                #endif
            }
            return my_ticket;
        }

        void release()
        {
            ++now_serving;
        }
};




int ticket()
{
    int my_count = 0;
    int my_ticket = 0;
    static TicketLock ticket_lock;
    if(mode)
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
    else
        while(my_count < I)
        {
            my_ticket = ticket_lock.acquire();
            #ifdef IBM
                __lwsync();
            #endif
            if(ticket_lock.now_serving == my_ticket)
            {   
                ++my_count;
                ++counter;
            }
            else
            {
                printf("lock aquisition and later critical session accesses are reordered!\n");
                exit(1);
            }
            #ifdef IBM
                __lwsync();
            #endif
            ticket_lock.release();
        }
    
    return my_count;
}

void pause(int delay)
{
    delay = std::min(delay, max_delay);
    for(int k = 0; k < delay; ++k);
}

int tatas_backoff()
{
    static std::atomic<bool> lock;
    int my_count = 0, delay;
    if(mode)
        while(counter < I)
        {
            // while(lock.test_and_set(std::memory_order_acquire))
            //     while(lock.test(std::memory_order_acquire)); // sadly not supported by the g++ compiler on our cycle machine
            delay = 1;
            while(lock.exchange(true, std::memory_order_acquire))
                while(lock)
                {
                    pause(delay);
                    delay <<= 1;
                }
                // while(lock.load(std::memory_order_acquire))
                // {
                //     pause(delay);
                //     delay <<= 1;
                // }

            if(counter < I)
            {
                ++my_count;
                ++counter;
            }
            // lock.clear(std::memory_order_release);
            lock.store(false, std::memory_order_release);
        }
    else
        while(my_count < I)
        {
            delay = 1;
            while(lock.exchange(true, std::memory_order_acquire))
                while(lock)
                {
                    pause(delay);
                    delay <<= 1;
                }
            ++my_count;
            ++counter;
            lock.store(false, std::memory_order_release);
        }
    return my_count;
}

int tatas()
{
    // static std::atomic_flag lock(false);
    static std::atomic<bool> lock;
    int my_count = 0;
    if(mode)
        while(counter < I)
        {
            // while(lock.test_and_set(std::memory_order_acquire))
            //     while(lock.test(std::memory_order_acquire));
            while(lock.exchange(true, std::memory_order_acquire))
                while(lock);
                // while(lock.load(std::memory_order_acquire));
            if(counter < I)
            {
                ++my_count;
                ++counter;
            }
            // lock.clear(std::memory_order_release);
            lock.store(false, std::memory_order_release);
        }
    else
        while(my_count < I)
        {
            while(lock.exchange(true, std::memory_order_acquire))
                while(lock);
            ++my_count;
            ++counter;
            // lock.clear(std::memory_order_release);
            lock.store(false, std::memory_order_release);
        }
    return my_count;
}

int tas()
{
    static std::atomic_flag lock(false);
    int my_count = 0;
    if(mode)
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
    else
        while(my_count < I)
        {
            while(lock.test_and_set(std::memory_order_acquire));
                ++counter;
                ++my_count;
            lock.clear(std::memory_order_release);
        }
    return my_count;
}

int fai()
{
    int my_count = 0;
    if(mode)
        while(atom_counter.fetch_add(1, std::memory_order_relaxed) < I-1)
        {
            ++my_count;
        }
    else
        while(my_count < I)
        {
            atom_counter.fetch_add(1, std::memory_order_relaxed);
            ++my_count;
        }
    return my_count;
}

int with_mutex()
{
    int my_count = 0;
    if(mode)
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
    else
        while(my_count < I)
        {
            pthread_mutex_lock(&lock);
            ++counter;
            ++my_count;
            pthread_mutex_unlock(&lock);
        }
    return my_count;
}

int no_sync ()
{
    int my_count = 0;
    if(mode)
        while(counter < I)
        {
            ++counter;
            ++my_count;
        }
    else
        while(my_count < I)
        {
            ++counter;
            ++my_count;
        }
    return my_count;
}

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
            if(same_socket)
                CPU_SET((threads_per_core * i) % MAX_CPU + (i/MAX_CPU), &cpuset);
            else
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

void output(std::string output_dir)
{
    using namespace std;
    stringstream s1;
    ofstream log_count_per_thread;
    ofstream log_time_per_round;
    if(!debug)
    {
        // if(mode)
        // {
            s1 << output_dir << "/thread_count_" << thread_num << ".txt";
            log_count_per_thread.open(s1.str());
            if(!log_count_per_thread.is_open())
            {
                fprintf(stderr, "output file thread_count_%d.txt fails to open\n", thread_num);
                exit(1);
            }
        // }

        s1.str("");
        s1 << output_dir <<"/time_round_" << thread_num << ".txt";
        log_time_per_round.open(s1.str());
        if(!log_time_per_round.is_open())
        {
            fprintf(stderr, "output file time_round_%d.txt fails to open\n", thread_num);
            exit(1);
        }
    }

    final[ROUND-2] = atom_counter; // fetch and increment
    int expected;
    if(mode)
        expected = I;
    else
        expected = I * thread_num;
    for(int r = 1; r < ROUND-2; ++r)
        if(final[r] != expected)
            printf("the counter of round %d is not correct!\n", r);

    for(int r = 0; r < ROUND; ++r)
    {
        // log round time
        double time_this_round = 0.0;
        for(int n = 0; n < repeat; ++n)
            time_this_round += elapsed[r][n];
        time_this_round /= repeat;

        if(debug == 1)
            cout << time_this_round << " " << final[r] << endl;
        else
            log_time_per_round << time_this_round << " " << final[r] << endl;

        // log thread count
        // if(mode)
        // {
            for(int k = 0; k < thread_num; ++k)
            {
                float thread_count = 0.0;
                for(int n = 0; n < repeat; ++n)
                    thread_count += count_per_thread[k][r][n];
                thread_count /= repeat;
                if(!debug)
                    log_count_per_thread << thread_count << endl;
            }
            if(!debug)
                log_count_per_thread << endl;
        // }
    }
    if(!debug)
    {
        log_time_per_round.close();
        // if(mode)
            log_count_per_thread.close();
    }
}

int main(int argc, char ** argv)
{
    // std::atomic<bool> test;
    // std::cout << test.is_lock_free() << std::endl;

    // default setting
    num_cpu = 4;
    thread_num = 4;
    I = 10000;
    repeat = 1;
    mode = 1;
    int rc;
    std::string output_dir = "./output";
    while((rc = getopt(argc, argv, "t:i:c:m:n:o:ds")) != -1)
    {
        switch(rc)
        {
            case 'o':
                output_dir.assign(optarg);
                break;
            case 'm':
                mode = atoi(optarg);
                if(mode != 0)
                    mode = 1;
                break;
            case 's':
                same_socket = 1;
                break;
            case 'd':
                debug = 1;
                break;
            case 't':
                thread_num = atoi(optarg);
                break;
            case 'i':
                I = atoi(optarg);
                break;
            case 'c':
                num_cpu = atoi(optarg);
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
    
    issue(exp1);
    join();

    output(output_dir);

    clean();
}