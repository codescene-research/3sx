/* Turn durability syncs into no-ops for throwaway replay traces. Data still
 * reaches the page cache through write(), so the traces are unchanged; only the
 * per-frame wait for the disk journal goes away. */
#include <stdio.h>
int fsync(int fd) { (void)fd; return 0; }
int fdatasync(int fd) { (void)fd; return 0; }
int syncfs(int fd) { (void)fd; return 0; }
void sync(void) {}
