"""Compile production authorization and Firebase code with local HTTP/RTOS fakes."""
from pathlib import Path
import json
import subprocess
import tempfile


def main():
    project = Path(__file__).resolve().parents[1]
    cjson = project / 'managed_components/espressif__cjson/cJSON'
    with tempfile.TemporaryDirectory(prefix='webcam-auth-') as directory:
        for development in (False, True):
            exe = Path(directory) / 'auth-test'
            args = ['cc', '-std=c11', '-Wall', '-Wextra', '-Wno-unused-parameter',
                    '-Wno-unused-variable', '-I' + str(project / 'tests/auth_stubs'),
                    '-I' + str(project / 'main'), '-I' + str(cjson)]
            if development:
                args += ['-DCONFIG_WEBCAM_ALLOW_INSECURE_HTTP=1']
            args += [str(project / source) for source in (
                'tests/device_auth_worker_test.c', 'main/device_auth.c',
                'main/device_auth_values.c', 'main/cloud_settings_values.c',
                'main/firebase_client.c', 'main/peer_authorization.c')]
            args += [str(cjson / 'cJSON.c'), '-lm', '-o', str(exe)]
            subprocess.run(args, check=True, capture_output=True, text=True)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            lines = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
            assert sum(line.get('type') == 'device_authorization' for line in lines) == 1
            assert 'dummy-refresh' not in result.stdout and 'dummy-device-code' not in result.stdout
            assert 'device_auth_worker_test: ok' in result.stdout
        # Compile the unmodified peer loop and globals from main.c. Platform
        # adapters replace camera/peer/RTOS only; sender control flow is real.
        source = (project / 'main/main.c').read_text()
        globals_start = source.index('static esp_peer_handle_t peer;')
        globals_end = source.index('\nstatic void stats_bump', globals_start)
        loop_start = source.index('static void peer_task(void *arg)')
        loop_end = source.index('\nbool webrtc_submit_offer', loop_start)
        unit = Path(directory) / 'peer-test.c'
        unit.write_text('#define PEER_TASK_TEST\n#include "' +
                        str(project / 'tests/device_auth_worker_test.c') +
                        '"\n#include "peer_task_mock.h"\n' +
                        source[globals_start:globals_end] +
                        '\nstatic void stats_reset(void) { memset(&stats, 0, sizeof(stats)); }\n' +
                        source[loop_start:loop_end] + r'''
static void enqueue_stale_offer(void) {
    command_t stale = {.sdp = malloc(32)};
    strcpy(stale.sdp, "stale SDP");
    assert(xQueueSend(commands, &stale, 0) == pdTRUE);
}

int main(void) {
    approved();
    reply = "{\"error\":\"invalid_grant\"}"; http_status = 400;
    peer = (void *)1; channel_open = true; jpeg_channel_created = true;
    ack_pending = true;
    commands = xQueueCreate(1, sizeof(command_t));
    answer_lock = (void *)1;
    pending_answer = malloc(32);
    if (!setjmp(peer_done)) peer_task(NULL);
    assert(peer_sends == 1 && peer_closes == 1 && camera_frames == 1);
    assert(peer == NULL && !channel_open && !jpeg_channel_created && !ack_pending);
    assert(pending_answer == NULL && !active_session_id[0]);
    assert(peer_authorization_blocked());
    command_t stale;
    assert(xQueueReceive(commands, &stale, 0) != pdTRUE);
    for (int i = 0; i < allocations_count; i++) free(allocations[i]);
    free(commands);
    puts("peer_task_test: invalid_grant stops partial JPEG and closes peer");
    return 0;
}
''')
        exe = Path(directory) / 'peer-test'
        args = ['cc', '-std=c11', '-Wno-unused-parameter',
                '-I' + str(project / 'tests/auth_stubs'),
                '-I' + str(project / 'tests'), '-I' + str(project / 'main'),
                '-I' + str(cjson), str(unit)]
        args += [str(project / 'main' / name) for name in
                 ('device_auth.c', 'device_auth_values.c', 'cloud_settings_values.c',
                  'firebase_client.c', 'peer_authorization.c')]
        args += [str(cjson / 'cJSON.c'), '-lm', '-o', str(exe)]
        result = subprocess.run(args, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    print('Authorization worker passed: queue/full, one JSON, revocation, grant expiry, HTTPS/dev HTTP.')


if __name__ == '__main__':
    main()
