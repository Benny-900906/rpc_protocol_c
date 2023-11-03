#define _POSIX_C_SOURCE 200112L
#define _OEPN_SYS_ITOA_EXT
#define NONBLOCKING
#include "rpc.h"
#include <assert.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
 
#define REQUEST_SENT 1
#define REQUEST_FAILED 0
#define RESPONSE_SENT 1
#define RESPONSE_FAILED 0
#define SUCCESS 1
#define FAILURE -1
#define TRUE 1
#define FALSE 0
#define NAME_FOUND 1
#define NAME_NOT_FOUND 0
#define MAX_NAME_LEN 1000
#define MIN_NAME_LEN 1
#define MAX_DATA2_LEN 100000
#define PORT_LEN 5
#define MIN_CLIENTS_TO_HANDLE 10
#define CALL_REQ 'c'
#define FIND_REQ 'f'
#define ERROR_MSG "ERROR: PROCEDURE RETURNED NULL"

// *******************************************
// ****************** START ******************
// ********** STRUCTS DEFINITIONS ************
// ****************** START ******************
// *******************************************
struct rpc_handle {
	char name[MAX_NAME_LEN + 1];
	rpc_handler rpc_handler;
	rpc_handle *next;
};

struct rpc_client {
	int sockfd;
	int is_connected;
};

struct rpc_server {
	int sockfd;
	int newsockfd;
	rpc_handle *head;
	struct addrinfo *res;
};
// *******************************************
// ******************* END *******************
// ********** STRUCTS DEFINITIONS ************
// ******************* END *******************
// *******************************************


// *******************************************
// ****************** START ******************
// ******* HELPER FUNCTION SIGNATURES ********
// ****************** START ******************
// *******************************************
int static send_response_to_cl(rpc_server *srv, rpc_data *response);
rpc_data static *receive_request_from_cl(rpc_server *srv);
int static send_request_to_srv(rpc_client *cl, rpc_data *request);
rpc_data static *receive_response_from_srv(rpc_client *cl);
char static *strdup(const char *str);
void static *serve_client(void *server);
u_int64_t static htonll(u_int64_t value);
u_int64_t static ntohll(u_int64_t value);
// *******************************************
// ******************* END *******************
// ******* HELPER FUNCTION SIGNATURES ********
// ******************* END *******************
// *******************************************


// The static function sends the response to the connected client in a stream of bytes, and returns either RESPONSE_SEND or RESPONSE_FAILED to 
// indicate whether the transmission was succeeded or not.  
int static send_response_to_cl(rpc_server *srv, rpc_data *response) {
	int n;

	// the len of data2 cannot be greater than 100000, otherwise the function will return an Overlength Error
	if (response->data2_len > MAX_DATA2_LEN) {
		perror("Overlength error");
		exit(EXIT_FAILURE);
	}

	// Converting the response (data1, data2_len, data2) into network order bytes before transimitting. 
	// Note that data1 is being casted into type u_int64_t from type int before transmitting. This is because
	// we are limiting data1 to be no more than 64 bits.
	u_int64_t net_data1 = htonll((u_int64_t) response->data1);
	// On the other hand, we are handling data2_len using u_int32_t acorss system architectures. This is because the spec/requirement
	// states that data2_len should not be greater than 100000. So we do not need to allow it up to 64 bits. 
	u_int32_t net_data2_len = htonl((u_int32_t) response->data2_len);
	// initially the buff_size only contains the sizes/bytes of net_data1 and net_data2_len. This is because we do not know whether data2 is non-NULL
	// or NULL yet. The response might be intended for rpc_find(), where data2 is NULL and only data1 has a value (either NAME_FOUND or NAME_NOT_FOUND) to indicate
	// whether the target function/handler is found in the server. The response might be intended for rpc_call() where some procedures will produce
	// either NULL or non-NULL data2; To deal with all these cases, it is better to check where the data2 is NULL or not before allocating the size of data2 into 
	// buff_size;
	int buff_size = sizeof(net_data1) + sizeof(net_data2_len);
	char net_data2[response->data2_len + 1];

	if (response->data2_len > 0) {
		sprintf(net_data2, "%s", (char *) response->data2);
		net_data2[response->data2_len] = '\0';
		buff_size += response->data2_len + 1;
	}

	char buff[buff_size];
	char *p = buff;

	// allocating bytes into the buffer
	memcpy(p, &net_data1, sizeof(net_data1));
	p += sizeof(net_data1);

	memcpy(p, &net_data2_len, sizeof(net_data2_len));
	// new

	if (response->data2_len > 0) {
		p += sizeof(net_data2_len);
		memcpy(p, net_data2, response->data2_len + 1);
	}

	n = write(srv->newsockfd, buff, sizeof(buff));

	// checks whether the transmission failed or not
	return (n == -1) ? RESPONSE_FAILED : RESPONSE_SENT;	
}

