# Usage:
#   ruby tool/generate_mjit_header.rb <source>.c <compiler> <args...>
#
# This will output preprocessed <source>.c to <source>.i.

src = ARGV.shift
if ARGV.size < 2 || !src.end_with?('.c')
  $stderr.puts "Usage:"
  $stderr.puts "  ruby tool/generate_mjit_header.rb <source>.c <compiler> <args...>"
end

cc = ARGV.shift
dest = src.sub(/\.c\z/, '.i')
args = [src, *ARGV]

case cc
when /\Acl(|\.exe)\z/
  # cl.exe outputs result to *.i
  args.push('-P')
else # for gcc, clang
  args.push('-o', dest, '-E', '-dD') # -dD is for clang
end

Process.exec(cc, *args)
