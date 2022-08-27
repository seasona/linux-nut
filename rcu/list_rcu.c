#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/rculist.h>
#include <linux/spinlock.h>
#include <linux/preempt.h>

struct book {
	int id;
	char name[64];
	char author[64];
	int borrow;		/* we could use atomic_t */
	struct list_head node;
	struct rcu_head rcu;
};

struct library {
	struct list_head books;
	spinlock_t lock;
};

static struct library my_library;

static void book_reclaim_callback(struct rcu_head *rcu)
{
	struct book *b = container_of(rcu, struct book, rcu);

	pr_info("callback free:%llx, preempt_count=%d\n",
		(unsigned long long)b, preempt_count());
	kfree(b);
}

static int add_book(int id, const char *name, const char *author)
{
	struct book *new_book;

	new_book = kzalloc(sizeof(*new_book), GFP_KERNEL);
	if (!new_book) {
		pr_err("Allocate memory for new_book failed. %s\n", __func__);
		return -ENOMEM;
	}

	new_book->id = id;
	strncpy(new_book->name, name, sizeof(new_book->name));
	strncpy(new_book->author, author, sizeof(new_book->author));
	new_book->borrow = 1;

	/* We must use other locker for a rcu writer */
	spin_lock(&my_library.lock);
	list_add_rcu(&new_book->node, &my_library.books);
	spin_unlock(&my_library.lock);

	return 0;
}

static int update_book(int id, int async, int borrow)
{
	struct book *b = NULL;
	struct book *new_b = NULL;
	struct book *old_b = NULL;

	rcu_read_lock();
	list_for_each_entry_rcu(b, &my_library.books, node) {
		if (b->id == id) {
			old_b = b;
			break;
		}
	}

	if (!old_b) {
		rcu_read_unlock();
		pr_err("Didn't find book, id=%d\n", id);
		return -EINVAL;
	}

	if (old_b->borrow == borrow) {
		rcu_read_unlock();
		pr_err("This book is borrowed/not borrowed");
		return -EINVAL;
	}

	new_b = kzalloc(sizeof(*new_b), GFP_KERNEL);
	if (!new_b) {
		rcu_read_unlock();
		pr_err("Allocate memory for new_book failed. %s\n", __func__);
		return -ENOMEM;
	}

	memcpy(new_b, old_b, sizeof(*old_b));
	new_b->borrow = borrow;

	spin_lock(&my_library.lock);
	list_replace_rcu(&old_b->node, &new_b->node);
	spin_unlock(&my_library.lock);

	pr_info("book id=%d update success(%d->%d), preempt_count=%d\n",
		id, old_b->borrow, borrow, preempt_count());

	rcu_read_unlock();

	if (async) {
		call_rcu(&old_b->rcu, book_reclaim_callback);
	} else {
		synchronize_rcu();
		kfree(old_b);
	}
	return 0;
}

static int book_is_borrow(int id)
{
	struct book *b;

	rcu_read_lock();
	list_for_each_entry_rcu(b, &my_library.books, node) {
		if (b->id == id) {
			rcu_read_unlock();
			return b->borrow;
		}
	}
	rcu_read_unlock();

	pr_err("book id=%d is not exist\n", id);

	return -1;
}

static int delete_book(int id, int async)
{
	struct book *b;

	spin_lock(&my_library.lock);
	list_for_each_entry_rcu(b, &my_library.books, node) {
		if (b->id == id) {
			list_del_rcu(&b->node);
			spin_unlock(&my_library.lock);

			if (async) {
				call_rcu(&b->rcu, book_reclaim_callback);
			} else {
				synchronize_rcu();
				kfree(b);
			}

			return 0;
		}
	}
	spin_unlock(&my_library.lock);

	return -1;
}

static void print_book(int id)
{
	struct book *b;

	rcu_read_lock();
	list_for_each_entry_rcu(b, &my_library.books, node) {
		if (b->id == id) {
			pr_info("id=%d, name=%s, author=%s, borrow=%d\n",
				b->id, b->name, b->author, b->borrow);
			rcu_read_unlock();
			return;
		}
	}
	rcu_read_unlock();

	pr_err("book id=%d is not exist\n", id);
}

static void test_example(int async)
{
	add_book(0, "A journey of linux kernel", "Tom Hoter");
	add_book(1, "Inside Linux Kernel", "Steve Jobs");

	print_book(0);
	print_book(1);

	pr_info("book1 borrow:%d\n", book_is_borrow(0));
	pr_info("book2 borrow:%d\n", book_is_borrow(1));

	update_book(0, async, 0);
	update_book(1, async, 0);

	print_book(0);
	print_book(1);

	update_book(0, async, 1);
	update_book(1, async, 1);

	print_book(0);
	print_book(1);

	delete_book(0, async);
	delete_book(1, async);

	print_book(0);
	print_book(1);
}

static int list_rcu_init(void)
{
	spin_lock_init(&my_library.lock);
	INIT_LIST_HEAD(&my_library.books);

	test_example(0);
	test_example(1);

	return 0;
}

static void list_rcu_exit(void)
{
	return;
}

module_init(list_rcu_init);
module_exit(list_rcu_exit);
MODULE_LICENSE("GPL");
