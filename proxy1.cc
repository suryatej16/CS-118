/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include <iostream>
#include <string>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <time.h>

#include "http-request.h"
#include "http-response.h"
#include "compat.h"

#define MAX_CONNECTIONS 20
#define PROXY_PORT "14886"

using namespace std;


char* format_name(char* fd)
{
  size_t len = strlen(fd);
  char *temp = (char*)malloc(len*sizeof(char));
  size_t i = 0;
  for(; i < len; i++)
    {
      if(fd[i] == '/')
        {
          temp[i] = '^';
        }
      else
        {
          temp[i] = fd[i];
        }
    }
  temp[i] = '\0';
  return temp;
}

int process_client(int client_fd)
{
  //initialize flags for persistent connection, conditional get                                                        

repeat: 
  bool p_connection = false;
  bool conditional;
  conditional = false;
  string c_buffer;

  //Get the client request                                                                                              
  while(memmem(c_buffer.c_str(), c_buffer.length(), "\r\n\r\n", 4) == NULL)
    {
      char buf[1024];
      memset(buf, '\0', 1024);
      fd_set fd;
      struct timeval tv;
      FD_ZERO(&fd);
      FD_SET(client_fd, &fd);
      
      //set timeout values
      tv.tv_sec = 25;
      tv.tv_usec = 0;
      if((select(client_fd+1, &fd, NULL, NULL, &tv)) <= 0)
      if(recv(client_fd, buf, sizeof(buf), 0) < 0)
        {
          perror("Error with recv");
          return -1;
        }
      c_buffer.append(buf);
    }

  //make sure the request is formatted correctly                                                                        
  HttpRequest client;
  try
    {
      client.ParseRequest(c_buffer.c_str(), c_buffer.length());
    }
  catch (ParseException excp)
    {
      string result = "HTTP/1.0 ";
      string cmp1 = "Request is not GET";
      string cmp2 = "Only GET method is supported";
      if(strcmp(excp.what(), cmp1.c_str()) == 0 || strcmp(excp.what(), cmp2.c_str()) == 0)
        {
          result += "501 Not Implemented\r\n\r\n";
        }
      else
        {
          result += "400 Bad Request\r\n\r\n";
        }
      int length = result.length();
      int sent = send(client_fd, result.c_str(), length, 0);
      if(sent == -1)
        {
          fprintf(stderr, "Send error\n");
          return -1;
        }
    }
  

  //check for request version number                                                                                    
  string temp = "1.0";
  string temp2 = "1.1";
  if(temp.compare(client.GetVersion()) == 0)
    {
      p_connection = false;
    }
  else if(temp2.compare(client.GetVersion()) == 0)
    {
      p_connection = true;
    }
  else
    {
      fprintf(stderr, "Unsupported version of HTTP\n");
      return -1;
    }

  //Prepare the client request                                                                                          
  size_t total_size = client.GetTotalLength();
  char *req = (char*)malloc(total_size);
  client.FormatRequest(req);

  //get the full path name
  string host = client.GetHost();
  string path = client.GetPath();
  string cache_file = host+path;

  //get the port number
  char port[6];
  sprintf(port, "%u", client.GetPort());

  //see if the cache file exists
  int c_fd;
  c_fd = open(cache_file.c_str(), O_RDONLY, 0666);

  //cache file exists
  if(c_fd != -1)
    {
      //read from cache
      size_t max_size;
      max_size = 100000;
      char* cr = (char*)malloc(max_size);
      ssize_t bytes_read = read(c_fd, cr, max_size);
      if(bytes_read == -1)
        {
          fprintf(stderr, "Reading error from cache\n");
          return -1;
        }
      
      //start creating a response
      HttpResponse client_response;
      client_response.ParseResponse(cr, bytes_read);
      
      //check if the cache is obsolete
      string deadline = client_response.FindHeader("Expires");
      char* time1 = (char*)malloc(deadline.length());
      strcpy(time1, deadline.c_str());
      
      //find date it was last modified
      string modified = client_response.FindHeader("Last-Modified");
      char* time2 = (char*)malloc(modified.length());
      strcpy(time2, modified.c_str());
      
      //parse out the time
      struct tm time3;
      strptime(time1, "%a, %d %b %Y %H:%M:%S GMT", &time3);
      time_t c_time1;
      c_time1 = timegm(&time3);
      time_t present_time = time(0);
      struct tm* curr_time1 = gmtime(&present_time);
      time_t curr_time2;
      curr_time2 = timegm(curr_time1);
      
      //if time has expired, we need to resend request to remote server
      if(c_time1 < curr_time2)
        {
          client.AddHeader("If-Modified-Since", time2);
          total_size = client.GetTotalLength();
          req = (char*)malloc(total_size);
          client.FormatRequest(req);
          conditional = true;
        }
      else
        {
          int send_client = send(client_fd, cr, bytes_read, 0);
          if(send_client == -1)
            {
              fprintf(stderr, "Error with send\n");
              return -1;
            }
          return 1;
        }
    }

  //If the cache doesn't exist or time has expired

  //start a connection to remote server
  struct addrinfo hints, *res;
  int rm_fd;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  int addr;
  addr = getaddrinfo(host.c_str(), port, &hints, &res);
  if(addr != 0)
    {
      fprintf(stderr, "Error with getaddrinfo\n");
      return -1;
    }

  //open up a new socket
  struct addrinfo *p;
  for(p = res; p != NULL; p=p->ai_next)
    {
      rm_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if(rm_fd < 0)
        {
          perror("Could not open socket");
          continue;
        }
      int con_status;
      con_status = connect(rm_fd, p->ai_addr, p->ai_addrlen);
      if(con_status < 0)
        {
          close(rm_fd);
          perror("Could not connect");
          continue;
        }
      break;
    }
  if(p == NULL)
    {
      fprintf(stderr, "Failed to connect remotely\n");
      return -1;
    }
  freeaddrinfo(res);


  //send the request to the remote server
  int sent1;
  sent1 = 0;
  while(sent1 != int(total_size))
    {
      int t;
      t = send(rm_fd, req, total_size, 0);
      if(t == -1)
        {
          fprintf(stderr, "Error sending to remote\n");
          return -1;
        }
      sent1 += t;
    } 


  //wait for the response back from the server
  string result;
  for(;;)
    {
      char rem_buf[256];
      int num_recv = recv(rm_fd, rem_buf, sizeof(rem_buf), 0);
      if(num_recv < 0)
        {
          fprintf(stderr, "Error with receive\n");
          return -1;
        }
      else if(num_recv == 0)
        {
          break;
        }
      result.append(rem_buf, num_recv);
    }

  //now check to see if the response was for a conditional request or not
  if(conditional)
    {
      //read from cache
      size_t max_size2;
      max_size2 = 100000;
      char* new_cr = (char*)malloc(max_size2);
      ssize_t new_bytes_read = read(c_fd, new_cr, max_size2);
      if(new_bytes_read == -1)
        {
          fprintf(stderr, "Reading error from cache\n");
          return -1;
        }
      HttpResponse rmsrv;
      rmsrv.ParseResponse(result.c_str(), result.length());
      string rmsrv_status;
      rmsrv_status = rmsrv.GetStatusCode();
      if(rmsrv_status.compare("304") == 0)
        {
          int final;
          final = 0;
          while(final != int(new_bytes_read))
            {
              int x;
              x = send(client_fd, new_cr, new_bytes_read, 0);
              if(final == -1)
                {
                  fprintf(stderr, "Send error\n");
                  return -1;
                }
              final += x;
            }
          return 1;
        }
      //DRAGON: update the cache and send new version to client                                                        
    }

  //create a new file for the new response
  c_fd = open(cache_file.c_str(), O_CREAT | O_WRONLY, 0666);
  if(c_fd == -1)
    {
      fprintf(stderr, "Error with opening file\n");
      return -1;
    }

  ssize_t new_write = write(c_fd, result.c_str(), result.length());
  if(new_write == -1)
    {
      fprintf(stderr, "Error with writing\n");
      return -1;
    }

  //send the response back to the client
  int final;
  final = 0;
  while(final != int(result.length()))
    {
      int g;
      g = send(client_fd, result.c_str(), result.length()+1, 0);
      if(g == -1)
        {
          fprintf(stderr, "Error sending back to client\n");
          return -1;
        }
      final += g;
    }

  close(rm_fd);
  close(c_fd);

  return 1;
}

