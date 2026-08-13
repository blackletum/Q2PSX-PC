#include "version.h"

#include <stdio.h>

const char *q2_version_string(void)
{
    /*
     * The dirty flag is part of the identity, not a footnote. A bug report from
     * a modified working tree needs to say so up front, because the commit hash
     * alone would be misleading.
     *
     * Two literals rather than one built at runtime: this is called from crash
     * paths where allocating or formatting is exactly what you do not want.
     */
#if Q2PSX_VERSION_DIRTY
    return Q2PSX_VERSION_GIT " (modified)";
#else
    return Q2PSX_VERSION_GIT;
#endif
}

void q2_version_print(void)
{
    printf("Q2PSX-PC %s\n", Q2PSX_VERSION_STRING);
    printf("  build    : %s\n", Q2PSX_VERSION_GIT);
    if (Q2PSX_VERSION_DIRTY)
        printf("             built from a MODIFIED working tree\n");
    printf("  type     : %s\n", Q2PSX_BUILD_TYPE);
    printf("  compiler : %s\n", Q2PSX_COMPILER);
    printf("  platform : %s\n", Q2PSX_PLATFORM);
}
