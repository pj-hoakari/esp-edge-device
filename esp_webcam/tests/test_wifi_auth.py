"""Check production Wi-Fi/EAP API wiring on a PC, with no radio or real keys."""
from pathlib import Path
import subprocess
import tempfile

MOCK = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_NOT_SUPPORTED 4
#define ESP_ERR_NVS_NOT_FOUND 5
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define PSA_SUCCESS 0
static int psa_crypto_init(void) { return PSA_SUCCESS; }
#define CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT 1
#ifndef TEST_FAST
#define CONFIG_ESP_WIFI_MBEDTLS_TLS_CLIENT 1
#define CONFIG_ESP_WIFI_SUITE_B_192 1
#endif
typedef int esp_err_t;
typedef int nvs_handle_t;
#include "wifi_auth.h"
typedef enum { WIFI_AUTH_WPA_ENTERPRISE, WIFI_AUTH_WPA2_ENTERPRISE, WIFI_AUTH_WPA3_ENTERPRISE, WIFI_AUTH_WPA3_ENT_192 } wifi_auth_mode_t;
typedef struct { struct { unsigned char ssid[32], password[64]; int sae_pwe_h2e;
    struct { wifi_auth_mode_t authmode; } threshold; struct { bool required; } pmf_cfg;
} sta; } wifi_config_t;
#define WIFI_IF_STA 0
#define WPA3_SAE_PWE_BOTH 2
typedef enum { ESP_EAP_TYPE_TLS=1, ESP_EAP_TYPE_TTLS=2, ESP_EAP_TYPE_PEAP=4, ESP_EAP_TYPE_FAST=8 } esp_eap_method_t;
typedef enum { ESP_EAP_TTLS_PHASE2_EAP, ESP_EAP_TTLS_PHASE2_MSCHAPV2, ESP_EAP_TTLS_PHASE2_MSCHAP,
    ESP_EAP_TTLS_PHASE2_PAP, ESP_EAP_TTLS_PHASE2_CHAP } esp_eap_ttls_phase2_types;
typedef struct { int fast_provisioning, fast_max_pac_list_len; bool fast_pac_format_binary; } esp_eap_fast_config;
static wifi_config_t applied;
static unsigned selected_method, selected_phase, enables, user_sets, pass_sets, cert_sets, ca_sets, domain_sets, bundles, suiteb;
static bool assets_present = true;
static int commits;
static char secret[129];
static const unsigned char *active_key;
static unsigned char ca[] = "CERT", client[] = "CERT", key[] = "KEY", pac[512];
static int credential_index(const char *name) {
    for (unsigned i=0; i<4; i++) if (!strcmp(name, wifi_credential_name(i))) return i;
    abort();
}
static int nvs_open(const char *name, int mode, int *h) { assert(!strcmp(name,"wifi_certs")); *h=1; return 0; }
static int nvs_get_blob(int h, const char *name, void *out, size_t *size) {
    if (!assets_present) return ESP_ERR_NVS_NOT_FOUND;
    unsigned i=credential_index(name);
    const unsigned char *data[] = {ca,client,key,pac}; size_t sizes[]={sizeof(ca),sizeof(client),sizeof(key),sizeof(pac)};
    if (out) { assert(*size >= sizes[i]); memcpy(out,data[i],sizes[i]); }
    *size=sizes[i]; return 0;
}
static int nvs_set_blob(int h,const char *name,const void *data,size_t size) { return 0; }
static int nvs_erase_key(int h,const char *name) { return 0; }
static int nvs_commit(int h) { commits++; return 0; }
static void nvs_close(int h) { }
static int esp_wifi_set_config(int iface,const wifi_config_t *cfg) { applied=*cfg; return 0; }
static int esp_eap_client_set_identity(const unsigned char *v,int len) { assert(len>0); return 0; }
static int esp_eap_client_set_username(const unsigned char *v,int len) { user_sets++; assert(len>0); return 0; }
static int esp_eap_client_set_password(const unsigned char *v,int len) { pass_sets++; memcpy(secret,v,len); secret[len]=0; return 0; }
static int esp_eap_client_set_disable_time_check(bool value) { return 0; }
static int esp_eap_client_set_ca_cert(const unsigned char *v,int len) { ca_sets++; assert(len>0); return 0; }
static int esp_eap_client_use_default_cert_bundle(bool value) { bundles++; assert(value); return 0; }
static int esp_eap_client_set_domain_name(const char *name) { domain_sets++; assert(name[0]); return 0; }
static int esp_eap_client_set_certificate_and_key(const unsigned char *crt,int crt_len,const unsigned char *k,int k_len,
    const unsigned char *pass,int pass_len) { cert_sets++; active_key=k; assert(crt_len>0 && k_len>0); return 0; }
