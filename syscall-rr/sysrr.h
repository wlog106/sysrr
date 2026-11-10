/* Shared record format between the BPF recorder, the user-space writer and
 * the decoder.  A log file is:
 *
 *   struct sysrr_file_hdr
 *   record, record, ...        each record starts with struct rec_hdr,
 *                              rec_hdr.len is the full record length
 *
 * Every syscall made by the target produces zero or more REC_DATA records
 * (captured user memory) followed by exactly one REC_SYSCALL record that
 * "commits" it.  They share the same seq.  Memory the kernel reads (D_IN,
 * D_STR) is captured at sys_enter, memory it writes (D_OUT) at sys_exit.
 * CRIU checkpoints show up as REC_MARKER records and bump the generation
 * counter (gen).
 */
#ifndef SYSRR_H
#define SYSRR_H

#ifndef __VMLINUX_H__
#include <linux/types.h>
typedef __u8 u8; typedef __u16 u16; typedef __u32 u32; typedef __u64 u64;
typedef __s32 s32; typedef __s64 s64;
#endif

#define SYSRR_MAGIC    0x31525253595353ULL     /* "SSYSRR1" little-endian */
#define SYSRR_VERSION  1

#define CHUNK_MAX      16384                   /* bytes of user memory per REC_DATA */
#define CHUNKS_MAX     64                      /* => at most 1 MiB per buffer */
#define STR_MAX        4096                    /* paths */

enum rec_type {
	REC_SYSCALL = 1,
	REC_DATA    = 2,
	REC_MARKER  = 3,
	REC_STATS   = 4,                           /* written by user space at the end */
	REC_SIGNAL  = 5,                           /* a thread got a signal, or ended */
};

/* rec_syscall.h.flags */
#define F_DUMP     0x01   /* made while CRIU had the thread seized (parasite code) */
#define F_NOENTER  0x02   /* sys_enter not seen (recorder attached mid-syscall) */
#define F_NOEXIT   0x04   /* syscall never returns (exit, exit_group): ret invalid */
#define F_TRUNC    0x08   /* a buffer was larger than CHUNKS_MAX*CHUNK_MAX */
#define F_FAULT    0x10   /* reading user memory failed, some data missing */
#define F_LOST     0x20   /* ring buffer full, some DATA records missing */

/* rec_data.h.flags */
#define DF_EXIT    0x01   /* input re-copied at sys_exit (-r): compare with the
                             sys_enter copy to see if it changed during the call */

/* rec_data.kind */
enum data_kind {
	D_OUT = 1,        /* kernel -> user buffer: replay must inject this */
	D_IN  = 2,        /* user -> kernel buffer (write data, sockaddr): replay can verify */
	D_STR = 3,        /* NUL-terminated string (path), user -> kernel */
};

/* marker kinds */
enum marker_kind {
	M_SEIZE  = 1,     /* CRIU attached to a target thread */
	M_DETACH = 2,     /* CRIU released a target thread */
	M_GEN    = 3,     /* last thread released: a new generation starts */
};

/* A record structure is consist of a hdr and one of the four record type */
struct rec_hdr {
	u16 type;
	u16 flags;
	u32 len;
	u64 seq;
};

/* The "commit" mark of a record */
struct rec_syscall {
	struct rec_hdr h;
	u64 ts_enter;          /* CLOCK_MONOTONIC ns */
	u64 ts_exit;
	u32 tgid, tid;         /* host pid namespace */
	u32 ns_tgid, ns_tid;   /* the container's pid namespace (stable across restore) */
	u32 gen;               /* generation at sys_enter */
	u32 ndata;             /* number of REC_DATA records emitted for this seq */
	s64 nr;
	u64 args[6];
	s64 ret;
};

struct rec_data {
	struct rec_hdr h;
	u32 tid;
	u8  arg;               /* which syscall argument the pointer came from */
	u8  kind;              /* enum data_kind */
	u16 idx;               /* sub-buffer index, e.g. iovec number */
	u32 offset;            /* offset of this chunk inside the buffer */
	u32 len;               /* bytes of data[] in this chunk */
	char data[];
};

struct rec_marker {
	struct rec_hdr h;
	u64 ts;
	u32 kind;              /* enum marker_kind */
	u32 tid;               /* target thread (host tid) */
	u32 tracer;            /* tgid of the process calling ptrace (criu) */
	u32 gen;               /* generation after this event */
};

/* A signal being delivered to a target thread, or the thread's end.  seq is the
 * next seq to be handed out: the signal came after every syscall below it.  The
 * log format stays compatible: older readers skip types they don't know. */
#define SIG_EXIT   (-1)    /* not a signal: the thread ended, code = its exit_code */
struct rec_signal {
	struct rec_hdr h;
	u64 ts;
	u32 tid, ns_tid;
	u32 gen;
	s32 sig;
	s32 code;              /* si_code; for SIG_EXIT the wait status */
	u32 fatal;             /* no handler, and the default action kills */
	u64 addr;              /* si_addr (SIGSEGV, SIGBUS, SIGFPE, SIGILL) */
	u64 rip, rsp;          /* where the thread was */
	u64 handler;
};

struct rec_stats {
	struct rec_hdr h;
	u64 syscalls;
	u64 data_records;
	u64 data_bytes;
	u64 lost;              /* records the BPF side could not put in the ring */
	u64 bytes_written;
};

#define OPT_RACECHECK  0x01   /* inputs copied at sys_enter and again at sys_exit */

struct sysrr_file_hdr {
	u64 magic;
	u32 version;
	u32 hdr_size;
	u32 target_tgid;       /* host pid given to the recorder */
	u32 target_ns_tgid;    /* same process inside its pid namespace */
	u64 pidns_ino;
	u32 follow;            /* descendants recorded too */
	u32 opts;              /* OPT_* */
	u64 start_mono_ns;
	char kernel[64];
	char comm[16];
};

#endif