// the static function receives a request sent from the client side and dynamically creates a rpc_data* to store all the information. 
// the rpc_data* will be returned to the server side to be processed eventually. 
rpc_data static *receive_request_from_cl(rpc_server *srv) {
	u_int64_t net_data1;
	u_int32_t net_data2_len;
	
	int n = 0;
	int offset = 0;
	int bytes_read = 0;
	// we do not have the knowledge of the size of data2 yet, so we do not include the size of it inside bytes_expected for now.
	int bytes_expected = sizeof(net_data1) + sizeof(net_data2_len);
	rpc_data *cl_request = (rpc_data *)malloc(sizeof(rpc_data));
	assert(cl_request);

	char *buff = (char *)malloc(sizeof(bytes_expected));
	assert(buff);

	// loop read()'s to receive bytes sent from the client side, since a single read() does not guarantee the entire message/bytes to be received. 
	while (bytes_read < bytes_expected) {
		n = read(srv->newsockfd, buff + bytes_read, bytes_expected - bytes_read);
		if (n == -1) {
			return NULL;
		}
		bytes_read += n;
	}

	// cast the data back to its originally intended data type once received.
	memcpy(&net_data1, buff + offset, sizeof(net_data1));
	cl_request->data1 = (int)ntohll(net_data1);
	offset += sizeof(net_data1);

	memcpy(&net_data2_len, buff + offset, sizeof(net_data2_len));
	cl_request->data2_len = (size_t) ntohl(net_data2_len);

	free(buff);
	
	cl_request->data2 = (char *)malloc(cl_request->data2_len);
	assert(cl_request->data2);
	
	bytes_read = 0;
	bytes_expected = cl_request->data2_len + 1;

	// now we have knowledge of the size of data2, we can start receiving it using read()'s
	while (bytes_read < bytes_expected) {
		int result = read(srv->newsockfd, cl_request->data2 + bytes_read, bytes_expected - bytes_read);
		if (result == -1) {
			return NULL;
		}
		bytes_read += result;
	}

	return cl_request;
}

// the static function sends the request as a packet to the server side and returns either REQUEST_SENT or REQUEST_FAILED indicating 
// whether the transmission was succeeded or not
int static send_request_to_srv(rpc_client *cl, rpc_data *request) {
	int n;

	if (request->data2_len > MAX_DATA2_LEN) {
		perror("Overlength error");
		exit(EXIT_FAILURE);
	}

	u_int64_t net_data1 = htonll((u_int64_t) request->data1);
	u_int32_t net_data2_len = htonl((u_int32_t) request->data2_len);
	char net_data2[request->data2_len + 1];
	sprintf(net_data2, "%s", (char *) request->data2);
	net_data2[request->data2_len] = '\0';

	int buff_size = sizeof(net_data1) + sizeof(net_data2_len) + request->data2_len + 1;
	char buff[buff_size];
	char *p = buff;

	memcpy(p, &net_data1, sizeof(net_data1));
	p += sizeof(net_data1);

	memcpy(p, &net_data2_len, sizeof(net_data2_len));
 	p += sizeof(net_data2_len);

 	memcpy(p, net_data2, request->data2_len + 1);

	n = write(cl->sockfd, buff, sizeof(buff));

	return (n == -1) ? REQUEST_FAILED : REQUEST_SENT;
}

