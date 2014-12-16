/*
 * Copyright (C) 2014 Karel Zak <kzak@redhat.com>
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 */

/**
 * SECTION: monitor
 * @title: Monitor
 * @short_description: interface to monitor mount tables
 *
 */

#include "fileutils.h"
#include "mountP.h"

#include <sys/inotify.h>
#include <sys/epoll.h>


enum {
	MNT_MONITOR_TYPE_NONE	= 0,
	MNT_MONITOR_TYPE_USERSPACE
};

struct monitor_opers;

struct monitor_entry {
	int			fd;		/* private entry file descriptor */
	char			*path;		/* path to the monitored file */
	int			type;		/* MNT_MONITOR_TYPE_* */

	const struct monitor_opers *opers;

	unsigned int		enable : 1;

	struct list_head	ents;
};

struct libmnt_monitor {
	int			refcount;
	int			fd;		/* public monitor file descriptor */

	struct list_head	ents;
};

struct monitor_opers {
	int (*op_get_fd)(struct libmnt_monitor *, struct monitor_entry *);
	int (*op_verify_change)(struct libmnt_monitor *, struct monitor_entry *);
};

static int monitor_enable_entry(struct libmnt_monitor *mn,
				struct monitor_entry *me, int enable);

/**
 * mnt_new_monitor:
 *
 * The initial refcount is 1, and needs to be decremented to
 * release the resources of the filesystem.
 *
 * Returns: newly allocated struct libmnt_monitor.
 */
struct libmnt_monitor *mnt_new_monitor(void)
{
	struct libmnt_monitor *mn = calloc(1, sizeof(*mn));
	if (!mn)
		return NULL;

	mn->refcount = 1;
	mn->fd = -1;
	INIT_LIST_HEAD(&mn->ents);

	DBG(MONITOR, ul_debugobj(mn, "alloc"));
	return mn;
}

/**
 * mnt_ref_monitor:
 * @mn: monitor pointer
 *
 * Increments reference counter.
 */
void mnt_ref_monitor(struct libmnt_monitor *mn)
{
	if (mn)
		mn->refcount++;
}

static void free_monitor_entry(struct monitor_entry *me)
{
	if (!me)
		return;
	list_del(&me->ents);
	if (me->fd >= 0)
		close(me->fd);
	free(me->path);
	free(me);
}

/**
 * mnt_unref_monitor:
 * @mn: monitor pointer
 *
 * De-increments reference counter, on zero the @mn is automatically
 * deallocated.
 */
void mnt_unref_monitor(struct libmnt_monitor *mn)
{
	if (!mn)
		return;

	mn->refcount--;
	if (mn->refcount <= 0) {
		if (mn->fd >= 0)
			close(mn->fd);

		while (!list_empty(&mn->ents)) {
			struct monitor_entry *me = list_entry(mn->ents.next,
						  struct monitor_entry, ents);
			free_monitor_entry(me);
		}

		free(mn);
	}
}

static struct monitor_entry *monitor_new_entry(struct libmnt_monitor *mn)
{
	struct monitor_entry *me;

	assert(mn);

	me = calloc(1, sizeof(*me));
	if (!me)
		return NULL;
        INIT_LIST_HEAD(&me->ents);
	list_add_tail(&me->ents, &mn->ents);

	me->fd = -1;

	return me;
}

static int monitor_next_entry(struct libmnt_monitor *mn,
			      struct libmnt_iter *itr,
			      struct monitor_entry **me)
{
	int rc = 1;

	assert(mn);
	assert(itr);
	assert(me);

	if (!mn || !itr || !me)
		return -EINVAL;

	*me = NULL;

	if (!itr->head)
		MNT_ITER_INIT(itr, &mn->ents);
	if (itr->p != itr->head) {
		MNT_ITER_ITERATE(itr, *me, struct monitor_entry, ents);
		rc = 0;
	}

	return rc;
}

static struct monitor_entry *monitor_get_entry(struct libmnt_monitor *mn, int type)
{
	struct libmnt_iter itr;
	struct monitor_entry *me;

	mnt_reset_iter(&itr, MNT_ITER_FORWARD);
	while (monitor_next_entry(mn, &itr, &me) == 0) {
		if (me->type == type)
			return me;
	}
	return NULL;
}


/*
 * Userspace monitor
 */

static int userspace_monitor_get_fd(struct libmnt_monitor *mn,
				    struct monitor_entry *me)
{
	int wd, rc;
	char *dirname, *sep;

	assert(mn);
	assert(me);

	if (!me || me->enable == 0)	/* not-initialized or disabled */
		return -EINVAL;
	if (me->fd >= 0)
		return me->fd;		/* already initialized */

	assert(me->path);
	DBG(MONITOR, ul_debugobj(mn, " open userspace monitor for %s", me->path));

