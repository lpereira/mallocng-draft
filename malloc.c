#define _GNU_SOURCE
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <assert.h>
#include <errno.h>

#undef assert
#define assert(x) do { if (!(x)) __builtin_trap(); } while(0)
//#define assert(x)

static inline int a_ctz_32(uint32_t x)
{
	return __builtin_ctz(x);
}

static inline int a_cas(volatile int *p, int t, int s)
{
	return __sync_val_compare_and_swap(p, t, s);
}

static inline int a_swap(volatile int *p, int v)
{
	int x;
	do x = *p;
	while (a_cas(p, x, v)!=x);
	return x;
}

static inline void a_or(volatile int *p, int v)
{
	__sync_fetch_and_or(p, v);
}

#if 1
static struct {
	int threads_minus_1;
} libc = { 1 };
#endif

static pthread_rwlock_t malloc_lock = PTHREAD_RWLOCK_INITIALIZER;

static void rdlock()
{
	if (libc.threads_minus_1)
		pthread_rwlock_rdlock(&malloc_lock);
}

static void wrlock()
{
	if (libc.threads_minus_1)
		pthread_rwlock_wrlock(&malloc_lock);
}

static void unlock()
{
	if (libc.threads_minus_1)
		pthread_rwlock_unlock(&malloc_lock);
}

static void upgradelock()
{
	unlock();
	wrlock();
}

static const uint16_t size_classes[] = {
	1, 2, 3, 4, 5, 6, 7, 8,
	9, 10, 12, 15,
	18, 21, 25, 31,
	36, 42, 51, 63,
	73, 85, 102, 127,
	146, 170, 204, 255,
	292, 341, 409, 511,
	584, 682, 818, 1023,
	1169, 1364, 1637, 2047,
	2340, 2730, 3276, 4095,
	4680, 5460, 6552, 8191,
};

#define MMAP_THRESHOLD 131052

static int size_to_class(size_t n)
{
	n = (n+3)>>4;
	if (n<10) return n;
	n++;
	size_t a = 10, c = sizeof size_classes / sizeof *size_classes - a;
	while (c) {
		int v = size_classes[a+c/2];
		if (n<v) {
			c /= 2;
		} else if (n>v) {
			a += c/2+1;
			c -= c/2+1;
		} else {
			return a+c/2;
		}
	}
	return a;
}

struct group {
	struct meta *meta;
	char pad[16 - sizeof(struct meta *)];
	unsigned char storage[];
};

struct meta {
	struct meta *prev, *next;
	struct group *mem;
	volatile int avail_mask, freed_mask;
	uintptr_t last_idx:5;
	uintptr_t freeable:1;
	uintptr_t sizeclass:6;
	uintptr_t maplen:8*sizeof(uintptr_t)-12;
};

static struct meta builtin_meta[16];
static struct meta *free_meta_head, *full_groups_head;
static struct meta *avail_meta = builtin_meta;
static size_t avail_meta_count = sizeof builtin_meta / sizeof *builtin_meta;
static struct meta *active[48];

static void queue(struct meta **phead, struct meta *m)
{
	assert(!m->next && !m->prev);
	if (*phead) {
		struct meta *head = *phead;
		m->next = head;
		m->prev = head->prev;
		m->next->prev = m->prev->next = m;
	} else {
		m->prev = m->next = m;
		*phead = m;
	}
}

static void dequeue(struct meta **phead, struct meta *m)
{
	if (m->next != m) {
		m->prev->next = m->next;
		m->next->prev = m->prev;
		if (*phead == m) *phead = m->next;
	} else {
		*phead = 0;
	}
	m->prev = m->next = 0;
}

static struct meta *dequeue_head(struct meta **phead)
{
	struct meta *m = *phead;
	if (m) dequeue(phead, m);
	return m;
}

