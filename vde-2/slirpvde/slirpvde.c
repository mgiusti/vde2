/* Copyright 2003 Renzo Davoli 
 * Licensed under the GPL
 * Modified by Ludovico Gardenghi 2005
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <libgen.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <libslirp.h>
#include <pwd.h>
#include <grp.h>
#include <getopt.h>
#include <stdarg.h>
#include <fcntl.h>
#include <vde.h>

#define SWITCH_MAGIC 0xfeedface
#define BUFSIZE 2048
#define ETH_ALEN 6

int dhcpmgmt=0;
static char *pidfile = NULL;
static char pidfile_path[_POSIX_PATH_MAX];
int logok=0;
char *prog;
extern FILE *lfd;

void printlog(int priority, const char *format, ...)
{
	va_list arg;

	va_start (arg, format);

	if (logok)
		vsyslog(priority,format,arg);
	else {
		fprintf(stderr,"%s: ",prog);
		vfprintf(stderr,format,arg);
		fprintf(stderr,"\n");
	}
	va_end (arg);
}

enum request_type { REQ_NEW_CONTROL };

#define MAXDESCR 128

struct request_v3 {
  uint32_t magic;
  uint32_t version;
  enum request_type type;
  struct sockaddr_un sock;
	char description[MAXDESCR];
};

static struct sockaddr_un inpath;

static int send_fd(char *name, int fddata, struct sockaddr_un *datasock, int port, char *g, int m)
{
  int pid = getpid();
  struct request_v3 req;
  int fdctl;
	int gid;
	struct group *gs;
	struct passwd *callerpwd;

  struct sockaddr_un sock;

	callerpwd=getpwuid(getuid());
  if((fdctl = socket(AF_UNIX, SOCK_STREAM, 0)) < 0){
    perror("socket");
    exit(1);
  }

	if (name == NULL)
		name=VDESTDSOCK;
	else {
		char *split;
		if(name[strlen(name)-1] == ']' && (split=rindex(name,'[')) != NULL) {
			*split=0;
			split++;
			port=atoi(split);
			if (*name==0) name=VDESTDSOCK;
		}
	}
	
	sock.sun_family = AF_UNIX;
	snprintf(sock.sun_path, sizeof(sock.sun_path), "%s/ctl", name);
	if(connect(fdctl, (struct sockaddr *) &sock, sizeof(sock))){
		if (name == VDESTDSOCK) {
			name=VDETMPSOCK;
			snprintf(sock.sun_path, sizeof(sock.sun_path), "%s/ctl", name);
			if(connect(fdctl, (struct sockaddr *) &sock, sizeof(sock))){
				snprintf(sock.sun_path, sizeof(sock.sun_path), "%s", name);
				if(connect(fdctl, (struct sockaddr *) &sock, sizeof(sock))){
					perror("connect");
					exit(1);
				}
			}
		}
	}

	req.magic=SWITCH_MAGIC;
	req.version=3;
	req.type=REQ_NEW_CONTROL+(port << 8);
	req.sock.sun_family=AF_UNIX;

	/* First choice, return socket from the switch close to the control dir*/
	memset(req.sock.sun_path, 0, sizeof(req.sock.sun_path));
	sprintf(req.sock.sun_path, "%s.%05d-%02d", name, pid, 0);
	if(bind(fddata, (struct sockaddr *) &req.sock, sizeof(req.sock)) < 0){
		/* if it is not possible -> /tmp */
		memset(req.sock.sun_path, 0, sizeof(req.sock.sun_path));
		sprintf(req.sock.sun_path, "/tmp/vde.%05d-%02d", pid, 0);
		if(bind(fddata, (struct sockaddr *) &req.sock, sizeof(req.sock)) < 0) {
			perror("bind");
			exit(1);
		}
	}
						
	snprintf(req.description,MAXDESCR,"slirpvde user=%s PID=%d SOCK=%s",
			callerpwd->pw_name,pid,req.sock.sun_path);
	memcpy(&inpath,&req.sock,sizeof(req.sock));

	if (send(fdctl,&req,sizeof(req)-MAXDESCR+strlen(req.description),0) < 0) {
		perror("send");
		exit(1);
	}

	if (recv(fdctl,datasock,sizeof(struct sockaddr_un),0)<0) {
		perror("recv");
		exit(1);
	}

	if (g) {
		if ((gs=getgrnam(g)) == NULL)
			gid=atoi(g);
		else 
			gid=gs->gr_gid;
		chown(inpath.sun_path,-1,gid);
	}
	if (m>=0)
		chmod(inpath.sun_path,m);

	return fdctl;
}

