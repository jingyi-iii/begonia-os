#include "mailbox.h"
#include "heap.h"
#include "string.h"
#include "errno.h"
#include "irq.h"
#include "syscall.h"
#include "cpu.h"
#include "log.h"

enum mailbox_ctrl_cmd {
    MAILBOX_CTRL_SEND = 0,
    MAILBOX_CTRL_LISTEN,
    MAILBOX_CTRL_ALLOC_MAIL,
    MAILBOX_CTRL_RELEASE_MAIL,
    MAILBOX_CTRL_ALLOC,
    MAILBOX_CTRL_RELEASE,
    MAILBOX_CTRL_SUBSCRIBE_MAIL,
    MAILBOX_CTRL_UNSUBSCRIBE_MAIL,
};

typedef struct mailmeta {
    u32         unique_id;
    mail*       payload;
    list_node   this_node;    // attach to mailbox->mails list
    list_node   inflight_node;
} mailmeta;

typedef struct mailbox_ctrl_config {
    u8           cmd;
    mail*        m;          /* in: mail to send / out: received mail from listen / alloc_mail */
    mailbox*     mb;         /* in: target mailbox / out: allocated mailbox */
    int          pid;        /* in: owner pid for mailbox_alloc */
    int          tid;        /* in: owner tid for mailbox_alloc */
    u32          magic;      /* in: magic for subscription */
    int          ret;        /* out: return value */
} mailbox_ctrl_config;

static LIST_HEAD(inflight_mails);
static LIST_HEAD(inflight_mailboxs);
static DECLARE_SPINLOCK(inflight_lock);

static inline int may_run_directly(void)
{
    return is_user_mode() == 0;
}

static mailmeta* get_mailmeta_locked(mail* m)
{
    if (!m)
        return 0;

    list_for_each(pos, &inflight_mails) {
        mailmeta* meta = list_entry(pos, mailmeta, inflight_node);
        if (meta->payload == m)
            return meta;
    }

    return 0;
}

static mail* alloc_mail_locked(void)
{
    static u32 unique_id = 0;

    mail* m = (mail*)malloc(sizeof(mail));
    if (!m)
        return 0;

    mailmeta* meta = (mailmeta*)malloc(sizeof(mailmeta));
    if (!meta) {
        free(m);
        return 0;
    }

    memset(m, 0, sizeof(mail));
    meta->payload = m;

    meta->unique_id = __sync_fetch_and_add(&unique_id, 1);
    list_init(&meta->this_node);
    list_init(&meta->inflight_node);
    list_add(&meta->inflight_node, &inflight_mails);

    return m;
}

static void release_mail_locked(mail* m)
{
    if (!m)
        return;

    mailmeta* meta = get_mailmeta_locked(m);
    if (!meta)
        return;

    list_del(&meta->inflight_node);
    memset(meta, 0, sizeof(mailmeta));
    free(meta);
    memset(m, 0, sizeof(mail));
    free(m);
}


static int send_mail_locked(mailbox* mb, mail* m)
{
    if (!mb || !m)
        return E_INVAL;

    u32 eflags = spinlock_lock_irqsave(&mb->sp_lock);
    mailmeta* meta = get_mailmeta_locked(m);
    if (!meta) {
        spinlock_unlock_irqrestore(&mb->sp_lock, eflags);
        release_mail_locked(m);
        return E_INVAL;
    }
    list_add(&meta->this_node, &mb->mails);
    spinlock_unlock_irqrestore(&mb->sp_lock, eflags);
    thread_wake_one(&mb->waiters);

    return 0;
}

static int mailbox_exec(void* arg)
{
    if (!arg)
        return E_INVAL;

    mailbox_ctrl_config* config = (mailbox_ctrl_config*)arg;
    switch (config->cmd) {
    case MAILBOX_CTRL_SEND:
        return mailbox_send(config->m);
    case MAILBOX_CTRL_LISTEN:
        config->m = mailbox_listen(config->mb);
        return config->m ? 0 : E_NOTFOUND;
    case MAILBOX_CTRL_ALLOC_MAIL:
        config->m = mailbox_alloc_mail();
        return config->m ? 0 : E_NOMEM;
    case MAILBOX_CTRL_RELEASE_MAIL:
        mailbox_release_mail(config->m);
        return 0;
    case MAILBOX_CTRL_ALLOC:
        config->mb = mailbox_alloc(config->pid, config->tid);
        return config->mb ? 0 : E_NOMEM;
    case MAILBOX_CTRL_RELEASE:
        return mailbox_release(config->mb);
    case MAILBOX_CTRL_SUBSCRIBE_MAIL:
        return mailbox_subscribe_mail(config->mb, config->magic);
    case MAILBOX_CTRL_UNSUBSCRIBE_MAIL:
        return mailbox_unsubscribe_mail(config->mb, config->magic);
    default:
        config->ret = E_INVAL;
        break;
    }

    return config->ret;
}

