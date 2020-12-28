#include "test/jemalloc_test.h"

#include "jemalloc/internal/sec.h"

typedef struct pai_test_allocator_s pai_test_allocator_t;
struct pai_test_allocator_s {
	pai_t pai;
	bool alloc_fail;
	size_t alloc_count;
	size_t dalloc_count;
	/*
	 * We use a simple bump allocator as the implementation.  This isn't
	 * *really* correct, since we may allow expansion into a subsequent
	 * allocation, but it's not like the SEC is really examining the
	 * pointers it gets back; this is mostly just helpful for debugging.
	 */
	uintptr_t next_ptr;
	size_t expand_count;
	bool expand_return_value;
	size_t shrink_count;
	bool shrink_return_value;
};

static inline edata_t *
pai_test_allocator_alloc(tsdn_t *tsdn, pai_t *self, size_t size,
    size_t alignment, bool zero) {
	pai_test_allocator_t *ta = (pai_test_allocator_t *)self;
	if (ta->alloc_fail) {
		return NULL;
	}
	edata_t *edata = malloc(sizeof(edata_t));
	assert_ptr_not_null(edata, "");
	ta->next_ptr += alignment - 1;
	edata_init(edata, /* arena_ind */ 0,
	    (void *)(ta->next_ptr & ~(alignment - 1)), size,
	    /* slab */ false,
	    /* szind */ 0, /* sn */ 1, extent_state_active, /* zero */ zero,
	    /* comitted */ true, /* ranged */ false, EXTENT_NOT_HEAD);
	ta->next_ptr += size;
	ta->alloc_count++;
	return edata;
}

static bool
pai_test_allocator_expand(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size, bool zero) {
	pai_test_allocator_t *ta = (pai_test_allocator_t *)self;
	ta->expand_count++;
	return ta->expand_return_value;
}

static bool
pai_test_allocator_shrink(tsdn_t *tsdn, pai_t *self, edata_t *edata,
    size_t old_size, size_t new_size) {
	pai_test_allocator_t *ta = (pai_test_allocator_t *)self;
	ta->shrink_count++;
	return ta->shrink_return_value;
}

static void
pai_test_allocator_dalloc(tsdn_t *tsdn, pai_t *self, edata_t *edata) {
	pai_test_allocator_t *ta = (pai_test_allocator_t *)self;
	ta->dalloc_count++;
	free(edata);
}

static inline void
pai_test_allocator_init(pai_test_allocator_t *ta) {
	ta->alloc_fail = false;
	ta->alloc_count = 0;
	ta->dalloc_count = 0;
	/* Just don't start the edata at 0. */
	ta->next_ptr = 10 * PAGE;
	ta->expand_count = 0;
	ta->expand_return_value = false;
	ta->shrink_count = 0;
	ta->shrink_return_value = false;
	ta->pai.alloc = &pai_test_allocator_alloc;
	ta->pai.expand = &pai_test_allocator_expand;
	ta->pai.shrink = &pai_test_allocator_shrink;
	ta->pai.dalloc = &pai_test_allocator_dalloc;
}

TEST_BEGIN(test_reuse) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;
	/*
	 * We can't use the "real" tsd, since we malloc within the test
	 * allocator hooks; we'd get lock inversion crashes.  Eventually, we
	 * should have a way to mock tsds, but for now just don't do any
	 * lock-order checking.
	 */
	tsdn_t *tsdn = TSDN_NULL;
	/*
	 * 10-allocs apiece of 1-PAGE and 2-PAGE objects means that we should be
	 * able to get to 30 pages in the cache before triggering a flush.
	 */
	enum { NALLOCS = 10 };
	edata_t *one_page[NALLOCS];
	edata_t *two_page[NALLOCS];
	sec_init(&sec, &ta.pai, /* nshards */ 1, /* alloc_max */ 2 * PAGE,
	    /* bytes_max */ NALLOCS * PAGE + NALLOCS * 2 * PAGE);
	for (int i = 0; i < NALLOCS; i++) {
		one_page[i] = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
		    /* zero */ false);
		expect_ptr_not_null(one_page[i], "Unexpected alloc failure");
		two_page[i] = pai_alloc(tsdn, &sec.pai, 2 * PAGE, PAGE,
		    /* zero */ false);
		expect_ptr_not_null(one_page[i], "Unexpected alloc failure");
	}
	expect_zu_eq(2 * NALLOCS, ta.alloc_count,
	    "Incorrect number of allocations");
	expect_zu_eq(0, ta.dalloc_count,
	    "Incorrect number of allocations");
	/*
	 * Free in a different order than we allocated, to make sure free-list
	 * separation works correctly.
	 */
	for (int i = NALLOCS - 1; i >= 0; i--) {
		pai_dalloc(tsdn, &sec.pai, one_page[i]);
	}
	for (int i = NALLOCS - 1; i >= 0; i--) {
		pai_dalloc(tsdn, &sec.pai, two_page[i]);
	}
	expect_zu_eq(2 * NALLOCS, ta.alloc_count,
	    "Incorrect number of allocations");
	expect_zu_eq(0, ta.dalloc_count,
	    "Incorrect number of allocations");
	/*
	 * Check that the n'th most recent deallocated extent is returned for
	 * the n'th alloc request of a given size.
	 */
	for (int i = 0; i < NALLOCS; i++) {
		edata_t *alloc1 = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
		    /* zero */ false);
		edata_t *alloc2 = pai_alloc(tsdn, &sec.pai, 2 * PAGE, PAGE,
		    /* zero */ false);
		expect_ptr_eq(one_page[i], alloc1,
		    "Got unexpected allocation");
		expect_ptr_eq(two_page[i], alloc2,
		    "Got unexpected allocation");
	}
	expect_zu_eq(2 * NALLOCS, ta.alloc_count,
	    "Incorrect number of allocations");
	expect_zu_eq(0, ta.dalloc_count,
	    "Incorrect number of allocations");
}
TEST_END