int main (int argc, char *argv[])
{
  //initialize the variables and the port
  int n_connections;
  n_connections = 0;

  //setting up the server socket to start listening                                                                    
  int socket_fd;
  struct addrinfo x;
  struct addrinfo *y;
  memset(&x, 0, sizeof(struct addrinfo));
  x.ai_family = AF_INET;
  x.ai_socktype = SOCK_STREAM;
  x.ai_flags = AI_PASSIVE;
  int status = getaddrinfo(NULL, PROXY_PORT, &x, &y);
  if(status != 0)
    {
      fprintf(stderr, "Failure with getaddrinfo\n");
      return -1;
    }

  //loop through and bind with a socket
  struct addrinfo *z;
  for(z = y; z != NULL; z = z->ai_next)
    {
      socket_fd = socket(z->ai_family, z->ai_socktype, z->ai_protocol);
      if(socket_fd < 0)
        {
          perror("Something wrong with socket()");
          continue;
        }
      
      //set socket options                                                                                             
      int flag;
      flag = 1;
      int options;
      options = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR,(char*)&flag, sizeof(int));
      if(options < 0)
        {
          perror("Something wrong with setsockopt()");
          continue;
        }
      int bind_status;
      bind_status = bind(socket_fd, z->ai_addr, z->ai_addrlen);
      if(bind_status != 0)
        {
          perror("Something wrong with bind()");
          continue;
        }
      break;
    }

  //check to se that a socket has been made
  if(z == NULL || socket_fd == -1)
    {
      fprintf(stderr, "Socket error\n");
      return -1;
    }
  freeaddrinfo(y);

  //start listening to the port
  if(listen(socket_fd, 20) == -1)
    {
      fprintf(stderr, "Error with listening\n");
      return -1;
    }

  //Main loop where we will accept connections
  while(1)  
    {
      if(n_connections < MAX_CONNECTIONS)
        {
          //Accept the client connection
          struct sockaddr_in client_addr;
          socklen_t client_size;
          client_size = sizeof(client_addr);
          int client_fd;
          client_fd = accept(socket_fd, (struct sockaddr*)(&client_addr), &client_size);
          if(client_fd == -1)
            {
              perror("Errot with accept");
              continue; //DRAGON: maybe we can change this to exit() 
            }

          //After we accept, increase the number of current connections
          n_connections++;
          //Fork off a new process for each incoming connection
          pid_t pid;
          pid = fork();
          if(pid < 0)
            {
              fprintf(stderr, "Failure to fork a new process\n");
              continue;
            }
          else if(pid == 0)
            {
              //child process: serve the client's request and exit with the status returned
              close(socket_fd);
              int result;
              result = process_client(client_fd);
              _exit(result);
            }
          else
            {
              //parent process: wait for any child to return, and decrement our connection 
              //count 
              close(client_fd);
              int status;
              if(waitpid(WAIT_ANY, &status, WNOHANG) < 1)
                {
                  continue;
                }
              else
                {
                  n_connections--;
                }
            }
        }
      else
        {
          //if we already have the max number of processes, keep looping 
          continue;
        }
    }
  

  return 0;
}
