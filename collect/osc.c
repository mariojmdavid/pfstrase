#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include "collect.h"
#include "osc.h"

#define OSSPATH "/sys/fs/lustre/osc"
//#define TYPEPATH "/proc/fs/lustre/osc"
//#define TYPEPATH "/sys/kernel/debug/lustre/osc"

#define STATS		 \
    X(stats)

int collect_osc(struct device_info *info, char **buffer)
{
  int rc = -1;

  char *typepath = "/sys/kernel/debug/lustre/osc";

  DIR *typedir = NULL;
  typedir = opendir(typepath);
  if(typedir == NULL) {
    fprintf(stderr, "cannot open `%s' : %m\n", typepath);
    goto typedir_err;
  }

  char *tmp = *buffer;
  asprintf(buffer, "%s{\"type\": \"osc\", \"osts\": [", *buffer);       
  if (tmp != NULL) free(tmp);

  struct dirent *typede;
  while ((typede = readdir(typedir)) != NULL) {  
    if (typede->d_type != DT_DIR || typede->d_name[0] == '.')
      continue;

    char osspath[256];
    snprintf(osspath, sizeof(osspath), "%s/%s/ost_conn_uuid", OSSPATH, typede->d_name);    

    char devpath[256];
    snprintf(devpath, sizeof(devpath), "%s/%s", typepath, typede->d_name);    

    if (strlen(typede->d_name) < 16) {
      fprintf(stderr, "invalid obd name `%s'\n", typede->d_name);
      continue;
    }
    char *p = typede->d_name + strlen(typede->d_name) - 20 - 1;
    *p = '\0'; 
    
    tmp = *buffer;
    asprintf(buffer, "%s{\"idx\": \"%s\"", *buffer, typede->d_name);
    if (tmp != NULL) free(tmp);

    if (collect_string(osspath, buffer, "oss") < 0)			       
      fprintf(stderr, "cannot read `%s' from `%s': %m\n", "ost_conn_uuid", osspath);
     
#define X(k,r...)							\
    ({									\
      char filepath[256];						\
      snprintf(filepath, sizeof(filepath), "%s/%s", devpath, #k);	\
      if (collect_stats(filepath, buffer) < 0)				\
	fprintf(stderr, "cannot read `%s' from `%s': %m\n", #k, filepath); \
    })
    STATS;
#undef X
    
    tmp = *buffer;
    asprintf(buffer, "%s},", *buffer);
    if (tmp != NULL) free(tmp);    
  }

  char *p = *buffer;
  p = *buffer + strlen(*buffer) - 1;
  *p = ']';

  rc = 0;
 typedir_err:
  if (typedir != NULL)
    closedir(typedir);
  
  return rc;
}