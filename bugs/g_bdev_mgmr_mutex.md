More work is needed on:

f1577e74b4 g_bdev_mgr.mutex needs to be recursive

There's probably a static initializer.  Maybe there's a error check wrapper in spdk already.
