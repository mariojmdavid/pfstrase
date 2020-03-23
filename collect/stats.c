#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <syslog.h>
#include "stats.h"

json_object *host_map;
json_object *nid_map;
json_object *server_tag_map;
json_object *server_tag_sum;

__attribute__((constructor))
static void map_init(void) {
  host_map = json_object_new_object();
  nid_map = json_object_new_object();
  server_tag_map = json_object_new_object();
  server_tag_sum = json_object_new_object();
}
__attribute__((destructor))
static void map_kill(void) {
  json_object_put(host_map);
  json_object_put(nid_map);
  json_object_put(server_tag_map);
  json_object_put(server_tag_sum);
}

#define printf_json(json) printf(">>> %s\n", json_object_get_string(json));

static void print_exports_map() {
  int arraylen;
  int j;
  json_object *obd_tag;
  json_object *client, *jid, *uid, *stats_type, *target;
  json_object *data_array, *data_json, *stats;
  printf("---------------Exports map--------------------\n");
  printf("%20s : %10s %10s %10s %10s %20s\n", "server", "client", "jobid", "user", "stats_type", "target");
  json_object_object_foreach(host_map, key, host_entry) {    
    if (json_object_object_get_ex(host_map, "obdclass", &obd_tag))
      if ((strcmp("mds", json_object_get_string(obd_tag)) != 0) &&	\
	  (strcmp("oss", json_object_get_string(obd_tag)) != 0))
	continue;
    if (!json_object_object_get_ex(host_entry, "data", &data_array))
      continue;

    arraylen = json_object_array_length(data_array);
    for (j = 0; j < arraylen; j++) {
      data_json = json_object_array_get_idx(data_array, j);
      if ((!json_object_object_get_ex(data_json, "client", &client)) ||
	  (!json_object_object_get_ex(data_json, "jid", &jid)) ||	   
	  (!json_object_object_get_ex(data_json, "uid", &uid)))
	continue;

      json_object_object_get_ex(data_json, "stats_type", &stats_type);
      json_object_object_get_ex(data_json, "target", &target);
      json_object_object_get_ex(data_json, "stats", &stats);
      printf("%20s : %10s %10s %10s %10s %20s %s\n", key, json_object_get_string(client), 
	     json_object_get_string(jid), json_object_get_string(uid), 
	     json_object_get_string(stats_type), json_object_get_string(target),json_object_get_string(stats));
    }
  }
}

static void print_nid_map() {
  printf("--------------nids map------------------------\n");
  printf("%20s : %10s %10s %10s\n", "nid", "host", "jid", "uid");
  json_object *hid, *jid, *uid;
  json_object_object_foreach(nid_map, key, val) {
    if ((json_object_object_get_ex(val, "hid", &hid)) && (json_object_object_get_ex(val, "jid", &jid)) && (json_object_object_get_ex(val, "uid", &uid)))
      printf("%20s : %10s %10s %10s\n", key, json_object_get_string(hid), 
	     json_object_get_string(jid), json_object_get_string(uid)); 
  }
}

void print_server_tag_sum(const char *tag) {  
  printf("---------------server %s map--------------------\n", tag);
  printf("%10s : %10s %16s %16s[MB]\n", "server", tag, "iops", "bytes");
  json_object_object_foreach(server_tag_sum, servername, server_entry) {    
    json_object_object_foreach(server_entry, clientname, client_entry) {    
      json_object *iops, *bytes;
      if (json_object_object_get_ex(client_entry, "iops", &iops) && \
	  json_object_object_get_ex(client_entry, "bytes", &bytes)) 
	printf("%10s : %10s %16lu %16.1f\n", servername, clientname, json_object_get_int64(iops), 
	       ((double)json_object_get_int64(bytes))/(1024*1024));
    }
  }
}

void print_server_tag_map(const char *tag) {  
  printf("---------------server %s map--------------------\n", tag);
  printf("%10s : %10s %20s\n", "server", tag, "data");
  json_object_object_foreach(server_tag_map, servername, server_entry) {    
    json_object_object_foreach(server_entry, tagname, tag_entry) {    
      printf("%10s : %10s %20s\n", servername, tagname, json_object_get_string(tag_entry));
    }
  }
}

