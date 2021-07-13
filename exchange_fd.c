/*
 *
 * ISC License
 * 
 * Copyright (c) Ahmed Bashandy
 * 
 * Permission to use, copy, modify, and/or distribute this software 
 * for any purpose with or without fee is hereby granted, provided 
 * that the above copyright notice and this permission notice appear 
 * in all copies. 
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(s) DISCLAIM ALL 
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED 
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE 
 * AUTHOR(s) BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, 
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN      
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 *
 * I.E USE IT AS YOU WISH BUT DO OT BLAME ANYONE FOR FOR ANYTHING
 *
 * What is this code
 * ================
 * This is a test code. Not production ready
 * The objective of this code are
 * - to test whether we can send the file descritpor created by eventfd() to
 *   another process and use it in that other process. 
 *   WE will use unix socket and use ancilliary SCM_RIGHTS message
 * - To test if a SINGLE "write()" to a file descriptor created by  eventfd()
 *   can be used to wakeup multiple processes that received that file
 *   descriptor using SCM_RIGHTS message and each is waiting on this received
 *   file descriptor using epoll_..()
 * - The only test that we did successfully is using epoll_...() on the received
 *   file descriptor that was created by ANOTHER PROCESS using eventfd()
 *
 * How to build
 * ============
 *   gcc -o exchange_fd exchange_fd.c -Wall -Wextra
 * if you want to debug it or decode a core dump
 *   gcc -g -O0 -o exchange_fd exchange_fd.c -Wall -Wextra
 *
 *
 * How to use
 * ============
 * - Build as specified above
 * - Start the application in server mode using 
 *     ./exchange_fd -s
 *   - The server is waiting for one or more clients to connect
 *   - The default number of clients is DEFAULT_NUM_CLIENTS client.
 *   - to change the number of clients, add "-c <n>"
 *
 * - In another window, start the client(s) by calling
 *     ./exchange_fd
 *   - The number of clients that are started MUST be the same number expected
 *     by the server
 *   - If, when you started the server you used "-c" to specify more or less
 *     clients than DEFAULT_NUM_CLIENTS, then you MUST start as many client
 *     processes as you specoified when using the "-c"
 *  
 * - Successful operation
 *   - Server opens the socket and sets it to listen mode
 *   - Server creates the eventfd() file descriptor
 *   - Server will wait till the expected number of clients connect
 *   - Clients connect to the server  and wait to receive the SCM_RIGHTS
 *     message by calling recvmsg
 *   - When all clients connect to the server, the server sends the SCM_RIGHTS
 *     messages containing the eventfd to each client
 *   - When a client receives the SCM_RIGHTS message, it extracts the eventfd
 *     file descriptor and uses epoll() to wait on the received eventfd() file
 *     descriptor 
 *   - The sever then sleeps for few   seconds to make sure that all clients
 *     are waiting on eventfd() file descriptor that it sent in the SCM_RIGHTS
 *      message
 *   - The server makes a SINGLE write() call to the eventfd() file descriptor
 *   - If every thing is successful, then ALL CLIENTS are awakened from the
 *     epoll() call that is waiting on the file descriptor
 */

#include <sys/types.h> /* AF_UNIX socket */
#include <sys/socket.h>/* AF_UNIX socket */
/*#include <sys/un.h>*/    /* AF_UNIX socket but does NOT have "UNIX_PATH_MAX"*/
#include <linux/un.h>    /* AF_UNIX socket and UNIX_PATH_MAX */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <unistd.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <sys/eventfd.h>
#include <poll.h> /* For poll() */
#include <sys/epoll.h> /* For epoll() */

#define COLOR_NORMAL   "\x1B[0m"
#define COLOR_RED   "\x1B[31m"
#define COLOR_GREEN   "\x1B[32m"
#define COLOR_YELLOW   "\x1B[33m"
#define COLOR_BLUE   "\x1B[34m"
#define COLOR_MAGENTA   "\x1B[35m"
#define COLOR_CYAN   "\x1B[36m"
#define COLOR_WIHTE   "\x1B[37m"
#define COLOR_RESET "\x1B[0m"


