#include "TCP_Connection.h"

#include <arpa/inet.h>

#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#include <iostream>

#include <ifaddrs.h>


TCP_Connection::TCP_Connection(bool verbose) : verbose(verbose)
{
}

TCP_Connection::~TCP_Connection()
{
}

int 
TCP_Connection::open_port(unsigned short int port) {
  int listenSocket;
//  socklen_t clientAddressLength;
  struct sockaddr_in serverAddress;
  // Create socket for listening for client connection requests.

  listenSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (listenSocket < 0) {
    std::cerr << "cannot create listen socket" << std::endl;
    return -1;
  }
  
  // Bind listen socket to listen port.  First set various fields in
  // the serverAddress structure, then call bind().
  // htonl() and htons() convert long integers and short integers
  // (respectively) from host byte order (on x86 this is Least
  // Significant Byte first) to network byte order (Most Significant
  // Byte first).
  serverAddress.sin_family = AF_INET;
  serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
  serverAddress.sin_port = htons(port);
  
  if (bind(listenSocket,
           (struct sockaddr *) &serverAddress,
           sizeof(serverAddress)) < 0) {
    std::cerr << "cannot bind socket to port " << port << std::endl;
    return -1;
  }
  
  if (verbose)
    std::cout << "Port used is: " << port << std::endl;

  // Wait for connections from clients.
  // This is a non-blocking call; i.e., it registers this program with
  // the system as expecting connections on this socket, and then
  // this thread of execution continues on.
  listen(listenSocket, 1);
  
  return listenSocket;
}

unsigned int 
TCP_Connection::open_connection(int socket) {
  int connectSocket;
  socklen_t clientAddressLength;
  struct sockaddr_in clientAddress;

  if (verbose)
    std::cout << "Waiting for TCP connection on port " << socket << " ...\n";

  // Accept a connection with a client that is requesting one.  The
  // accept() call is a blocking call; i.e., this thread of
  // execution stops until a connection comes in.
  // connectSocket is a new socket that the system provides,
  // separate from listenSocket.  We *could* accept more
  // connections on listenSocket, before connectSocket is closed,
  // but this program doesn't do that.
  clientAddressLength = sizeof(clientAddress);
  connectSocket = accept(socket,
                         (struct sockaddr *) &clientAddress,
                         &clientAddressLength);
  if (connectSocket < 0) {
    std::cerr << "cannot accept connection ";
    return -1;
  }
  
  // Show the IP address of the client.
  // inet_ntoa() converts an IP address from binary form to the
  // standard "numbers and dots" notation.
  if (verbose)
    std::cout << "  connected to " << inet_ntoa(clientAddress.sin_addr);

  // Show the client's port number.
  // ntohs() converts a short int from network byte order (which is
  // Most Significant Byte first) to host byte order (which on x86,
  // for example, is Least Significant Byte first).
  if (verbose)
    std::cout << ":" << ntohs(clientAddress.sin_port) << "\n";

  return connectSocket;
}

int 
TCP_Connection::do_connect(const char *hostname, unsigned short int port) {
  int socketDescriptor;
  struct sockaddr_in serverAddress;
  struct hostent *hostInfo;

  // gethostbyname() takes a host name or ip address in "numbers and
  // dots" notation, and returns a pointer to a hostent structure,
  // which we'll need later.  It's not important for us what this
  // structure is actually composed of.
  hostInfo = gethostbyname(hostname);
  if (hostInfo == NULL) {
    std::cout << "problem interpreting host: " << hostname << "\n";
    return -1;
  }

  // Create a socket.  "AF_INET" means it will use the IPv4 protocol.
  // "SOCK_STREAM" means it will be a reliable connection (i.e., TCP;
  // for UDP use SOCK_DGRAM), and I'm not sure what the 0 for the last
  // parameter means, but it seems to work.
  socketDescriptor = socket(AF_INET, SOCK_STREAM, 0);
  if (socketDescriptor < 0) {
    std::cerr << "cannot create socket\n";
    return -1;
  }

  // Connect to server.  First we have to set some fields in the
  // serverAddress structure.  The system will assign me an arbitrary
  // local port that is not in use.
  serverAddress.sin_family = hostInfo->h_addrtype;
  memcpy((char *) &serverAddress.sin_addr.s_addr,
         hostInfo->h_addr_list[0], hostInfo->h_length);
  serverAddress.sin_port = htons(port);
        
  if (connect(socketDescriptor,
              (struct sockaddr *) &serverAddress,
              sizeof(serverAddress)) < 0) {
    std::cerr << "cannot connect\n";
    close(socketDescriptor);
    return -1;
  }

  return socketDescriptor;
}

