c/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

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

#define MAX_CONNECTIONS "20"
#define PROXY_PORT "14886"
#define BACKLOG "20"

//using std::string;
using namespace std;

char* format_name(char* fd)
{
  size_t len = strlen(fd);
  char *temp = (char*)malloc(len*sizeof(char));
  for(size_t i = 0; i < len; i++)
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
  temp[len] = '\0';
  return temp;
}


int process_client(int client_fd, unsigned short int client_ip, uint32_t client_port)
{
  bool p_connection = false;
  bool conditional = false;
  string c_buffer = "";
  while(memmem(c_buffer.c_str(), c_buffer.length(), "\r\n\r\n", 4) == NULL)
    {
      char buf[1024];
      if(recv(client_fd, buf, sizeof(buf), 0) < 0)
        {
          perror("Error with recv");
          return -1;
        }
      c_buffer.append(buf);
    }
  HttpRequest client;
  try
    {
      client.ParseRequest(c_buffer.c_str(), c_buffer.length());
      string temp = "1.0";
      string temp2 = "1.1";
      if(temp.compare(client.GetVersion()) == 0)
        {
          cout << "HTTP/1.0" << endl;
          p_connection = false;
        }
      else if(temp2.compare(client.GetVersion()) == 0)
        {
          cout << "HTTP/1.1" << endl;
          p_connection = true;
        }
      else
        {
          fprintf(stderr, "Unsupported version of HTTP\n");
          return -1;
        }
    }
  catch (ParseException excp)
    {
      fprintf(stderr, "Exception: %s\n", excp.what());
      string result = "HTTP/1.0 ";
      char *cmp1 = "Request is not GET";
      char *cmp2 = "Only GET method is supported";
      if(strcmp(excp.what(), cmp1) == 0 || strcmp(excp.what(), cmp2) == 0)
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
  size_t total_size = client.GetTotalLength()+1;
  char *req = (char*)malloc(total_size);
  client.FormatRequest(req);
  
  char *host; 
  if(client.GetHost().length() == 0)
    {
      host = format_name(client.FindHeader("Host"));
    }
  else
    {
      host = format_name(client.GetHost().c_str());
    }
  char *path = format_name(client.GetPath().c_str());
  size_t host_len = strlen(host);
  size_t path_len = strlen(path);
  char port[6];
  sprintf(port, "%u", client.GetPort());
  char *cache_file = (char*)malloc(host_len + path_len);
  memcpy(cache_file, host, host_len);
  memcpy(cache_file+host_len, path, path_len);

  int c_fd = open(cache_file, O_RDONLY);
  if(c_fd != -1)
    {
      size_t max_size = 100000;
      char* cr = (char*)malloc(max_size*sizeof(char));
      ssize_t bytes_read = read(c_fd, cr, max_size);
      if(bytes_read == -1)
        {
          fprintf(stderr, "Reading error from cache\n");
          return -1;
        }
      HttpResponse client_response;
      client_response.ParseResponse(cr, bytes_read);
      string deadline = client_response.FindHeader("Expires");
      char* time1 = (char*)malloc(deadline.length()*sizeof(char));
      strcpy(time1,deadline.c_str());

      string modified = client_response.FindHeader("Last-Modified");
      char* time2 = (char*)malloc(modified.length()*sizeof(char));
      strcpy(time2, modified.c_str());

      struct tm time3;
      strptime(time1, "%a, %d %b %Y %H:%M:%S GMT", &time3);
      time_t c_time1 = timegm(&time3);
      time_t present_time = time(0);
      struct tm* curr_time1 = gmtime(&present_time);
      time_t curr_time2 = timegm(curr_time1);

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

      struct addrinfo hints, *res;
      int rm_fd;
      memset(&hints, 0, sizeof(struct addrinfo));
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      // const char* host = client.GetHost().c_str();
      int addr = getaddrinfo(host, port, &hints, &res);
      if(addr != 0)
        {
          fprintf(stderr, "Error with getaddrinfo\n");
          return -1;
        }

      struct addrinfo *p;
      for(p = res; p != NULL; p=p->ai_next)
        {
          rm_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
          if(rm_fd < 0)
            {
              perror("Could not open socket");
              continue;
            }
          int con_status = connect(rm_fd, p->ai_addr, p->ai_addrlen);
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
      
      int sent = send(rm_fd, req, total_size, 0);
      if(sent == -1)
        {
          fprintf(stderr, "Error sending to remote\n");
          return -1;
        }

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
      char* c_resp = result.c_str();
      
      if(conditional)
        {
          HttpResponse rmsrv;
          rmsrv.ParseResponse(c_resp, result.length());
          string rmsrv_status = rmsrv.GetStatusCode();
          if(rmsrv_status.compare("304") == 0)
            {
              int final = send(client_fd, cr, bytes_read, 0);
              if(final == -1)
                {
                  fprintf(stderr, "Send error\n");
                  return -1;
                }
              return 1;
            }
          //DRAGON: update the cache and send new version to client
        }          
      c_fd = open(cache_file, O_CREAT | O_WRONLY, 0666);
      if(c_fd == -1)
        {
          fprintf(stderr, "Error with opening file\n");
          return -1;
        }
      ssize_t new_write = write(c_fd, c_resp, result.length()+1, 0);
      if(new_write == -1)
        {
          fprintf(stderr, "Error with writing\n");
          return -1;
        }
      sent = send(client_fd, c_resp, result.length()+1, 0);
      if(sent == -1)
        {
          fprintf(stderr, "Error sending back to client\n");
          return -1;
        }

      close(rm_fd);
      close(c_fd);
      return 1;
}



int main (int argc, char *argv[])
{
  //initialize variables
  int n_connections = 0;
  //setting up the server socket to start listening
  int socket_fd;
  struct addrinfo x;
  struct addrinfo *y;
  memset(&x, 0, sizeof(struct addrinfo));
  x.ai_family = AF_UNSPEC;
  x.ai_socktype = SOCK_STREAM;
  x.ai_protocol = 0;
  x.ai_flags = AI_PASSIVE;
  int status = getaddrinfo(NULL, PROXY_PORT, &x, &y);
  if(status != 0)
    {
      fprintf(stderr, "Failure with getaddrinfo\n");
      return -1;
    }
  struct addrinfo *z;
  //loop through the results and connect to the first
  for(z = y; z != NULL; z = z->ai_next)
    {
      socket_fd = socket(z->ai_family, z->ai_socktype, z->ai_protocol);
      if(socket_fd < 0)
        {
          perror("Something wrong with socket()");
          continue;
        }
      //set socket options
      int flag = 1;
      int options = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR,(char*)&flag, sizeof(int));
      if(options < 0)
        {
          perror("Something wrong with setsockopt()");
          continue;
        }
      int bind = bind(socket_fd, z->ai_addr, z->ai_addrlen);
      if(bind != 0)
        {
          perror("Something wrong with bind()");
          continue;
        }
      break;
    }
  if(z == NULL || socket_fd == -1)
    {
      fprintf(stderr, "Socket error\n");
      return -1;
    }
  freeaddrinfo(y);
  
  //start listening to the connection
  if(listen(socket_fd, BACKLOG) == -1)
    {
      fprintf(stderr, "Error with listening\n");
      return -1;
    }
  //Main loop where we will accept connections
  while(1)//DRAGON: maybe set the limit for this at 20
    {
      if(n_connections <= MAX_CONNECTIONS)
        {
          struct sockaddr_in client_addr;
          socklen_t client_size = sizeof(client_addr);
          int client_fd = accept(socket_fd, (struct sockaddr*)(&client_addr), &client_size);
          if(client_fd == -1)
            {
              perror("Errot with accept");
              continue; //DRAGON: maybe we can change this to exit()
            }
          n_connections++;
          pid_t pid = fork();
          if(pid < 0)
            {
              fprintf(stderr, "Failure to fork a new process\n");
              return -1;
            }
          else if(pid == 0)
            {
              //child process: serve the client's request
              int result;
              result = process_client(client_fd, client_addr.sin_addr.s_addr, client_addr.sin_port);
              _exit(result);
            }
          else
            {
              //parent process: when child returns, decrease n_connections
              //need to use waitpid() somehow
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
          //if we already have the max number of processes
          continue;
        }
    }
      

  return 0;
}