// the static function receives a resposne sent from the server side and constructs a rpc_data* (packet) dynamically to store all the 
// received informaiton. This could be a response for rpc_call() or rpc_find() invoked by the client side. The rpc_data* will be passed back to the 
// client side eventually. 
rpc_data static *receive_response_from_srv(rpc_client *cl) {
	int n = 0;
	int offset = 0;
	u_int64_t net_data1;
	u_int32_t net_data2_len;
	int bytes_read = 0;
	int bytes_expected = sizeof(net_data1) + sizeof(net_data2_len);
	rpc_data *response = (rpc_data *)malloc(sizeof(rpc_data));
	assert(response);

	char *buff = (char *)malloc(sizeof(bytes_expected));
	assert(buff);

	while (bytes_read < bytes_expected) {
		n = read(cl->sockfd, buff + bytes_read, bytes_expected - bytes_read);
		if (n == -1) {
			return NULL;
		}
		bytes_read += n;
	}

	memcpy(&net_data1, buff + offset, sizeof(net_data1));
	response->data1 = (int) ntohll(net_data1);
	offset += sizeof(net_data1);

	memcpy(&net_data2_len, buff + offset, sizeof(net_data2_len));
	response->data2_len = (size_t) ntohl(net_data2_len);

	free(buff);

// new
	if ((int) response->data2_len == 0) {
		response->data2 = NULL;
	} else {
		response->data2 = (char *)malloc(response->data2_len);
		assert(response->data2);
		
		bytes_read = 0;
		bytes_expected = response->data2_len + 1;

		while (bytes_read < bytes_expected) {
			int result = read(cl->sockfd, response->data2 + bytes_read, bytes_expected - bytes_read);
			if (result == -1) {
				return NULL;
			}
			bytes_read += result;
		}
	}

	return response;
}

// the static function was intended to be an explicit implementation for strdup() in the header file <string.h>
// this is because the strdup() function was not working properly in both the VM and my local computer. 
char static *strdup(const char *str) {
    char* dup = NULL;
    if (str) {
        size_t len = strlen(str);
        dup = (char*) malloc(len + 1);
        if (dup) {
            memcpy(dup, str, len);
            dup[len] = '\0';
        }
    }
    return dup;
}

