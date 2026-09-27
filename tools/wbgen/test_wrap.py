import hashlib
import os

import pytest

from wbgen.wrap import DLL, Emulator

# Zero-PSK white box as written by the Windows driver: TLV 0xbb010003 inside
# the preset_psk_write packet captured by available314/goodix-27c6-5125
# (src/constants.c, PRESET_PSK_WRITE_R_BUF).
WINDOWS_ZERO_PSK_BLOB = bytes.fromhex(
    "194d152ea08bcca2d886538be4ca6b15c6097848e56cfb6341278f1537fb029b"
    "02ff20000000"
    "aefe57607c64a6e63b1f6d9a7b73f6160c00f3e899251599207fca1503aa098f"
    "5f9c92f2f7d54a1383513e4877a48f5fe4946975d486475e9775e0e0530ee9f4")

pytestmark = pytest.mark.skipif(not DLL.exists(), reason="gfusb.dll missing")


@pytest.fixture(scope="module")
def emulator():
    return Emulator()


def test_zero_psk_matches_windows_capture(emulator):
    assert emulator.wrap(bytes(32)) == WINDOWS_ZERO_PSK_BLOB


def test_zero_psk_hash():
    assert hashlib.sha256(WINDOWS_ZERO_PSK_BLOB).hexdigest() == (
        "b5e0beeb94c84eb99b883abd5c251073c56b91035c562a91a46c7f3349c36c89")


def test_deterministic_and_psk_dependent(emulator):
    psk = os.urandom(32)
    blob = emulator.wrap(psk)
    assert blob == emulator.wrap(psk)
    assert blob != emulator.wrap(bytes(32))


def test_blob_layout(emulator):
    blob = emulator.wrap(os.urandom(32))
    assert len(blob) == 102
    assert blob[32:38] == b"\x02\xff\x20\x00\x00\x00"
