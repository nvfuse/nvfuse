//#include "include/nvfuse_core.h"

#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#ifdef __linux__
#	include <sys/types.h>
#	include <sys/stat.h>
#endif
#include <fcntl.h>
#include "nvfuse_core.h"
#include "nvfuse_config.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_api.h"
#include "imark.h"



int main()
{

    nvfuse_mount(NULL,0);
    return 0;        
}
