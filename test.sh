# !/usr/bin/bash
cpu_num=36
repeat=4
i=10000
output=./output_exp1.txt

for t in 1 2 4 6 8 12 16 32 48 64 96 128
    do
        ./exp1 -t ${t} -c ${cpu_num} -i ${i} -n ${repeat}
        echo "finish ${t}"
done