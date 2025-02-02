/*
 * Group# 110
 * Group member     /   member's UCInetID
 * Huikang Huang    /   huikangh
 * Seungjin Lee     /   seungl21
 *
 */

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
sem_t rooms_mutex;
List_t rooms_list;

/* 3: Audit Log */
char* audit_log;
FILE* audit_fp;
sem_t audit_mutex;

/* 4: JOB BUFFER */
sbuf_t job_buffer;

/* 5: Variables for the server thread */
char* client_name;
int* client_fd;

/* 6: Pointer to the job_thread tid array */
int N;
pthread_t* job_thread_array_ptr;

/* 7: Time structure, function, string */
char logtime[50];
char* gettime(){
    bzero(logtime, sizeof(logtime));
    time_t rawtime;
    time(&rawtime);
    struct tm * timeinfo = localtime(&rawtime);
    strftime(logtime, sizeof(logtime), "%a %b %d %Y, %T", timeinfo);
    return logtime;
}



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


/* Handler to clean up in case of "ctrl-c" on server */
void sigint_handler(int sig) {
    printf("\nshutting down server\n");
    // update audit log
    P(&audit_mutex);
    audit_fp = fopen(audit_log, "a");
    fprintf(audit_fp, "%s\tServer terminates\n\n", gettime());
    fclose(audit_fp);
    V(&audit_mutex);

    // Remove and free everything in rooms_list
    node_t* current = rooms_list.head;
    while(current != NULL){
        chat_room* room_ptr = (chat_room*)(current->value);
        node_t* participant = room_ptr->participants->head;
        while(participant != NULL){
            free(participant->value);
            participant = participant->next;
        }
        deleteList(room_ptr->participants);
        free(room_ptr->room_name);
        free(room_ptr->participants);
        free(room_ptr);

        current = current->next;
    }
    deleteList(&rooms_list);

    // Remove and free everything in users_list
    current = users_list.head;
    while(current != NULL){
        free(current->value);
        current = current->next;
    }
    deleteList(&users_list);

    // Clean up all the job_threads
    for(int i=0; i < N; i++){
        //printf("job thread tid: %ld\n", job_thread_array_ptr[i]);
        pthread_cancel(job_thread_array_ptr[i]);
    }

    // Close any file descriptors and free any allocatd memories
    free(client_name);
    free(client_fd);
    close(listen_fd);
    sbuf_deinit(&job_buffer);
    bzero(&job_buffer, sizeof(job_buffer));
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
        //printf("Socket successfully created\n");
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
        //printf("Socket successfully binded\n");
    }

    /* Now server is ready to listen and verification */
    if ((listen(sockfd, 1)) != 0) {
        printf("Listen failed\n");
        exit(EXIT_FAILURE);
    } else
        printf("Currently listening on port %d.\n", server_port);

    return sockfd;
}

