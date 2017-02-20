/**
 * author: zhoukk
 * link: https://github.com/zhoukk/ring
 *
 * A
 * Cache-Optimized
 * Multi-Producer & Multi-Consumer
 * Lock-Free
 * Concurrent
 * FIFO
 * Circular Array Based
 * Ring.
 */

#ifndef _ring_h_
#define _ring_h_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RING_API
#define RING_API extern
#endif // RING_API

/**
 * Flag used when ring initialize.
 */
#define RING_F_SP 0x01	/* Default push allow single producer. */
#define RING_F_SC 0x02	/* Default pop allow single consumer. */

/**
 * Behavior used when push and pop.
 */
#define RING_B_FIXED 0		/* Push/Pop fixed number of objects on a ring. */
#define RING_B_VARIABLE 1	/* Push/Pop as many objects as possible on a ring. */

#define RING_SIZE_MASK (unsigned)(0x0fffffff)	/* Ring size mask. */

/**
 * Yield after times of pause, no yield
 * if RING_PAUSE_REP not defined.
 */
#ifndef RING_PAUSE_REP
#define RING_PAUSE_REP 0
#endif

struct ring;

/**
 * Calculate the memory size needed for a ring structure.
 *
 * @param count
 *		The number of elements in the ring (must be power of 2).
 * @return
 *		The memory size needed for the ring on success.
 *		Or 0 if count is not power of 2 or greater than ring size mask.
 */
RING_API unsigned ring_memsize(unsigned count);


/**
 * Initialize a ring structure.
 *
 * @param r
 *		The pointer to the ring structure.
 * @param count
 *		The number of elements in the ring (must be power of 2).
 * @param flags
 *		Or of the following:
 *		- RING_F_SP:  If the flag is set, only allow one producer.
 *		- RING_F_SC:  If the flag is set, only allow one consumer.
 * @return
 *		no return.
 */
RING_API void ring_init(struct ring *r, unsigned count, unsigned flags);


/**
 * Push several objects on the ring.
 *
 * @param r
 * 		A pointer to the ring structure.
 * @param objs
 *		A pointer to a list of void * pointers (objects) to pushed.
 * @param n
 *		The number of objects to add on the ring.
 * @param behavior
 *		RING_B_FIXED:	Push a fixed number of objects to a ring.
 *		RING_B_VARIABLE:Push as many objects as possible to a ring.
 * @return
 *		- 0: Not enough room in the ring to push, no object is pushed.
 *		- n: Number of objects pushed.
 */
RING_API unsigned ring_push(struct ring *r, void * const *objs, unsigned n, int behavior);


/**
 * Pop several objects from a ring.
 * @param r
 * 		A pointer to the ring structure.
 * @param objs
 *		A pointer to a list of void * pointers (objects) that will be filled.
 * @param n
 *		The number of objects to pop from the ring.
 * @param behavior
 *		RING_B_FIXED:	Pop a fixed number of objects from a ring.
 *		RING_B_VARIABLE:Pop as many objects as possible from a ring.
 * @return
 *		- O: Not enough entries in the ring to pop, no object is poped.
 *		- n: Actual number of objects poped.
 */
RING_API unsigned ring_pop(struct ring *r, void **objs, unsigned n, int behavior);


/**
 * Test if a ring is full.
 *
 * @param r
 *		A pointer to the ring structure.
 * @return
 *		- 1:  The ring is full.
 *		- 0:  The ring is not full.
 */
RING_API int ring_full(const struct ring *r);


/**
 * Test if a ring is empty.
 *
 * @param r
 *		A pointer to the ring structure.
 * @return
 *		- 1:  The ring is empty.
 *		- 0:  The ring is not empty.
 */
RING_API int ring_empty(const struct ring *r);


/**
 * Return the number of entries in a ring.
 *
 * @param r
 *		A pointer to the ring structure.
 * @return
 *		The number of entries in the ring.
 */
RING_API unsigned ring_count(const struct ring *r);


/**
 * Return the number of free entries in a ring.
 *
 * @param r
 *		A pointer to the ring structure.
 * @return
 *		The number of free entries in the ring.
 */
RING_API unsigned ring_avail(const struct ring *r);


#ifdef __cplusplus
}
#endif

#endif // _ring_h_


#ifdef RING_IMPLEMENTATION

/**
 * Implement
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <inttypes.h>
#include <emmintrin.h>
#include <sched.h>

#include <stddef.h>
#ifndef offsetof
#define offsetof(s,m) __builtin_offsetof(s,m)
#endif

#ifndef likely
#define likely(x) __builtin_expect((x),1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect((x),0)
#endif

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

#define cache_aligned __attribute__((__aligned__(CACHE_LINE_SIZE)))

#define compiler_barrier() do { \
	asm volatile("":::"memory"); \
} while (0)

/* True if x is power of 2. */
#define POWEROF2(x) ((((x)-1) & (x)) == 0)

