project(
  'nix-eval-jobs',
  'cpp',
  version: '0.1.6',
  license: 'GPL-3.0',
  default_options: [
    # c++23 to match the v3 nix headers (nix-expr-v3 exports -std=c++23);
    # keeping a single consistent standard avoids a mixed -std across TUs.
    'cpp_std=c++23',
    'warning_level=3'
  ]
)

threads_dep = dependency('threads', required: true)
nlohmann_json_dep = dependency('nlohmann_json', required: true)
libcurl_dep = dependency('libcurl', required: true)

nix_store_dep = dependency('nix-store', required: true)
nix_fetchers_dep = dependency('nix-fetchers', required: true)
nix_expr_dep = dependency('nix-expr', required: true)
nix_flake_dep = dependency('nix-flake', required: true)
nix_main_dep = dependency('nix-main', required: true)
nix_cmd_dep = dependency('nix-cmd', required: true)
# Model B (NIX_EVAL_JOBS_PATCH_2026-07-18): the v3 bytecode-VM accessor
# surface (include/v3/eval_jobs_api.hh).  Discovered via pkg-config from the
# v3 nix's installed prefix (nix-expr-v3.pc).
nix_expr_v3_dep = dependency('nix-expr-v3', required: true)

subdir('src')

gtest_dep = dependency('gtest', main: true, required: false)
if gtest_dep.found()
  subdir('tests-unit')
endif