TEST_BEGIN(test_auto_flush) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;
	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;
	/*
	 * 10-allocs apiece of 1-PAGE and 2-PAGE objects means that we should be
	 * able to get to 30 pages in the cache before triggering a flush.
	 */
	enum { NALLOCS = 10 };
	edata_t *extra_alloc;
	edata_t *allocs[NALLOCS];
	sec_init(&sec, &ta.pai, /* nshards */ 1, /* alloc_max */ PAGE,
	    /* bytes_max */ NALLOCS * PAGE);
	for (int i = 0; i < NALLOCS; i++) {
		allocs[i] = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
		    /* zero */ false);
		expect_ptr_not_null(allocs[i], "Unexpected alloc failure");
	}
	extra_alloc = pai_alloc(tsdn, &sec.pai, PAGE, PAGE, /* zero */ false);
	expect_ptr_not_null(extra_alloc, "Unexpected alloc failure");
	expect_zu_eq(NALLOCS + 1, ta.alloc_count,
	    "Incorrect number of allocations");
	expect_zu_eq(0, ta.dalloc_count,
	    "Incorrect number of allocations");
	/* Free until the SEC is full, but should not have flushed yet. */
	for (int i = 0; i < NALLOCS; i++) {
		pai_dalloc(tsdn, &sec.pai, allocs[i]);
	}
	expect_zu_eq(NALLOCS + 1, ta.alloc_count,
	    "Incorrect number of allocations");
	expect_zu_eq(0, ta.dalloc_count,
	    "Incorrect number of allocations");
	/*
	 * Free the extra allocation; this should trigger a flush of all
	 * extents in the cache.
	 */
	pai_dalloc(tsdn, &sec.pai, extra_alloc);
	expect_zu_eq(NALLOCS + 1, ta.alloc_count,
	    "Incorrect number of allocations");
	expect_zu_eq(NALLOCS + 1, ta.dalloc_count,
	    "Incorrect number of deallocations");
}
TEST_END

/*
 * A disable and a flush are *almost* equivalent; the only difference is what
 * happens afterwards; disabling disallows all future caching as well.
 */
static void
do_disable_flush_test(bool is_disable) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;
	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;

	enum { NALLOCS = 10 };
	edata_t *allocs[NALLOCS];
	sec_init(&sec, &ta.pai, /* nshards */ 1, /* alloc_max */ PAGE,
	    /* bytes_max */ NALLOCS * PAGE);
	for (int i = 0; i < NALLOCS; i++) {
		allocs[i] = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
		    /* zero */ false);
		expect_ptr_not_null(allocs[i], "Unexpected alloc failure");
	}
	/* Free all but the last aloc. */
	for (int i = 0; i < NALLOCS - 1; i++) {
		pai_dalloc(tsdn, &sec.pai, allocs[i]);
	}
	expect_zu_eq(NALLOCS, ta.alloc_count,
	    "Incorrect number of allocations");
	expect_zu_eq(0, ta.dalloc_count,
	    "Incorrect number of allocations");

	if (is_disable) {
		sec_disable(tsdn, &sec);
	} else {
		sec_flush(tsdn, &sec);
	}

	expect_zu_eq(NALLOCS, ta.alloc_count,
	    "Incorrect number of allocations");
	expect_zu_eq(NALLOCS - 1, ta.dalloc_count,
	    "Incorrect number of deallocations");

	/*
	 * If we free into a disabled SEC, it should forward to the fallback.
	 * Otherwise, the SEC should accept the allocation.
	 */
	pai_dalloc(tsdn, &sec.pai, allocs[NALLOCS - 1]);

	expect_zu_eq(NALLOCS, ta.alloc_count,
	    "Incorrect number of allocations");
	expect_zu_eq(is_disable ? NALLOCS : NALLOCS - 1, ta.dalloc_count,
	    "Incorrect number of deallocations");
}