mail* mailbox_alloc_mail(void)
{
    if (!may_run_directly()) {
        mailbox_ctrl_config config = {0};
        config.cmd = MAILBOX_CTRL_ALLOC_MAIL;
        syscall(SYSCALL_CTRL_ID_MAILBOX, &config, sizeof(config));
        return config.m;
    }

    u32 eflags = spinlock_lock_irqsave(&inflight_lock);
    mail* m = alloc_mail_locked();
    spinlock_unlock_irqrestore(&inflight_lock, eflags);

    return m;
}

void mailbox_release_mail(mail* m)
{
    if (!m)
        return;

    if (!may_run_directly()) {
        mailbox_ctrl_config config = {0};
        config.m = m;
        config.cmd = MAILBOX_CTRL_RELEASE_MAIL;
        syscall(SYSCALL_CTRL_ID_MAILBOX, &config, sizeof(config));
        return;
    }

    u32 eflags = spinlock_lock_irqsave(&inflight_lock);
    release_mail_locked(m);
    spinlock_unlock_irqrestore(&inflight_lock, eflags);
}

mailbox* mailbox_alloc(int owner_pid, int owner_tid)
{
    if (!may_run_directly()) {
        mailbox_ctrl_config config = {0};
        config.cmd = MAILBOX_CTRL_ALLOC;
        config.pid = owner_pid;
        config.tid = owner_tid;
        syscall(SYSCALL_CTRL_ID_MAILBOX, &config, sizeof(config));
        return config.mb;
    }

    mailbox* mb = (mailbox*)malloc(sizeof(mailbox));
    if (!mb)
        return 0;

    memset(mb, 0, sizeof(mailbox));
    mb->owner_pid = owner_pid;
    mb->owner_tid = owner_tid;
    spinlock_init(&mb->sp_lock);
    list_init(&mb->mails);
    wait_queue_init(&mb->waiters);

    u32 eflags = spinlock_lock_irqsave(&inflight_lock);
    list_add(&mb->inflight_node, &inflight_mailboxs);
    spinlock_unlock_irqrestore(&inflight_lock, eflags);

    return mb;
}

int mailbox_release(mailbox* mb)
{
    if (!mb)
        return E_INVAL;

    if (!may_run_directly()) {
        mailbox_ctrl_config config = {0};
        config.cmd = MAILBOX_CTRL_RELEASE;
        config.mb = mb;
        return syscall(SYSCALL_CTRL_ID_MAILBOX, &config, sizeof(config));
    }

    u32 eflags = spinlock_lock_irqsave(&inflight_lock);
    u32 inner_eflags = spinlock_lock_irqsave(&mb->sp_lock);
    if (thread_has_waiters(&mb->waiters)) {
        spinlock_unlock_irqrestore(&mb->sp_lock, inner_eflags);
        spinlock_unlock_irqrestore(&inflight_lock, eflags);
        return E_BUSY;
    }

    while (!list_empty(&mb->mails)) {
        mailmeta* meta = list_entry(mb->mails.prev, mailmeta, this_node);
        list_del(&meta->this_node);
        release_mail_locked(meta->payload);
    }
    spinlock_unlock_irqrestore(&mb->sp_lock, inner_eflags);

    list_del(&mb->inflight_node);
    spinlock_unlock_irqrestore(&inflight_lock, eflags);

    memset(mb, 0, sizeof(mailbox));
    free(mb);
    return 0;
}

int mailbox_send(mail* m)
{
    if (!m)
        return E_INVAL;

    if (!may_run_directly()) {
        mailbox_ctrl_config config = {0};
        config.cmd = MAILBOX_CTRL_SEND;
        config.m = m;
        return syscall(SYSCALL_CTRL_ID_MAILBOX, &config, sizeof(config));
    }

    int ret = 0;
    u32 eflags = 0;
    if (m->receiver_tid == MAIL_ANY_TID) {
        u32 eflags = spinlock_lock_irqsave(&inflight_lock);
        if (list_empty(&inflight_mailboxs)) {
            spinlock_unlock_irqrestore(&inflight_lock, eflags);
            release_mail_locked(m);
            return 0;
        }

        mailmeta* meta = get_mailmeta_locked(m);
        if (!meta) {
            spinlock_unlock_irqrestore(&inflight_lock, eflags);
            return E_INVAL;
        }

        list_for_each(node, &inflight_mailboxs) {
            mailbox* mb = list_entry(node, mailbox, inflight_node);

            int has_subscribed = 0;
            u32 inner_eflags = spinlock_lock_irqsave(&mb->sp_lock);
            for (int i = 0; i < MAX_SUBSCRIPTION_COUNT; i++) {
                if (mb->subscriptions[i] == m->magic) {
                    has_subscribed = 1;
                    break;
                }
            }
            spinlock_unlock_irqrestore(&mb->sp_lock, inner_eflags);

            if (!has_subscribed)
                continue;

            mail* clone = alloc_mail_locked();
            if (clone) {
                clone->magic        = m->magic;
                clone->sender_tid   = m->sender_tid;
                clone->receiver_tid = m->receiver_tid;
                clone->data_size    = m->data_size;
                memcpy(clone->data, m->data, sizeof(m->data));

                u32 inner_eflags = spinlock_lock_irqsave(&mb->sp_lock);
                mailmeta* clone_meta = get_mailmeta_locked(clone);
                if (clone_meta)
                    list_add(&clone_meta->this_node, &mb->mails);
                spinlock_unlock_irqrestore(&mb->sp_lock, inner_eflags);

                thread_wake_one(&mb->waiters);
            }
        }

        spinlock_unlock_irqrestore(&inflight_lock, eflags);

        /* Release the sender's reference */
        mailbox_release_mail(m);
    } else {
        eflags = spinlock_lock_irqsave(&inflight_lock);
        mailbox* mb = 0;
        list_for_each(node, &inflight_mailboxs) {
            mb = list_entry(node, mailbox, inflight_node);
            if (m->receiver_tid == mb->owner_tid)
                break;
            mb = 0;
        }

        if (!mb) {
            release_mail_locked(m);
            spinlock_unlock_irqrestore(&inflight_lock, eflags);
            return E_NOTFOUND;
        }

        ret = send_mail_locked(mb, m);
        spinlock_unlock_irqrestore(&inflight_lock, eflags);
    }

    return ret;
}