// the static function is utilized on the server side to receive requests, invoking procedures, and 
// sending desired responses/results back to a single client. This is a helper function that will be used in pthread_create().
// This will help to create a multithreading server to serve multiple clients concurrently. 
void static *serve_client(void *server) {

	rpc_server *srv = (rpc_server *)server;

	while (TRUE) {

		rpc_data *cl_request = receive_request_from_cl(srv);
		assert(cl_request != NULL);

		char *request_str = (char *) cl_request->data2;	
		// extract requested operation: rpc_find() 'f' or rpc_call() 'c'
		char request_op = request_str[0];

		// find operation request msg format: "f: func_name"
		if (request_op == FIND_REQ) {
			char *colon_space = strstr(request_str, ": ");
			int len = strlen(colon_space + 2);
			char func_name[len + 1];
			strncpy(func_name, colon_space + 2, len);
			func_name[len] = '\0';

			// extracted function name, now traverse handles in srv to see if the function/procedure exists
			int found_func = FALSE;
			rpc_handle *curr = srv->head;
			while (curr != NULL) {
				if (strcmp(curr->name, func_name) == 0) {
					found_func = TRUE;
					break;
				}
				curr = curr->next;
			}

			rpc_data *response = (rpc_data *)malloc(sizeof(rpc_data));
			assert(response);

			if (found_func) {
				response->data1 = NAME_FOUND;
			} else {
				response->data1 = NAME_NOT_FOUND;
			}

			response->data2 = NULL;
			response->data2_len = 0;

			send_response_to_cl(srv, response);
			rpc_data_free(response);

		// call operation request msg format: "c: func_name, argument"
		} else if(request_op == CALL_REQ) {

			// extract function/procedure name
			char *start = strstr(request_str, ": ");
			start += 2;
			char *end = strchr(start, ',');
			int len = end - start;
			char func_name[len + 1];
			strncpy(func_name, start, len);
			func_name[len] = '\0';

			// extract data2 to be sent to the function/procedure
			char *comma_space = strstr(request_str, ", ");
			len = strlen(comma_space + 2);
			char data2[len + 1];
			strncpy(data2, comma_space + 2, len);
			data2[len] = '\0';
			
			// construct rpc_data* (packet) to be sent to the function/procedure
			rpc_data *data = (rpc_data *)malloc(sizeof(rpc_data));
			assert(data);
			data->data1 = cl_request->data1;
			data->data2 = strdup(data2);
			data->data2_len = strlen(data2);

			// retrieve function/procedure from srv
			rpc_data *result = NULL;
			rpc_handle *curr = srv->head;
			while (curr != NULL) {
				if (strcmp(curr->name, func_name) == 0) {
					result = curr->rpc_handler(data);
					break;
				}
				curr = curr->next;
			}

			rpc_data_free(data);

			// safety check: if the response is NULL or the response's data2_len/data2 are inconsistent send an error response
			// back to the client.
			if ( result == NULL 
					 || ((int) result->data2_len == 0 && result->data2 != NULL) 
					 || ((int) result->data2_len != 0 && result->data2 == NULL) ) {

				rpc_data *error_res = (rpc_data *)malloc(sizeof(rpc_data));
				assert(error_res);

				error_res->data1 = FAILURE;
				error_res->data2 = strdup(ERROR_MSG);
				error_res->data2_len = strlen(error_res->data2);

				send_response_to_cl(srv, error_res);
				rpc_data_free(error_res);

				if (result != NULL) {

					rpc_data_free(result);
					
				}

			} else {

				send_response_to_cl(srv, result);
				rpc_data_free(result);

			}
		}

		rpc_data_free(cl_request);

	}
	return NULL;
}

// the static functions htonll() and ntohll() are the explicit implmentations for the non-standard htonll() and ntohll() functions in C
// they are used to convert u_int64_t's into network order bytes. ntohll() on the other hand is used for reverting the network order bytes
// inspired by: https://stackoverflow.com/questions/25708182/about-the-source-code-of-htonl
u_int64_t static htonll(u_int64_t value) {
  // check the native endianness
  const int num = 42;
  if (*(const char*)&num == 42) {
    // Little-endian system
    return ((value & 0xFF00000000000000) >> 56) |
           ((value & 0x00FF000000000000) >> 40) |
           ((value & 0x0000FF0000000000) >> 24) |
           ((value & 0x000000FF00000000) >> 8) |
           ((value & 0x00000000FF000000) << 8) |
           ((value & 0x0000000000FF0000) << 24) |
           ((value & 0x000000000000FF00) << 40) |
           ((value & 0x00000000000000FF) << 56);
  } else {
  
    return value;
  }
}

u_int64_t static ntohll(u_int64_t value) {
  // check the native endianness
  const int num = 42;
  if (*(const char*)&num == 42) {
  	
    return ((value & 0xFF00000000000000) >> 56) |
           ((value & 0x00FF000000000000) >> 40) |
           ((value & 0x0000FF0000000000) >> 24) |
           ((value & 0x000000FF00000000) >> 8) |
           ((value & 0x00000000FF000000) << 8) |
           ((value & 0x0000000000FF0000) << 24) |
           ((value & 0x000000000000FF00) << 40) |
           ((value & 0x00000000000000FF) << 56);
  } else {
    
    return value;
  }
}

