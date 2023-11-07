# Copyright (c) 2022-2023, PostgreSQL Global Development Group

tdeoutput_sources = files(
  'tdeoutput.c',
)

incdir = include_directories('../../src/include')

tdeoutput = shared_module('tdeoutput',
  tdeoutput_sources,
  include_directories: incdir,
  kwargs: contrib_mod_args,
)

contrib_targets += tdeoutput
