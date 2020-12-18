#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <err.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>
using namespace std;

struct httpObject {
	char buffer[4096];
	char request[100];
	char slash_filename[100];
	char* filename;
	char content_length[4096];
	int status_code;
	int body_size;
	char* body_data;
	int valid;
};

struct CircularBuffer {
	int* client_q;
	int q_size, max_size;
	int front, rear;
	pthread_mutex_t buffer_mutex;
	pthread_cond_t cond;
};

struct FileLock {
	pthread_mutex_t lock;
	pthread_cond_t cond;
};

struct ThreadArg {
	struct CircularBuffer *pcb;
	struct FileLock *gl;
	int rd;
	unordered_map <string, FileLock> file_map;
};

// Store a new lock for the new file in our vector mapping
int storeLock(const char* file_name, unordered_map <string, FileLock> lock_map) {
	// check again if the lock has been made by a different worker thread
	if (lock_map.find(file_name) == lock_map.end()) {
		struct FileLock fl;
		pthread_mutex_init(&fl.lock,NULL);
		lock_map[file_name] = fl;
		return 1; // dont unlock till we process the request
	} else {
		return 0; // unlock it, its already in there - we already made the lock in another thread
	}
}

// Send a response to the client based on the given status code + content length
void sendResponse(int client_sock_fd, struct httpObject *header) {
	if (header->status_code == 200) {
		dprintf(client_sock_fd, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", atoi(header->content_length));
	}
	if (header->status_code == 201) {
		dprintf(client_sock_fd, "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
	}
	if (header->status_code == 400) {
		dprintf(client_sock_fd, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
	}
	if (header->status_code == 403) {
		dprintf(client_sock_fd, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
	}
	if (header->status_code == 404) {
		dprintf(client_sock_fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
	}
	if (header->status_code == 500) {
		dprintf(client_sock_fd, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n");
	}
	close(client_sock_fd);
	return;
}

// Receive the HTTP header from the request
void recv_http_header(int client_sock_fd, struct httpObject* header) {
	int recv_header = recv(client_sock_fd, header->buffer, 4096, 0);
	if (recv_header == -1) {
		perror("recv() on header");
		exit(1);
	}
	char* found_body = strstr(header->buffer, "\r\n\r\n");
	char* extracted;
	// Check if we somehow received part of the HTTP body
	if (found_body != NULL) {
		int body_size = strlen(found_body);
		header->body_size = body_size;
		if (body_size != 4) {
			header->body_data = found_body; 
		}
	}
	sscanf(header->buffer, "%s %s", header->request, header->slash_filename);
	header->filename = header->slash_filename + 1;
	char *cLength = strstr(header->buffer, "Content-Length: ");
	if (cLength == NULL) {
	} else {
		sscanf(cLength, "Content-Length: %s\r\n", header->content_length);
	}
}

// Check if we are dealing with a valid request
void validate_req(int client_sock_fd, struct httpObject* header) {
	// check if valid file name length - 400
	if (strlen(header->filename) != 10) {
		header->status_code = 400;
		sendResponse(client_sock_fd, header);
		return;
	}
	// check if valid file name  - 400
	for (char * i = header->filename; *i; i++) {
		if (!isalnum(*i)) {
			header->status_code = 400;
			sendResponse(client_sock_fd, header);
			return;
		}
	}
	// check if PUT/GET request - 400
	if ((strcmp(header->request, "GET") != 0) && (strcmp(header->request, "PUT") != 0)) {
		header->status_code = 400;
		sendResponse(client_sock_fd, header);
		return;
	}
	// we are a valid request
	header->valid = 1;
	return;
}

// Respond to the valid request
void complete_req (int client_sock_fd, struct httpObject* header, int redundancy) {
	if (strcmp(header->request, "PUT") == 0) {
		int put_fd;
		struct stat buf;
		int file_exist = stat(header->filename, &buf);
		// open the file descriptor
		put_fd = open(header->filename, O_CREAT | O_RDWR | O_TRUNC, 0644);
		if (put_fd == -1) {
			// check if permissions to open file - 403
			header->status_code = 403;
			sendResponse(client_sock_fd, header);
			close(put_fd);
			return;
		}
		char put_buffer[4096];
		// if empty file or content length "0" - 201 or 200
		if (atoi(header->content_length) == 0) {
			write(put_fd, put_buffer, 0);
			if (file_exist == -1){
				header->status_code = 201;
			} else {
				header->status_code = 200;
			}
			sendResponse(client_sock_fd, header);
			close(put_fd);
			return;
		}

		int bytes_written = 0;
		int bytes_recv;
		int cLength = atoi(header->content_length);
		// if we received the body in our HTTP header, write it to the socket
		if (header->body_data != NULL) {
			if (header->body_size != 4) {
				cLength -= header->body_size-4;
				char* need = (header->body_data + 4*sizeof(char));
				write(put_fd, need, strlen(need));
			}
		}
		// receive the body data and write it to the socket
		while (cLength > 0) {
			bytes_recv = recv(client_sock_fd, put_buffer, 4096, 0);
			if (bytes_recv == -1) {
				// ran into errors receiving the data - 500
				header->status_code = 500;
				sendResponse(client_sock_fd, header);
				close(put_fd);
				return;
			}
			write(put_fd, put_buffer, bytes_recv);
			cLength -= bytes_recv;
			if (cLength == 0){
				break;
			}
			bytes_recv = 0;
		}
		// all is good if we reached this point - 201 or 200, newly created or OK?
		if (file_exist == -1){
			header->status_code = 201;
		} else {
			header->status_code = 200;
		}
		sendResponse(client_sock_fd, header);
		close(put_fd);		
		return;
	}

	if (strcmp(header->request, "GET") == 0) {
		char get_buffer[2048];
		// open our file and check for the file size so we can include it in our content-length
		int f_desc = open(header->filename, O_RDONLY, S_IRWXU);
		struct stat finfo;
		int exist = fstat(f_desc, &finfo);
		int length = 0;
		if (exist == 0) {
			length = finfo.st_size;
		}
		if (f_desc == -1) {
			// check if file exists - if not respond 404
			if (exist == -1) {
				header->status_code = 404;
				sendResponse(client_sock_fd, header);
				close(f_desc);
				return;
			}
		}
		if ((S_IRUSR & finfo.st_mode) == 0){
			// you don't have permissions - respond 403
			header->status_code = 403;
			sendResponse(client_sock_fd, header);
			close(f_desc);
			return;
		}
		// read from our file and write it to the socket
		int bytes_read;
		int bytes_written = 0;
		bool respond = false;
		while ((bytes_read = read(f_desc, get_buffer, 1)) > 0) {
			if ((bytes_read != -1) && (respond == false)){
				respond = true;
				dprintf(client_sock_fd, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", length);
			}
			bytes_written = write(client_sock_fd, get_buffer, 1);
		}
		// response for an empty file - 200
		if (length == 0) {
			dprintf(client_sock_fd, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", 0);
			write(client_sock_fd, get_buffer, 0);
		}
		// error reading file - 500
		if (bytes_read == -1) {
			header->status_code = 500;
			sendResponse(client_sock_fd, header);
			close(f_desc);
			return;
		}
		close(f_desc);
		close(client_sock_fd);
		return;
		}
	}

// enqueue + dequeue code for our CircularBuffer
void cb_enqueue(CircularBuffer *cb, int client_sock_fd) {
	while(cb->q_size == cb->max_size) {
		// if queue is full, wait for a dequeue to happen
		printf("Queue is currently full\n");
		pthread_cond_wait(&cb->cond, &cb->buffer_mutex);
	}
	if (cb->front == -1) {
		cb->front = cb->rear = 0;
		cb->client_q[cb->rear] = client_sock_fd;
	} else if (cb->rear == cb->max_size - 1 && cb->front != 0) {
		cb->rear = 0;
		cb->client_q[cb->rear] = client_sock_fd;
	} else {
		cb->rear+=1;
		cb->client_q[cb->rear] = client_sock_fd;
	}
	cb->q_size+=1;
	printf("Item has been enqueued\n");
}

int cb_dequeue(CircularBuffer *cb) {
	while (cb->q_size == 0) {
		// if empty, wait for a queue to happen
		printf("Queue is currently empty\n");
		pthread_cond_wait(&cb->cond, &cb->buffer_mutex);
	}
	int client_sock_fd = cb->client_q[cb->front];
	cb->client_q[cb->front] = -1;
	if (cb->front == cb->rear) {
		cb->front = cb->rear = -1;
	} else if (cb->front == cb->max_size - 1) {
		cb->front = 0;
	} else {
		cb->front+=1;
	}
	cb->q_size-=1;
	printf("Item has been dequeued\n");
	return client_sock_fd;
}

// Code for our worker threads
void* worker(void* args) {
	struct ThreadArg* parg = (struct ThreadArg*) args;
	struct CircularBuffer* pcb = parg->pcb;
	struct httpObject header;
	int rd = parg->rd;
	struct FileLock* gLock = parg->gl;
	int locked = 0;
	header.valid = 0;
	while(1) {
		// lock the mutex
		pthread_mutex_lock(&pcb->buffer_mutex);
		// queue is empty wait for signal
		while(pcb->q_size == 0) {
			printf("Queue is currently empty\n");
			pthread_cond_wait(&pcb->cond, &pcb->buffer_mutex);
		}
		int q_sock_fd = cb_dequeue(pcb);
		pthread_cond_signal(&pcb->cond);
		pthread_mutex_unlock(&pcb->buffer_mutex);
		recv_http_header(q_sock_fd, &header);
		// lock is not in our mapping
		if (parg->file_map.find(header.filename) == parg->file_map.end()) {
			// need to create the lock using the global lock
			locked = 1; // need to lock
			pthread_mutex_lock(&gLock->lock); 
			locked = storeLock(header.filename, parg->file_map); // create and store the lock
			if (locked == 0) { // dont need to lock it
				pthread_mutex_unlock(&gLock->lock);
			}
		}
		pthread_mutex_lock(&parg->file_map[header.filename].lock);
		validate_req(q_sock_fd, &header);
		if (header.valid == 1) {
			complete_req(q_sock_fd, &header, rd);			
		}
		memset(header.buffer, '\0', 4096);
		pthread_mutex_unlock(&parg->file_map[header.filename].lock);
		if (locked == 1) {
			pthread_mutex_unlock(&gLock->lock);
		}
	}
}

int main(int argc, char *argv[]){
	struct sockaddr_in serverAddress;
	int addrLen = sizeof(serverAddress);
	memset(&serverAddress, 0, sizeof serverAddress);

	// reject anything without a specified ip address
	if (argc < 2) {
		printf("Usage:\n");
		printf("  sudo ./httpserver <ip> [port] [-N...]\n");
		printf("  Any ip/port, any number greater than 4 after option [-N]\n");
		exit(1);
	}

	int opt, numThreads, port;
	int foundThreads = 0;
	int redundancy = 0; 
	// read all command-line options - current getopt() for finding number of threads
	while ((opt = getopt(argc, argv, "N:r")) != -1){
		switch (opt) {
			case 'N':
				// number found after option "-N"
				foundThreads = 1;
				numThreads = atoi(optarg);
				break;
			case 'r':
				redundancy = 1;
				break;
			case '?':
				// invalid option
				printf("Usage:\n");
				printf("  sudo ./httpserver <ip> [port] [-N...]\n");
				printf("  Any ip/port, any number greater than 4 after option [-N]\n");
				exit(1);
		}
	}
	// optind - index of first non-option element
	char const *hostname = argv[optind];
	// check if an argument exists after the ip address
	if (argc != optind + 1) {
		port = atoi(argv[optind + 1]);		
	} else {
		port = 80;
	}
	// check if numThreads exists
	if (numThreads < 4 || foundThreads == 0) {
		numThreads = 4;
	}

	// setup the port and ip in serverAddress
	serverAddress.sin_family = AF_INET;
	serverAddress.sin_port = htons(port);
	inet_aton(hostname, &serverAddress.sin_addr);

	// create a socket file descriptor
	int server_sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_sock_fd == -1) {
		perror("socket()");
		exit(1);
	}

	// allow your socket to reuse a port/ip
	int enable = 1;
	int sockopt_result =  setsockopt(server_sock_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &enable, sizeof(enable));
	if (sockopt_result == -1) {
		perror("setsockopt()");
		exit(1);
	}

	// bind the socket to the address and port number specified in serverAddress
	int bind_result = bind(server_sock_fd, (struct sockaddr*) &serverAddress, sizeof serverAddress);
	if (bind_result == -1) {
		perror("bind()");
		exit(1);
	}

	// make socket wait for a client to connect to the server
	int listen_result = listen(server_sock_fd, SOMAXCONN);
	if (listen_result < 0) {
		perror("listen()");
		exit(1);
	}

	/*
	Initialize our CircularBuffer struct and its variables, mutex, and condition variables, pointers to store teh thread
	Create user-specified number of threads
	*/

	int client_queue[numThreads];
	struct CircularBuffer cb;
	pthread_mutex_init(&cb.buffer_mutex, NULL);
	cb.client_q = client_queue;
	cb.max_size = numThreads;
	cb.front = -1;
	cb.rear = -1;
	cb.q_size = 0;
	pthread_cond_init(&cb.cond, NULL);
	pthread_t thread_id[numThreads];
	int i;

	ThreadArg args;
	args.pcb = &cb;
	if (redundancy == 0) {
		args.rd = 0;
	} else {
		args.rd = 1;
	}	

	struct FileLock gLock;
	pthread_mutex_init(&gLock.lock, NULL);
	args.gl = &gLock;

	// Create our worker threads - calls worker()
	for (i=0; i<numThreads; ++i) {
		pthread_create(&thread_id[i], NULL, worker, &args);
	}

	// start of communication with the client
	while(1) {
		// queue is full, wait for a signal
		while(&cb.q_size == &cb.max_size) {
			printf("Queue is currently full\n");
			pthread_cond_wait(&cb.cond, &cb.buffer_mutex);
		}
		// Dispatcher thread waiting for connections and queueing into the CircularBuffer
		int client_sock_fd = accept(server_sock_fd, (struct sockaddr*) &serverAddress, (socklen_t*)&addrLen);
		pthread_mutex_lock(&cb.buffer_mutex);
		cb_enqueue(&cb, client_sock_fd);
		pthread_cond_signal(&cb.cond);
		pthread_mutex_unlock(&cb.buffer_mutex);
	}
}