#define CAS(p,o,n) __sync_bool_compare_and_swap(p,o,n)

struct ring {
	/* Ring producer struct. */
	struct prod {
		uint32_t sp;
		uint32_t size;
		uint32_t mask;
		volatile uint32_t head;
		volatile uint32_t tail;
	} prod cache_aligned;

	/* Ring consumer struct. */
	struct cons {
		uint32_t sc;
		uint32_t size;
		uint32_t mask;
		volatile uint32_t head;
		volatile uint32_t tail;
	} cons cache_aligned;

	/* Memory space of ring data. */
	void *ring[0] cache_aligned;
};

/* Align x to next power of 2. */
static inline uint32_t
align32_pow2(uint32_t x) {
	x--;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;

	return x + 1;
}

/* Align 64bit x to next power of 2. */
static inline uint64_t
align64_pow2(uint64_t x) {
	x--;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
	x |= x >> 32;

	return x + 1;
}

RING_API unsigned
ring_memsize(unsigned count) {
	unsigned sz;

	if ((!POWEROF2(count)) || (count > RING_SIZE_MASK)) {
		return 0;
	}
	sz = sizeof(struct ring) + count * sizeof(void *);
	return sz;
}

RING_API void
ring_init(struct ring *r, unsigned count, unsigned flags) {
	memset(r, 0, sizeof(*r));
	r->prod.sp = !!(flags & RING_F_SP);
	r->cons.sc = !!(flags & RING_F_SC);
	r->prod.size = r->cons.size = count;
	r->prod.mask = r->cons.mask = count - 1;
	r->prod.head = r->cons.head = 0;
	r->prod.tail = r->cons.tail = 0;
}

#define PUSH_PTRS() do { \
	const uint32_t size = r->prod.size; \
	uint32_t idx = prod_head & mask; \
	unsigned i; \
	if (likely(idx + n < size)) { \
		for (i = 0; i < (n & ((~(unsigned)0x3))); i+=4, idx+=4) { \
			r->ring[idx] = objs[i]; \
			r->ring[idx+1] = objs[i+1]; \
			r->ring[idx+2] = objs[i+2]; \
			r->ring[idx+3] = objs[i+3]; \
		} \
		switch (n & 0x3) { \
			case 3: r->ring[idx++] = objs[i++]; \
			case 2: r->ring[idx++] = objs[i++]; \
			case 1: r->ring[idx++] = objs[i++]; \
		} \
	} else { \
		for (i = 0; idx < size; i++, idx++) \
		r->ring[idx] = objs[i]; \
		for (idx = 0; i < n; i++, idx++) \
		r->ring[idx] = objs[i]; \
	} \
} while (0)

/* Single producer push on the ring. */
static unsigned
ring_sp_push(struct ring *r, void * const *objs, unsigned n, int behavior) {
	uint32_t prod_head, cons_tail;
	uint32_t prod_next, avail;
	uint32_t mask = r->prod.mask;

	prod_head = r->prod.head;
	cons_tail = r->cons.tail;
	avail = mask + cons_tail - prod_head;

	if (unlikely(n > avail)) {
		if (behavior == RING_B_FIXED) {
			return 0;
		} else {
			if (unlikely(avail == 0)) {
				return 0;
			}
			n = avail;
		}
	}
	prod_next = prod_head + n;
	r->prod.head = prod_next;

	PUSH_PTRS();
	compiler_barrier();

	r->prod.tail = prod_next;
	return n;
}

/* Multi producer push on the ring. */
static unsigned
ring_mp_push(struct ring *r, void * const *objs, unsigned n, int behavior) {
	uint32_t prod_head, prod_next;
	uint32_t cons_tail, avail;
	uint32_t mask = r->prod.mask;
	const unsigned max = n;
	int ok;

	do {
		n = max;
		prod_head = r->prod.head;
		cons_tail = r->cons.tail;
		avail = mask + cons_tail - prod_head;

		if (unlikely(n > avail)) {
			if (behavior == RING_B_FIXED) {
				return 0;
			} else {
				if (unlikely(avail == 0)) {
					return 0;
				}
				n = avail;
			}
		}
		prod_next = prod_head + n;
		ok = CAS(&r->prod.head, prod_head, prod_next);
	} while (unlikely(!ok));

	PUSH_PTRS();
	compiler_barrier();

	int rep = 0;
	while (unlikely(r->prod.tail != prod_head)) {
		_mm_pause();
		if (RING_PAUSE_REP && ++rep == RING_PAUSE_REP) {
			rep = 0;
			sched_yield();
		}
	}

	r->prod.tail = prod_next;
	return n;
}

