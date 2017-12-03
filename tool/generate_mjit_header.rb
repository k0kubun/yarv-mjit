# Usage:
#   ruby tool/generate_mjit_header.rb <builddir> <source>.c <compiler> <args...>
#
# This will output preprocessed <source>.c to <builddir>/<source>.i.

require 'shellwords'

builddir = ARGV.shift
src = ARGV.shift
if ARGV.size < 2 || !src.end_with?('.c')
  $stderr.puts "Usage:"
  $stderr.puts "  ruby tool/generate_mjit_header.rb <builddir> <source>.c <compiler> <args...>"
end

cc = ARGV.shift
args = [src, *ARGV]
dest = File.join(builddir, File.basename(src).sub(/\.c\z/, '.i'))

suffix = ''
case cc
when /\Acl(|\.exe)\z/
  # cl.exe outputs result to *.i
  args.push('-P', '-EP')

  # workaround: This manually preserves macros only for cl.exe in a VERY rough way.
  # This is needed because cl.exe can't keep macro definitions with /P.
  %w[
    ../include/ruby/ruby.h
    ../vm_core.h
  ].each do |f|
    header = File.read(File.expand_path(f, __dir__))
    suffix << header.gsub(/^(?!#define ).+$/, '').gsub(/^.+\\$/, '')
  end
else # for gcc, clang
  args.push('-o', dest, '-E', '-dD') # -dD is for clang
end

cmd = [cc, *args].shelljoin
unless system(cmd)
  abort "Failed to execute: #{cmd}"
end

unless suffix.empty?
  File.open(dest, 'a') do |f|
    f.puts suffix
  end
end
