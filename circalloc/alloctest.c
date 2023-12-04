#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>

// Some simple test code
#define ASSERT_EQ(a,b) if ((a) != (b)) { printf("ASSERT_EQ(" #a ", " #b ") FAILED in %s::%s()::%d.\n", __FILE__, __FUNCTION__, __LINE__); exit(1); }
#define ASSERT_NE(a,b) if ((a) == (b)) { printf("ASSERT_NE(" #a ", " #b ") FAILED in %s::%s()::%d.\n", __FILE__, __FUNCTION__, __LINE__); exit(1); }
#define ASSERT_LE(a,b) if ((a) > (b)) { printf("ASSERT_LE(" #a ", " #b ") FAILED in %s::%s()::%d.\n", __FILE__, __FUNCTION__, __LINE__); exit(1); }
#define ASSERT_LT(a,b) if ((a) >= (b)) { printf("ASSERT_LT(" #a ", " #b ") FAILED in %s::%s()::%d.\n", __FILE__, __FUNCTION__, __LINE__); exit(1); }
#define ASSERT_GE(a,b) if ((a) < (b)) { printf("ASSERT_GE(" #a ", " #b ") FAILED in %s::%s()::%d.\n", __FILE__, __FUNCTION__, __LINE__); exit(1); }
#define ASSERT_GT(a,b) if ((a) <= (b)) { printf("ASSERT_GT(" #a ", " #b ") FAILED in %s::%s()::%d.\n", __FILE__, __FUNCTION__, __LINE__); exit(1); }

// Must be a multiple of alignment, e.g. 16.
#define BUFFSIZE 2048

// Local datastructure for fixed memory allocation
uint8_t buffer[BUFFSIZE];

// If head == tail, then we are empty.
uint32_t head;
uint32_t tail;

// Some details on our list
#define HDR_FREE 0     // This block is free
#define HDR_INUSE 1    // This block is in use
#define HDR_GAP 2      // This is a gap block. See free pointer for next element

// Must be 16 bytes or less in size, which is also our alignment.
struct hdr {
    uint8_t free;
    uint32_t len;
};

uint32_t avail()
{
    return head >= tail ?
        BUFFSIZE - head + tail :
        tail - head;
}

void circallocblock(uint32_t size, uint8_t hdr_free)
{
    if (size == 0) return;
    struct hdr *meta;
    meta = (struct hdr *)(buffer + head);
    meta->free = hdr_free;
    meta->len = size;
    head = (head + size) % BUFFSIZE;
}

void *circalloc(uint32_t size)
{
    int offset = head;
    int rem = 0;

    // Ensure additional memory for our header, which is always at the
    // beginning. The total size is aligned to 16 bytes. That means every time
    // we allocate, the start of the buffer is always aligned to 16 bytes.
    int block_size = (size + sizeof(struct hdr) + 0xF) & ~0xF;

    // Take into account that we might want to wrap. So if the head > tail, and
    // we allocate more than what there is at the end, we need to ignore the end
    // by allocating an extra chunk.
    if (head >= tail && (BUFFSIZE - head < block_size)) {
        rem = BUFFSIZE - head;     // We know this is already aligned
        offset = 0;
    }

    // Not enough memory. Note the equals, so that head == tail is empty is
    // preserved.
    if (avail() <= block_size + rem) return NULL;

    // Doing this lockless is not yet considered. `circalloc` and `circfree` may
    // all be called simultaneously, so the `meta`, `head, `tail` must all be
    // set atomically.
    circallocblock(rem, HDR_GAP);
    circallocblock(block_size, HDR_INUSE);

    return buffer + offset + sizeof(struct hdr);
}

void circfree(void* addr)
{
    struct hdr *meta;
    struct hdr *gmeta = NULL;
    meta = (struct hdr *)(addr - sizeof(struct hdr));

    // Mark this block as free. It might not be the tail, and might be somewhere
    // in the middle. If it's the head, we don't allow that memory yet to be
    // reclaimed until the tail catches up.
    meta->free = HDR_FREE;

    // If there is corruption in the structure, this might result in an infinite
    // loop.
    meta = (struct hdr *)(buffer + tail);
    do {
        switch(meta->free) {
        case HDR_INUSE:
            return;
        case HDR_GAP:
            // To know if this is free, we need to find the next element. It is
            // an error to have to HDR_GAP after each other, or no other buffer
            // at all. This is special, because the user will never pass the
            // pointer to this block when freeing (as the user never knows about
            // it).
            gmeta = meta;
            int gtail = (tail + gmeta->len) % BUFFSIZE;
            meta = (struct hdr *)(buffer + gtail);
            break;
        case HDR_FREE:
            if (gmeta) tail = (tail + gmeta->len) % BUFFSIZE;
            tail = (tail + meta->len) % BUFFSIZE;
            gmeta = NULL;
            meta = (struct hdr *)(buffer + tail);
            break;
        }
    } while (head != tail);
}