	dirname = me->path;
	sep = stripoff_last_component(dirname);	/* add \0 between dir/filename */

	/* make sure the directory exists */
	rc = mkdir(dirname, S_IWUSR|
			    S_IRUSR|S_IRGRP|S_IROTH|
			    S_IXUSR|S_IXGRP|S_IXOTH);
	if (rc && errno != EEXIST)
		goto err;

	/* initialize inotify stuff */
	me->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (me->fd < 0)
		goto err;

	/*
	 * libmount uses rename(2) to atomically update utab/mtab, the finame
	 * change is possible to detect by IN_MOVE_TO inotify event.
	 */
	wd = inotify_add_watch(me->fd, dirname, IN_MOVED_TO);
	if (wd < 0)
		goto err;

	if (sep && sep > dirname)
		*(sep - 1) = '/';		/* set '/' back to the path */

	return me->fd;
err:
	DBG(MONITOR, ul_debugobj(mn, "failed to create userspace monitor [rc=%d]", rc));
	return -errno;
}

static int userspace_monitor_verify_change(struct libmnt_monitor *mn,
					   struct monitor_entry *me)
{
	char wanted[NAME_MAX + 1];
	char buf[sizeof(struct inotify_event) + NAME_MAX + 1];
	struct inotify_event *event;
	char *p;
	ssize_t r;
	int rc = 0;

	DBG(MONITOR, ul_debugobj(mn, "checking fd=%d for userspace changes", me->fd));

	p = strrchr(me->path, '/');
	if (!p)
		p = me->path;
	else
		p++;
	strncpy(wanted, p, sizeof(wanted) - 1);
	wanted[sizeof(wanted) - 1] = '\0';
	rc = 0;

	while ((r = read(me->fd, buf, sizeof(buf))) > 0) {
		for (p = buf; p < buf + r; ) {
			event = (struct inotify_event *) p;

			if (strcmp(event->name, wanted) == 0)
				rc = 1;
			p += sizeof(struct inotify_event) + event->len;
		}
		if (rc)
			break;
	}

	return rc;
}

static const struct monitor_opers userspace_opers = {
	.op_get_fd		= userspace_monitor_get_fd,
	.op_verify_change	= userspace_monitor_verify_change
};


/**
 * mnt_monitor_enable_userspace:
 * @mn: monitor
 * @enable: 0 or 1
 * @filename: overwrites default
 *
 * Enables or disables userspace monitor. If the monitor does not exist and
 * enable=1 then allocates new resources necessary for the monitor.
 *
 * If high-level monitor has been already initialized (by mnt_monitor_get_fd()
 * or mnt_wait_monitor()) then it's updated according to @enable.
 *
 * The @filename is used only first time when you enable the monitor. It's
 * impossible to have more than one userspace monitor.
 *
 * Return: 0 on success and <0 on error
 */
int mnt_monitor_enable_userspace(struct libmnt_monitor *mn, int enable, const char *filename)
{
	struct monitor_entry *me;
	int rc = 0;

	if (!mn)
		return -EINVAL;

	me = monitor_get_entry(mn, MNT_MONITOR_TYPE_USERSPACE);
	if (me) {
		rc = monitor_enable_entry(mn, me, enable);
		if (!enable && me->fd) {
			close(me->fd);		/* disable inotify notification */
			me->fd = -1;
		}
		return rc;
	}
	if (!enable)
		return 0;

	DBG(MONITOR, ul_debugobj(mn, "allocate new userspace monitor"));

	/* create a new entry */
	if (!mnt_has_regular_mtab(&filename, NULL))	/* /etc/mtab */
		filename = mnt_get_utab_path();		/* /run/mount/utab */
	if (!filename) {
		DBG(MONITOR, ul_debugobj(mn, "failed to get userspace mount table path"));
		return -EINVAL;
	}

	me = monitor_new_entry(mn);
	if (!me)
		goto err;

	me->type = MNT_MONITOR_TYPE_USERSPACE;
	me->opers = &userspace_opers;
	me->path = strdup(filename);
	if (!me->path)
		goto err;

	return monitor_enable_entry(mn, me, 1);
err:
	rc = -errno;
	free_monitor_entry(me);
	DBG(MONITOR, ul_debugobj(mn, "failed to allocate userspace monitor [rc=%d]", rc));
	return rc;
}


static int monitor_enable_entry(struct libmnt_monitor *mn,
				struct monitor_entry *me, int enable)
{
	assert(mn);
	assert(me);

	me->enable = enable ? 1 : 0;

	/* TODO : remove / add me->fd to high-level*/
	return 0;
}

int mnt_monitor_close_fd(struct libmnt_monitor *mn)
{
	if (mn && mn->fd >= 0) {
		close(mn->fd);
		mn->fd = -1;
	}

	return 0;
}

