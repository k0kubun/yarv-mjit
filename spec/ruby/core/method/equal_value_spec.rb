require File.expand_path('../../../spec_helper', __FILE__)
require File.expand_path('../shared/eql', __FILE__)

describe "Method#==" do
  it_behaves_like :method_equal, :==
end
