# frozen_string_literal: true
return if defined?(MJIT) && MJIT.enabled?

module ZombieHunter
  def after_teardown
    super
    assert_empty(Process.waitall)
  end
end

Test::Unit::TestCase.include ZombieHunter
