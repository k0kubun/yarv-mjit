# Copyright (C) 2017 Vladimir Makarov, <vmakarov@redhat.com>
# This is a script to transform functions to static inline.
# The script puts the result into stdout.

require 'tempfile'

module MJITHeader
  ATTR_VALUE_REGEXP  = /[^()]|\([^()]*\)/
  ATTR_REGEXP        = /__attribute__\s*\(\((#{ATTR_VALUE_REGEXP})*\)\)/
  FUNC_HEADER_REGEXP = /\A(\s*#{ATTR_REGEXP})*[^\[{(]*\((#{ATTR_REGEXP}|[^()])*\)(\s*#{ATTR_REGEXP})*\s*/

  # Return start..stop of last (N>=1) decls in CODE ending STOP
  def self.find_decl(code, stop, n)
    level = curr = start = 0

    stop.downto(0) do |i|
      if level == 0 && (i == 0 || code[i] == ';' || code[i] == '}')
        start = i
        curr += 1 if stop != start
        start = -1 if i == 0 && code[i] != ';' && code[i] != '}'
        return start + 1..stop if curr == n
        level += 1 if code[i] == '}'
      elsif code[i] == '}'
        level += 1
      elsif code[i] == '{'
        level -= 1
      end
    end
    0..-1
  end

  # Given DECL return the name of it, nil if failed
  def self.decl_name_of(decl)
    ident_regex = /\w+/
    reduced_decl = decl.gsub(/#{ATTR_REGEXP}/, '') # remove attributes
    su1_regex = /{[^{}]*}/
    su2_regex = /{([^{}]|su1_regex)*}/
    su3_regex = /{([^{}]|su2_regex)*}/ # 3 nested structs/unions is probably enough
    reduced_decl.gsub!(/#{su3_regex}/, '') # remove strutcs/unions in the header
    id_seq_regex = /\s*(#{ident_regex}(\s+|\s*[*]\s*))*/
    # Process function header:
    match = /\A#{id_seq_regex}(?<name>#{ident_regex})\s*\(/.match(reduced_decl)
    return match[:name] if match
    # Process non-function declaration:
    reduced_decl.gsub!(/\s*=[^;]+(?=;)/, '') # remove initialization
    match = /#{id_seq_regex}(?<name>#{ident_regex})/.match(reduced_decl);
    return match[:name] if match
    nil
  end

  # Return true if CC with CFLAGS compiles successfully the current code.
  # Use STAGE in the message in case of a compilation failure
  def self.check_code!(code, cc, cflags, stage:)
    Tempfile.create("") do |f|
      f.puts code
      f.close

      unless system("#{cc} #{cflags} #{f.path} 2>/dev/null")
        STDERR.puts "error in #{stage} header file:"
        system("#{cc} #{cflags} #{f.path}")
        exit 1
      end
    end
  end
end

if ARGV.size != 2
  STDERR.puts 'Usage: <c-compiler> <header file> > out'
  exit 1
end

cc     = ARGV[0]
code   = File.read(ARGV[1]) # Current version of the header file.
cflags = '-S -DMJIT_HEADER -fsyntax-only -Werror=implicit-function-declaration -Werror=implicit-int -Wfatal-errors'

# Check initial file correctness
MJITHeader.check_code!(code, cc, cflags, stage: 'initial')
STDERR.puts "\nTransforming external function to static:"

stop_pos     = code.length - 1
extern_names = []

# This loop changes function declarations to static inline.
loop do
  decl_range = MJITHeader.find_decl(code, stop_pos, 1)
  break if decl_range.end < 0

  stop_pos = decl_range.begin - 1
  decl = code[decl_range]
  decl_name = MJITHeader.decl_name_of(decl)

  if extern_names.include?(decl_name) && (decl =~ /#{MJITHeader::FUNC_HEADER_REGEXP};/)
    decl.sub!(/extern|static|inline/, '')
    STDERR.puts "warning: making declaration of '#{decl_name}' static inline:"

    code[decl_range] = "static inline #{decl}"
  elsif (match = /#{MJITHeader::FUNC_HEADER_REGEXP}{/.match(decl)) && (header = match[0]) !~ /static/
    extern_names << decl_name
    decl[match.begin(0)...match.end(0)] = ''

    if decl =~ /static/
      STDERR.puts "warning: a static decl inside external definition of '#{decl_name}'"
    end

    header.sub!(/extern|inline/, '')
    STDERR.puts "warning: making external definition of '#{decl_name}' static inline:"
    code[decl_range] = "static inline #{header}#{decl}"
  end
end

# Check the final file correctness
MJITHeader.check_code!(code, cc, cflags, stage: 'final')

puts code # Output the result
