from yt_env_setup import YTEnvSetup

from yt_commands import authors, get

from yt.yson import YsonEntity

##################################################################


class TestErrorCodes(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_NODES = 1
    NUM_SCHEDULERS = 1

    @authors("achulkov2")
    def test_basic_error_codes(self):
        assert get("//sys/scheduler/orchid/error_codes/116") == "NYT::NRpc::EErrorCode::TransientFailure"

    @authors("achulkov2")
    def test_attribute_is_opaque(self):
        full_attributes = get("//sys/scheduler/orchid")
        assert full_attributes["error_codes"] == YsonEntity()
