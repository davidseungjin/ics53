#include "server.h"
#include "protocol.h"
#include "linkedList.h"
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>

#define USAGE_MSG   "./bin/petr_server [-h][-j N] PORT_NUMBER AUDIT_FILENAME\n\n" \
                    "  -h               Display this help menu, and returns EXIT_SUCCESS.\n" \
                    "  -j N             Number of job threads. Default to 2.\n" \
                    "  AUDIT_FILENAME   File to output Audit Log message to.\n" \
                    "  PORT_NUMBER      Port number to listen on.\n"


char buffer[BUFFER_SIZE];
int listen_fd;

// created global shared resources
sem_t users_mutex;
List_t users_list;

void sigint_handler(int sig) {
    printf("shutting down server\n");
    close(listen_fd);
    exit(0);
}

int server_init(int server_port) {
    int sockfd;
    struct sockaddr_in servaddr;

    // socket create and verification
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        printf("Socket creation failed...\n");
        exit(EXIT_FAILURE);
    } else {
        printf("Socket successfully created\n");
    }

    bzero(&servaddr, sizeof(servaddr));

    // assign IP, PORT
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(server_port);

    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, (char *)&opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // Binding newly created socket to given IP and verification
    if ((bind(sockfd, (SA *)&servaddr, sizeof(servaddr))) != 0) {
        printf("Socket bind failed\n");
        exit(EXIT_FAILURE);
    } else {
        printf("Socket successfully binded\n");
    }

    // Now server is ready to listen and verification
    if ((listen(sockfd, 1)) != 0) {
        printf("Listen failed\n");
        exit(EXIT_FAILURE);
    } else
        printf("Currently listening on port %d.\n", server_port);

    return sockfd;
}

//Function running in client thread
void *process_client(void *clientfd_ptr) {

    pthread_detach(pthread_self());

    int client_fd = *(int *)clientfd_ptr;
    free(clientfd_ptr);

    int retval;

    petr_header recv_header;

    petr_header send_header;
    send_header.msg_len = 0;
    send_header.msg_type = OK;

    while (1) {

        retval = rd_msgheader(client_fd, &recv_header);
        if (retval < 0) {
            printf("Receiving failed\n");
            break;
        }

        bzero(buffer, BUFFER_SIZE);
        retval = read(client_fd, buffer, recv_header.msg_len);
        if (retval <=  0) {
            printf("Receiving failed\n");
            break;
        }

        // print buffer which contains the client message
        printf("\nReceive message from client\n");
        printf("Type: 0x%x\n", recv_header.msg_type);
        printf("Message: %s\n\n", buffer);

        // insert message/job into job buffer
        // to be continued..

    }

    // Close the socket at the end
    printf("Close one client connection\n");
    close(client_fd);
    return NULL;
}

void run_server(int server_port) {
    listen_fd = server_init(server_port); // Initiate server and start listening on specified port

    // create new local variables;
    int client_fd;
    struct sockaddr_in client_addr;
    int client_addr_len = sizeof(client_addr);

    int retval = 0;
    int user_exist = 0;
    char* user_name = NULL;
    pthread_t tid;

    petr_header login_header;
    petr_header reply_header;

    // initialize global shared resources
    sem_init(&users_mutex, 0, 1);

    users_list.head = NULL;
    users_list.length = 0;

    // create N job threads
    // to be continued..

    while (1) {
        user_exist = 0;
        user_name = malloc(sizeof(char));
        // Wait and Accept the connection from client
        int *client_fd = malloc(sizeof(int));
        *client_fd = accept(listen_fd, (SA *)&client_addr, (socklen_t*)&client_addr_len);

        if (*client_fd < 0) {
            printf("server acccept failed\n");
            exit(EXIT_FAILURE);

        // accept now connection
        } else {
            printf("New client connetion accepted\n");
            // read the message header from the new client
            retval = rd_msgheader(*client_fd, &login_header);
            if (retval < 0) {
                printf("Receiving failed\n");
                exit(EXIT_FAILURE);
            }

            // read the message body (username) from the new client
            bzero(buffer, BUFFER_SIZE);
            retval = read(*client_fd, user_name, login_header.msg_len);
            if (retval < 0) {
                printf("Receiving failed\n");
                exit(EXIT_FAILURE);
            }

            // check if the message type is LOGIN
            if(login_header.msg_type == LOGIN){
                printf("New client login\n");
                sem_wait(&users_mutex);

                // check if the user name already exists
                node_t* current = users_list.head;
                while(current != NULL){
                    // send error message back to client if username already exists
                    if(*((char*)(current->value)) == *user_name){
                        user_exist = 1;
                        reply_header.msg_len = 0;
                        reply_header.msg_type = EUSREXISTS;
                        retval = wr_msg(*client_fd, &reply_header, NULL);
                        if (retval < 0) {
                            printf("Sending failed\n");
                            exit(EXIT_FAILURE);
                        }
                        close(*client_fd);
                        break;
                    }
                    current = current->next;
                }
                // continue and wait for new client if user name already exists
                if(user_exist == 1){
                    free(user_name);
                    sem_post(&users_mutex);
                    printf("User name already exists\n");
                    continue;
                }
                // if username did not exist, add username to linked list
                insertRear(&users_list, (void*)user_name);

                // (for testing) print out the linked list
                current = users_list.head;
                while(current != NULL){
                    printf("user: %s\n", (char*)(current->value));
                    current = current->next;
                }

                sem_post(&users_mutex);

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
    return;
}

int main(int argc, char *argv[]) {
    int opt;
    int N = 2;
    unsigned int port = 0;
    char* audit = NULL;

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
    audit = argv[optind+1];

    run_server(port);

    return 0;
}
