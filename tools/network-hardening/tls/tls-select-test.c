/* Extracted IDF state machine, real loopback connect/select/SO_ERROR.
 * Only TLS crypto/hostname setup are narrow seams; no mocked select result. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
typedef int esp_err_t;
enum {ESP_OK=0};
#include "tls-state.inc"
enum {ESP_TLS_ERR_TYPE_ESP=1,ESP_TLS_ERR_TYPE_SYSTEM,ESP_ERR_ESP_TLS_SOCKET_SETOPT_FAILED,ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST};
typedef struct {bool non_block,is_plain_tcp;int timeout_ms;} esp_tls_cfg_t;
typedef struct esp_tls {int sockfd,conn_state;bool is_tls;fd_set rset,wset;void *error_handle;ssize_t (*read)(struct esp_tls *,char *,size_t);ssize_t (*write)(struct esp_tls *,const char *,size_t);} esp_tls_t;
static int prepared_fd,ssl_created,handshakes,system_error,esp_error,selects,last_select;
static bool first_empty;
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_INT_EVENT_TRACKER_CAPTURE(h,t,e) capture(t,e)
static void capture(int type,int error){if(type==ESP_TLS_ERR_TYPE_SYSTEM)system_error=error;else esp_error=error;}
static int real_select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *tv) {
    int ret=select(n,r,w,e,tv);last_select=ret;++selects;
    if(selects==1&&ret==0)first_empty=!FD_ISSET(prepared_fd,r)&&!FD_ISSET(prepared_fd,w);
    return ret;
}
#define select real_select
static void _esp_tls_net_init(esp_tls_t *tls){(void)tls;}
static esp_err_t tcp_connect(const char *h,int l,int port,const esp_tls_cfg_t *cfg,void *error,int *fd) {
    (void)h;(void)l;(void)port;(void)cfg;(void)error;*fd=prepared_fd;return ESP_OK;
}
static ssize_t tcp_read(esp_tls_t *t,char *s,size_t n){return recv(t->sockfd,s,n,0);}
static ssize_t tcp_write(esp_tls_t *t,const char *s,size_t n){return send(t->sockfd,s,n,0);}
#define _esp_tls_read tcp_read
#define _esp_tls_write tcp_write
static int create_ssl_handle(const char *h,int len,const esp_tls_cfg_t *cfg,esp_tls_t *tls){(void)h;(void)len;(void)cfg;(void)tls;++ssl_created;return ESP_OK;}
static int esp_tls_handshake(esp_tls_t *tls,const esp_tls_cfg_t *cfg){(void)cfg;++handshakes;tls->conn_state=ESP_TLS_DONE;return 1;}
#include TLS_FUNCTIONS
#undef select
static void reset(void){ssl_created=handshakes=system_error=esp_error=selects=last_select=0;first_empty=false;}
static int bound_socket(struct sockaddr_in *addr) {
    int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);*addr=(struct sockaddr_in){.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(bind(fd,(struct sockaddr *)addr,sizeof(*addr))==0);socklen_t len=sizeof(*addr);assert(getsockname(fd,(struct sockaddr *)addr,&len)==0);return fd;
}
static int pending(struct sockaddr_in *addr){
    int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);assert(fcntl(fd,F_SETFL,O_NONBLOCK)==0);
    assert(connect(fd,(struct sockaddr *)addr,sizeof(*addr))<0&&errno==EINPROGRESS);return fd;
}
static void noop(int n){(void)n;}
static int failures;
static void timeout_then_ready(void) {
    reset();struct sockaddr_in addr;int listener=bound_socket(&addr);assert(listen(listener,0)==0);
    int filler=socket(AF_INET,SOCK_STREAM,0);assert(filler>=0);assert(connect(filler,(struct sockaddr *)&addr,sizeof(addr))==0);
    prepared_fd=pending(&addr);esp_tls_t tls={0};esp_tls_cfg_t cfg={.non_block=true,.timeout_ms=50};
    int first=esp_tls_conn_new_async("localhost",9,ntohs(addr.sin_port),&cfg,&tls);
    assert(first==0&&last_select==0&&first_empty&&ssl_created==0&&tls.conn_state==ESP_TLS_CONNECTING);
    int accepted=accept(listener,NULL,NULL);assert(accepted>=0);close(accepted);close(filler);
    fd_set probe;FD_ZERO(&probe);FD_SET(prepared_fd,&probe);struct timeval tv={.tv_sec=3};
    assert(select(prepared_fd+1,NULL,&probe,NULL,&tv)==1); /* Real SYN retry now completes. */
    int second=esp_tls_conn_new_async("localhost",9,ntohs(addr.sin_port),&cfg,&tls);
    bool pass=second==1&&ssl_created==1&&handshakes==1;
    printf("{\"case\":\"timeout_then_real_TCP_ready\",\"first_result\":%d,\"first_select_emptied_sets\":true,\"private_probe_writable\":true,\"second_result\":%d,\"handshakes\":%d,\"pass\":%s}\n",first,second,handshakes,pass?"true":"false");
    failures+=!pass;close(prepared_fd);close(listener);
}
static void interrupted_select(void) {
    reset();struct sockaddr_in addr;int listener=bound_socket(&addr);assert(listen(listener,0)==0);int filler=socket(AF_INET,SOCK_STREAM,0);assert(filler>=0);assert(connect(filler,(struct sockaddr *)&addr,sizeof(addr))==0);
    prepared_fd=pending(&addr);struct sigaction action={.sa_handler=noop};sigemptyset(&action.sa_mask);assert(sigaction(SIGALRM,&action,NULL)==0);
    struct itimerval timer={.it_value={.tv_usec=20000}};assert(setitimer(ITIMER_REAL,&timer,NULL)==0);
    esp_tls_t tls={0};esp_tls_cfg_t cfg={.non_block=true,.timeout_ms=100};int result=esp_tls_conn_new_async("localhost",9,1,&cfg,&tls);
    assert(last_select==-1);bool pass=result==-1&&tls.conn_state==ESP_TLS_FAIL&&handshakes==0&&system_error==EINTR&&esp_error==ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST;
    printf("{\"case\":\"real_select_EINTR\",\"select_result\":%d,\"result\":%d,\"handshakes\":%d,\"captured_system_error\":%d,\"pass\":%s}\n",last_select,result,handshakes,system_error,pass?"true":"false");
    failures+=!pass;close(prepared_fd);close(filler);close(listener);
}
static void refused_connect(void) {
    reset();struct sockaddr_in addr;int reserved=bound_socket(&addr);/* Bound but not listening: deterministic refused local port. */
    prepared_fd=pending(&addr);esp_tls_t tls={0};esp_tls_cfg_t cfg={.non_block=true,.timeout_ms=100};int result=esp_tls_conn_new_async("localhost",9,ntohs(addr.sin_port),&cfg,&tls);
    bool pass=result==-1&&tls.conn_state==ESP_TLS_FAIL&&handshakes==0&&system_error==ECONNREFUSED&&esp_error==ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST;
    printf("{\"case\":\"real_SO_ERROR_ECONNREFUSED\",\"select_result\":%d,\"result\":%d,\"handshakes\":%d,\"captured_system_error\":%d,\"pass\":%s}\n",last_select,result,handshakes,system_error,pass?"true":"false");
    failures+=!pass;close(prepared_fd);close(reserved);
}
int main(void){timeout_then_ready();interrupted_select();refused_connect();return failures?2:0;}