static struct meta *alloc_meta(void)
{
	struct meta *m;
	if ((m = dequeue_head(&free_meta_head))) return m;
	if (!avail_meta_count) {
		char *p;
		p = mmap(0, 2*PAGESIZE, PROT_NONE, MAP_PRIVATE|MAP_ANON, -1, 0);
		if (!p) return 0;
		if (mprotect(p+PAGESIZE, PAGESIZE, PROT_READ|PROT_WRITE)) {
			munmap(p, 2*PAGESIZE);
			return 0;
		}
		avail_meta_count = PAGESIZE/sizeof *m;
		avail_meta = (void *)(p + PAGESIZE);
	}
	avail_meta_count--;
	m = avail_meta++;
	m->prev = m->next = 0;
	return m;
}

static void free_meta(struct meta *m)
{
	*m = (struct meta){0};
	queue(&free_meta_head, m);
}

static uint32_t try_avail(struct meta **pm, uint32_t mask)
{
	struct meta *m = *pm;
	uint32_t first;
	if (!mask) {
		if (!m) return 0;
		if (!m->freed_mask) {
			dequeue(pm, m);
			queue(&full_groups_head, m);
			m = *pm;
			if (!m) return 0;
		} else {
			*pm = m = m->next;
		}
		mask = a_swap(&m->freed_mask, 0);
		if (!mask) return 0;
	}
	first = mask&-mask;
	m->avail_mask = mask-first;
	return first;
}

static int get_slot_index(const unsigned char *p)
{
	return p[-3] & 31;
}

static struct meta *get_meta(const unsigned char *p)
{
	int offset = *(const uint16_t *)(p - 2);
	int index = get_slot_index(p);
	assert(!p[-4]);
	const struct group *base = (const void *)(p - 16*offset - sizeof *base);
	const struct meta *meta = base->meta;
	assert(meta->mem == base);
	assert(index <= meta->last_idx);
	if (meta->sizeclass < 48) {
		assert(offset >= size_classes[meta->sizeclass]*index);
		assert(offset < size_classes[meta->sizeclass]*(index+1));
	} else {
		assert(meta->sizeclass == 63);
		assert(offset <= meta->maplen*4096/16 - 1);
	}
	return (struct meta *)meta;
}

static size_t get_nominal_size(const unsigned char *p, const unsigned char *end)
{
	size_t reserved = p[-3] >> 5;
	if (reserved >= 5) {
		assert(reserved == 5);
		reserved = *(const uint32_t *)(end-4);
		assert(reserved >= 5 && !end[-5]);
	}
	assert(reserved <= end-p && !*(end-reserved));
	return end-reserved-p;
}

static void nontrivial_free(struct meta *, int, uint32_t);

static void free_group(struct meta *g)
{
	if (g->maplen) {
		munmap(g->mem, g->maplen*4096);
	} else if (g->freeable) {
		void *p = g->mem;
		struct meta *m = get_meta(p);
		int idx = get_slot_index(p);
		nontrivial_free(m, idx, m->freed_mask);
	}
	free_meta(g);
}

