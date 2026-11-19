/* Slot-accounting proof for kbase_kmod_submit_job (0004).
 *
 * Model: atom batch = [wait?] + job + trigger; each atom consumes one
 * table slot via slots[idx++].  Assert: allocated == consumed.
 *
 * Old code: n_slots = 1 (+1 iff in-fence).  New: n_slots = 2 (+1).
 * Cases: no-fence (job+trigger = 2 atoms), in-fence (wait+job+trigger = 3).
 */
#include <stdio.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
   if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
   else { printf("ok: %s\n", msg); } } while (0)

static unsigned n_slots_old(int in_fence) { return 1 + (in_fence ? 1 : 0); }
static unsigned n_slots_new(int in_fence) { return 2 + (in_fence ? 1 : 0); }
static unsigned n_atoms_used(int in_fence) { return (in_fence ? 3u : 2u); }

int
main(void)
{
   /* Allocation must cover consumption in both fence cases. */
   CHECK(n_slots_new(0) == n_atoms_used(0), "no-fence: alloc 2 == use 2 (job+trigger)");
   CHECK(n_slots_new(1) == n_atoms_used(1), "in-fence: alloc 3 == use 3 (wait+job+trigger)");
   /* The old count under-allocates in both cases (the off-by-one). */
   CHECK(n_slots_old(0) < n_atoms_used(0), "old no-fence under-allocates (1 < 2)");
   CHECK(n_slots_old(1) < n_atoms_used(1), "old in-fence under-allocates (2 < 3)");
   /* With the old count the job_slot index itself is out of bounds when
    * no in-fence is present (slots[1] from a 1-entry reservation). */
   CHECK(n_slots_old(0) <= 1, "old no-fence reservation holds 1 entry (job_slot OOB)");
   printf("%s\n", fails ? "FAILED" : "slot-proof OK");
   return fails ? 1 : 0;
}
