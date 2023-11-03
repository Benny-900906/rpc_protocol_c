CFLAGS = -Wall

all: client server

test: test_client test_server

test_client: client.a rpc.o
	gcc -o test_client client.a rpc.o

test_server: server.a rpc.o
	gcc -o test_server server.a rpc.o

client: client.o rpc.o
	gcc -o client client.o rpc.o

server: server.o rpc.o
	gcc -o server server.o rpc.o

# Other targets specify how to create .o files and what they rely on
client.o: client.c 
	gcc -c $(CFLAGS) client.c

server.o: server.c
	gcc -c $(CFLAGS) server.c

rpc.o: rpc.c rpc.h 
	gcc -c $(CFLAGS) rpc.c

clean:
	rm -f *.o client server test_client test_server