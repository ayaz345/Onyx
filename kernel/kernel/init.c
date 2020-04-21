/*
* Copyright (c) 2016, 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
/**************************************************************************
 *
 *
 * File: init.c
 *
 * Description: Main init file, contains the entry point and initialization
 *
 * Date: 30/1/2016
 *
 *
 **************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <mbr.h>
#include <multiboot2.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <pthread_kernel.h>
#include <partitions.h>
#include <assert.h>

#include <sys/mman.h>
#include <acpica/acpi.h>

#include <onyx/debug.h>
#include <onyx/slab.h>
#include <onyx/vm.h>
#include <onyx/paging.h>
#include <onyx/idt.h>
#include <onyx/tty.h>
#include <onyx/panic.h>
#include <onyx/cpu.h>
#include <onyx/pit.h>
#include <onyx/vfs.h>
#include <onyx/initrd.h>
#include <onyx/task_switching.h>
#include <onyx/binfmt.h>
#include <onyx/elf.h>
#include <onyx/tss.h>
#include <onyx/heap.h>
#include <onyx/acpi.h>
#include <onyx/power_management.h>
#include <onyx/udp.h>
#include <onyx/dhcp.h>
#include <onyx/modules.h>
#include <onyx/ethernet.h>
#include <onyx/random.h>
#include <onyx/dev.h>
#include <onyx/bootmem.h>
#include <onyx/log.h>
#include <onyx/dns.h>
#include <onyx/icmp.h>
#include <onyx/process.h>
#include <onyx/block.h>
#include <onyx/elf.h>
#include <onyx/smbios.h>
#include <onyx/fscache.h>
#include <onyx/page.h>
#include <onyx/irq.h>
#include <onyx/vdso.h>
#include <onyx/timer.h>
#include <onyx/worker.h>
#include <onyx/utils.h>
#include <onyx/sysfs.h>
#include <onyx/pagecache.h>
#include <onyx/driver.h>
#include <onyx/rwlock.h>
#include <onyx/crypt/sha256.h>
#include <onyx/clock.h>
#include <onyx/percpu.h>
#include <onyx/drm.h>
#include <onyx/ktrace.h>
#include <onyx/exec.h>
#include <onyx/init.h>

#include <pci/pci.h>

extern uint64_t kernel_end;
extern uintptr_t _start_smp;
extern uintptr_t _end_smp;

void *initrd_addr = NULL;
void *tramp = NULL;
void *phys_fb = NULL;
char kernel_cmdline[256];
uintptr_t address = 0;

extern void libc_late_init();

char *kernel_arguments[200];
int kernel_argc = 0;

void kernel_parse_command_line(char *cmd)
{
	char *original_string = cmd;
	while(*cmd)
	{
		if(*cmd == '-') /* Found an argument */
		{
			char *token = strchr(cmd, ' ');
			if(!token)
				token = original_string + strlen(original_string);
			size_t size_token = (size_t)(token - cmd);
			char *new_string = malloc(size_token + 1);
			if(!new_string)
			{
				ERROR("kernel", "failed parsing: out of memory\n");
			}

			memset(new_string, 0, size_token + 1);
			memcpy(new_string, cmd, size_token);
			
			if(kernel_argc == 200)
			{
				panic("kernel: too many arguments passed to the kernel");
			}

			kernel_arguments[kernel_argc] = new_string;
			kernel_argc++;
			cmd += size_token -1;
		}

		cmd++;
	}
}

char *get_kernel_cmdline(void)
{
	return kernel_cmdline;
}

void set_initrd_address(void *initrd_address)
{
	initrd_addr = initrd_address;
}

char *kernel_getopt(char *opt)
{
	for(int i = 0; i < kernel_argc; i++)
	{
		if(memcmp(kernel_arguments[i], opt, strlen(opt)) == 0)
		{
			/* We found the argument, retrieve the value */
			if(strlen(opt) == strlen(kernel_arguments[i])) /* if len(opt) == len(kargs[i]),
			 the argument has no value (or the caller fucked up) */
				return opt;
			char *parse = kernel_arguments[i] + strlen(opt);
			if(*parse == '=')
				return ++parse;
			if(*parse == ' ')
				return ++parse;
		}
	}
	ERROR("kernel", "%s: no such argument\n", opt);
	return NULL;
}


