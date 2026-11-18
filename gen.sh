gcc -O3 -Wall -Wextra -std=c11 genesis.c -o genesis
./genesis 9001 9002 &
./genesis 9002 9001
