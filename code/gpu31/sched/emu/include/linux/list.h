/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Host emulation of the kernel list API used by the dma-fence/resv/
 * syncobj/sched backport shims. Synchronous, single-threaded, no
 * allocation: queue_work runs inline (deterministic dispatch).
 */
#ifndef _SCHED_EMU_LIST_H
#define _SCHED_EMU_LIST_H

#include <stddef.h>

struct list_head {
	struct list_head *next, *prev;
};

#define LIST_HEAD(name) \
	struct list_head name = { &(name), &(name) }

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline int list_empty(const struct list_head *head)
{
	return head->next == head;
}

static inline void __list_add(struct list_head *new,
			      struct list_head *prev,
			      struct list_head *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

static inline void list_add_tail(struct list_head *new,
				 struct list_head *head)
{
	__list_add(new, head->prev, head);
}

static inline void __list_del(struct list_head *prev,
			      struct list_head *next)
{
	next->prev = prev;
	prev->next = next;
}

static inline void list_del_init(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	INIT_LIST_HEAD(entry);
}

#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

#define list_entry(ptr, type, member) \
	container_of(ptr, type, member)

#define list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)

#define list_for_each_entry(pos, head, member)				\
	for (pos = list_entry((head)->next, __typeof__(*pos), member);	\
	     &(pos)->member != (head);					\
	     pos = list_entry((pos)->member.next, __typeof__(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member)			\
	for (pos = list_entry((head)->next, __typeof__(*pos), member),	\
		n = list_entry((pos)->member.next, __typeof__(*pos), member); \
	     &(pos)->member != (head);					\
	     pos = n,							\
		n = list_entry((n)->member.next, __typeof__(*n), member))

static inline void list_splice_init(struct list_head *list,
				    struct list_head *head)
{
	struct list_head *first, *last;

	if (list_empty(list))
		return;
	first = list->next;
	last = list->prev;
	first->prev = head;
	last->next = head->next;
	head->next->prev = last;
	head->next = first;
	INIT_LIST_HEAD(list);
}

#endif /* _SCHED_EMU_LIST_H */
