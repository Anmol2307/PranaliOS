/*
 *  Multi2Sim
 *  Copyright (C) 2007  Rafael Ubal Tena (raurte@gap.upv.es)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "m2skernel.h"

#include <unistd.h>
#include <utime.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <linux/unistd.h>


int syscall_debug_category;
int blocksize, numblocks;
int *blockowners;

static char *syscall_name[] = {
#define DEFSYSCALL(name,code) #name,
#include "syscall.dat"
#undef DEFSYSCALL
    ""
};

enum {
#define DEFSYSCALL(name,code) syscall_code_##name = code,
#include "syscall.dat"
#undef DEFSYSCALL
    syscall_code_count
};

static uint64_t syscall_freq[syscall_code_count + 1];

/* Print a string in debug output.
 * If 'force' is set, keep printing after \0 is found. */
void syscall_debug_string(char *text, char *s, int len, int force) {
    char buf[200], *bufptr;
    int trunc = 0;
    if (!debug_status(syscall_debug_category))
        return;

    memset(buf, 0, 200);
    strcpy(buf, "\"");
    bufptr = &buf[1];
    if (len > 40) {
        len = 40;
        trunc = 1;
    }
    for (;;) {
        if (!len || (!*s && !force)) {
            strcpy(bufptr, !len && trunc ? "\"..." : "\"");
            break;
        }
        if ((unsigned char) *s >= 32) {
            *bufptr = *s;
            bufptr++;
        } else if (*s == '\0') {
            strcpy(bufptr, "\\0");
            bufptr += 2;
        } else if (*s == '\n') {
            strcpy(bufptr, "\\n");
            bufptr += 2;
        } else if (*s == '\t') {
            strcpy(bufptr, "\\t");
            bufptr += 2;
        } else {
            sprintf(bufptr, "\\%02x", *s);
            bufptr += 3;
        }
        s++;
        len--;
    }
    syscall_debug("%s=%s\n", text, buf);
}


/* Error messages */
char *err_syscall_note =
        "\tThe system calls performed by the executed application are intercepted by\n"
        "\tMulti2Sim and emulated in file 'syscall.c'. The most common system calls are\n"
        "\tcurrently supported, but your application might perform specific unsupported\n"
        "\tsystem calls or combinations of parameters. To request support for a given\n"
        "\tsystem call, please email 'development@multi2sim.org'.\n";


/* For 'open' */

struct string_map_t open_flags_map = {
    16,
    {
        { "O_RDONLY", 00000000},
        { "O_WRONLY", 00000001},
        { "O_RDWR", 00000002},
        { "O_CREAT", 00000100},
        { "O_EXCL", 00000200},
        { "O_NOCTTY", 00000400},
        { "O_TRUNC", 00001000},
        { "O_APPEND", 00002000},
        { "O_NONBLOCK", 00004000},
        { "O_SYNC", 00010000},
        { "FASYNC", 00020000},
        { "O_DIRECT", 00040000},
        { "O_LARGEFILE", 00100000},
        { "O_DIRECTORY", 00200000},
        { "O_NOFOLLOW", 00400000},
        { "O_NOATIME", 01000000}
    }
};


/* For 'msync' */

struct string_map_t msync_flags_map = {
    3,
    {
        { "MS_ASYNC", 1},
        { "MS_INAVLIAGE", 2},
        { "MS_SYNC", 4}
    }
};


/* For 'access' */

struct string_map_t access_mode_map = {
    3,
    {
        { "X_OK", 1},
        { "W_OK", 2},
        { "R_OK", 4}
    }
};


/* For clone */
struct string_map_t clone_flags_map = {
    23,
    {
        { "CLONE_VM", 0x00000100},
        { "CLONE_FS", 0x00000200},
        { "CLONE_FILES", 0x00000400},
        { "CLONE_SIGHAND", 0x00000800},
        { "CLONE_PTRACE", 0x00002000},
        { "CLONE_VFORK", 0x00004000},
        { "CLONE_PARENT", 0x00008000},
        { "CLONE_THREAD", 0x00010000},
        { "CLONE_NEWNS", 0x00020000},
        { "CLONE_SYSVSEM", 0x00040000},
        { "CLONE_SETTLS", 0x00080000},
        { "CLONE_PARENT_SETTID", 0x00100000},
        { "CLONE_CHILD_CLEARTID", 0x00200000},
        { "CLONE_DETACHED", 0x00400000},
        { "CLONE_UNTRACED", 0x00800000},
        { "CLONE_CHILD_SETTID", 0x01000000},
        { "CLONE_STOPPED", 0x02000000},
        { "CLONE_NEWUTS", 0x04000000},
        { "CLONE_NEWIPC", 0x08000000},
        { "CLONE_NEWUSER", 0x10000000},
        { "CLONE_NEWPID", 0x20000000},
        { "CLONE_NEWNET", 0x40000000},
        { "CLONE_IO", 0x80000000}
    }
};

/* For utime */

struct sim_utimbuf {
    uint32_t actime;
    uint32_t modtime;
} __attribute__((packed));

static void syscall_utime_sim_to_read(struct utimbuf *real, struct sim_utimbuf *sim) {
    real->actime = sim->actime;
    real->modtime = sim->modtime;
}


/* For 'fcntl' */

struct string_map_t fcntl_cmd_map = {
    15,
    {
        { "F_DUPFD", 0},
        { "F_GETFD", 1},
        { "F_SETFD", 2},
        { "F_GETFL", 3},
        { "F_SETFL", 4},
        { "F_GETLK", 5},
        { "F_SETLK", 6},
        { "F_SETLKW", 7},
        { "F_SETOWN", 8},
        { "F_GETOWN", 9},
        { "F_SETSIG", 10},
        { "F_GETSIG", 11},
        { "F_GETLK64", 12},
        { "F_SETLK64", 13},
        { "F_SETLKW64", 14}
    }
};


/* For 'socketcall' */

struct string_map_t socketcall_call_map = {
    17,
    {
        { "SYS_SOCKET", 1},
        { "SYS_BIND", 2},
        { "SYS_CONNECT", 3},
        { "SYS_LISTEN", 4},
        { "SYS_ACCEPT", 5},
        { "SYS_GETSOCKNAME", 6},
        { "SYS_GETPEERNAME", 7},
        { "SYS_SOCKETPAIR", 8},
        { "SYS_SEND", 9},
        { "SYS_RECV", 10},
        { "SYS_SENDTO", 11},
        { "SYS_RECVFROM", 12},
        { "SYS_SHUTDOWN", 13},
        { "SYS_SETSOCKOPT", 14},
        { "SYS_GETSOCKOPT", 15},
        { "SYS_SENDMSG", 16},
        { "SYS_RECVMSG", 17}
    }
};

struct string_map_t socket_family_map = {
    29,
    {
        { "PF_UNSPEC", 0},
        { "PF_UNIX", 1},
        { "PF_INET", 2},
        { "PF_AX25", 3},
        { "PF_IPX", 4},
        { "PF_APPLETALK", 5},
        { "PF_NETROM", 6},
        { "PF_BRIDGE", 7},
        { "PF_ATMPVC", 8},
        { "PF_X25", 9},
        { "PF_INET6", 10},
        { "PF_ROSE", 11},
        { "PF_DECnet", 12},
        { "PF_NETBEUI", 13},
        { "PF_SECURITY", 14},
        { "PF_KEY", 15},
        { "PF_NETLINK", 16},
        { "PF_PACKET", 17},
        { "PF_ASH", 18},
        { "PF_ECONET", 19},
        { "PF_ATMSVC", 20},
        { "PF_SNA", 22},
        { "PF_IRDA", 23},
        { "PF_PPPOX", 24},
        { "PF_WANPIPE", 25},
        { "PF_LLC", 26},
        { "PF_TIPC", 30},
        { "PF_BLUETOOTH", 31},
        { "PF_IUCV", 32}
    }
};

struct string_map_t socket_type_map = {
    7,
    {
        { "SOCK_STREAM", 1},
        { "SOCK_DGRAM", 2},
        { "SOCK_RAW", 3},
        { "SOCK_RDM", 4},
        { "SOCK_SEQPACKET", 5},
        { "SOCK_DCCP", 6},
        { "SOCK_PACKET", 10}
    }
};

/* For fstat64, lstat64 */

struct sim_stat64 {
    uint64_t dev; /* 0 8 */
    uint32_t pad1; /* 8 4 */
    uint32_t __ino; /* 12 4 */
    uint32_t mode; /* 16 4 */
    uint32_t nlink; /* 20 4 */
    uint32_t uid; /* 24 4 */
    uint32_t gid; /* 28 4 */
    uint64_t rdev; /* 32 8 */
    uint32_t pad2; /* 40 4 */
    int64_t size; /* 44 8 */
    uint32_t blksize; /* 52 4 */
    uint64_t blocks; /* 56 8 */
    uint32_t atime; /* 64 4 */
    uint32_t atime_nsec; /* 68 4 */
    uint32_t mtime; /* 72 4 */
    uint32_t mtime_nsec; /* 76 4 */
    uint32_t ctime; /* 80 4 */
    uint32_t ctime_nsec; /* 84 4 */
    uint64_t ino; /* 88 8 */
} __attribute__((packed));

static void syscall_copy_stat64(struct sim_stat64 *sim, struct stat *real) {
    bzero(sim, sizeof (struct sim_stat64));
    sim->dev = real->st_dev;
    sim->__ino = real->st_ino;
    sim->mode = real->st_mode;
    sim->nlink = real->st_nlink;
    sim->uid = real->st_uid;
    sim->gid = real->st_gid;
    sim->rdev = real->st_rdev;
    sim->size = real->st_size;
    sim->blksize = real->st_blksize;
    sim->blocks = real->st_blocks;
    sim->atime = real->st_atime;
    sim->mtime = real->st_mtime;
    sim->ctime = real->st_ctime;
    sim->ino = real->st_ino;
    syscall_debug("  stat64 structure:\n");
    syscall_debug("    dev=%lld, ino=%d, mode=%d, nlink=%d\n",
            (long long) sim->dev, (int) sim->ino, (int) sim->mode, (int) sim->nlink);
    syscall_debug("    uid=%d, gid=%d, rdev=%lld\n",
            (int) sim->uid, (int) sim->gid, (long long) sim->rdev);
    syscall_debug("    size=%lld, blksize=%d, blocks=%lld\n",
            (long long) sim->size, (int) sim->blksize, (long long) sim->blocks);
}


/* For setitimer */

static struct string_map_t itimer_map = {
    3,
    {
        {"ITIMER_REAL", 0},
        {"ITIMER_VIRTUAL", 1},
        {"ITIMER_PROF", 2}
    }
};

struct sim_timeval {
    uint32_t tv_sec;
    uint32_t tv_usec;
} __attribute__((packed));

struct sim_itimerval {
    struct sim_timeval it_interval;
    struct sim_timeval it_value;
} __attribute__((packed));

void sim_timeval_debug(struct sim_timeval *sim_timeval) {
    syscall_debug("    tv_sec=%u, tv_usec=%u\n",
            sim_timeval->tv_sec, sim_timeval->tv_usec);
}

void sim_itimerval_debug(struct sim_itimerval *sim_itimerval) {
    syscall_debug("    it_interval: tv_sec=%u, tv_usec=%u\n",
            sim_itimerval->it_interval.tv_sec, sim_itimerval->it_interval.tv_usec);
    syscall_debug("    it_value: tv_sec=%u, tv_usec=%u\n",
            sim_itimerval->it_value.tv_sec, sim_itimerval->it_value.tv_usec);
}

/* For uname */

struct sim_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} __attribute__((packed));

struct sim_utsname sim_utsname = {
    "Linux",
    "multi2sim",
    "2.6.18-6-686",
    "#1 Mon Jul 17 09:21:59 UTC 2006",
    "i686"
    ""
};

/* For getrusage */

struct sim_rusage {
    uint32_t utime_sec, utime_usec;
    uint32_t stime_sec, stime_usec;
    uint32_t maxrss;
    uint32_t ixrss;
    uint32_t idrss;
    uint32_t isrss;
    uint32_t minflt;
    uint32_t majflt;
    uint32_t nswap;
    uint32_t inblock;
    uint32_t oublock;
    uint32_t msgsnd;
    uint32_t msgrcv;
    uint32_t nsignals;
    uint32_t nvcsw;
    uint32_t nivcsw;
} __attribute__((packed));

static void syscall_copy_rusage(struct sim_rusage *sim, struct rusage *real) {
    sim->utime_sec = real->ru_utime.tv_sec;
    sim->utime_usec = real->ru_utime.tv_usec;
    sim->stime_sec = real->ru_stime.tv_sec;
    sim->stime_usec = real->ru_stime.tv_usec;
    sim->maxrss = real->ru_maxrss;
    sim->ixrss = real->ru_ixrss;
    sim->idrss = real->ru_idrss;
    sim->isrss = real->ru_isrss;
    sim->minflt = real->ru_minflt;
    sim->majflt = real->ru_majflt;
    sim->nswap = real->ru_nswap;
    sim->inblock = real->ru_inblock;
    sim->oublock = real->ru_oublock;
    sim->msgsnd = real->ru_msgsnd;
    sim->msgrcv = real->ru_msgrcv;
    sim->nsignals = real->ru_nsignals;
    sim->nvcsw = real->ru_nvcsw;
    sim->nivcsw = real->ru_nivcsw;
}


/* For relimit */

struct string_map_t rlimit_resource_map = {
    16,
    {

        { "RLIMIT_CPU", 0},
        { "RLIMIT_FSIZE", 1},
        { "RLIMIT_DATA", 2},
        { "RLIMIT_STACK", 3},
        { "RLIMIT_CORE", 4},
        { "RLIMIT_RSS", 5},
        { "RLIMIT_NPROC", 6},
        { "RLIMIT_NOFILE", 7},
        { "RLIMIT_MEMLOCK", 8},
        { "RLIMIT_AS", 9},
        { "RLIMIT_LOCKS", 10},
        { "RLIMIT_SIGPENDING", 11},
        { "RLIMIT_MSGQUEUE", 12},
        { "RLIMIT_NICE", 13},
        { "RLIMIT_RTPRIO", 14},
        { "RLIM_NLIMITS", 15}
    }
};

struct sim_rlimit {
    uint32_t cur;
    uint32_t max;
} __attribute__((packed));

void syscall_rlimit_real_to_sim(struct sim_rlimit *sim, struct rlimit *real) {
    sim->cur = real->rlim_cur;
    sim->max = real->rlim_max;
}

void syscall_rlimit_sim_to_real(struct rlimit *real, struct sim_rlimit *sim) {
    real->rlim_cur = sim->cur;
    real->rlim_max = sim->max;
}

/* For times */

struct sim_tms {
    uint32_t utime;
    uint32_t stime;
    uint32_t cutime;
    uint32_t cstime;
} __attribute__((packed));

static void syscall_copy_tms(struct sim_tms *sim, struct tms *real) {
    sim->utime = real->tms_utime;
    sim->stime = real->tms_stime;
    sim->cutime = real->tms_cutime;
    sim->cstime = real->tms_cstime;
}

/* For 'set_thread_area' */

struct sim_user_desc {
    uint32_t entry_number;
    uint32_t base_addr;
    uint32_t limit;
    uint32_t seg_32bit : 1;
    uint32_t contents : 2;
    uint32_t read_exec_only : 1;
    uint32_t limit_in_pages : 1;
    uint32_t seg_not_present : 1;
    uint32_t useable : 1;
};


