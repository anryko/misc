#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "libtcpsrv.h"

#define fd_slot(t,fd) (t->slots + (fd * t->p.sz))

/* signals that we'll accept synchronously via signalfd */
static int sigs[] = {SIGHUP,SIGTERM,SIGINT,SIGQUIT,SIGALRM};

static int setup_listener(tcpsrv_t *t) {
  int rc = -1, one=1;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    fprintf(stderr,"socket: %s\n", strerror(errno));
    goto done;
  }

  /**********************************************************
   * internet socket address structure: our address and port
   *********************************************************/
  struct sockaddr_in sin;
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = t->p.addr;
  sin.sin_port = htons(t->p.port);

  /**********************************************************
   * bind socket to address and port 
   *********************************************************/
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (bind(fd, (struct sockaddr*)&sin, sizeof(sin)) == -1) {
    fprintf(stderr,"bind: %s\n", strerror(errno));
    goto done;
  }

  /**********************************************************
   * put socket into listening state
   *********************************************************/
  if (listen(fd,1) == -1) {
    fprintf(stderr,"listen: %s\n", strerror(errno));
    goto done;
  }

  t->fd = fd;
  rc=0;

 done:
  if ((rc < 0) && (fd != -1)) close(fd);
  return rc;
}

static int add_epoll(int epoll_fd, int events, int fd) {
  int rc;
  struct epoll_event ev;
  memset(&ev,0,sizeof(ev)); // placate valgrind
  ev.events = events;
  ev.data.fd= fd;
  rc = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
  if (rc == -1) {
    fprintf(stderr,"epoll_ctl: %s\n", strerror(errno));
  }
  return rc;
}

static int mod_epoll(int epoll_fd, int events, int fd) {
  int rc;
  struct epoll_event ev;
  memset(&ev,0,sizeof(ev)); // placate valgrind
  ev.events = events;
  ev.data.fd= fd;
  rc = epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
  if (rc == -1) {
    fprintf(stderr,"epoll_ctl: %s\n", strerror(errno));
  }
  return rc;
}

static int del_epoll(int epoll_fd, int fd) {
  int rc;
  struct epoll_event ev;
  rc = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ev);
  if (rc == -1) {
    fprintf(stderr,"epoll_ctl: %s\n", strerror(errno));
  }
  return rc;
}

/* send a byte to each worker on the control pipe */
static void send_workers(tcpsrv_t *t, char op) {
  int n;
  for(n=0; n < t->p.nthread; n++) {
    write(t->tc[n].pipe_fd[1], &op, sizeof(op));
  }
}

static void periodic(tcpsrv_t *t) {
  send_workers(t,WORKER_PING);
}

/* drain is a built-in default used if app has no on_data callback */
static void drain(void *slot, int fd, void *data, int *flags) {
  int rc, pos, *fp;
  char buf[1024];

  rc = read(fd, buf, sizeof(buf));
  switch(rc) { 
    default: fprintf(stderr,"received %d bytes\n", rc);         break;
    case  0: fprintf(stderr,"fd %d closed\n", fd);              break;
    case -1: fprintf(stderr, "recv: %s\n", strerror(errno));    break;
  }

  if (rc == 0) {
    *flags |= TCPSRV_DO_CLOSE;
  }
}

static void accept_client(tcpsrv_t *t) { // always in main thread
  int fd;
  struct sockaddr_in in;
  socklen_t sz = sizeof(in);

  fd = accept(t->fd,(struct sockaddr*)&in, &sz);
  if (fd == -1) {
    fprintf(stderr,"accept: %s\n", strerror(errno)); 
    goto done;
  }

  if (t->p.verbose && (sizeof(in)==sz)) {
    fprintf(stderr,"connection fd %d from %s:%d\n", fd,
    inet_ntoa(in.sin_addr), (int)ntohs(in.sin_port));
  }

  if (fd > t->p.maxfd) {
    if (t->p.verbose) fprintf(stderr,"overload fd %d > %d\n", fd, t->p.maxfd);
    t->num_overloads++;
    close(fd);
    goto done;
  }

  /* pick a thread to own the connection */
  t->num_accepts++;
  int thread_idx = fd % t->p.nthread;
  char *slot = fd_slot(t,fd);

  /* if the app has an on-accept callback, invoke it. */ 
  int flags = TCPSRV_POLL_READ;
  if (t->p.on_accept) {
    t->p.on_accept(slot, fd, t->p.data, &flags); // TODO give remote IP etc
    if (flags & TCPSRV_DO_EXIT) t->shutdown=1;
    if (flags & TCPSRV_DO_CLOSE) {
      if (t->p.upon_close) t->p.upon_close(slot, fd, t->p.data);
      close(fd);
    }
    if (flags & (TCPSRV_DO_EXIT | TCPSRV_DO_CLOSE)) goto done;
  }

  /* hand all further I/O on this fd to the worker. */
  int events = 0;
  if (flags & TCPSRV_POLL_READ)  events |= EPOLLIN;
  if (flags & TCPSRV_POLL_WRITE) events |= EPOLLOUT;
  if (add_epoll(t->tc[thread_idx].epoll_fd, events, fd) == -1) { 
    fprintf(stderr,"can't give accepted connection to thread %d\n", thread_idx);
    close(fd); 
    t->shutdown=1;
  }

 done:
  return;
}

