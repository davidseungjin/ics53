#ifndef SERVER_H
#define SERVER_H

#include <arpa/inet.h>
#include <getopt.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include "protocol.h"

#include "linkedList.h"

#define BUFFER_SIZE 1024
#define SA struct sockaddr
#define NUMBER_JOB_BUF 100

/* prototype. Stop and think once again before beginning implementation */
typedef struct {
    char* room_name;
    char* room_creater;
    List_t* participants;
} chat_room;

typedef struct {
    int client_fd;
    petr_header header;
    char* msg;
} header_and_msg;


/* sbuf_t: Bounded buffer used by the Sbuf package. */
typedef struct {
    header_and_msg* buf; /* Buffer array */
    int n; /* Maximum number of slots */
    int front; /* buf[(front+1)%n] is first item */
    int rear; /* buf[rear%n] is last item */
    sem_t mutex; /* Protects accesses to buf */
    sem_t slots; /* Counts available slots */
    sem_t items; /* Counts available items */
} sbuf_t;

// void *job_thread(void *vargp);
void *job_thread(void* vargp);
void Sem_init(sem_t *sem, int pshared, unsigned int value);
void P(sem_t *sem);
void V(sem_t *sem);
void sbuf_init(sbuf_t *sp, int n);
void sbuf_deinit(sbuf_t *sp);
// void sbuf_insert(sbuf_t *sp, int item);
void sbuf_insert(sbuf_t *sp, int client_fd, petr_header header, char* msg);
header_and_msg sbuf_remove(sbuf_t *sp);

/*
 * Old one is: ... 
 * void run_server(int server_port);
 */
void run_server(int server_port, int number_job_thread);

#endif