void *process_setup_auxv(void *buffer, struct process *process);
void dump_used_mem(void);

int alloc_fd(int fdbase);

extern PML *current_pml4;
int find_and_exec_init(char **argv, char **envp)
{
	char *path = strdup("/sbin/init");
	assert(path != NULL);
retry:;
	struct file *in = open_vfs(get_fs_root(), path);
	if(!in)
	{
		printk("%s: Not found\n", path);
		perror("open_vfs");
		if(!strcmp(path, "/bin/init"))
		{
			perror("open");
			panic("No init program found!\n");
		}
		path = "/bin/init";
		goto retry;
	}

	struct process *proc = process_create(path, NULL, NULL);
	if(!proc)
		return errno = ENOMEM, -1;

	vm_save_current_mmu(&proc->address_space);

	get_current_thread()->owner = proc;
 
	/* Setup standard file descriptors (STDIN(0), STDOUT(1), STDERR(2)) */
	
	unsigned int flags[3] = {O_RDONLY, O_WRONLY, O_WRONLY};

	for(int i = 0; i < 3; i++)
	{
		struct file *streams = open_vfs(get_fs_root(), "/dev/tty");

		assert(open_with_vnode(streams, flags[i]) == i);
		fd_put(streams);
	}

	proc->ctx.cwd = get_fs_root();
	proc->ctx.name = strdup("/");
	assert(proc->ctx.name != NULL);

	/* Read the file signature */
	unsigned char *buffer = zalloc(BINFMT_SIGNATURE_LENGTH);
	if (!buffer)
		return errno = ENOMEM, -1;
	
	if(read_vfs(0, BINFMT_SIGNATURE_LENGTH, buffer, in) < 0)
	{
		return -1;
	}

	struct exec_state st;
	st.flushed = true;

	argv[0] = path;
	/* Prepare the argument struct */
	struct binfmt_args args = {0};
	args.file_signature = buffer;
	args.filename = path;
	args.file = in;
	args.argv = argv;
	args.envp = envp;
	args.state = &st;

	assert(vm_create_address_space(&proc->address_space, proc) == 0);

	struct process *current = get_current_process();

	/* Finally, load the binary */
	void *entry = load_binary(&args);

	assert(entry != NULL);

	assert(vm_create_brk(get_current_address_space()) == 0);

	int argc;
	char **_argv = process_copy_envarg(argv, false, &argc);
	char **_env = process_copy_envarg(envp, false, NULL);

	struct thread *main_thread = process_create_thread(proc, (thread_callback_t) entry, 0,
		argc, _argv, _env);
	
	assert(main_thread != NULL);

	Elf64_auxv_t *auxv = process_setup_auxv(main_thread->user_stack_bottom, current);
	registers_t *regs = (registers_t *) main_thread->kernel_stack;
	regs->rcx = (uintptr_t) auxv;
	
	uintptr_t *fs = get_user_pages(VM_TYPE_REGULAR, 1, VM_WRITE | VM_NOEXEC | VM_USER);
	assert(fs != NULL);
	main_thread->fs = (void*) fs;
	__pthread_t *p = (__pthread_t*) fs;
	p->self = (__pthread_t*) fs;
	p->tid = main_thread->id;
	p->pid = get_current_process()->pid;

	sched_start_thread(main_thread);

	get_current_thread()->owner = NULL;

	return 0;
}

#if 0
void dump_used_mem(void)
{
	struct memstat ps;
	page_get_stats(&ps);
	printk("Used total: %lu - Page cache %lu, kernel heap %lu\n", ps.allocated_pages,
		ps.page_cache_pages, ps.kernel_heap_pages);

	unsigned long memory_pressure = (ps.allocated_pages * 1000000) / (ps.total_pages);
	printk("Global %lu\n", ps.total_pages);
	printk("Memory pressure: 0.%06lu\n", memory_pressure);
}

#endif

static thread_t *new_thread;

void kernel_multitasking(void *);
void reclaim_initrd(void);
void do_ktests(void);

struct init_level_info
{
	unsigned long *level_start;
	unsigned long *level_end;
};

