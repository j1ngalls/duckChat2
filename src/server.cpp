#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <time.h>

// C++ includes
#include <string>
#include <map>
#include <iostream>

// Local includes
#include "duckchat.h"

using namespace std;

#define MAX_CONNECTIONS 10
#define HOSTNAME_MAX 100
#define MAX_MESSAGE_LEN 65536

/* Handler function declarations */
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
void send_error_message(struct sockaddr_in sock, string error_msg);

/* The type for our channel map */
typedef map<string, struct sockaddr_in> channel_type; 

int s; //socket for listening
struct sockaddr_in server;
char hostname[HOSTNAME_MAX];
int port;

map<string,struct sockaddr_in> usernames; //<username, sockaddr_in of user>
map<string,int>             active_usernames; //0-inactive , 1-active
map<string,string>          rev_usernames; //<ip+port in string, username>
map<string,channel_type>    channels;

int main(int argc, char *argv[]){

    // check user entered correct commandline arguments
    if (argc < 3){
        printf("Usage: %s domain_name port_num [serverIPs] [serverPort]\n", argv[0]);
        exit(1);
    }


    strcpy(hostname, argv[1]);
    port = atoi(argv[2]);

    // make socket
    s = socket(PF_INET, SOCK_DGRAM, 0);
    if (s < 0){
        perror ("socket() failed\n");
        exit(1);
    }

    struct hostent     *he;

    server.sin_family = AF_INET;
    server.sin_port = htons(port);

    if ((he = gethostbyname(hostname)) == NULL) {
        puts("error resolving hostname..");
        exit(1);
    }
    memcpy(&server.sin_addr, he->h_addr_list[0], he->h_length);

    int err;

    // bind the socket to our addrinfo
    err = bind(s, (struct sockaddr*)&server, sizeof server);
    if (err < 0){
        perror("bind failed\n");
    }

    // create default channel Common
    string default_channel = "Common";
    map<string,struct sockaddr_in> default_channel_users;
    channels[default_channel] = default_channel_users;

    // create struct for 2 minute timout
    struct timeval tv;
    tv.tv_sec = 120;
    tv.tv_usec = 0;

    time_t t1,t2;

    // main event loop to accept client requests
    while(1){

        // use a file descriptor with a timer to handle timeouts
        int rc;
        fd_set fds;

        FD_ZERO(&fds);
        FD_SET(s, &fds);

        time(&t1);

        rc = select(s+1, &fds, NULL, NULL, &tv);
        if (rc < 0)
            printf("error in select\n");
        else{
            int socket_data = 0;

            if (FD_ISSET(s,&fds)){
                // reading from socket
                handle_socket_input();
                socket_data = 1;
            }

            time(&t2);

            int elapsed_time = (int) t2-t1;

            if (socket_data){
                // reduce the timer
                if (elapsed_time <= 120){
                    tv.tv_sec = tv.tv_sec - elapsed_time;
                    tv.tv_usec = 0;
                
                }else{
                    tv.tv_sec = 120;
                    tv.tv_usec = 0;
                }

            }else{
                //reset timer
                tv.tv_sec = 120;
                tv.tv_usec = 0;

                //cout << "timer time out: reseting the timer "<< endl;
                //check whether users are active and remove users if not active
                map<string,int>::iterator active_user_iter;
                for(active_user_iter = active_usernames.begin(); active_user_iter != active_usernames.end(); active_user_iter++){
                    string username = active_user_iter->first;
                    int isActive = active_user_iter->second;

                    if (!(isActive))
                    {
                        //key has to be constructed to remove from rev_usernames

                        cout << "server: forcibly removing user " << username << endl;

                        map<string,struct sockaddr_in>::iterator user_iter;
                        user_iter = usernames.find(username);

                        //key has to be constructed to remove from rev_usernames

                        struct sockaddr_in sock = user_iter->second;
                        string ip = inet_ntoa(sock.sin_addr);

                        int srcport = sock.sin_port;

                        char port_str[6];
                        sprintf(port_str, "%d", srcport);
                        string key = ip + "." +port_str;

                        map <string,string> :: iterator rev_user_iter;

                        rev_user_iter = rev_usernames.find(key);
                        rev_usernames.erase(rev_user_iter);

                        //remove from usernames
                        usernames.erase(user_iter);

                        //remove from all the channels if found
                        map<string,channel_type>::iterator channel_iter;
                        for(channel_iter = channels.begin(); channel_iter != channels.end(); channel_iter++)
                        {
                            //cout << "key: " << iter->first << " username: " << iter->second << endl;
                            //channel_type current_channel = channel_iter->second;
                            map<string,struct sockaddr_in>::iterator within_channel_iterator;
                            within_channel_iterator = channel_iter->second.find(username);
                            if (within_channel_iterator != channel_iter->second.end())
                            {
                                channel_iter->second.erase(within_channel_iterator);
                            }

                        }
                        //erase from active users
                        active_usernames.erase(active_user_iter);
                    }
                }
                //reset the data structure that keep track of active users
                for(active_user_iter = active_usernames.begin(); active_user_iter != active_usernames.end(); active_user_iter++)
                    active_user_iter->second = 0;
            }
        }
    }

    /* should never get here */
    return 0;
}