/* Aggregate all events along current tag */
static void aggregate_server_tag_events() {
  json_object_object_foreach(server_tag_map, servername, server_entry) {    
    json_object *client_sum_entry = json_object_new_object();
    json_object_object_foreach(server_entry, clientname, client_entry) {    
      long long sum_reqs = 0;
      long long sum_bytes = 0;
      json_object_object_foreach(client_entry, eventname, value) {    
	if (strcmp(eventname, "read_bytes") == 0 || strcmp(eventname, "write_bytes") == 0) 
	  sum_bytes += json_object_get_int64(value);
	else
	  sum_reqs += json_object_get_int64(value);
      }
      json_object *sum_json = json_object_new_object();
      json_object_object_add(sum_json, "iops", json_object_new_int64(sum_reqs));
      json_object_object_add(sum_json, "bytes", json_object_new_int64(sum_bytes));
      json_object_object_add(client_sum_entry, clientname, sum_json); 
    }
    json_object_object_add(server_tag_sum, servername, client_sum_entry);
  }
}

/* Aggregate (group) stats by given tag (client/jid/uid are most likely) */
void group_statsbytag(const char *tag_str) {
  int arraylen;
  int j;
  json_object *data_array, *data_entry;
  json_object *tag_map, *tag_stats;
  json_object *stats_type, *stats_json;
  json_object *tag;
  json_object *oldval;
  json_object_object_foreach(host_map, key, host_entry) {    
    if (json_object_object_get_ex(host_entry, "obdclass", &tag)) {
      if ((strcmp("mds", json_object_get_string(tag)) != 0) &&	\
	  (strcmp("oss", json_object_get_string(tag)) != 0))
	continue;
    }
    else
      continue;

    if (!json_object_object_get_ex(host_entry, "data", &data_array))
      continue;

    tag_map = json_object_new_object();

    arraylen = json_object_array_length(data_array);
    for (j = 0; j < arraylen; j++) {
      data_entry = json_object_array_get_idx(data_array, j);
      if (json_object_object_get_ex(data_entry, "stats_type", &stats_type)) {
	if ((strcmp("mds", json_object_get_string(stats_type)) != 0) &&	\
	    (strcmp("oss", json_object_get_string(stats_type)) != 0))
	  continue;
      }
      else 
	continue;

      if (!json_object_object_get_ex(data_entry, tag_str, &tag))
	continue;     
      if (!json_object_object_get_ex(data_entry, "stats", &stats_json))
	continue;

      if (!json_object_object_get_ex(tag_map, json_object_get_string(tag), &tag_stats)) {
	tag_stats = json_object_new_object();
	json_object_object_add(tag_map, json_object_get_string(tag), tag_stats);
      }
      /* Add stats values for all devices with same client/jid/uid */
      json_object_object_foreach(stats_json, event, newval) {
	  if (json_object_object_get_ex(tag_stats, event, &oldval))
	    json_object_object_add(tag_stats, event,  
				   json_object_new_int64(json_object_get_int64(oldval) + \
							 json_object_get_int64(newval)));
	  else 
	    json_object_object_add(tag_stats, event, json_object_get(newval));  
      }
      json_object_object_add(tag_map, json_object_get_string(tag), json_object_get(tag_stats));
      
    }
    json_object_object_add(server_tag_map, key, json_object_get(tag_map));   
    json_object_put(tag_map);
  }  
  aggregate_server_tag_events();
}