void *tcpsrv_init(tcpsrv_init_t *p) {
  int rc=-1,n;

  tcpsrv_t *t = calloc(1,sizeof(*t)); if (!t) goto done;
  t->p = *p; // struct copy
  if (t->p.on_data == NULL) t->p.on_data = drain;
  t->slots = calloc(p->maxfd+1, p->sz); 
  if (t->slots == NULL) goto done;
  if (p->slot_init) p->slot_init(t->slots, p->maxfd+1, p->data);
  t->th = calloc(p->nthread,sizeof(pthread_t));
  if (t->th == NULL) goto done;

  sigfillset(&t->all);  /* set of all signals */
  sigemptyset(&t->few); /* set of signals we accept */
  for(n=0; n < sizeof(sigs)/sizeof(*sigs); n++) sigaddset(&t->few, sigs[n]);

  /* create the signalfd for receiving signals */
  t->signal_fd = signalfd(-1, &t->few, 0);
  if (t->signal_fd == -1) {
    fprintf(stderr,"signalfd: %s\n", strerror(errno));
    goto done;
  }

  /* set up the epoll instance */
  t->epoll_fd = epoll_create(1); 
  if (t->epoll_fd == -1) {
    fprintf(stderr,"epoll: %s\n", strerror(errno));
    goto done;
  }

  /* create thread areas */
  t->tc = calloc(t->p.nthread,sizeof(tcpsrv_thread_t));
  if (t->tc == NULL) goto done;
  for(n=0; n < t->p.nthread; n++) {
    t->tc[n].thread_idx = n;
    t->tc[n].t = t;
    if (pipe(t->tc[n].pipe_fd) == -1) goto done;
    t->tc[n].epoll_fd = epoll_create(1);
    if (t->tc[n].epoll_fd == -1) {
      fprintf(stderr,"epoll: %s\n", strerror(errno));
      goto done;
    }
  }

  rc=0;

 done:
  if (rc < 0) {
    fprintf(stderr, "tcpsrv_init failed\n");
    if (t) {
      if (t->slots) free(t->slots);
      if (t->th) free(t->th);
      if (t->signal_fd > 0) close(t->signal_fd);
      if (t->epoll_fd > 0) close(t->epoll_fd);
      if (t->tc) {
        for(n=0; n < t->p.nthread; n++) {
          if (t->tc[n].pipe_fd[0] > 0) close(t->tc[n].pipe_fd[0]);
          if (t->tc[n].pipe_fd[1] > 0) close(t->tc[n].pipe_fd[1]);
          if (t->tc[n].epoll_fd > 0) close(t->tc[n].epoll_fd);
        }
        free(t->tc);
      }
      free(t);
      t=NULL;
    }
  }
  return t;
}

