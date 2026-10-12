/*
BENCH_HOST.C

The host's side of port/wii/tests/draw_bench.c (compiled for powerpc-linux
and run under qemu-ppc by tools/wii_draw_bench.py): the contiguous window at
the Wii's address, and the output. The bench itself is compiled as the Wii
build compiles the device (for powerpc-eabi, against newlib's headers), so
it reaches the C library only through these and calls of the same ABI.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <ucontext.h>

/* a fault's address and its caller's, for addr2line */
static void fault(int signal_number, siginfo_t *info, void *context)
{
	const ucontext_t *state = context;

	fprintf(stderr, "bench: signal %d at %08lx (from %08lx), address %p\n", signal_number,
		(unsigned long)state->uc_mcontext.regs->nip, (unsigned long)state->uc_mcontext.regs->link, info->si_addr);
	_exit(3);
}

/* the window at the Wii's PLATFORM_CONTIGUOUS_WINDOW, so that the device's
physical and virtual addresses are the Wii's */
void *bench_window(unsigned long address, unsigned long size)
{
	struct sigaction action;
	void *window;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = fault;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	window = mmap((void *)address, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

	if (window == MAP_FAILED)
	{
		perror("bench: the window");
		exit(1);
	}
	return window;
}

void bench_print(const char *text)
{
	fputs(text, stdout);
	fflush(stdout);
}

/* the device's log, kept apart from the results (debug only) */
static int log_on = -1;

void platform_log(const char *format, ...)
{
	va_list arguments;

	if (log_on < 0)
		log_on = getenv("BENCH_LOG") != NULL;
	if (!log_on)
		return;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}