RING_API unsigned
ring_push(struct ring *r, void * const *objs, unsigned n, int behavior) {
	if (r->prod.sp)
		return ring_sp_push(r, objs, n, behavior);
	else
		return ring_mp_push(r, objs, n, behavior);
}

#define POP_PTRS() do { \
	uint32_t idx = cons_head & mask; \
	const uint32_t size = r->cons.size; \
	unsigned i; \
	if (likely(idx + n < size)) { \
		for (i = 0; i < (n & (~(unsigned)0x3)); i+=4, idx+=4) { \
			objs[i] = r->ring[idx]; \
			objs[i+1] = r->ring[idx+1]; \
			objs[i+2] = r->ring[idx+2]; \
			objs[i+3] = r->ring[idx+3]; \
		} \
		switch (n & 0x3) { \
			case 3: objs[i++] = r->ring[idx++]; \
			case 2: objs[i++] = r->ring[idx++]; \
			case 1: objs[i++] = r->ring[idx++]; \
		} \
	} else { \
		for (i = 0; idx < size; i++, idx++) \
		objs[i] = r->ring[idx]; \
		for (idx = 0; i < n; i++, idx++) \
		objs[i] = r->ring[idx]; \
	} \
} while (0)


/* Single consumer pop on the ring. */
static unsigned
ring_sc_pop(struct ring *r, void **objs, unsigned n, int behavior) {
	uint32_t cons_head, prod_tail;
	uint32_t cons_next, avail;
	uint32_t mask = r->prod.mask;

	cons_head = r->cons.head;
	prod_tail = r->prod.tail;
	avail = prod_tail - cons_head;

	if (n > avail) {
		if (behavior == RING_B_FIXED) {
			return 0;
		} else {
			if (unlikely(avail == 0)) {
				return 0;
			}
			n = avail;
		}
	}
	cons_next = cons_head + n;
	r->cons.head = cons_next;

	POP_PTRS();
	compiler_barrier();

	r->cons.tail = cons_next;
	return n;
}

/* Multi consumer pop on the ring. */
static unsigned
ring_mc_pop(struct ring *r, void **objs, unsigned n, int behavior) {
	uint32_t cons_head, prod_tail;
	uint32_t cons_next, avail;
	uint32_t mask = r->prod.mask;
	const unsigned max = n;
	int ok;

	do {
		n = max;

		cons_head = r->cons.head;
		prod_tail = r->prod.tail;
		avail = prod_tail - cons_head;

		if (n > avail) {
			if (behavior == RING_B_FIXED) {
				return 0;
			} else {
				if (unlikely(avail == 0)) {
					return 0;
				}
				n = avail;
			}
		}
		cons_next = cons_head + n;
		ok = CAS(&r->cons.head, cons_head, cons_next);
	} while (unlikely(!ok));

	POP_PTRS();
	compiler_barrier();

	int rep = 0;
	while (unlikely(r->cons.tail != cons_head)) {
		_mm_pause();
		if (RING_PAUSE_REP && ++rep == RING_PAUSE_REP) {
			rep = 0;
			sched_yield();
		}
	}

	r->cons.tail = cons_next;
	return n;
}

RING_API unsigned
ring_pop(struct ring *r, void **objs, unsigned n, int behavior) {
	if (r->cons.sc)
		return ring_sc_pop(r, objs, n, behavior);
	else
		return ring_mc_pop(r, objs, n, behavior);
}

RING_API int
ring_full(const struct ring *r) {
	uint32_t prod_tail = r->prod.tail;
	uint32_t cons_tail = r->cons.tail;
	return (((cons_tail - prod_tail - 1) & r->prod.mask) == 0);
}

RING_API int
ring_empty(const struct ring *r) {
	uint32_t prod_tail = r->prod.tail;
	uint32_t cons_tail = r->cons.tail;
	return !!(cons_tail == prod_tail);
}

RING_API unsigned
ring_count(const struct ring *r) {
	uint32_t prod_tail = r->prod.tail;
	uint32_t cons_tail = r->cons.tail;
	return ((prod_tail - cons_tail) & r->prod.mask);
}

RING_API unsigned
ring_avail(const struct ring *r) {
	uint32_t prod_tail = r->prod.tail;
	uint32_t cons_tail = r->cons.tail;
	return ((cons_tail - prod_tail - 1) & r->prod.mask);
}

#endif // RING_IMPLEMENTATION