void handle_socket_input(){
    struct sockaddr_in recv_client;
    ssize_t bytes;
    void *data;
    size_t len;
    socklen_t fromlen;
    fromlen = sizeof(recv_client);
    char recv_text[MAX_MESSAGE_LEN];
    data = &recv_text;
    len = sizeof recv_text;

    bytes = recvfrom(s, data, len, 0, (struct sockaddr*)&recv_client, &fromlen);

    if (bytes < 0)
        perror ("recvfrom failed\n");
    
    else{
        struct request* request_msg;
        request_msg = (struct request*)data;

        request_t message_type = request_msg->req_type;

        if (message_type == REQ_LOGIN){
            handle_login_message(data, recv_client); 
        
        }else if (message_type == REQ_LOGOUT){
            handle_logout_message(recv_client);
        
        }else if (message_type == REQ_JOIN){
            handle_join_message(data, recv_client);
        
        }else if (message_type == REQ_LEAVE){
            handle_leave_message(data, recv_client);
        
        }else if (message_type == REQ_SAY){
            handle_say_message(data, recv_client);
        
        }else if (message_type == REQ_LIST){
            handle_list_message(recv_client);
        
        }else if (message_type == REQ_WHO){
            handle_who_message(data, recv_client);
        
        }else if (message_type == REQ_KEEP_ALIVE){
            handle_keep_alive_message(recv_client);
        
        }else if (message_type == S2S_JOIN){
            handle_s_join(data, recv_client);
        
        }else if (message_type == S2S_LEAVE){
            handle_s_leave(data, recv_client);
        
        }else if (message_type == S2S_SAY){
            handle_s_say(data, recv_client);

        }else
            //send error message to client
            send_error_message(recv_client, "*Unknown command");
    }
}

void handle_login_message(void *data, struct sockaddr_in sock){

    struct request_login* msg;
    msg = (struct request_login*)data;

    string username = msg->req_username;
    usernames[username] = sock;
    active_usernames[username] = 1;

    string ip = inet_ntoa(sock.sin_addr);
    int srcport = sock.sin_port;

    char port_str[6];
    sprintf(port_str, "%d", srcport);

    string key = ip + "." +port_str;
    rev_usernames[key] = username;

    // print debug message
    cout << hostname << ":" << port << ip << ":" << srcport 
        << " recv Request Login (from" << username << ")"  << endl;
}

void handle_logout_message(struct sockaddr_in sock){
    //construct the key using sockaddr_in
    string ip = inet_ntoa(sock.sin_addr);
    int srcport = sock.sin_port;

    char port_str[6];
    sprintf(port_str, "%d", srcport);

    string key = ip + "." +port_str;

    //check whether key is in rev_usernames
    map <string,string> :: iterator iter;

    iter = rev_usernames.find(key);
    if (iter == rev_usernames.end() ){

        //send an error message saying not logged in
        send_error_message(sock, "Not logged in");

    }else{

        string username = rev_usernames[key];
        rev_usernames.erase(iter);

        //remove from usernames
        map<string,struct sockaddr_in>::iterator user_iter;
        user_iter = usernames.find(username);
        usernames.erase(user_iter);

        //remove from all the channels if found
        map<string,channel_type>::iterator channel_iter;

        for(channel_iter = channels.begin(); channel_iter != channels.end(); channel_iter++){
            map<string,struct sockaddr_in>::iterator within_channel_iterator;
            within_channel_iterator = channel_iter->second.find(username);

            if (within_channel_iterator != channel_iter->second.end())
                channel_iter->second.erase(within_channel_iterator);

        }

        //remove entry from active usernames also
        map<string,int>::iterator active_user_iter;
        active_user_iter = active_usernames.find(username);
        active_usernames.erase(active_user_iter);

        // print debug message
        cout << hostname << ":" << port << ip << ":" << srcport 
            << " recv Request Logout (from" << username << ")"  << endl;
    }
}

