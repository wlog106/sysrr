/* Decode a sysrr log.
 *
 *   ./sysrr_dump [-g GEN] [-a] [-x] [-s] [-r] sysrr.log
 *     -g GEN  only syscalls of that generation (= the replay log of checkpoint GEN)
 *     -a      also show syscalls made by CRIU's parasite code (F_DUMP)
 *     -x      hex-dump captured buffers instead of a short preview
 *     -s      summary only: per-generation counts and integrity check
 *     -r      only syscalls whose input buffer changed while the syscall ran
 *             (needs a log recorded with sysrr -r)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include "sysrr.h"

static const char *names[512] = {
#include "syscall_names.h"
};

static const char *sysname(long nr)
{
	static char buf[24];
	if (nr >= 0 && nr < 512 && names[nr])
		return names[nr];
	snprintf(buf, sizeof(buf), "syscall_%ld", nr);
	return buf;
}

/* DATA records arrive before the SYSCALL record that commits them; park them
 * here until then.  Usually only a handful are pending at a time. */
struct pend { struct rec_data *d; };
static struct pend *pend;
static size_t npend, cappend;

static int opt_gen = -1, opt_all, opt_hex, opt_summary, opt_races;

#define MAX_GEN 64
static struct { unsigned long long syscalls, dump, data_bytes; } per_gen[MAX_GEN];
static unsigned long long problems, races, max_seq, nseen;
static unsigned char *seen;    /* bitmap of seqs seen, for gap detection */
static size_t seen_cap;

static void mark_seen(unsigned long long seq)
{
	if (seq / 8 >= seen_cap) {
		size_t n = (seq / 8 + 1) * 2;
		seen = realloc(seen, n);
		memset(seen + seen_cap, 0, n - seen_cap);
		seen_cap = n;
	}
	seen[seq / 8] |= 1 << (seq % 8);
	if (seq > max_seq)
		max_seq = seq;
	nseen++;
}

static void preview(const struct rec_data *d)
{
	static const char *kinds[] = { "?", "OUT", "IN", "STR" };
	unsigned n = d->len;

	printf("      [%s%s arg%u", kinds[d->kind < 4 ? d->kind : 0],
	       d->h.flags & DF_EXIT ? "@exit" : "", d->arg);
	if (d->idx < 0xfff0)
		printf(" #%u", d->idx);
	else if (d->idx == 0xffff)
		printf(" iovec[]");
	else if (d->idx == 0xfffe)
		printf(" msghdr");
	if (d->offset)
		printf(" +%u", d->offset);
	printf(" %uB] ", n);
	if (opt_hex) {
		for (unsigned i = 0; i < n; i++) {
			if (i % 32 == 0)
				printf("\n        %06x ", i);
			printf("%02x", (unsigned char)d->data[i]);
		}
		putchar('\n');
		return;
	}
	putchar('"');
	for (unsigned i = 0; i < n && i < 64; i++) {
		unsigned char c = d->data[i];
		if (d->kind == D_STR && c == 0)
			break;
		if (c == '\n')
			printf("\\n");
		else
			putchar(isprint(c) ? c : '.');
	}
	printf(n > 64 && d->kind != D_STR ? "\"...\n" : "\"\n");
}

/* last committed seq per thread: a DATA record newer than that at the end of
 * the log belongs to a syscall still in flight when recording stopped (its
 * inputs were copied at sys_enter, the commit never came) */
static struct { unsigned tid; unsigned long long seq; } *last;
static size_t nlast;

static unsigned long long *last_of(unsigned tid)
{
	for (size_t i = 0; i < nlast; i++)
		if (last[i].tid == tid)
			return &last[i].seq;
	last = realloc(last, (nlast + 1) * sizeof(*last));
	last[nlast].tid = tid;
	last[nlast].seq = 0;
	return &last[nlast++].seq;
}

static void print_byte(const struct rec_data *d, unsigned at)
{
	if (at >= d->len) {
		printf("(ends)");
		return;
	}
	unsigned char c = d->data[at];
	printf("0x%02x '%c'", c, isprint(c) ? c : '.');
}

/* Race check (sysrr -r): the sys_exit copy of an input (DF_EXIT) is compared
 * with the sys_enter copy of the same piece.  A difference means another
 * thread changed the buffer while the syscall was using it.  Which version
 * the kernel consumed depends on when it copied the buffer, so both are
 * shown.  Returns 1 if the piece changed. */