//Function running in client thready
void *process_client(void *clientfd_ptr) {
    /* After finishing work, automatically terminate instead of join to mainthread */
    pthread_detach(pthread_self());

    /* make it local rather than using dynamically allocated variable. After assigning, free the original */
    int client_fd = *(int *)clientfd_ptr;
    free(clientfd_ptr);

    /* declare return value */
    int retval;
    /* flag to check if the last message is LOGOUT */
    int logout = 0;

    /* declare message header for receiving via petr protocol */
    petr_header recv_header;
    bzero(&recv_header, sizeof(petr_header));

    Sem_init(&buffer_mutex, 0, 1);

    while (1) {

        /* Two step reading.
         * First: receive msg to clienf_fd.
         * The data contained is msg type and length. into recv_header
         * retval is just to see status of transit
         * bzero is to clear recv_header buffer before receiving data.
         */
        bzero(&recv_header, sizeof(recv_header));

        retval = rd_msgheader(client_fd, &recv_header);
        if (retval < 0) {
            printf("Reading message header failed\n");
            break;
        }

        // update audit log
        P(&audit_mutex);
        audit_fp = fopen(audit_log, "a");
        fprintf(audit_fp, "%s\tClient thread %ld receives message from client\n\t\t\t\t\t\t\t" \
                "Header Type: 0x%x,\tHeader Length: %d\n", gettime(), pthread_self(), recv_header.msg_type, recv_header.msg_len);
        fclose(audit_fp);
        V(&audit_mutex);

        /* Check the value of the header.
         * If the header has a msg_type of 0 and a msg_len of 0,
         * that means the header did not read in new informations,
         * hence the client connection is closed.
         */
        if(recv_header.msg_type == 0 && recv_header.msg_len == 0){
            /* If the client did not logout properly,
             * send LOGOUT to job buffer to remove user properly.
             * Set msg_len to -1 so the job_thread won't send OK message
             * back to the closed client.
             */
            if(logout != 1){
                recv_header.msg_len = -1;
                recv_header.msg_type = LOGOUT;
                sbuf_insert(&job_buffer, client_fd, recv_header, NULL);
            }
            printf("Client closes connection\n");
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
        if (retval <  0) {
            printf("Reading message body failed\n");
            break;
        // break if EOF is reached (client closes connection)
        }else if (retval == 0 && recv_header.msg_len != 0){
            printf("Reading message body failed\n");
            break;
        }
        V(&buffer_mutex);

        // update audit log
        P(&audit_mutex);
        audit_fp = fopen(audit_log, "a");
        fprintf(audit_fp, "%s\tClient thread %ld reads message body\n\t\t\t\t\t\t\t" \
                "Message Body: %s\n", gettime(), pthread_self(), buffer);
        fclose(audit_fp);
        V(&audit_mutex);

        /* using sbuf producer-consumer system
         * 1. store data in buf: client_fd, header, msg stored in buffer
         * 2. circular array to make FIFO.
         * 3. three semaphores: a. for overall. b. for slot, c. for put item
         * especially, the reason I made structure contains data was because
         * rd_msgheader function didn't work well in the second execution.
         * Pass by value, and forget about the rd_msgheader function.
         */

        sbuf_insert(&job_buffer, client_fd, recv_header, buffer);

        // update audit log
        P(&audit_mutex);
        audit_fp = fopen(audit_log, "a");
        fprintf(audit_fp, "%s\tClient thread %ld inserts job to job buffer\n", gettime(), pthread_self());
        fclose(audit_fp);
        V(&audit_mutex);

        if(recv_header.msg_type == LOGOUT){
            logout = 1;
        }

    }

    // update audit log
    P(&audit_mutex);
    audit_fp = fopen(audit_log, "a");
    fprintf(audit_fp, "%s\tTerminate Client thread %ld\n", gettime(), pthread_self());
    fclose(audit_fp);
    V(&audit_mutex);

    /* Close the socket at the end */
    printf("Server closes connection to client\n");
    close(client_fd);
    return NULL;
}

/* Job thread function*/
void *job_thread(void* vargp){
    pthread_detach(pthread_self());

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

        // update audit log
        P(&audit_mutex);
        audit_fp = fopen(audit_log, "a");
        fprintf(audit_fp, "%s\tJob thread %ld removes job from job buffer\n\t\t\t\t\t\t\t" \
                "ClientFD: %d,\tHeader Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t" \
                "Message Body: %s\n", gettime(), pthread_self(), item.client_fd, item.header.msg_type, item.header.msg_len, item.msg);
        fclose(audit_fp);
        V(&audit_mutex);

        //printf("job_thread: item.msg is %s\n", item.msg);
        //printf("job_thread: item.client_fd is %d\n", item.client_fd);
        //printf("job_thread: item.header.msg_type %d\n", item.header.msg_type);
        //printf("job_thread: item.header.msg_len %d\n", item.header.msg_len);

        /* declare mssage header for sending via petr protocaol */
        petr_header send_header;
        bzero(&send_header, sizeof(petr_header));
        send_header.msg_len = 0;
        send_header.msg_type = OK;

        petr_header recv_header;
        bzero(&recv_header, sizeof(petr_header));
        recv_header.msg_len = 0;
        recv_header.msg_type = OK;

        /* Later, the message should be customized.
         * It is just for checking xterm window work or not.
         * According to msg type. Server needs to react differently.
         * If server react differently, xterm window will occur accordingly.
         * uncomment out below for testing.
         */

        // ESERV: generic error

        if(item.header.msg_type == LOGOUT){
            /*
            when from_user send logout,
            server send "OK" to from_user

            if: from_user is participants of rooms. --> RMLEAVE
            if: from_user is creater of rooms. --> send participants RMCLOSED; RMDELETE this room (Does it need OK to from_user again?)
            AFTER these two --> from_user will be deleted from user_list.
            */
            P(&users_mutex);
            // flag used to keep track if there are any more room to delete
            // in rooms_list
            int deleteroom = 1;
            char room_name[1024];
            char user_name[1024];
            strcpy(user_name, find_name_by_fd(&users_list, item.client_fd));
            int user_fd;
            user_fd = item.client_fd;
            V(&users_mutex);

            /* First, repeatly iterate through rooms_list to find all the room
             * that the user created. For each of these room, send RMCLOSED to
             * all the participants. Delete only one room in each full
             * iteration of the rooms_list
             */
            P(&rooms_mutex);
            while(deleteroom == 1){
                // reset the flags
                int index = 0;
                deleteroom = 0;

                node_t* current = rooms_list.head;
                while(current != NULL){
                    chat_room* room_ptr = (chat_room*)(current->value);
                    // if the user is the creater of the room
                    if(strcmp(user_name, room_ptr->room_creater) == 0){
                        // set the deleteroom flag to 1
                        deleteroom = 1;
                        // send RMCLOSED to every participant except the user
                        // in the room, and free the participant name
                        strcpy(room_name, room_ptr->room_name);
                        node_t* participant = room_ptr->participants->head;
                        while(participant != NULL){
                            if(strcmp(user_name, participant->value) != 0){
                                send_header.msg_len = strlen(room_name)+1;
                                send_header.msg_type = RMCLOSED;
                                wr_msg(participant->fd, &send_header, room_name);

                                // update audit log
                                P(&audit_mutex);
                                audit_fp = fopen(audit_log, "a");
                                fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                                        "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                                        "Message Body: %s\n", gettime(), pthread_self(), participant->fd, send_header.msg_type, send_header.msg_len, room_name);
                                fclose(audit_fp);
                                V(&audit_mutex);
                            }
                            free(participant->value);
                            participant = participant->next;
                        }
                        // delete the participants list in the user created room
                        deleteList(room_ptr->participants);
                        // free everything inside the room structure
                        free(room_ptr->room_name);
                        free(room_ptr->participants);
                        free(room_ptr);
                        break;
                    }
                    current = current->next;
                    index++;
                }
                // delete the room using the index
                removeByIndex(&rooms_list, index);

            }

            /* After deleting all the rooms created by the user,
             * now iterate through rooms_list to leave any room
             * the user is in.
             */

            // For each room in rooms_list, check if user is in the room.
            // If user is in the room, remove user from the room's participants.
            node_t* current = rooms_list.head;
            while(current != NULL){
                int index = 0;
                chat_room* room_ptr = (chat_room*)(current->value);
                node_t* participant = room_ptr->participants->head;
                while(participant != NULL){
                    // if the user is in the room
                    if(strcmp(user_name, participant->value) == 0){
                        // remove the user from the room
                        free(participant->value);
                        removeByIndex(room_ptr->participants, index);
                        break;
                    }
                    participant = participant->next;
                    index++;
                }
                current = current->next;
            }
            V(&rooms_mutex);

            /* Finally, remove the user from users_list */
            P(&users_mutex);
            int index = 0;
            current = users_list.head;
            while(current != NULL){
                if(strcmp(user_name, current->value) == 0){
                    free(current->value);
                    break;
                }
                current = current->next;
                index++;
            }
            removeByIndex(&users_list, index);
            V(&users_mutex);

            /* send OK back to client if the client did not force-terminate
             * send_header.msg_len = 0;
             * send_header.msg_type = OK;
             */
            if(item.header.msg_len != -1){
                send_header.msg_len = 0;
                send_header.msg_type = OK;
                wr_msg(user_fd, &send_header, NULL);
            }

            // update audit log
            P(&audit_mutex);
            audit_fp = fopen(audit_log, "a");
            fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                    "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                    "Message Body: %s\n", gettime(), pthread_self(), item.client_fd, send_header.msg_type, send_header.msg_len, "");
            fclose(audit_fp);
            V(&audit_mutex);

            continue;
        }

        if(item.header.msg_type == RMCREATE){
            // flag that checks if room exists or not
            int roomexist = 0;

            P(&rooms_mutex);
            // check if room already exist
            node_t* current = rooms_list.head;
            while(current != NULL){
                // if the room name already exists in rooms_list
                if(strcmp(item.msg, ((chat_room*)(current->value))->room_name) == 0){
                    // sending to client error ERMEXISTS: Room exists already
                    send_header.msg_len = 0;
                    send_header.msg_type = ERMEXISTS;
                    wr_msg(item.client_fd, &send_header, NULL);
                    roomexist = 1;

                    // update audit log
                    P(&audit_mutex);
                    audit_fp = fopen(audit_log, "a");
                    fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                            "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                            "Message Body: %s\n", gettime(), pthread_self(), item.client_fd, send_header.msg_type, send_header.msg_len, "");
                    fclose(audit_fp);
                    V(&audit_mutex);
                    break;
                }
                current = current->next;
            }
            V(&rooms_mutex);

            /* If room does not exist, create a new chat_room,
               and create a new linked list for the participants
             */
            if(roomexist == 0){

                P(&rooms_mutex);
                /* create new strings and new spaces in the memory
                   so the strings won't get overwritten
                 */
                char* room_name = malloc(BUFFER_SIZE);
                strcpy(room_name, item.msg);

                char* user = find_name_by_fd(&users_list, item.client_fd);
                char* user_name = malloc(BUFFER_SIZE);
                strcpy(user_name, user);

                int user_fd = item.client_fd;

                // create new linked list for the partcipants in the room
                List_t* participants = malloc(sizeof(List_t));
                // initialize the linked list
                participants->head = NULL;
                participants->length = 0;
                // add the creater to the linked list
                insertRear(participants, (void*)user_name, user_fd);

                // create and initialize new chat_room
                chat_room* new_room = malloc(sizeof(chat_room));
                new_room->room_name = room_name;
                new_room->room_creater = user_name;
                new_room->participants = participants;
                // add the new chat_room to the rooms_list linked list
                insertRear(&rooms_list, (void*)new_room, -1);

                // sending to client "OK" when successfully create room
                send_header.msg_len = 0;
                send_header.msg_type = OK;
                wr_msg(item.client_fd, &send_header, NULL);

                V(&rooms_mutex);

                // update audit log
                P(&audit_mutex);
                audit_fp = fopen(audit_log, "a");
                fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                        "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                        "Message Body: %s\n", gettime(), pthread_self(), item.client_fd, send_header.msg_type, send_header.msg_len, "");
                fclose(audit_fp);
                V(&audit_mutex);
                }
            continue;
        }

        if(item.header.msg_type == RMDELETE){
            // flag that checks if room exists or not
            int roomexist = 0;
            // flag that checks if we should remove the room or not
            int roomremove = 0;
            // flag that keeps track of the index of the targer room in list
            int index = 0;
            // obtain room name
            char* room_name = item.msg;
            // obtain user name
            char* user_name = find_name_by_fd(&users_list, item.client_fd);

            P(&rooms_mutex);
            // check if room already exist
            node_t* current = rooms_list.head;
            while(current != NULL){
                chat_room* room_ptr = (chat_room*)(current->value);
                // if the room exists in the rooms_list
                if(strcmp(room_name, room_ptr->room_name) == 0){

                    // if the room exists, and user is the room creater
                    if(strcmp(user_name, room_ptr->room_creater) == 0){

                        // for every participant in the room
                        // send RMCLOSED message, and free the participant name
                        node_t* participant = room_ptr->participants->head;
                        while(participant != NULL){
                            if(strcmp(participant->value, user_name) != 0){
                                send_header.msg_len = strlen(room_name) + 1;
                                send_header.msg_type = RMCLOSED;
                                wr_msg(participant->fd, &send_header, room_name);

                                // update audit log
                                P(&audit_mutex);
                                audit_fp = fopen(audit_log, "a");
                                fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                                        "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                                        "Message Body: %s\n", gettime(), pthread_self(), participant->fd, send_header.msg_type, send_header.msg_len, room_name);
                                fclose(audit_fp);
                                V(&audit_mutex);
                            }
                            free(participant->value);
                            participant = participant->next;
                        }

                        // free every node in participants list
                        deleteList(room_ptr->participants);
                        // free everything inside the room structure
                        free(room_ptr->room_name);
                        free(room_ptr->participants);
                        free(room_ptr);
                        // est the flag for room remove
                        roomremove = 1;

                    // if the room exist, but user is not the creater
                    }else{
                        // send ERMDENIED message to user
                        send_header.msg_len = 0;
                        send_header.msg_type = ERMDENIED;
                        wr_msg(item.client_fd, &send_header, NULL);

                        // update audit log
                        P(&audit_mutex);
                        audit_fp = fopen(audit_log, "a");
                        fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                                "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                                "Message Body: %s\n", gettime(), pthread_self(), item.client_fd, send_header.msg_type, send_header.msg_len, "");
                        fclose(audit_fp);
                        V(&audit_mutex);
                    }

                    // set the flag for room exist
                    roomexist = 1;
                    break;
                }
                current = current->next;
                index++;
            }
            V(&rooms_mutex);

            // if the room does not exist, send ERMNOTFOUND error client
            if(roomexist == 0){
                send_header.msg_len = 0;
                send_header.msg_type = ERMNOTFOUND;
                wr_msg(item.client_fd, &send_header, NULL);

                // update audit log
                P(&audit_mutex);
                audit_fp = fopen(audit_log, "a");
                fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                        "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                        "Message Body: %s\n", gettime(), pthread_self(), item.client_fd, send_header.msg_type, send_header.msg_len, "");
                fclose(audit_fp);
                V(&audit_mutex);
                continue;
            }

            // if the room exist and the user is the creater,
            // remove the room node from rooms_list
            // and send OK message to user
            P(&rooms_mutex);
            if(roomexist == 1 && roomremove == 1){
                removeByIndex(&rooms_list, index);
                send_header.msg_len = 0;
                send_header.msg_type = OK;
                wr_msg(item.client_fd, &send_header, NULL);

                // update audit log
                P(&audit_mutex);
                audit_fp = fopen(audit_log, "a");
                fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                        "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                        "Message Body: %s\n", gettime(), pthread_self(), item.client_fd, send_header.msg_type, send_header.msg_len, "");
                fclose(audit_fp);
                V(&audit_mutex);
            }
            V(&rooms_mutex);

            continue;
        }

        if(item.header.msg_type == RMLIST){
            // return list of rooms w/ users per room
            // <roomname>:<username>,...,<username>\n...

            P(&rooms_mutex);
            // check if there are any rooms in rooms_list
            // if no room in rooms_list, send RMLIST with no message body to user
            node_t* current = rooms_list.head;
            if(current == NULL){
                send_header.msg_len = 0;
                send_header.msg_type = RMLIST;
                wr_msg(item.client_fd, &send_header, NULL);
                V(&rooms_mutex);

                // update audit log
                P(&audit_mutex);
                audit_fp = fopen(audit_log, "a");
                fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                        "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                        "Message Body: %s\n", gettime(), pthread_self(), item.client_fd, send_header.msg_type, send_header.msg_len, "");
                fclose(audit_fp);
                V(&audit_mutex);
                continue;
            }

            int msg_size = 0;
            char msg_body[BUFFER_SIZE];
            bzero(&msg_body, BUFFER_SIZE);

            // for every room in room_list
            while(current != NULL){
                chat_room* room_ptr = (chat_room*)(current->value);
                // concatenate the room name to msg_body
                strcat(msg_body, room_ptr->room_name);
                strcat(msg_body, ": ");
                msg_size = msg_size + strlen(room_ptr->room_name) + 2;
                // for every participant in the room
                node_t* participant = room_ptr->participants->head;
                while(participant != NULL){
                    // concatenate the participant name to msg_body
                    strcat(msg_body, participant->value);
                    msg_size = msg_size + strlen(participant->value);
                    // concatenate comma if the current participant is not the last
                    if(participant->next != NULL){
                        strcat(msg_body, ",");
                        msg_size = msg_size + 1;
                    }
                    participant = participant->next;
                }
                // concatenate new line after concatenating every participants in the room
                strcat(msg_body, "\n");
                msg_size = msg_size + 1;
                current = current->next;
            }
            V(&rooms_mutex);

            // size + 1 for the null terminator at the end
            msg_size = msg_size + 1;

            send_header.msg_len = msg_size;
            send_header.msg_type = RMLIST;
            wr_msg(item.client_fd, &send_header, msg_body);

            // update audit log
            P(&audit_mutex);
            audit_fp = fopen(audit_log, "a");
            fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                    "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                    "Message Body: %s\n", gettime(), pthread_self(), item.client_fd, send_header.msg_type, send_header.msg_len, msg_body);
            fclose(audit_fp);
            V(&audit_mutex);

            continue;
        }

        if(item.header.msg_type == RMJOIN){
            // set up local variables
            int roomexist = 0;
            char* room_name = item.msg;
            int user_fd = item.client_fd;

            char* user = find_name_by_fd(&users_list, item.client_fd);
            char* user_name = malloc(BUFFER_SIZE);
            strcpy(user_name, user);


            P(&rooms_mutex);
            // check if the room exist or not
            node_t* current = rooms_list.head;
            while(current != NULL){
                chat_room* room_ptr = (chat_room*)(current->value);
                // if the room name exists in rooms_list
                if(strcmp(room_name, room_ptr->room_name) == 0){

                    // check if the user is not previously in the room
                    int userinroom = 0;
                    node_t* participant = room_ptr->participants->head;
                    while(participant != NULL){
                        if(strcmp(user_name, participant->value) == 0){
                            userinroom = 1;
                        }
                        participant = participant->next;
                    }

                    // if the user is not previouslt in the list,
                    // add user to the room's participant linked list
                    if(userinroom == 0){
                        List_t* members_list = ((chat_room*)(current->value))->participants;
                        insertRear(members_list, (void*)user_name, user_fd);
                    }
                    // set the flag for room exist
                    roomexist = 1;
                    break;
                }
                current = current->next;
            }
            V(&rooms_mutex);

            // if the room does not exist, send error to client
            if(roomexist == 0){
                send_header.msg_len = 0;
                send_header.msg_type = ERMNOTFOUND;
                wr_msg(item.client_fd, &send_header, NULL);

            // if the user is added to the room, send OK to client
            }else{
                send_header.msg_len = 0;
                send_header.msg_type = OK;
                wr_msg(item.client_fd, &send_header, NULL);
            }

            // update audit log
            P(&audit_mutex);
            audit_fp = fopen(audit_log, "a");
            fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                    "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                    "Message Body: %s\n", gettime(), pthread_self(), item.client_fd, send_header.msg_type, send_header.msg_len, "");
            fclose(audit_fp);
            V(&audit_mutex);

            continue;
        }

        if(item.header.msg_type == RMLEAVE){
            // flag that checks if room exists or not
            int roomexist = 0;
            // flag that checks if user in the room or not
            int userinroom = 0;
            // index that keeps track of the user's index in the participants list
            int index = 0;
            // obtain room name
            char* room_name = item.msg;
            // obtain user name
            char* user_name = find_name_by_fd(&users_list, item.client_fd);

            P(&rooms_mutex);
            // check if room already exist
            node_t* current = rooms_list.head;
            while(current != NULL){
                chat_room* room_ptr = (chat_room*)(current->value);
                // if the room exists in the rooms_list,
                if(strcmp(room_name, room_ptr->room_name) == 0){
                    // set flag for room exist
                    roomexist = 1;
                    // check if the user is in the room
                    node_t* participant = room_ptr->participants->head;
                    while(participant != NULL){
                        // if the user is in the room
                        if(strcmp(participant->value, user_name) == 0){
                            // set flag for user in room
                            userinroom = 1;
                            // if the user is in the room & not creater
                            if(strcmp(room_ptr->room_creater, user_name) != 0){
                                // remove the user from the participants list
                                free(participant->value);
                                removeByIndex(room_ptr->participants, index);
                                // send OK message to user
                                send_header.msg_len = 0;
                                send_header.msg_type = OK;
                                wr_msg(item.client_fd, &send_header, NULL);

                                // update audit log
                                P(&audit_mutex);
                                audit_fp = fopen(audit_log, "a");
                                fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                                        "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                                        "Message Body: %s\n", gettime(), pthread_self(), item.client_fd, send_header.msg_type, send_header.msg_len, "");
                                fclose(audit_fp);
                                V(&audit_mutex);

                            // else if the user is in the room but is the creater
                            }else{
                                // send ERMDENIED because creater can't leave room
                                send_header.msg_len = 0;
                                send_header.msg_type = ERMDENIED;
                                wr_msg(item.client_fd, &send_header, NULL);

                                // update audit log
                                P(&audit_mutex);
                                audit_fp = fopen(audit_log, "a");
                                fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                                        "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                                        "Message Body: %s\n", gettime(), pthread_self(), item.client_fd, send_header.msg_type, send_header.msg_len, "");
                                fclose(audit_fp);
                                V(&audit_mutex);
                                break;
                            }
                        }
                        participant = participant->next;
                        index++;
                    }
                }
                current = current->next;
            }
            V(&rooms_mutex);

            // if the room does not exist,
            // send ERMNOTFOUND to user
            if(roomexist == 0){
                send_header.msg_len = 0;
                send_header.msg_type = ERMNOTFOUND;
                wr_msg(item.client_fd, &send_header, NULL);
            }

            // if the room exist, but the user is not in the room
            // send OK to user
            if(roomexist == 1 && userinroom == 0){
                send_header.msg_len = 0;
                send_header.msg_type = OK;
                wr_msg(item.client_fd, &send_header, NULL);
            }

            // update audit log
            P(&audit_mutex);
            audit_fp = fopen(audit_log, "a");
            fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                    "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                    "Message Body: %s\n", gettime(), pthread_self(), item.client_fd, send_header.msg_type, send_header.msg_len, "");
            fclose(audit_fp);
            V(&audit_mutex);

            continue;
        }

        if(item.header.msg_type == RMSEND){
            /* Sent by the Client to the Server to send <message> to <chatroom>
             * If room not exist -> ERMNOTFOUND
             * If room exist, but user is not the participant of the room -> ERMDENIED (this should never happen using our client)
             * Upon successful send of the message to all users (not the sender) in roomname,
             * server responds to from_user "OK"
             * msg looks like  <roomname>\r\n<message>...
             * then, from server to clients joined the room
             *
             * RMSEND: <to_username>\r\n<message>
             * RMRECV: receive message from a user in room.
             * <roomname>\r\n<from_username>\r\n<message>
             */

            int from_user_fd = item.client_fd;
            char* from_username = find_name_by_fd(&users_list, from_user_fd);

            /* using msg body (format is <to_username>\r\n<msg> )
             * extract to_username, and pulling fd data by function
             * this to_user_fd will be used fot wd_msg function.
             */

            char* room_name = strtok_r(item.msg, "\r\n", &(item.msg));
            //printf("room_name: %s\n", room_name);

            /* This is room already existence check. there are same iterations
             * in many different function.
             * may need to determine on separating to helper function.
             */
            int roomexist = 0;
            P(&rooms_mutex);

            /* RM already exisit check.
             * this while-loop function is to check and flag.
             */

            node_t* current = rooms_list.head;
            while(current != NULL){
                // if the room name already exists in rooms_list
                if(strcmp(room_name, ((chat_room*)(current->value))->room_name) == 0){
                   roomexist = 1;
                   break;
                }
                current = current->next;
            }

            V(&rooms_mutex);

            /* Using the value processed above
             * determine whether RM exists or not
             */
            if(roomexist == 0){
                send_header.msg_len = 0;
                send_header.msg_type = ERMNOTFOUND;
                int retval = wr_msg(from_user_fd, &send_header, NULL);

                // update audit log
                P(&audit_mutex);
                audit_fp = fopen(audit_log, "a");
                fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                        "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                        "Message Body: %s\n", gettime(), pthread_self(), from_user_fd, send_header.msg_type, send_header.msg_len, "");
                fclose(audit_fp);
                V(&audit_mutex);
                continue;
            }

            /*
             * RM exists, but sender is not the participants of the RM.
             * send ERMDENIED to from_username
             * (this should never happen using our client. just in case for any future)
             * Because this doesn't happen as long as we use current client, so skip implementation if necessary.
             */

            char* msg_content = strchr(item.msg, '\n') + 1;
            int msg_len = strlen(room_name) + 2 + strlen(from_username) + 2 + strlen(msg_content) + 1;

            //printf("room_name, from_username, msg_content is %s %s %s\n", room_name, from_username, msg_content);

            /* making msg to send to room_name
             * <roomname>\r\n<from_username>\r\n<message>
             */

            char buf[msg_len];
            bzero(&buf, sizeof(buf));
            strcat(buf, room_name);
            strcat(buf, "\r\n");
            strcat(buf, from_username);
            strcat(buf, "\r\n");
            strcat(buf, msg_content);
            buf[strlen(buf)] = '\0';

            P(&rooms_mutex);
            // sending to from_username "OK" when successfully received
            send_header.msg_len = 0;
            send_header.msg_type = OK;
            int retval1 = wr_msg(from_user_fd, &send_header, NULL);

            // update audit log
            P(&audit_mutex);
            audit_fp = fopen(audit_log, "a");
            fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                    "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                    "Message Body: %s\n", gettime(), pthread_self(), from_user_fd, send_header.msg_type, send_header.msg_len, "");
            fclose(audit_fp);
            V(&audit_mutex);

            // update recv_header for to_username
            recv_header.msg_len = msg_len;
            recv_header.msg_type = RMRECV;
            V(&rooms_mutex);

            /* after retrieving the participants of the room
             * send the message to each fd except from_user_fd
             */
            chat_room* room_ptr = (chat_room*)(current->value);
            node_t* participant = room_ptr->participants->head;

            while(participant != NULL){
                if(strcmp(from_username, ((char*)(participant->value))) !=0){
                    // printf("participant: %s\n", ((char*)(participant->value)));
                    char* to_username = (char*)(participant->value);
                    int to_user_fd = find_fd_by_name(&users_list, to_username);
                    int retval = wr_msg(to_user_fd, &recv_header, buf);

                    // update audit log
                    P(&audit_mutex);
                    audit_fp = fopen(audit_log, "a");
                    fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                            "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                            "Message Body: %s\n", gettime(), pthread_self(), to_user_fd, recv_header.msg_type, recv_header.msg_len, buf);
                    fclose(audit_fp);
                    V(&audit_mutex);
                }
                participant = participant -> next;
            }
            continue;
        }

        if(item.header.msg_type == USRSEND){
            /* Attention
             *
             * WHen sending unknown user, server.c cannot
             * handle preventing xterm window opening
             * so, it should be handled once the window is opened.
             *
             * EUSRNOTFOUND: user does not exist on server
             * If user exist, the message template is
             * USERRECV: <from_username>\r\n<message>
             */
            int from_user_fd = item.client_fd;
            char* from_username = find_name_by_fd(&users_list, from_user_fd);

            /* using msg body (format is <to_username>\r\n<msg> )
             * extract to_username, and pulling fd data by function
             * this to_user_fd will be used fot wd_msg function.
             */

            char* to_username = strtok_r(item.msg, "\r\n", &(item.msg));
            // printf("to_username: %s\n", to_username);
            char* msg_content = strchr(item.msg, '\n') + 1;

            int to_user_fd = find_fd_by_name(&users_list, to_username);
            // printf("2. to_username: %s\n", to_username);
            // printf("2. to_user fd: %d\n", to_user_fd);

            if(to_user_fd == -1){
                send_header.msg_len = 0;
                send_header.msg_type = EUSRNOTFOUND;
                // printf("to_user_fd == -1 case\n");
                // printf("send_header.msg_len: %d \n", send_header.msg_len);
                // printf("send_header.msg_type: %d \n", send_header.msg_type);

                int retval = wr_msg(from_user_fd, &send_header, NULL);
                // printf("retval: %d\n", retval);

                // update audit log
                P(&audit_mutex);
                audit_fp = fopen(audit_log, "a");
                fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                        "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                        "Message Body: %s\n", gettime(), pthread_self(), from_user_fd, send_header.msg_type, send_header.msg_len, "");
                fclose(audit_fp);
                V(&audit_mutex);

                /* Need to check from prof or TA.
                 * Understood server (main, client, job thread)
                 * cannot prevent from opening xterm window
                 * nevertheless unknown user.
                 * However, when sending this EUSRNOTFOUND to from_user
                 * what happens? from_username get EUSRNOTFOUND msg header
                 * but xterm window still exist, and it is even possible to input
                 * text. (everytimg text is input, server generate this msg to )
                 * from_username. that's it.
                 */
                continue;
            }

            if(to_user_fd == from_user_fd){
                send_header.msg_len = 0;
                send_header.msg_type = ESERV;
                int retval = wr_msg(from_user_fd, &send_header, NULL);
                // printf("retval: %d\n", retval);

                // update audit log
                P(&audit_mutex);
                audit_fp = fopen(audit_log, "a");
                fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                        "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                        "Message Body: %s\n", gettime(), pthread_self(), from_user_fd, send_header.msg_type, send_header.msg_len, "");
                fclose(audit_fp);
                V(&audit_mutex);

                /* Same concern like EUSRNOTFOUND
                 * nothing we can do other than making server send this
                 * msg to from_username
                 */
                continue;
            }

            int msg_len = strlen(from_username) + 2 + strlen(msg_content)+1;
            // printf("3. msg_content: %s\n", msg_content);
            // printf("3. msg length: %d\n", msg_len);


            /* making msg to send to to_username */
            char buf[msg_len];
            bzero(&buf, sizeof(buf));
            strcat(buf, from_username);
            strcat(buf, "\r");
            strcat(buf, "\n");
            strcat(buf, msg_content);
            buf[strlen(buf)] = '\0';
            // sending to from_username "OK" when successfully received
            send_header.msg_len = 0;
            send_header.msg_type = OK;
            int retval1 = wr_msg(from_user_fd, &send_header, NULL);
            // printf("retval for wr_msg: %d\n", retval);

            // update audit log
            P(&audit_mutex);
            audit_fp = fopen(audit_log, "a");
            fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                    "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                    "Message Body: %s\n", gettime(), pthread_self(), from_user_fd, send_header.msg_type, send_header.msg_len, "");
            fclose(audit_fp);
            V(&audit_mutex);

            // update recv_header for to_username
            recv_header.msg_len = msg_len;
            recv_header.msg_type = USRRECV;

            int retval2 = wr_msg(to_user_fd, &recv_header, buf);
            //printf("buf for wr_msg: %s\n", buf);

            // update audit log
            P(&audit_mutex);
            audit_fp = fopen(audit_log, "a");
            fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                    "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                    "Message Body: %s\n", gettime(), pthread_self(), to_user_fd, recv_header.msg_type, recv_header.msg_len, buf);
            fclose(audit_fp);
            V(&audit_mutex);

            continue;
        }

        if(item.header.msg_type == USRLIST){
            int from_user_fd = item.client_fd;
            char* from_username = find_name_by_fd(&users_list, from_user_fd);
            char msg[1000];
            bzero(&msg, sizeof(msg));
            node_t* current = users_list.head;

            // if user is the only one in the users_list,
            // then send USRLIST with empty message
            if(users_list.length == 1){
                send_header.msg_len = 0;
                send_header.msg_type = USRLIST;
                wr_msg(from_user_fd, &send_header, NULL);

                // update audit log
                P(&audit_mutex);
                audit_fp = fopen(audit_log, "a");
                fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                        "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                        "Message Body: %s\n", gettime(), pthread_self(), from_user_fd, send_header.msg_type, send_header.msg_len, "");
                fclose(audit_fp);
                V(&audit_mutex);
                continue;
            }

            while(current != NULL){
                if(strcmp((char*)(current->value), from_username) == 0){
                    current = current->next;
                    continue;
                }
                strcat(msg, (char*)(current->value));
                strcat(msg, "\n");
                current = current -> next;
            }
            send_header.msg_len = strlen(msg) + 1;
            send_header.msg_type = USRLIST;

            // temprary for checking msg sending to client.
            // for(int i = 0; i < (strlen(msg)+10); i++){
            //     printf("i c and d is %c \t %d\n", msg[i], msg[i]);
            // }
            wr_msg(from_user_fd, &send_header, msg);

            // update audit log
            P(&audit_mutex);
            audit_fp = fopen(audit_log, "a");
            fprintf(audit_fp, "%s\tJob Thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                    "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                    "Message Body: %s\n", gettime(), pthread_self(), from_user_fd, send_header.msg_type, send_header.msg_len, msg);
            fclose(audit_fp);
            V(&audit_mutex);

            // bzero(&msg, sizeof(msg));
            // free(msg);
            // if ok, server returns list of users
            continue;
        }

    }
    /* Is it necessary? is it just because of void* function? */
    // return NULL;
}