int 
TCP_Connection::do_connect(UINT64 ip, unsigned short int port) {
  struct in_addr addr;
  addr.s_addr = ip;

  int socketDescriptor;
  struct sockaddr_in serverAddress;
  struct hostent *hostInfo;

  // gethostbyname() takes a host name or ip address in "numbers and
  // dots" notation, and returns a pointer to a hostent structure,
  // which we'll need later.  It's not important for us what this
  // structure is actually composed of.
  hostInfo = gethostbyaddr(&addr, sizeof(addr), AF_INET);
  if (hostInfo == NULL) {
    std::cout << "problem interpreting host: " << inet_ntoa(addr) << "\n";
    return -1;
  }
  
  // Create a socket.  "AF_INET" means it will use the IPv4 protocol.
  // "SOCK_STREAM" means it will be a reliable connection (i.e., TCP;
  // for UDP use SOCK_DGRAM), and I'm not sure what the 0 for the last
  // parameter means, but it seems to work.
  socketDescriptor = socket(AF_INET, SOCK_STREAM, 0);
  if (socketDescriptor < 0) {
    std::cerr << "cannot create socket\n";
    return -1;
  }

  // Connect to server.  First we have to set some fields in the
  // serverAddress structure.  The system will assign me an arbitrary
  // local port that is not in use.
  serverAddress.sin_family = hostInfo->h_addrtype;
  memcpy((char *) &serverAddress.sin_addr.s_addr,
         hostInfo->h_addr_list[0], hostInfo->h_length);
  serverAddress.sin_port = htons(port);
        
  if (connect(socketDescriptor,
              (struct sockaddr *) &serverAddress,
              sizeof(serverAddress)) < 0) {
    std::cerr << "cannot connect\n";
    close(socketDescriptor);
    return -1;
  }

  return socketDescriptor;
}

void 
TCP_Connection::get_ip_addresses(std::vector<std::string> &addr) {
  struct ifaddrs *ifa = NULL;
  
  if (getifaddrs (&ifa) < 0)
    {
      std::cout << "error in getifaddrs" << std::endl;;
      return;
    }

  for (; ifa; ifa = ifa->ifa_next) {
    char ip[ 15 ];
    socklen_t salen;

    if (ifa->ifa_addr->sa_family == AF_INET) {
      // IP v4
      salen = sizeof (struct sockaddr_in);
      if (getnameinfo (ifa->ifa_addr, salen,
                       ip, sizeof (ip), NULL, 0, NI_NUMERICHOST) < 0) {
          perror ("getnameinfo");
          continue;
        }
      addr.push_back(std::string(ip));
    }
  }

  freeifaddrs (ifa);
}

void 
TCP_Connection::get_ip_addresses(std::vector<UINT64>  &addr) {
  std::vector<std::string> tmp_addr;
  get_ip_addresses(tmp_addr);
  
  for (std::vector<std::string>::iterator it = tmp_addr.begin();
       it != tmp_addr.end(); it++) {
    struct in_addr address;
    inet_aton(it->c_str(), &address);
    addr.push_back(address.s_addr);
  }      
}
