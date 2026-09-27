import os

import pytest

from fwre.fw import IMAGE
from wbgen.wrap import DLL, Emulator

pytestmark = pytest.mark.skipif(not (IMAGE.exists() and DLL.exists()),
                                reason="firmware image or gfusb.dll missing")


@pytest.mark.parametrize("psk", [bytes(32), os.urandom(32)])
def test_firmware_unwraps_generated_white_box(psk):
    from fwre.unwrap import unwrap
    status, out = unwrap(Emulator().wrap(psk))
    assert status == 0
    assert out == psk


def test_firmware_rejects_garbage():
    from fwre.unwrap import unwrap
    status, _ = unwrap(bytes(102))
    assert status != 0


def test_record_padding_aligns_crc():
    from provision import record_padding
    blob = bytes(102)
    assert (8 + len(blob) + len(record_padding(blob))) % 4 == 0
    assert record_padding(bytes(96)) == b""
