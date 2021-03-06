#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include <syscall.h>
#include <syscall_nums.h>

DEFN_SYSCALL1(exit,  0, int);
DEFN_SYSCALL1(print, 1, const char *);
DEFN_SYSCALL2(gettimeofday, 6, void *, void *);
DEFN_SYSCALL3(execve, 7, char *, char **, char **);
DEFN_SYSCALL1(sbrk, 10, int);
DEFN_SYSCALL0(getgraphicsaddress, 11);
DEFN_SYSCALL1(setgraphicsoffset, 16, int);
DEFN_SYSCALL1(wait, 17, unsigned int);
DEFN_SYSCALL0(getgraphicswidth,  18);
DEFN_SYSCALL0(getgraphicsheight, 19);
DEFN_SYSCALL0(getgraphicsdepth,  20);
DEFN_SYSCALL0(mkpipe, 21);
DEFN_SYSCALL1(kernel_string_XXX, 25, char *);
DEFN_SYSCALL0(reboot, 26);
DEFN_SYSCALL3(readdir, 27, int, int, void *);
DEFN_SYSCALL3(clone, 30, uintptr_t, uintptr_t, void *);
DEFN_SYSCALL0(mousedevice, 33);
DEFN_SYSCALL2(mkdir, 34, char *, unsigned int);
DEFN_SYSCALL2(shm_obtain, 35, char *, size_t *);
DEFN_SYSCALL1(shm_release, 36, char *);
DEFN_SYSCALL2(share_fd, 39, int, int);
DEFN_SYSCALL1(get_fd, 40, int);
DEFN_SYSCALL0(gettid, 41);
DEFN_SYSCALL2(system_function, 43, int, char **);
DEFN_SYSCALL1(open_serial, 44, int);
DEFN_SYSCALL2(sleepabs,  45, unsigned long, unsigned long);
DEFN_SYSCALL3(ioctl, 47, int, int, void *);
DEFN_SYSCALL2(access, 48, char *, int);
DEFN_SYSCALL2(stat, 49, char *, void *);
DEFN_SYSCALL3(waitpid, 53, int, int *, int);
DEFN_SYSCALL5(mount, SYS_MOUNT, char *, char *, char *, unsigned long, void *);
DEFN_SYSCALL2(lstat, SYS_LSTAT, char *, void *);

extern void _init();
extern void _fini();

char ** environ = NULL;
int _environ_size = 0;
char * _argv_0 = NULL;
int __libc_debug = 0;

char ** __argv = NULL;
extern char ** __get_argv(void) {
	return __argv;
}

extern void __stdio_init_buffers(void);

void _exit(int val){
	_fini();
	syscall_exit(val);

	__builtin_unreachable();
}

__attribute__((constructor))
static void _libc_init(void) {
	__stdio_init_buffers();

	unsigned int x = 0;
	unsigned int nulls = 0;
	for (x = 0; 1; ++x) {
		if (!__get_argv()[x]) {
			++nulls;
			if (nulls == 2) {
				break;
			}
			continue;
		}
		if (nulls == 1) {
			environ = &__get_argv()[x];
			break;
		}
	}
	if (!environ) {
		environ = malloc(sizeof(char *) * 4);
		environ[0] = NULL;
		environ[1] = NULL;
		environ[2] = NULL;
		environ[3] = NULL;
		_environ_size = 4;
	} else {
		/* Find actual size */
		int size = 0;

		char ** tmp = environ;
		while (*tmp) {
			size++;
			tmp++;
		}

		if (size < 4) {
			_environ_size = 4;
		} else {
			/* Multiply by two */
			_environ_size = size * 2;
		}

		char ** new_environ = malloc(sizeof(char*) * _environ_size);
		int i = 0;
		while (i < _environ_size && environ[i]) {
			new_environ[i] = environ[i];
			i++;
		}

		while (i < _environ_size) {
			new_environ[i] = NULL;
			i++;
		}

		environ = new_environ;
	}
	if (getenv("__LIBC_DEBUG")) __libc_debug = 1;
	_argv_0 = __get_argv()[0];
}

void pre_main(int (*main)(int,char**), int argc, char * argv[]) {
	if (!__get_argv()) {
		/* Statically loaded, must set __argv so __get_argv() works */
		__argv = argv;
	}
	_init();
	exit(main(argc, argv));
}

