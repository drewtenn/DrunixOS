#include "lib/stdio.h"
#include "lib/syscall.h"

static int g_ctor_value = 0;

class GlobalProbe {
public:
    GlobalProbe()
    {
        g_ctor_value = 42;
    }

    ~GlobalProbe()
    {
        sys_write("cpphello: global destructor ran\n");
    }
};

static GlobalProbe g_probe;

class Greeter {
public:
    virtual ~Greeter() {}
    virtual const char *message() const { return "base"; }
};

class DrunixGreeter : public Greeter {
public:
    const char *message() const override { return "virtual dispatch works"; }
};

static int sum_array(const int *values, int count)
{
    int sum = 0;
    for (int i = 0; i < count; ++i)
        sum += values[i];
    return sum;
}

int main(int argc, char **argv)
{
    printf("Hello from C++ userland!\n");
    printf("argc=%d\n", argc);
    if (argc > 0 && argv && argv[0])
        printf("argv[0]=%s\n", argv[0]);
    printf("global constructor value=%d\n", g_ctor_value);

    Greeter *greeter = new DrunixGreeter();
    printf("%s\n", greeter->message());
    delete greeter;

    int *values = new int[3];
    values[0] = 1;
    values[1] = 2;
    values[2] = 3;
    int sum = sum_array(values, 3);
    printf("new[] sum=%d\n", sum);
    delete[] values;

    if (g_ctor_value != 42)
        return 1;
    if (sum != 6)
        return 2;
    return 0;
}
