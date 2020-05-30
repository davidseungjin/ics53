#include "server.h"


#define USAGE_MSG   "./bin/petr_server [-h][-j N] PORT_NUMBER AUDIT_FILENAME\n\n" \
                    "  -h               Display this help menu, and returns EXIT_SUCCESS.\n" \
                    "  -j N             Number of job threads. Default to 2.\n" \
                    "  AUDIT_FILENAME   File to output Audit Log message to.\n" \
                    "  PORT_NUMBER      Port number to listen on.\n"


char buffer[BUFFER_SIZE];
int listen_fd;

sem_t buffer_mutex;

/* created global shared resources */
/* 1: User list management */
sem_t users_mutex;
List_t users_list;

/* 2: Room management */
sem_t room_mutex;
chat_room roominfo;

/* 3: Audit Log */
char* audit_log;
sem_t audit_mutex;

/* 4: JOB BUFFER */
sbuf_t job_buffer;

/* sem_init wrapper function: Be careful that this begins with 'S' */
void Sem_init(sem_t *sem, int pshared, unsigned int value) {
    if (sem_init(sem, pshared, value) < 0)
	    perror("Sem_init error");
}

/* sem_wait wrapper function */
void P(sem_t *sem) {
    if (sem_wait(sem) < 0)
	    perror("P error");
}

/* sem_post wrapper function */
void V(sem_t *sem) {
    if (sem_post(sem) < 0)
	    perror("V error");
}

/* Write the given string to the audit_log text file.
   Maybe we can implement more arguments for output?*/
void audit_write(char* text){
    P(&audit_mutex);
    FILE* audit_fp;
    // Open file
    if ( (audit_fp = fopen(audit_log, "a")) < 0)
        perror("Open Audit Log error");
    // Write to file
    fprintf(audit_fp, "%ld\t%s\n", time(NULL), text);
    // Close file
    if (fclose(audit_fp) < 0)
        perror("Close Audit Log error");
    V(&audit_mutex);
}


/* Create an empty, bounded, shared FIFO buffer with n slots.
 * Quoted from textbook
 * Sbuf: A package for synchronizing concurrent access to bounded buffers.
 */
void sbuf_init(sbuf_t *sp, int n){
    sp->buf = calloc(n, sizeof(header_and_msg));
    sp->n = n; /* Buffer holds max of n items */
    sp->front = sp->rear = 0; /* Empty buffer iff front == rear */
    Sem_init(&sp->mutex, 0, 1); /* Binary semaphore for locking */
    Sem_init(&sp->slots, 0, n); /* Initially, buf has n empty slots */
    Sem_init(&sp->items, 0, 0); /* Initially, buf has zero data items */
}

/* Clean up buffer sp */
void sbuf_deinit(sbuf_t *sp){
    free(sp->buf);
}

/* Insert item onto the rear of shared buffer sp */
void sbuf_insert(sbuf_t *sp, int client_fd, petr_header header, char* msg){
    P(&sp->slots); /* Wait for available slot */
    P(&sp->mutex); /* Lock the buffer */
    // printf("1. rear and n is %d, %d\n", sp->rear, sp->n);
    // printf("1. client_fd, header_type, header_len is %d, %d, %d\n", (sp->buf[(sp->rear)%(sp->n)]).client_fd, (sp->buf[(sp->rear)%(sp->n)]).header.msg_type, (sp->buf[(sp->rear)%(sp->n)]).header.msg_len);
    // printf("1. msg is %s\n", (sp->buf[(sp->rear)%(sp->n)]).msg);
    (sp->buf[(sp->rear)%(sp->n)]).client_fd = client_fd; /* Insert the descriptor */
    (sp->buf[(sp->rear)%(sp->n)]).header = header; /* Insert the descriptor */
    (sp->buf[(sp->rear)%(sp->n)]).msg = msg; /* Insert the descriptor */
    // printf("2. rear and n is %d, %d\n", sp->rear, sp->n);
    // printf("2. client_fd, header_type, header_len is %d, %d, %d\n", (sp->buf[(sp->rear)%(sp->n)]).client_fd, (sp->buf[(sp->rear)%(sp->n)]).header.msg_type, (sp->buf[(sp->rear)%(sp->n)]).header.msg_len);
    // printf("2. msg is %s\n", (sp->buf[(sp->rear)%(sp->n)]).msg);
    (sp->rear)++;
    V(&sp->mutex); /* Unlock the buffer */
    V(&sp->items); /* Announce available item */
}