static int esp_eap_client_set_ttls_phase2_method(esp_eap_ttls_phase2_types phase) { selected_phase=phase; return 0; }
static int esp_eap_client_set_pac_file(const unsigned char *file,int size) { assert(size>=512); return 0; }
static int esp_eap_client_set_fast_params(esp_eap_fast_config cfg) { assert(cfg.fast_provisioning==0 && cfg.fast_pac_format_binary); return 0; }
static int esp_eap_client_set_suiteb_192bit_certification(bool value) { suiteb++; assert(value); return 0; }
static int esp_eap_client_set_eap_methods(esp_eap_method_t method) { selected_method=method; return 0; }
static int esp_wifi_sta_enterprise_enable(void) { enables++; return 0; }
typedef struct { int placeholder; } mbedtls_pk_context;
typedef struct { int placeholder; } mbedtls_x509_crt;
static void mbedtls_pk_init(mbedtls_pk_context *k) { }
static void mbedtls_pk_free(mbedtls_pk_context *k) { }
static int mbedtls_pk_parse_key(mbedtls_pk_context *k,const unsigned char *data,size_t size,const unsigned char *p,size_t n) {
    return size==4 && !memcmp(data,"KEY",4) ? 0 : -1;
}
static void mbedtls_x509_crt_init(mbedtls_x509_crt *k) { }
static void mbedtls_x509_crt_free(mbedtls_x509_crt *k) { }
static int mbedtls_x509_crt_parse(mbedtls_x509_crt *k,const unsigned char *data,size_t size) {
    return size==5 && !memcmp(data,"CERT",5) ? 0 : -1;
}
struct timeval { int64_t tv_sec; int tv_usec; };
static int settimeofday(const struct timeval *tv,void *zone) { assert(tv->tv_sec>0); return 0; }
'''

TEST = r'''
static void reset(void) {
    for (unsigned i=0;i<WIFI_CRED_COUNT;i++) { free(boot_credentials[i]); boot_credentials[i]=NULL; boot_sizes[i]=0; }
    memset(&applied,0,sizeof(applied)); memset(secret,0,sizeof(secret));
    selected_method=selected_phase=enables=user_sets=pass_sets=cert_sets=ca_sets=domain_sets=bundles=suiteb=0;
    active_key=NULL; assets_present=true;
}
static network_settings_t profile(unsigned method) {
    network_settings_t s={.version=NETWORK_SETTINGS_VERSION,.eap_method=method,.enterprise_link=NETWORK_LINK_WPA2};
    strcpy(s.ssid,"CorpSSID"); strcpy(s.identity,"identity"); strcpy(s.username,"inner-user"); strcpy(s.eap_password,"EapSecret");
    return s;
}
int main(void) {
    network_settings_t s=profile(NETWORK_EAP_NONE); strcpy(s.password,"PersonalSecret");
    assert(wifi_auth_configure(&s)==0 && !enables && !user_sets && !pass_sets);
    assert(!strcmp((char *)applied.sta.password,"PersonalSecret"));
    for (unsigned method=NETWORK_EAP_TLS;method<NETWORK_EAP_COUNT;method++) {
        reset(); s=profile(method);
        if (!wifi_auth_method_available(method)) {
            assert(wifi_auth_configure(&s)==ESP_ERR_NOT_SUPPORTED && !enables); continue;
        }
        assert(wifi_auth_configure(&s)==0 && enables==1);
        for (unsigned i=0;i<sizeof(applied.sta.password);i++) assert(applied.sta.password[i]==0);
        assert(applied.sta.threshold.authmode==WIFI_AUTH_WPA2_ENTERPRISE);
        if (method==NETWORK_EAP_TLS) {
            assert(selected_method==ESP_EAP_TYPE_TLS && cert_sets==1 && !pass_sets && !user_sets);
            assert(!memcmp(active_key,"KEY",4)); key[0]='X'; assert(active_key[0]=='K'); key[0]='K';
        } else {
            assert(pass_sets==1 && user_sets==1 && !strcmp(secret,"EapSecret"));
            if (method==NETWORK_EAP_PEAP) assert(selected_method==ESP_EAP_TYPE_PEAP);
            else if (method==NETWORK_EAP_FAST) assert(selected_method==ESP_EAP_TYPE_FAST && !ca_sets);
            else assert(selected_method==ESP_EAP_TYPE_TTLS && selected_phase==method-NETWORK_EAP_TTLS_EAP);
        }
    }
    reset(); s=profile(NETWORK_EAP_PEAP); assets_present=false;
    assert(wifi_auth_configure(&s)==ESP_ERR_NVS_NOT_FOUND && !enables); /* No CA means no fallback. */
#ifndef TEST_FAST
    reset(); s=profile(NETWORK_EAP_TLS); s.enterprise_link=NETWORK_LINK_SUITEB;
    s.server_trust=NETWORK_TRUST_BUNDLE; strcpy(s.server_domain,"radius.example");
    assert(wifi_auth_configure(&s)==0 && applied.sta.pmf_cfg.required && suiteb==1 && bundles==1 && domain_sets==1);
    reset(); s=profile(NETWORK_EAP_PEAP); s.enterprise_link=NETWORK_LINK_WPA3; s.server_trust=NETWORK_TRUST_NONE;
    assert(wifi_auth_configure(&s)==ESP_ERR_INVALID_ARG && !enables);
#else
    assert(wifi_auth_method_available(NETWORK_EAP_FAST));
    assert(!wifi_auth_link_available(NETWORK_LINK_WPA3) && !wifi_auth_link_available(NETWORK_LINK_SUITEB));
#endif
    assert(wifi_credential_save(WIFI_CRED_CA,(unsigned char *)"CERT",5,NULL)==0);
    int before=commits;
    assert(wifi_credential_save(WIFI_CRED_CA,(unsigned char *)"BAD",4,NULL)==ESP_ERR_INVALID_ARG && commits==before);
    assert(wifi_credential_save(WIFI_CRED_KEY,(unsigned char *)"KEY",4,"")==0);
    assert(wifi_credential_save(WIFI_CRED_KEY,(unsigned char *)"KEY",3,"")==ESP_ERR_INVALID_ARG);
    assert(wifi_credential_save(WIFI_CRED_PAC,pac,511,NULL)==ESP_ERR_INVALID_SIZE);
    assert(wifi_credential_save(WIFI_CRED_PAC,pac,512,NULL)==0);
    assert(wifi_auth_set_time(0)==ESP_ERR_INVALID_ARG && wifi_auth_set_time(1793577600LL)==0);
    reset(); puts("WIFI_AUTH_TEST_PASSED"); return 0;
}
'''


def main():
    project = Path(__file__).resolve().parents[1]
    root = (project / '.tools').resolve()
    root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='wifi-auth-', dir=root) as directory:
        temp = Path(directory)
        assert temp.resolve().is_relative_to(root)
        (temp / 'mock.h').write_text(MOCK, encoding='utf-8')
        for name in ('esp_err.h', 'sdkconfig.h', 'esp_wifi.h', 'esp_eap_client.h', 'nvs.h',
                     'mbedtls/pk.h', 'mbedtls/x509_crt.h', 'sys/time.h', 'psa/crypto.h'):
            header = temp / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "mock.h"\n', encoding='utf-8')
        source = temp / 'auth_test.c'
        source.write_text('#include "mock.h"\n#include "' + (project / 'main/wifi_auth.c').as_posix() + '"\n' + TEST,
                          encoding='utf-8')
        for fast in (False, True):
            exe = temp / ('fast.exe' if fast else 'normal.exe')
            compile_result = subprocess.run(['cl', '/nologo', '/std:c11', '/O2', '/D_CRT_SECURE_NO_WARNINGS',
                *(['/DTEST_FAST'] if fast else []), '/I' + str(temp), '/I' + str(project / 'main'),
                '/Fe' + str(exe), str(source), str(project / 'main/network_settings_values.c')],
                cwd=temp, capture_output=True, text=True, timeout=60)
            if compile_result.returncode:
                raise RuntimeError(compile_result.stdout + compile_result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            if result.returncode or 'WIFI_AUTH_TEST_PASSED' not in result.stdout:
                raise RuntimeError(result.stdout + result.stderr)
    print('Wi-Fi auth wiring passed: normal/FAST backends, all EAP mappings, no PSK/EAP mixing, trust failures, credential validation.')


if __name__ == '__main__':
    main()