uint32_t testgetoffset(void *addr)
{
    if (addr == NULL) return -1;
    return (uint32_t)((uint8_t *)addr - buffer);
}

void *testalloc(uint32_t size)
{
    void *p = circalloc(size);
    printf("circalloc(%d); addr(offset)=0x%08x (head=0x%04x; tail=0x%04x)\n", size, testgetoffset(p), head, tail);
    return p;
}

void testfree(void *addr)
{
    circfree(addr);
    printf("circfree(0x%08x); (head=0x%04x; tail=0x%04x)\n", testgetoffset(addr), head, tail);
}

void testreset(const char *testcasename)
{
    printf("\nRESET: %s\n", testcasename);
    head = 0;
    tail = 0;
}

int testgetaligned(uint32_t size)
{
    return (size + 0xF) & ~0xF;
}

// Tested to pass on 64-bit. 32-bit might change the sizes, so assumptions of
// the test cases might fail. e.g.
//
// Metadata Size = 0x0008  `sizeof(struct hdr)`.
int main(void)
{
    void *p1, *p2, *p3, *p4;
    int msize = sizeof(struct hdr);
    printf("Metadata Size = 0x%04d\n\n", msize);
    ASSERT_LE(msize, 16);          // The structure must be less than the alignment we chose

    // TEST 1: Allocate and free in order
    testreset("Allocate and free in order");
    p1 = testalloc(10);
    ASSERT_EQ(tail, 0);
    ASSERT_EQ(head, 0x20);         // aligned(10 + 8) = 0x20.
    p2 = testalloc(8);
    ASSERT_EQ(tail, 0);
    ASSERT_EQ(head, 0x30);         // 0x20 + aligned(8 + 8);
    p3 = testalloc(1001);
    ASSERT_EQ(tail, 0);
    ASSERT_EQ(head, 0x430);        // 0x30 + aligned(1001 + 8) = 0z430
    testfree(p1);                  // Now free the tail, it should immediately increment the tail
    ASSERT_EQ(tail, 0x20);
    ASSERT_EQ(head, 0x430);
    testfree(p2);
    ASSERT_EQ(tail, 0x30);
    ASSERT_EQ(head, 0x430);
    testfree(p3);
    ASSERT_EQ(tail, 0x430);
    ASSERT_EQ(head, 0x430);


    // TEST 2: Allocate and free out of order (but not the last)
    testreset("Allocate and then free out of order");
    p1 = testalloc(10);
    ASSERT_EQ(tail, 0);
    ASSERT_EQ(head, 0x20);         // aligned(10 + 8) = 0x20.
    p2 = testalloc(8);
    ASSERT_EQ(tail, 0);
    ASSERT_EQ(head, 0x30);         // 0x20 + aligned(8 + 8);
    p3 = testalloc(1001);
    ASSERT_EQ(tail, 0);
    ASSERT_EQ(head, 0x430);        // 0x30 + aligned(1001 + 8) = 0z430
    testfree(p2);
    ASSERT_EQ(tail, 0x00);         // The tail wasn't freed, so it looks allocated
    ASSERT_EQ(head, 0x430);
    testfree(p1);
    ASSERT_EQ(tail, 0x30);
    ASSERT_EQ(head, 0x430);
    testfree(p3);
    ASSERT_EQ(tail, 0x430);
    ASSERT_EQ(head, 0x430);

    // TEST 3: Allocate and free out of order (the last entry first)
    testreset("Allocate and then free out of order, the head first");
    p1 = testalloc(10);
    ASSERT_EQ(tail, 0);
    ASSERT_EQ(head, 0x20);         // aligned(10 + 8) = 0x20.
    p2 = testalloc(8);
    ASSERT_EQ(tail, 0);
    ASSERT_EQ(head, 0x30);         // 0x20 + aligned(8 + 8);
    p3 = testalloc(1001);
    ASSERT_EQ(tail, 0);
    ASSERT_EQ(head, 0x430);        // 0x30 + aligned(1001 + 8) = 0z430
    testfree(p3);
    ASSERT_EQ(tail, 0x0);          // The tail wasn't freed, so it looks allocated
    ASSERT_EQ(head, 0x430);
    testfree(p2);                  // The tail still isn't freed
    ASSERT_EQ(tail, 0x0);
    ASSERT_EQ(head, 0x430);
    testfree(p1);
    ASSERT_EQ(tail, 0x430);
    ASSERT_EQ(head, 0x430);

    // TEST 4: Allocate so we precisely reach the end
    testreset("Allocate to precisely reach the end");
    head = BUFFSIZE - 48;
    tail = head;
    p1 = testalloc(30);
    ASSERT_EQ(tail, BUFFSIZE - 48);
    ASSERT_EQ(head, 0);            // the head should have wrapped around
    p2 = testalloc(20);
    ASSERT_EQ(tail, BUFFSIZE - 48);
    ASSERT_EQ(head, 0x20);
    testfree(p1);
    ASSERT_EQ(tail, 0);
    ASSERT_EQ(head, 0x20);
    testfree(p2);
    ASSERT_EQ(tail, 0x20);
    ASSERT_EQ(head, 0x20);

    // TEST 5: Allocate so we have to wrap around
    testreset("Allocate near the end");
    head = BUFFSIZE - 48;
    tail = head;
    p1 = testalloc(1000);
    ASSERT_EQ(tail, BUFFSIZE - 48);
    ASSERT_EQ(head, 0x3F0);        // Metadata is at 0x7D0, pointer is at the buffer
    ASSERT_EQ(p1, buffer + msize); // And because we can't allocate 1000 bytes here, it must move forward to `buffer`
    p2 = testalloc(20);
    ASSERT_EQ(tail, BUFFSIZE - 48);
    ASSERT_EQ(head, 0x410);
    testfree(p1);
    ASSERT_EQ(tail, 0x3F0);
    ASSERT_EQ(head, 0x410);
    testfree(p2);
    ASSERT_EQ(tail, 0x410);
    ASSERT_EQ(head, 0x410);

    // TEST 6: Allocate the maximum amount possible, such that we also need to
    // wrap.
    testreset("Allocating all memory starting in the middle");
    head = 512;
    tail = 512;
    p1 = testalloc(1500);
    ASSERT_EQ(tail, 0x200);
    ASSERT_EQ(head, 0x7F0);        // 1500 + 8, rounded is 0x5F0.
    p2 = testalloc(250);
    ASSERT_EQ(tail, 0x200);
    ASSERT_EQ(head, 0x110);        // Had to wrap around. Pad with 16 bytes, then alloc 0x110.
    p3 = testalloc(120);
    ASSERT_EQ(tail, 0x200);
    ASSERT_EQ(head, 0x190);        // 120 + 128 bytes.
    p4 = testalloc(121);
    ASSERT_EQ(p4, NULL);
    ASSERT_EQ(tail, 0x200);
    ASSERT_EQ(head, 0x190);        // Nothing changed
    p4 = testalloc(104);           // 104 + 8 = 112, which is exactly how much is remaining
    ASSERT_EQ(p4, NULL);           // And fails because head cannot equal tail, unless empty.
    ASSERT_EQ(tail, 0x200);
    ASSERT_EQ(head, 0x190);        // Nothing changed
    p4 = testalloc(88);            // 88 + 8 = 96, which now should work.
    ASSERT_EQ(tail, 0x200);
    ASSERT_EQ(head, 0x1F0);        // We're now full.
    testfree(p1);
    ASSERT_EQ(tail, 0x7F0);
    ASSERT_EQ(head, 0x1F0);
    testfree(p3);
    ASSERT_EQ(tail, 0x7F0);        // Didn't free at the tail, so no change
    ASSERT_EQ(head, 0x1F0);
    testfree(p2);
    ASSERT_EQ(tail, 0x190);        // Now frees p2, p3
    ASSERT_EQ(head, 0x1F0);
    testfree(p4);
    ASSERT_EQ(tail, 0x1F0);
    ASSERT_EQ(head, 0x1F0);

    return 0;
}
