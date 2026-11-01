"""NAT check server tests.

The public server needs two different IPs (nncs1/nncs2), each answering on UDP 10025 and
10125; the lab uses 127.0.0.1 and 127.0.0.2. This test sends the same datagrams the game
sends and checks every reply: which socket answers, and the sender/port/IP it echoes.

Run from the repository root:
    py -3.12 -m unittest tools.tests.test_natcheck -v
"""
import asyncio
import socket
import struct
import unittest

from tools.nexlab import natcheck_server

NNCS1 = "127.0.0.1"
NNCS2 = "127.0.0.2"
CLIENT = "127.0.0.3"
PORTS = (11025, 11125)          # not the lab ports, so a running lab server is undisturbed


def reply(data):
    return struct.unpack(">IIII", data)


class NatCheckTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.transports = await natcheck_server.start((NNCS1, NNCS2), PORTS)
        self.addCleanup(self._close)
        self.loop = asyncio.get_running_loop()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((CLIENT, 0))
        self.sock.setblocking(False)
        self.client_port = self.sock.getsockname()[1]

    def _close(self):
        self.sock.close()
        for transport in self.transports:
            transport.close()

    async def exchange(self, destination, kind, expect=True):
        self.sock.sendto(struct.pack(">IIII", kind, 0, 0, 0), destination)
        try:
            data, sender = await asyncio.wait_for(self.loop.sock_recvfrom(self.sock, 64), 1.0)
        except asyncio.TimeoutError:
            if expect:
                self.fail(f"no reply for type {kind} from {destination}")
            return None
        self.assertTrue(expect, f"unexpected reply for type {kind}: {data.hex()}")
        return reply(data), sender

    async def test_types_answered_by_the_receiving_socket(self):
        for address in (NNCS1, NNCS2):
            for kind in (1, 4, 5, 101, 103):
                with self.subTest(address=address, type=kind):
                    fields, sender = await self.exchange((address, PORTS[0]), kind)
                    self.assertEqual(fields[0], kind)
                    self.assertEqual(fields[1], self.client_port)
                    self.assertEqual(socket.inet_ntoa(struct.pack(">I", fields[2])), CLIENT)
                    self.assertEqual(socket.inet_ntoa(struct.pack(">I", fields[3])), address)
                    self.assertEqual(sender[0], address)
                    self.assertEqual(sender[1], PORTS[0])
                    self.assertEqual(natcheck_server.OBSERVATIONS[CLIENT][-1][0], self.client_port)

    async def test_observations_are_kept_per_source_address(self):
        """S-02: two players checking NAT at the same time must not overwrite each other."""
        natcheck_server.OBSERVATIONS.clear()
        first = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        first.bind((CLIENT, 0))
        first.setblocking(False)
        second = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        second.bind((NNCS2, 0))
        second.setblocking(False)
        try:
            first_port, second_port = first.getsockname()[1], second.getsockname()[1]
            packet = struct.pack(">IIII", 101, 0, 0, 0)
            first.sendto(packet, (NNCS1, PORTS[0]))
            second.sendto(packet, (NNCS1, PORTS[0]))
            await asyncio.sleep(0.2)
            self.assertEqual(natcheck_server.consume(CLIENT), (CLIENT, first_port))
            self.assertEqual(natcheck_server.consume(NNCS2), (NNCS2, second_port))
            self.assertIsNone(natcheck_server.consume("127.0.0.9"))
        finally:
            first.close()
            second.close()

    async def test_same_ip_other_port_types(self):
        for kind in (3, 102):
            fields, sender = await self.exchange((NNCS1, PORTS[0]), kind)
            self.assertEqual(fields[0], kind)
            self.assertEqual(sender[0], NNCS1)
            self.assertNotIn(sender[1], PORTS)           # the alternate socket
            self.assertEqual(socket.inet_ntoa(struct.pack(">I", fields[3])), NNCS1)

    async def test_type_two_answers_from_the_other_ip(self):
        fields, sender = await self.exchange((NNCS1, PORTS[0]), 2)
        self.assertEqual(fields[0], 2)
        self.assertEqual(sender[0], NNCS2)
        self.assertNotIn(sender[1], PORTS)
        self.assertEqual(socket.inet_ntoa(struct.pack(">I", fields[3])), NNCS2)

    async def test_second_port_and_unknown_type(self):
        fields, sender = await self.exchange((NNCS2, PORTS[1]), 101)
        self.assertEqual(fields[0], 101)
        self.assertEqual(sender[1], PORTS[1])
        self.assertIsNone(await self.exchange((NNCS1, PORTS[0]), 7, expect=False))
        self.assertIsNone(await self.exchange((NNCS1, PORTS[0]), 0xDEADBEEF, expect=False))


class SingleAddressTests(unittest.IsolatedAsyncioTestCase):
    async def test_type_two_is_left_unanswered_with_one_address(self):
        transports = await natcheck_server.start((NNCS1,), PORTS)
        loop = asyncio.get_running_loop()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((CLIENT, 0))
        sock.setblocking(False)
        try:
            sock.sendto(struct.pack(">IIII", 2, 0, 0, 0), (NNCS1, PORTS[0]))
            with self.assertRaises(asyncio.TimeoutError):
                await asyncio.wait_for(loop.sock_recvfrom(sock, 64), 0.5)
            sock.sendto(struct.pack(">IIII", 101, 0, 0, 0), (NNCS1, PORTS[0]))
            data, _ = await asyncio.wait_for(loop.sock_recvfrom(sock, 64), 1.0)
            self.assertEqual(reply(data)[0], 101)
        finally:
            sock.close()
            for transport in transports:
                transport.close()


class SameSocketTests(unittest.IsolatedAsyncioTestCase):
    async def test_every_probe_is_answered_from_the_receiving_socket(self):
        transports = await natcheck_server.start((NNCS1,), PORTS, same_socket=True)
        loop = asyncio.get_running_loop()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((CLIENT, 0))
        sock.setblocking(False)
        try:
            for kind in (101, 102, 3, 2):       # 102 and 3: other port, 2: other IP (none here)
                sock.sendto(struct.pack(">IIII", kind, 0, 0, 0), (NNCS1, PORTS[0]))
                data, source = await asyncio.wait_for(loop.sock_recvfrom(sock, 64), 1.0)
                self.assertEqual(reply(data)[0], kind)
                self.assertEqual(source, (NNCS1, PORTS[0]))
        finally:
            sock.close()
            for transport in transports:
                transport.close()


if __name__ == "__main__":
    unittest.main()
