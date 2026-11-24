#include<stdio.h>
char * match(char *s, char ch)
{
    char*p=s;
    while(*p!='\0')
        {
            p++;
        }
    while(p>s)
        {
        if(*p==ch)
            return p;
        p--;
        }
    return NULL;
}