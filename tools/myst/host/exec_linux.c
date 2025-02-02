// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <assert.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <syscall.h>
#include <time.h>

#include <myst/args.h>
#include <myst/cpio.h>
#include <myst/elf.h>
#include <myst/eraise.h>
#include <myst/file.h>
#include <myst/kernel.h>
#include <myst/reloc.h>
#include <myst/round.h>
#include <myst/strings.h>
#include <myst/tcall.h>
#include <myst/thread.h>

#include "../config.h"
#include "../shared.h"
#include "archive.h"
#include "exec_linux.h"
#include "utils.h"

#define USAGE_FORMAT \
    "\n\
\n\
Usage: %s exec-linux [options] <rootfs> <application> <args...>\n\
\n\
Where:\n\
    exec-linux           -- execute an application within <rootfs> in a\n\
                            non-trusted Linux environment\n\
    <rootfs>             -- the root file system containing the application\n\
                            (CPIO or EXT2)\n\
    <application>        -- the path of the executable program within\n\
                            <rootfs> that will be executed\n\
    <args>               -- arguments to passed through to the <application>\n\
\n\
Options:\n\
    --help               -- this message\n\
    --memory-size <size> -- the memory size required by the Mystikos kernel\n\
                            and application, where <size> may have a\n\
                            multiplier suffix: k 1024, m 1024*1024, or\n\
                            g 1024*1024*1024\n\
    --app-config-path <json> -- specifies the configuration json file for\n\
                                running an unsigned binary. The file can be\n\
                                the same one used for the signing process.\n\
\n\
"

struct options
{
    bool trace_errors;
    bool trace_syscalls;
    bool export_ramfs;
    char rootfs[PATH_MAX];
};

struct regions
{
    void* rootfs_data;
    size_t rootfs_size;
    void* archive_data;
    size_t archive_size;
    elf_image_t libmystkernel;
    elf_image_t libmystcrt;
    void* mman_data;
    size_t mman_size;
    void* app_config;
    size_t app_config_size;
};

static void _get_options(
    int* argc,
    const char* argv[],
    struct options* options,
    size_t* heap_size,
    const char** app_config_path)
{
    /* Get --trace-syscalls option */
    if (cli_getopt(argc, argv, "--trace-syscalls", NULL) == 0 ||
        cli_getopt(argc, argv, "--strace", NULL) == 0)
    {
        options->trace_syscalls = true;
    }

    /* Get --trace-errors option */
    if (cli_getopt(argc, argv, "--trace-errors", NULL) == 0 ||
        cli_getopt(argc, argv, "--etrace", NULL) == 0)
    {
        options->trace_errors = true;
    }

    /* Get --export-ramfs option */
    if (cli_getopt(argc, argv, "--export-ramfs", NULL) == 0)
        options->export_ramfs = true;

    /* Set export_ramfs option based on MYST_ENABLE_GCOV env variable */
    {
        const char* val;

        if ((val = getenv("MYST_ENABLE_GCOV")) && strcmp(val, "1") == 0)
            options->export_ramfs = true;
    }

    /* Get --memory-size or --memory-size option */
    {
        const char* opt;
        const char* arg = NULL;

        if ((cli_getopt(argc, argv, "--memory-size", &arg) == 0))
        {
            opt = "--memory-size";
        }

        if (!arg && cli_getopt(argc, argv, "--user-mem-size", &arg) == 0)
        {
            /* legacy option (kept for backwards compatibility) */
            opt = "--user-mem-size";
        }

        if (arg)
        {
            if ((myst_expand_size_string_to_ulong(arg, heap_size) != 0) ||
                (myst_round_up(*heap_size, PAGE_SIZE, heap_size) != 0))
            {
                _err("%s <size> -- bad suffix (must be k, m, or g)\n", opt);
            }
        }
    }

    // get app config if present
    cli_getopt(argc, argv, "--app-config-path", app_config_path);
}

static void* _map_mmap_region(size_t length)
{
    const int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    const int flags = MAP_ANONYMOUS | MAP_PRIVATE;
    void* addr;

    assert((length % PAGE_SIZE) == 0);

    if ((addr = mmap(NULL, length, prot, flags, -1, 0)) == MAP_FAILED)
        return NULL;

    return addr;
}

