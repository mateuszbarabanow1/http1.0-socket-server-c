#!/bin/bash
gcc -Wall -Wextra -pthread -o server server.c
if [ $? -eq 0 ]; then
    ./server
fi