TEST_BEGIN(test_disable) {
	do_disable_flush_test(/* is_disable */ true);
}
TEST_END

TEST_BEGIN(test_flush) {
	do_disable_flush_test(/* is_disable */ false);
}
TEST_END

TEST_BEGIN(test_alloc_max_respected) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;
	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;

	size_t alloc_max = 2 * PAGE;
	size_t attempted_alloc = 3 * PAGE;

	sec_init(&sec, &ta.pai, /* nshards */ 1, alloc_max,
	    /* bytes_max */ 1000 * PAGE);

	for (size_t i = 0; i < 100; i++) {
		expect_zu_eq(i, ta.alloc_count,
		    "Incorrect number of allocations");
		expect_zu_eq(i, ta.dalloc_count,
		    "Incorrect number of deallocations");
		edata_t *edata = pai_alloc(tsdn, &sec.pai, attempted_alloc,
		    PAGE, /* zero */ false);
		expect_ptr_not_null(edata, "Unexpected alloc failure");
		expect_zu_eq(i + 1, ta.alloc_count,
		    "Incorrect number of allocations");
		expect_zu_eq(i, ta.dalloc_count,
		    "Incorrect number of deallocations");
		pai_dalloc(tsdn, &sec.pai, edata);
	}
}
TEST_END

TEST_BEGIN(test_expand_shrink_delegate) {
	/*
	 * Expand and shrink shouldn't affect sec state; they should just
	 * delegate to the fallback PAI.
	 */
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;
	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;

	sec_init(&sec, &ta.pai, /* nshards */ 1, /* alloc_max */ 10 * PAGE,
	    /* bytes_max */ 1000 * PAGE);
	edata_t *edata = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
	    /* zero */ false);
	expect_ptr_not_null(edata, "Unexpected alloc failure");

	bool err = pai_expand(tsdn, &sec.pai, edata, PAGE, 4 * PAGE,
	    /* zero */ false);
	expect_false(err, "Unexpected expand failure");
	expect_zu_eq(1, ta.expand_count, "");
	ta.expand_return_value = true;
	err = pai_expand(tsdn, &sec.pai, edata, 4 * PAGE, 3 * PAGE,
	    /* zero */ false);
	expect_true(err, "Unexpected expand success");
	expect_zu_eq(2, ta.expand_count, "");

	err = pai_shrink(tsdn, &sec.pai, edata, 4 * PAGE, 2 * PAGE);
	expect_false(err, "Unexpected shrink failure");
	expect_zu_eq(1, ta.shrink_count, "");
	ta.shrink_return_value = true;
	err = pai_shrink(tsdn, &sec.pai, edata, 2 * PAGE, PAGE);
	expect_true(err, "Unexpected shrink success");
	expect_zu_eq(2, ta.shrink_count, "");
}
TEST_END

TEST_BEGIN(test_nshards_0) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;
	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;

	sec_init(&sec, &ta.pai, /* nshards */ 0, /* alloc_max */ 10 * PAGE,
	    /* bytes_max */ 1000 * PAGE);

	edata_t *edata = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
	    /* zero */ false);
	pai_dalloc(tsdn, &sec.pai, edata);

	/* Both operations should have gone directly to the fallback. */
	expect_zu_eq(1, ta.alloc_count, "");
	expect_zu_eq(1, ta.dalloc_count, "");
}
TEST_END

static void
expect_stats_pages(tsdn_t *tsdn, sec_t *sec, size_t npages) {
	sec_stats_t stats;
	/*
	 * Check that the stats merging accumulates rather than overwrites by
	 * putting some (made up) data there to begin with.
	 */
	stats.bytes = 123;
	sec_stats_merge(tsdn, sec, &stats);
	assert_zu_eq(npages * PAGE + 123, stats.bytes, "");
}