/* Remove and return the first item from buffer sp */
header_and_msg sbuf_remove(sbuf_t *sp){
    header_and_msg item;
    P(&sp->items); /* Wait for available item */
    P(&sp->mutex); /* Lock the buffer */
    // printf("1. front and n is %d, %d\n", sp->front, sp->n);
    // printf("1. client_fd, header_type, header_len is %d, %d, %d\n", (sp->buf[(sp->front)%(sp->n)]).client_fd, (sp->buf[(sp->front)%(sp->n)]).header.msg_type, (sp->buf[(sp->front)%(sp->n)]).header.msg_len);
    // printf("1. msg is %s\n", (sp->buf[(sp->front)%(sp->n)]).msg);
    item.client_fd = (sp->buf[(sp->front)%(sp->n)]).client_fd; /* Insert the descriptor */
    item.header = (sp->buf[(sp->front)%(sp->n)]).header; /* Insert the descriptor */
    item.msg = (sp->buf[(sp->front)%(sp->n)]).msg; /* Insert the descriptor */
    (sp->front)++;
    // printf("2. front and n is %d, %d\n", sp->front, sp->n);
    // printf("2. client_fd, header_type, header_len is %d, %d, %d\n", (sp->buf[(sp->front)%(sp->n)]).client_fd, (sp->buf[(sp->front)%(sp->n)]).header.msg_type, (sp->buf[(sp->front)%(sp->n)]).header.msg_len);
    // printf("2. msg is %s\n", (sp->buf[(sp->front)%(sp->n)]).msg);
    V(&sp->mutex); /* Unlock the buffer */
    V(&sp->slots); /* Announce available slot */

    return item;
}


void sigint_handler(int sig) {
    printf("shutting down server\n");
    close(listen_fd);
    exit(0);
}

int server_init(int server_port) {
    int sockfd;
    struct sockaddr_in servaddr;

    /* Socket creation with setting of IPv4, TCP/IP */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        printf("Socket creation failed...\n");
        exit(EXIT_FAILURE);
    } else {
        printf("Socket successfully created\n");
    }

    /* fill zero at servaddr structure */
    bzero(&servaddr, sizeof(servaddr));

    /* fill the setting. host to network short is network order or byte order? */
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(server_port);

    /* Socket reuse */
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, (char *)&opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    /* Binding newly created socket to given IP and verification */
    if ((bind(sockfd, (SA *)&servaddr, sizeof(servaddr))) != 0) {
        printf("Socket bind failed\n");
        exit(EXIT_FAILURE);
    } else {
        printf("Socket successfully binded\n");
    }

    /* Now server is ready to listen and verification */
    if ((listen(sockfd, 1)) != 0) {
        printf("Listen failed\n");
        exit(EXIT_FAILURE);
    } else
        printf("Currently listening on port %d.\n", server_port);

    return sockfd;
}