// *******************************************
// ****************** START ******************
// ******** RPC API IMPLEMENTATIONS **********
// ****************** START ******************
// *******************************************
rpc_server *rpc_init_server(int port) {
	int enable, s, sockfd;
	struct addrinfo hints, *res;
	char service[sizeof(int) * PORT_LEN + 1];

	memset(&hints, 0, sizeof(hints));

	// IPv6
	hints.ai_family = AF_INET6;
	// socket type: communicates using byte streams (TCP)
	hints.ai_socktype = SOCK_STREAM; 
	// passive: for bind(), listen(), and accept()
	hints.ai_flags = AI_PASSIVE;     

	// convert int port into a string, as getaddrinfo() only takes the port number as a string
	sprintf(service, "%d", port);
	service[strlen(service)] = '\0';

	// used to obtain the adddress and port nuumber for binding a socket and listening for incoming connections. 
	s = getaddrinfo(NULL, service, &hints, &res);

	if (s != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
		exit(EXIT_FAILURE);
	}

	// creates a new communication endpoint that allows the server to write() and read() data/streams of bytes over a network.
	sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

	if (sockfd < 0) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	// reuse port if possible
	enable = 1;

	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
		perror("setsocket");
		exit(EXIT_FAILURE);
	}

	rpc_server *server = (rpc_server *)malloc(sizeof(rpc_server));
	assert(server);

	server->sockfd = sockfd;
	server->res = res;
	server->head = NULL;

	return server;
}

int rpc_register(rpc_server *srv, char *name, rpc_handler handler) {
	// the is_in_list flag checks whether the function/procedure name is already in the handles list. 
	// if it is in the list, replace it. if it is not in the list create a new handle dynamically and prepend it. 
	int is_in_list = FALSE;
	int name_len;
	int i = 0;

	if (srv == NULL || name == NULL || handler == NULL) {
		return FAILURE;
	}
	
	name_len = strlen(name);
	// the name of the function/procedure to be registered should have a length between 1 ~ 1000 characters
	if (name_len < MIN_NAME_LEN || name_len > MAX_NAME_LEN) {
		return FAILURE;
	}

	// the name of the function can only be ASCII characters between 32 to 126
	for (i = 0; i < name_len; i ++) {
		if (name[i] < 32 || name[i] > 126) {
			return FAILURE;
		}
	}

	rpc_handle *handle = (rpc_handle *)malloc(sizeof(rpc_handle));
	assert(handle != NULL);
	strcpy(handle->name, name);
	handle->rpc_handler = handler;
	handle->next = NULL;

	// the handles list in the srv is empty, assign handle to the head
	if (srv->head == NULL) {

		srv->head = handle;

	} else {
		// look for the same name and replce it in the list
		rpc_handle *curr = srv->head;
		rpc_handle *prev = NULL;
		while (curr != NULL) {
			// found target handle name, replace it
			if (strcmp(curr->name, name) == 0) {
				free(curr->name);
				curr->rpc_handler = NULL;
				// the handle is located at the head
				if (prev == NULL) {
					handle->next = curr->next;
					srv->head = handle;
				} else {
					handle->next = curr->next;
					prev->next = handle;
				}
				free(curr);
				curr = NULL;
				is_in_list = TRUE;
				break;
			}
			prev = curr;
			curr = curr->next;
		}

		// did not find target handle name, prepend it to the list
		if (!is_in_list) {
			handle->next = srv->head;
			srv->head = handle;
		}
	}

	return SUCCESS;
}

