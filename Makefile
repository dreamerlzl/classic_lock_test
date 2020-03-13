PLATFORM = $(shell uname -p)
FLAGS1 = -pthread -w -g -pg -std=c++11
FLAGS2 = -pthread -w -g -pg
CPP = g++
CC = gcc
OUTPUT1 = exp1
OUTPUT2 = exp2

ifeq ($(PLATFORM), x86_64)
	dependency1 = exp1.cpp hrtimer_x86.c
	dependency2 = exp2.c hrtimer_x86.c
else
	dependency1 = exp1.cpp
	dependency2 = exp2.c
endif

all: exp1 exp2

exp1: $(dependency1)
	$(CPP) $(dependency1) $(FLAGS1) -o $(OUTPUT1)

exp2: $(dependency2)
	$(CC) $(dependency2) $(FLAGS2) -o $(OUTPUT2)

.exp1:
	$(CPP) $(dependency1) $(FLAGS1) -o $(OUTPUT1)

.exp2:
	$(CC) $(dependency2) $(FLAGS2) -o $(OUTPUT2)

clean:
	rm $(OUTPUT1) $(OUTPUT2)