//Function running in client thread
void *process_client(void *clientfd_ptr) {
    /* After finishing work, automatically terminate instead of join to mainthread */
    pthread_detach(pthread_self());

    /* make it local rather than using dynamically allocated variable. After assigning, free the original */
    int client_fd = *(int *)clientfd_ptr;
    free(clientfd_ptr);

    /* declare return value */
    int retval;

    /* declare message header for receiving via petr protocol */
    petr_header recv_header;

    Sem_init(&buffer_mutex, 0, 1);

    while (1) {
        /* Two step reading.
         * First: receive msg to clienf_fd.
         * The data contained is msg type and length. into recv_header
         * retval is just to see status of transit
         */
        retval = rd_msgheader(client_fd, &recv_header);
        if (retval < 0) {
            printf("Receiving failed\n");
            break;
        }
        /* After reading header, receive the correct length
         * as stated in recv_header.msg_len. (buffer clear is necessary, so used bzero)
         * retval is just to check status of transfer
         *
         * And MUTEX on buffer to write.
         */

        P(&buffer_mutex);
        /* So, latest data written on buffer will be maintained
         * until the next thread access to buffer with P(&buffer_mutex)
         */
        bzero(buffer, BUFFER_SIZE);

        retval = read(client_fd, buffer, recv_header.msg_len);
        if (retval <=  0) {
            printf("Receiving failed\n");
            break;
        }

        V(&buffer_mutex);

        /* using sbuf producer-consumer system
         * 1. store data in buf: client_fd, header, msg stored in buffer
         * 2. circular array to make FIFO.
         * 3. three semaphores: a. for overall. b. for slot, c. for put item
         * especially, the reason I made structure contains data was because
         * rd_msgheader function didn't work well in the second execution.
         * Pass by value, and forget about the rd_msgheader function.
         */
        sbuf_insert(&job_buffer, client_fd, recv_header, buffer);

    }

    /* Close the socket at the end */
    printf("Close one client connection\n");
    close(client_fd);
    return NULL;
}

/* Job thread function*/
void *job_thread(void* vargp){
    // printf("job_thread is doing, thread ID is %ld\n", pthread_self());
    while(1){

        /* Background
         * Original plan was to use rd_msgheader function again to pull header
         * and msg (from buffer) before bzero.
         * However, the function didn't work twice well enoguth.
         * So, modified sbuf_t. Instead of putting integer, made structure.
         * That contains client_fd, header, msg. By dynamically allocation.
         */
        header_and_msg item = sbuf_remove(&job_buffer);

        /* Once you took client_fd, header, msg... DO SOMETHING !!!!! */

        /* declare message header for receiving via petr protocol */

        printf("job_thread: item.msg is %s\n", item.msg);
        printf("job_thread: item.client_fd is %d\n", item.client_fd);
        printf("job_thread: item.header.msg_type 0x%x\n", item.header.msg_type);
        printf("job_thread: item.header.msg_len %d\n", item.header.msg_len);

        /* declare message header for sending via petr protocaol */
        petr_header send_header;
        send_header.msg_len = 0;
        send_header.msg_type = OK;

        /* Later, the message should be customized.
         * It is just for checking xterm window work or not.
         * According to msg type. Server needs to react differently.
         * If server react differently, xterm window will occur accordingly.
         * uncomment out below for testing.
         */
        // wr_msg(item.client_fd, &send_header, NULL);




    }
    /* Is it necessary? is it just because of void* function? */
    return NULL;
}