static int check_race(struct rec_data **mine, unsigned n, const struct rec_data *x, int show)
{
	for (unsigned i = 0; i < n; i++) {
		const struct rec_data *e = mine[i];
		if ((e->h.flags & DF_EXIT) || e->arg != x->arg || e->kind != x->kind ||
		    e->idx != x->idx || e->offset != x->offset)
			continue;
		unsigned l = e->len < x->len ? e->len : x->len, at = 0, ndiff = 0;
		while (at < l && e->data[at] == x->data[at])
			at++;
		if (at == l && e->len == x->len)
			return 0;
		for (unsigned j = at; j < l; j++)
			ndiff += e->data[j] != x->data[j];
		if (show) {
			printf("      !! RACE: arg%u changed during the syscall (%u of %u bytes); "
			       "first at byte %u: enter ", x->arg, ndiff + (e->len != x->len),
			       e->len, x->offset + at);
			print_byte(e, at);
			printf(", exit ");
			print_byte(x, at);
			putchar('\n');
		}
		return 1;
	}
	return 0;       /* no entry copy (it faulted): the exit copy is the input */
}

static void on_syscall(const struct rec_syscall *r, unsigned long long t0)
{
	static struct rec_data **mine;
	static size_t capmine;
	unsigned found = 0, changed = 0;

	/* collect this syscall's data records (they keep their emission order) */
	for (size_t i = 0; i < npend; ) {
		if (pend[i].d->h.seq == r->h.seq) {
			if (found == capmine)
				mine = realloc(mine, (capmine = capmine * 2 + 16) * sizeof(*mine));
			mine[found++] = pend[i].d;
			memmove(&pend[i], &pend[i + 1], (npend - i - 1) * sizeof(*pend));
			npend--;
		} else {
			i++;
		}
	}
	for (unsigned i = 0; i < found; i++)
		if (mine[i]->h.flags & DF_EXIT)
			changed += check_race(mine, found, mine[i], 0);
	races += changed;

	int show = (opt_gen < 0 || r->gen == (unsigned)opt_gen) &&
		   (opt_all || !(r->h.flags & F_DUMP)) && !opt_summary &&
		   (!opt_races || changed);

	mark_seen(r->h.seq);
	if (r->h.seq > *last_of(r->tid))
		*last_of(r->tid) = r->h.seq;
	if (r->gen < MAX_GEN) {
		per_gen[r->gen].syscalls++;
		if (r->h.flags & F_DUMP)
			per_gen[r->gen].dump++;
	}

	if (show) {
		double t = r->ts_enter ? (r->ts_enter - t0) / 1e9 : 0;
		printf("#%-7llu g%u %u/%u %10.6f %s(", (unsigned long long)r->h.seq, r->gen,
		       r->ns_tid, r->tid, t, sysname(r->nr));
		for (int i = 0; i < 6; i++)
			printf(i ? ", %#llx" : "%#llx", (unsigned long long)r->args[i]);
		if (r->h.flags & F_NOEXIT)
			printf(") = ?");
		else
			printf(") = %lld", (long long)r->ret);
		if (r->h.flags) {
			printf("  <");
			if (r->h.flags & F_DUMP)    printf(" criu-parasite");
			if (r->h.flags & F_NOENTER) printf(" no-enter");
			if (r->h.flags & F_NOEXIT)  printf(" no-exit");
			if (r->h.flags & F_TRUNC)   printf(" TRUNCATED");
			if (r->h.flags & F_FAULT)   printf(" FAULT");
			if (r->h.flags & F_LOST)    printf(" LOST");
			printf(" >");
		}
		putchar('\n');
	}

	for (unsigned i = 0; i < found; i++) {
		if (show)
			preview(mine[i]);
		if (r->gen < MAX_GEN)
			per_gen[r->gen].data_bytes += mine[i]->len;
	}
	for (unsigned i = 0; i < found && show && changed; i++)
		if (mine[i]->h.flags & DF_EXIT)
			check_race(mine, found, mine[i], 1);
	for (unsigned i = 0; i < found; i++)
		free(mine[i]);
	if (found != r->ndata || (r->h.flags & (F_LOST | F_FAULT | F_TRUNC))) {
		problems++;
		if (show)
			printf("      !! incomplete: %u of %u data records\n", found, r->ndata);
	}
}

