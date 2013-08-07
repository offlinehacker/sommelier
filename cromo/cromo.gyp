{
  'target_defaults': {
    'dependencies': [
      '../libchromeos/libchromeos-<(libbase_ver).gyp:libchromeos-<(libbase_ver)',
      '../metrics/metrics.gyp:libmetrics',
    ],
    'variables': {
      'deps': [
        'dbus-c++-1',
        'gthread-2.0',
        'gobject-2.0',
        'libchrome-<(libbase_ver)',
      ],
    },
    'link_settings': {
      'libraries': [
        '-lgflags',
        '-ldl',
        '-lminijail',
      ],
    },
  },
  'targets': [
    {
      'target_name': 'cromo-adaptors',
      'type': 'none',
      'variables': {
        'xml2cpp_type': 'adaptor',
        'xml2cpp_in_dir': 'local-xml',
        'xml2cpp_out_dir': 'include/cromo/dbus_adaptors',
      },
      'sources': [
        '<(xml2cpp_in_dir)/org.freedesktop.DBus.Properties.xml',
      ],
      'includes': ['../common-mk/xml2cpp.gypi'],
    },
    {
      'target_name': 'libcromo',
      'type': 'static_library',
      'standalone_static_library': 1, # tells GYP to not make this a 'thin' library
      'dependencies': [
        '../common-mk/external_dependencies.gyp:modemmanager-dbus-adaptors',
        'cromo-adaptors',
      ],
      'sources': [
        'cromo_server.cc',
        'hooktable.cc',
        'modem_handler.cc',
        'sms_message.cc',
        'sms_cache.cc',
        'syslog_helper.cc',
        'utilities.cc',
      ],
    },
    {
      'target_name': 'cromo',
      'type': 'executable',
      'dependencies': ['libcromo'],
      'ldflags': [
        '-Wl,--dynamic-list-cpp-typeinfo,--dynamic-list=<(platform_root)/cromo/cromo.ver',
      ],
      'sources': [
        'carrier.cc',
        'main.cc',
        'plugin_manager.cc',
        'sandbox.cc',
      ],
    },
  ],
  'conditions': [
    ['USE_test == 1', {
      'targets': [
        {
          'target_name': 'dummy_modem',
          'type': 'shared_library',
          'dependencies': ['../common-mk/external_dependencies.gyp:modemmanager-dbus-adaptors'],
          'sources': [
            'dummy_modem.cc',
            'dummy_modem_handler.cc',
          ],
        },
        {
          'target_name': 'cromo_server_unittest',
          'type': 'executable',
          'includes': ['../common-mk/common_test.gypi'],
          'sources': [
            'carrier.cc',
            'cromo_server_unittest.cc',
            'cromo_server.cc',
            'hooktable.cc',
            'syslog_helper.cc',
          ]
        },
        {
          'target_name': 'utilities_unittest',
          'type': 'executable',
          'dependencies': ['libcromo'],
          'includes': ['../common-mk/common_test.gypi'],
          'sources': [
            'utilities_unittest.cc',
          ]
        },
        {
          'target_name': 'sms_message_unittest',
          'type': 'executable',
          'dependencies': ['libcromo'],
          'includes': ['../common-mk/common_test.gypi'],
          'sources': [
            'sms_message_unittest.cc',
          ]
        },
        {
          'target_name': 'sms_cache_unittest',
          'type': 'executable',
          'dependencies': ['libcromo'],
          'includes': ['../common-mk/common_test.gypi'],
          'sources': [
            'sms_cache_unittest.cc',
          ]
        },
      ],
    }],
  ],
}