TEST_BEGIN(test_stats_simple) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;

	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;

	enum {
		NITERS = 100,
		FLUSH_PAGES = 10,
	};

	sec_init(&sec, &ta.pai, /* nshards */ 1, /* alloc_max */ PAGE,
	    /* bytes_max */ FLUSH_PAGES * PAGE);

	edata_t *allocs[FLUSH_PAGES];
	for (size_t i = 0; i < FLUSH_PAGES; i++) {
		allocs[i] = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
		    /* zero */ false);
		expect_stats_pages(tsdn, &sec, 0);
	}

	/* Increase and decrease, without flushing. */
	for (size_t i = 0; i < NITERS; i++) {
		for (size_t j = 0; j < FLUSH_PAGES / 2; j++) {
			pai_dalloc(tsdn, &sec.pai, allocs[j]);
			expect_stats_pages(tsdn, &sec, j + 1);
		}
		for (size_t j = 0; j < FLUSH_PAGES / 2; j++) {
			allocs[j] = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
			    /* zero */ false);
			expect_stats_pages(tsdn, &sec, FLUSH_PAGES / 2 - j - 1);
		}
	}
}
TEST_END

TEST_BEGIN(test_stats_auto_flush) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;

	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;

	enum {
		FLUSH_PAGES = 10,
	};

	sec_init(&sec, &ta.pai, /* nshards */ 1, /* alloc_max */ PAGE,
	    /* bytes_max */ FLUSH_PAGES * PAGE);

	edata_t *extra_alloc0;
	edata_t *extra_alloc1;
	edata_t *allocs[2 * FLUSH_PAGES];

	extra_alloc0 = pai_alloc(tsdn, &sec.pai, PAGE, PAGE, /* zero */ false);
	extra_alloc1 = pai_alloc(tsdn, &sec.pai, PAGE, PAGE, /* zero */ false);

	for (size_t i = 0; i < 2 * FLUSH_PAGES; i++) {
		allocs[i] = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
		    /* zero */ false);
		expect_stats_pages(tsdn, &sec, 0);
	}

	for (size_t i = 0; i < FLUSH_PAGES; i++) {
		pai_dalloc(tsdn, &sec.pai, allocs[i]);
		expect_stats_pages(tsdn, &sec, i + 1);
	}
	pai_dalloc(tsdn, &sec.pai, extra_alloc0);
	/* The last dalloc should have triggered a flush. */
	expect_stats_pages(tsdn, &sec, 0);

	/* Flush the remaining pages; stats should still work. */
	for (size_t i = 0; i < FLUSH_PAGES; i++) {
		pai_dalloc(tsdn, &sec.pai, allocs[FLUSH_PAGES + i]);
		expect_stats_pages(tsdn, &sec, i + 1);
	}

	pai_dalloc(tsdn, &sec.pai, extra_alloc1);
	/* The last dalloc should have triggered a flush, again. */
	expect_stats_pages(tsdn, &sec, 0);
}
TEST_END

TEST_BEGIN(test_stats_manual_flush) {
	pai_test_allocator_t ta;
	pai_test_allocator_init(&ta);
	sec_t sec;

	/* See the note above -- we can't use the real tsd. */
	tsdn_t *tsdn = TSDN_NULL;

	enum {
		FLUSH_PAGES = 10,
	};

	sec_init(&sec, &ta.pai, /* nshards */ 1, /* alloc_max */ PAGE,
	    /* bytes_max */ FLUSH_PAGES * PAGE);

	edata_t *allocs[FLUSH_PAGES];
	for (size_t i = 0; i < FLUSH_PAGES; i++) {
		allocs[i] = pai_alloc(tsdn, &sec.pai, PAGE, PAGE,
		    /* zero */ false);
		expect_stats_pages(tsdn, &sec, 0);
	}

	/* Dalloc the first half of the allocations. */
	for (size_t i = 0; i < FLUSH_PAGES / 2; i++) {
		pai_dalloc(tsdn, &sec.pai, allocs[i]);
		expect_stats_pages(tsdn, &sec, i + 1);
	}

	sec_flush(tsdn, &sec);
	expect_stats_pages(tsdn, &sec, 0);

	/* Flush the remaining pages. */
	for (size_t i = 0; i < FLUSH_PAGES / 2; i++) {
		pai_dalloc(tsdn, &sec.pai, allocs[FLUSH_PAGES / 2 + i]);
		expect_stats_pages(tsdn, &sec, i + 1);
	}
	sec_disable(tsdn, &sec);
	expect_stats_pages(tsdn, &sec, 0);
}
TEST_END

int
main(void) {
	return test(
	    test_reuse,
	    test_auto_flush,
	    test_disable,
	    test_flush,
	    test_alloc_max_respected,
	    test_expand_shrink_delegate,
	    test_nshards_0,
	    test_stats_simple,
	    test_stats_auto_flush,
	    test_stats_manual_flush);
}
