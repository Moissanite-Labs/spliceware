cc -O3 -Wall genesis.c -o genesis

# Terminal 1 (Node A listening on :9001, gossiping to :9002)
./genesis 9001 9002

# Terminal 2 (Node B listening on :9002, gossiping to :9001)
./genesis 9002 9001
