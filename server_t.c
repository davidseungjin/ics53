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

/* 5: general purpose?: for check at JOB THREAD */
sem_t jobjob;


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




/* Handler to clean up in case of "Ctrl-C" */
void sigint_handler(int sig) {
    printf("shutting down server\n");

    P(&audit_mutex);
    audit_fp = fopen(audit_log, "a");
    fprintf(audit_fp, "%ld\tServer terminated due to Ctrl-C", time(NULL));
    fclose(audit_fp);
    V(&audit_mutex);

    close(listen_fd);
    sbuf_deinit(&job_buffer);
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
            printf("Reading message header failed\n");
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

        if(recv_header.msg_len > 0){
            retval = read(client_fd, buffer, recv_header.msg_len);
            if (retval <=  0) {
                printf("Reading message body failed\n");
                break;
            }
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
    Sem_init(&jobjob, 0, 1);
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
        printf("job_thread: item.header.msg_type %d\n", item.header.msg_type);
        printf("job_thread: item.header.msg_len %d\n", item.header.msg_len);


        /* declare mssage header for sending via petr protocaol */
        petr_header send_header;
        send_header.msg_len = 0;
        send_header.msg_type = OK;

        petr_header recv1_header;
        recv1_header.msg_len = 0;
        recv1_header.msg_type = OK;

        /* Later, the message should be customized.
         * It is just for checking xterm window work or not.
         * According to msg type. Server needs to react differently.
         * If server react differently, xterm window will occur accordingly.
         * uncomment out below for testing.
         */
        // wr_msg(item.client_fd, &send_header, NULL);


        // ESERV: generic error

        if(item.header.msg_type == LOGOUT){

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
                char* room_name = malloc(1000);
                strcpy(room_name, item.msg);

                char* user = find_name_by_fd(&users_list, item.client_fd);
                char* user_name = malloc(1000);
                strcpy(user_name, user);

                int user_fd = item.client_fd;

                // create new linked list for the partcipants in the room
                List_t* participants = malloc(sizeof(List_t));
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

                // (testing) printing all room info in rooms_list
                current = rooms_list.head;
                while(current != NULL){
                    printf("room_name: %s\n", ((chat_room*)(current->value))->room_name);
                    printf("room_creater: %s\n", ((chat_room*)(current->value))->room_creater);
                    node_t* participant = ((chat_room*)(current->value))->participants->head;
                    // (testing) priting all participants in the room
                    // while(participant != NULL){
                    //     printf("partcipant: %s\n", (char*)(participant->value));
                    //     participant = participant->next;
                    // }
                    current = current->next;
                }

                V(&rooms_mutex);

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
                            }
                            free(participant->value);
                            participant = participant->next;
                        }

                        // free every node in participants list
                        deleteList(room_ptr->participants);
                        // free everything inside the room structure
                        free(room_ptr->room_name);
                        free(room_ptr->room_creater);
                        free(room_ptr->participants);
                        // est the flag for room remove
                        roomremove = 1;

                    // if the room exist, but user is not the creater
                    }else{
                        // send ERMDENIED message to user
                        send_header.msg_len = 0;
                        send_header.msg_type = ERMDENIED;
                        wr_msg(item.client_fd, &send_header, NULL);
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
            }
            V(&rooms_mutex);

            continue;
        }

        if(item.header.msg_type == RMLIST){
            // return list of rooms w/ users per room
            // <roomname>:<username>,...,<username>\n...


            /* THIS IS ONLY FOR DEBUGGING
               Printing all room info in rooms_list,
               and all participants info in each room.
             */

            printf("\nPrinting Room List\n");

            node_t* room = rooms_list.head;
            while(room != NULL){
                printf("room_name: %s\n", ((chat_room*)(room->value))->room_name);
                printf("room_creater: %s\n", ((chat_room*)(room->value))->room_creater);
                node_t* participant = ((chat_room*)(room->value))->participants->head;
                while(participant != NULL){
                    printf("participant: %s\n", (char*)(participant->value));
                    participant = participant->next;
                }
                room = room->next;
            }

            send_header.msg_len = 0;
            send_header.msg_type = RMLIST;
            wr_msg(item.client_fd, &send_header, NULL);

            continue;
        }

        if(item.header.msg_type == RMDELETE){
            // ERMNOTFOUND: room does not exist on server
            // ERMDENIED: anyone except created can not RMDELETE

            // RMCLOSED to client?

            continue;
        }

        if(item.header.msg_type == RMJOIN){
            // set up local variables
            int roomexist = 0;
            char* room_name = item.msg;
            int user_fd = item.client_fd;

            char* user = find_name_by_fd(&users_list, item.client_fd);
            char* user_name = malloc(1000);
            strcpy(user_name, user);


            P(&rooms_mutex);
            // check if the room exist or not
            node_t* current = rooms_list.head;
            while(current != NULL){
                // if the room name exists in rooms_list
                if(strcmp(room_name, ((chat_room*)(current->value))->room_name) == 0){
                    // add user to the room's participant linked list
                    List_t* members_list = ((chat_room*)(current->value))->participants;
                    insertRear(members_list, (void*)user_name, user_fd);
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

            continue;
        }

        if(item.header.msg_type == RMLEAVE){
            // ERMNOTFOUND: room does not exist on server
            // ERMDENIED: room creater can not leave

            continue;
        }

        if(item.header.msg_type == RMSEND){
            // ERMNOTFOUND: room does not exist on server

            // msg looks like  <roomname>\r\n<message>...
            // then, from server to clients joined the room
            // RMRECV: <roomname>\r\n<from_username>\r\n<message>

            continue;
        }

        if(item.header.msg_type == USRSEND){
            // EUSRNOTFOUND: user does not exist on server
            //
            // then, from server to user who receives
            // USERRECV: <from_username>\r\n<message>

            int from_user_fd = item.client_fd;
            char* from_username = find_name_by_fd(&users_list, from_user_fd);
            printf("from_username and fd are %s, %d\n", from_username, from_user_fd);

            char* temp = item.msg;
            printf("item.msg is %s\n", item.msg);
            printf("job_thread: item.msg is %s\n", item.msg);
            // char* rest = test1;
            // printf("1. %s\n", test1);

            char* to_username = strtok_r(temp, "\r\n", &temp);
            //printf("2. to_username: %s\n", to_username);

            int to_user_fd = find_fd_by_name(&users_list, to_username);
            //printf("2. to_user fd: %d\n", to_user_fd);

            char* msg_content = strtok_r(temp, "\r\n", &temp);
            //printf("3. msg_content: %s\n", msg_content);

            int msg_len = strlen(to_username) + 2 + strlen(msg_content)+2;
            //printf("3. msg length: %d\n", msg_len);

            char buf[msg_len];
            bzero(&buf, sizeof(buf));

            strcat(buf, from_username);
            strcat(buf, "\r\n");
            strcat(buf, msg_content);
            strcat(buf, "\r\n");

            //bzero(example, sizeof(example));
            //printf("4. concatenated: %s\n", buf);

            /* sending to from_username "OK" when successfully received */
            send_header.msg_len = 0;
            send_header.msg_type = OK;

            wr_msg(from_user_fd, &send_header, NULL);

            /* update recv_header for to_username */
            recv1_header.msg_len = msg_len;
            recv1_header.msg_type = USRRECV;

            int retval = wr_msg(to_user_fd, &recv1_header, buf);
            // printf("retval for wr_msg: %d\n", retval);

            continue;
        }

        if(item.header.msg_type == USRLIST){

            // if ok, server returns list of users
            continue;
        }

    }
    /* Is it necessary? is it just because of void* function? */
    return NULL;
}


