import getopt, sys
import numpy as np
import matplotlib.pyplot as plt
from math import log
from typing import List

thread_setting = [1, 2, 4, 6, 8, 12, 16, 32, 48, 64, 96, 128]
exp1_method = ['no sync', 'mutex', 'tas', 'tatas', 'tatas with backoff', 'ticket', 'mcs', 'fai', 'local']

def exp1(platforms:List, rows=2, cols=2):
    # for exp1
    plt.title('Experiment 1')
    for i, platform in enumerate(platforms):
        plt.subplot(rows, cols, i+1)
        exp1_time = [[] for i in range(len(exp1_method))]

        for t in thread_setting:
            with open(f'{platform}/time_round_{t}.txt', 'r') as f:
                for i, line in enumerate(f):
                    exp1_time[i].append(log(float(line.split(' ')[0])))

        plt.title(f"{platform}")

        color = ['purple', 'black', 'blue', 'blue', 'blue', 'aqua', 'green', 'red', 'magenta']
        marker = ['x', 'x', 'x', 'v', 'o', 'x', 'x', 'x', 'x']

        scale = [i for i in range(len(thread_setting))]
        plt.xticks(scale, thread_setting)
        for i, record in enumerate(exp1_time):
            plt.plot(scale, record, label=exp1_method[i], marker=marker[i], color=color[i])

        plt.legend(loc=2)
        plt.xlabel('number of threads')
        plt.ylabel('log of time(s)')
    plt.show()

def exp2():
    exp2_method = ['tas', 'hle']
    for i, x in enumerate(['same', 'across']):
        exp2_time = [ [] for i in range(2)]
        plt.subplot(1, 2, i+1)
        with open(f'output_hle_{x}.txt', 'r') as f:
            for line in f:
                t1, t2 = line.strip().split(' ')
                exp2_time[0].append(float(t1))
                exp2_time[1].append(float(t2))
    
        plt.title(x)
        scale = [i for i in range(len(thread_setting))]
        plt.xticks(scale, thread_setting)
        color = ['red', 'blue']
        marker = ['x', 'o']
        for i, record in enumerate(exp2_time):
            plt.plot(scale, record, label=exp2_method[i], marker=marker[i], color=color[i])

        plt.legend(loc=2)
        plt.xlabel('number of threads')
        plt.ylabel('time(s)')
    plt.show()

if __name__ == '__main__':
    platforms = []

    # default setting
    rows = 1
    cols = 0

    try:
        opts, args = getopt.getopt(sys.argv[1:], "hm:p:r:c:")
    except getopt.GetoptError:
        print ('plot.py -p <platforms> -r <rows> -c <cols> -m <mode>')
        sys.exit(2)

    for opt, arg in opts:
        if opt == '-h':
            print('plot.py -m <mode>')
            sys.exit()
        elif opt in ("-m", "--mode"):
            mode = int(arg)
            if mode == 0:
                thread_setting = thread_setting[:len(thread_setting)-3]
            elif mode not in {1, 2}:
                print('the mode must be one of {0,1,2}')
                sys.exit()
        elif opt in ('-p', '--platform'):
            platforms = arg.split(',')
            if cols == 0:
                cols = len(platforms)
        elif opt in ('-r', '--rows'):
            rows=int(arg)
        elif opt in ('-c', '--cols'):
            cols = int(arg)
    
    if not platforms:
        print('must specify the platform!')
        sys.exit()

    if mode < 3:
        exp1(platforms, rows=rows, cols=cols)
    else:
        exp2()