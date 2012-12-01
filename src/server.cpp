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
#include <list>
#include <iostream>

// Local includes
#include "duckchat.h"
#include "server.h"

using namespace std;

/** Information about our server that we need globaly for error messages **/
int                 our_sockfd;                 // socket for listening
struct sockaddr_in  our_server;                 // OUR SERVER sockaddr struct
char                our_hostname[HOSTNAME_MAX]; // name that we resolve for our IP Addr
int                 our_port;                   // port number of our server

/** Data structures that we wish to access from any context **/
//map from username to User's sockaddr_in
map< string, struct sockaddr_in >               usernames;          // holds users and their sockaddr_in
//map from ip+port to username 
map< string, string >                           rev_usernames;      // holds users that have logged in and their ip+port in string form
//map from username to activity code [0-inactive, 1-active]
map< string, int >                              active_usernames;   // holds users that have logged in and their activity status
//map from channel to (map from username to User's sockaddr_in)
map< string, map<string,struct sockaddr_in> >   channels;           // holds channels the users who have joined that channel        
//map from channel to (list of sockaddr_in of nearby servers)        
map< string, list<struct sockaddr_in> >         channels_server;    // a map from channel name to the servers that have joined that channel
//list of nearby servers
list< struct sockaddr_in >                      nearby_servers;     // a list of the nearby servers we are connected to
//list of random IDs
list< long long >                                     uniqueID_list;   // a list of unique IDs that will not exceed MAX_UNIQUEID 


