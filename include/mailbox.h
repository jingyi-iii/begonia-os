#ifndef MAILBOX_H
#define MAILBOX_H

#include "typedefs.h"
#include "list.h"
#include "spinlock.h"
#include "process.h"

#define MAIL_ANY_TID            (-0xcd)   /* receiver_tid wildcard = broadcast
                                           * MUST equal USER_MAIL_ANY_TID
                                           * (user/userlib.h) */
#define MAX_SUBSCRIPTION_COUNT  (16)
#define SYSCALL_CTRL_ID_MAILBOX (1)

typedef struct mail {
    u32 magic;
    int sender_tid;          /* informational: sender's thread (reply-to)  */
    int receiver_tid;        /* routing: target thread, or MAIL_ANY_TID for
                              * broadcast to subscribers of m->magic        */
    char data[256];
    u32 data_size;
} mail;

typedef struct mailbox {
    int                 owner_pid;
    int                 owner_tid;
    u32                 subscriptions[MAX_SUBSCRIPTION_COUNT];
    spinlock            sp_lock;
    list_node           mails;
    list_node           inflight_node;
    wait_queue          waiters;
} mailbox;

void        mailbox_syscall_init        (void);
void        mailbox_syscall_exit        (void);
mail*       mailbox_alloc_mail          (void);
void        mailbox_release_mail        (mail* m);
mailbox*    mailbox_alloc               (int owner_pid, int owner_tid);
int         mailbox_release             (mailbox* mb);
int         mailbox_send                (mail* m);
mail*       mailbox_listen              (mailbox* mb);
int         mailbox_subscribe_mail      (mailbox* mb, u32 magic);
int         mailbox_unsubscribe_mail    (mailbox* mb, u32 magic);

#endif