static void save_pidfile()
{
	if(pidfile[0] != '/')
		strncat(pidfile_path, pidfile, PATH_MAX - strlen(pidfile_path));
	else
		strcpy(pidfile_path, pidfile);

	int fd = open(pidfile_path,
			O_WRONLY | O_CREAT | O_EXCL,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	FILE *f;

	if(fd == -1) {
		printlog(LOG_ERR, "Error in pidfile creation: %s", strerror(errno));
		exit(1);
	}

	if((f = fdopen(fd, "w")) == NULL) {
		printlog(LOG_ERR, "Error in FILE* construction: %s", strerror(errno));
		exit(1);
	}

	if(fprintf(f, "%ld\n", (long int)getpid()) <= 0) {
		printlog(LOG_ERR, "Error in writing pidfile");
		exit(1);
	}

	fclose(f);
}

static void cleanup(void)
{
	if((pidfile != NULL) && unlink(pidfile_path) < 0) {
		printlog(LOG_WARNING,"Couldn't remove pidfile '%s': %s", pidfile, strerror(errno));
	}
	unlink(inpath.sun_path);
}

static void sig_handler(int sig)
{
	  cleanup();
		  signal(sig, SIG_DFL);
			  kill(getpid(), sig);
}

static void setsighandlers()
{
	/* setting signal handlers.
	 * sets clean termination for SIGHUP, SIGINT and SIGTERM, and simply
	 * ignores all the others signals which could cause termination. */
	struct { int sig; const char *name; int ignore; } signals[] = {
		{ SIGHUP, "SIGHUP", 0 },
		{ SIGINT, "SIGINT", 0 },
		{ SIGPIPE, "SIGPIPE", 1 },
		{ SIGALRM, "SIGALRM", 1 },
		{ SIGTERM, "SIGTERM", 0 },
		{ SIGUSR1, "SIGUSR1", 1 },
		{ SIGUSR2, "SIGUSR2", 1 },
		{ SIGPROF, "SIGPROF", 1 },
		{ SIGVTALRM, "SIGVTALRM", 1 },
#ifdef VDE_LINUX
		{ SIGPOLL, "SIGPOLL", 1 },
		{ SIGSTKFLT, "SIGSTKFLT", 1 },
		{ SIGIO, "SIGIO", 1 },
		{ SIGPWR, "SIGPWR", 1 },
		{ SIGUNUSED, "SIGUNUSED", 1 },
#endif
#ifdef VDE_DARWIN
		{ SIGXCPU, "SIGXCPU", 1 },
		{ SIGXFSZ, "SIGXFSZ", 1 },
#endif
		{ 0, NULL, 0 }
	};

	int i;
	for(i = 0; signals[i].sig != 0; i++)
		if(signal(signals[i].sig,
					signals[i].ignore ? SIG_IGN : sig_handler) < 0)
			perror("Setting handler");
}

unsigned char bufin[BUFSIZE];

int slirp_can_output(void)
{
	return 1;
}

static int fddata;
static struct sockaddr_un dataout;

#if 0
#define convery2ascii(x) ((x)>=' ' && (x) <= '~')?(x):'.'
void dumppkt(const uint8_t *pkt, int pkt_len)
{
	register int i,j;
	printf("Packet dump len=%d\n",pkt_len);
	if (pkt_len == 0) 
		return;
	for (i=0;i<((pkt_len-1)/16)+1;i++) {
		for (j=0;j<16;j++)
			if (i*16+j > pkt_len)
				printf("   ");
			else
				printf("%02x ",pkt[i*16+j]);
		printf("  ");
		for (j=0;j<16;j++)
			if (i*16+j > pkt_len)
				printf(" ");
			else
				printf("%c",convery2ascii(pkt[i*16+j]));
		printf("\n");
	}
}
#endif

void slirp_output(const uint8_t *pkt, int pkt_len)
{
	/* slirp -> vde */
	//fprintf(stderr,"RX from slirp %d\n",pkt_len);
	//dumppkt(pkt,pkt_len);
	sendto(fddata,pkt,pkt_len,0,(struct sockaddr *) &dataout, sizeof(struct sockaddr_un));
}

struct redirx {
	u_int32_t inaddr;
	int start_port;
	int display;
	int screen;
	struct redirx *next;
};

struct redirtcp {
	u_int32_t inaddr;
	int port;
	int lport;
	struct redirtcp *next;
};

static struct redirtcp *parse_redir_tcp(struct redirtcp *head, char *buff)
{
	u_int32_t inaddr=0;
	int port=0;
	int lport=0;
	char *ipaddrstr=NULL;
	char *portstr=NULL;
	struct redirtcp *new;
			
	if ((ipaddrstr = strchr(buff, ':'))) {
		*ipaddrstr++ = 0;
		if (*ipaddrstr == 0) {
			fprintf(stderr,"redir TCP syntax error\n");
			return head;
		}
	}
	if ((portstr = strchr(ipaddrstr, ':'))) {
		*portstr++ = 0;
		if (*portstr == 0) {
			fprintf(stderr,"redir TCP syntax error\n");
			return head;
		}
	}

	sscanf(buff,"%d",&lport);
	sscanf(portstr,"%d",&port);
	if (ipaddrstr) 
		inaddr = inet_addr(ipaddrstr);

	if (!inaddr) {
		lprint(stderr,"TCP redirection error: an IP address must be specified\r\n");
		return head;
	}

	if ((new=malloc(sizeof(struct redirtcp)))==NULL)
		return head;
	else {
		new->inaddr=inaddr;
		new->port=port;
		new->lport=lport;
		new->next=head;
		return new;
	}
}

static struct redirx *parse_redir_x(struct redirx *head, char *buff)
{
	char *ptr=NULL;
	u_int32_t inaddr = 0;
	int display=0;
	int screen=0;
	int start_port = 0;
	struct redirx *new;
	if ((ptr = strchr(buff, ':'))) {
		*ptr++ = 0;
		if (*ptr == 0) {
			fprintf(stderr,"X-redirection syntax error\n");
			return head;
		}
	}
	if (buff[0]) {
		inaddr = inet_addr(buff);
		if (inaddr == 0xffffffff) {
			lprint(stderr,"Error: X-redirection bad address\r\n");
			return head;
		}
	}
	if (ptr) {
		if (strchr(ptr, '.')) {
			if (sscanf(ptr, "%d.%d", &display, &screen) != 2)
				return head;
		} else {
			if (sscanf(ptr, "%d", &display) != 1)
				return head;
		}
	}

	if (!inaddr) {
		lprint(stderr,"Error: X-redirection an IP address must be specified\r\n");
		return head;
	}

	if ((new=malloc(sizeof(struct redirx)))==NULL)
		return head;
	else {
		new->inaddr=inaddr;
		new->display=display;
		new->screen=screen;
		new->start_port=start_port;
		new->next=head;
		return new;
	}
}

static void do_redir_tcp(struct redirtcp *head)
{
	if (head) {
		do_redir_tcp(head->next);
		redir_tcp(head->inaddr,head->port,head->lport);
		free(head);
	}
}

static void do_redir_x(struct redirx *head)
{
	if (head) {
		do_redir_x(head->next);
		redir_x(head->inaddr,head->start_port,head->display,head->screen);
		free(head);
	}
}

void usage(char *name) {
	fprintf(stderr,"Usage: %s [-socket vdesock] [-dhcp] [-daemon] [-network netaddr] \n\t%s [-s vdesock] [-D] [-d] [-n netaddr]\n",name,name);
	exit(-1);
}

struct option slirpvdeopts[] = {
	{"socket",1,NULL,'s'},
	{"sock",1,NULL,'s'},
	{"vdesock",1,NULL,'s'},
	{"unix",1,NULL,'s'},
	{"pidfile", 1, 0, 'p'},
	{"dhcp",0,NULL,'D'},
	{"daemon",0,NULL,'d'},
	{"network",0,NULL,'n'},
	{"mod",1,0,'m'},
	{"group",1,0,'g'},
	{"port",1,0,'P'},
	{NULL,0,0,0}};

int main(int argc, char **argv)
{
  char *sockname=NULL;
  int result,nfds;
  int port=0;
  int connected_fd;
  register ssize_t nx;
  register int i;
  fd_set rs,ws,xs;
  int opt,longindx;
  char *netw=NULL;
	char *group=NULL;
	int mode=0700;
	int daemonize=0;
	struct redirtcp *rtcp=NULL;
	struct redirx *rx=NULL;

  prog=basename(argv[0]);

  while ((opt=GETOPT_LONG(argc,argv,"s:n:p:g:m:L:X:dD",slirpvdeopts,&longindx)) > 0) {
		switch (opt) {
			case 's' : sockname=optarg;
								 break;
			case 'D' : dhcpmgmt = 1;
								 break;
			case 'd' : daemonize = 1;
								 break;
			case 'n' : netw=optarg;
								 break;
			case 'm' : sscanf(optarg,"%o",&mode);
								 break;
			case 'g' : group=strdup(optarg);
								 break;
			case 'p':  pidfile=strdup(optarg);
								 break;
			case 'P' : port=atoi(optarg);
								 break;
			case 'L': rtcp=parse_redir_tcp(rtcp,optarg);
								 break;
			case 'X': rx=parse_redir_x(rx,optarg);
								 break;
			default  : usage(prog);
								 break;
		}
  }
	atexit(cleanup);
	if (daemonize) {
		openlog(basename(prog), LOG_PID, 0);
		logok=1;
		syslog(LOG_INFO,"slirpvde started");
	}
	if(getcwd(pidfile_path, PATH_MAX-1) == NULL) {
		printlog(LOG_ERR, "getcwd: %s", strerror(errno));
		exit(1);
	}
	strcat(pidfile_path, "/");
	if (daemonize && daemon(0, 1)) {
		printlog(LOG_ERR,"daemon: %s",strerror(errno));
		exit(1);
	}

	if((fddata = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0){
		perror("socket");
		exit(1);
	}
	connected_fd=send_fd(sockname, fddata, &dataout, port, group, mode);
	lfd=stderr;
	slirp_init(netw);

	do_redir_tcp(rtcp);
	do_redir_x(rx);

	for(;;) {
		FD_ZERO(&rs);
		FD_ZERO(&ws);
		FD_ZERO(&xs);
		nfds= -1;
		slirp_select_fill(&nfds,&rs,&ws,&xs);
		FD_SET(fddata,&rs);
		FD_SET(connected_fd,&rs);
		if (fddata>nfds) nfds=fddata;
		if (connected_fd>nfds) nfds=connected_fd;
		result=select(nfds+1,&rs,&ws,&xs,NULL);
		//printf("SELECT %d %d\n",nfds,result);
		if (FD_ISSET(fddata,&rs)) {
			nx=recv(fddata,bufin,BUFSIZE,0);
		  //fprintf(stderr,"TX to slirp %d\n",nx);
			result--;
		  slirp_input(bufin,nx);
		  //fprintf(stderr,"TX to slirp %d exit\n",nx);
	  }
	  if (result > 0) {
		  //fprintf(stderr,"slirp poll\n");
		  slirp_select_poll(&rs,&ws,&xs);
		  //fprintf(stderr,"slirp poll exit\n");
	  }
		if (FD_ISSET(connected_fd,&rs)) {
			if(read(connected_fd,bufin,BUFSIZE)==0)
				exit(0);
		}
  }
  return(0);
}