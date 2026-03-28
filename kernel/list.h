/*
 * Anykernel OS — Generic Intrusive Linked List
 *
 * Linux-style intrusive doubly-linked list. Each node is embedded
 * in the containing struct. No heap allocation required.
 *
 * Usage:
 *   struct my_item { int data; struct list_node node; };
 *   struct list_head head = LIST_HEAD_INIT(head);
 *   list_push(&head, &item.node);
 */

#ifndef LIST_H
#define LIST_H

#include <stddef.h>

/* Node embedded in each list item */
struct list_node {
    struct list_node *next;
    struct list_node *prev;
};

/* List head (sentinel) */
struct list_head {
    struct list_node sentinel;
};

/* Static initializer */
#define LIST_HEAD_INIT(name) { .sentinel = { &(name).sentinel, &(name).sentinel } }

/* Runtime init */
static inline void list_head_init(struct list_head *head) {
    head->sentinel.next = &head->sentinel;
    head->sentinel.prev = &head->sentinel;
}

/* Check if list is empty */
static inline int list_empty(struct list_head *head) {
    return head->sentinel.next == &head->sentinel;
}

/* Insert node after prev */
static inline void list_insert_after(struct list_node *prev, struct list_node *node) {
    node->next = prev->next;
    node->prev = prev;
    prev->next->prev = node;
    prev->next = node;
}

/* Push to front of list */
static inline void list_push_front(struct list_head *head, struct list_node *node) {
    list_insert_after(&head->sentinel, node);
}

/* Push to back of list */
static inline void list_push_back(struct list_head *head, struct list_node *node) {
    list_insert_after(head->sentinel.prev, node);
}

/* Remove a node */
static inline void list_remove_node(struct list_node *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node;
    node->prev = node;
}

/* Get containing struct from node pointer */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* Iterate over list entries */
#define list_for_each(pos, head) \
    for (pos = (head)->sentinel.next; pos != &(head)->sentinel; pos = pos->next)

/* Safe iteration (allows removal during iteration) */
#define list_for_each_safe(pos, tmp, head) \
    for (pos = (head)->sentinel.next, tmp = pos->next; \
         pos != &(head)->sentinel; \
         pos = tmp, tmp = pos->next)

#endif /* LIST_H */
