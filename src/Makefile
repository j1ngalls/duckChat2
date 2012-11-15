CC=g++
CFLAGS=-Wall -W 
UNAME=$(shell uname)
PROJECTDIR=..

# these depend on if we are running on developement machines or on ix
ifeq ($(UNAME), Linux) # Local to developement machines
LOADLIBES= -lnsl -lresolv
endif
ifeq ($(UNAME), SunOS) # on IX
LOADLIBES= -lnsl -lsocket -lresolv
endif

all: client server

debug: CFLAGS += -g -DDEBUG
debug: all

keep_alive: CFLAGS += -DGRAD
keep_alive: all

client: client.cpp raw.c
	$(CC) client.cpp raw.c $(LOADLIBES) $(CFLAGS) -o $(PROJECTDIR)/bin/client

server: server.cpp 
	$(CC) server.cpp $(LOADLIBES) $(CFLAGS) -o $(PROJECTDIR)/bin/server

clean:
	rm -f *.o
	rm $(PROJECTDIR)/bin/client $(PROJECTDIR)/bin/server

