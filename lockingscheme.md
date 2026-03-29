
- For files: order of lock attainment is `fd_table_lock -> file_table_lock -> file_lock -> vnode_lock / pipe_lock`
- With initfs: acquisition order is `... file lock -> (vnode lock ->) initfs -> specific memfile lock`
  - Exception is in `init_memfile_entry`, where it doesn't make sense to make a vnode and obtain its lock before first checking for the memfile entry
