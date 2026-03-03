CS 161 Problem Set 2 Answers
============================

Leave your name out of this file. Put collaboration notes and credit in
`pset2collab.md`.

**Brief, clear answers preferred!**


C. Parent processes: Per-process metadata design
------------------------------------------------
```{C++}
pid_t parent_id_;
list_links child_links_;
list<proc, &proc::child_links_> children;
```
Each `proc` has a `parent_id_` member and an unordered linked list of its children (as pointers to their structs).

There are also `blocked_wq` and `child_exited_` members for dealing with `E_INTR`.


C. Parent processes: Synchronization plan
-----------------------------------------
There are three `spinlock`s, `ptable_lock` and `phierarchy_lock`.

- `ptable_lock` must be held whenever modifying or indexing `ptable`. It also protects the `pagetable_` field for the purposes of `memusage::refresh`.
- `phierarchy_lock` must be held whenever modifying or accessing any `proc`'s `parent_id_` or `children` members.
- `sleep_lock` must be held whenever modifying `blocked_wq_` or `child_exited_` on any `proc`.

In practice, the first two are often held at the same time. If the kernel wants to hold both at the same time, it **MUST** obtain `phierarchy_lock` **FIRST**. (`sleep_lock` should be obtained last of the three.)


D. Wait and exit status: Synchronization plan
---------------------------------------------
Exit has a first phase where it reparents children and holds the hierarchy lock (also the ptable lock temporarily to reparent to init). It has a second phase where it stores its page table locally and then nulls it while holding `ptable_lock` (to protect `memusage::refresh`). It cleans the page table in its own stack without a lock. In its final phase, it takes both locks, notifies its parent (if sleeping), and sets its exit status and new pstate.

Waitpid holds the hierarchy lock in order to index children, and also the ptable lock when it needs to index the ptable directly (e.g. `pid != 0`).

Since the child hierarchy, ptable, and proc statuses do not need to be treated as one atomic operation, any interleavings are acceptable.

Other notes
-----------


Grading notes
-------------
