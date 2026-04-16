typedef void (*cxx_init_fn_t)(void);

extern cxx_init_fn_t __init_array_start[];
extern cxx_init_fn_t __init_array_end[];
extern cxx_init_fn_t __ctors_start[];
extern cxx_init_fn_t __ctors_end[];
extern cxx_init_fn_t __fini_array_start[];
extern cxx_init_fn_t __fini_array_end[];
extern cxx_init_fn_t __dtors_start[];
extern cxx_init_fn_t __dtors_end[];

static void run_forward(cxx_init_fn_t *start, cxx_init_fn_t *end)
{
    for (cxx_init_fn_t *fn = start; fn < end; ++fn)
        (*fn)();
}

static void run_reverse(cxx_init_fn_t *start, cxx_init_fn_t *end)
{
    while (end > start)
        (*--end)();
}

void __drunix_run_constructors(void)
{
    run_forward(__init_array_start, __init_array_end);
    run_forward(__ctors_start, __ctors_end);
}

void __drunix_run_destructors(void)
{
    run_reverse(__fini_array_start, __fini_array_end);
    run_reverse(__dtors_start, __dtors_end);
}