/* For rt_sigprocmask */

struct string_map_t sigprocmask_how_map = {
    3,
    {
        { "SIG_BLOCK", 0},
        { "SIG_UNBLOCK", 1},
        { "SIG_SETMASK", 2}
    }
};


/* For 'poll' */

struct string_map_t poll_event_map = {
    6,
    {
        { "POLLIN", 0x0001},
        { "POLLPRI", 0x0002},
        { "POLLOUT", 0x0004},
        { "POLLERR", 0x0008},
        { "POLLHUP", 0x0010},
        { "POLLNVAL", 0x0020}
    }
};

struct sim_pollfd {
    uint32_t fd;
    uint16_t events;
    uint16_t revents;
};


/* For 'select' */

/* Dump host 'fd_set' structure */
void sim_fd_set_dump(char *fd_set_name, fd_set *fds, int n) {
    int i;
    char *comma;

    /* Set empty */
    if (!n || !fds) {
        syscall_debug("    %s={}\n", fd_set_name);
        return;
    }

    /* Dump set */
    syscall_debug("    %s={", fd_set_name);
    comma = "";
    for (i = 0; i < n; i++) {
        if (!FD_ISSET(i, fds))
            continue;
        syscall_debug("%s%d", comma, i);
        comma = ",";
    }
    syscall_debug("}\n");
}

/* Read bitmap of 'guest_fd's from guest memory, and store it into
 * a bitmap of 'host_fd's into host memory. */
int sim_fd_set_read(uint32_t addr, fd_set *fds, int n) {
    int nbyte, nbit, i;
    int host_fd;
    unsigned char c;

    FD_ZERO(fds);
    for (i = 0; i < n; i++) {

        /* Check if fd is set */
        nbyte = i >> 3;
        nbit = i & 7;
        mem_read(isa_mem, addr + nbyte, 1, &c);
        if (!(c & (1 << nbit)))
            continue;

        /* Obtain 'host_fd' */
        host_fd = fdt_get_host_fd(isa_ctx->fdt, i);
        if (host_fd < 0)
            return 0;
        FD_SET(host_fd, fds);
    }
    return 1;
}

/* Read bitmap of 'host_fd's from host memory, and store it into
 * a bitmap of 'guest_fd's into guest memory. */
void sim_fd_set_write(uint32_t addr, fd_set *fds, int n) {
    int nbyte, nbit, i;
    int guest_fd;
    unsigned char c;

    /* No valid address given */
    if (!addr)
        return;

    /* Write */
    mem_zero(isa_mem, addr, (n + 7) / 8);
    for (i = 0; i < n; i++) {

        /* Check if fd is set */
        if (!FD_ISSET(i, fds))
            continue;

        /* Obtain 'guest_fd' and write */
        guest_fd = fdt_get_guest_fd(isa_ctx->fdt, i);
        assert(guest_fd >= 0);
        nbyte = guest_fd >> 3;
        nbit = guest_fd & 7;
        mem_read(isa_mem, addr + nbyte, 1, &c);
        c |= 1 << nbit;
        mem_write(isa_mem, addr + nbyte, 1, &c);
    }
}


/* For 'waitpid' */

struct string_map_t waitpid_options_map = {
    8,
    {
        { "WNOHANG", 0x00000001},
        { "WUNTRACED", 0x00000002},
        { "WEXITED", 0x00000004},
        { "WCONTINUED", 0x00000008},
        { "WNOWAIT", 0x01000000},
        { "WNOTHREAD", 0x20000000},
        { "WALL", 0x40000000},
        { "WCLONE", 0x80000000}
    }
};


/* For 'mmap' */

#define MMAP_BASE_ADDRESS 0xb7fb0000

struct string_map_t mmap_prot_map = {
    6,
    {
        { "PROT_READ", 0x1},
        { "PROT_WRITE", 0x2},
        { "PROT_EXEC", 0x4},
        { "PROT_SEM", 0x8},
        { "PROT_GROWSDOWN", 0x01000000},
        { "PROT_GROWSUP", 0x02000000}
    }
};

struct string_map_t mmap_flags_map = {
    11,
    {
        { "MAP_SHARED", 0x01},
        { "MAP_PRIVATE", 0x02},
        { "MAP_FIXED", 0x10},
        { "MAP_ANONYMOUS", 0x20},
        { "MAP_GROWSDOWN", 0x00100},
        { "MAP_DENYWRITE", 0x00800},
        { "MAP_EXECUTABLE", 0x01000},
        { "MAP_LOCKED", 0x02000},
        { "MAP_NORESERVE", 0x04000},
        { "MAP_POPULATE", 0x08000},
        { "MAP_NONBLOCK", 0x10000}
    }
};

static uint32_t do_mmap(uint32_t addr, uint32_t len, int prot,
        int flags, int guest_fd, uint32_t offset) {
    uint32_t alen; /* Aligned len */
    struct fd_t *fd;
    int perm, host_fd;
    void *host_ptr;

    /* Check that protection flags match in guest and host */
    assert(PROT_READ == 1);
    assert(PROT_WRITE == 2);
    assert(PROT_EXEC == 4);

    /* Check that mapping flags match */
    assert(MAP_SHARED == 0x01);
    assert(MAP_PRIVATE == 0x02);
    assert(MAP_FIXED == 0x10);
    assert(MAP_ANONYMOUS == 0x20);

    /* Translate file descriptor */
    fd = fdt_entry_get(isa_ctx->fdt, guest_fd);
    host_fd = fd ? fd->host_fd : -1;
    if (guest_fd > 0 && host_fd < 0)
        fatal("do_mmap: invalid 'guest_fd'");

    /* Permissions */
    perm = mem_access_init;
    perm |= prot & PROT_READ ? mem_access_read : 0;
    perm |= prot & PROT_WRITE ? mem_access_write : 0;
    perm |= prot & PROT_EXEC ? mem_access_exec : 0;

    /* Flag MAP_ANONYMOUS.
     * If it is set, the 'fd' parameter is ignored. */
    if (flags & MAP_ANONYMOUS)
        host_fd = -1;

    /* 'addr' and 'offset' must be aligned to page size boundaries.
     * 'len' is rounded up to page boundary. */
    if (offset & ~MEM_PAGEMASK)
        fatal("do_mmap: unaligned offset");
    if (addr & ~MEM_PAGEMASK)
        fatal("do_mmap: unaligned addr");
    alen = ROUND_UP(len, MEM_PAGESIZE);

    /* Find region for allocation */
    if (flags & MAP_FIXED) {

        /* If MAP_FIXED is set, the 'addr' parameter must be obeyed, and is not just a
         * hint for a possible base address of the allocated range. */
        if (!addr)
            fatal("do_mmap: no start specified for fixed mapping");

        /* Any allocated page in the range specified by 'addr' and 'len'
         * must be discarded. */
        mem_unmap(isa_mem, addr, alen);

    } else {

        if (!addr || mem_map_space_down(isa_mem, addr, alen) != addr)
            addr = MMAP_BASE_ADDRESS;
        addr = mem_map_space_down(isa_mem, addr, alen);
        if (addr == (uint32_t) - 1)
            fatal("do_mmap: out of guest memory");
    }

    /* Allocation of memory */
    mem_map(isa_mem, addr, alen, perm);

    /* Host mapping */
    if (host_fd >= 0) {
        host_ptr = mmap(NULL, len, prot, flags & ~MAP_FIXED, host_fd, offset);
        if (host_ptr == MAP_FAILED)
            fatal("do_mmap: host call to 'mmap' failed");
        mem_map_host(isa_mem, fd, addr, alen, perm, host_ptr);
        syscall_debug("    host_ptr=%p\n", host_ptr);
        syscall_debug("    host_fd=%d\n", host_fd);
    }

    return addr;
}


/* For 'futex' */

struct string_map_t futex_cmd_map = {
    13,
    {
        { "FUTEX_WAIT", 0},
        { "FUTEX_WAKE", 1},
        { "FUTEX_FD", 2},
        { "FUTEX_REQUEUE", 3},
        { "FUTEX_CMP_REQUEUE", 4},
        { "FUTEX_WAKE_OP", 5},
        { "FUTEX_LOCK_PI", 6},
        { "FUTEX_UNLOCK_PI", 7},
        { "FUTEX_TRYLOCK_PI", 8},
        { "FUTEX_WAIT_BITSET", 9},
        { "FUTEX_WAKE_BITSET", 10},
        { "FUTEX_WAIT_REQUEUE_PI", 11},
        { "FUTEX_CMP_REQUEUE_PI", 12}
    }
};

/* For 'sysctl' */

struct sysctl_args_t {
    uint32_t pname;
    uint32_t nlen;
    uint32_t poldval;
    uint32_t oldlenp;
    uint32_t pnewval;
    uint32_t newlen;
};

/* Summary of performed system calls */
void syscall_summary() {
    int i;
    syscall_debug("\nSystem calls summary:\n");
    for (i = 1; i < 325; i++) {
        if (!syscall_freq[i])
            continue;
        syscall_debug("%s  %lld\n", syscall_name[i],
                (long long) syscall_freq[i]);
    }
}


/* Usually, glibc wrappers to system calls return -1 on error and set the
 * 'errno' variable to the appropriate value. However, the ABI with the
 * operating system is different - a return value less than 0 specifies
 * the error code. We use this macro for this kind of system calls. */
#define RETVAL(X) { retval = (X); if (retval == -1) retval = -errno; }
//unsigned int myarr[10];



/*  Following are the functions which implement systemcalls provided by guest os*/
int get_pid() {
	return isa_ctx->pid;
}

int handle_guest_syscalls() {
	int syscode = isa_regs->eax;
	int retval = 0;
	switch (syscode) {
		case syscall_code_get_pid:
		{
			retval=get_pid();
			break;
		}
		case syscall_code_set_instruction_slice:
		{
			retval = isa_ctx->instr_slice = isa_regs->ebx;
			printf ("Instruction slice is now %d\n", isa_ctx->instr_slice);
			break;
		}
		case syscall_code_disk_io:
		{
			int op = isa_regs->ebx;
			int numbytes = isa_regs->ecx; 
			uint32_t addr = isa_regs->edx;
			int blocknum = isa_regs->esi;
			int offset = isa_regs->edi;
			
			printf("Process of user %d attempting to write on block %d.\n", isa_ctx->uid, blocknum);
			
			if (blocknum >= numblocks) {
				printf ("Block does not exist.\n");
				return -1;
			}
			if (blockowners[blocknum]!=0 && blockowners[blocknum]!=isa_ctx->uid) {
				printf ("This block belongs to another user.\n");
				return -2;
			}
			else if (offset + numbytes > blocksize) {
				printf ("Intended I/O exceeds block size.\n");
				return -3;
			}
			
			char *data = (char*)(malloc(numbytes));
			FILE *disk = fopen ("Sim_disk","r+");
			fseek (disk, blocknum*blocksize + offset, SEEK_SET);
			
			if (op) {//Read mode
				if (blockowners[blocknum]==0) {
					printf ("This block is not allocated to any user.\n");
					return -2;
				}
				fread(data,1,numbytes,disk);
				mem_write_string(isa_mem, addr,data);
			}
			else {//Write mode
				if (blockowners[blocknum]==0) {
					printf ("Block now allocated to this user.\n");
					blockowners[blocknum] = isa_ctx->uid;
				}
				mem_read_string(isa_mem, addr, numbytes, data);
				fwrite(data,1,numbytes,disk);
			}
			
			fclose(disk);
			retval = 0;
			send_to_io(isa_ctx, 30);
			
			break;
		}
		default:
			if (syscode >= syscall_code_count) {
				 retval = -38;
			} else {
				 fatal("not implemented system call '%s' (code %d) at 0x%x\n%s",
							syscode < syscall_code_count ? syscall_name[syscode] : "",
							syscode, isa_regs->eip, err_syscall_note);
			}
	}

	return retval;
}

/* Simulation of system calls.
 * The system call code is in eax.
 * The parameters are given in ebx, ecx, edx, esi, edi, ebp.
 * The return value is placed in eax. */
