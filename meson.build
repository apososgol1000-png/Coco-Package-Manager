project('coco', 'c',
  version : '0.3.0',
  default_options : ['warning_level=2', 'optimization=2', 'c_std=c11'])

cc = meson.get_compiler('c')

# gcc no logra rastrear el bound de char g_repos[MAX_REPOS][MAX_URL] a través del
# puntero que decae al pasarlo; los snprintf ya están correctamente acotados.
add_project_arguments(cc.get_supported_arguments('-Wno-format-truncation'), language : 'c')

curl_dep    = dependency('libcurl')
archive_dep = dependency('libarchive')
sqlite_dep  = dependency('sqlite3')
jansson_dep = dependency('jansson')

src = files(
  'src/main.c',
  'src/net.c',
  'src/pkg.c',
)

executable('coco', src,
  dependencies : [curl_dep, archive_dep, sqlite_dep, jansson_dep],
  install       : true,
  install_dir   : get_option('prefix') / 'bin')
