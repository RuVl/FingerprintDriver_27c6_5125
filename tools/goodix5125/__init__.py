"""Goodix 27c6:5125 prototype.

protocol.py, goodix.py and image.py are adapted from goodix-fp-dump
(https://github.com/goodix-fp-linux-dev/goodix-fp-dump, MIT, see
LICENSE.goodix-fp-dump). Firmware erase/write/read/check commands (0xa4, 0xf0, 0xf2,
0xf4) were removed on purpose: reflashing is what bricked another 5125.
"""