static void _load_regions(
    const char* rootfs,
    const char* archive,
    size_t heap_size,
    const char* app_config,
    struct regions* r)
{
    char path[PATH_MAX];

    if (myst_load_file(rootfs, &r->rootfs_data, &r->rootfs_size) != 0)
        _err("failed to map file: %s", rootfs);

    if (myst_load_file(archive, &r->archive_data, &r->archive_size) != 0)
        _err("failed to map file: %s", archive);

    if (app_config &&
        (myst_load_file(app_config, &r->app_config, &r->app_config_size) != 0))
        _err("failed to load config file: %s", app_config);

    /* Load libmystcrt.so */
    {
        if (format_libmystcrt(path, sizeof(path)) != 0)
            _err("cannot find libmystcrt.so");

        if (elf_image_load(path, &r->libmystcrt) != 0)
            _err("failed to load C runtime image: %s", path);

        /* Add crt debugger symbols to gdb */
        {
            void* file_data;
            size_t file_size;

            if (myst_load_file(path, &file_data, &file_size) != 0)
                _err("failed to load file: %s", path);

            if (myst_tcall_add_symbol_file(
                    file_data,
                    file_size,
                    r->libmystcrt.image_data,
                    r->libmystcrt.image_size) != 0)
            {
                _err("failed to add crt debug symbols");
            }

            /* Load symbols and notify gdb */
            myst_tcall_load_symbols();

            free(file_data);
        }
    }

    /* Load libmystcrt.so */
    {
        if (format_libmystkernel(path, sizeof(path)) != 0)
            _err("cannot find libmystkernel.so");

        if (elf_image_load(path, &r->libmystkernel) != 0)
            _err("failed to load C runtime image: %s", path);

        /* Add kernel debugger symbols to gdb */
        {
            void* file_data;
            size_t file_size;

            if (myst_load_file(path, &file_data, &file_size) != 0)
                _err("failed to load file: %s", path);

            if (myst_tcall_add_symbol_file(
                    file_data,
                    file_size,
                    r->libmystkernel.image_data,
                    r->libmystkernel.image_size) != 0)
            {
                _err("failed to add kernel debug symbols");
            }

            /* Load symbols and notify gdb */
            myst_tcall_load_symbols();

            free(file_data);
        }
    }

    if (heap_size == 0)
    {
        r->mman_size = DEFAULT_MMAN_SIZE;
    }
    else
    {
        /* command line parsing gave pages. convert to size */
        r->mman_size = heap_size;
    }

    if (!(r->mman_data = _map_mmap_region(r->mman_size)))
        _err("failed to map mmap region");

    /* Apply relocations to the libmystkernel.so image */
    if (myst_apply_relocations(
            r->libmystkernel.image_data,
            r->libmystkernel.image_size,
            r->libmystkernel.reloc_data,
            r->libmystkernel.reloc_size) != 0)
    {
        _err("failed to apply relocations to libmystkernel.so\n");
    }
}

static void _release_regions(struct regions* r)
{
    free(r->rootfs_data);
    free(r->archive_data);
    elf_image_free(&r->libmystcrt);
    elf_image_free(&r->libmystkernel);
    munmap(r->mman_data, r->mman_size);
}

/* the address of this is eventually passed to futex (uaddr argument) */
static __thread int _thread_event;