void rpc_serve_all(rpc_server *srv) {
	struct sockaddr_in client_addr;
	socklen_t client_addr_size;
	int newsockfd;

	if (srv == NULL) {
		return;
	}

	// only call bind() and listen() until now. 
	// switch1: the server cannot be switched if the server has invoked 
	// bind() and listen() before rpc_serve_all()
	if (bind(srv->sockfd, srv->res->ai_addr, srv->res->ai_addrlen) < 0) {
		perror("bind");
		exit(EXIT_FAILURE);
	}

	freeaddrinfo(srv->res);

	if (listen(srv->sockfd, MIN_CLIENTS_TO_HANDLE) < 0) {
		perror("listen");
		exit(EXIT_FAILURE);
	}

	while (TRUE) {
		client_addr_size = sizeof(client_addr);

		// accepting a client connection request
		newsockfd = accept(srv->sockfd, (struct sockaddr *)&client_addr, &client_addr_size);

		if (newsockfd < 0) {
			perror("accept");
			exit(EXIT_FAILURE);
		}

		// creates a new rpc_server to store the sockfd of the current server
		// but the newsockfd will be used to store different incoming clients
		// we will pass this srv_instance (same sockfd, different newsockfd)
		// to deal with different client's requests concurrently in different
		// worker threads
		rpc_server *srv_instance = (rpc_server *)malloc(sizeof(rpc_server));
		assert(srv_instance);

		srv_instance->sockfd = srv->sockfd;
		srv_instance->head = srv->head;
		srv_instance->res = srv->res;

		srv_instance->newsockfd = newsockfd;

		// creates a worker thread to resolve the client's requests
		pthread_t worker_thread;

		// the worker thread will only take care of (same sockfd, different newsockfd) pair using the serve_client() function
		pthread_create(&worker_thread, NULL, serve_client, (void *) srv_instance);
	}
}

rpc_client *rpc_init_client(char *addr, int port) {
	struct addrinfo hints, *servinfo, *rp;
	int sockfd, s;
	char service[sizeof(int) * PORT_LEN + 1];

	if (addr == NULL || port < 0) {
		fprintf(stderr, "invalid client_ip or host_port\n");
		exit(EXIT_FAILURE);
	}

	// set up hints for getaddrinfo
	memset(&hints, 0, sizeof(hints));

	// accepts IPv6
	hints.ai_family = AF_INET6;
	// TCP socket
	hints.ai_socktype = SOCK_STREAM;

	// converting the port number into a string
	sprintf(service, "%d", port);
	service[strlen(service)] = '\0';

	s = getaddrinfo(addr, service, &hints, &servinfo);

	if (s != 0) {
		fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(s));
		exit(EXIT_FAILURE);
	}

	// looping over a linked list of struct addrinfo pointers to retrieve 
	// address interface family, address interface socket type, and address interface protocol for setting up network connections
	for (rp = servinfo; rp != NULL; rp = rp->ai_next) {
		sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sockfd == -1) {
			continue;
		}

		// connection established
		if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) != -1) {
			break; 
		}

		// close the socket that we were trying to create
		close(sockfd);
	}

	if (rp == NULL) {
		fprintf(stderr, "client: failed to connect\n");
		exit(EXIT_FAILURE);
	}

	// clean up and return the RPC client object
	freeaddrinfo(servinfo);

	rpc_client *client = (rpc_client *)malloc(sizeof(rpc_client));
	assert(client != NULL);

	// stores the socket fd to communicate with the connected server
	client->sockfd = sockfd;
	client->is_connected = TRUE;

	return client;
}

