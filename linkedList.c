#ifndef LINKEDLIST_H
#define LINKEDLIST_H

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
#include <string.h>

#define INT_MODE 0
#define STR_MODE 1

/*
 * Structre for each node of the linkedList
 *
 * value - a pointer to the data of the node. 
 * next - a pointer to the next node in the list. 
 */
typedef struct node {
    void* value;
    int fd;
    struct node* next;
} node_t;

/*
 * Structure for the base linkedList
 * 
 * head - a pointer to the first node in the list. NULL if length is 0.
 * length - the current length of the linkedList. Must be initialized to 0.
 * comparator - function pointer to linkedList comparator. Must be initialized!
 */
typedef struct list {
    node_t* head;
    int length;
    /* the comparator uses the values of the nodes directly (i.e function has to be type aware) */
    int (*comparator)(void*, void*);
} List_t;

/* 
 * Each of these functions inserts the reference to the data (valref)
 * into the linkedList list at the specified position
 *
 * @param list pointer to the linkedList struct
 * @param valref pointer to the data to insert into the linkedList
 */
void insertRear(List_t* list, void* valref, int fd);
void insertFront(List_t* list, void* valref, int fd);
void insertInOrder(List_t* list, void* valref, int fd);

/*
 * Each of these functions removes a single linkedList node from
 * the LinkedList at the specfied function position.
 * @param list pointer to the linkedList struct
 * @return a pointer to the removed list node
 */ 
void* removeFront(List_t* list);
void* removeRear(List_t* list);
void* removeByIndex(List_t* list, int n);

/* 
 * Free all nodes from the linkedList
 *
 * @param list pointer to the linkedList struct
 */
void deleteList(List_t* list);

/*
 * Traverse the list printing each node in the current order.
 * @param list pointer to the linkedList strut
 * @param mode STR_MODE to print node.value as a string,
 * INT_MODE to print node.value as an int
 */
void printList(List_t* list, char mode);

/* find fd by name */
int find_fd_by_name(List_t* list, char* name);
char* find_name_by_fd(List_t* list, int fd);

#endif
/*
    What is a linked list?
    A linked list is a set of dynamically allocated nodes, arranged in
    such a way that each node contains one value and one pointer.
    The pointer always points to the next member of the list.
    If the pointer is NULL, then it is the last node in the list.

    A linked list is held using a local pointer variable which
    points to the first item of the list. If that pointer is also NULL,
    then the list is considered to be empty.
    -----------------------------              -------------------------------
    |              |             |            \ |              |             |
    |     DATA     |     NEXT    |--------------|     DATA     |     NEXT    |
    |              |             |            / |              |             |
    ------------------------------              ------------------------------
*/
/*
int main(){

    List_t* mylist = (List_t*)malloc(sizeof(list_t));
    insertFront(mylist, "myval 1");
    insertFront(mylist, "myval 2");
    insertFront(mylist, "myval 3");
    insertFront(mylist, "myval 4");

    insertRear(mylist, "insert rear 1");
    insertRear(mylist, "target");
    insertRear(mylist, "insert rear 2");

    printf("print list\n");
    printf("list length: %d\n",mylist->length);
*/

int find_fd_by_name(List_t* list, char* name){
    node_t* ptr = list->head;
    // for(ptr; ptr != NULL; ptr = ptr->next)
    //     printf("node: %s\n", (char*)(ptr->value));

    // char mytarget[] = "target";
    for(ptr = list->head; ptr != NULL; ptr = ptr->next){
        if(strcmp(ptr->value, name)==0){
            printf("found\n");
            // printf("it's next val is %s\n", ptr->next->value);
            return ptr->fd;
        }
    }
    printf("find_fd_by_name function: fd not found in the list\n");
    return -1;
}