void run_server(int server_port, int number_job_thread) {
    listen_fd = server_init(server_port); // Initiate server and start listening on specified port

    // update audit log
    P(&audit_mutex);
    audit_fp = fopen(audit_log, "a");
    
    fprintf(audit_fp, "\n%s\tServer initializes, listening on port: %d\n", gettime(), server_port);
    fclose(audit_fp);
    V(&audit_mutex);

    /* create new local variables. */

    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    // change type from int to socklen_t

    int retval = 0;
    int user_exist = 0;
    client_name = NULL;
    client_fd = NULL;
    pthread_t tid;

    petr_header login_header;
    petr_header reply_header;
    bzero(&login_header, sizeof(petr_header));
    bzero(&reply_header, sizeof(petr_header));

    /* initialize global shared resources. setup semaphore for user name */
    Sem_init(&users_mutex, 0, 1);
    Sem_init(&rooms_mutex, 0, 1);

    users_list.head = NULL;
    users_list.length = 0;

    rooms_list.head = NULL;
    rooms_list.head = 0;

    /* create an array of size N to hold all the job threads' tid */
    pthread_t job_thread_array[number_job_thread];

    /* create N job threads and store each job thread's tid into array
     * update audit log */
    P(&audit_mutex);
    audit_fp = fopen(audit_log, "a");
    for(int i = 0; i < number_job_thread; i++){
        pthread_create(&tid, NULL, job_thread, NULL);
        job_thread_array[i] = tid;
        //printf("create job thread: %ld\n", job_thread_array[i]);

        fprintf(audit_fp, "%s\tJob thread %ld created\n", gettime(), tid);
    }
    fclose(audit_fp);
    V(&audit_mutex);

    job_thread_array_ptr = job_thread_array;

    /* from accepting, user check */
    while (1) {
        user_exist = 0;
        client_name = malloc(BUFFER_SIZE);

        /* Wait and Accept the connection from client
         * in some other books, it says connfd because connected fd on server
         */
        client_fd = malloc(sizeof(int));
        *client_fd = accept(listen_fd, (SA *)&client_addr, &client_addr_len);

        /* Condition when connection is failed, check by return value of accept */
        if (*client_fd < 0) {
            printf("server acccept failed\n");
            exit(EXIT_FAILURE);

        } else {

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
            retval = read(*client_fd, client_name, login_header.msg_len);
            if (retval < 0) {
                printf("Receiving failed\n");
                exit(EXIT_FAILURE);
            }

            // update audit log
            P(&audit_mutex);
            audit_fp = fopen(audit_log, "a");
            fprintf(audit_fp, "%s\tMain thread receives login request from client\n", gettime());
            fclose(audit_fp);
            V(&audit_mutex);

            // check if the message type is LOGIN
            if(login_header.msg_type == LOGIN){
                printf("New client login\n");
                P(&users_mutex);

                /* check if the user name already exists
                 * users_list is List_t struct
                 */
                node_t* current = users_list.head;
                while(current != NULL){
                    /* send error message back to client if username already exists
                     */
                    if(strcmp( ((char*)(current->value)), client_name ) == 0){
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
                    free(client_name);
                    V(&users_mutex);
                    printf("User name already exists\n");
                    continue;
                }
                /* if username did not exist, add username to linked list. UNORDERED */
                insertRear(&users_list, (void*)client_name, *client_fd);

                // update audit log
                P(&audit_mutex);
                audit_fp = fopen(audit_log, "a");
                fprintf(audit_fp, "%s\tClient successfully logged in\n\t\t\t\t\t\t\t" \
                        "Username: %s,\tUserFD: %d\n", gettime(), client_name, *client_fd);
                fclose(audit_fp);
                V(&audit_mutex);

                V(&users_mutex);

                // write message back to client to confirm login
                reply_header.msg_len = 0;
                reply_header.msg_type = OK;
                retval = wr_msg(*client_fd, &reply_header, NULL);
                if (retval < 0) {
                    printf("Sending failed\n");
                    exit(EXIT_FAILURE);
                }

                // update audit log
                P(&audit_mutex);
                audit_fp = fopen(audit_log, "a");
                fprintf(audit_fp, "%s\tClient thread %ld sends message to client fd %d\n\t\t\t\t\t\t\t" \
                        "Header Type: 0x%x,\tHeader Length: %d\n\t\t\t\t\t\t\t"
                        "Message Body: %s\n", gettime(), pthread_self(), *client_fd, reply_header.msg_type, reply_header.msg_len, "");
                fclose(audit_fp);
                V(&audit_mutex);

                // after login successfully, create a client thread
                pthread_create(&tid, NULL, process_client, (void *)client_fd);

                // update audit log
                P(&audit_mutex);
                audit_fp = fopen(audit_log, "a");
                fprintf(audit_fp, "%s\tClient thread %ld created for user\n", gettime(), tid);
                fclose(audit_fp);
                V(&audit_mutex);
            }
        }
    }
    bzero(buffer, BUFFER_SIZE);
    close(listen_fd);
}

int main(int argc, char *argv[]) {
    int opt;
    N = 2;
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

    /* Install SIGINT handler */
    signal(SIGINT, sigint_handler);

    /* initialization of sbuf. It's job buffer.
     * After complete running run_server fuction
     * free it
     */
    sbuf_init(&job_buffer, NUMBER_JOB_BUF);

    /* initilization of audit mutex.
     * Open the audit log file for writing
     */
    Sem_init(&audit_mutex, 0, 1);

    /* run server with N number of job thread */
    run_server(port, N);

    /* After complete execution of run_server
     * deinitialization(free the buffer)
     */
    sbuf_deinit(&job_buffer);
    /* After finish execution, is it necessary to user pthread_join for job threads? */

    return 0;
}
