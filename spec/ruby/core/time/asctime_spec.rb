require File.expand_path('../../../spec_helper', __FILE__)
require File.expand_path('../shared/asctime', __FILE__)

describe "Time#asctime" do
  it_behaves_like :time_asctime, :asctime
end
