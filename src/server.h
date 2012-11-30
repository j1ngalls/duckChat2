#ifndef SERVER_H
#define SERVER_H

#include <string>

#ifdef DEBUG
#define DBG(fmt, ...)                                                    \
    do{                                                             \
    printf("--[DEBUG]-- %s:%d  %s  ",__FILE__ ,__LINE__ ,__func__); \
    printf(fmt, __VA_ARGS__);                                             \
    }while(0);                                                      
#else
#define DBG(...)    \
    do{}while(0);

#endif/*DEBUG*/

#define TIMEOUT_CLIENT_USAGE 120
//TODO double check below defines against original
#define HOSTNAME_MAX 256
#define MAX_MESSAGE_LEN 1024 

void handle_socket_input();
void handle_login_message(void *data, struct sockaddr_in sock);
void handle_logout_message(struct sockaddr_in sock);
void handle_join_message(void *data, struct sockaddr_in sock);
void handle_leave_message(void *data, struct sockaddr_in sock);
void handle_say_message(void *data, struct sockaddr_in sock);
void handle_list_message(struct sockaddr_in sock);
void handle_who_message(void *data, struct sockaddr_in sock);
void handle_keep_alive_message(struct sockaddr_in sock);
void handle_s_join(void *data, struct sockaddr_in sock);
void handle_s_leave(void *data, struct sockaddr_in sock);
void handle_s_say(void *data, struct sockaddr_in sock);
void send_error_message(struct sockaddr_in sock, std::string error_msg);

#endif