void run_server(int server_port, int number_job_thread) {
    listen_fd = server_init(server_port); // Initiate server and start listening on specified port

    // update audit log
    P(&audit_mutex);
    audit_fp = fopen(audit_log, "a");
    fprintf(audit_fp, "\n%ld\tInitializes server, listening on port: %d\n", time(NULL), server_port);
    fclose(audit_fp);
    V(&audit_mutex);

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
    sem_init(&users_mutex, 0, 1);
    sem_init(&rooms_mutex, 0, 1);

    users_list.head = NULL;
    users_list.length = 0;

    rooms_list.head = NULL;
    rooms_list.head = 0;

    /* create N job threads and update audit log */
    P(&audit_mutex);
    audit_fp = fopen(audit_log, "a");
    for(int i = 0; i < number_job_thread; i++){
        pthread_create(&tid, NULL, job_thread, NULL);
        fprintf(audit_fp, "%ld\tCreate job thread #%d\n", time(NULL), i);
    }
    fclose(audit_fp);
    V(&audit_mutex);

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

            // update audit log
            P(&audit_mutex);
            audit_fp = fopen(audit_log, "a");
            fprintf(audit_fp, "%ld\tReceive message from client on main thread\n\t\t\t" \
                              "msg_type: 0x%x\tmsg_body: %s\n", time(NULL),login_header.msg_type, user_name); 
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
                     * is it necessary to use strcmp or strncmp instead of logical
                     * operator equal ?
                     */
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

                // update audit log
                P(&audit_mutex);
                audit_fp = fopen(audit_log, "a");
                fprintf(audit_fp, "%ld\tClient successfully logged in\n\t\t\t" \
                        "Username: %s\tUserFD: %d\n", time(NULL), user_name, *client_fd);
                fclose(audit_fp);
                V(&audit_mutex);

                // (for testing) print out the linked list
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

                // update audit log
                P(&audit_mutex);
                audit_fp = fopen(audit_log, "a");
                fprintf(audit_fp, "%ld\tCreate client thread for user\n", time(NULL));
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
    return 0;
}