static int _enter_kernel(
    int argc,
    const char* argv[],
    int envc,
    const char* envp[],
    struct options* options,
    struct regions* regions,
    long (*tcall)(long n, long params[6]),
    int* return_status,
    char* err,
    size_t err_size)
{
    int ret = 0;
    myst_kernel_args_t args;
    const elf_ehdr_t* ehdr = regions->libmystkernel.image_data;
    myst_kernel_entry_t entry;
    myst_args_t env;
    const char* cwd = "/";       /* default */
    const char* hostname = NULL; // kernel has a default

    if (err)
        *err = '\0';
    else
        ERAISE(-EINVAL);

    memset(&env, 0, sizeof(env));

    if (!argv || !envp || !options || !regions || !tcall || !return_status)
    {
        snprintf(err, err_size, "bad argument");
        ERAISE(-EINVAL);
    }

    if (return_status)
        *return_status = 0;

    /* Make a copy of the environment variables */
    {
        if (myst_args_init(&env) != 0)
        {
            snprintf(err, err_size, "myst_args_init() failed");
            ERAISE(-EINVAL);
        }

        if (myst_args_append(&env, envp, envc) != 0)
        {
            snprintf(err, err_size, "myst_args_append() failed");
            ERAISE(-EINVAL);
        }
    }

    /* Inject the MYST_TARGET environment variable */
    {
        const char val[] = "MYST_TARGET=";

        for (size_t i = 0; i < env.size; i++)
        {
            if (strncmp(env.data[i], val, sizeof(val) - 1) == 0)
            {
                snprintf(err, err_size, "environment already contains %s", val);
                ERAISE(-EINVAL);
            }
        }

        myst_args_append1(&env, "MYST_TARGET=linux");
    }

    /* Extract any settings from the config, if present */
    config_parsed_data_t parsed_data = {0};
    if (regions->app_config && regions->app_config_size)
    {
        if (parse_config_from_buffer(
                regions->app_config, regions->app_config_size, &parsed_data) ==
            0)
        {
            /* only override if we have a cwd config item */
            if (parsed_data.cwd)
                cwd = parsed_data.cwd;

            if (parsed_data.hostname)
                hostname = parsed_data.hostname;
        }
        else
        {
            _err("Failed to parse app config from");
        }
    }

    memset(&args, 0, sizeof(args));
    args.image_data = (void*)0;
    args.image_size = 0x7fffffffffffffff;
    args.kernel_data = regions->libmystkernel.image_data;
    args.kernel_size = regions->libmystkernel.image_size;
    args.reloc_data = regions->libmystkernel.reloc_data;
    args.reloc_size = regions->libmystkernel.reloc_size;
    args.crt_reloc_data = regions->libmystcrt.reloc_data;
    args.crt_reloc_size = regions->libmystcrt.reloc_size;
    args.symtab_data = regions->libmystkernel.symtab_data;
    args.symtab_size = regions->libmystkernel.symtab_size;
    args.dynsym_data = regions->libmystkernel.dynsym_data;
    args.dynsym_size = regions->libmystkernel.dynsym_size;
    args.strtab_data = regions->libmystkernel.strtab_data;
    args.strtab_size = regions->libmystkernel.strtab_size;
    args.dynstr_data = regions->libmystkernel.dynstr_data;
    args.dynstr_size = regions->libmystkernel.dynstr_size;
    args.argc = argc;
    args.argv = argv;
    args.envc = env.size;
    args.envp = env.data;
    args.cwd = cwd;
    args.hostname = hostname;
    args.mman_data = regions->mman_data;
    args.mman_size = regions->mman_size;
    args.rootfs_data = regions->rootfs_data;
    args.rootfs_size = regions->rootfs_size;
    args.archive_data = regions->archive_data;
    args.archive_size = regions->archive_size;
    args.crt_data = regions->libmystcrt.image_data;
    args.crt_size = regions->libmystcrt.image_size;
    args.max_threads = LONG_MAX;
    args.trace_errors = options->trace_errors;
    args.trace_syscalls = options->trace_syscalls;
    args.have_syscall_instruction = true;
    args.export_ramfs = options->export_ramfs;
    args.event = (uint64_t)&_thread_event;
    args.tee_debug_mode = true;
    args.tcall = tcall;

    if (options->rootfs)
        myst_strlcpy(args.rootfs, options->rootfs, sizeof(args.rootfs));

    /* Verify that the kernel is an ELF image */
    if (!elf_valid_ehdr_ident(ehdr))
    {
        snprintf(err, err_size, "bad kernel image");
        ERAISE(-EINVAL);
    }

    /* Resolve the the kernel entry point */
    entry = (myst_kernel_entry_t)((uint8_t*)ehdr + ehdr->e_entry);

    if ((uint8_t*)entry < (uint8_t*)ehdr ||
        (uint8_t*)entry >= (uint8_t*)ehdr + regions->libmystkernel.image_size)
    {
        snprintf(err, err_size, "kernel entry point is out of bounds");
        ERAISE(-EINVAL);
    }

    *return_status = (*entry)(&args);

done:
    if (regions->app_config && regions->app_config_size)
        free_config(&parsed_data);

    if (env.data)
        free(env.data);

    return ret;
}

