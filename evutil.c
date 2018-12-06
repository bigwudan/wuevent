#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *
evutil_getenv(const char *varname)
{
    return getenv(varname);
}

