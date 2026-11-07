"""Run portable C regressions and serial/authorization tests (C11 cc required)."""
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    project = Path(__file__).resolve().parents[1]
    cjson = project / 'managed_components/espressif__cjson/cJSON'
    tests = {
        'cloud_auth_test': ['main/cloud_settings_values.c', 'main/device_auth_values.c',
                            'main/cloud_auth_logic.c', str(cjson / 'cJSON.c')],
        'cloud_signaling_test': ['main/cloud_auth_logic.c', str(cjson / 'cJSON.c')],
        'network_settings_test': ['main/network_settings_values.c'],
        'nvs_boot_test': ['main/nvs_boot_values.c'],
        'sender_policy_test': [],
    }
    with tempfile.TemporaryDirectory(prefix='webcam-host-') as directory:
        exe = Path(directory) / 'test'
        for name, sources in tests.items():
            modes = (False, True) if name in ('cloud_auth_test', 'network_settings_test') else (False,)
            for development in modes:
                args = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(project / 'tests/stubs'), '-I' + str(project / 'main'),
                        '-I' + str(cjson)]
                if development:
                    args += ['-DCONFIG_WEBCAM_ALLOW_INSECURE_HTTP=1']
                args += [str(project / 'tests' / f'{name}.c')]
                args += [str(project / source) for source in sources]
                args += ['-lm', '-o', str(exe)]
                subprocess.run(args, check=True)
                subprocess.run([str(exe)], check=True)
    for script in ('test_serial_protocol.py', 'test_device_auth_worker.py'):
        subprocess.run([sys.executable, str(project / 'tests' / script)], check=True)


if __name__ == '__main__':
    main()
