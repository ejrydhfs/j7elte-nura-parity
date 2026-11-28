#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ucontext.h>

static void dump_file(const char *path, const char *data, ssize_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		ssize_t off = 0;
		while (off < len) {
			ssize_t n = write(fd, data + off, len - off);
			if (n <= 0)
				break;
			off += n;
		}
		close(fd);
	}
}

static void handler(int sig, siginfo_t *si, void *uc)
{
	ucontext_t *u = (ucontext_t *)uc;
	unsigned long pc = (unsigned long)u->uc_mcontext.pc;
	unsigned long sp = (unsigned long)u->uc_mcontext.sp;
	unsigned long lr = (unsigned long)u->uc_mcontext.regs[30];
	unsigned long fp = (unsigned long)u->uc_mcontext.regs[29];
	char b[200];
	int n = snprintf(b, sizeof b,
			 "=== CRASH %s addr=%p pc=%#lx lr=%#lx sp=%#lx fp=%#lx\n",
			 sig == SIGSEGV ? "SEGV" : "BUS", si->si_addr, pc, lr, sp, fp);
	write(2, b, n);

	{
		static char mb[262144];
		int fd = open("/proc/self/maps", O_RDONLY);
		ssize_t got = 0;
		if (fd >= 0) {
			ssize_t r;
			char tmp[8192];
			while (got < (ssize_t)sizeof mb - 1 &&
			       (r = read(fd, tmp, sizeof tmp)) > 0) {
				memcpy(mb + got, tmp, r);
				got += r;
			}
			close(fd);
		}
		if (got > 0)
			dump_file("/data/kwin-maps-crash.txt", mb, got);
	}

	{
		char sb[4096];
		int off = 0, i;
		off += snprintf(sb + off, sizeof sb - off,
				"PC %#lx\nLR %#lx\nSP %#lx\nFP %#lx\n",
				pc, lr, sp, fp);
		for (i = 0; i < 20 && off < (int)sizeof sb - 48; i++) {
			unsigned long nfp, ret;
			if (!fp)
				break;
			nfp = ((unsigned long *)fp)[0];
			ret = ((unsigned long *)fp)[1];
			off += snprintf(sb + off, sizeof sb - off,
					"FRAME %#lx %#lx\n", ret, nfp);
			if (nfp <= fp || nfp - fp > 0x200000)
				break;
			fp = nfp;
		}
		dump_file("/data/kwin-stack.txt", sb, off);
	}
	_exit(139);
}

__attribute__((constructor)) static void segv_init(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
}