void handle_join_message(void *data, struct sockaddr_in sock)
{
    //get message fields
    struct request_join* msg;
    msg = (struct request_join*)data;

    string channel = msg->req_channel;

    string ip = inet_ntoa(sock.sin_addr);

    int srcport = sock.sin_port;

    char port_str[6];
    sprintf(port_str, "%d", srcport);
    string key = ip + "." +port_str;

    //check whether key is in rev_usernames
    map <string,string> :: iterator iter;

    iter = rev_usernames.find(key);
    if (iter == rev_usernames.end() ){
        
        //ip+port not recognized - send an error message
        send_error_message(sock, "Not logged in");

    }else{

        string username = rev_usernames[key];

        map<string,channel_type>::iterator channel_iter;

        channel_iter = channels.find(channel);

        active_usernames[username] = 1;

        if (channel_iter == channels.end()){
            //channel not found
            map<string,struct sockaddr_in> new_channel_users;
            new_channel_users[username] = sock;
            channels[channel] = new_channel_users;
        
        }else
            //channel already exits
            channels[channel][username] = sock;
        
        // print debug message 
        cout << hostname << ":" << port << ip << ":" << srcport 
            << " recv Request Join " << channel << " (from" << username << ")"  << endl;
    }
}


void handle_leave_message(void *data, struct sockaddr_in sock){

    //check whether the user is in usernames
    //if yes check whether channel is in channels
    //check whether the user is in the channel
    //if yes, remove user from channel
    //if not send an error message to the user

    //get message fields
    struct request_leave* msg;
    msg = (struct request_leave*)data;

    string channel = msg->req_channel;

    string ip = inet_ntoa(sock.sin_addr);

    int srcport = sock.sin_port;

    char port_str[6];
    sprintf(port_str, "%d", srcport);
    string key = ip + "." +port_str;

    //check whether key is in rev_usernames
    map <string,string> :: iterator iter;

    // print debug message 
    cout << hostname << ":" << port << ip << ":" << srcport 
        << " recv Request Leave " << channel  << endl;

    iter = rev_usernames.find(key);
    if (iter == rev_usernames.end() ){
    
        //ip+port not recognized - send an error message
        send_error_message(sock, "Not logged in");

    }else{

        string username = rev_usernames[key];

        map<string,channel_type>::iterator channel_iter;

        channel_iter = channels.find(channel);

        active_usernames[username] = 1;

        if (channel_iter == channels.end()){
            
            //channel not found
            send_error_message(sock, "No channel by the name " + channel);
            cout << "server: " << username << " trying to leave non-existent channel " << channel << endl;

        }else{

            //channel already exits
            //map<string,struct sockaddr_in> existing_channel_users;
            //existing_channel_users = channels[channel];
            map<string,struct sockaddr_in>::iterator channel_user_iter;
            channel_user_iter = channels[channel].find(username);

            if (channel_user_iter == channels[channel].end()){
                //user not in channel
                send_error_message(sock, "You are not in channel " + channel);
                cout << "server: " << username << " trying to leave channel " << channel  << " where he/she is not a member" << endl;
            }else{
                channels[channel].erase(channel_user_iter);
                cout << "server: " << username << " leaves channel " << channel <<endl;

                //delete channel if no more users
                if (channels[channel].empty() && (channel != "Common")){
                    channels.erase(channel_iter);
                    cout << "server: " << "removing empty channel " << channel <<endl;
                }
            }
        }
    }
}

