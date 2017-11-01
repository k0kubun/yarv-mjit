# frozen_string_literal: true
require 'test/unit'

class TestCompile < Test::Unit::TestCase
  NUM_CALLS_TO_ADD = 5
  SAVE_TEMPS = $DEBUG
  @unchecked_insns = %w[
    nop
    getlocal
    setlocal
    getspecial
    setspecial
    getinstancevariable
    setinstancevariable
    getclassvariable
    setclassvariable
    getconstant
    setconstant
    getglobal
    setglobal
    putnil
    putself
    putobject
    putspecialobject
    putiseq
    putstring
    concatstrings
    tostring
    freezestring
    toregexp
    intern
    newarray
    duparray
    expandarray
    concatarray
    splatarray
    newhash
    newrange
    pop
    dup
    dupn
    swap
    reverse
    reput
    topn
    setn
    adjuststack
    defined
    checkmatch
    checkkeyword
    trace
    trace2
    send
    opt_str_freeze
    opt_str_uminus
    opt_newarray_max
    opt_newarray_min
    opt_send_without_block
    invokesuper
    invokeblock
    leave
    throw
    jump
    branchif
    branchunless
    branchnil
    branchiftype
    getinlinecache
    setinlinecache
    opt_case_dispatch
    opt_plus
    opt_minus
    opt_mult
    opt_div
    opt_mod
    opt_eq
    opt_neq
    opt_lt
    opt_le
    opt_gt
    opt_ge
    opt_ltlt
    opt_aref
    opt_aset
    opt_aset_with
    opt_aref_with
    opt_length
    opt_size
    opt_empty_p
    opt_succ
    opt_not
    opt_regexpmatch1
    opt_regexpmatch2
    bitblt
    answer
    getlocal_OP__WC__0
    getlocal_OP__WC__1
    setlocal_OP__WC__0
    setlocal_OP__WC__1
    putobject_OP_INT2FIX_O_0_C_
    putobject_OP_INT2FIX_O_1_C_
  ]

  class << self
    attr_accessor :unchecked_insns

    def shutdown
      if $DEBUG
        STDERR.puts "unchecked insns are: ", *unchecked_insns
      end
    end
  end

  def unchecked_insns
    self.class.unchecked_insns
  end

  if $DEBUG
    def test_zzz_show_unchecked_insns
      STDERR.puts "", "Unchecked insns are: ", unchecked_insns
    end
  end

  def ruby(*args, verbose: 1, debug: false, **execopt)
    args = ['-e', '$>.write($<.read)'] if args.empty?
    execopt = { err: [:child, :out] } if execopt.empty?
    ruby = EnvUtil.rubybin
    f = IO.popen([ruby, *args, execopt], 'r+')
    yield(f)
  ensure
    f.close unless !f || f.closed?
  end

  def assert_insns_compile(code, *insns, debug: false, **features)
    unchecked_insns.reject! { |insn| insns.include?(insn) }
    feature_string = String.new
    features.each do |feature, enable_p|
      feature_string << ", #{feature}: #{!!enable_p}"
    end
    script = "#{<<-"begin;"}\n#{<<-"end;"}"
    begin;
      iseq = RubyVM::InstructionSequence.compile(<<-'begin;;' + "\\n" + <<-'end;;', nil, nil, 1#{feature_string})
      begin;;
        unless $0
          #{code}
        end
      end;;
      STDERR.puts iseq.disasm
      #{NUM_CALLS_TO_ADD}.times do
        iseq.eval
      end
      STDIN.getc
    end;

    options = %w[--disable-gems -j -j:v=3]
    options << "-j:s" if SAVE_TEMPS
    options << "-j:d" if debug
    ruby(*options, '-e', script) do |f|
      output = String.new
      c = 0
      while true
        l = f.gets
        break unless l
        output << l if /^(?:Unl|L)ock(?:ed|ing)/ !~ l
        break if /^MJIT warning: JIT stack exceeded its max|^MJIT warning: failure in loading code|^JIT success / =~ l
      end
      insns.each do |insn|
        assert_match /^\d+\s+#{insn}(?:$|\s)/, output
      end
      assert_match /^JIT success /, output
      f.puts
    end
  end

  def test_compile_rescue
    insns = %w[
      nop
      putnil
      trace
      leave
    ]
    code = "#{<<-"begin;"}\n#{<<-'end;'}"
    begin;
      begin
      rescue
      end
    end;

    assert_insns_compile(code, *insns)
    assert_insns_compile(code, debug: true)
  end
end
