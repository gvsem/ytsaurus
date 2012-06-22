import unittest

from yt_env_setup import YTEnvSetup

##################################################################

class TestRunNothing(YTEnvSetup):
    NUM_MASTERS = 0
    NUM_HOLDERS = 0

    def test(self):
        assert True

class TestRunMaster(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_HOLDERS = 0

    def test(self):
        assert True

class TestRunHolder(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_HOLDERS = 1

    def test(self):
        assert True

class TestRunScheduler(YTEnvSetup):
    NUM_MASTERS = 1
    NUM_HOLDERS = 0
    NUM_SCHEDULERS = 1

    def test(self):
        assert True