static void *worker(void *data) {
  tcpsrv_thread_t *tc = (tcpsrv_thread_t*)data;
  int thread_idx = tc->thread_idx;
  struct epoll_event ev;
  tcpsrv_t *t = tc->t;
  void *rv=NULL;
  char op;

  if (t->p.verbose) fprintf(stderr,"thread %d starting\n", thread_idx);

  /* block all signals */
  sigset_t all;
  sigfillset(&all);
  pthread_sigmask(SIG_BLOCK, &all, NULL);

  /* listen to parent */
  if (add_epoll(tc->epoll_fd, EPOLLIN, tc->pipe_fd[0])) goto done; 

  /* event loop */
  while (epoll_wait(tc->epoll_fd, &ev, 1, -1) > 0) {

    if (t->p.verbose) {
      fprintf(stderr,"thread %d %s %s fd %d\n", thread_idx, 
        (ev.events & EPOLLIN ) ? "IN " : "   ", 
        (ev.events & EPOLLOUT) ? "OUT" : "   ", ev.data.fd);
    }

    /* is I/O from the from main thread on the control pipe? */ 
    if (ev.data.fd == tc->pipe_fd[0]) { 
      if (read(tc->pipe_fd[0],&op,sizeof(op)) != sizeof(op)) goto done;
      fprintf(stderr,"> thread %d: '%c' from main thread\n", thread_idx, op);
      if (op == WORKER_SHUTDOWN) goto done;
      continue;
    }

    /* regular I/O. */
    char *slot = fd_slot(t,ev.data.fd);
    int flags = 0;
    if (ev.events & EPOLLIN)  flags |= TCPSRV_CAN_READ;
    if (ev.events & EPOLLOUT) flags |= TCPSRV_CAN_WRITE;
    t->p.on_data(slot, ev.data.fd, t->p.data, &flags); 

    /* did app set terminal condition or close fd? */
    if (flags & TCPSRV_DO_EXIT) t->shutdown=1; // main checks at @1hz
    if (flags & TCPSRV_DO_CLOSE) {
      if (t->p.upon_close) t->p.upon_close(slot, ev.data.fd, t->p.data);
      close(ev.data.fd);
    }
    if (flags & (TCPSRV_DO_EXIT | TCPSRV_DO_CLOSE)) continue;

    /* did app modify poll condition? (set neither bit to keep current poll) */
    if (flags & (TCPSRV_POLL_READ | TCPSRV_POLL_WRITE)) {
      int events = 0;
      if (flags & TCPSRV_POLL_READ)  events |= EPOLLIN;
      if (flags & TCPSRV_POLL_WRITE) events |= EPOLLOUT;
      if (mod_epoll(tc->epoll_fd, events, ev.data.fd)) goto done;
    }
  }

 done:
  if (t->p.verbose) fprintf(stderr,"thread %d exiting\n", thread_idx);
  return rv;
}

void handle_signal(tcpsrv_t *t) {
  struct signalfd_siginfo info;
  int rc = -1;

  if (read(t->signal_fd, &info, sizeof(info)) != sizeof(info)) {
    fprintf(stderr,"failed to read signal fd buffer\n");
    goto done;
  }

  switch (info.ssi_signo) {
    case SIGALRM: 
      if ((++t->ticks % 10) == 0) periodic(t); 
      alarm(1); 
      break;
    default:  /* exit */
      fprintf(stderr,"got signal %d\n", info.ssi_signo);  
      goto done;
  }
  rc = 0;

 done:
  if (rc < 0) t->shutdown=1;
}

int tcpsrv_run(void *_t) {
  tcpsrv_t *t = (tcpsrv_t*)_t;
  struct epoll_event ev;
  int rc=-1,n;

  pthread_sigmask(SIG_SETMASK, &t->all, NULL);                  // block all signals 
  if (setup_listener(t)) goto done; 
  if (add_epoll(t->epoll_fd, EPOLLIN, t->fd))        goto done; // listening socket
  if (add_epoll(t->epoll_fd, EPOLLIN, t->signal_fd)) goto done; // signal socket


  for(n=0; n < t->p.nthread; n++) {
    void *data = &t->tc[n];
    pthread_create(&t->th[n],NULL,worker,data);
  }

  alarm(1);
  while (epoll_wait(t->epoll_fd, &ev, 1, -1) > 0) {

    assert(ev.events & EPOLLIN);
    if (t->p.verbose) fprintf(stderr,"POLLIN fd %d\n", ev.data.fd);

    if      (ev.data.fd == t->fd)        accept_client(t);
    else if (ev.data.fd == t->signal_fd) handle_signal(t);

    if (t->shutdown) {
      send_workers(t,WORKER_SHUTDOWN);
      rc = 0;
      goto done;
    }
  }

 done:
  return rc;
}


void tcpsrv_fini(void *_t) {
  int n,rc;
  tcpsrv_t *t = (tcpsrv_t*)_t;
  for(n=0; n < t->p.nthread; n++) { // wait for thread term 
    rc=pthread_join(t->th[n],NULL);
    if (rc == -1) fprintf(stderr,"pthread_join %d: %s\n",n,strerror(errno));
    else if (t->p.verbose) fprintf(stderr,"thread %d exited\n",n);
  }
  if (t->p.slot_fini) t->p.slot_fini(t->slots, t->p.maxfd+1, t->p.data);
  free(t->slots);
  close(t->signal_fd);
  close(t->epoll_fd);
  for(n=0; n < t->p.nthread; n++) {
    close(t->tc[n].pipe_fd[0]);
    close(t->tc[n].pipe_fd[1]);
    close(t->tc[n].epoll_fd);
  }
  free(t->th);
  free(t->tc);
  free(t);
}
