#ifndef _STATS_H_
#define _STATS_H_
#include <json-c12/json.h>

int update_host_map(char *rpc);
void group_statsbytag(const char *tag);
void print_server_tag_sum(const char *tag);
void print_server_tag_map(const char *tag);

#endif
