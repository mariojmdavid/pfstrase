#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <amqp.h>
#include <amqp_tcp_socket.h>

#include "utils.h"
#include "collect.h"
#include "lfs_utils.h"
#include "socket_utils.h"

char const *exchange = "amq.direct";
amqp_basic_properties_t props; 
amqp_connection_state_t conn;
static char *port;
static char *hostname;

int sockfd;

// Setup connection to RabbitMQ Server
int amqp_setup_connection(char *iport, char *ihostname)
{
  amqp_socket_t *socket = NULL;
  amqp_bytes_t request_queue;
  amqp_bytes_t response_queue;
  int status = -1;

  port = iport;
  hostname = ihostname;

  props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
  props.content_type = amqp_cstring_bytes("text/plain");
  props.delivery_mode = 2; /* persistent delivery mode */

  fprintf(stderr, "Connection to RMQ server %s:%s Attempting\n", hostname, port);

  conn = amqp_new_connection();

  socket = amqp_tcp_socket_new(conn);
  if (!socket) {
    fprintf(stderr, "Connection to RMQ server %s:%s Failed\n", hostname, port);
    return -1;
  }

  status = amqp_socket_open(socket, hostname, atoi(port));
  if (status) {
    fprintf(stderr, "Connection to RMQ server %s:%s Failed\n", hostname, port);
    return -1;
  }

  die_on_amqp_error(amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN,
			       "guest", "guest"), "Logging in");
  amqp_channel_open(conn, 1);
  die_on_amqp_error(amqp_get_rpc_reply(conn), "Opening channel");
  
  {
    amqp_queue_declare_ok_t *r = amqp_queue_declare(conn, 1, amqp_cstring_bytes(get_dev_data()->hostname), 
						    0, 0, 1, 1, amqp_empty_table);
    die_on_amqp_error(amqp_get_rpc_reply(conn), "Declaring request queue");
    request_queue = amqp_bytes_malloc_dup(r->queue);
    if (request_queue.bytes == NULL) {
      fprintf(stderr, "Out of memory while copying queue name");
      return -1;
    }
  }
  {
    amqp_queue_declare_ok_t *r = amqp_queue_declare(conn, 1, amqp_cstring_bytes("response"), 
						    0, 1, 0, 0, amqp_empty_table);
    die_on_amqp_error(amqp_get_rpc_reply(conn), "Declaring response queue");
    response_queue = amqp_bytes_malloc_dup(r->queue);
    if (response_queue.bytes == NULL) {
      fprintf(stderr, "Out of memory while copying queue name");
      return -1;
    }
  }
  
  amqp_queue_bind(conn, 1, request_queue, amqp_cstring_bytes(exchange),
                  amqp_cstring_bytes("request"), amqp_empty_table);
  die_on_amqp_error(amqp_get_rpc_reply(conn), "Binding queue");

  amqp_queue_bind(conn, 1, response_queue, amqp_cstring_bytes(exchange),
                  amqp_cstring_bytes("response"), amqp_empty_table);
  die_on_amqp_error(amqp_get_rpc_reply(conn), "Binding queue");

  amqp_basic_consume(conn, 1, request_queue, amqp_empty_bytes, 0, 1, 0,
                     amqp_empty_table);
  die_on_amqp_error(amqp_get_rpc_reply(conn), "Consuming");

  amqp_bytes_free(request_queue);
  amqp_bytes_free(response_queue);
  fprintf(stderr, "Connection to RMQ server %s:%s Established\n", hostname, port);
  return amqp_socket_get_sockfd(socket);
}

void amqp_kill_connection()
{
    die_on_error(amqp_destroy_connection(conn), "Ending connection");
}

// Process RPC
static void process_rpc(char *rpc)
{
  printf("RPC %s\n", rpc);
    
  json_object *rpc_json = json_tokener_parse(rpc);  
  if (rpc_json == NULL) return;

  json_object_object_foreach(rpc_json, key, val) {
    if (strcmp(key, "jid") == 0)
      snprintf(get_dev_data()->jid, sizeof(get_dev_data()->jid), json_object_get_string(val));
    if (strcmp(key, "user") == 0)
      snprintf(get_dev_data()->user, sizeof(get_dev_data()->user), json_object_get_string(val));
  }
}

// Receive and process rpc
void amqp_rpc()
{
  amqp_rpc_reply_t res;
  amqp_envelope_t envelope;

  amqp_maybe_release_buffers(conn);
  res = amqp_consume_message(conn, &envelope, NULL, 0);
  if (AMQP_RESPONSE_NORMAL != res.reply_type) {
    return;
  }
  
  char *p = (char*)(envelope.message.body.bytes + envelope.message.body.len);
  *p = '\0';
  char *rpc = (char *)envelope.message.body.bytes;
  process_rpc(rpc);
  
  amqp_destroy_envelope(&envelope);
  amqp_send_data();
}

// Collect and send data
void amqp_send_data()      
{  
  json_object *message_json = json_object_new_object();
  collect_devices(message_json);
  printf ("The json object created: %s\n",json_object_to_json_string(message_json));
  if (amqp_basic_publish(conn, 1,
			 amqp_cstring_bytes(exchange),
			 amqp_cstring_bytes("response"),
			 0, 0, &props,
			 amqp_cstring_bytes(json_object_to_json_string(message_json))) < 0) {
    amqp_destroy_connection(conn);
    amqp_setup_connection(port, hostname);
  }
  json_object_put(message_json);
}

int sock_setup_connection(const char *port) 
{
  int opt = 1;
  int backlog = 10;
  struct sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);
  int fd = -1;

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(atoi(port));

  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {    
    fprintf(stderr, "cannot initialize socket: %s\n", strerror(errno));
    goto err;
  }
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (char *) &opt, sizeof(opt)) == -1) {
    fprintf(stderr, "cannot set SO_REUSEADDR: %s\n", strerror(errno));
    goto err; 
  }
  if (bind(sockfd, (struct sockaddr *)&addr, addrlen) == -1) {
    fprintf(stderr, "cannot bind: %s\n", strerror(errno));
    close(sockfd);
    goto err;
  }
  if (listen(sockfd, backlog) == -1) {
    fprintf(stderr, "cannot listen: %s\n", strerror(errno));
    close(sockfd);
    goto err;
  }
 err:
  return sockfd;
}

void sock_rpc() 
{  
  struct sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);
  int fd = -1;

  if ((fd = accept(sockfd, (struct sockaddr *)&addr, &addrlen)) < 0)
    if (!(errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED))
      fprintf(stderr, "cannot accept connections: %s\n", strerror(errno));

  char request[SOCKET_BUFFERSIZE];
  memset(request, 0, sizeof(request));
  ssize_t bytes_recvd = recv(fd, request, sizeof(request), 0);
  close(fd);

  if (bytes_recvd < 0) {
    fprintf(stderr, "cannot recv: %s\n", strerror(errno));
    return;
  }

  char *p = request + strlen(request) - 1;
  *p = '\0';
  process_rpc(request);
  amqp_send_data();
}

void sock_send_data()
{
  json_object *message_json = json_object_new_object();
  collect_devices(message_json);
  printf("The json object created: %s\n",json_object_to_json_string(message_json));
  int rv = send(sockfd, json_object_to_json_string(message_json), strlen(json_object_to_json_string(message_json)), 0);
  json_object_put(message_json);
}