int mnt_monitor_get_fd(struct libmnt_monitor *mn)
{
	struct libmnt_iter itr;
	struct monitor_entry *me;
	int rc = 0;

	if (!mn)
		return -EINVAL;
	if (mn->fd >= 0)
		return mn->fd;

	DBG(MONITOR, ul_debugobj(mn, "create top-level monitor fd"));
	mn->fd = epoll_create1(EPOLL_CLOEXEC);
	if (mn->fd < 0)
		return -errno;

	mnt_reset_iter(&itr, MNT_ITER_FORWARD);

	DBG(MONITOR, ul_debugobj(mn, "adding monitor entries to epoll (fd=%d)", mn->fd));
	while (monitor_next_entry(mn, &itr, &me) == 0) {
		int fd;
		struct epoll_event ev = { .events = EPOLLPRI | EPOLLIN };

		if (!me->enable)
			continue;

		fd = me->opers->op_get_fd(mn, me);
		if (fd < 0)
			goto err;

		DBG(MONITOR, ul_debugobj(mn, " add fd=%d (for %s)", fd, me->path));

		ev.data.ptr = (void *) me;
		if (epoll_ctl(mn->fd, EPOLL_CTL_ADD, fd, &ev) < 0)
			goto err;
	}

	DBG(MONITOR, ul_debugobj(mn, "successfully created monitor"));
	return mn->fd;
err:
	rc = errno ? -errno : -EINVAL;
	close(mn->fd);
	mn->fd = -1;
	DBG(MONITOR, ul_debugobj(mn, "failed to create monitor [rc=%d]", rc));
	return rc;
}

int mnt_monitor_next_changed(struct libmnt_monitor *mn,
			     const char **filename,
			     int *type)
{
	int rc;

	if (!mn || mn->fd < 0)
		return -EINVAL;

	do {
		struct monitor_entry *me;
		struct epoll_event events[1];

		rc = epoll_wait(mn->fd, events, 1, 0);
		if (rc < 0)
			return -errno;		/* error */
		if (rc == 0)
			return 1;		/* nothing */

		me = (struct monitor_entry *) events[0].data.ptr;
		if (!me)
			continue;

		if (me->opers->op_verify_change != NULL &&
		    me->opers->op_verify_change(mn, me) != 1)
			continue;		/* false positive */

		if (filename)
			*filename = me->path;
		if (type)
			*type = me->type;
		return 0;
	} while (1);

	return 0;
}

#ifdef TEST_PROGRAM

/* monitor @fd by epoll */
static int my_epoll(struct libmnt_monitor *mn, int fd)
{
	int efd = -1, rc = -1;
	struct epoll_event ev;

	assert(mn);
	assert(fd >= 0);

	efd = epoll_create1(EPOLL_CLOEXEC);
	if (efd < 0) {
		warn("failed to create epoll");
		goto done;
	}

	ev.events = EPOLLPRI | EPOLLIN;
	ev.data.fd = fd;

	rc = epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
	if (rc < 0) {
		warn("failed to add fd to epoll");
		goto done;
	}

	printf("waiting for changes...\n");
	do {
		const char *filename = NULL;
		struct epoll_event events[1];
		int n = epoll_wait(efd, events, 1, -1);

		if (n < 0) {
			rc = -errno;
			warn("polling error");
			goto done;
		}
		if (n == 0 || events[0].data.fd != fd)
			continue;

		while (mnt_monitor_next_changed(mn, &filename, NULL) == 0)
			printf("%s: change detected\n", filename);
	} while (1);

	rc = 0;
done:
	if (efd >= 0)
		close(efd);
	return rc;
}

/*
 * create a monitor and add the monitor fd to epoll
 */
int test_epoll(struct libmnt_test *ts, int argc, char *argv[])
{
	struct libmnt_monitor *mn;
	int i, fd, rc = -1;

	mn = mnt_new_monitor();
	if (!mn) {
		warn("failed to allocate monitor");
		goto done;
	}

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "userspace") == 0) {
			if (mnt_monitor_enable_userspace(mn, TRUE, NULL)) {
				warn("failed to initialize userspace monitor");
				goto done;
			}
		}
	}

	fd = mnt_monitor_get_fd(mn);
	if (fd < 0) {
		warn("failed to initialize monitor fd");
		goto done;
	}

	rc = my_epoll(mn, fd);
done:
	printf("done");
	mnt_unref_monitor(mn);
	return rc;
}

int main(int argc, char *argv[])
{
	struct libmnt_test tss[] = {
		{ "--epoll", test_epoll, "<userspace kernel ...>  test monitor in epoll" },
		{ NULL }
	};

	return mnt_run_test(tss, argc, argv);
}

#endif /* TEST_PROGRAM */
