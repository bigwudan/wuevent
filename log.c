#include <stdio.h>
#include <stdlib.h>

#include "log.h"



void 
event_err(int eval, const char *fmt, ...)
{

    printf("error \n");
    exit(1);

}

void 
event_errx(int eval, const char *fmt, ...)
{

    printf("error \n");
    exit(1);

}

void 
event_msgx(const char *fmt, ...)
{
    printf("msgx \n");


}

void 
event_warn(const char *fmt, ...)
{
    printf("warn \n");

}
