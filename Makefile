# Makefile

CC  = gcc
CFLAGS   = -g -Wall $(INCLUDES)

.PHONY: default
default: http-server 

# header dependency
http-server.o: 

.PHONY: vg
vg:
	valgrind --leak-check=yes ./http-server 11736 ~/html localhost 12736 

.PHONY: clean
clean:
	rm -f *.o *~ a.out core http-server

.PHONY: all
all: clean default