mail* mailbox_listen(mailbox* mb)
{
    if (!mb) {
        log_warn("%s: invalid mailbox\n", __func__);
        return 0;
    }

    if (!may_run_directly()) {
        mailbox_ctrl_config config = {0};
        config.cmd = MAILBOX_CTRL_LISTEN;
        config.mb = mb;
        syscall(SYSCALL_CTRL_ID_MAILBOX, &config, sizeof(config));
        return config.m;
    }

    for ( ;; ) {
        u32 eflags = spinlock_lock_irqsave(&mb->sp_lock);
        if (!list_empty(&mb->mails)) {
            mailmeta* meta = list_entry(mb->mails.prev, mailmeta, this_node);
            list_del(&meta->this_node);
            spinlock_unlock_irqrestore(&mb->sp_lock, eflags);
            return meta->payload;
        } else {
            thread_prepare_wait(&mb->waiters);
        }
        spinlock_unlock_irqrestore(&mb->sp_lock, eflags);
        
        int ret = thread_wait();
        /* maybe no other threads to run, return */
        if (ret < 0)
            return 0;
    }
}

int mailbox_subscribe_mail(mailbox* mb, u32 magic)
{
    if (!mb)
        return E_INVAL;

    if (!may_run_directly()) {
        mailbox_ctrl_config config = {0};
        config.cmd = MAILBOX_CTRL_SUBSCRIBE_MAIL;
        config.mb = mb;
        config.magic = magic;
        return syscall(SYSCALL_CTRL_ID_MAILBOX, &config, sizeof(config));
    }

    u32 eflags = spinlock_lock_irqsave(&mb->sp_lock);
    for (int i = 0; i < MAX_SUBSCRIPTION_COUNT; i++) {
        if (mb->subscriptions[i] == magic) {
            spinlock_unlock_irqrestore(&mb->sp_lock, eflags);
            return 0;
        }
    }

    for (int i = 0; i < MAX_SUBSCRIPTION_COUNT; i++) {
        if (mb->subscriptions[i] == 0) {
            mb->subscriptions[i] = magic;
            spinlock_unlock_irqrestore(&mb->sp_lock, eflags);
            return 0;
        }
    }
    spinlock_unlock_irqrestore(&mb->sp_lock, eflags);
    return E_NOMEM;
}

int mailbox_unsubscribe_mail(mailbox* mb, u32 magic)
{
    if (!mb)
        return E_INVAL;

    if (!may_run_directly()) {
        mailbox_ctrl_config config = {0};
        config.cmd = MAILBOX_CTRL_UNSUBSCRIBE_MAIL;
        config.mb = mb;
        config.magic = magic;
        return syscall(SYSCALL_CTRL_ID_MAILBOX, &config, sizeof(config));
    }

    u32 eflags = spinlock_lock_irqsave(&mb->sp_lock);
    for (int i = 0; i < MAX_SUBSCRIPTION_COUNT; i++) {
        if (mb->subscriptions[i] == magic) {
            mb->subscriptions[i] = 0;
            spinlock_unlock_irqrestore(&mb->sp_lock, eflags);
            return 0;
        }
    }
    spinlock_unlock_irqrestore(&mb->sp_lock, eflags);
    return E_INVAL;
}

void mailbox_syscall_init(void)
{
    syscall_register(SYSCALL_CTRL_ID_MAILBOX,
        mailbox_exec, sizeof(mailbox_ctrl_config));
}

void mailbox_syscall_exit(void)
{
    syscall_unregister(SYSCALL_CTRL_ID_MAILBOX);
}