static struct meta *alloc_group(size_t n, int cnt)
{
	struct meta *m;
	size_t needed = (n+4)*cnt + sizeof(struct group);
	if (1 || needed >= MMAP_THRESHOLD) {
		void *p;
		p = mmap(0, needed, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
		if (!p) return 0;
		m = alloc_meta();
		if (!m) {
			munmap(p, needed);
			return 0;
		}
		m->avail_mask = (2u<<(cnt-1))-1;
		m->freed_mask = 0;
		m->mem = p;
		m->mem->meta = m;
		m->last_idx = cnt-1;
		m->freeable = 1;
		m->sizeclass = n>=16*size_classes[47] ? 63 : size_to_class(n);
		m->maplen = (needed+4095)/4096;
		return m;
	}
#if 0
	int sc = size_to_class(

	if (needed < MMAP_THRESHOLD) {
		int i, sc = size_to_class(needed);
		for (i=sc; i<SIZE_CLASS_CNT; i++) {
			if (active[i]) {
				if (!active[i]->avail_mask)
					cycle_full_group(&active[i]);
				if (active[i]) break;
			}
		}
		if (i<SIZE_CLASS_CNT)
	}
#endif
}

static size_t get_stride(struct meta *g)
{
	if (g->sizeclass >= 48) {
		assert(g->sizeclass == 63);
		return g->maplen*4096 - sizeof(struct group);
	} else {
		return 16*size_classes[g->sizeclass];
	}
}

static void set_size(unsigned char *p, unsigned char *end, size_t n)
{
	int reserved = end-p-n;
	if (reserved >= 5) {
		*(uint32_t *)(end-4) = reserved;
		end[-5] = 0;
		reserved = 5;
	}
	if (reserved) end[-reserved] = 0;
	p[-3] = (p[-3]&31) + (reserved<<5);
}

static void *enframe(struct meta *g, int idx, size_t n)
{
	size_t stride = get_stride(g);
	unsigned char *p = g->mem->storage + stride*idx;
	*(uint16_t *)(p-2) = (p-g->mem->storage)/16U;
	p[-3] = idx;
	set_size(p, p+stride-4, n);
	return p;
}

static int size_overflows(size_t n)
{
	if (n >= SIZE_MAX/2 - 4096) {
		errno = ENOMEM;
		return 1;
	}
	return 0;
}

void *malloc(size_t n)
{
	if (size_overflows(n)) return 0;
	struct meta *cur;
	uint32_t mask, first;
	int sc;

	if (n >= MMAP_THRESHOLD) {
		wrlock();
		struct meta *m = alloc_group(n, 1);
		if (!m) {
			unlock();
			return 0;
		}
		m->avail_mask = m->freed_mask = 0;
		queue(&full_groups_head, m);
		cur = m;
		first = 1;
		goto success;
	}

	sc = size_to_class(n);

	rdlock();
	cur = active[sc];
	if (0) for (;;) {
		mask = cur ? cur->avail_mask : 0;
		first = mask&-mask;
		if (!first) break;
		if (!libc.threads_minus_1) {
			cur->avail_mask = mask-first;
			goto success;
		}
		if (a_cas(&cur->avail_mask, mask, mask-first)==mask) {
			goto success;
		}
	}
	upgradelock();
//FIXME
	cur = active[sc];
	mask = cur ? cur->avail_mask : 0;
	if ((first = try_avail(&active[sc], mask))) {
		cur = active[sc];
		goto success;
	}
	cur = alloc_group(16*size_classes[sc]-4, 15);
	first = 1;
	if (!cur) goto fail;
	cur->avail_mask -= first;
	queue(&active[sc], cur);

success:
	unlock();
	struct meta *g = cur;
	int idx = a_ctz_32(first);
	return enframe(g, idx, n);
fail:
	unlock();
	return 0;
}

static void nontrivial_free(struct meta *g, int i, uint32_t mask)
{
	uint32_t self = 1u<<i;
	int sc = g->sizeclass;
	if (!mask) {
		// might still be active, or may be on full groups list
		if (active[sc] != g) {
			dequeue(&full_groups_head, g);
			queue(&active[sc], g);
		}
	} else if (mask+self == (2u<<g->last_idx)-1 && (g->maplen || g->freeable)) {
		// FIXME: decide whether to free the whole group
		// hack because we don't know what list it's on to call dequeue
		if (active[sc]==g) {
			if (g->next != g) {
				struct meta *m = g->next;
				active[sc] = m;
				m->avail_mask = a_swap(&m->freed_mask, 0);
			} else {
				active[sc] = 0;
			}
		}
		// may not be on here, but if it was instead on active[sc] the
		// above already covered it.
		dequeue(&full_groups_head, g);
		free_group(g);
		return;
	}
	a_or(&g->freed_mask, self);
}


void free(void *p)
{
	if (!p) return;

	struct meta *g = get_meta(p);
	int idx = get_slot_index(p);
	get_nominal_size(p, g->mem->storage+get_stride(g)*(idx+1)-4);
	unsigned mask, self = 1u<<idx, all = (2u<<g->last_idx)-1;

	// atomic free without locking if this is neither first or last slot
	do {
		mask = g->freed_mask;
		assert(!(mask&self));
	} while (mask && mask+self!=all && a_cas(&g->freed_mask, mask, mask+self)!=mask);
	if (mask && mask+self!=all) return;

	/* free individually-mmapped allocation by performing munmap
	 * before taking the lock, since we are exclusive user. */
	if (g->sizeclass >= 48) {
		assert(g->sizeclass==63 && g->maplen && !g->last_idx);
		munmap(g->mem, g->maplen*4096);
		wrlock();
		dequeue(&full_groups_head, g);
		free_meta(g);
		unlock();
		return;
	}

	wrlock();
	nontrivial_free(g, idx, mask);
	unlock();
}

void *realloc(void *p, size_t n)
{
	if (!p) return malloc(n);
	if (size_overflows(n)) return 0;

	struct meta *g = get_meta(p);
	int idx = get_slot_index(p);
	size_t stride = get_stride(g);
	unsigned char *start = g->mem->storage + stride*idx;
	unsigned char *end = start + stride - 4;
	size_t old_size = get_nominal_size(p, end);
	size_t avail_size = end-(unsigned char *)p;
	void *new;

	// fixme: don't do this unless size class doesn't reduce
	if (n <= avail_size) {
		set_size(p, end, n);
		return p;
	}

	// use mremap if old and new size are both mmap-worthy
	if (g->sizeclass>=48 && n>=MMAP_THRESHOLD) {
		assert(g->sizeclass==63);
		size_t base = (unsigned char *)p-start;
		size_t needed = (n + base + sizeof *g->mem + 4 + 4095) & -4096;
		new = mremap(g->mem, g->maplen*4096, needed, MREMAP_MAYMOVE);
		if (new) {
			g->mem = new;
			g->maplen = needed/4096;
			p = g->mem->storage + base;
			end = g->mem->storage + (needed - sizeof *g->mem);
			set_size(p, end, n);
			return p;
		}
	}

	new = malloc(n);
	if (!new) return 0;
	memcpy(new, p, old_size);
	free(p);
	return new;
}

void *calloc(size_t m, size_t n)
{
	if (n && m > (size_t)-1/n) {
		errno = ENOMEM;
		return 0;
	}
	n *= m;
	void *p = malloc(n);
	if (!p) return p;
	return memset(p, 0, n);
}

size_t malloc_usable_size(void *p)
{
	struct meta *g = get_meta(p);
	int idx = get_slot_index(p);
	size_t stride = get_stride(g);
	unsigned char *start = g->mem->storage + stride*idx;
	unsigned char *end = start + stride - 4;
	return get_nominal_size(p, end);
}

#include <stdio.h>

static void print_group(FILE *f, struct meta *g)
{
	fprintf(f, "%p: %p [%d slots] [class %d (%d)]: ", g, g->mem,
		g->last_idx+1, g->sizeclass,
			g->sizeclass>48?g->maplen*4096-16:16*size_classes[g->sizeclass]);
	for (int i=0; i<=g->last_idx; i++) {
		putc((g->avail_mask & (1u<<i)) ? 'a'
			: (g->freed_mask & (1u<<i)) ? 'f' : '_', f);
	}
	putc('\n', f);
}

static void print_group_list(FILE *f, struct meta *h)
{
	struct meta *m = h;
	if (!m) return;
	do print_group(f, m);
	while ((m=m->next)!=h);
}

void dump_heap(FILE *f)
{
	wrlock();

	fprintf(f, "free meta records:\n");
	print_group_list(f, free_meta_head);

	fprintf(f, "entirely filled, inactive groups:\n");
	print_group_list(f, full_groups_head);

	fprintf(f, "free groups by size class:\n");
	for (int i=0; i<48; i++) {
		if (!active[i]) continue;
		fprintf(f, "-- class %d (%d) --\n", i, size_classes[i]*16);
		print_group_list(f, active[i]);
	}

	unlock();
}