void handle_say_message(void *data, struct sockaddr_in sock)
{
    //check whether the user is in usernames
    //if yes check whether channel is in channels
    //check whether the user is in the channel
    //if yes send the message to all the members of the channel
    //if not send an error message to the user

    //get message fields
    struct request_say* msg;
    msg = (struct request_say*)data;

    string channel = msg->req_channel;
    string text = msg->req_text;

    string ip = inet_ntoa(sock.sin_addr);

    int srcport = sock.sin_port;

    char port_str[6];
    sprintf(port_str, "%d", srcport);
    string key = ip + "." +port_str;

    //check whether key is in rev_usernames
    map <string,string> :: iterator iter;
    
    // print debug message 
    cout << hostname << ":" << port << ip << ":" << srcport 
        << " recv Request Say " << channel << " \"" << text << "\"" << endl;

    iter = rev_usernames.find(key);
    if (iter == rev_usernames.end() ){

        //ip+port not recognized - send an error message
        send_error_message(sock, "Not logged in ");

    }else{
        string username = rev_usernames[key];

        map<string,channel_type>::iterator channel_iter;

        channel_iter = channels.find(channel);

        active_usernames[username] = 1;

        if (channel_iter == channels.end()){

            //channel not found
            send_error_message(sock, "No channel by the name " + channel);
            cout << "server: " << username << " trying to send a message to non-existent channel " << channel << endl;

        }else{
    
            //channel already exits
            //map<string,struct sockaddr_in> existing_channel_users;
            //existing_channel_users = channels[channel];
            map<string,struct sockaddr_in>::iterator channel_user_iter;
            channel_user_iter = channels[channel].find(username);

            if (channel_user_iter == channels[channel].end()){
           
                 //user not in channel
                send_error_message(sock, "You are not in channel " + channel);
                cout << "server: " << username << " trying to send a message to channel " << channel  << " where he/she is not a member" << endl;
            
            }else{
                map<string,struct sockaddr_in> existing_channel_users;
                existing_channel_users = channels[channel];
                for(channel_user_iter = existing_channel_users.begin(); channel_user_iter != existing_channel_users.end(); channel_user_iter++){
                    ssize_t bytes;
                    void *send_data;
                    size_t len;

                    struct text_say send_msg;
                    send_msg.txt_type = TXT_SAY;

                    const char* str = channel.c_str();
                    strcpy(send_msg.txt_channel, str);
                    str = username.c_str();
                    strcpy(send_msg.txt_username, str);
                    str = text.c_str();
                    strcpy(send_msg.txt_text, str);
                    send_data = &send_msg;

                    len = sizeof send_msg;

                    struct sockaddr_in send_sock = channel_user_iter->second;

                    bytes = sendto(s, send_data, len, 0, (struct sockaddr*)&send_sock, sizeof send_sock);

                    if (bytes < 0)
                        perror("Message failed\n"); //error

                }
                cout << "server: " << username << " sends say message in " << channel <<endl;
            }
        }
    }
}

void handle_list_message(struct sockaddr_in sock)
{

    //check whether the user is in usernames
    //if yes, send a list of channels
    //if not send an error message to the user

    string ip = inet_ntoa(sock.sin_addr);

    int srcport = sock.sin_port;

    char port_str[6];
    sprintf(port_str, "%d", srcport);
    string key = ip + "." +port_str;


    //check whether key is in rev_usernames
    map <string,string> :: iterator iter;

    // print debug message 
    cout << hostname << ":" << port << ip << ":" << srcport 
        << " recv Request List " << endl;

    iter = rev_usernames.find(key);
    if (iter == rev_usernames.end() )
    {
        //ip+port not recognized - send an error message
        send_error_message(sock, "Not logged in ");
    }
    else
    {
        string username = rev_usernames[key];
        int size = channels.size();
        //cout << "size: " << size << endl;

        active_usernames[username] = 1;

        ssize_t bytes;
        void *send_data;
        size_t len;


        //struct text_list temp;
        struct text_list *send_msg = (struct text_list*)malloc(sizeof (struct text_list) + (size * sizeof(struct channel_info)));


        send_msg->txt_type = TXT_LIST;

        send_msg->txt_nchannels = size;


        map<string,channel_type>::iterator channel_iter;



        //struct channel_info current_channels[size];
        //send_msg.txt_channels = new struct channel_info[size];
        int pos = 0;

        for(channel_iter = channels.begin(); channel_iter != channels.end(); channel_iter++)
        {
            string current_channel = channel_iter->first;
            const char* str = current_channel.c_str();
            //strcpy(current_channels[pos].ch_channel, str);
            //cout << "channel " << str <<endl;
            strcpy(((send_msg->txt_channels)+pos)->ch_channel, str);
            //strcpy(((send_msg->txt_channels)+pos)->ch_channel, "hello");
            //cout << ((send_msg->txt_channels)+pos)->ch_channel << endl;

            pos++;

        }



        //send_msg.txt_channels =
        //send_msg.txt_channels = current_channels;
        send_data = send_msg;
        len = sizeof (struct text_list) + (size * sizeof(struct channel_info));

                    //cout << username <<endl;
        struct sockaddr_in send_sock = sock;

        bytes = sendto(s, send_data, len, 0, (struct sockaddr*)&send_sock, sizeof send_sock);

        if (bytes < 0)
            perror("Message failed\n"); //error

        cout << "server: " << username << " lists channels"<<endl;
    }
}


