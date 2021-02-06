#ifndef MEDIAD_H
#define MEDIAD_H

#include <pthread.h>
#include <signal.h>
#include <mntent.h>
#include <limits.h>
#include <syslog.h>

#define CONFIGFILE			"/etc/mediad/mediad.conf"
#define SOCKNAME			"/dev/.mediad"
#define SOCKLOCK			"/dev/.mediad.lock"
#define ETC_FSTAB			"/etc/fstab"
#define ETC_MTAB			"/etc/mtab"

#define DEF_FSOPTIONS		"nosuid,nodev"
#define DEF_AUTOFS_EXP_FREQ	2
#define DEF_AUTOFS_TIMEOUT	4
#define MAX_IDS				32
#define MAX_ALIASES			16

typedef enum {
	MWH_DEVNAME,
	MWH_MTABDEVNAME,
	MWH_VENDOR,
	MWH_MODEL,
	MWH_SERIAL,
	MWH_PARTITION,
	MWH_FSTYPE,
	MWH_UUID,
	MWH_LABEL,
} matchwhat_t;

typedef enum {
	MOP_EQ,
	MOP_NE,
} matchop_t;

typedef enum {
	WAT_NONSPEC = 1,
	WAT_FSSPEC = 2,
	WAT_ALL = 3,
} whatalias_t;

typedef enum {
	CCS_UNKNOWN,
	CCS_NONE,
	CCS_CDROM,
	CCS_FLOPPY,
} check_change_t;

typedef enum {
	MOPT_NO_AUTOMOUNT = 1,
} mntoption_flag_t;

typedef struct _mcond {
	struct _mcond  *next;
	matchwhat_t    what;
	matchop_t      op;
	const char     *value;
} mcond_t;

typedef struct _fsoptions {
	struct _fsoptions *next;
	mcond_t        *cond;
	unsigned       prio;
	const char     *options;
} fsoptions_t;

typedef struct _mntoptions {
	struct _mntoptions *next;
	mcond_t        *cond;
	unsigned       prio;
	unsigned       options;
} mntoptions_t;

typedef struct _alias {
	struct _alias  *next;
	mcond_t        *cond;
	const char     *alias;
} alias_t;

typedef struct _alist {
	struct _alist  *next;
	const char     *name;
	const char     *created;
	unsigned       flags;
} alist_t;

/* alist flags: */
#define AF_FSSPEC 1
#define AF_PERM   2
#define AF_OLD    4

typedef struct _mnt {
	struct _mnt     *next;
	struct _mnt     *parent;
	unsigned		n_children;
	pthread_mutex_t lock;
	const char      *dev;
	const char		*devpath;
	const char      *dir;
	unsigned		partition;
	const char      *type;
	const char      *vendor;
	const char      *model;
	const char      *serial;
	const char      *uuid;
	const char      *label;
	alist_t         *aliases;
	unsigned        medium_present : 1;
	unsigned        medium_changed : 1;
	unsigned        mounted : 1;
	unsigned        suppress_message : 1;
	unsigned        no_automount : 1;
	check_change_t  check_change_strategy;
	int             check_change_param;
} mnt_t;

typedef struct _config {
	unsigned int  expire_freq;
	unsigned long expire_timeout;
	unsigned char blink_led;
	unsigned debug            : 1;
	unsigned no_scan_fstab    : 1;
	unsigned no_model_alias   : 1;
	unsigned no_label_alias   : 1;
	unsigned no_label_unique  : 1;
	unsigned uuid_alias       : 1;
	unsigned hide_device_name : 1;
} config_t;

typedef struct _mntent_list {
	struct _mntent_list *next;
	struct mntent       ent;
	char                buf[512];
} mntent_list_t;