int main(int argc, char **argv)
{
	struct sysrr_file_hdr fh;
	struct rec_hdr h;
	struct rec_stats st = {0};
	int opt;

	while ((opt = getopt(argc, argv, "g:axsr")) != -1) {
		switch (opt) {
		case 'g': opt_gen = atoi(optarg); break;
		case 'a': opt_all = 1; break;
		case 'x': opt_hex = 1; break;
		case 's': opt_summary = 1; break;
		case 'r': opt_races = 1; break;
		default:
			fprintf(stderr, "usage: %s [-g GEN] [-a] [-x] [-s] [-r] FILE\n", argv[0]);
			return 1;
		}
	}
	FILE *f = fopen(optind < argc ? argv[optind] : "sysrr.log", "r");
	if (!f) {
		perror("open");
		return 1;
	}
	if (fread(&fh, sizeof(fh), 1, f) != 1 || fh.magic != SYSRR_MAGIC) {
		fprintf(stderr, "not a sysrr log\n");
		return 1;
	}
	printf("# target pid %u (ns pid %u, %s)%s%s, kernel %s\n", fh.target_tgid,
	       fh.target_ns_tgid, fh.comm, fh.follow ? " +children" : "",
	       fh.opts & OPT_RACECHECK ? " +race-check" : "", fh.kernel);
	if (opt_races && !(fh.opts & OPT_RACECHECK))
		fprintf(stderr, "warning: log was recorded without sysrr -r, nothing to compare\n");
	printf("# columns: seq gen ns_tid/host_tid seconds syscall(args) = ret\n");

	while (fread(&h, sizeof(h), 1, f) == 1) {
		char *buf = malloc(h.len);
		memcpy(buf, &h, sizeof(h));
		if (h.len < sizeof(h) || fread(buf + sizeof(h), h.len - sizeof(h), 1, f) != 1) {
			fprintf(stderr, "truncated record\n");
			free(buf);
			break;
		}
		switch (h.type) {
		case REC_DATA:
			if (npend == cappend)
				pend = realloc(pend, (cappend = cappend * 2 + 16) * sizeof(*pend));
			pend[npend++].d = (struct rec_data *)buf;
			continue;                       /* freed when committed */
		case REC_SYSCALL:
			on_syscall((struct rec_syscall *)buf, fh.start_mono_ns);
			break;
		case REC_MARKER: {
			struct rec_marker *m = (void *)buf;
			static const char *k[] = { "?", "CRIU SEIZE", "CRIU DETACH", "NEW GENERATION" };
			if (!opt_summary)
				printf("==== %s tid %u (tracer pid %u) at %.6f -> gen %u ====\n",
				       k[m->kind < 4 ? m->kind : 0], m->tid, m->tracer,
				       (m->ts - fh.start_mono_ns) / 1e9, m->gen);
			break;
		}
		case REC_SIGNAL: {
			struct rec_signal *r = (void *)buf;
			if (opt_summary || (opt_gen >= 0 && (int)r->gen != opt_gen))
				break;
			if (r->sig == SIG_EXIT)
				printf("==== thread %u ended: status %#x (signal %d, exit code %d) g%u ====\n",
				       r->ns_tid, r->code, r->code & 0x7f, (r->code >> 8) & 0xff, r->gen);
			else
				printf("==== thread %u got signal %d (%s) code %d addr %#llx at rip %#llx%s g%u ====\n",
				       r->ns_tid, r->sig, strsignal(r->sig), r->code,
				       (unsigned long long)r->addr, (unsigned long long)r->rip,
				       r->fatal ? ", FATAL" : "", r->gen);
			break;
		}
		case REC_STATS:
			st = *(struct rec_stats *)buf;
			break;
		}
		free(buf);
	}

	/* leftover DATA of a syscall still in flight at the end is not an orphan */
	size_t inflight = 0;
	for (size_t i = 0; i < npend; ) {
		unsigned long long seq = pend[i].d->h.seq;
		unsigned tid = pend[i].d->tid;
		if (seq <= *last_of(tid)) {
			i++;
			continue;
		}
		if (!opt_summary && !opt_races)
			printf("#%-7llu %u: in flight when recording stopped, inputs captured\n", seq, tid);
		mark_seen(seq);                         /* not a gap either */
		inflight++;
		for (size_t j = i; j < npend; ) {
			if (pend[j].d->h.seq == seq) {
				free(pend[j].d);
				memmove(&pend[j], &pend[j + 1], (npend - j - 1) * sizeof(*pend));
				npend--;
			} else {
				j++;
			}
		}
	}

	/* integrity: every seq between the first and last must be present */
	unsigned long long gaps = 0, first = ~0ULL;
	for (unsigned long long s = 0; s <= max_seq && nseen; s++) {
		int have = seen[s / 8] & (1 << (s % 8));
		if (have && first == ~0ULL)
			first = s;
		if (!have && first != ~0ULL)
			gaps++;
	}

	printf("\n# summary\n");
	for (int g = 0; g < MAX_GEN; g++)
		if (per_gen[g].syscalls)
			printf("#   gen %d: %llu syscalls (%llu by criu parasite), %llu data bytes\n",
			       g, per_gen[g].syscalls, per_gen[g].dump, per_gen[g].data_bytes);
	printf("#   lost in kernel: %llu, missing seqs: %llu, incomplete syscalls: %llu, "
	       "orphan data records: %zu, in flight at stop: %zu\n",
	       (unsigned long long)st.lost, gaps, problems, npend, inflight);
	if (fh.opts & OPT_RACECHECK)
		printf("#   input buffers changed during a syscall (race check): %llu\n", races);
	int ok = !st.lost && !gaps && !problems && !npend;
	printf("#   %s\n", ok ? "OK: log is complete" : "WARNING: log is incomplete");
	return ok ? 0 : 2;
}