void handle_who_message(void *data, struct sockaddr_in sock)
{
    //check whether the user is in usernames
    //if yes check whether channel is in channels
    //if yes, send user list in the channel
    //if not send an error message to the user

    //get message fields
    struct request_who* msg;
    msg = (struct request_who*)data;

    string channel = msg->req_channel;

    string ip = inet_ntoa(sock.sin_addr);

    int srcport = sock.sin_port;

    char port_str[6];
    sprintf(port_str, "%d", srcport);
    string key = ip + "." +port_str;

    //check whether key is in rev_usernames
    map <string,string> :: iterator iter;

    // print debug message 
    cout << hostname << ":" << port << ip << ":" << srcport 
        << " recv Request Who " << channel << endl;

    iter = rev_usernames.find(key);
    if (iter == rev_usernames.end() ){
    
        //ip+port not recognized - send an error message
        send_error_message(sock, "Not logged in ");
    
    }else{
        string username = rev_usernames[key];

        active_usernames[username] = 1;

        map<string,channel_type>::iterator channel_iter;

        channel_iter = channels.find(channel);

        if (channel_iter == channels.end()){
            //channel not found
            send_error_message(sock, "No channel by the name " + channel);
            cout << "server: " << username << " trying to list users in non-existing channel " << channel << endl;

        }else{
            //channel exits
            map<string,struct sockaddr_in> existing_channel_users;
            existing_channel_users = channels[channel];
            int size = existing_channel_users.size();

            ssize_t bytes;
            void *send_data;
            size_t len;

            struct text_who *send_msg = (struct text_who*)malloc(sizeof (struct text_who) + (size * sizeof(struct user_info)));

            send_msg->txt_type = TXT_WHO;

            send_msg->txt_nusernames = size;

            const char* str = channel.c_str();

            strcpy(send_msg->txt_channel, str);

            map<string,struct sockaddr_in>::iterator channel_user_iter;

            int pos = 0;

            for(channel_user_iter = existing_channel_users.begin(); channel_user_iter != existing_channel_users.end(); channel_user_iter++){

                string username = channel_user_iter->first;

                str = username.c_str();

                strcpy(((send_msg->txt_users)+pos)->us_username, str);

                pos++;

            }

            send_data = send_msg;
            len = sizeof(struct text_who) + (size * sizeof(struct user_info));

            struct sockaddr_in send_sock = sock;

            bytes = sendto(s, send_data, len, 0, (struct sockaddr*)&send_sock, sizeof send_sock);

            if (bytes < 0)
                perror("Message failed\n"); //error

            cout << "server: " << username << " lists users in channnel "<< channel << endl;
        }
    }
}

void handle_keep_alive_message(struct sockaddr_in sock)
{

    //check whether the user is in usernames
    //if yes, set active_usernames[username]
    //if not send an error message to the user

    string ip = inet_ntoa(sock.sin_addr);

    int srcport = sock.sin_port;

    char port_str[6];
    sprintf(port_str, "%d", srcport);
    string key = ip + "." +port_str;
    
    // print debug message 
    cout << hostname << ":" << port << ip << ":" << srcport 
        << " recv Request Keep_Alive " << endl;

    //check whether key is in rev_usernames
    map <string,string> :: iterator iter;

    iter = rev_usernames.find(key);
    if (iter == rev_usernames.end() )
    {
        //ip+port not recognized - send an error message
    }
    else
    {
        string username = rev_usernames[key];
        active_usernames[username] = 1; //set the user as active
        cout << "server: " << username << " keeps alive" << endl;
    }
}

void handle_s_join(void *data, struct sockaddr_in sock){
}

void handle_s_leave(void *data, struct sockaddr_in sock){
}

void handle_s_say(void *data, struct sockaddr_in sock){
}

void send_error_message(struct sockaddr_in sock, string error_msg)
{
    ssize_t bytes;
    void *send_data;
    size_t len;

    struct text_error send_msg;
    send_msg.txt_type = TXT_ERROR;

    const char* str = error_msg.c_str();
    strcpy(send_msg.txt_error, str);

    send_data = &send_msg;

    len = sizeof send_msg;

    struct sockaddr_in send_sock = sock;

    bytes = sendto(s, send_data, len, 0, (struct sockaddr*)&send_sock, sizeof send_sock);

    if (bytes < 0)
        perror("Message failed\n"); //error

}
