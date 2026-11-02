"""Check that probes use only GET /ice and one independent STUN Binding request."""
from pathlib import Path
import subprocess
import tempfile

MOCK = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "network_settings.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
static char stun_url[128]="stun:probe.example:3478";
#define CONFIG_WEBCAM_STUN_URL stun_url
static network_settings_t settings;
const network_settings_t *network_settings_active(void) { return &settings; }
static bool online=true;
bool network_diagnostics_has_ipv4(void) { return online; }
static int64_t ticks;
static int64_t esp_timer_get_time(void) { ticks+=1000; return ticks; }
static const char *esp_err_to_name(int err) { return "mock error"; }
static void esp_fill_random(void *out,size_t size) { for(size_t i=0;i<size;i++) ((uint8_t*)out)[i]=(uint8_t)(i+1); }
typedef void *esp_http_client_handle_t;
typedef struct { const char *url; int timeout_ms; bool disable_auto_redirect,keep_alive_enable; } esp_http_client_config_t;
#define HTTP_METHOD_GET 0
static unsigned http_inits,http_opens,http_headers,http_closes,http_cleanups;
static bool http_init_error,http_open_error,http_header_error;
static int http_status=200;
static char requested_url[256];
static void *esp_http_client_init(const esp_http_client_config_t *cfg) {
    http_inits++; assert(cfg->timeout_ms==3000 && cfg->disable_auto_redirect && !cfg->keep_alive_enable);
    strcpy(requested_url,cfg->url); return http_init_error ? NULL : (void*)1;
}
static int esp_http_client_set_method(void *h,int method) { assert(h==(void*)1 && method==HTTP_METHOD_GET); return 0; }
static int esp_http_client_open(void *h,int body_size) { http_opens++; assert(body_size==0); return http_open_error ? ESP_FAIL : 0; }
static int esp_http_client_fetch_headers(void *h) { http_headers++; return http_header_error ? -1 : 100; }
static int esp_http_client_get_status_code(void *h) { return http_status; }
static int esp_http_client_close(void *h) { http_closes++; return 0; }
static int esp_http_client_cleanup(void *h) { http_cleanups++; return 0; }
#define AF_INET 2
#define SOCK_DGRAM 2
#define IPPROTO_UDP 17
#define SOL_SOCKET 1
#define SO_RCVTIMEO 20
struct sockaddr { int unused; };
struct timeval { long tv_sec,tv_usec; };
struct addrinfo { int ai_family,ai_socktype; struct sockaddr *ai_addr; unsigned ai_addrlen; };
static struct sockaddr destination;
static struct addrinfo resolved={AF_INET,SOCK_DGRAM,&destination,sizeof(destination)};
static unsigned dns_calls,dns_frees,socket_calls,sends,receives,socket_closes;
static bool dns_error,socket_error,connect_error,send_error,receive_error,bad_transaction,error_response;
static uint8_t sent_packet[20];
static int getaddrinfo(const char *host,const char *port,const struct addrinfo *hints,struct addrinfo **result) {
    dns_calls++; assert(!strcmp(host,"probe.example") && !strcmp(port,"3478"));
    assert(hints->ai_family==AF_INET && hints->ai_socktype==SOCK_DGRAM);
    *result=dns_error ? NULL : &resolved; return dns_error ? -1 : 0;
}
static void freeaddrinfo(struct addrinfo *p) { assert(p==&resolved); dns_frees++; }
static int socket(int family,int type,int protocol) { socket_calls++; assert(family==AF_INET && type==SOCK_DGRAM && protocol==IPPROTO_UDP); return socket_error ? -1 : 42; }
static int setsockopt(int fd,int level,int option,const void *value,size_t size) {
    assert(fd==42 && level==SOL_SOCKET && option==SO_RCVTIMEO);
    assert(size==sizeof(struct timeval) && ((const struct timeval*)value)->tv_sec==3); return 0;
}
static int connect(int fd,const struct sockaddr *address,unsigned size) { assert(fd==42 && address==&destination); return connect_error ? -1 : 0; }
static int send(int fd,const void *data,size_t size,int flags) {
    sends++; assert(fd==42 && size==20 && !flags);
    const uint8_t prefix[]={0,1,0,0,0x21,0x12,0xa4,0x42};
    assert(!memcmp(data,prefix,sizeof(prefix))); memcpy(sent_packet,data,20);
    return send_error ? -1 : (int)size;
}
static int recv(int fd,void *data,size_t size,int flags) {
    receives++; assert(fd==42 && size==512 && !flags);
    if(receive_error) { errno=EAGAIN; return -1; }
    uint8_t *out=data; memset(out,0,32); memcpy(out,sent_packet,20); out[0]=1; out[1]=1;
    out[3]=12; out[21]=0x20; out[23]=8; out[25]=1;
    if(bad_transaction) out[8]++;
    if(error_response) { out[1]=0x11; out[3]=8; out[21]=9; out[23]=4; out[25]=0; out[26]=4; out[27]=1; return 28; }
    return 32;
}
static int close(int fd) { assert(fd==42); socket_closes++; return 0; }
'''

TEST = r'''
static void reset(void) {
    http_inits=http_opens=http_headers=http_closes=http_cleanups=0;
    dns_calls=dns_frees=socket_calls=sends=receives=socket_closes=0;
    http_init_error=http_open_error=http_header_error=false; http_status=200;
    dns_error=socket_error=connect_error=send_error=receive_error=bad_transaction=error_response=false;
}
int main(void) {
    strcpy(settings.signaling_url,"http://192.0.2.1:8080");
    online=false; network_probes_signaling(); network_probes_stun(); assert(!http_inits && !dns_calls);
    online=true; network_probes_signaling();
    assert(!strcmp(requested_url,"http://192.0.2.1:8080/ice"));
    assert(http_inits==1 && http_opens==1 && http_headers==1 && http_closes==1 && http_cleanups==1);
    reset(); http_status=302; network_probes_signaling(); assert(http_opens==1 && http_cleanups==1); /* no redirect */
    reset(); http_open_error=true; network_probes_signaling(); assert(!http_headers && http_closes==1 && http_cleanups==1);
    reset(); http_header_error=true; network_probes_signaling(); assert(http_headers==1 && http_cleanups==1);
    reset(); http_init_error=true; network_probes_signaling(); assert(!http_opens && !http_cleanups);
    reset(); settings.signaling_url[0]=0; network_probes_signaling(); assert(!http_inits);
    network_probes_stun(); assert(dns_calls==1 && dns_frees==1 && sends==1 && receives==1 && socket_closes==1);
    reset(); error_response=true; network_probes_stun(); assert(sends==1 && socket_closes==1); /* no auth retry */
    reset(); bad_transaction=true; network_probes_stun(); assert(sends==1 && socket_closes==1);
    reset(); receive_error=true; network_probes_stun(); assert(sends==1 && socket_closes==1);
    reset(); dns_error=true; network_probes_stun(); assert(!socket_calls && !sends);
    reset(); socket_error=true; network_probes_stun(); assert(dns_frees==1 && !socket_closes);
    reset(); connect_error=true; network_probes_stun(); assert(socket_closes==1 && !sends);
    reset(); send_error=true; network_probes_stun(); assert(socket_closes==1 && !receives);
    reset(); strcpy(stun_url,"stuns:probe.example"); network_probes_stun(); assert(!dns_calls);
    stun_url[0]=0; network_probes_stun(); assert(!dns_calls);
    puts("PROBES_TEST_PASSED"); return 0;
}
'''


def main():
    project = Path(__file__).resolve().parents[1]
    root = (project / '.tools').resolve()
    root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='net-probes-', dir=root) as directory:
        temp = Path(directory)
        assert temp.resolve().is_relative_to(root)
        (temp / 'mock.h').write_text(MOCK, encoding='utf-8')
        for name in ('sdkconfig.h', 'esp_http_client.h', 'esp_random.h', 'esp_timer.h', 'lwip/netdb.h', 'lwip/sockets.h'):
            header = temp / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "mock.h"\n', encoding='utf-8')
        source = temp / 'probes_test.c'
        source.write_text('#include "mock.h"\n#include "' + (project / 'main/network_probes.c').as_posix() + '"\n' + TEST,
                          encoding='utf-8')
        exe = temp / 'probes_test.exe'
        compilation = subprocess.run(['cl', '/nologo', '/std:c11', '/O2', '/D_CRT_SECURE_NO_WARNINGS',
            '/I' + str(temp), '/I' + str(project / 'main'), '/Fe' + str(exe), str(source)],
            cwd=temp, capture_output=True, text=True, timeout=60)
        if compilation.returncode:
            raise RuntimeError(compilation.stdout + compilation.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        assert 'PROBES_TEST_PASSED' in result.stdout
        assert 'Signaling HTTP reachable: status=200' in result.stdout
        assert 'STUN UDP reachable: Binding response OK' in result.stdout
        assert 'invalid or unrelated response' in result.stdout
    print('Connectivity probes passed: GET-only /ice, no redirects/SDP, one STUN Binding request, failure cleanup.')


if __name__ == '__main__':
    main()