#define fatal(str,args...)		do { logit(LOG_ERR, str, ## args); exit(1); } while(0)
#define error(str,args...)		do { logit(LOG_ERR, str, ## args); } while(0)
#define warning(str,args...)	do { logit(LOG_WARNING, str, ## args); } while(0)
/* make them a higher level to make them visible in a console window... */
#define msg(str,args...)		do { logit(LOG_WARNING, str, ## args); } while(0)
#define lpmsg(str,args...)		do { logit(LOG_INFO, str, ## args); } while(0)
#define debug(str,args...)											\
	do {															\
		if (config.debug)											\
			logit(LOG_INFO, "{%u} " str, pthread_self(), ## args);	\
	} while(0)

static __inline__ int streq(const char *a, const char *b) {
	return strcmp(a, b) == 0;
}
static __inline__ int strcaseeq(const char *a, const char *b) {
	return strcasecmp(a, b) == 0;
}
static __inline__ const char *strprefix(const char *a, const char *b) {
	return (strncmp(a, b, strlen(b)) == 0) ? a+strlen(b) : NULL;
}

/* daemon.c */
extern struct udev *udev;
extern pthread_attr_t thread_detached;
extern sigset_t termsigs;
extern int volatile shutting_down;
extern int used_sigs[];
int do_mount(const char *name);
int do_umount(const char *name);
void add_mount_with_devpath(const char *devname, const char *devpath);
int daemon_main(void);

/* autofs.c */
extern const char *autodir;
void inc_mounted(void);
void dec_mounted(void);
void start_automount(const char *dir);
void prepare_stop_automount(void);
void stop_automount(const char *dir);

/* changed.c */
int no_medium_errno(void);
int check_medium(const char *dev);
void check_medium_change(mnt_t *m);
void set_no_medium_present(mnt_t *m);

/* coldplug.c */
void coldplug(void);

/* config.c */
extern config_t config;
void read_config(void);

/* mount.c */
void add_fstype_replace(const char *from, const char *to);
void purge_fstype_replace(void);
int call_mount(const char *dev, const char *path,
			   const char *type, const char *options);

/* fsoptions.c */
void add_fsoptions(mcond_t *cond, const char *opts);
const char *find_fsoptions(mnt_t *m);
void purge_fsoptions(void);
void parse_mount_options(const char *_opts, int *iopts, const char **sopts);
void add_mntoptions(mcond_t *cond, unsigned options);
unsigned find_mntoptions(mnt_t *m);
void purge_mntoptions(void);

/* aliases.c */
void add_alias(mcond_t *cond, const char *alias);
void purge_aliases(void);
void match_aliases(mnt_t *m, int fsspec_only, unsigned flags);
void mark_aliases(mnt_t *m, unsigned mask, unsigned flags, unsigned newflags);
void mnt_add_alias(mnt_t *m, const char *a, unsigned flags);
void mnt_free_aliases(mnt_t *m, unsigned mask, unsigned flags);
void mk_aliases(mnt_t *m, whatalias_t fsspec);
void rm_aliases(mnt_t *m, whatalias_t fsspec);
void mnt_add_model_alias(mnt_t *m);
void mnt_add_label_alias(mnt_t *m, unsigned flags);
void mnt_add_uuid_alias(mnt_t *m, unsigned flags);

/* mcond.c */
mcond_t *new_mcond(matchwhat_t what, matchop_t op, const char *value);
void free_mcond(mcond_t *p);
int match_mcond(mcond_t *cond, mnt_t *m, int *fsspec);
unsigned mcond_prio(mcond_t *cond);

/* mtab.c */
void add_mtab(const char *dev, const char *dir,
			  const char *fstype, const char *options);
void rm_mtab(const char *dir);

/* util.c */
void logit(int pri, const char *fmt, ...);
void *xmalloc(size_t sz);
void *xrealloc(void *p, size_t sz);
char *xstrdup(const char *str);
void xfree(const char **p);
char *mkpath(char *buf, const char *add);
size_t is_name_eq_val(const char *str);
const char *getid(unsigned n, const char **ids, const char *what);
void parse_id(mnt_t *m, const char *line);
void replace_untrusted_chars(char *p);
void get_dev_infos(mnt_t *m);
void find_devpath(mnt_t *m);
int find_by_property(const char *propname, const char *propval, char *outname, size_t outsize);
void mk_dir(mnt_t *m);
void rm_dir(mnt_t *m);
void send_num(int fd, unsigned num);
void send_str(int fd, const char *str);
unsigned recv_num(int fd);
void recv_str(int fd, char **p);
unsigned int linux_version_code(void);

#endif /* MEDIAD_H */