extern unsigned long __init_level0_start;
extern unsigned long __init_level0_end;
extern unsigned long __init_level1_start;
extern unsigned long __init_level1_end;
extern unsigned long __init_level2_start;
extern unsigned long __init_level2_end;
extern unsigned long __init_level3_start;
extern unsigned long __init_level3_end;
extern unsigned long __init_level4_start;
extern unsigned long __init_level4_end;
extern unsigned long __init_level5_start;
extern unsigned long __init_level5_end;
extern unsigned long __init_level6_start;
extern unsigned long __init_level6_end;
extern unsigned long __init_level7_start;
extern unsigned long __init_level7_end;

static struct init_level_info init_levels[INIT_LEVEL_CORE_AFTER_SCHED + 2] = {
	{&__init_level0_start, &__init_level0_end},
	{&__init_level1_start, &__init_level1_end},
	{&__init_level2_start, &__init_level2_end},
	{&__init_level3_start, &__init_level3_end},
	{&__init_level4_start, &__init_level4_end},
	{&__init_level5_start, &__init_level5_end},
	{&__init_level6_start, &__init_level6_end},
	{&__init_level7_start, &__init_level7_end}
};


void do_init_level(unsigned int level)
{
	unsigned long *start = init_levels[level].level_start;
	unsigned long *end = init_levels[level].level_end;

	while(start != end)
	{
		void (*func)() = (void *) *start;
		func();
		start++;
	}
}

void fs_init(void)
{
	/* Initialize the VFS */
	vfs_init();

	if(!initrd_addr)
		panic("Initrd not found");

	initrd_addr = (void*)((char*) initrd_addr + PHYS_BASE);

	/* Initialize the initrd */
	init_initrd(initrd_addr);

	reclaim_initrd();
}

void kernel_main(void)
{
	do_init_level(INIT_LEVEL_VERY_EARLY_CORE);

	do_init_level(INIT_LEVEL_VERY_EARLY_PLATFORM);

	fs_init();
	
	do_init_level(INIT_LEVEL_EARLY_CORE_KERNEL);

	do_init_level(INIT_LEVEL_EARLY_PLATFORM);
	
	do_init_level(INIT_LEVEL_CORE_PLATFORM);

	do_init_level(INIT_LEVEL_CORE_INIT);

	DISABLE_INTERRUPTS();

	/* Initialize the scheduler */
	if(sched_init())
		panic("sched: failed to initialize!");

#ifdef CONFIG_DO_TESTS
	/* Execute ktests */
	do_ktests();
#endif

	/* Initalize multitasking */
	new_thread = sched_create_thread(kernel_multitasking, 1, NULL);

	assert(new_thread);

	do_init_level(INIT_LEVEL_CORE_AFTER_SCHED);

	/* Start the new thread */
	sched_start_thread(new_thread);

	ENABLE_INTERRUPTS();
	
	sched_transition_to_idle();
}

void kernel_multitasking(void *arg)
{
	LOG("kernel", "Command line: %s\n", kernel_cmdline);

	/* Parse the command line string to a more friendly argv-like buffer */
	kernel_parse_command_line(kernel_cmdline);

	do_init_level(INIT_LEVEL_CORE_KERNEL);

	/* Start populating /dev */
	tty_create_dev(); /* /dev/tty */
	null_init(); /* /dev/null */
	zero_init(); /* /dev/zero */
	entropy_init_dev(); /* /dev/random and /dev/urandom */

	/* Mount sysfs */
	sysfs_mount();

	/* Populate /sys */
	vm_sysfs_init();

	/* Mount the root partition */
	char *root_partition = kernel_getopt("--root");
	if(!root_partition)
		panic("--root wasn't specified in the kernel arguments");

	/* Note that we don't actually allocate an extra byte for the NULL terminator, since the partition number will
	 become just that */
	char *device_name = malloc(strlen(root_partition));
	if(!device_name)
		panic("Out of memory while allocating ´device_name´");

	strcpy(device_name, root_partition);
	/* Zero-terminate the string */
	device_name[strlen(root_partition)-1] = '\0';

	/* Pass the root partition to init */
	char *args[] = {"", root_partition, NULL};
	char *envp[] = {"PATH=/bin:/usr/bin:/sbin:", NULL};

	assert(find_and_exec_init(args, envp) == 0);

	set_current_state(THREAD_UNINTERRUPTIBLE);
	sched_yield();
}
