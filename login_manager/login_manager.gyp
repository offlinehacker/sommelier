{
  'target_defaults': {
    'defines': [
      'OS_CHROMEOS',
      'USE_CHEETS=<(USE_cheets)',
      'USE_NSS_CERTS',
      'USE_SYSTEMD=<(USE_systemd)',
    ],
    'variables': {
      'deps': [
        'dbus-1',
        'libbrillo-<(libbase_ver)',
        'libchrome-<(libbase_ver)',
        'libchromeos-ui-<(libbase_ver)',
        'libcontainer',
        'libcros_config',
        'libmetrics-<(libbase_ver)',
        'libminijail',
        'libpasswordprovider',
        'nss',
        # system_api depends on protobuf (or protobuf-lite). It must appear
        # before protobuf here or the linker flags won't be in the right
        # order.
        'system_api',
        'protobuf-lite',
        'vboot_host',
      ],
    },
  },
  'targets': [
    {
      # For naming, '-adaptors' suffix uses hyphen, but 'session_manager' uses
      # underscore. So, here intentionally mix them.
      'target_name': 'session_manager-adaptors',
      'type': 'none',
      'variables': {
        'dbus_service_config': 'dbus_bindings/dbus-service-config.json',
        'dbus_adaptors_out_dir': 'include/login_manager/dbus_adaptors',
      },
      'sources': [
        'dbus_bindings/org.chromium.SessionManagerInterface.xml',
      ],
      'includes': ['../common-mk/generate-dbus-adaptors.gypi'],
    },
    {
      'target_name': 'libsession_manager',
      'type': 'static_library',
      'dependencies': [
        'session_manager-adaptors',
        '../common-mk/external_dependencies.gyp:install_attributes-proto',
        '../common-mk/external_dependencies.gyp:policy-protos',
      ],
      'link_settings': {
        'libraries': [
          '-lbootstat',
          '-linstallattributes-<(libbase_ver)',
          '-lpolicy-<(libbase_ver)',
        ],
      },
      'sources': [
        'android_oci_wrapper.cc',
        'blob_util.cc',
        'browser_job.cc',
        'child_exit_handler.cc',
        'child_job.cc',
        'chrome_setup.cc',
        'container_manager_interface.cc',
        'crossystem.cc',
        'crossystem_impl.cc',
        'cumulative_use_time_metric.cc',
        'dbus_util.cc',
        'device_local_account_manager.cc',
        'device_policy_service.cc',
        'file_checker.cc',
        'generator_job.cc',
        'key_generator.cc',
        'liveness_checker_impl.cc',
        'login_metrics.cc',
        'named_platform_handle_utils_posix.cc',
        'nss_util.cc',
        'owner_key_loss_mitigator.cc',
        'policy_key.cc',
        'policy_service.cc',
        'policy_store.cc',
        'regen_mitigator.cc',
        'resilient_policy_store.cc',
        'server_backed_state_key_generator.cc',
        'session_manager_impl.cc',
        'session_manager_service.cc',
        'subprocess.cc',
        'system_utils_impl.cc',
        'systemd_unit_starter.cc',
        'termina_manager_impl.cc',
        'upstart_signal_emitter.cc',
        'user_policy_service.cc',
        'user_policy_service_factory.cc',
        'validator_utils.cc',
        'vpd_process_impl.cc',
      ],
    },
    {
      'target_name': 'keygen',
      'type': 'executable',
      'sources': [
        'keygen.cc',
        'keygen_worker.cc',
        'named_platform_handle_utils_posix.cc',
        'nss_util.cc',
        'policy_key.cc',
        'system_utils_impl.cc',
      ],
    },
    {
      'target_name': 'session_manager',
      'type': 'executable',
      'libraries': [
        '-lrootdev',
        '-lcontainer',
      ],
      'dependencies': ['libsession_manager'],
      'sources': ['session_manager_main.cc'],
    },
  ],
  'conditions': [
    ['USE_test == 1', {
      'targets': [
        {
          'target_name': 'session_manager_test',
          'type': 'executable',
          'includes': ['../common-mk/common_test.gypi'],
          'defines': ['UNIT_TEST'],
          'dependencies': [
            'libsession_manager',
            '../common-mk/testrunner.gyp:testrunner',
          ],
          'variables': {
            'deps': [
              'libbrillo-test-<(libbase_ver)',
              'libchrome-test-<(libbase_ver)',
              'libcros_config',
            ],
          },
          'link_settings': {
            'libraries': [
              '-lkeyutils',
            ],
          },
          'sources': [
            'android_oci_wrapper_unittest.cc',
            'browser_job_unittest.cc',
            'child_exit_handler_unittest.cc',
            'chrome_setup_unittest.cc',
            'cumulative_use_time_metric_unittest.cc',
            'device_local_account_manager_unittest.cc',
            'device_policy_service_unittest.cc',
            'fake_browser_job.cc',
            'fake_child_process.cc',
            'fake_container_manager.cc',
            'fake_crossystem.cc',
            'fake_generated_key_handler.cc',
            'fake_generator_job.cc',
            'fake_termina_manager.cc',
            'key_generator_unittest.cc',
            'keygen_worker.cc',
            'liveness_checker_impl_unittest.cc',
            'login_metrics_unittest.cc',
            'mock_constructors.cc',
            'mock_nss_util.cc',
            'nss_util_unittest.cc',
            'policy_key_unittest.cc',
            'policy_service_unittest.cc',
            'policy_store_unittest.cc',
            'regen_mitigator_unittest.cc',
            'resilient_policy_store_unittest.cc',
            'server_backed_state_key_generator_unittest.cc',
            'session_manager_impl_unittest.cc',
            'session_manager_process_unittest.cc',
            'system_utils_unittest.cc',
            'user_policy_service_unittest.cc',
            'validator_utils_unittest.cc',
          ],
        },
      ],
    }],
  ],
}