static long _tcall(long n, long params[6])
{
    return myst_tcall(n, params);
}

int exec_linux_action(int argc, const char* argv[], const char* envp[])
{
    struct options options = {.trace_syscalls = false, .export_ramfs = false};
    const char* rootfs_arg;
    const char* program_arg;
    struct regions regions;
    char err[128];
    static const size_t max_pubkeys = 128;
    const char* pubkeys[max_pubkeys];
    size_t num_pubkeys = 0;
    static const size_t max_roothashes = 128;
    const char* roothashes[max_roothashes];
    size_t num_roothashes = 0;
    char archive_path[PATH_MAX];
    char rootfs_path[] = "/tmp/mystXXXXXX";
    size_t heap_size = 0;
    const char* app_config_path = NULL;

    (void)program_arg;

    /* Get the command-line options */
    _get_options(&argc, argv, &options, &heap_size, &app_config_path);

    /* Get --pubkey=filename options */
    get_archive_options(
        &argc,
        argv,
        pubkeys,
        max_pubkeys,
        &num_pubkeys,
        roothashes,
        max_roothashes,
        &num_roothashes);

    /* Check usage */
    if (argc < 4)
    {
        fprintf(stderr, USAGE_FORMAT, argv[0]);
        return 1;
    }

    rootfs_arg = argv[2];
    program_arg = argv[3];
    create_archive(
        pubkeys, num_pubkeys, roothashes, num_roothashes, archive_path);

    /* copy the rootfs path to the options */
    if (myst_strlcpy(options.rootfs, rootfs_arg, sizeof(options.rootfs)) >=
        sizeof(options.rootfs))
    {
        _err("<rootfs> command line argument is too long: %s", rootfs_arg);
    }

    /* if not a CPIO archive, create a zero-filled file with one page */
    if (myst_cpio_test(rootfs_arg) == -ENOTSUP)
    {
        int fd;
        uint8_t page[PAGE_SIZE];

        if ((fd = mkstemp(rootfs_path)) < 0)
            _err("failed to create temporary file");

        memset(page, 0, sizeof(page));

        if (write(fd, page, sizeof(page)) != sizeof(page))
            _err("failed to create file");

        close(fd);
        rootfs_arg = rootfs_path;
    }

    /* Load the regions into memory */
    _load_regions(
        rootfs_arg, archive_path, heap_size, app_config_path, &regions);

    unlink(archive_path);

    int envc = 0;
    while (envp[envc] != NULL)
    {
        envc++;
    }
    int return_status = 0;

    assert(argc >= 4);
    argc -= 3;
    argv += 3;

    /* Enter the kernel image */
    if (_enter_kernel(
            argc,
            argv,
            envc,
            envp,
            &options,
            &regions,
            _tcall,
            &return_status,
            err,
            sizeof(err)) != 0)
    {
        _err("%s", err);
    }

    /* release the regions memory */
    _release_regions(&regions);

#if 0
    if (rootfs_arg == rootfs_path)
        unlink(rootfs_path);
#endif

    return return_status;
}

/*
**==============================================================================
**
** Threading tcalls:
**
**==============================================================================
*/

static void* _thread_func(void* arg)
{
    uint64_t cookie = (uint64_t)arg;
    uint64_t event = (uint64_t)&_thread_event;

    if (myst_run_thread(cookie, event) != 0)
    {
        fprintf(stderr, "myst_run_thread() failed\n");
        exit(1);
    }

    return NULL;
}

long myst_tcall_create_thread(uint64_t cookie)
{
    long ret = 0;
    pthread_t t;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&t, &attr, _thread_func, (void*)cookie) != 0)
    {
        ret = -EINVAL;
        goto done;
    }

done:
    pthread_attr_destroy(&attr);
    return ret;
}