// rpc_find returns a rpc_handle* that initializes both "head" and "handler" to be NULL and only contains non-NULL char[MAX_NAME_LEN + 1] so that it is a flat struct and can be free'ed using free(rpc_handle) by the client
rpc_handle *rpc_find(rpc_client *cl, char *name) {
	int name_len, i;
	
	if (cl == NULL || name == NULL) {
		return NULL;
	}

	name_len = strlen(name);
	// the name of the function to be registered should have a length between 1 ~ 1000 characters
	if (name_len < MIN_NAME_LEN || name_len > MAX_NAME_LEN) {
		return NULL;
	}

	// the name of the function can only be ASCII characters between 32 to 126
	for (i = 0; i < name_len; i ++) {
		if (name[i] < 32 || name[i] > 126) {
			return NULL;
		}
	}

	rpc_data *request = (rpc_data *)malloc(sizeof(rpc_data));
	assert(request != NULL);

	// request format: "f: func_name"
	int data2_size = strlen(name) + 3 + 1;
	char data2[data2_size];
	sprintf(data2, "%c: %s", FIND_REQ, name);
	data2[data2_size - 1] = '\0';

	request->data1 = -1;
	request->data2_len = strlen(data2);
	request->data2 = strdup(data2);

	if (send_request_to_srv(cl, request) == REQUEST_FAILED) {
		rpc_data_free(request);
		return NULL;
	}

	rpc_data_free(request);

	rpc_data *response = receive_response_from_srv(cl);

	// found target function/procedure in server, name is registered
	if (response->data1 == NAME_FOUND) {
		rpc_handle *handle = (rpc_handle *)malloc(sizeof(rpc_handle));
		assert(handle);
		
		strcpy(handle->name, name);
		handle->next = NULL;
		handle->rpc_handler = NULL;

		rpc_data_free(response);

		return handle;
	} else {
		
		rpc_data_free(response);

		return NULL;
	}
}

rpc_data *rpc_call(rpc_client *cl, rpc_handle *h, rpc_data *payload) {
	
	if (cl == NULL || h == NULL || payload == NULL) {
		return NULL;
	}

	// return NULL if encounters data2_len/data2 inconsistency
	if ((int) payload->data2_len == 0 && payload->data2 != NULL) {
		return NULL;
	} 

	if ((int) payload->data2_len > 0 && payload->data2 == NULL) {
		return NULL;
	}

	// return NULL if the lenght of the function/procedure name is
	// greater than 1000, as we will not be tested on handle name length > 1000
	if (strlen(h->name) > MAX_NAME_LEN) {
		return NULL;
	}

	rpc_data *request = (rpc_data *)malloc(sizeof(rpc_data));
	assert(request);

	// new data2 format: "c: function_name, data2"
	// so the total length is 3 + strlen(h->name) + 2 + payload->data2_len + 1;
	int data2_size = 3 + strlen(h->name) + 2 + payload->data2_len + 1;
	char data2[data2_size];
	sprintf(data2, "%c: %s, %s", CALL_REQ, h->name, (char *) payload->data2);
	data2[data2_size - 1] = '\0';

	request->data1 = payload->data1;
	request->data2_len = strlen(data2);
	request->data2 = strdup(data2);

	if (send_request_to_srv(cl, request) == REQUEST_FAILED) {
		rpc_data_free(request);
		return NULL;
	}

	rpc_data_free(request);
	
	rpc_data *response = receive_response_from_srv(cl);

	// the procedure produced NULL or Inconsistent data2_len/data2.
	// sending an error response back to trigger NULL return
	if (response->data2 != NULL && strcmp((char *) response->data2, ERROR_MSG) == 0) {
		
		rpc_data_free(response);
		
		return NULL;
	}

	return response;
}

// called after the final rpc_find or rpc_call
void rpc_close_client(rpc_client *cl) {
	if (cl == NULL || cl->is_connected == FALSE) {
		return;
	}

	close(cl->sockfd);
	cl->is_connected = FALSE;
	free(cl);
}

void rpc_data_free(rpc_data *data) {
	if (data == NULL) {
		return;
	}
	if (data->data2 != NULL) {
		free(data->data2);
	}
	free(data);
}
// *******************************************
// ******************* END *******************
// ******** RPC API IMPLEMENTATIONS **********
// ******************* END *******************
// *******************************************