void syscall_do() {
    int syscode = isa_regs->eax;
    int retval = 0;
    if (syscode > 325) {
        retval = handle_guest_syscalls();
    } else // Pass system call to host os
    {



        //printf("\n In syscall.c +syscall_do(),**** val of syscode is %d and syscall_code_count is %d ",syscode,syscall_code_count);
        //	        printf("\n \n ######### syscal_count val is %d####\n",syscall_code_count);
        /* Debug in syscall and call logs */
        syscall_debug("syscall '%s' (code %d, inst %lld, pid %d)\n",
                syscode < syscall_code_count ? syscall_name[syscode] : "",
                syscode, (long long) isa_inst_count, isa_ctx->pid);
        if (syscode < syscall_code_count)
            syscall_freq[syscode]++;
        if (debug_status(isa_call_debug_category)) {
            int i;
            for (i = 0; i < isa_function_level; i++)
                isa_call_debug("| ");
            isa_call_debug("syscall '%s' (code %d, inst %lld, pid %d)\n",
                    syscode < syscall_code_count ? syscall_name[syscode] : "",
                    syscode, (long long) isa_inst_count, isa_ctx->pid);
        }

        switch (syscode) {


                /* 1 */
            case syscall_code_exit:
            {
                int status;

                status = isa_regs->ebx;
                syscall_debug("  status=0x%x\n", status);
                ctx_finish(isa_ctx, status);
                break;
            }

                /* 2 */
            case syscall_code_close:
            {
                uint32_t guest_fd, host_fd;
                struct fd_t *fd;

                guest_fd = isa_regs->ebx;
                syscall_debug("  guest_fd=%d\n", guest_fd);
                host_fd = fdt_get_host_fd(isa_ctx->fdt, guest_fd);
                syscall_debug("  host_fd=%d\n", host_fd);

                /* Get file descriptor table entry. */
                fd = fdt_entry_get(isa_ctx->fdt, guest_fd);
                if (!fd) {
                    retval = -EBADF;
                    break;
                }

                /* Close host file descriptor only if it is valid and not stdin/stdout/stderr. */
                if (host_fd > 2)
                    close(host_fd);

                /* Free guest file descriptor. This will delete the host file if it's a virtual file. */
                if (fd->kind == fd_kind_virtual)
                    syscall_debug("    host file '%s': temporary file deleted\n", fd->path);
                fdt_entry_free(isa_ctx->fdt, fd->guest_fd);
                break;
            }


                /* 3 */
            case syscall_code_read:
            {
                uint32_t pbuf, count;
                int guest_fd, host_fd, err;
                void *buf;
                struct fd_t *fd;
                struct pollfd fds;

                /* Get parameters */
                guest_fd = isa_regs->ebx;
                pbuf = isa_regs->ecx;
                count = isa_regs->edx;
                syscall_debug("  guest_fd=%d, pbuf=0x%x, count=0x%x\n",
                        guest_fd, pbuf, count);

                /* Get file descriptor */
                fd = fdt_entry_get(isa_ctx->fdt, guest_fd);
                if (!fd) {
                    retval = -EBADF;
                    break;
                }
                host_fd = fd->host_fd;
                syscall_debug("  host_fd=%d\n", host_fd);

                /* Allocate buffer */
                buf = calloc(1, count);
                if (!buf)
                    fatal("syscall read: cannot allocate buffer");

                /* Poll the file descriptor to check if read is blocking */
                fds.fd = host_fd;
                fds.events = POLLIN;
                err = poll(&fds, 1, 0);
                if (err < 0)
                    fatal("syscall 'read': error in 'poll'");

                /* Non-blocking read */
                if (fds.revents || (fd->flags & O_NONBLOCK)) {
                    RETVAL(read(host_fd, buf, count));
                    if (retval > 0) {
                        mem_write(isa_mem, pbuf, retval, buf);
                        syscall_debug_string("  buf", buf, count, 1);
                    }
                    free(buf);
                    break;
                }

                /* Blocking read - suspend thread */
                syscall_debug("  blocking read - process suspended\n");
                isa_ctx->wakeup_fd = guest_fd;
                isa_ctx->wakeup_events = 1; /* POLLIN */
                ctx_set_status(isa_ctx, ctx_suspended | ctx_read);
                ke_process_events_schedule();

                free(buf);
                break;
            }


                /* 4 */
            case syscall_code_write:
            {
                uint32_t pbuf, count;
                int guest_fd, host_fd;
                struct fd_t *fd;
                void *buf;
                struct pollfd fds;

                guest_fd = isa_regs->ebx;
                pbuf = isa_regs->ecx;
                count = isa_regs->edx;
                syscall_debug("  guest_fd=%d, pbuf=0x%x, count=0x%x\n",
                        guest_fd, pbuf, count);

                /* Get file descriptor */
                fd = fdt_entry_get(isa_ctx->fdt, guest_fd);
                if (!fd) {
                    retval = -EBADF;
                    break;
                }
                host_fd = fd->host_fd;
                syscall_debug("  host_fd=%d\n", host_fd);

                /* Read buffer from memory */
                buf = malloc(count);
                if (!buf)
                    fatal("syscall write: out of memory");
                mem_read(isa_mem, pbuf, count, buf);
                syscall_debug_string("  buf", buf, count, 0);

                /* Poll the file descriptor to check if write is blocking */
                fds.fd = host_fd;
                fds.events = POLLOUT;
                poll(&fds, 1, 0);

                /* Non-blocking write */
                if (fds.revents) {
                    RETVAL(write(host_fd, buf, count));
                    free(buf);
                    break;
                }

                /* Blocking write - suspend thread */
                syscall_debug("  blocking write - process suspended\n");
                isa_ctx->wakeup_fd = guest_fd;
                ctx_set_status(isa_ctx, ctx_suspended | ctx_write);
                ke_process_events_schedule();
                free(buf);
                break;
            }

                /* 5 */
            case syscall_code_open:
            {
                char filename[MAX_PATH_SIZE], fullpath[MAX_PATH_SIZE], temppath[MAX_PATH_SIZE];
                uint32_t pfilename;
                int flags, mode;
                char sflags[MAX_STRING_SIZE];
                int length, fullpath_length;
                int host_fd;
                struct fd_t *fd;

                /* Read parameters */
                pfilename = isa_regs->ebx;
                flags = isa_regs->ecx;
                mode = isa_regs->edx;
                length = mem_read_string(isa_mem, pfilename, MAX_PATH_SIZE, filename);
                if (length >= MAX_PATH_SIZE)
                    fatal("syscall open: maximum path length exceeded");
                ld_get_full_path(isa_ctx, filename, fullpath, MAX_PATH_SIZE);
                fullpath_length = strlen(fullpath);
                syscall_debug("  filename='%s' flags=0x%x, mode=0x%x\n",
                        filename, flags, mode);
                syscall_debug("  fullpath='%s'\n", fullpath);
                map_flags(&open_flags_map, flags, sflags, MAX_STRING_SIZE);
                syscall_debug("  flags=%s\n", sflags);

                /* Intercept attempt to access OpenCL library and redirect to 'm2s-opencl.so' */
                gk_libopencl_redirect(fullpath, MAX_PATH_SIZE);

                /* Virtual files */
                if (!strncmp(fullpath, "/proc/", 6)) {

                    /* File /proc/self/maps */
                    if (!strcmp(fullpath, "/proc/self/maps")) {

                        /* Create temporary file and open it. */
                        ctx_gen_proc_self_maps(isa_ctx, temppath);
                        host_fd = open(temppath, flags, mode);
                        assert(host_fd > 0);

                        /* Add file descriptor table entry. */
                        fd = fdt_entry_new(isa_ctx->fdt, fd_kind_virtual, host_fd, temppath, flags);
                        syscall_debug("    host file '%s' opened: guest_fd=%d, host_fd=%d\n",
                                temppath, fd->guest_fd, fd->host_fd);
                        retval = fd->guest_fd;
                        break;
                    }

                    /* Unhandled virtual file. Let the application read the contents of the host
                     * version of the file as if it was a regular file. */
                    syscall_debug("    warning: unhandled virtual file\n");
                }

                /* Regular file. */
                host_fd = open(fullpath, flags, mode);
                if (host_fd < 0) {
                    retval = -errno;
                    break;
                }

                /* File opened, create a new file descriptor. */
                fd = fdt_entry_new(isa_ctx->fdt, fd_kind_regular, host_fd, fullpath, flags);
                syscall_debug("    file descriptor opened: guest_fd=%d, host_fd=%d\n",
                        fd->guest_fd, fd->host_fd);
                retval = fd->guest_fd;

                /* Return */
                break;
            }


                /* 7 */
            case syscall_code_waitpid:
            {
                int pid, options;
                uint32_t pstatus;
                char soptions[0x100];
                struct ctx_t *child;

                pid = isa_regs->ebx;
                pstatus = isa_regs->ecx;
                options = isa_regs->edx;
                syscall_debug("  pid=%d, pstatus=0x%x, options=0x%x\n",
                        pid, pstatus, options);
                map_flags(&waitpid_options_map, options, soptions, 0x100);
                syscall_debug("  options=%s\n", soptions);
                if (pid != -1 && pid <= 0)
                    fatal("syscall waitpid: only supported for pid=-1 or pid>0");

                /* Look for a zombie child. */
                child = ctx_get_zombie(isa_ctx, pid);

                /* If there is no child and the flag WNOHANG was not specified,
                 * we get suspended until the specified child finishes. */
                if (!child && !(options & 0x1)) {
                    isa_ctx->wakeup_pid = pid;
                    ctx_set_status(isa_ctx, ctx_suspended | ctx_waitpid);
                    break;
                }

                /* Context is not suspended. WNOHANG was specified, or some child
                 * was found in the zombie list. */
                if (child) {
                    retval = child->pid;
                    if (pstatus)
                        mem_write(isa_mem, pstatus, 4, &child->exit_code);
                    ctx_set_status(child, ctx_finished);
                }
                break;
            }


                /* 10 */
            case syscall_code_unlink:
            {
                char filename[MAX_PATH_SIZE], fullpath[MAX_PATH_SIZE];
                uint32_t pfilename;
                int length;

                pfilename = isa_regs->ebx;
                length = mem_read_string(isa_mem, pfilename, MAX_PATH_SIZE, filename);
                if (length >= MAX_PATH_SIZE)
                    fatal("syscall unlink: maximum path length exceeded");
                ld_get_full_path(isa_ctx, filename, fullpath, MAX_PATH_SIZE);
                syscall_debug("  pfilename=0x%x\n", pfilename);
                syscall_debug("  filename=%s, fullpath=%s\n", filename, fullpath);

                RETVAL(unlink(fullpath));
                break;
            }


                /* 13 */
            case syscall_code_time:
            {
                uint32_t ptime, t;

                ptime = isa_regs->ebx;
                syscall_debug("  ptime=0x%x\n", ptime);
                t = time(NULL);
                if (ptime)
                    mem_write(isa_mem, ptime, 4, &t);
                retval = t;
                break;
            }


                /* 15 */
            case syscall_code_chmod:
            {
                char filename[MAX_PATH_SIZE], fullpath[MAX_PATH_SIZE];
                uint32_t pfilename, mode;
                int len;

                pfilename = isa_regs->ebx;
                mode = isa_regs->ecx;
                len = mem_read_string(isa_mem, pfilename, MAX_PATH_SIZE, filename);
                if (len >= MAX_PATH_SIZE)
                    fatal("syscall chmod: maximum path length exceeded");
                ld_get_full_path(isa_ctx, filename, fullpath, MAX_PATH_SIZE);
                syscall_debug("  pfilename=0x%x, mode=0x%x\n", pfilename, mode);
                syscall_debug("  filename='%s', fullpath='%s'\n", filename, fullpath);
                RETVAL(chmod(fullpath, mode));
                break;
            }


                /* 19 */
            case syscall_code_lseek:
            {
                uint32_t fd, offset, origin, host_fd;

                fd = isa_regs->ebx;
                offset = isa_regs->ecx;
                origin = isa_regs->edx;
                host_fd = fdt_get_host_fd(isa_ctx->fdt, fd);
                syscall_debug("  fd=%d, offset=0x%x, origin=0x%x\n",
                        fd, offset, origin);
                syscall_debug("  host_fd=%d\n", host_fd);

                RETVAL(lseek(host_fd, offset, origin));
                break;
            }


                /* 20 */
            case syscall_code_getpid:
            {
                retval = isa_ctx->pid;
                break;
            }


                /* 30 */
            case syscall_code_utime:
            {
                char filename[MAX_PATH_SIZE], fullpath[MAX_PATH_SIZE];
                uint32_t pfilename, putimbuf;
                struct utimbuf utimbuf;
                struct sim_utimbuf sim_utimbuf;
                int len;

                pfilename = isa_regs->ebx;
                putimbuf = isa_regs->ecx;
                len = mem_read_string(isa_mem, pfilename, MAX_PATH_SIZE, filename);
                if (len >= MAX_PATH_SIZE)
                    fatal("syscall utime: maximum path length exceeded");
                ld_get_full_path(isa_ctx, filename, fullpath, MAX_PATH_SIZE);
                mem_read(isa_mem, putimbuf, sizeof (struct sim_utimbuf), &sim_utimbuf);
                syscall_utime_sim_to_read(&utimbuf, &sim_utimbuf);
                syscall_debug("  filename='%s', putimbuf=0x%x\n",
                        filename, putimbuf);
                syscall_debug("  fullpath='%s'\n", fullpath);
                syscall_debug("  utimbuf.actime = %u, utimbuf.modtime = %u\n",
                        sim_utimbuf.actime, sim_utimbuf.modtime);
                RETVAL(utime(fullpath, &utimbuf));
                break;
            }


                /* 33 */
            case syscall_code_access:
            {
                char filename[MAX_PATH_SIZE], fullpath[MAX_PATH_SIZE], smode[MAX_STRING_SIZE];
                uint32_t pfilename, mode;
                int len;

                pfilename = isa_regs->ebx;
                mode = isa_regs->ecx;
                len = mem_read_string(isa_mem, pfilename, MAX_PATH_SIZE, filename);
                if (len >= MAX_PATH_SIZE)
                    fatal("syscall access: maximum path length exceeded");
                ld_get_full_path(isa_ctx, filename, fullpath, MAX_PATH_SIZE);
                map_flags(&access_mode_map, mode, smode, MAX_STRING_SIZE);
                syscall_debug("  filename='%s', mode=0x%x\n",
                        filename, mode);
                syscall_debug("  fullpath='%s'\n", fullpath);
                syscall_debug("  mode=%s\n", smode);

                RETVAL(access(fullpath, mode));
                break;
            }


                /* 37 */
            case syscall_code_kill:
            {
                uint32_t pid, sig;
                struct ctx_t *ctx;

                pid = isa_regs->ebx;
                sig = isa_regs->ecx;
                syscall_debug("  pid=%d, sig=%d (%s)\n", pid,
                        sig, sim_signal_name(sig));

                /* Find ctx. If it is not found, assume own fault rather than
                 * program's, so generate fatal error. */
                ctx = ctx_get(pid);
                if (!ctx)
                    fatal("syscall kill: pid %d does not exist", pid);

                /* Send signal */
                sim_sigset_add(&ctx->signal_masks->pending, sig);
                ctx_host_thread_suspend_cancel(ctx); /* Target ctx might wake up */
                ke_process_events_schedule();
                ke_process_events();
                break;
            }


                /* 38 */
            case syscall_code_rename:
            {
                uint32_t poldpath, pnewpath;
                char oldpath[MAX_PATH_SIZE], newpath[MAX_PATH_SIZE];
                char oldfullpath[MAX_PATH_SIZE], newfullpath[MAX_PATH_SIZE];
                int len1, len2;

                poldpath = isa_regs->ebx;
                pnewpath = isa_regs->ecx;
                len1 = mem_read_string(isa_mem, poldpath, MAX_PATH_SIZE, oldpath);
                len2 = mem_read_string(isa_mem, pnewpath, MAX_PATH_SIZE, newpath);
                if (len1 >= MAX_PATH_SIZE || len2 >= MAX_PATH_SIZE)
                    fatal("syscall rename: maximum path length exceeded");
                ld_get_full_path(isa_ctx, oldpath, oldfullpath, MAX_PATH_SIZE);
                ld_get_full_path(isa_ctx, newpath, newfullpath, MAX_PATH_SIZE);
                syscall_debug("  poldpath=0x%x, pnewpath=0x%x\n", poldpath, pnewpath);
                syscall_debug("  oldpath='%s', newpath='%s'\n", oldpath, newpath);
                syscall_debug("  oldfullpath='%s', newfullpath='%s'\n", oldfullpath, newfullpath);

                RETVAL(rename(oldfullpath, newfullpath));
                break;
            }


                /* 39 */
            case syscall_code_mkdir:
            {
                uint32_t ppath, mode;
                char path[MAX_PATH_SIZE], fullpath[MAX_PATH_SIZE];
                int length;

                ppath = isa_regs->ebx;
                mode = isa_regs->ecx;
                length = mem_read_string(isa_mem, ppath, MAX_PATH_SIZE, path);
                if (length >= MAX_PATH_SIZE)
                    fatal("syscall open: maximum path length exceeded");
                ld_get_full_path(isa_ctx, path, fullpath, MAX_PATH_SIZE);
                syscall_debug("  ppath=0x%x, mode=0x%x\n", ppath, mode);
                syscall_debug("  path='%s', fullpath='%s'\n", path, fullpath);

                RETVAL(mkdir(fullpath, mode));
                break;
            }


                /* 41 */
            case syscall_code_dup:
            {
                int guest_fd, dup_guest_fd;
                int host_fd, dup_host_fd;
                struct fd_t *fd, *dup_fd;

                guest_fd = isa_regs->ebx;
                syscall_debug("  guest_fd=%d\n", guest_fd);

                /* Check that file descriptor is valid. */
                fd = fdt_entry_get(isa_ctx->fdt, guest_fd);
                if (!fd) {
                    retval = -EBADF;
                    break;
                }
                host_fd = fd->host_fd;
                syscall_debug("  host_fd=%d\n", host_fd);

                /* Duplicate host file descriptor. */
                dup_host_fd = dup(host_fd);
                if (dup_host_fd < 0) {
                    retval = -errno;
                    break;
                }

                /* Create a new entry in the file descriptor table. */
                dup_fd = fdt_entry_new(isa_ctx->fdt, fd_kind_regular, dup_host_fd, fd->path, fd->flags);
                dup_guest_fd = dup_fd->guest_fd;

                /* Return new file descriptor. */
                retval = dup_guest_fd;
                break;
            }


                /* 42 */
            case syscall_code_pipe:
            {
                uint32_t pfd;
                struct fd_t *read_fd, *write_fd;
                uint32_t guest_read_fd, guest_write_fd;
                int host_fd[2], err;

                pfd = isa_regs->ebx;
                syscall_debug("  pfd=0x%x\n", pfd);

                /* Create host pipe */
                err = pipe(host_fd);
                if (err < 0)
                    fatal("syscall 'pipe': could not create pipe");
                syscall_debug("  host pipe created: fd={%d, %d}\n",
                        host_fd[0], host_fd[1]);

                /* Create guest pipe */
                read_fd = fdt_entry_new(isa_ctx->fdt, fd_kind_pipe, host_fd[0], "", O_RDONLY);
                write_fd = fdt_entry_new(isa_ctx->fdt, fd_kind_pipe, host_fd[1], "", O_WRONLY);
                syscall_debug("  pipe created: fd={%d, %d}\n",
                        read_fd->guest_fd, write_fd->guest_fd);
                guest_read_fd = read_fd->guest_fd;
                guest_write_fd = write_fd->guest_fd;

                /* Return file descriptors. */
                mem_write(isa_mem, pfd, 4, &guest_read_fd);
                mem_write(isa_mem, pfd + 4, 4, &guest_write_fd);
                break;
            }


                /* 43 */
            case syscall_code_times:
            {
                uint32_t ptms;
                struct tms tms;
                struct sim_tms sim_tms;

                ptms = isa_regs->ebx;
                syscall_debug("  ptms=0x%x\n", ptms);

                retval = times(&tms);
                syscall_copy_tms(&sim_tms, &tms);
                if (ptms)
                    mem_write(isa_mem, ptms, sizeof (sim_tms), &sim_tms);
                break;
            }


                /* 45 */
            case syscall_code_brk:
            {
                uint32_t oldbrk, newbrk, size;
                uint32_t oldbrk_rnd, newbrk_rnd;

                newbrk = isa_regs->ebx;
                oldbrk = isa_ctx->loader->brk;
                syscall_debug("  newbrk=0x%x (previous brk was 0x%x)\n",
                        newbrk, oldbrk);

                newbrk_rnd = ROUND_UP(newbrk, MEM_PAGESIZE);
                oldbrk_rnd = ROUND_UP(oldbrk, MEM_PAGESIZE);

                /* If argument is zero, the system call is used to
                 * obtain the current top of the heap. */
                if (!newbrk) {
                    retval = oldbrk;
                    break;
                }

                /* If the heap is increased: if some page in the way is
                 * allocated, do nothing and return old heap top. Otherwise,
                 * allocate pages and return new heap top. */
                if (newbrk > oldbrk) {
                    size = newbrk_rnd - oldbrk_rnd;
                    if (size) {
                        if (mem_map_space(isa_mem, oldbrk_rnd, size) != oldbrk_rnd)
                            fatal("syscall brk: out of memory");
                        mem_map(isa_mem, oldbrk_rnd, size,
                                mem_access_read | mem_access_write);
                    }
                    isa_ctx->loader->brk = newbrk;
                    retval = newbrk;
                    syscall_debug("  heap grows 0x%x bytes\n", newbrk - oldbrk);
                    break;
                }

                /* Always allow to shrink the heap. */
                if (newbrk < oldbrk) {
                    size = oldbrk_rnd - newbrk_rnd;
                    if (size)
                        mem_unmap(isa_mem, newbrk_rnd, size);
                    isa_ctx->loader->brk = newbrk;
                    retval = newbrk;
                    syscall_debug("  heap shrinks 0x%x bytes\n", oldbrk - newbrk);
                    break;
                }
                break;
            }


                /* 54 */
                /* An 'ioctl' code (first argument) is a 32-bit word split into 4 fields:
                 *   -NR [7..0]: ioctl code number.
                 *   -TYPE [15..8]: ioctl category.
                 *   -SIZE [29..16]: size of the structure passed as 2nd argument.
                 *   -DIR [31..30]: direction (01=Write, 10=Read, 11=R/W).
                 */
            case syscall_code_ioctl:
            {
                uint32_t cmd, arg;
                int guest_fd;
                struct fd_t *fd;

                guest_fd = isa_regs->ebx;
                cmd = isa_regs->ecx;
                arg = isa_regs->edx;
                syscall_debug("  guest_fd=%d, cmd=0x%x, arg=0x%x\n",
                        guest_fd, cmd, arg);

                /* File descriptor */
                fd = fdt_entry_get(isa_ctx->fdt, guest_fd);
                if (!fd) {
                    retval = -EBADF;
                    break;
                }

                /* Process IOCTL */
                if (cmd >= 0x5401 || cmd <= 0x5408) {

                    /* 'ioctl' commands using 'struct termios' as the argument.
                     * This structure is 60 bytes long both for x86 and x86_64
                     * architectures, so it doesn't vary between guest/host.
                     * No translation needed, so just use a 60-byte I/O buffer. */
                    unsigned char buf[60];

                    mem_read(isa_mem, arg, sizeof (buf), buf);
                    RETVAL(ioctl(fd->host_fd, cmd, &buf));
                    if (!retval)
                        mem_write(isa_mem, arg, sizeof (buf), buf);

                } else
                    fatal("syscall ioctl: cmd = 0x%x not implemented", cmd);

                break;
            }


                /* 64 */
            case syscall_code_getppid:
            {
                /* Return a pid of 1 if there is no parent */
                if (!isa_ctx->parent)
                    retval = 1;
                else
                    retval = isa_ctx->parent->pid;
                break;
            }


                /* 75 */
            case syscall_code_setrlimit:
            {
                uint32_t resource, prlim;
                char *sresource;
                struct sim_rlimit sim_rlimit;

                resource = isa_regs->ebx;
                prlim = isa_regs->ecx;
                sresource = map_value(&rlimit_resource_map, resource);
                syscall_debug("  resource=0x%x, prlim=0x%x\n", resource, prlim);
                syscall_debug("  resource=%s\n", sresource);

                mem_read(isa_mem, prlim, sizeof (struct sim_rlimit), &sim_rlimit);
                syscall_debug("  rlim->cur=0x%x, rlim->max=0x%x\n",
                        sim_rlimit.cur, sim_rlimit.max);
                switch (resource) {

                    case RLIMIT_DATA:
                        /* Default limit is maximum.
                         * This system call is ignored. */
                        break;

                    case RLIMIT_STACK:
                    {
                        /* A program should allocate its stack with calls to mmap.
                         * This should be a limit for the stack, which is ignored here. */
                        break;
                    }

                    default:
                        fatal("setrlimit: not implemented for resource=%s", sresource);
                }
                break;
            }


                /* 77 */
            case syscall_code_getrusage:
            {
                uint32_t who, pru;
                struct rusage rusage;
                struct sim_rusage sim_rusage;

                who = isa_regs->ebx;
                pru = isa_regs->ecx;
                syscall_debug("  who=0x%x, pru=0x%x\n", who, pru);

                if (who != 0) /* RUSAGE_SELF */
                    fatal("syscall getrusage: not implemented for who != RUSAGE_SELF");
                RETVAL(getrusage(RUSAGE_SELF, &rusage));
                if (!retval) {
                    syscall_copy_rusage(&sim_rusage, &rusage);
                    mem_write(isa_mem, pru, sizeof (sim_rusage), &sim_rusage);
                }
                /* FIXME: update these values
                 * ru_maxrss: maximum resident set size
                 * ru_ixrss: integral shared memory size
                 * ru_idrss: integral unshared data size
                 * ru_isrss: integral unshared stack size */
                break;
            }


                /* 78 */
            case syscall_code_gettimeofday:
            {
                uint32_t ptv, ptz;
                struct timeval tv;
                struct timezone tz;

                ptv = isa_regs->ebx;
                ptz = isa_regs->ecx;
                syscall_debug("  ptv=0x%x, ptz=0x%x\n", ptv, ptz);

                RETVAL(gettimeofday(&tv, &tz));
                if (ptv) {
                    mem_write(isa_mem, ptv, 4, &tv.tv_sec);
                    mem_write(isa_mem, ptv + 4, 4, &tv.tv_usec);
                }
                if (ptz) {
                    mem_write(isa_mem, ptz, 4, &tz.tz_minuteswest);
                    mem_write(isa_mem, ptz + 4, 4, &tz.tz_dsttime);
                }
                break;
            }


                /* 85 */
            case syscall_code_readlink:
            {
                uint32_t path, buf, bufsz;
                char path_str[MAX_PATH_SIZE];
                char full_path_str[MAX_PATH_SIZE];
                char dest_path[MAX_PATH_SIZE];
                int dest_size;

                path = isa_regs->ebx;
                buf = isa_regs->ecx;
                bufsz = isa_regs->edx;
                syscall_debug("  path=0x%x, buf=0x%x, bufsz=%d\n", path, buf, bufsz);

                /* Read path */
                mem_read_string(isa_mem, path, MAX_PATH_SIZE, path_str);
                ld_get_full_path(isa_ctx, path_str, full_path_str, MAX_PATH_SIZE);
                syscall_debug("  path_str='%s'\n", path_str);

                /* Special file '/proc/self/exe' intercepted */
                if (!strcmp(full_path_str, "/proc/self/exe")) {

                    /* Return path to simulated executable */
                    strcpy(dest_path, isa_ctx->loader->exe);

                } else {

                    /* Perform host call */
                    RETVAL(readlink(full_path_str, dest_path, MAX_PATH_SIZE));
                    if (retval < 0)
                        break;
                }

                /* Check if guest allocated enough space */
                dest_size = strlen(dest_path) + 1;
                if (dest_size > bufsz) {
                    retval = -EFAULT;
                    break;
                }

                /* Copy host buffer to guest */
                mem_write_string(isa_mem, buf, dest_path);
                syscall_debug("  dest_path='%s'\n", dest_path);
                break;
            }


                /* 90 */
            case syscall_code_mmap:
            {
                uint32_t pargs;
                uint32_t addr, len, prot;
                uint32_t flags, offset;
                int guest_fd;
                char sprot[MAX_STRING_SIZE], sflags[MAX_STRING_SIZE];

                /* 'old_mmap' takes the arguments from memory, at the address
                 * pointed by EBX. */
                pargs = isa_regs->ebx;
                mem_read(isa_mem, pargs, 4, &addr);
                mem_read(isa_mem, pargs + 4, 4, &len);
                mem_read(isa_mem, pargs + 8, 4, &prot);
                mem_read(isa_mem, pargs + 12, 4, &flags);
                mem_read(isa_mem, pargs + 16, 4, &guest_fd);
                mem_read(isa_mem, pargs + 20, 4, &offset);

                syscall_debug("  pargs=0x%x\n", pargs);
                syscall_debug("  addr=0x%x, len=%u, prot=0x%x, flags=0x%x, "
                        "guest_fd=%d, offset=0x%x\n",
                        addr, len, prot, flags, guest_fd, offset);
                map_flags(&mmap_prot_map, prot, sprot, sizeof (sprot));
                map_flags(&mmap_flags_map, flags, sflags, sizeof (sflags));
                syscall_debug("  prot=%s, flags=%s\n", sprot, sflags);

                /* Map */
                retval = do_mmap(addr, len, prot, flags,
                        guest_fd, offset);
                break;
            }


                /* 91 */
            case syscall_code_munmap:
            {
                uint32_t addr, size;
                uint32_t size_align;

                addr = isa_regs->ebx;
                size = isa_regs->ecx;
                syscall_debug("  addr=0x%x, size=0x%x\n", addr, size);
                if (addr & (MEM_PAGESIZE - 1))
                    fatal("munmap: size is not a multiple of page size");

                size_align = ROUND_UP(size, MEM_PAGESIZE);
                mem_unmap(isa_mem, addr, size_align);
                break;
            }


                /* 94 */
            case syscall_code_fchmod:
            {
                uint32_t fd, host_fd, mode;

                fd = isa_regs->ebx;
                mode = isa_regs->ecx;
                host_fd = fdt_get_host_fd(isa_ctx->fdt, fd);
                syscall_debug("  fd=%d, mode=%d\n",
                        fd, mode);
                syscall_debug("  host_fd=%d\n", host_fd);

                RETVAL(fchmod(host_fd, mode));
                break;
            }


                /* 102 */
            case syscall_code_socketcall:
            {
                int call;
                uint32_t args;
                char *call_name;

                call = isa_regs->ebx;
                args = isa_regs->ecx;
                call_name = map_value(&socketcall_call_map, call);
                syscall_debug("  call=%d (%s)\n", call, call_name);
                syscall_debug("  args=0x%x\n", args);

                /* Process call */
                if (call == 1) { /* SYS_SOCKET */

                    uint32_t family, type, protocol;
                    char *family_name, *type_name;
                    int host_fd;
                    struct fd_t *fd;

                    /* Read parameters */
                    mem_read(isa_mem, args, 4, &family);
                    mem_read(isa_mem, args + 4, 4, &type);
                    mem_read(isa_mem, args + 8, 4, &protocol);
                    family_name = map_value(&socket_family_map, family);
                    type_name = map_value(&socket_type_map, type & 0xff);
                    syscall_debug("  family=0x%x, type=0x%x, protocol=0x%x\n",
                            family, type, protocol);
                    syscall_debug("    family=%s\n", family_name);
                    syscall_debug("    type=%s", type_name);
                    if (type & 0x80000) /* SOCK_CLOEXEC */
                        syscall_debug("|SOCK_CLOEXEC");
                    if (type & 0x800) /* SOCK_NONBLOCK */
                        syscall_debug("|SOCK_NONBLOCK");
                    syscall_debug("\n");

                    /* Allow only sockets of type SOCK_STREAM */
                    if ((type & 0xff) != 1)
                        fatal("syscall 'socketcall': SYS_SOCKET: only sockets of type SOCK_STREAM allowed");

                    /* Create socket */
                    host_fd = socket(family, type, protocol);
                    if (host_fd < 0) {
                        retval = -errno;
                        break;
                    }

                    /* Create new file descriptor table entry. */
                    fd = fdt_entry_new(isa_ctx->fdt, fd_kind_socket, host_fd, "", O_RDWR);
                    syscall_debug("    file descriptor opened: guest_fd=%d, host_fd=%d\n",
                            fd->guest_fd, fd->host_fd);
                    retval = fd->guest_fd;
                    break;

                } else if (call == 3) { /* SYS_CONNECT */

                    uint32_t guest_fd, paddr, addrlen;
                    struct fd_t *fd;
                    char buf[MAX_STRING_SIZE];
                    struct sockaddr *addr;

                    mem_read(isa_mem, args, 4, &guest_fd);
                    mem_read(isa_mem, args + 4, 4, &paddr);
                    mem_read(isa_mem, args + 8, 4, &addrlen);
                    syscall_debug("  guest_fd=%d, paddr=0x%x, addrlen=%d\n",
                            guest_fd, paddr, addrlen);

                    /* Get sockaddr structure - read family and data */
                    if (addrlen > MAX_STRING_SIZE)
                        fatal("syscall 'socketcall': SYS_CONNECT: maximum string size exceeded");
                    addr = (struct sockaddr *) &buf[0];
                    assert(sizeof (addr->sa_family) == 2);
                    assert((void *) &addr->sa_data - (void *) &addr->sa_family == 2);
                    mem_read(isa_mem, paddr, addrlen, addr);
                    syscall_debug("    sockaddr.family=%s\n", map_value(&socket_family_map, addr->sa_family));
                    syscall_debug_string("    sockaddr.data", addr->sa_data, addrlen - 2, 1);

                    /* Get file descriptor */
                    fd = fdt_entry_get(isa_ctx->fdt, guest_fd);
                    if (!fd) {
                        retval = -EBADF;
                        break;
                    }
                    if (fd->kind != fd_kind_socket)
                        fatal("  syscall 'socketcall': SYS_CONNECT: file descriptor is not a socket");
                    syscall_debug("    host_fd=%d\n", fd->host_fd);

                    /* Connect socket */
                    RETVAL(connect(fd->host_fd, addr, addrlen));
                    break;

                } else if (call == 7) { /* SYS_GETPEERNAME */

                    uint32_t guest_fd, paddr, paddrlen, addrlen;
                    struct fd_t *fd;
                    struct sockaddr *addr;
                    socklen_t host_addrlen;

                    mem_read(isa_mem, args, 4, &guest_fd);
                    mem_read(isa_mem, args + 4, 4, &paddr);
                    mem_read(isa_mem, args + 8, 4, &paddrlen);
                    syscall_debug("  guest_fd=%d, paddr=0x%x, paddrlen=0x%x\n",
                            guest_fd, paddr, paddrlen);

                    /* Get file descriptor */
                    fd = fdt_entry_get(isa_ctx->fdt, guest_fd);
                    if (!fd) {
                        retval = -EBADF;
                        break;
                    }

                    /* Read current buffer size and allocate buffer. */
                    mem_read(isa_mem, paddrlen, 4, &addrlen);
                    syscall_debug("    addrlen=%d\n", addrlen);
                    host_addrlen = addrlen;
                    addr = malloc(addrlen);

                    /* Get peer name */
                    RETVAL(getpeername(fd->host_fd, addr, &host_addrlen));
                    if (retval < 0) {
                        free(addr);
                        break;
                    }
                    addrlen = host_addrlen;
                    syscall_debug("  result:\n");
                    syscall_debug("    addrlen=%d\n", host_addrlen);
                    syscall_debug_string("    sockaddr.data", addr->sa_data, addrlen - 2, 1);

                    /* Copy result to guest memory */
                    mem_write(isa_mem, paddrlen, 4, &addrlen);
                    mem_write(isa_mem, paddr, addrlen, addr);
                    free(addr);
                    break;

                } else
                    fatal("syscall 'socketcall': call '%s' not implemented",
                        call_name);

                break;
            }


                /* 104 */
            case syscall_code_setitimer:
            {
                uint32_t which, pvalue, povalue;
                struct sim_itimerval itimerval;
                uint64_t now = ke_timer();

                which = isa_regs->ebx;
                pvalue = isa_regs->ecx;
                povalue = isa_regs->edx;
                syscall_debug("  which=%d (%s), pvalue=0x%x, povalue=0x%x\n",
                        which, map_value(&itimer_map, which), pvalue, povalue);

                /* Read value */
                if (pvalue) {
                    mem_read(isa_mem, pvalue, sizeof (itimerval), &itimerval);
                    syscall_debug("  itimerval at 'pvalue':\n");
                    sim_itimerval_debug(&itimerval);
                }

                /* Check range of 'which' */
                if (which >= 3)
                    fatal("syscall 'setitimer': wrong value for 'which' argument");

                /* Set 'itimer_value' (ke_timer domain) and 'itimer_interval' (usec) */
                isa_ctx->itimer_value[which] = now + itimerval.it_value.tv_sec * 1000000
                        + itimerval.it_value.tv_usec;
                isa_ctx->itimer_interval[which] = itimerval.it_interval.tv_sec * 1000000
                        + itimerval.it_interval.tv_usec;

                /* New timer inserted, so interrupt current 'ke_host_thread_timer'
                 * waiting for the next timer expiration. */
                ctx_host_thread_timer_cancel(isa_ctx);
                ke_process_events_schedule();
                break;
            }


                /* 105 */
            case syscall_code_getitimer:
            {
                uint32_t which, pvalue;
                struct sim_itimerval itimerval;
                uint64_t now = ke_timer();
                uint64_t rem;

                which = isa_regs->ebx;
                pvalue = isa_regs->ecx;
                syscall_debug("  which=%d (%s), pvalue=0x%x\n",
                        which, map_value(&itimer_map, which), pvalue);

                /* Check range of 'which' */
                if (which >= 3)
                    fatal("syscall 'getitimer': wrong value for 'which' argument");

                /* Return value in structure */
                rem = now < isa_ctx->itimer_value[which] ? isa_ctx->itimer_value[which] - now : 0;
                itimerval.it_value.tv_sec = rem / 1000000;
                itimerval.it_value.tv_usec = rem % 1000000;
                itimerval.it_interval.tv_sec = isa_ctx->itimer_interval[which] / 1000000;
                itimerval.it_interval.tv_usec = isa_ctx->itimer_interval[which] % 1000000;
                mem_write(isa_mem, pvalue, sizeof (itimerval), &itimerval);
                break;
            }


                /* 119 */
            case syscall_code_sigreturn:
            {
                signal_handler_return(isa_ctx);
                ke_process_events_schedule();
                ke_process_events();
                break;
            }



                /* 120 */
                /* Prototype: long sys_clone(unsigned long clone_flags, unsigned long newsp,
                 * 	int __user *parent_tid, int unused, int __user *child_tid);
                 * There is an unused parameter, that's why we read child_tidptr from edi
                 * instead of esi. */
            case syscall_code_clone:
            {
                uint32_t flags, newsp, parent_tidptr, child_tidptr;
                uint32_t supported_flags, mandatory_flags;
                struct ctx_t *new_ctx;
                char sflags[MAX_STRING_SIZE];

                flags = isa_regs->ebx;
                newsp = isa_regs->ecx;
                parent_tidptr = isa_regs->edx;
                child_tidptr = isa_regs->edi;
                syscall_debug("  flags=0x%x, newsp=0x%x, parent_tidptr=0x%x, child_tidptr=0x%x\n",
                        flags, newsp, parent_tidptr, child_tidptr);
                map_flags(&clone_flags_map, flags & ~0xff, sflags, MAX_STRING_SIZE);
                syscall_debug("  flags=%s\n", sflags);

                if (!newsp)
                    newsp = isa_regs->esp;

                /* Create new context */
                new_ctx = ctx_clone(isa_ctx);
                retval = new_ctx->pid;
                syscall_debug("  context %d created with pid %d\n",
                        new_ctx->pid, retval);

                /* Check not supported and mandatory flags */
                mandatory_flags = 0x00000f00;
                supported_flags = 0x013d00ff | mandatory_flags;
                if ((flags & mandatory_flags) != mandatory_flags) {
                    map_flags(&clone_flags_map, ~flags & mandatory_flags, sflags, MAX_STRING_SIZE);
                    fatal("syscall clone: these mandatory flags are not specified: %s",
                            sflags);
                }
                if (flags & ~supported_flags) {
                    map_flags(&clone_flags_map, flags & ~supported_flags, sflags, MAX_STRING_SIZE);
                    fatal("syscall clone: one of these flags is specified and not supported: %s",
                            sflags);
                }

                /* Flag CLONE_THREAD.
                 * If specified, the exit signal is ignored. Otherwise, it is specified in the
                 * lower byte of the flags. */
                if (flags & 0x10000) {
                    new_ctx->exit_signal = 0;
                } else {
                    new_ctx->exit_signal = flags & 0xff;
                }

                /* Flag CLONE_PARENT_SETTID */
                if (flags & 0x100000)
                    mem_write(isa_ctx->mem, parent_tidptr, 4, &new_ctx->pid);

                /* Flags CLONE_CHILD_SETTID and CLONE_CHILD_CLEARTID */
                if (flags & 0x1000000)
                    new_ctx->set_child_tid = child_tidptr;
                if (flags & 0x200000)
                    new_ctx->clear_child_tid = child_tidptr;

                /* Flag CLONE_SETTLS */
                if (flags & 0x80000) {
                    struct sim_user_desc uinfo;
                    uint32_t puinfo;

                    puinfo = isa_regs->esi;
                    syscall_debug("  puinfo=0x%x\n", puinfo);

                    mem_read(isa_mem, puinfo, sizeof (struct sim_user_desc), &uinfo);
                    syscall_debug("  entry_number=0x%x, base_addr=0x%x, limit=0x%x\n",
                            uinfo.entry_number, uinfo.base_addr, uinfo.limit);
                    syscall_debug("  seg_32bit=0x%x, contents=0x%x, read_exec_only=0x%x\n",
                            uinfo.seg_32bit, uinfo.contents, uinfo.read_exec_only);
                    syscall_debug("  limit_in_pages=0x%x, seg_not_present=0x%x, useable=0x%x\n",
                            uinfo.limit_in_pages, uinfo.seg_not_present, uinfo.useable);
                    if (!uinfo.seg_32bit)
                        fatal("syscall set_thread_area: only 32-bit segments supported");

                    /* Limit given in pages (4KB units) */
                    if (uinfo.limit_in_pages)
                        uinfo.limit <<= 12;

                    /*if (uinfo.entry_number != 6)
                            fatal("syscall clone: uinfo.entry_number=0x%x (!= 6)", uinfo.entry_number);*/
                    /// FIXME? ///
                    uinfo.entry_number = 6;
                    mem_write(isa_mem, puinfo, 4, &uinfo.entry_number);

                    new_ctx->glibc_segment_base = uinfo.base_addr;
                    new_ctx->glibc_segment_limit = uinfo.limit;
                }

                /* New context returns 0. */
                new_ctx->initial_stack = newsp;
                new_ctx->regs->esp = newsp;
                new_ctx->regs->eax = 0;

                break;
            }


                /* 122 */
            case syscall_code_newuname:
            {
                uint32_t putsname;

                putsname = isa_regs->ebx;
                syscall_debug("  putsname=0x%x\n", putsname);
                syscall_debug("  sysname='%s', nodename='%s'\n", sim_utsname.sysname, sim_utsname.nodename);
                syscall_debug("  relaese='%s', version='%s'\n", sim_utsname.release, sim_utsname.version);
                syscall_debug("  machine='%s', domainname='%s'\n", sim_utsname.machine, sim_utsname.domainname);

                mem_write(isa_mem, putsname, sizeof (sim_utsname), &sim_utsname);
                break;
            }


                /* 125 */
            case syscall_code_mprotect:
            {
                uint32_t start, len, prot;
                enum mem_access_enum perm = 0;

                start = isa_regs->ebx;
                len = isa_regs->ecx;
                prot = isa_regs->edx;
                syscall_debug("  start=0x%x, len=0x%x, prot=0x%x\n",
                        start, len, prot);

                /* Permissions */
                perm |= prot & 0x01 ? mem_access_read : 0;
                perm |= prot & 0x02 ? mem_access_write : 0;
                perm |= prot & 0x04 ? mem_access_exec : 0;
                mem_protect(isa_mem, start, len, perm);
                break;
            }


                /* 140 */
            case syscall_code_llseek:
            {
                uint32_t fd, presult, origin, host_fd;
                int32_t offset_high, offset_low;
                int64_t offset;

                fd = isa_regs->ebx;
                offset_high = isa_regs->ecx;
                offset_low = isa_regs->edx;
                offset = ((int64_t) offset_high << 32) | offset_low;
                presult = isa_regs->esi;
                origin = isa_regs->edi;
                host_fd = fdt_get_host_fd(isa_ctx->fdt, fd);
                syscall_debug("  fd=%d, offset_high=0x%x, offset_low=0x%x, presult=0x%x, origin=0x%x\n",
                        fd, offset_high, offset_low, presult, origin);
                syscall_debug("  host_fd=%d\n", host_fd);
                syscall_debug("  offset=0x%llx\n", (long long) offset);
                if (offset_high != -1 && offset_high)
                    fatal("syscall llseek: only supported for 32-bit files");

                offset = lseek(host_fd, offset_low, origin);
                retval = offset;
                if (retval >= 0 && presult) {
                    mem_write(isa_mem, presult, 8, &offset);
                    retval = 0;
                }
                break;
            }


                /* 141 */
            case syscall_code_getdents:
            {
                uint32_t fd, pdirent, count, host_fd;
                void *buf;
                int nread, host_offs, guest_offs;
                char d_type;

                struct linux_dirent {
                    long d_ino;
                    off_t d_off;
                    unsigned short d_reclen;
                    char d_name[];
                } *dirent;

                struct sim_linux_dirent {
                    uint32_t d_ino;
                    uint32_t d_off;
                    uint16_t d_reclen;
                    char d_name[];
                } __attribute__((packed)) sim_dirent;

                /* Read parameters */
                fd = isa_regs->ebx;
                pdirent = isa_regs->ecx;
                count = isa_regs->edx;
                host_fd = fdt_get_host_fd(isa_ctx->fdt, fd);
                syscall_debug("  fd=%d, pdirent=0x%x, count=%d\n",
                        fd, pdirent, count);
                syscall_debug("  host_fd=%d\n", host_fd);

                /* Call host getdents */
                buf = calloc(1, count);
                if (!buf)
                    fatal("getdents: cannot allocate buffer");
                nread = syscall(SYS_getdents, host_fd, buf, count);

                /* Error or no more entries */
                if (nread < 0)
                    fatal("getdents: call to host system call returned error");
                if (!nread) {
                    retval = 0;
                    break;
                }

                /* Copy to host memory */
                host_offs = 0;
                guest_offs = 0;
                while (host_offs < nread) {
                    dirent = (struct linux_dirent *) (buf + host_offs);
                    sim_dirent.d_ino = dirent->d_ino;
                    sim_dirent.d_off = dirent->d_off;
                    sim_dirent.d_reclen = (15 + strlen(dirent->d_name)) / 4 * 4;
                    d_type = *(char *) (buf + host_offs + dirent->d_reclen - 1);

                    syscall_debug("    d_ino=%u ", sim_dirent.d_ino);
                    syscall_debug("d_off=%u ", sim_dirent.d_off);
                    syscall_debug("d_reclen=%u(host),%u(guest) ", dirent->d_reclen, sim_dirent.d_reclen);
                    syscall_debug("d_name='%s'\n", dirent->d_name);

                    mem_write(isa_mem, pdirent + guest_offs, 4, &sim_dirent.d_ino);
                    mem_write(isa_mem, pdirent + guest_offs + 4, 4, &sim_dirent.d_off);
                    mem_write(isa_mem, pdirent + guest_offs + 8, 2, &sim_dirent.d_reclen);
                    mem_write_string(isa_mem, pdirent + guest_offs + 10, dirent->d_name);
                    mem_write(isa_mem, pdirent + guest_offs + sim_dirent.d_reclen - 1, 1, &d_type);

                    host_offs += dirent->d_reclen;
                    guest_offs += sim_dirent.d_reclen;
                    if (guest_offs > count)
                        fatal("getdents: host buffer too small");
                }
                syscall_debug("  ret=%d(host),%d(guest)\n", host_offs, guest_offs);
                free(buf);
                retval = guest_offs;
                break;
            }


                /* 142 */
                /* Prototype:
                 * int select(int n, fd_set *inp, fd_set *outp, fd_set *exp, struct timeval *tvp);
                 */
            case syscall_code_select:
            {
                uint32_t n, inp, outp, exp, tvp;
                fd_set in, out, ex;
                struct sim_timeval sim_tv;
                struct timeval tv;

                n = isa_regs->ebx;
                inp = isa_regs->ecx;
                outp = isa_regs->edx;
                exp = isa_regs->esi;
                tvp = isa_regs->edi;
                syscall_debug("  n=%d, inp=0x%x, outp=0x%x, exp=0x%x, tvp=0x%x\n",
                        n, inp, outp, exp, tvp);

                /* Read file descriptor bitmaps. If any file descriptor is invalid, return EBADF. */
                if (!sim_fd_set_read(inp, &in, n)
                        || !sim_fd_set_read(outp, &out, n)
                        || !sim_fd_set_read(exp, &ex, n)) {
                    retval = -EBADF;
                    break;
                }

                /* Dump file descriptors */
                sim_fd_set_dump("inp", &in, n);
                sim_fd_set_dump("outp", &out, n);
                sim_fd_set_dump("exp", &ex, n);

                /* Read and dump 'sim_tv' */
                memset(&sim_tv, 0, sizeof (sim_tv));
                if (tvp)
                    mem_read(isa_mem, tvp, sizeof (sim_tv), &sim_tv);
                syscall_debug("  tv:\n");
                sim_timeval_debug(&sim_tv);

                /* Blocking 'select' not supported yet */
                if (sim_tv.tv_sec || sim_tv.tv_usec)
                    fatal("syscall 'select': not supported for 'tv' other than 0");

                /* Host system call */
                memset(&tv, 0, sizeof (tv));
                RETVAL(select(n, &in, &out, &ex, &tv));
                if (retval < 0)
                    break;

                /* Write result */
                sim_fd_set_write(inp, &in, n);
                sim_fd_set_write(outp, &out, n);
                sim_fd_set_write(exp, &ex, n);
                break;
            }


                /* 144 */
            case syscall_code_msync:
            {
                uint32_t start, len, flags;
                char sflags[MAX_STRING_SIZE];

                /* Parameters */
                start = isa_regs->ebx;
                len = isa_regs->ecx;
                flags = isa_regs->edx;
                map_flags(&msync_flags_map, flags, sflags, MAX_STRING_SIZE);
                syscall_debug("  start=0x%x, len=0x%x, flags=0x%x\n",
                        start, len, flags);
                syscall_debug("  flags=%s\n", sflags);

                /* System call is ignored */
                warning("syscall 'msync' ignored");
                break;
            }


                /* 146 */
            case syscall_code_writev:
            {
                int guest_fd, host_fd;
                struct fd_t *fd;
                uint32_t piovec, vlen;
                uint32_t iov_base, iov_len;
                void *buf;
                int v, length;

                guest_fd = isa_regs->ebx;
                piovec = isa_regs->ecx;
                vlen = isa_regs->edx;
                syscall_debug("  guest_fd=%d, piovec = 0x%x, vlen=0x%x\n",
                        guest_fd, piovec, vlen);


                /* Check file descriptor */
                fd = fdt_entry_get(isa_ctx->fdt, guest_fd);
                if (!fd) {
                    errno = -EBADF;
                    break;
                }
                host_fd = fd->host_fd;
                syscall_debug("  host_fd=%d\n", host_fd);
                if (fd->kind == fd_kind_pipe)
                    fatal("syscall writev: not supported for pipes");

                /* Proceed */
                for (v = 0; v < vlen; v++) {

                    /* Read io vector element */
                    mem_read(isa_mem, piovec, 4, &iov_base);
                    mem_read(isa_mem, piovec + 4, 4, &iov_len);
                    piovec += 8;

                    /* Read buffer from memory and write it to file */
                    buf = malloc(iov_len);
                    mem_read(isa_mem, iov_base, iov_len, buf);
                    length = write(host_fd, buf, iov_len);
                    free(buf);

                    /* Check error */
                    retval += length;
                    if (length < 0) {
                        retval = -1;
                        break;
                    }
                }
                break;
            }


                /* 149 */
            case syscall_code_sysctl:
            {
                uint32_t pargs;

                struct sysctl_args_t {
                    uint32_t pname;
                    uint32_t nlen;
                    uint32_t poldval;
                    uint32_t oldlenp;
                    uint32_t pnewval;
                    uint32_t newlen;
                } args;
                int i;
                uint32_t aux;
                uint32_t zero = 0;

                pargs = isa_regs->ebx;
                syscall_debug("  pargs=0x%x\n", pargs);
                mem_read(isa_mem, pargs, sizeof (struct sysctl_args_t), &args);
                syscall_debug("    pname=0x%x\n", args.pname);
                syscall_debug("    nlen=%d\n      ", args.nlen);
                for (i = 0; i < args.nlen; i++) {
                    mem_read(isa_mem, args.pname + i * 4, 4, &aux);
                    syscall_debug("name[%d]=%d ", i, aux);
                }
                syscall_debug("\n    poldval=0x%x\n", args.poldval);
                syscall_debug("    oldlenp=0x%x\n", args.oldlenp);
                syscall_debug("    pnewval=0x%x\n", args.pnewval);
                syscall_debug("    newlen=%d\n", args.newlen);
                warning("syscall sysctl: partially supported and not debugged");

                if (!args.oldlenp || !args.poldval)
                    fatal("syscall sysctl: not supported for poldval=0 or oldlenp=0");
                if (args.pnewval || args.newlen)
                    fatal("syscall sysctl: not supported for pnewval or newlen other than 0");

                mem_write(isa_mem, args.oldlenp, 4, &zero);
                mem_write(isa_mem, args.poldval, 1, &zero);
                break;
            }


                /* 154 */
            case syscall_code_sched_setparam:
            {
                uint32_t pid, pparam;
                uint32_t sched_priority;

                pid = isa_regs->ebx;
                pparam = isa_regs->ecx;
                syscall_debug("  pid=%d\n", pid);
                syscall_debug("  pparam=0x%x\n", pparam);
                mem_read(isa_mem, pparam, 4, &sched_priority);
                syscall_debug("    param.sched_priority=%d\n", sched_priority);

                /* Ignore system call */
                break;
            }


                /* 155 */
            case syscall_code_sched_getparam:
            {
                uint32_t pid, pparam;
                uint32_t zero = 0;

                pid = isa_regs->ebx;
                pparam = isa_regs->ecx;
                syscall_debug("  pid=%d\n", pid);
                syscall_debug("  pparam=0x%x\n", pparam);

                /* Return 0 in pparam->sched_priority */
                mem_write(isa_mem, pparam, 4, &zero);
                break;
            }


                /* 157 */
            case syscall_code_sched_getscheduler:
            {
                uint32_t pid;

                pid = isa_regs->ebx;
                syscall_debug("  pid=%d\n", pid);
                break;
            }


                /* 159 */
            case syscall_code_sched_get_priority_max:
            {
                uint32_t policy;

                policy = isa_regs->ebx;
                syscall_debug("  policy=%d\n", policy);

                switch (policy) {
                    case 0: retval = 0;
                        break; /* SCHED_OTHER */
                    case 1: retval = 99;
                        break; /* SCHED_FIFO */
                    case 2: retval = 99;
                        break; /* SCHED_RR */
                    default: fatal("syscall 'sched_get_priority_max' not implemented for policy=%d", policy);
                }
                break;
            }


                /* 160 */
            case syscall_code_sched_get_priority_min:
            {
                uint32_t policy;

                policy = isa_regs->ebx;
                syscall_debug("  policy=%d\n", policy);

                switch (policy) {
                    case 0: retval = 0;
                        break; /* SCHED_OTHER */
                    case 1: retval = 1;
                        break; /* SCHED_FIFO */
                    case 2: retval = 1;
                        break; /* SCHED_RR */
                    default: fatal("syscall 'sched_get_priority_min' not implemented for policy=%d", policy);
                }
                break;
            }


                /* 162 */
            case syscall_code_nanosleep:
            {
                uint32_t rqtp, rmtp;
                uint32_t sec, nsec;
                uint64_t total;

                rqtp = isa_regs->ebx;
                rmtp = isa_regs->ecx;
                syscall_debug("  rqtp=0x%x, rmtp=0x%x\n", rqtp, rmtp);

                mem_read(isa_mem, rqtp, 4, &sec);
                mem_read(isa_mem, rqtp + 4, 4, &nsec);
                total = (uint64_t) sec * 1000000 + (nsec / 1000);
                syscall_debug("  sleep time (us): %lld\n", (long long) total);
                isa_ctx->wakeup_time = ke_timer() + total;

                /* Suspend process */
                ctx_set_status(isa_ctx, ctx_suspended | ctx_nanosleep);
                ke_process_events_schedule();
                break;
            }


                /* 163 */
            case syscall_code_mremap:
            {
                uint32_t addr, old_len, new_len, flags;
                uint32_t new_addr;

                addr = isa_regs->ebx;
                old_len = isa_regs->ecx;
                new_len = isa_regs->edx;
                flags = isa_regs->esi;
                syscall_debug("  addr=0x%x, old_len=0x%x, new_len=0x%x flags=0x%x\n",
                        addr, old_len, new_len, flags);

                /* Restrictions */
                assert(!(addr & (MEM_PAGESIZE - 1)));
                assert(!(old_len & (MEM_PAGESIZE - 1)));
                assert(!(new_len & (MEM_PAGESIZE - 1)));
                if (!(flags & 0x1))
                    fatal("syscall mremap: flags MAP_MAYMOVE must be present");
                if (!old_len || !new_len)
                    fatal("syscall mremap: old_len or new_len cannot be zero");
                retval = addr;

                /* New size equals to old size means no action. */
                if (new_len == old_len)
                    break;

                /* Shrink region. This is always possible. */
                if (new_len < old_len) {
                    mem_unmap(isa_mem, addr + new_len, old_len - new_len);
                    break;
                }

                /* Increase region at the same address. This is only possible if
                 * there is enough free space for the new region. */
                if (new_len > old_len &&
                        mem_map_space(isa_mem, addr + old_len, new_len - old_len) == addr + old_len) {
                    mem_map(isa_mem, addr + old_len, new_len - old_len,
                            mem_access_read | mem_access_write);
                    break;
                }

                /* A new region must be found for the new size. */
                new_addr = mem_map_space_down(isa_mem, MMAP_BASE_ADDRESS, new_len);
                if (new_addr == (uint32_t) - 1)
                    fatal("syscall mremap: out of memory");
                mem_map(isa_mem, new_addr, new_len,
                        mem_access_read | mem_access_write);
                mem_copy(isa_mem, new_addr, addr, MIN(old_len, new_len));
                mem_unmap(isa_mem, addr, old_len);
                retval = new_addr;
                break;
            }


                /* 168 */
            case syscall_code_poll:
            {
                uint32_t pfds, nfds;
                int timeout, guest_fd, host_fd;
                struct sim_pollfd guest_fds;
                struct pollfd host_fds;
                char sevents[MAX_STRING_SIZE];
                struct fd_t *fd;

                pfds = isa_regs->ebx;
                nfds = isa_regs->ecx;
                timeout = isa_regs->edx;
                syscall_debug("  pfds=0x%x, nfds=%d, timeout=%d\n",
                        pfds, nfds, timeout);
                if (nfds != 1)
                    fatal("syscall poll: not suported for nfds != 1");
                assert(sizeof (struct sim_pollfd) == 8);
                assert(POLLIN == 1 && POLLOUT == 4);

                /* Read pollfd */
                mem_read(isa_mem, pfds, sizeof (struct sim_pollfd), &guest_fds);
                guest_fd = guest_fds.fd;
                map_flags(&poll_event_map, guest_fds.events, sevents, MAX_STRING_SIZE);
                syscall_debug("  guest_fd=%d, events=%s\n", guest_fd, sevents);

                /* Get file descriptor */
                fd = fdt_entry_get(isa_ctx->fdt, guest_fd);
                if (!fd) {
                    retval = -EBADF;
                    break;
                }
                host_fd = fd->host_fd;
                syscall_debug("  host_fd=%d\n", host_fd);

                /* Only POLLIN (0x1) and POLLOUT (0x4) supported */
                if (guest_fds.events & ~0x5)
                    fatal("syscall poll: only POLLIN and POLLOUT events supported");

                /* Not supported file descriptor */
                if (fd->host_fd < 0)
                    fatal("syscall 'poll': not supported file descriptor");

                /* Perform host 'poll' system call with a 0 timeout to distinguish
                 * blocking from non-blocking cases. */
                host_fds.fd = host_fd;
                host_fds.events = ((guest_fds.events & 1) ? POLLIN : 0) |
                        ((guest_fds.events & 4) ? POLLOUT : 0);
                RETVAL(poll(&host_fds, 1, 0));
                if (retval < 0)
                    break;

                /* If host 'poll' returned a value greater than 0, the guest call is non-blocking,
                 * since I/O is ready for the file descriptor. */
                if (retval > 0) {

                    /* Non-blocking POLLOUT on a file. */
                    if (guest_fds.events & host_fds.revents & POLLOUT) {
                        syscall_debug("  non-blocking write to file guaranteed\n");
                        guest_fds.revents = POLLOUT;
                        mem_write(isa_mem, pfds, sizeof (struct sim_pollfd), &guest_fds);
                        retval = 1;
                        break;
                    }

                    /* Non-blocking POLLIN on a file. */
                    if (guest_fds.events & host_fds.revents & POLLIN) {
                        syscall_debug("  non-blocking read from file guaranteed\n");
                        guest_fds.revents = POLLIN;
                        mem_write(isa_mem, pfds, sizeof (struct sim_pollfd), &guest_fds);
                        retval = 1;
                        break;
                    }

                    /* Never should get here */
                    abort();
                }

                /* At this point, host 'poll' returned 0, which means that none of the requested
                 * events is ready on the file, so we must suspend until they occur. */
                syscall_debug("  process going to sleep waiting for events on file\n");
                isa_ctx->wakeup_time = 0;
                if (timeout >= 0)
                    isa_ctx->wakeup_time = ke_timer() + (uint64_t) timeout * 1000;
                isa_ctx->wakeup_fd = guest_fd;
                isa_ctx->wakeup_events = guest_fds.events;
                ctx_set_status(isa_ctx, ctx_suspended | ctx_poll);
                ke_process_events_schedule();
                break;
            }


                /* 174 */
            case syscall_code_rt_sigaction:
            {
                uint32_t sig, pact, poact, sigsetsize;
                struct sim_sigaction act;

                sig = isa_regs->ebx;
                pact = isa_regs->ecx;
                poact = isa_regs->edx;
                sigsetsize = isa_regs->esi;
                syscall_debug("  sig=%d, pact=0x%x, poact=0x%x, sigsetsize=0x%x\n",
                        sig, pact, poact, sigsetsize);
                syscall_debug("  signal=%s\n", sim_signal_name(sig));

                /* Invalid signal */
                if (sig < 1 || sig > 64)
                    fatal("syscall rt_sigaction: invalid signal (%d)", sig);

                /* Read new sigaction */
                if (pact) {
                    mem_read(isa_mem, pact, sizeof (act), &act);
                    if (debug_status(syscall_debug_category)) {
                        FILE *f = debug_file(syscall_debug_category);
                        syscall_debug("  act: ");
                        sim_sigaction_dump(&act, f);
                        syscall_debug("\n    flags: ");
                        sim_sigaction_flags_dump(act.flags, f);
                        syscall_debug("\n    mask: ");
                        sim_sigset_dump(act.mask, f);
                        syscall_debug("\n");
                    }
                }

                /* Store previous sigaction */
                if (poact) {
                    mem_write(isa_mem, poact, sizeof (struct sim_sigaction),
                            &isa_ctx->signal_handlers->sigaction[sig - 1]);
                }

                /* Make new sigaction effective */
                if (pact)
                    isa_ctx->signal_handlers->sigaction[sig - 1] = act;

                break;
            }


                /* 175 */
            case syscall_code_rt_sigprocmask:
            {
                uint32_t how, pset, poset, sigsetsize;
                uint64_t set, oset;

                how = isa_regs->ebx;
                pset = isa_regs->ecx;
                poset = isa_regs->edx;
                sigsetsize = isa_regs->esi;
                syscall_debug("  how=0x%x, pset=0x%x, poset=0x%x, sigsetsize=0x%x\n",
                        how, pset, poset, sigsetsize);
                syscall_debug("  how=%s\n", map_value(&sigprocmask_how_map, how));

                /* Save old set */
                oset = isa_ctx->signal_masks->blocked;

                /* New set */
                if (pset) {

                    /* Read it from memory */
                    mem_read(isa_mem, pset, 8, &set);
                    if (debug_status(syscall_debug_category)) {
                        syscall_debug("  set=0x%llx ", (long long) set);
                        sim_sigset_dump(set, debug_file(syscall_debug_category));
                        syscall_debug("\n");
                    }

                    /* Set new set */
                    switch (how) {
                        case 0: /* SIG_BLOCK */
                            isa_ctx->signal_masks->blocked |= set;
                            break;
                        case 1: /* SIG_UNBLOCK */
                            isa_ctx->signal_masks->blocked &= ~set;
                            break;
                        case 2: /* SIG_SETMASK */
                            isa_ctx->signal_masks->blocked = set;
                            break;
                        default:
                            fatal("syscall rt_sigprocmask: wrong how value");
                    }
                }

                /* Return old set */
                if (poset)
                    mem_write(isa_mem, poset, 8, &oset);

                /* A change in the signal mask can cause pending signals to be
                 * able to execute, so check this. */
                ke_process_events_schedule();
                ke_process_events();

                break;
            }


                /* 179 */
            case syscall_code_rt_sigsuspend:
            {
                uint32_t pnewset, sigsetsize;
                uint64_t newset;

                pnewset = isa_regs->ebx;
                sigsetsize = isa_regs->ecx;
                syscall_debug("  pnewset=0x%x, sigsetsize=0x%x\n",
                        pnewset, sigsetsize);

                /* Read temporary signal mask */
                mem_read(isa_mem, pnewset, 8, &newset);
                if (debug_status(syscall_debug_category)) {
                    FILE *f = debug_file(syscall_debug_category);
                    syscall_debug("  old mask: ");
                    sim_sigset_dump(isa_ctx->signal_masks->blocked, f);
                    syscall_debug("\n  new mask: ");
                    sim_sigset_dump(newset, f);
                    syscall_debug("\n  pending:  ");
                    sim_sigset_dump(isa_ctx->signal_masks->pending, f);
                    syscall_debug("\n");
                }

                /* Save old mask and set new one, then suspend. */
                isa_ctx->signal_masks->backup = isa_ctx->signal_masks->blocked;
                isa_ctx->signal_masks->blocked = newset;
                ctx_set_status(isa_ctx, ctx_suspended | ctx_sigsuspend);

                /* New signal mask may cause new events */
                ke_process_events_schedule();
                ke_process_events();
                break;
            }


                /* 183 */
            case syscall_code_getcwd:
            {
                uint32_t pbuf, size, len;
                char *cwd;

                pbuf = isa_regs->ebx;
                size = isa_regs->ecx;
                syscall_debug("  pbuf=0x%x, size=0x%x\n", pbuf, size);

                cwd = isa_ctx->loader->cwd;
                len = strlen(cwd);
                if (size <= len)
                    retval = -ERANGE;
                else {
                    mem_write_string(isa_mem, pbuf, cwd);
                    retval = len + 1;
                }
                break;
            }


                /* 191 */
            case syscall_code_getrlimit:
            {
                uint32_t resource, prlim;
                char *sresource;
                struct sim_rlimit sim_rlimit;

                resource = isa_regs->ebx;
                prlim = isa_regs->ecx;
                sresource = map_value(&rlimit_resource_map, resource);
                syscall_debug("  resource=0x%x, prlim=0x%x\n", resource, prlim);
                syscall_debug("  resource=%s\n", sresource);

                switch (resource) {
                    case 2: /* RLIMIT_DATA */
                        sim_rlimit.cur = 0xffffffff;
                        sim_rlimit.max = 0xffffffff;
                        break;
                    case 3: /* RLIMIT_STACK */
                        sim_rlimit.cur = isa_ctx->loader->stack_size;
                        sim_rlimit.max = 0xffffffff;
                        break;
                    case 7: /* RLIMIT_NOFILE */
                        sim_rlimit.cur = 0x400;
                        sim_rlimit.max = 0x400;
                        break;
                    default:
                        fatal("getrlimit: not implemented for resource=%s", sresource);
                }

                mem_write(isa_mem, prlim, sizeof (struct sim_rlimit), &sim_rlimit);
                syscall_debug("  retval: cur=0x%x, max=0x%x\n", sim_rlimit.cur,
                        sim_rlimit.max);
                break;
            }


                /* System calls 'mmap' and 'mmap2' only differ in the interpretation of
                 * the parameter 'offset'. */
            case syscall_code_mmap2: /* 192 */
            {
                uint32_t addr, len, prot;
                uint32_t flags, offset;
                int guest_fd;
                char sprot[MAX_STRING_SIZE], sflags[MAX_STRING_SIZE];

                /* Read params */
                addr = isa_regs->ebx;
                len = isa_regs->ecx;
                prot = isa_regs->edx;
                flags = isa_regs->esi;
                guest_fd = isa_regs->edi;
                offset = isa_regs->ebp;

                /* Debug */
                syscall_debug("  addr=0x%x, len=%u, prot=0x%x, flags=0x%x, guest_fd=%d, offset=0x%x\n",
                        addr, len, prot, flags, guest_fd, offset);
                map_flags(&mmap_prot_map, prot, sprot, MAX_STRING_SIZE);
                map_flags(&mmap_flags_map, flags, sflags, MAX_STRING_SIZE);
                syscall_debug("  prot=%s, flags=%s\n", sprot, sflags);

                /* Map */
                retval = do_mmap(addr, len, prot, flags,
                        guest_fd, offset << MEM_PAGESHIFT);
                break;

            }


                /* 194 */
            case syscall_code_ftruncate64:
            {
                uint32_t fd, length, host_fd;

                fd = isa_regs->ebx;
                length = isa_regs->ecx;
                host_fd = fdt_get_host_fd(isa_ctx->fdt, fd);
                syscall_debug("  fd=%d, length=0x%x\n", fd, length);
                syscall_debug("  host_fd=%d\n", host_fd);

                RETVAL(ftruncate(host_fd, length));
                break;
            }


                /* 195 */
            case syscall_code_stat64:
            {
                uint32_t pfilename, pstatbuf;
                char filename[MAX_PATH_SIZE], fullpath[MAX_PATH_SIZE];
                struct stat statbuf;
                struct sim_stat64 sim_statbuf;
                int length;

                pfilename = isa_regs->ebx;
                pstatbuf = isa_regs->ecx;
                length = mem_read_string(isa_mem, pfilename, MAX_PATH_SIZE, filename);
                if (length >= MAX_PATH_SIZE)
                    fatal("syscall stat64: maximum path length exceeded");
                ld_get_full_path(isa_ctx, filename, fullpath, MAX_PATH_SIZE);
                syscall_debug("  pfilename=0x%x, pstatbuf=0x%x\n",
                        pfilename, pstatbuf);
                syscall_debug("  filename='%s', fullpath='%s'\n", filename, fullpath);

                RETVAL(stat(fullpath, &statbuf));
                if (!retval) {
                    syscall_copy_stat64(&sim_statbuf, &statbuf);
                    mem_write(isa_mem, pstatbuf, sizeof (sim_statbuf), &sim_statbuf);
                }
                break;
            }


                /* 196 */
            case syscall_code_lstat64:
            {
                uint32_t pfilename, pstatbuf;
                char filename[MAX_PATH_SIZE], fullpath[MAX_PATH_SIZE];
                int length;
                struct stat statbuf;
                struct sim_stat64 sim_statbuf;

                pfilename = isa_regs->ebx;
                pstatbuf = isa_regs->ecx;
                length = mem_read_string(isa_mem, pfilename, MAX_PATH_SIZE, filename);
                if (length >= MAX_PATH_SIZE)
                    fatal("syscall lstat64: maximum path length exceeded");
                ld_get_full_path(isa_ctx, filename, fullpath, MAX_PATH_SIZE);
                syscall_debug("  pfilename=0x%x, pstatbuf=0x%x\n", pfilename, pstatbuf);
                syscall_debug("  filename='%s', fullpath='%s'\n", filename, fullpath);

                RETVAL(lstat(fullpath, &statbuf));
                if (!retval) {
                    syscall_copy_stat64(&sim_statbuf, &statbuf);
                    mem_write(isa_mem, pstatbuf, sizeof (sim_statbuf), &sim_statbuf);
                }
                break;
            }


                /* 197 */
            case syscall_code_fstat64:
            {
                uint32_t fd, pstatbuf, host_fd;
                struct stat statbuf;
                struct sim_stat64 sim_statbuf;

                fd = isa_regs->ebx;
                pstatbuf = isa_regs->ecx;
                host_fd = fdt_get_host_fd(isa_ctx->fdt, fd);
                syscall_debug("  fd=%d, pstatbuf=0x%x\n", fd, pstatbuf);
                syscall_debug("  host_fd=%d\n", host_fd);

                RETVAL(fstat(host_fd, &statbuf));
                if (!retval) {
                    syscall_copy_stat64(&sim_statbuf, &statbuf);
                    mem_write(isa_mem, pstatbuf, sizeof (sim_statbuf), &sim_statbuf);
                }
                break;
            }


                /* 199 */
            case syscall_code_getuid:
            {
                RETVAL(getuid());
                break;
            }


                /* 200 */
            case syscall_code_getgid:
            {
                RETVAL(getgid());
                break;
            }


                /* 201 */
            case syscall_code_geteuid:
            {
                RETVAL(geteuid());
                break;
            }


                /* 202 */
            case syscall_code_getegid:
            {
                RETVAL(getegid());
                break;
            }


                /* 212 */
            case syscall_code_chown:
            {
                char filename[MAX_PATH_SIZE], fullpath[MAX_PATH_SIZE];
                uint32_t pfilename, owner, group;
                int len;

                pfilename = isa_regs->ebx;
                owner = isa_regs->ecx;
                group = isa_regs->edx;
                len = mem_read_string(isa_mem, pfilename, MAX_PATH_SIZE, filename);
                if (len >= MAX_PATH_SIZE)
                    fatal("syscall chmod: maximum path length exceeded");
                ld_get_full_path(isa_ctx, filename, fullpath, MAX_PATH_SIZE);
                syscall_debug("  pfilename=0x%x, owner=%d, group=%d\n", pfilename, owner, group);
                syscall_debug("  filename='%s', fullpath='%s'\n", filename, fullpath);
                RETVAL(chown(fullpath, owner, group));
                break;
            }


                /* 219 */
            case syscall_code_madvise:
            {
                uint32_t start, len, advice;

                start = isa_regs->ebx;
                len = isa_regs->ecx;
                advice = isa_regs->edx;
                syscall_debug("  start=0x%x, len=%d, advice=%d\n",
                        start, len, advice);
                break;
            }


                /* 220 */
            case syscall_code_getdents64:
            {
                uint32_t fd, pdirent, count, host_fd;
                void *buf;
                int nread, host_offs, guest_offs;

                struct linux_dirent {
                    long d_ino;
                    off_t d_off;
                    unsigned short d_reclen;
                    char d_name[];
                } *dirent;

                struct sim_linux_dirent64 {
                    uint64_t d_ino;
                    int64_t d_off;
                    uint16_t d_reclen;
                    unsigned char d_type;
                    char d_name[];
                } __attribute__((packed)) sim_dirent;

                /* Read parameters */
                fd = isa_regs->ebx;
                pdirent = isa_regs->ecx;
                count = isa_regs->edx;
                host_fd = fdt_get_host_fd(isa_ctx->fdt, fd);
                syscall_debug("  fd=%d, pdirent=0x%x, count=%d\n",
                        fd, pdirent, count);
                syscall_debug("  host_fd=%d\n", host_fd);

                /* Call host getdents */
                buf = calloc(1, count);
                if (!buf)
                    fatal("getdents: cannot allocate buffer");
                nread = syscall(SYS_getdents, host_fd, buf, count);

                /* Error or no more entries */
                if (nread < 0)
                    fatal("getdents: call to host system call returned error");
                if (!nread) {
                    retval = 0;
                    break;
                }

                /* Copy to host memory */
                host_offs = 0;
                guest_offs = 0;
                while (host_offs < nread) {
                    dirent = (struct linux_dirent *) (buf + host_offs);
                    sim_dirent.d_ino = dirent->d_ino;
                    sim_dirent.d_off = dirent->d_off;
                    sim_dirent.d_reclen = (27 + strlen(dirent->d_name)) / 8 * 8;
                    sim_dirent.d_type = *(char *) (buf + host_offs + dirent->d_reclen - 1);

                    syscall_debug("    d_ino=%lld ", (long long) sim_dirent.d_ino);
                    syscall_debug("d_off=%lld ", (long long) sim_dirent.d_off);
                    syscall_debug("d_reclen=%u(host),%u(guest) ", dirent->d_reclen, sim_dirent.d_reclen);
                    syscall_debug("d_name='%s'\n", dirent->d_name);

                    mem_write(isa_mem, pdirent + guest_offs, 8, &sim_dirent.d_ino);
                    mem_write(isa_mem, pdirent + guest_offs + 8, 8, &sim_dirent.d_off);
                    mem_write(isa_mem, pdirent + guest_offs + 16, 2, &sim_dirent.d_reclen);
                    mem_write(isa_mem, pdirent + guest_offs + 18, 1, &sim_dirent.d_type);
                    mem_write_string(isa_mem, pdirent + guest_offs + 19, dirent->d_name);

                    host_offs += dirent->d_reclen;
                    guest_offs += sim_dirent.d_reclen;
                    if (guest_offs > count)
                        fatal("getdents: host buffer too small");
                }
                syscall_debug("  ret=%d(host),%d(guest)\n", host_offs, guest_offs);
                free(buf);
                retval = guest_offs;
                break;
            }



                /* 221 */
            case syscall_code_fcntl64:
            {
                uint32_t guest_fd, cmd, arg;
                char *cmd_name;
                char sflags[MAX_STRING_SIZE];
                struct fd_t *fd;

                guest_fd = isa_regs->ebx;
                cmd = isa_regs->ecx;
                arg = isa_regs->edx;
                syscall_debug("  guest_fd=%d, cmd=%d, arg=0x%x\n",
                        guest_fd, cmd, arg);
                cmd_name = map_value(&fcntl_cmd_map, cmd);
                syscall_debug("    cmd=%s\n", cmd_name);

                /* Get file descriptor table entry */
                fd = fdt_entry_get(isa_ctx->fdt, guest_fd);
                if (!fd) {
                    retval = -EBADF;
                    break;
                }
                if (fd->host_fd < 0)
                    fatal("syscall 'fcntl64': not suported for this type of files");
                syscall_debug("    host_fd=%d\n", fd->host_fd);

                /* Process command */
                switch (cmd) {

                    case 1: /* F_GETFD */
                        RETVAL(fcntl(fd->host_fd, F_GETFD));
                        break;

                    case 2: /* F_SETFD */
                        RETVAL(fcntl(fd->host_fd, F_SETFD, arg));
                        break;

                    case 3: /* F_GETFL */
                        RETVAL(fcntl(fd->host_fd, F_GETFL));
                        map_flags(&open_flags_map, retval, sflags, MAX_STRING_SIZE);
                        syscall_debug("    retval=%s\n", sflags);
                        break;

                    case 4: /* F_SETFL */
                        map_flags(&open_flags_map, arg, sflags, MAX_STRING_SIZE);
                        syscall_debug("    arg=%s\n", sflags);
                        fd->flags = arg;
                        RETVAL(fcntl(fd->host_fd, F_SETFL, arg));
                        break;

                    default:
                        fatal("syscall fcntl64: command %s not implemented", cmd_name);
                }
                break;
            }


                /* 224 */
            case syscall_code_gettid:
            {
                /* FIXME: return different 'tid' for threads, but the system call
                 * 'getpid' should return the same 'pid' for threads from the same group
                 * created with CLONE_THREAD flag. */
                retval = isa_ctx->pid;
                break;
            }


                /* 240
                 * Prototype: sys_futex(void *addr1, int op, int val1, struct timespec *timeout,
                 *   void *addr2, int val3); */
            case syscall_code_futex:
            {
                uint32_t addr1, op, val1, ptimeout, addr2, val3;
                uint32_t timeout_sec, timeout_usec;
                uint32_t futex, cmd, bitset;

                addr1 = isa_regs->ebx;
                op = isa_regs->ecx;
                val1 = isa_regs->edx;
                ptimeout = isa_regs->esi;
                addr2 = isa_regs->edi;
                val3 = isa_regs->ebp;
                syscall_debug("  addr1=0x%x, op=%d, val1=%d, ptimeout=0x%x, addr2=0x%x, val3=%d\n",
                        addr1, op, val1, ptimeout, addr2, val3);


                /* Command - 'cmd' is obtained by removing 'FUTEX_PRIVATE_FLAG' (128) and
                 * 'FUTEX_CLOCK_REALTIME' from 'op'. */
                cmd = op & ~(256 | 128);
                mem_read(isa_mem, addr1, 4, &futex);
                syscall_debug("  futex=%d, cmd=%d (%s)\n",
                        futex, cmd, map_value(&futex_cmd_map, cmd));

                switch (cmd) {

                    case 0: /* FUTEX_WAIT */
                    case 9: /* FUTEX_WAIT_BITSET */

                        /* Default bitset value (all bits set) */
                        bitset = cmd == 9 ? val3 : 0xffffffff;

                        /* First, we compare the value of the futex with val1. If it's not the
                         * same, we exit with the error EWOULDBLOCK. */
                        if (futex != val1) {
                            retval = -11; /* EAGAIN = EWOULDBLOCK */
                            break;
                        }

                        /* Read timeout */
                        if (ptimeout) {
                            fatal("syscall futex: FUTEX_WAIT not supported with timeout");
                            mem_read(isa_mem, ptimeout, 4, &timeout_sec);
                            mem_read(isa_mem, ptimeout + 4, 4, &timeout_usec);
                            syscall_debug("  timeout={sec %d, usec %d}\n",
                                    timeout_sec, timeout_usec);
                        } else {
                            timeout_sec = 0;
                            timeout_usec = 0;
                        }

                        /* Suspend thread in the futex. */
                        isa_ctx->wakeup_futex = addr1;
                        isa_ctx->wakeup_futex_bitset = bitset;
                        isa_ctx->wakeup_futex_sleep = ++ke->futex_sleep_count;
                        ctx_set_status(isa_ctx, ctx_suspended | ctx_futex);
                        break;

                    case 1: /* FUTEX_WAKE */
                    case 10: /* FUTEX_WAKE_BITSET */

                        /* Default bitset value (all bits set) */
                        bitset = cmd == 10 ? val3 : 0xffffffff;
                        retval = ctx_futex_wake(isa_ctx, addr1, val1, bitset);
                        syscall_debug("  futex at 0x%x: %d processes woken up\n", addr1, retval);
                        break;

                    case 4: /* FUTEX_CMP_REQUEUE */
                    {
                        int requeued = 0;
                        struct ctx_t *ctx;

                        /* 'ptimeout' is interpreted here as an integer; only supported for INTMAX */
                        if (ptimeout != 0x7fffffff)
                            fatal("syscall futex, cmd=FUTEX_CMP_REQUEUE: only supported for ptimeout=INTMAX");

                        /* The value of val3 must be the same as the value of the futex
                         * at 'addr1' (stored in 'futex') */
                        if (futex != val3) {
                            retval = -11; /* EAGAIN */
                            break;
                        }

                        /* Wake up 'val1' threads from futex at 'addr1'. The number of woken up threads
                         * is the return value of the system call. */
                        retval = ctx_futex_wake(isa_ctx, addr1, val1, 0xffffffff);
                        syscall_debug("  futex at 0x%x: %d processes woken up\n", addr1, retval);

                        /* The rest of the threads waiting in futex 'addr1' are requeued into futex 'addr2' */
                        for (ctx = ke->suspended_list_head; ctx; ctx = ctx->suspended_next) {
                            if (ctx_get_status(ctx, ctx_futex) && ctx->wakeup_futex == addr1) {
                                ctx->wakeup_futex = addr2;
                                requeued++;
                            }
                        }
                        syscall_debug("  futex at 0x%x: %d processes requeued to futex 0x%x\n",
                                addr1, requeued, addr2);
                        break;
                    }

                    case 5: /* FUTEX_WAKE_OP */
                    {
                        int32_t op, oparg, cmp, cmparg;
                        int32_t val2 = ptimeout;
                        int32_t oldval, newval = 0, cond = 0;

                        op = (val3 >> 28) & 0xf;
                        cmp = (val3 >> 24) & 0xf;
                        oparg = (val3 >> 12) & 0xfff;
                        cmparg = val3 & 0xfff;

                        mem_read(isa_mem, addr2, 4, &oldval);
                        switch (op) {
                            case 0: newval = oparg;
                                break; /* FUTEX_OP_SET */
                            case 1: newval = oldval + oparg;
                                break; /* FUTEX_OP_ADD */
                            case 2: newval = oldval | oparg;
                                break; /* FUTEX_OP_OR */
                            case 3: newval = oldval & oparg;
                                break; /* FUTEX_OP_AND */
                            case 4: newval = oldval ^ oparg;
                                break; /* FOTEX_OP_XOR */
                            default: fatal("FUTEX_WAKE_OP: invalid operation");
                        }
                        mem_write(isa_mem, addr2, 4, &newval);

                        retval = ctx_futex_wake(isa_ctx, addr1, val1, 0xffffffff);

                        switch (cmp) {
                            case 0: cond = oldval == cmparg;
                                break; /* FUTEX_OP_CMP_EQ */
                            case 1: cond = oldval != cmparg;
                                break; /* FUTEX_OP_CMP_NE */
                            case 2: cond = oldval < cmparg;
                                break; /* FUTEX_OP_CMP_LT */
                            case 3: cond = oldval <= cmparg;
                                break; /* FUTEX_OP_CMP_LE */
                            case 4: cond = oldval > cmparg;
                                break; /* FUTEX_OP_CMP_GT */
                            case 5: cond = oldval >= cmparg;
                                break; /* FUTEX_OP_CMP_GE */
                            default: fatal("FUTEX_WAKE_OP: invalid condition");
                        }
                        if (cond)
                            retval += ctx_futex_wake(isa_ctx, addr2, val2, 0xffffffff);
                        /* FIXME: we are returning the total number of threads woken up
                         * counting both calls to ctx_futex_wake. Is this correct? */
                        break;
                    }

                    default:
                        fatal("syscall futex: not implemented for cmd=%d (%s)",
                                cmd, map_value(&futex_cmd_map, cmd));
                }
                break;
            }


                /* 241 */
            case syscall_code_sched_setaffinity:
            {
                uint32_t pid, len, pmask;
                uint32_t num_procs = 4;
                uint32_t mask;

                pid = isa_regs->ebx;
                len = isa_regs->ecx;
                pmask = isa_regs->edx;

                mem_read(isa_mem, pmask, 4, &mask);
                syscall_debug("  pid=%d, len=%d, pmask=0x%x\n", pid, len, pmask);
                syscall_debug("  mask=0x%x\n", mask);

                /* FIXME: system call ignored. Return the number of procs. */
                retval = num_procs;
                break;
            }


                /* 242 */
            case syscall_code_sched_getaffinity:
            {
                uint32_t pid, len, pmask;
                uint32_t num_procs = 4;
                uint32_t mask = (1 << num_procs) - 1;

                pid = isa_regs->ebx;
                len = isa_regs->ecx;
                pmask = isa_regs->edx;
                syscall_debug("  pid=%d, len=%d, pmask=0x%x\n", pid, len, pmask);

                /* FIXME: the affinity is set to 1 for num_procs processors and only the 4 LSBytes are set.
                 * The return value is set to num_procs. This is the behavior on a 4-core processor
                 * in a real system. */
                mem_write(isa_mem, pmask, 4, &mask);
                retval = num_procs;
                break;
            }


                /* 243 */
            case syscall_code_set_thread_area:
            {
                uint32_t puinfo;
                struct sim_user_desc uinfo;

                puinfo = isa_regs->ebx;
                syscall_debug("  puinfo=0x%x\n", puinfo);

                mem_read(isa_mem, puinfo, sizeof (struct sim_user_desc), &uinfo);
                syscall_debug("  entry_number=0x%x, base_addr=0x%x, limit=0x%x\n",
                        uinfo.entry_number, uinfo.base_addr, uinfo.limit);
                syscall_debug("  seg_32bit=0x%x, contents=0x%x, read_exec_only=0x%x\n",
                        uinfo.seg_32bit, uinfo.contents, uinfo.read_exec_only);
                syscall_debug("  limit_in_pages=0x%x, seg_not_present=0x%x, useable=0x%x\n",
                        uinfo.limit_in_pages, uinfo.seg_not_present, uinfo.useable);
                if (!uinfo.seg_32bit)
                    fatal("syscall set_thread_area: only 32-bit segments supported");

                /* Limit given in pages (4KB units) */
                if (uinfo.limit_in_pages)
                    uinfo.limit <<= 12;

                if (uinfo.entry_number == (uint32_t) - 1) {
                    if (isa_ctx->glibc_segment_base)
                        fatal("set_thread_area: glibc segment already set");
                    isa_ctx->glibc_segment_base = uinfo.base_addr;
                    isa_ctx->glibc_segment_limit = uinfo.limit;
                    uinfo.entry_number = 6;
                    mem_write(isa_mem, puinfo, 4, &uinfo.entry_number);
                } else {
                    if (uinfo.entry_number != 6)
                        fatal("set_thread_area: erroneous entry_number field");
                    if (!isa_ctx->glibc_segment_base)
                        fatal("set_thread_area: glibc segment was not set");
                    isa_ctx->glibc_segment_base = uinfo.base_addr;
                    isa_ctx->glibc_segment_limit = uinfo.limit;
                }

                break;
            }


                /* 250 */
            case syscall_code_fadvise64:
            {
                uint32_t fd;
                uint32_t off_hi, off_lo;
                uint32_t len, advice;

                fd = isa_regs->ebx;
                off_lo = isa_regs->ecx;
                off_hi = isa_regs->edx;
                len = isa_regs->esi;
                advice = isa_regs->edi;

                syscall_debug("  fd=%d, off={0x%x, 0x%x}, len=%d, advice=%d\n",
                        fd, off_hi, off_lo, len, advice);
                break;
            }


                /* 252 */
            case syscall_code_exit_group:
            {
                int status;

                status = isa_regs->ebx;
                syscall_debug("  status=0x%x\n", status);

                ctx_finish_group(isa_ctx, status);
                break;
            }


                /* 258 */
            case syscall_code_set_tid_address:
            {
                uint32_t tidptr;

                tidptr = isa_regs->ebx;
                syscall_debug("  tidptr=0x%x\n", tidptr);

                isa_ctx->clear_child_tid = tidptr;
                retval = isa_ctx->pid;
                break;
            }


                /* 266 */
            case syscall_code_clock_getres:
            {
                uint32_t clk_id, pres;
                uint32_t tv_sec, tv_nsec;

                clk_id = isa_regs->ebx;
                pres = isa_regs->ecx;
                syscall_debug("  clk_id=%d\n", clk_id);
                syscall_debug("  pres=0x%x\n", pres);

                tv_sec = 0;
                tv_nsec = 1;
                mem_write(isa_mem, pres, 4, &tv_sec);
                mem_write(isa_mem, pres + 4, 4, &tv_nsec);
                break;
            }


                /* 270 */
            case syscall_code_tgkill:
            {
                uint32_t tgid, pid, sig;
                struct ctx_t *ctx;

                tgid = isa_regs->ebx;
                pid = isa_regs->ecx;
                sig = isa_regs->edx;
                syscall_debug("  tgid=%d, pid=%d, sig=%d (%s)\n",
                        tgid, pid, sig, sim_signal_name(sig));

                /* Implementation restrictions. */
                if ((int) tgid == -1)
                    fatal("syscall 'tgkill': not implemented for tgid = -1");

                /* Find context referred by pid. */
                ctx = ctx_get(pid);
                if (!ctx)
                    fatal("syscall 'tgkill': pid %d does not exist", pid);

                /* Send signal */
                sim_sigset_add(&ctx->signal_masks->pending, sig);
                ctx_host_thread_suspend_cancel(ctx); /* Target ctx might wake up */
                ke_process_events_schedule();
                ke_process_events();
                break;
            }


                /* 311 */
            case syscall_code_set_robust_list:
            {
                uint32_t head, len;

                head = isa_regs->ebx;
                len = isa_regs->ecx;
                syscall_debug("  head=0x%x, len=%d\n", head, len);
                if (len != 12)
                    fatal("set_robust_list: only working for len = 12");
                isa_ctx->robust_list_head = head;
                break;
            }


                /* 325 */
                /* Artificial system call used to implement the OpenCL 1.1 interface. */
            case syscall_code_opencl:
            {
                uint32_t func_code, pargs;
                uint32_t args[OPENCL_MAX_ARGS];
                int i;
                char *func_name;
                int func_argc;

                func_code = isa_regs->ebx;
                pargs = isa_regs->ecx;

                /* Check 'func_code' range */
                if (func_code < OPENCL_FUNC_FIRST || func_code > OPENCL_FUNC_LAST)
                    fatal("syscall 'opencl': func_code out of range");

                /* Get function info */
                func_name = opencl_func_names[func_code - OPENCL_FUNC_FIRST];
                func_argc = opencl_func_argc[func_code - OPENCL_FUNC_FIRST];
                syscall_debug("  func_code=%d (%s, %d arguments), pargs=0x%x\n",
                        func_code, func_name, func_argc, pargs);

                /* Read function args */
                assert(func_argc <= OPENCL_MAX_ARGS);
                mem_read(isa_mem, pargs, func_argc * 4, args);
                for (i = 0; i < func_argc; i++)
                    syscall_debug("    args[%d] = %d (0x%x)\n",
                        i, args[i], args[i]);

                /* Run OpenCL function */
                retval = opencl_func_run(func_code, args);
                break;
            }


            default:
                if (syscode >= syscall_code_count) {
                    retval = -38;
                } else {
                    fatal("not implemented system call '%s' (code %d) at 0x%x\n%s",
                            syscode < syscall_code_count ? syscall_name[syscode] : "",
                            syscode, isa_regs->eip, err_syscall_note);
                }
        }

      }
	 /* Return value (for all system calls except 'sigreturn') */
        if (syscode != syscall_code_sigreturn && !ctx_get_status(isa_ctx, ctx_suspended))
            isa_regs->eax = retval;
}

