CS 161 Problem Set 3 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset3collab.md`.

Answers to written questions
----------------------------

## Part B.

### Changes to VFS design:
- Add an additional lock inside of each file (since vnode operations might block, shouldn't force everything to hold file table lock). `file_table_lock` held only for adding/removing files from table and when solely updating ref counts. `file_lock` held when performing some file op.
- `file_ref()`, `file_unref()`, `vnode_ref()`, `vnode_unref()` helpers to maintain refcounts in `fork` and `exit`. 
- `fd_table_lock` member of `proc`, to guard any accesses of `fd_table`. Lock acquisition order is `fd_table_lock -> file_table_lock -> file_lock`. Right now a thread should not hold more than one of any `fd_table_lock`s or `file_lock`s.

Grading notes
-------------