void run_server(int server_port, int number_job_thread) {
    listen_fd = server_init(server_port); // Initiate server and start listening on specified port

    // update audit_log
    Sem_init(&audit_mutex, 0, 1);
    audit_write("\nServer initialized and listening");

    /* create new local variables. */
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    // change type from int to socklen_t

    int retval = 0;
    int user_exist = 0;
    char* user_name = NULL;
    pthread_t tid;

    petr_header login_header;
    petr_header reply_header;

    /* initialize global shared resources. setup semaphore for user name */
    Sem_init(&users_mutex, 0, 1);

    users_list.head = NULL;
    users_list.length = 0;

    /* create N job threads */
    for(int i = 0; i < number_job_thread; i++){
        pthread_create(&tid, NULL, job_thread, NULL);
        // update audit log
        audit_write("Job Thread created");
    }

    /* from accepting, user check */
    while (1) {
        user_exist = 0;
        user_name = malloc(sizeof(char));

        /* Wait and Accept the connection from client
         * in some other books, it says connfd because connected fd on server
         */
        int *client_fd = malloc(sizeof(int));
        *client_fd = accept(listen_fd, (SA *)&client_addr, &client_addr_len);
        printf("*client_fd is %d\n", *client_fd);

        /* Condition when connection is failed, check by return value of accept */
        if (*client_fd < 0) {
            printf("server acccept failed\n");
            exit(EXIT_FAILURE);

        // accept now connection
        } else {
            //update audit_log
            audit_write("New client connection accepted");
            printf("New client connetion accepted\n");

            /* read the message header from the new client
             * login_header is petr_header struct
             */
            retval = rd_msgheader(*client_fd, &login_header);
            if (retval < 0) {
                printf("Receiving failed\n");
                exit(EXIT_FAILURE);
            }

            /* read the message body (username) from the new client */
            bzero(buffer, BUFFER_SIZE);
            retval = read(*client_fd, user_name, login_header.msg_len);
            if (retval < 0) {
                printf("Receiving failed\n");
                exit(EXIT_FAILURE);
            }

            // check if the message type is LOGIN
            if(login_header.msg_type == LOGIN){
                printf("New client login\n");
                P(&users_mutex);

                /* check if the user name already exists
                 * users_list is List_t struct
                 */
                node_t* current = users_list.head;
                while(current != NULL){
                    /* send error message back to client if username already exists*/
                    if(strcmp( ((char*)(current->value)), user_name ) == 0){
                        user_exist = 1;
                        reply_header.msg_len = 0;
                        reply_header.msg_type = EUSREXISTS;
                        retval = wr_msg(*client_fd, &reply_header, NULL);
                        if (retval < 0) {
                            printf("Sending failed\n");
                            exit(EXIT_FAILURE);
                        }
                        /* when EUSREXISTS, client_fd close, disconnected */
                        close(*client_fd);
                        break;
                    }
                    current = current->next;
                }
                // continue and wait for new client if user name already exists
                if(user_exist == 1){
                    free(user_name);
                    V(&users_mutex);
                    printf("User name already exists\n");
                    continue;
                }
                /* if username did not exist, add username to linked list. UNORDERED */
                insertRear(&users_list, (void*)user_name, *client_fd);

                // (for testing) print out the users linked list
                current = users_list.head;
                while(current != NULL){
                    printf("user: %s\t", (char*)(current->value));
                    printf("fd: %d\n", (current->fd));
                    current = current->next;
                }

                V(&users_mutex);

                // write message back to client to confirm login
                reply_header.msg_len = 0;
                reply_header.msg_type = OK;
                retval = wr_msg(*client_fd, &reply_header, NULL);
                if (retval < 0) {
                    printf("Sending failed\n");
                    exit(EXIT_FAILURE);
                }

                // after login successfully, create a client thread
                pthread_create(&tid, NULL, process_client, (void *)client_fd);
            }
        }
    }
    bzero(buffer, BUFFER_SIZE);
    close(listen_fd);
}

int main(int argc, char *argv[]) {
    int opt;
    int N = 2;
    unsigned int port = 0;

    while ((opt = getopt(argc, argv, "hj:")) != -1) {
        switch (opt) {
        case 'h':
            printf(USAGE_MSG);
            return EXIT_SUCCESS;
        case 'j':
            N = atoi(optarg);
            break;
        default:
            fprintf(stderr, "\n" USAGE_MSG);
            return EXIT_FAILURE;
        }
    }

    // validate that we have 2 positional argument
    if (optind + 2 != argc) {
        fprintf(stderr, "Exactly two positional argument should be specified.\n\n" USAGE_MSG);
        return EXIT_FAILURE;
    }

    // parse the two positional argument
    port = atoi(argv[optind]);

    audit_log = argv[optind+1];

    /* initialization of sbuf. It's job buffer.
     * After complete running run_server fuction
     * free it
     */
    sbuf_init(&job_buffer, NUMBER_JOB_BUF);

    /* run server with N number of job thread */
    run_server(port, N);

    /* After complete execution of run_server
     * deinitialization(free the buffer)
     */
    sbuf_deinit(&job_buffer);
    return 0;
}
