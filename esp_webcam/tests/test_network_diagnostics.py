"""Exercise live-IP checks and finite ping lifecycle with PC-only SDK adapters."""
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
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
typedef void *TaskHandle_t;
typedef struct { uint32_t addr; } ip4_addr_t;
typedef struct { ip4_addr_t ip4; int type; } ip_addr_t;
typedef struct { ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct { int unused; } esp_netif_t;
typedef struct { int unused; } wifi_ap_record_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) (unsigned)((p)->addr>>24), (unsigned)(((p)->addr>>16)&255), (unsigned)(((p)->addr>>8)&255), (unsigned)((p)->addr&255)
#define IP_IS_V4(p) ((p)->type==4)
#define ip_addr_set_ip4_u32(p,v) do { (p)->ip4.addr=(v); (p)->type=4; } while(0)
#define ip_2_ip4(p) (&(p)->ip4)
#define ip4_addr_get_u32(p) ((p)->addr)
static int ipaddr_aton(const char *s,ip_addr_t *p) {
    unsigned a,b,c,d; char extra;
    if (strchr(s,':')) { p->type=6; return 1; }
    if (sscanf(s,"%u.%u.%u.%u%c",&a,&b,&c,&d,&extra)!=4 || a>255 || b>255 || c>255 || d>255) return 0;
    p->ip4.addr=(a<<24)|(b<<16)|(c<<8)|d; p->type=4; return 1;
}
static char *ipaddr_ntoa_r(const ip_addr_t *p,char *out,int size) { snprintf(out,size,IPSTR,IP2STR(&p->ip4)); return out; }
static bool exists=true, up=true, associated=true, get_error;
static esp_netif_t iface;
static esp_netif_ip_info_t live;
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { assert(!strcmp(key,"WIFI_STA_DEF")); return exists ? &iface : NULL; }
static bool esp_netif_is_netif_up(esp_netif_t *n) { return up; }
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) { return associated ? ESP_OK : ESP_FAIL; }
static int esp_netif_get_ip_info(esp_netif_t *n,esp_netif_ip_info_t *out) { *out=live; return get_error ? ESP_FAIL : ESP_OK; }
static const char *esp_err_to_name(int err) { return "mock error"; }
typedef void *esp_ping_handle_t;
typedef struct { void *cb_args; void (*on_ping_success)(void*,void*), (*on_ping_timeout)(void*,void*), (*on_ping_end)(void*,void*); } esp_ping_callbacks_t;
typedef struct { uint32_t count,interval_ms,timeout_ms,data_size; ip_addr_t target_addr; } esp_ping_config_t;
#define ESP_PING_DEFAULT_CONFIG() {0}
typedef enum { ESP_PING_PROF_SEQNO, ESP_PING_PROF_TIMEGAP, ESP_PING_PROF_IPADDR, ESP_PING_PROF_REQUEST, ESP_PING_PROF_REPLY } esp_ping_profile_t;
static esp_ping_config_t config;
static esp_ping_callbacks_t callbacks;
static unsigned created, deleted, notifications, waited;
static bool new_error, start_error, timeout_mode;
static uint16_t seq;
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void*)0x1234; }
static void xTaskNotifyGive(TaskHandle_t task) { assert(task==(void*)0x1234 && deleted>0); notifications++; }
static uint32_t ulTaskNotifyTake(int clear,uint32_t timeout) {
    unsigned n=notifications; notifications=0;
    if (timeout==portMAX_DELAY) { assert(n==1); waited++; }
    return n;
}
static int esp_ping_new_session(const esp_ping_config_t *cfg,const esp_ping_callbacks_t *cb,void **handle) {
    created++;
    if (new_error) return ESP_FAIL;
    config=*cfg; callbacks=*cb; *handle=(void*)1; return 0;
}
static int esp_ping_delete_session(void *handle) { assert(handle==(void*)1); deleted++; return 0; }
static int esp_ping_get_profile(void *handle,esp_ping_profile_t kind,void *out,uint32_t size) {
    assert(handle==(void*)1);
    if (kind==ESP_PING_PROF_SEQNO) { assert(size==sizeof(seq)); memcpy(out,&seq,size); }
    else if (kind==ESP_PING_PROF_IPADDR) { assert(size==sizeof(config.target_addr)); memcpy(out,&config.target_addr,size); }
    else { uint32_t value=kind==ESP_PING_PROF_TIMEGAP ? 7 : kind==ESP_PING_PROF_REQUEST ? 3 : timeout_mode ? 0 : 3;
        assert(size==sizeof(value)); memcpy(out,&value,size); }
    return 0;
}
static int esp_ping_start(void *handle) {
    if (start_error) return ESP_FAIL;
    assert(config.count==3 && config.timeout_ms==1000 && config.interval_ms==1000 && config.data_size==32);
    for (seq=1;seq<=3;seq++) {
        if (timeout_mode) callbacks.on_ping_timeout(handle,callbacks.cb_args);
        else callbacks.on_ping_success(handle,callbacks.cb_args);
    }
    callbacks.on_ping_end(handle,callbacks.cb_args); return 0;
}
'''

TEST = r'''
int main(void) {
    ip_addr_t addr; assert(ipaddr_aton("192.0.2.10",&addr)); live.ip=addr.ip4;
    assert(ipaddr_aton("255.255.255.0",&addr)); live.netmask=addr.ip4;
    assert(ipaddr_aton("192.0.2.1",&addr)); live.gw=addr.ip4;
    puts("DOWN_BEGIN"); associated=false;
    network_diagnostics_show(); network_diagnostics_ping(""); assert(created==0);
    associated=true; up=false; network_diagnostics_show();
    up=true; exists=false; network_diagnostics_show();
    exists=true; get_error=true; network_diagnostics_show();
    get_error=false; uint32_t saved_ip=live.ip.addr; live.ip.addr=0;
    network_diagnostics_show(); network_diagnostics_ping("192.0.2.1"); assert(created==0);
    live.ip.addr=saved_ip; puts("DOWN_END");
    network_diagnostics_show();
    network_diagnostics_ping("");
    assert(config.target_addr.ip4.addr==live.gw.addr && created==1 && deleted==1 && waited==1);
    network_diagnostics_ping("192.0.2.9");
    assert(config.target_addr.ip4.addr!=live.gw.addr && created==2 && deleted==2 && waited==2);
    network_diagnostics_ping("bad-host"); network_diagnostics_ping("::1"); network_diagnostics_ping("0.0.0.0");
    assert(created==2);
    uint32_t gateway=live.gw.addr; live.gw.addr=0; network_diagnostics_ping(""); assert(created==2); live.gw.addr=gateway;
    timeout_mode=true; network_diagnostics_ping(""); assert(created==3 && deleted==3 && waited==3);
    new_error=true; network_diagnostics_ping(""); assert(created==4 && deleted==3 && waited==3);
    new_error=false; start_error=true; network_diagnostics_ping(""); assert(created==5 && deleted==4 && waited==3);
    puts("DIAGNOSTICS_TEST_PASSED"); return 0;
}
'''


def main():
    project = Path(__file__).resolve().parents[1]
    root = (project / '.tools').resolve()
    root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='net-diagnostics-', dir=root) as directory:
        temp = Path(directory)
        assert temp.resolve().is_relative_to(root)
        (temp / 'mock.h').write_text(MOCK, encoding='utf-8')
        for name in ('esp_netif.h', 'esp_wifi.h', 'freertos/FreeRTOS.h', 'freertos/task.h', 'ping/ping_sock.h'):
            header = temp / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "mock.h"\n', encoding='utf-8')
        source = temp / 'diagnostics_test.c'
        source.write_text('#include "mock.h"\n#include "' + (project / 'main/network_diagnostics.c').as_posix() + '"\n' + TEST,
                          encoding='utf-8')
        exe = temp / 'diagnostics_test.exe'
        compilation = subprocess.run(['cl', '/nologo', '/std:c11', '/O2', '/D_CRT_SECURE_NO_WARNINGS',
            '/I' + str(temp), '/I' + str(project / 'main'), '/Fe' + str(exe), str(source)],
            cwd=temp, capture_output=True, text=True, timeout=60)
        if compilation.returncode:
            raise RuntimeError(compilation.stdout + compilation.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        assert 'DIAGNOSTICS_TEST_PASSED' in result.stdout
        down = result.stdout.split('DOWN_BEGIN')[1].split('DOWN_END')[0]
        assert '192.0.2.10' not in down
        assert 'IPv4: 192.0.2.10' in result.stdout and 'Gateway: 192.0.2.1' in result.stdout
        assert 'Ping: 3 sent, 3 received' in result.stdout
        assert 'Ping: 3 sent, 0 received' in result.stdout
    print('Network diagnostics passed: live/down IP, gateway/IPv4 ping, timeouts, failure cleanup, repeat sessions.')


if __name__ == '__main__':
    main()