int main(int argc, char *argv[]){
        
    int
        devrand_fd,     // the fd that we will use to access a random number 
        ret;            // return value from function calls

    struct timeval 
        tv;             // create struct for select timeout
    
    fd_set 
        fds;            // for call to select

    char 
        seed[sizeof(unsigned int)];        // holds the seed collected from ura
    
    devrand_fd = open("/dev/urand", O_RDONLY);   
    read(devrand_fd, seed, sizeof(unsigned int));
    close(devrand_fd);
    srand((unsigned int)*seed); 

// (1) Verify user input
    if (argc < 3){
        printf("Usage: %s our_domain_name our_port_num [nearby_serverIP & nearby_serverPort]\n", argv[0]);
        exit(1);
    }
    
// (2) Get information about our server and nearby servers
    // ask the OS for a socket FD for OUR server
    our_sockfd = socket(PF_INET, SOCK_DGRAM, 0);
    if (our_sockfd < 0){
        perror ("socket() failed");
        exit(1);
    }

    // get the stuff for our server and all of the other nearby servers
    struct hostent                  *tmp_hostent;   // this helps us resolve hosts
    int                             tmp_port;       // hold onto port number until we resolve it 
    char                            tmp_hostname[HOSTNAME_MAX]; // hold onto hostname until we resolve it  
    struct sockaddr_in              tmp_serv;       // hold the sockaddr_in of the resolved host before we place it into nearby_servers
    
    // step through all of the command line argument hostname/port pairs
    for(int i=0; i < (argc-1) ; i+=2){
    
        // set temporary hostname and port number
        strcpy(tmp_hostname, argv[i+1]);
        tmp_port = atoi(argv[i+2]);
        
        // resolve information about host
        if ((tmp_hostent = gethostbyname(tmp_hostname)) == NULL) {
            cout << "error resolving hostname.." << endl;
            exit(1);
        }

        // set up the tmp_serv info for storing
        tmp_serv.sin_port = htons(tmp_port);
        tmp_serv.sin_family = AF_INET;
        memcpy(&tmp_serv.sin_addr, tmp_hostent->h_addr_list[0], tmp_hostent->h_length);
       
        // Save server info to either nearby server list or to our server info
        if(i == 0){ // "our" server
            our_port = tmp_port;
            strcpy(our_hostname, inet_ntoa(tmp_serv.sin_addr));
            memcpy(&our_server.sin_addr, tmp_hostent->h_addr_list[0], tmp_hostent->h_length);
            our_server.sin_port = tmp_serv.sin_port;
            our_server.sin_family = tmp_serv.sin_family;
        
        }else{ // nearby (NOT ours)
            nearby_servers.push_back(tmp_serv);
        }
    }
    
    // bind the socket to our addrinfo
    ret = bind(our_sockfd, (struct sockaddr*)&our_server, sizeof(our_server));
    if (ret < 0){
        perror("bind failed");
        exit(1);
    }
    

// (3) Main event loop for client and s2s requests    
    // TODO: Set up a timeout that resends s2s join to all nearby servers that we are on the channel of
    // TODO: Set up a timeout that kicks connected servers if they have not sent a join within the last two minutes
    // set our timeout
    // change this timeout to one minute and store some extra state so that 
    tv.tv_sec = TIMEOUT_CLIENT_USAGE;
    tv.tv_usec = 0;
    int client_keepAlive = 0;
    while(1){

        // set up the fd_set for call to select 
        FD_ZERO(&fds);
        FD_SET(our_sockfd, &fds);

        // Use select to determine where the request is coming from
        ret = select(our_sockfd+1, &fds, NULL, NULL, &tv);
        if (ret < 0){
            perror("error in select");
            exit(1);
        }
        
        // if there was a request, handle it and reduce the time until we reasses client usage 
        if (FD_ISSET(our_sockfd,&fds)){
            
            handle_socket_input();
            
        // if there was not a request, it means that two minutes have elapsed and we need to verify that users
        // are still active
        }else{ // 2 minute timeout
    
            
            // reset timer
            tv.tv_sec = TIMEOUT_CLIENT_USAGE;
            tv.tv_usec = 0;
            client_keepAlive++;
    
            // Server soft state stuff
            // This for loop resends to each server that we are connected to for each channel
            map< string, list<struct sockaddr_in> >::iterator servList_it;
            for (servList_it = channels_server.begin() ; servList_it != channels_server.end() ; servList_it++){
                
                // pack the s2s_join message with ID and channel name
                struct s2s_join join_msg;
                strncpy(join_msg.s2s_channel, servList_it->first.c_str(), CHANNEL_MAX);
                join_msg.s2s_type = htonl(S2S_JOIN);
    
                list<struct sockaddr_in> servList = servList_it->second;
                list<struct sockaddr_in>::iterator serv_it;
                for(serv_it = servList.begin() ; serv_it != servList.end() ; serv_it++){
                    ret = sendto(our_sockfd, &join_msg, sizeof(join_msg), 0, (struct sockaddr*) &(*serv_it), sizeof(*serv_it));
                    if (ret == -1){
                        perror("sendto fail");
                    }            
                }
            }                     

            // every OTHER timeout
            if (client_keepAlive >= 2){
                // means that we do not do it next time
                client_keepAlive = 0;
            
                // CLIENT version of keepalive
                // loop through all users. if they are inactive, remove them from appropriate lists
                map<string,int>::iterator active_user_iter;
                for(active_user_iter = active_usernames.begin(); active_user_iter != active_usernames.end(); active_user_iter++){
                    string username = active_user_iter->first;
                    int isActive = active_user_iter->second;

                    if (!(isActive)){

                        //key has to be constructed to remove from rev_usernames
                        cout << "server: forcibly removing user " << username << endl;

                        map<string, struct sockaddr_in>::iterator user_iter;
                        user_iter = usernames.find(username);

                        //key has to be constructed to remove from rev_usernames
                        struct sockaddr_in sock = user_iter->second;
                        string ip = inet_ntoa(sock.sin_addr);

                        int srcport = ntohs(sock.sin_port);

                        char port_str[6];
                        sprintf(port_str, "%d", srcport);
                        string key = ip + "." +port_str;

                        map <string,string> :: iterator rev_user_iter;

                        rev_user_iter = rev_usernames.find(key);
                        rev_usernames.erase(rev_user_iter);

                        //remove from usernames
                        usernames.erase(user_iter);

                        //remove from all the channels if found
                        map<string,map<string, struct sockaddr_in> >::iterator channel_iter;
                        for(channel_iter = channels.begin(); channel_iter != channels.end(); channel_iter++){
                            map<string, struct sockaddr_in>::iterator within_channel_iterator;
                            within_channel_iterator = channel_iter->second.find(username);

                            if (within_channel_iterator != channel_iter->second.end())
                                channel_iter->second.erase(within_channel_iterator);

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

    // never get here
    return 0;
}

/******************************** Local Functions ********************************/

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

    bytes = recvfrom(our_sockfd, data, len, 0, (struct sockaddr*)&recv_client, &fromlen);

    if (bytes < 0)
        perror ("recvfrom failed\n");
    
    else{
        struct request* request_msg;
        request_msg = (struct request*)data;

        request_t message_type = ntohl(request_msg->req_type);
        
        switch (message_type){
                
            case REQ_LOGIN:
                handle_login_message(data, recv_client); 
                break;
            
            case REQ_LOGOUT: 
                handle_logout_message(recv_client);
                break;
            
            case REQ_JOIN:
                handle_join_message(data, recv_client);
                break;
            
            case REQ_LEAVE: 
                handle_leave_message(data, recv_client);
                break;
            
            case REQ_SAY: 
                handle_say_message(data, recv_client);
                break;
            
            case REQ_LIST:    
                handle_list_message(recv_client);
                break;
            
            case REQ_WHO: 
                handle_who_message(data, recv_client);
                break;
            
            case REQ_KEEP_ALIVE: 
                handle_keep_alive_message(recv_client);
                break;
            
            case S2S_JOIN: 
                handle_s_join(data, recv_client);
                break;
            
            case S2S_LEAVE: 
                handle_s_leave(data, recv_client);
                break;
            
            case S2S_SAY: 
                handle_s_say(data, recv_client);
                break;

            default:
                //send error message to client
                send_error_message(recv_client, "!!Incoming request type unkown!!");
        }    
    }
}


void handle_login_message(void *data, struct sockaddr_in sock){

    struct request_login* msg;
    msg = (struct request_login*)data;

    string username = msg->req_username;
    usernames[username] = sock;
    active_usernames[username] = 1;

    string ip = inet_ntoa(sock.sin_addr);
    int srcport = ntohs(sock.sin_port);

    char port_str[6];
    sprintf(port_str, "%d", srcport);

    string key = ip + "." +port_str;
    rev_usernames[key] = username;

    // print debug message
    cout << our_hostname << ":" << our_port << " " << ip << ":" << srcport 
        << " recv Request Login" << endl;
}

void handle_logout_message(struct sockaddr_in sock){
    //construct the key using sockaddr_in
    string ip = inet_ntoa(sock.sin_addr);
    int srcport = ntohs(sock.sin_port);

    char port_str[6];
    sprintf(port_str, "%d", srcport);

    string key = ip + "." +port_str;

    //check whether key is in rev_usernames
    map <string,string> :: iterator iter;

    iter = rev_usernames.find(key);
        
    // print debug message
    cout << our_hostname << ":" << our_port << " " << ip << ":" << srcport 
            << " recv Request Logout" << endl;
    
    if (iter == rev_usernames.end() ){ // if the user was not in our list, send not logged in
        send_error_message(sock, "Not logged in");

    }else{

        string username = rev_usernames[key];
        rev_usernames.erase(iter);

        //remove from usernames
        map<string, struct sockaddr_in>::iterator user_iter;
        user_iter = usernames.find(username);
        usernames.erase(user_iter);

        //remove from all the channels if found
        map<string,map<string, struct sockaddr_in> >::iterator channel_iter;

        for(channel_iter = channels.begin(); channel_iter != channels.end(); channel_iter++){
            map<string, struct sockaddr_in>::iterator within_channel_iterator;
            within_channel_iterator = channel_iter->second.find(username);

            if (within_channel_iterator != channel_iter->second.end())
                channel_iter->second.erase(within_channel_iterator);

        }

        //remove entry from active usernames also
        map<string,int>::iterator active_user_iter;
        active_user_iter = active_usernames.find(username);
        active_usernames.erase(active_user_iter);

    }
}

// DONE I THINK, we double checked all of the logic. So check everywhere else before
// you change anything in this function.
void handle_join_message(void *data, struct sockaddr_in sock)
{
    // If we are subscribed, everything goes as normal
    // if we ar NOT subscribed, flood all nearby servers with join message 

    int ret;
    
    //get message fields
    struct request_join* msg;
    msg = (struct request_join*)data;
    
    string channel = msg->req_channel;
    string ip = inet_ntoa(sock.sin_addr);
    int srcport = ntohs(sock.sin_port);
    char port_str[6];
    sprintf(port_str, "%d", srcport);
    string key = ip + "." + port_str;

    // print debug message
    cout << our_hostname << ":" << our_port << " " << ip << ":" << srcport 
         << " recv Request Join " << channel << endl;
  
    // if the user is not logged in, send back an error message 
    if (rev_usernames.find(key) == rev_usernames.end() ){ //ip+port not recognized - send an error message
        send_error_message(sock, "Not logged in");

    // if the user is logged in
        // if the server has the channel, simply add the user
        // if the server does not have the channel, create
        // if the server does not have a s2s channel, broadcast join to nearby servers 
    }else{

        string username = rev_usernames[key]; // get their username based on key
        active_usernames[username] = 1;
    
        // if we cannot find the channel in our nearby_servers
            // try to locate other servers in the channel by sending a join message to all nearby servers 
        if(channels_server.find(channel) == channels_server.end()){
            
            // pack the s2s_join message with ID and channel name
            struct s2s_join join_msg;
            strncpy(join_msg.s2s_channel, channel.c_str(), CHANNEL_MAX);
            join_msg.s2s_type = htonl(S2S_JOIN);
            
            // send to all nearby servers
            for( list<struct sockaddr_in>::iterator it = nearby_servers.begin() ; it != nearby_servers.end() ; it++){
                
                // INFORMATION message
                cout << our_hostname << ":" << our_port << " " << ip << ":" << srcport 
                    << " send S2S Join " << channel << endl;

                // send the message to our nearby server
                ret = sendto(our_sockfd, &join_msg, sizeof(join_msg), 0, (struct sockaddr*) &(*it), sizeof(*it));
                if (ret == -1){
                    perror("sendto fail");
                }            
            } 
        }

        // if the channel does not exist in our list of channels
        if (channels.find(channel) != channels.end() ){
    
            // add the user to the channels
            map<string, struct sockaddr_in> new_channel_users;
            new_channel_users[username] = sock;
            channels[channel] = new_channel_users;
       
        // when the channel is one that we already know about
        //      add the user to the channels to username map 
        }else{
            channels[channel][username] = sock;
        }        
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

    int srcport = ntohs(sock.sin_port);

    char port_str[6];
    sprintf(port_str, "%d", srcport);
    string key = ip + "." +port_str;

    //check whether key is in rev_usernames
    map <string,string> :: iterator iter;

    // print debug message 
    cout << our_hostname << ":" << our_port << " " << ip << ":" << srcport 
        << " recv Request Leave " << channel  << endl;

    iter = rev_usernames.find(key);
    if (iter == rev_usernames.end() ){
    
        //ip+port not recognized - send an error message
        send_error_message(sock, "Not logged in");

    }else{

        string username = rev_usernames[key];

        map<string,map<string, struct sockaddr_in> >::iterator channel_iter;

        channel_iter = channels.find(channel);

        active_usernames[username] = 1;

        if (channel_iter == channels.end()){
            
            //channel not found
            send_error_message(sock, "No channel by the name " + channel);
            cout << "server: " << username << " trying to leave non-existent channel " << channel << endl;

        }else{

            //channel already exits
            map<string, struct sockaddr_in>::iterator channel_user_iter;
            channel_user_iter = channels[channel].find(username);

            if (channel_user_iter == channels[channel].end()){
                //user not in channel
                send_error_message(sock, "You are not in channel " + channel);
                cout << "server: " << username << " trying to leave channel " << channel  << " where he/she is not a member" << endl;
            }else{
                channels[channel].erase(channel_user_iter);
                cout << "server: " << username << " leaves channel " << channel <<endl;

                //delete channel if no more users
                if (channels[channel].empty()){
                    channels.erase(channel_iter);
                    cout << "server: " << "removing empty channel " << channel <<endl;
                }
            }
        }
    }
}

void handle_say_message(void *data, struct sockaddr_in sock)
{


    // check whether the user is in usernames
    // if yes check whether channel is in channels
    // check whether the user is in the channel
    // if yes send the message to all the members of the channel and to servers on the channel
    // if not send an error message to the user

    int ret;

    //get message fields
    struct request_say* msg;
    msg = (struct request_say*)data;

    string channel = msg->req_channel;
    string text = msg->req_text;

    string ip = inet_ntoa(sock.sin_addr);

    int srcport = ntohs(sock.sin_port);

    char port_str[6];
    sprintf(port_str, "%d", srcport);
    string key = ip + "." +port_str;

    //check whether key is in rev_usernames
    map <string,string> :: iterator iter;
    
    // print debug message 
    cout << our_hostname << ":" << our_port << " " << ip << ":" << srcport 
        << " recv Request Say " << channel << " \"" << text << "\"" << endl;

    iter = rev_usernames.find(key);
    if (iter == rev_usernames.end() ){

        //ip+port not recognized - send an error message
        send_error_message(sock, "Not logged in ");

    }else{
    
        string username = rev_usernames[key];
        active_usernames[username] = 1;

        // dealing with nearby servers
        if (channels_server.find(channel) != channels_server.end()){
            list<struct sockaddr_in> servList = channels_server[channel];

            // This works              
            long long uniqueID = rand();
            uniqueID = uniqueID<<32 | rand();           
 
            // put together our s2s message to broadcast
            struct s2s_say s_msg;
            s_msg.s2s_type = htonl(S2S_SAY);
            s_msg.s2s_uniqueID = be64toh(uniqueID);
            strcpy(s_msg.s2s_channel, channel.c_str());
            strcpy(s_msg.s2s_username, username.c_str());
            strncpy(s_msg.s2s_text, text.c_str(), SAY_MAX);
    
            // broadcast to all servers connected to channel
            for(list< struct sockaddr_in>::iterator it = servList.begin(); it != servList.end() ; it++ ){

                struct sockaddr_in send_sock = *it;

                ret = sendto(our_sockfd, &s_msg, sizeof(s_msg), 0, (struct sockaddr*)&send_sock, sizeof send_sock);
                if (ret == -1){
                    perror("sendto fail");
                }            
            } 
        }

        map<string,map<string, struct sockaddr_in> >::iterator channel_iter;
        channel_iter = channels.find(channel);
        
        if (channel_iter != channels.end()){

            //channel already exits
            map<string,struct sockaddr_in>::iterator channel_user_iter;
            channel_user_iter = channels[channel].find(username);

            if (channel_user_iter == channels[channel].end()){
           
                 //user not in channel
                send_error_message(sock, "You are not in channel " + channel);
                cout << "server: " << username << " trying to send a message to channel " << channel  << " where he/she is not a member" << endl;
            
            }else{ 
                map<string, struct sockaddr_in> existing_channel_users;
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

                    bytes = sendto(our_sockfd, send_data, len, 0, (struct sockaddr*)&send_sock, sizeof send_sock);

                    if (bytes < 0)
                        perror("Message failed\n"); //error

                }
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

    int srcport = ntohs(sock.sin_port);

    char port_str[6];
    sprintf(port_str, "%d", srcport);
    string key = ip + "." +port_str;


    //check whether key is in rev_usernames
    map <string,string> :: iterator iter;

    // print debug message 
    cout << our_hostname << ":" << our_port << " " << ip << ":" << srcport 
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


        map<string,map<string, struct sockaddr_in> >::iterator channel_iter;



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

        bytes = sendto(our_sockfd, send_data, len, 0, (struct sockaddr*)&send_sock, sizeof send_sock);

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

    int srcport = ntohs(sock.sin_port);

    char port_str[6];
    sprintf(port_str, "%d", srcport);
    string key = ip + "." +port_str;

    //check whether key is in rev_usernames
    map <string,string> :: iterator iter;

    // print debug message 
    cout << our_hostname << ":" << our_port << " " << ip << ":" << srcport 
        << " recv Request Who " << channel << endl;

    iter = rev_usernames.find(key);
    if (iter == rev_usernames.end() ){
    
        //ip+port not recognized - send an error message
        send_error_message(sock, "Not logged in ");
    
    }else{
        string username = rev_usernames[key];

        active_usernames[username] = 1;

        map<string,map<string, struct sockaddr_in> >::iterator channel_iter;

        channel_iter = channels.find(channel);

        if (channel_iter == channels.end()){
            //channel not found
            send_error_message(sock, "No channel by the name " + channel);
            cout << "server: " << username << " trying to list users in non-existing channel " << channel << endl;

        }else{
            //channel exits
            map<string, struct sockaddr_in> existing_channel_users;
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

            map<string, struct sockaddr_in>::iterator channel_user_iter;

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

            bytes = sendto(our_sockfd, send_data, len, 0, (struct sockaddr*)&send_sock, sizeof send_sock);

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

    int srcport = ntohs(sock.sin_port);

    char port_str[6];
    sprintf(port_str, "%d", srcport);
    string key = ip + "." +port_str;
    
    // print debug message 
    cout << our_hostname << ":" << our_port << " " << ip << ":" << srcport 
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
   
    // (1) check to see if we are subscribed to channel
    //      if YES end join message flooding
    // (2) if not, subscribe ourselves to the channel and broadcast to all nearby servers excluding sock

    int ret;

    // make it easy to access msg crap
    struct s2s_join *msg;
    msg = (struct s2s_join*) data;

    // initialize all the message crap
    string channel  = msg->s2s_channel;
    
    // initialize all of the ipaddr and port stuff
    string ip = inet_ntoa(sock.sin_addr);
    int srcport = ntohs(sock.sin_port);
    char port_str[6];
    sprintf(port_str, "%d", srcport);
    string key = ip + "." + port_str;

    // print debug message 
    cout << our_hostname << ":" << our_port << " " << ip << ":" << srcport 
        << " recv S2S Join " << channel << endl;

    
    // MUST VERIFY THAT WE ARE SUBSCRIBED
    // if we are not subscribed to the channel
        // subscribe ourselves to channel
        // broadcast to all nearby _excluding_ sock
    if(channels_server.find(channel) == channels_server.end()){
        DBG("%s:%d is not subscribed to %s\n", our_hostname, our_port, channel.c_str());
    
            list<struct sockaddr_in> servList;
            // step through all nearby servers. adding them to our list associated with that channel as we go
            for( list<struct sockaddr_in>::iterator it = nearby_servers.begin() ; it != nearby_servers.end() ; it++){
                
                // we make this check because we do not want to broadcast back to whoever broadcasted to us
                if (sock.sin_addr.s_addr == it->sin_addr.s_addr && ntohs(sock.sin_port) == it->sin_port)
                    continue;
        
                cout << our_hostname << ":" << our_port << " " << inet_ntoa(it->sin_addr) << ":" << it->sin_port
                    << " send S2S Join " << channel << endl;
                
                // adding this nearby server to our list
                servList.push_back(*it);
                
                ret = sendto(our_sockfd, data, sizeof(*(struct s2s*)data), 0,  (struct sockaddr*) &(*it), sizeof(*it));
                if (ret == -1){
                    perror("sendto fail");
                }            
            }
         
            // finally, push servList into channels_server structure
            channels_server[channel] = servList; 
    }

    // why does 
    DBG("--------------- Servers in channel: %s --------------------\n", channel.c_str());
    // while servers in channel
    for(list<struct sockaddr_in>::iterator it = channels_server[channel].begin() ; it != channels_server[channel].end() ; it++){
        DBG("%s:%d\n", inet_ntoa(it->sin_addr), it->sin_port );
        // print server
    }
}

void handle_s_leave(void *data, struct sockaddr_in sock){
   
    // make it easy to access msg crap
    struct s2s_leave *msg;
    msg = (struct s2s_leave*) data;

    // initialize all the message crap
    string channel = msg->s2s_channel;
    
    // initialize all of the ipaddr and port stuff from the sender
    string ip = inet_ntoa(sock.sin_addr);
    int srcport = htons(sock.sin_port);
    char port_str[6];
    sprintf(port_str, "%d", srcport);
    string key = ip + "." + port_str;

    // print debug message 
    cout << our_hostname << ":" << our_port << " " << ip << ":" << srcport 
        << " recv S2S Leave " << channel << endl;

    // make sure that we have information on that channel
    if(channels_server.find(channel) != channels_server.end()){
        list<struct sockaddr_in> server_list = channels_server[channel];
        
        // check the list for this incoming sock and remove it if found
        for (list<struct sockaddr_in>::iterator it = server_list.begin() ; it != server_list.end() ; it++){
            sock.sin_addr = it->sin_addr;
            if(it->sin_addr.s_addr == sock.sin_addr.s_addr && it->sin_port == ntohs(sock.sin_port)){
                server_list.erase(it);
                break;        
            }
        }

        channels_server.erase(channel);
        channels_server[channel] = server_list;
    }
}

void handle_s_say(void *data, struct sockaddr_in sock){

    // Check unique identifier against our list of unique identifiers
    //          if it matches any in our list then drop the message and send s2s leave to the server received from
  
    // Forward message to any other S2S server in this channel
    // AND to any interested users
    
    // If no users are subscribed to channel that message is sent over AND server that sent this 
    // message is the only one of our nearby servers subscribed to channel,
    //          reply to server that sent this with 's2s_leave'


    int ret; 
    // make it easy to access msg content
    struct s2s_say *msg;
    msg = (struct s2s_say*) data;

    // initialize all the message content to local variables
    long long uniqueID = htobe64(msg->s2s_uniqueID);
    string channel  = msg->s2s_channel;
    string username = msg->s2s_username;
    string text     = msg->s2s_text; 
    
    // initialize all of the ipaddr and port stuff
    string ip = inet_ntoa(sock.sin_addr);
    int srcport = ntohs(sock.sin_port);
    char port_str[6];
    sprintf(port_str, "%d", srcport);
    string key = ip + "." + port_str;

    //check whether key is in rev_usernames
    map <string,string> :: iterator iter;

    // print debug message 
    cout << our_hostname << ":" << our_port << " " << ip << ":" << srcport 
        << " recv S2S Say " << username << " " << channel << "\"" << text << "\"" << endl;
    
    // if sock is the only in channels_server[channel]
    if (channels_server.find(channel) != channels_server.end()){
        if (channels_server[channel].size() <= 1 && channels.find(channel) == channels.end()){ 
        
            // construct our leave message
            struct s2s_leave l_msg;
            l_msg.s2s_type = htonl(S2S_LEAVE);
            strcpy(l_msg.s2s_channel, channel.c_str());

            // send leave request to server that sent us this say 
            ret = sendto(our_sockfd, &l_msg, sizeof(l_msg), 0,(struct sockaddr*)&sock, sizeof(sock));    
            if (ret == -1)
                perror("sendto failed");
            return;
        } 
    }
    
    // if this message has been received before
    // reply with leave!
    for(list<long long>::iterator it = uniqueID_list.begin() ; it != uniqueID_list.end() ; it++){
        if (*it == uniqueID){
            // construct our leave message
            struct s2s_leave l_msg;
            l_msg.s2s_type = htonl(S2S_LEAVE);
            strcpy(l_msg.s2s_channel, channel.c_str());

            // send leave request to server that sent us this say 
            ret = sendto(our_sockfd, &l_msg, sizeof(l_msg), 0,(struct sockaddr*)&sock, sizeof(sock));    
            if (ret == -1)
                perror("sendto failed");
            return;
        }
    }
    // if we have gotten hear, it means that we have not yet seen this uniqueID
    // so add it to the list
    
    uniqueID_list.push_back(uniqueID);
    while(uniqueID_list.size() >= MAX_UNIQUE_LIST)
        uniqueID_list.pop_front();
    
    // dealing with clients 
    if (channels.find(channel) != channels.end()){
        map<string, struct sockaddr_in> clientList = channels[channel];
        for(map<string, struct sockaddr_in>::iterator it = clientList.begin(); it != clientList.end() ; it++ ){

            struct text_say send_msg;
            send_msg.txt_type = TXT_SAY;

            strcpy(send_msg.txt_channel, channel.c_str());
            strcpy(send_msg.txt_username, username.c_str());
            strcpy(send_msg.txt_text, text.c_str());

            struct sockaddr_in send_sock = it->second;

            ret = sendto(our_sockfd, &send_msg, sizeof(send_msg), 0, (struct sockaddr*)&send_sock, sizeof send_sock);
            if (ret == -1){
                perror("sendto fail");
            }            
        } 
    }
    
    // dealing with nearby servers
    if (channels_server.find(channel) != channels_server.end()){
        list<struct sockaddr_in> servList = channels_server[channel];
        for(list< struct sockaddr_in>::iterator it = servList.begin(); it != servList.end() ; it++ ){
   
            // do not send back to the host that we received the message from         
            if(sock.sin_addr.s_addr == it->sin_addr.s_addr && ntohs(sock.sin_port) == it->sin_port )
                continue;

            struct sockaddr_in send_sock = *it;

            ret = sendto(our_sockfd, msg, sizeof(*msg), 0, (struct sockaddr*)&send_sock, sizeof send_sock);
            if (ret == -1){
                perror("sendto fail");
            }            
        } 
    }

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

    bytes = sendto(our_sockfd, send_data, len, 0, (struct sockaddr*)&send_sock, sizeof send_sock);

    if (bytes < 0)
        perror("Message failed\n"); //error

}