/*
 * Default named socket (AF_UNIX) path
 */
#define DEFAULT_SOCK_PATH "/tmp/test_named_socket_path"


/*
 * max and default values
 */
#define MAX_NUM_CLIENTS (4)
#define MAX_NUM_MESSAGES (1024)
#define DEFAULT_NUM_CLIENTS (1)
#define DEFAULT_NUM_MESSAGES (1)

/* Need these to be public so that the signal handler can use them */
static char *sock_path = DEFAULT_SOCK_PATH;
static int server_accept_fd[MAX_NUM_CLIENTS];
static int sock_fd;
static int sent_eventfd_fd = -1;
static int received_eventfd_fd = -1;
static int is_server = false;
static uint32_t num_messages = DEFAULT_NUM_MESSAGES;
static uint32_t num_clients = DEFAULT_NUM_CLIENTS;
static uint32_t num_accepted_clients = 0;
static int eventfd_flags = EFD_NONBLOCK;



/* Macro to print error */
#define PRINT_ERR(str, param...)                                        \
  do {                                                                  \
    print_debug("%s %d: " str, __FUNCTION__, __LINE__, ##param);        \
  } while (false);
#define PRINT_INFO(str, param...)                                       \
  do {                                                                  \
    print_debug("%s %d: " str, __FUNCTION__, __LINE__, ##param);        \
  } while (false);
#define PRINT_DEBUG(str, param...)                                      \
  do {                                                                  \
    print_debug("%s %d: " str, __FUNCTION__, __LINE__, ##param);        \
  } while (false);


/* Print error in red color */
__attribute__ ((format (printf, 1, 2), unused))
static void print_error(char *string, ...) 
{
  va_list args;
  va_start(args, string);
  fprintf(stderr, COLOR_RED);
  vfprintf(stderr, string, args);
  fprintf(stderr, COLOR_RESET);
  va_end(args);
}

/* Print error in info or debug in normal color */
__attribute__ ((format (printf, 1, 2), unused))
static void print_debug(char *string, ...)
{
  va_list args;
  va_start(args, string);
  fprintf(stdout, COLOR_RESET);
  vfprintf(stdout, string, args);
  va_end(args);
}




/*
 * Close server side sockets and file descriptor
 */
static void
close_server_fds(void)
{
  uint32_t i;
  for (i = 0; i < num_clients; i++) {
    if (server_accept_fd[i] > 0) {
      close (server_accept_fd[i]);
    } else {
      PRINT_ERR("Server socket %u NOT opened\n", i);
    }
  }
  if (unlink(sock_path)) {
    if (errno != ENOENT) {
      PRINT_ERR("Cannot unlink sock_path %s: %d %s\n",
                sock_path,
                errno, strerror(errno));
      exit(EXIT_FAILURE);
    } else {
      PRINT_ERR("sock_path %s does NOT exist\n",
                sock_path);
    }
  }
  if (sent_eventfd_fd > 0) {
    close(sent_eventfd_fd);
  } else {
    PRINT_DEBUG("eventfd() NOT opened\n");
  }
}

/*
 * Close client side sockets and file descriptor
 */
static void __attribute__ ((unused))
close_client_fds(void)
{
  if (sock_fd > 0) {
    close(sock_fd);
  } else {
    PRINT_ERR("sockets AF_UNIX %d NOT opened\n",
              sock_fd);
  }
}


/* CTRL^C handler */
static void
signal_handler(int s) 
{
  PRINT_DEBUG("Caught signal %d\n",s);

  PRINT_DEBUG( "Going to unlink %s then server socket %d and %u accepted "
               "sockets out of expected %u sockets, "
               "client socket  %d "
               "and eventfd() %d\n",
               sock_path,
               sock_fd,
               num_accepted_clients,               
               num_clients,
               sock_fd,
               sent_eventfd_fd);
  if (is_server) {
    close_server_fds();      
  } else {
    close_client_fds();    
  } 
           
  exit(EXIT_SUCCESS); 
}


static void
print_usage (const char *progname)
{
  fprintf (stdout,
           "usage: %s [-s] [-c <num_clients>] [-p <sockpath>] [-n <num_messages>] \n"
           "\t-s Server mode instead of the default client mode\n"
           "\t-p <sockpath>: user another socket path instead of default'%s'\n" 
           "\t-c <num_clients>: Specify number of clients instead of default %u\n"
           "\t-e eventfd() is called with 'EFD_SEMAPHORE' flag\n"
           "\t-n <num_messages>  Send/Receive messages. Default %u\n",
           progname,
           DEFAULT_SOCK_PATH,
           DEFAULT_NUM_CLIENTS,
           DEFAULT_NUM_MESSAGES);
}



  /*
   * bind an open unix socket to sockpath
   */
static void
bind_to_sock_path(int sock_fd)
{
  struct sockaddr_un sockaddr;
  sockaddr.sun_family = AF_UNIX;
  strcpy(sockaddr.sun_path, sock_path);
  socklen_t len = strlen(sockaddr.sun_path) + sizeof(sockaddr.sun_family);
  /*
   * delete the sockpath
   */
  if (unlink(sockaddr.sun_path)) {
    if (errno != ENOENT) {
      PRINT_ERR("Cannot unlink sock_path %s: %d %s\n",
                sockaddr.sun_path,
                errno, strerror(errno));
      exit(EXIT_FAILURE);
    } else {
      PRINT_ERR("sock_path %s does NOT exist\n",
                sock_path);
    }
  }
    
  /* bind to the sockpath */
  if (bind(sock_fd, (struct sockaddr *)&sockaddr, len) == -1) {
    PRINT_ERR("Cannot bind socker %d to sockpath %s length %d: %d %s\n",
              sock_fd, sockaddr.sun_path, len,
              errno, strerror(errno));
    exit(EXIT_FAILURE);
  } else {
    PRINT_DEBUG("Succesfully bound socker %d to sockpath '%s' length %d\n",
              sock_fd, sockaddr.sun_path, len);
  }
}

/*
 * Prepares msg and cmsg for the "num_fs" file descriptors to be either sent or
 * received
 * It does NOT copy into or extract from the msg or cmsg the file descriptors
 */

static void
prepare_msg(int *fds,
            struct msghdr *msg,
            struct cmsghdr **cmsg_p,
            uint32_t num_fds)
{
  static char iobuf[1];
  struct cmsghdr *cmsg;
  static struct iovec io = {
    .iov_base = iobuf,
    .iov_len = sizeof(iobuf)
  };
  static union {         /* Ancillary data buffer, wrapped in a union
                            in order to ensure it is suitably aligned */
    char buf[0];
    struct cmsghdr align;
  } *u;

  size_t size = CMSG_SPACE(sizeof(*fds)*num_fds);
  u = malloc(size);
  if (!u) {
    PRINT_ERR("Cannot allocate %zu bytes for %u fds\n",
              CMSG_SPACE(sizeof(*fds)*num_fds), num_fds);
    exit(EXIT_FAILURE);
  }
  
         
  /* Setup the message */
  memset(msg, 0, sizeof(*msg));
  msg->msg_iov = &io;
  msg->msg_iovlen = 1;
  msg->msg_control = u->buf;
  msg->msg_controllen = size;

  /* Setup the control message */
  cmsg = CMSG_FIRSTHDR(msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(*fds) * num_fds);

  /* Return the populated value */
  *cmsg_p =  cmsg;
    
}


/*
 * receive the file descriptors
 */
static ssize_t
recev_fds(int sock, int *fds, uint32_t num_fds)
{
  struct msghdr msg;
  struct cmsghdr *cmsg = NULL;
  int *fdptr;

  /*
   * Prepare the structures
   */
  prepare_msg(fds,
              &msg,
              &cmsg,
              num_fds);
  
  PRINT_DEBUG("Going to wait to receive %u fds.\n", num_fds);
  ssize_t j = recvmsg(sock, &msg, 0); /* send the SCM_RIGHTS message */
  if (j < 0) {
    PRINT_ERR("FAILED recvmsg() to fd %d for %u fds "
              "msg.msg_iovlen=%u, msg.msg_controllen=%u "
              "cmsg->cmsg_level=%u "
              "cmsg->cmsg_type=%u "
              "cmsg->cmsg_len = %u"
              "\tmsg=  %p\n"
              "\tcmsg= %p\n"
              ": %d %s\n",
              sock, num_fds,
              (uint32_t)msg.msg_iovlen,
              (uint32_t)msg.msg_controllen,
              (uint32_t)cmsg->cmsg_level,
              (uint32_t)cmsg->cmsg_type,
              (uint32_t)cmsg->cmsg_len,
              &msg,
              cmsg,
              errno, strerror(errno));
  } else {
    fdptr = (int *) CMSG_DATA(cmsg);    /* get the location of the payload */ 
    memcpy(fds, fdptr, num_fds * sizeof(*fds)); /* Copy the fds from the msg */
    PRINT_DEBUG("SUCCESS recv_fds() sent %zd bytes to fd %d for %u fds\n "
              "\tmsg.msg_iovlen=%zu\n "
              "\tmsg.msg_controllen=%zu \n"
              "\tcmsg->cmsg_level=%u\n"
              "\tcmsg->cmsg_type=%u \n"
              "\tcmsg->cmsg_len=%zu\n"
              "\tmsg=  %p\n"
              "\tcmsg= %p\n"
              "\tfdptr=%p\n",
              j,
              sock, num_fds,
              msg.msg_iovlen,
              msg.msg_controllen,
              cmsg->cmsg_level,
              cmsg->cmsg_type,
              cmsg->cmsg_len,
              &msg,
              cmsg,
              fdptr);
  }
  /* Free mem we4 allocated in prepare(msg */
  free(msg.msg_control);
  return (j);
}

/*
 * send any array of file descriptors to the socket using SCM_RIGHTS buffer
 * over an AF_UNIX socket. 
 * Assume socket is already connected
 * Most of this came from "man -s3 cmsg"
 */
static ssize_t
send_fds(int sock, int *fds, uint32_t num_fds)
{
  struct msghdr msg;
  struct cmsghdr *cmsg = NULL;
  int *fdptr;

  /*
   * Prepare the structures
   */
  prepare_msg(fds,
              &msg,
              &cmsg,
              num_fds);

  /* Get a pointer to the payload */
  fdptr = (int *) CMSG_DATA(cmsg);
  /* Copy the fds into the msg */
  memcpy(fdptr, fds, num_fds * sizeof(*fds));
  ssize_t j = sendmsg(sock, &msg, 0); /* send the SCM_RIGHTS message */
  if (j < 0) {
    PRINT_ERR("FAILED send_fds() to fd %d for %u fds "
              "msg.msg_iovlen=%u, msg.msg_controllen=%u "
              "cmsg->cmsg_level=%u "
              "cmsg->cmsg_type=%u "
              "cmsg->cmsg_len = %u"
              ": %d %s\n",
              sock, num_fds,
              (uint32_t)msg.msg_iovlen,
              (uint32_t)msg.msg_controllen,
              (uint32_t)cmsg->cmsg_level,
              (uint32_t)cmsg->cmsg_type,
              (uint32_t)cmsg->cmsg_len,
              errno, strerror(errno));
  } else {
    PRINT_ERR("SUCCESS send_fds() sent %zd bytes to fd %d for %u fds\n "
              "\tmsg.msg_iovlen=%zu\n "
              "\tmsg.msg_controllen=%zu \n"
              "\tcmsg->cmsg_level=%u\n"
              "\tcmsg->cmsg_type=%u \n"
              "\tcmsg->cmsg_len=%zu\n"
              "\tmsg=  %p\n"
              "\tcmsg= %p\n"
              "\tfdptr=%p\n",
              j,
              sock, num_fds,
              msg.msg_iovlen,
              msg.msg_controllen,
              cmsg->cmsg_level,
              cmsg->cmsg_type,
              cmsg->cmsg_len,
              &msg,
              cmsg,
              fdptr);
    
  }
  /* Free mem we4 allocated in prepare(msg */
  free(msg.msg_control);
  return (j);
}

int
main (int    argc,
      char **argv)
{
  uint32_t i;
  int opt;
  struct sockaddr_un sockaddr_remote;
  socklen_t remote_len;
  
  while ((opt = getopt (argc, argv, "esp:c:n:")) != -1) {
    switch (opt)
      {
      case 's':
        is_server = true;
        break;
      case 'p':
        sock_path = optarg;
        if (strlen(sock_path) >= UNIX_PATH_MAX) {
          PRINT_ERR("\nSocket path '%s' len = %u. MUST be less than %u\n",
                    sock_path,
                    (uint32_t)strlen(sock_path), UNIX_PATH_MAX)
          exit (EXIT_FAILURE);
        }                    
        break;
      case 'n':
        if (1 != sscanf(optarg, "%u", &num_messages)) {
          PRINT_ERR("\nCannot read number of messages %s. \n", optarg);
          exit (EXIT_FAILURE);
        }
        if (num_messages >= MAX_NUM_MESSAGES || !num_messages) {
          PRINT_ERR("\nInvalid number of characters %s. "
                    "Must be between 0 and %u\n", optarg, MAX_NUM_MESSAGES);
          exit (EXIT_FAILURE);
        }
        break;
      case 'c':
        if (1 != sscanf(optarg, "%u", &num_clients)) {
          PRINT_ERR("\nCannot read number of clients %s. \n", optarg);
          exit (EXIT_FAILURE);
        }
        if (num_clients >= MAX_NUM_CLIENTS || !num_clients) {
          PRINT_ERR("\nInvalid number of clients %s. "
                    "Must be between 0 and %u\n", optarg, MAX_NUM_CLIENTS);
          exit (EXIT_FAILURE);
        }
        break;
      case 'e':
        eventfd_flags = EFD_NONBLOCK | EFD_SEMAPHORE;
        break;
      default: /* '?' */
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
  }
  
  if (optind < argc) {
    PRINT_ERR("\nInvalid number of arguments optind=%d argc=%d. ",
              optind, argc);
    exit (EXIT_FAILURE);
  }

  /* Setup handler for CTRL^C */
  struct sigaction sigIntHandler;
  sigIntHandler.sa_handler = signal_handler;
  sigemptyset(&sigIntHandler.sa_mask);
  sigIntHandler.sa_flags = 0;  
  if (sigaction(SIGINT, &sigIntHandler, NULL) != 0) {
    PRINT_ERR("Cannot setup signal handler: %d %s\n",
              errno, strerror(errno));
    exit(EXIT_FAILURE);
  }

  /*
   * Init the array of client file descriptors
   */
  for (i = 0; i < MAX_NUM_CLIENTS; i++) {
    server_accept_fd[i] = -1;
  }


  /* Create the AF_UNIX socket. */
  sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    PRINT_ERR("socket() failed: %d %s\n",
              errno, strerror(errno));
    exit(EXIT_FAILURE);
  }
  
  
  /*
   * Server mode
   */
  if (is_server) {
    /* Bind it to the sockpath */
    bind_to_sock_path(sock_fd);

    /*
     * Put socket in listen mode
     */
    if (listen(sock_fd, MAX_NUM_CLIENTS) == -1) {
      PRINT_ERR("listen() sock_fd %d backlong=%u "
                "failed : %d %s\n",
                sock_fd, MAX_NUM_CLIENTS, errno, strerror(errno));
      exit(EXIT_FAILURE);
    }
    
    /*
     * prepare the eventfd socket
     * - We will NOT use the semaphore mode because we want to the server to do
     *   one read to wakeup clients from poll a
     * - It has to be non-blocking to work with pol() or select()
     */
    sent_eventfd_fd = eventfd(0,eventfd_flags);
    if (sent_eventfd_fd == -1) {
      PRINT_ERR("eventfd() failed with flags 0x%x: %d %s\n",
                eventfd_flags,
                errno, strerror(errno));
      exit(EXIT_FAILURE);
    }


    PRINT_DEBUG("Going to wait on sock_fd %d for %u clients.\n",
                sock_fd, num_clients);
    /*
     * Loop until all clients connect
     * For each client connection, 
     * - send SCM_RIGHTS message containing the eventfd file descriptor
     */
    for (i = 0; i < num_clients; i++) {
      if ((server_accept_fd[i] = accept(sock_fd,
                                        (struct sockaddr *)&sockaddr_remote,
                                        &remote_len)) == -1) {
        PRINT_ERR("accept() client (%d) sock_fd %d remote_len=%u "
                  "failed : %d %s\n",
                  i, sock_fd, remote_len, errno, strerror(errno));
        exit(EXIT_FAILURE);
      }

      PRINT_DEBUG("Connected to client %d accept_fd %d , len %d path '%s'.\n",
                  i, server_accept_fd[i],remote_len, sockaddr_remote.sun_path);
      /*
       * Send the SCM_RIGHTS message containing the eventfd
       */
      if (send_fds(server_accept_fd[i],
                   &sent_eventfd_fd, 1) < 1) {
        PRINT_ERR("send_fds() to client %d for eventfd %d failed : %d %s\n", i,
                  sent_eventfd_fd,
                errno, strerror(errno));
        exit(EXIT_FAILURE);
      }
    }
    

    
    /* Sleep for 1 second after all clients connect to make sure that all
     * clients have called poll() or read to wait on the eventfd
     */
    PRINT_DEBUG("WAaiting to let clients wait on the eventfd() filedescriptor\n");
    sleep(3);
      
    
    /*
     * Send the number of messages while sleeping after each message to see
     * that eventfd works
     */
    for (i = 0; i < num_messages; i++) {
      uint64_t eventfd_write = 1234;
      if (write(sent_eventfd_fd, &eventfd_write, sizeof(eventfd_write)) !=
          sizeof(eventfd_write)) {
        PRINT_ERR("Cannot write message %d to evenfd %d %d %s\n", i,
                sent_eventfd_fd, errno, strerror(errno));
        /* exit(EXIT_FAILURE);*/
      } else {
        PRINT_DEBUG("Successfuly wrote message number %d to evenfd %d %d %s\n", i,
                sent_eventfd_fd, errno, strerror(errno));
      }
      sleep(1);
      
    }
  } else {
    sockaddr_remote.sun_family = AF_UNIX;
    strcpy(sockaddr_remote.sun_path, sock_path);
    remote_len =
      strlen(sockaddr_remote.sun_path) + sizeof(sockaddr_remote.sun_family);
    
    PRINT_DEBUG("going to connect to sock_path %s...\n",
                sockaddr_remote.sun_path);
    if (connect(sock_fd,
                (struct sockaddr *)&sockaddr_remote,
                remote_len)){
      PRINT_ERR("FAILED connect() sock_fd %d remote_len=%u: %d %s\n",
                sock_fd, remote_len, errno, strerror(errno));
      exit(EXIT_FAILURE);
    }
    
    PRINT_DEBUG("Connected to server sock_fd %d len %d path '%s'.\n",
                sock_fd,remote_len, sockaddr_remote.sun_path);
    
    if (recev_fds(sock_fd,
                  &received_eventfd_fd, 1) < 1) {
        PRINT_ERR("FAILED rec_fds() to client %d for eventfd %d: %d %s\n", i,
                  received_eventfd_fd,
                errno, strerror(errno));
        exit(EXIT_FAILURE);
    } else {
      PRINT_DEBUG("Received eventfd %d\n", received_eventfd_fd);
    }


    /*  epoll on the received eventfd */
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
      PRINT_ERR("epoll_create() failed for received_evenfd %d: %d %s\n",
                received_eventfd_fd, errno, strerror(errno));
        exit(EXIT_FAILURE);
    } else {
      PRINT_DEBUG("Successfully called epoll_create() epoll_fd=%d evenfd %d \n",
                  epoll_fd, received_eventfd_fd);
    }
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = received_eventfd_fd;
    int j = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, received_eventfd_fd, &event);
    if (j < 0) {
      PRINT_ERR("epoll_ctl failed for epoll_fd %d received_evenfd %d: %d %s\n",
                epoll_fd, received_eventfd_fd, errno, strerror(errno));
        exit(EXIT_FAILURE);
    } else {
      PRINT_DEBUG("Successfully called epoll_ctl() epoll_fd=%d evenfd %d \n",
                  epoll_fd, received_eventfd_fd);
    }
    PRINT_DEBUG("\nGoing to call epoll_wait() on epoll_fd=%d for evenfd %d.... \n",
                epoll_fd, received_eventfd_fd);
    j = epoll_wait(epoll_fd, &event, 1, -1);
    if (j < 0) {
      PRINT_ERR("epoll_wait failed for epoll_fd %d received_evenfd %d: %d %s\n",
                epoll_fd,  received_eventfd_fd, errno, strerror(errno));
        exit(EXIT_FAILURE);
    } else {
      PRINT_DEBUG("Successfully came out of epoll_wait epoll_fd=%d evenfd %d \n",
                  epoll_fd, received_eventfd_fd);
    }
    
    /* poll on the received eventfd 
     * The commented code below DOES NOT work. It is probably my mistake*/
    /*struct pollfd fds[2];
    fds[0].fd = received_eventfd_fd;
    fds[0].events = 0;
    int j = poll(fds, 1, -1);
    if (j < 0) {
      PRINT_ERR("Poll failed %d bytes from evenfd %d %d %s\n",
                j,
                received_eventfd_fd, errno, strerror(errno));
        exit(EXIT_FAILURE);
    } else {
      PRINT_DEBUG("Successfully came out of polling evenfd %d with %d\n",
                  received_eventfd_fd,j );
     }*/
    
    /* Wait on read from eventfd
     * The commented code below DOES NOT work. It is probably my mistake */
    /*uint64_t eventfd_read = 0;
    ssize_t j = read(received_eventfd_fd, &eventfd_read, sizeof(eventfd_read));
    if ( j!=
        sizeof(eventfd_read)) {
      PRINT_ERR("read invalid %zd bytes expecting %zu from evenfd %d %d %s\n",
                j,sizeof(eventfd_read),
                received_eventfd_fd, errno, strerror(errno));
        exit(EXIT_FAILURE);
    } else {
      PRINT_DEBUG("Successfully read %lu from received_evenfd %d\n",
                  eventfd_read, received_eventfd_fd );
                  }*/
    
    exit(EXIT_SUCCESS);
  }



  PRINT_DEBUG( "Going to unlink '%s' then server socket %d and %u accepted "
               "sockets out of expected %u sockets, "
               "client socket  %d\n",
               sock_path,
               sock_fd,
               num_accepted_clients,               
               num_clients,
               sock_fd);
  if (is_server) {
    close_server_fds();      
  } else {
    close_client_fds();    
  } 
  return 0;
}


 

            
