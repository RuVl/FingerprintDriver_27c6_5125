"""In-process TLS-PSK server for the Goodix MCU (the MCU is the TLS client).

Replaces the external `openssl s_server` used by goodix-fp-dump: the
handshake runs over ssl.MemoryBIO and records are shuttled through the
Goodix message pack with flags 0xb0.
"""

import ssl

from . import goodix

CIPHER = "PSK-AES128-GCM-SHA256"


class TLSServer:

    def __init__(self, psk: bytes):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.maximum_version = ssl.TLSVersion.TLSv1_2
        context.set_ciphers(f"{CIPHER}:@SECLEVEL=0")
        context.set_psk_server_callback(lambda identity: psk)

        self.incoming = ssl.MemoryBIO()
        self.outgoing = ssl.MemoryBIO()
        self.ssl = context.wrap_bio(self.incoming, self.outgoing,
                                    server_side=True)

    def _step(self):
        try:
            self.ssl.do_handshake()
            return True
        except ssl.SSLWantReadError:
            return False

    def connect(self, device: goodix.Device):
        """Run the handshake with the MCU, mirroring tool.connect_device."""
        self.incoming.write(device.request_tls_connection())  # ClientHello
        self._step()
        device.protocol.write(
            goodix.encode_message_pack(self.outgoing.read(),
                                       goodix.FLAGS_TRANSPORT_LAYER_SECURITY))

        # ClientKeyExchange, ChangeCipherSpec, Finished
        for _ in range(3):
            self.incoming.write(
                goodix.check_message_pack(
                    device.protocol.read(),
                    goodix.FLAGS_TRANSPORT_LAYER_SECURITY))

        if not self._step():
            raise ssl.SSLError("handshake did not complete")

        device.protocol.write(
            goodix.encode_message_pack(self.outgoing.read(),
                                       goodix.FLAGS_TRANSPORT_LAYER_SECURITY))

    def decrypt(self, record: bytes) -> bytes:
        self.incoming.write(record)
        chunks = []
        while True:
            try:
                chunks.append(self.ssl.read(0x10000))
            except ssl.SSLWantReadError:
                break
        return b"".join(chunks)