/* Tag servers exports with client names, jids, uids, and filesystem */
static void tag_stats() {
  int arraylen;
  int j;
  json_object *data_array, *data_entry, *nid_entry;
  json_object *tag;

  json_object_object_foreach(host_map, key, host_entry) {    
    if (json_object_object_get_ex(host_entry, "obdclass", &tag)) {
      if ((strcmp("mds", json_object_get_string(tag)) != 0) &&	\
	  (strcmp("oss", json_object_get_string(tag)) != 0))
	continue;
    }
    else 
      continue;

    if (!json_object_object_get_ex(host_entry, "data", &data_array))
      continue;

    arraylen = json_object_array_length(data_array);
    for (j = 0; j < arraylen; j++) {
      data_entry = json_object_array_get_idx(data_array, j);
      if ((json_object_object_get_ex(data_entry, "client_nid", &tag)) && \
	  (json_object_object_get_ex(nid_map, json_object_get_string(tag), &nid_entry))) {
	if (json_object_object_get_ex(nid_entry, "hid", &tag))
	  json_object_object_add(data_entry, "client", json_object_get(tag));
	if (json_object_object_get_ex(nid_entry, "jid", &tag))
	  json_object_object_add(data_entry, "jid", json_object_get(tag));
	if (json_object_object_get_ex(nid_entry, "uid", &tag))
	  json_object_object_add(data_entry, "uid", json_object_get(tag));
	/* Tag with filesystem name */
	if (json_object_object_get_ex(data_entry, "target", &tag)) {
	  char target[json_object_get_string_len(tag)];
	  snprintf(target, sizeof(target), "%s", json_object_get_string(tag));  
	  char *p = &target[0];
	  char *filesystem = strsep(&p, "-");
	  json_object_object_add(data_entry, "fid", json_object_new_string(filesystem));
	  
	}
      }
    }
  }
}

int update_host_map(char *rpc) {

  int rc = -1;
  json_object *rpc_json = NULL;
  char hostname[32];

  enum json_tokener_error error = json_tokener_success;
  rpc_json = json_tokener_parse_verbose(rpc, &error);  
  if (error != json_tokener_success) {
    fprintf(stderr, "RPC `%s': %s\n", rpc, json_tokener_error_desc(error));
    goto out;
  }

  /* If hostname is not in the rpc it is not valid so return */
  json_object *hostname_tag;
  if (json_object_object_get_ex(rpc_json, "hostname", &hostname_tag)) {
    snprintf(hostname, sizeof(hostname), "%s", json_object_get_string(hostname_tag));  
    /* init host_entry of hostmap if does not exist */
    json_object *hostname_entry;
    if (!json_object_object_get_ex(host_map, hostname, &hostname_entry)) {
      json_object *entry_json = json_object_new_object();
      json_object_object_add(entry_json, "jid", json_object_new_string("-"));
      json_object_object_add(entry_json, "uid", json_object_new_string("-"));
      json_object_object_add(host_map, hostname, entry_json);
    }
  }
  else {
    fprintf(stderr, "%s\n", "RPC does not contain a hostname to update server data");
    goto out;
  }

  /* update hostname_entry if tags or data are present in rpc */
  json_object *entry_json, *tag;
  if (json_object_object_get_ex(host_map, hostname, &entry_json)) {

    if (json_object_object_get_ex(rpc_json, "hostname", &tag))
      json_object_object_add(entry_json, "hid", json_object_get(tag));

    if (json_object_object_get_ex(rpc_json, "jid", &tag))
      json_object_object_add(entry_json, "jid", json_object_get(tag));

    if (json_object_object_get_ex(rpc_json, "uid", &tag))
      json_object_object_add(entry_json, "uid", json_object_get(tag));

    if (json_object_object_get_ex(rpc_json, "obdclass", &tag))
      json_object_object_add(entry_json, "obdclass", json_object_get(tag));

    if (json_object_object_get_ex(rpc_json, "data", &tag))
      json_object_object_add(entry_json, "data", json_object_get(tag));

    if (json_object_object_get_ex(rpc_json, "nid", &tag))
      json_object_object_add(nid_map, json_object_get_string(tag), json_object_get(entry_json));

    tag_stats();
  }
  rc = 1;

  group_statsbytag("client");
  print_server_tag_map("client");
  print_server_tag_sum("client");
  group_statsbytag("jid");
  print_server_tag_map("jid");
  print_server_tag_sum("jid");
  group_statsbytag("uid");
  print_server_tag_map("uid");
  print_server_tag_sum("uid");
  group_statsbytag("fid");
  print_server_tag_map("fid");
  print_server_tag_sum("fid");

 out:
  if (rpc_json)
    json_object_put(rpc_json);
}

void map_destroy() {
  if (host_map)
    json_object_put(host_map);
  if (nid_map)
    json_object_put(nid_map);
}