char* find_name_by_fd(List_t* list, int fd){
    node_t* ptr = list->head;

    for(ptr = list->head; ptr != NULL; ptr = ptr->next){
        if(ptr->fd == fd){
            printf("found\n");
            // printf("it's next val is %s\n", ptr->next->value);
            return ptr->value;
        }
    }
    printf("find_name_by_fd function: name not found in the list\n");
    return NULL;
}

void insertFront(List_t* list, void* valref, int fd) {
    if (list->length == 0)
        list->head = NULL;

    node_t** head = &(list->head);
    node_t* new_node;
    new_node = malloc(sizeof(node_t));

    new_node->value = valref;
    new_node->fd = fd;
    new_node->next = *head;
    *head = new_node;
    list->length++; 
}

void insertRear(List_t* list, void* valref, int fd) {
    if (list->length == 0) {
        insertFront(list, valref, fd);
        return;
    }

    node_t* head = list->head;
    node_t* current = head;
    while (current->next != NULL) {
        current = current->next;
    }

    current->next = malloc(sizeof(node_t));
    current->next->value = valref;
    current->next->fd = fd;
    current->next->next = NULL;
    list->length++;
}

void insertInOrder(List_t* list, void* valref, int fd) {
    if (list->length == 0) {
        insertFront(list, valref, fd);
        return;
    }

    node_t** head = &(list->head);
    node_t* new_node;
    new_node = malloc(sizeof(node_t));
    new_node->value = valref;
    new_node->fd = fd;
    new_node->next = NULL;

    if (list->comparator(new_node->value, (*head)->value) <= 0) {
        new_node->next = *head;
        *head = new_node;
    } 
    else if ((*head)->next == NULL){ 
        (*head)->next = new_node;
    }                                
    else {
        node_t* prev = *head;
        node_t* current = prev->next;
        while (current != NULL) {
            if (list->comparator(new_node->value, current->value) > 0) {
                if (current->next != NULL) {
                    prev = current;
                    current = current->next;
                } else {
                    current->next = new_node;
                    break;
                }
            } else {
                prev->next = new_node;
                new_node->next = current;
                break;
            }
        }
    }
    list->length++;
}

void* removeFront(List_t* list) {
    node_t** head = &(list->head);
    void* retval = NULL;
    node_t* next_node = NULL;

    if (list->length == 0) {
        return NULL;
    }

    next_node = (*head)->next;
    retval = (*head)->value;
    list->length--;

    node_t* temp = *head;
    *head = next_node;
    free(temp);

    return retval;
}

void* removeRear(List_t* list) {
    if (list->length == 0) {
        return NULL;
    } else if (list->length == 1) {
        return removeFront(list);
    }

    void* retval = NULL;
    node_t* head = list->head;
    node_t* current = head;

    while (current->next->next != NULL) { 
        current = current->next;
    }

    retval = current->next->value;
    free(current->next);
    current->next = NULL;

    list->length--;

    return retval;
}

/* indexed by 0 */
void* removeByIndex(List_t* list, int index) {
    if (list->length <= index) {
        return NULL;
    }

    node_t** head = &(list->head);
    void* retval = NULL;
    node_t* current = *head;
    node_t* prev = NULL;
    int i = 0;

    if (index == 0) {
        retval = (*head)->value;
        
		node_t* temp = *head;
        *head = current->next;
        free(temp);
        
		list->length--;
        return retval;
    }

    while (i++ != index) {
        prev = current;
        current = current->next;
    }

    prev->next = current->next;
    retval = current->value;
    free(current);

    list->length--;

    return retval;
}

/*
void* removeByvalue(List_t* list, (void*) value) {
    if (list->length == 0) {
        return NULL;
    }

    node_t* temp = (node_t*)malloc(sizeof(node_t));
    temp = list->head;
    for(temp; temp != NULL; temp = temp->next){
        if(strcmp(temp->next->value, value)==0)
            break;
    }
    temp->next



    list->length--;

    return retval;
}
*/

void deleteList(List_t* list) {
    if (list->length == 0)
        return;
    while (list->head != NULL){
        removeFront(list);
    }
    list->length = 0;
}
