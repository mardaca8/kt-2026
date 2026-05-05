# 1_lab

## How to run

macos
```
cc -pthread -o server.o server.c
cc -pthread -o client.o client.c    
```

## Usage 

./server.o <port>
./client.o <ip> <port>

Commands: /nick <nickname>, /rooms, /create <room>, /join <room>, /who, /leave (leave the room), /quit (quit the program)