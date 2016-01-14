/**
 * File: efficient-server.cc
 * -------------------------
 * Simple application that relies on nonblocking
 * sockets and the suite of epoll functions to
 * implement event-driven web-server.
 */

#include <iostream>                    // for cerr
#include <map>                         // for map
#include <string>                      // for string
#include <unistd.h>                    // for close
#include <sys/epoll.h>                 // for epoll functions
#include <sys/types.h>                 // for accept
#include <sys/socket.h>                // for accept
#include <string.h>                    // for strerror
#include "server-socket.h"             // for createServerSocket
#include "non-blocking-utils.h"        // for setAsNonBlocking
using namespace std;

/**
 * Function: buildEvent
 * --------------------
 * Populates the only two fields of an epoll_event struct that
 * matter to us, and returns a copy of it to the call site.
 * The events flag is really a set of flags stating what
 * event types and behaviors we're interested in for a file
 * descrptor, and fd is the file descriptor to be registered.
 */
static struct epoll_event buildEvent(uint32_t events, int fd) {
  struct epoll_event event;
  event.events = events;
  event.data.fd = fd;
  return event;
}

/**
 * Function: acceptNewConnections
 * ------------------------------
 * Called when the kernel detects a read-oriented event
 * on the server socket (which is always in the watch set).
 *
 * In theory, many incoming requests (e.g. one or more) may have
 * accumulated in the time it took for the kernel to detect them,
 * and because the server socket was registered for edge-triggered
 * event notification (e.g. via the EPOLLET bit being set), we are
 * required to accepts each and every one of those incoming connections
 * before returning.  Each of those client connections need to made nonblocking
 * and then added to the watch set (in edge-triggered mode, initially for
 * read events, since we need to ingest the request header before we 
 * conern ourselves with writing back a response).
 */
static void acceptNewConnections(int watchSet, int serverSocket) {
  while (true) {
    int clientSocket = accept(serverSocket, NULL, NULL);
    if (clientSocket == -1) return;
    setAsNonBlocking(clientSocket);
    struct epoll_event info = 
      buildEvent(/* events = */ EPOLLIN | EPOLLET, /* fd = */ clientSocket);
    epoll_ctl(watchSet, EPOLL_CTL_ADD, clientSocket, &info);
  }
}

/**
 * Function: consumeAvailableData
 * ------------------------------
 * Reads in as much available data from the supplied client socket
 * until it either would have blocked, or until we have enough of the
 * response header (e.g. we've read up through a "\r\n\r\n") to respond.
 * Because the client sockets were registered for edge-triggered read events,
 * we need to consume *all* available data before returning, else epoll_wait
 * will never surface this client socket again.
 */
static const size_t kBufferSize = 512;
static const string kRequestHeaderEnding("\r\n\r\n");
static void consumeAvailableData(int watchSet, int clientSocket) {
  static map<int, string> requestLines; // tracks what's been read in thus far over each client socket
  while (true) {
    char buffer[kBufferSize];
    ssize_t count = read(clientSocket, buffer, kBufferSize);
    if (count <= 0) {
      if (count == -1 && errno == EWOULDBLOCK) return; // not done reading everything yet, so return and expect to be called later
      // if we get here, then count == -1 for some other reason. bail on connection, as it's borked
      close(clientSocket); // also removes clientSocket from all epoll sets
      break;
    }

    requestLines[clientSocket] += string(buffer, buffer + count);
    size_t pos = requestLines[clientSocket].rfind(kRequestHeaderEnding);
    if (pos != string::npos) { // yes? then we have the entire request header
      string requestLine = requestLines[clientSocket].substr(0, pos + kRequestHeaderEnding.size());
      cout << requestLine << flush;
      struct epoll_event info = // now register epoll interest in ability to *write* data
        buildEvent(/* events = */ EPOLLOUT | EPOLLET, /* fd = */ clientSocket);
      epoll_ctl(watchSet, EPOLL_CTL_MOD, clientSocket, &info); // MOD == modify
      break;
    }
  }  

  requestLines.erase(clientSocket);
}

/**
 * Function: publishResponse
 * -------------------------
 * Called on behalf of the specified client socket whenever the
 * kernel detects that we're able to write to it (and we're interested
 * in writing to it).
 *
 * The implementation of this *should* be more elaborate, but we can get away
 * with pretending the provided client socket is blocking instead of nonblocking
 * because the string we write to it is so incredibly short.  A more robust
 * implementation would check the return value to see how much of the payload
 * was actually accepted, keep calling write until -1 was returned, etc.
 */
static const string kResponseString("HTTP/1.0 200 OK\r\n\r\nThank you for your request! We're working on it.");
static void publishResponse(int clientSocket) {
  write(clientSocket, kResponseString.c_str(), kResponseString.size());
  close(clientSocket);
}

/**
 * Function: buildInitialWatchSet
 * ------------------------------
 * Creates an epoll watch set around the supplied server socket.  We
 * register an interested in being notified when the server socket is
 * available for read (and accept) operations via EPOLLIN, and we also
 * note that the event notificiations should be edge triggered (EPOLLET)
 * which means that we'd only like to be notified that data is available
 * to be read when the kernel is certain there is data.
 */
static const int kMaxEvents = 64;
static int buildInitialWatchSet(int serverSocket) {
  int watchSet = epoll_create(/* ignored parameter = */ kMaxEvents); // value is ignored nowadays, but must be positive
  struct epoll_event info = buildEvent(/* events = */ EPOLLIN | EPOLLET, /* fd = */ serverSocket);
  epoll_ctl(watchSet, EPOLL_CTL_ADD, serverSocket, &info);
  return watchSet;
}

/**
 * Function: runServer
 * -------------------
 * Converts the supplied server socket to be nonblocking, constructs
 * the initial watch set around the server socket, and then enter the
 * wait/response loop, blocking with each iteration until the kernel
 * detects something interesting happened to one or more of the
 * descriptors residing within the watch set.  The call to epoll_wait
 * is the only blocking system call in the entire (single-thread-of-execution)
 * web server.
 */
static void runServer(int serverSocket) {
  setAsNonBlocking(serverSocket);
  int watchSet = buildInitialWatchSet(serverSocket);
  struct epoll_event events[kMaxEvents];
  while (true) {
    int numEvents = epoll_wait(watchSet, events, kMaxEvents, /* timeout = */ -1);
    for (int i = 0; i < numEvents; i++) {
      if (events[i].data.fd == serverSocket) {
        acceptNewConnections(watchSet, serverSocket);
      } else if (events[i].events & EPOLLIN) { // we're still reading the client's request
        consumeAvailableData(watchSet, events[i].data.fd);
      } else if (events[i].events & EPOLLOUT) { // we've read in enough of the client's request to respond
        publishResponse(events[i].data.fd);
      }
    }
  }
}

/**
 * Function: main
 * --------------
 * Provides the entry point for the entire server.  The implementation
 * of main just passes the buck to runServer.
 */
static const unsigned short kDefaultPort = 33333;
static const int kDefaultBacklog = 128;
int main(int argc, char **argv) {
  int serverSocket = createServerSocket(kDefaultPort, kDefaultBacklog);
  if (serverSocket == kServerSocketFailure) {
    cerr << "Failed to start server.  Port " << kDefaultPort << " is probably already in use." << endl;
    return 1;
  }
  
  cout << "Server listening on port " << kDefaultPort << endl;
  runServer(serverSocket);
  return 0;
}
