#!/usr/bin/env python3.8

import socket
import struct
import argparse


DEFAULT_LENGTH = 100


def build_parser():
    parser = argparse.ArgumentParser(
        description="Receive udp messages.")
    parser.add_argument(
        'type',
        choices=['unicast', 'multicast', 'broadcast'],
        help="the type of transmission")
    parser.add_argument(
        '--ip',
        type=ip_address,
        default='',
        help="the remote ip address")
    parser.add_argument(
        '--port', '-p',
        type=int,
        default=10110,
        help="the remote port")
    parser.add_argument(
        '--length', '-l',
        default=DEFAULT_LENGTH,
        help="the length of the message")
    parser.add_argument(
        '--timeout', '-t',
        type=int)
    return parser


def create_socket(timeout=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    if timeout is not None:
        s.settimeout(timeout)
    return s


def recv_unicast(port, *, ip='', length=DEFAULT_LENGTH, timeout=None):
    s = create_socket(timeout)
    s.bind((ip, port))
    return s.recvfrom(length)


def recv_multicast(ip, port, *, all_groups=True, length=DEFAULT_LENGTH, timeout=1):
    s = create_socket(timeout)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    mreq = struct.pack("4sl", socket.inet_aton(ip), socket.INADDR_ANY)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    if all_groups:
        # on this port, receives ALL multicast groups
        s.bind(('', port))
    else:
        # on this port, listen ONLY to IP
        s.bind((ip, port))
    return s.recvfrom(length)


def recv_broadcast(port, *, ip='', length=DEFAULT_LENGTH, timeout=1):
    s = create_socket(timeout)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind((ip, port))
    return s.recvfrom(length)


def main(argv):
    parser = build_parser()
    args = parser.parse_args(argv[1:])
    try:
        data = args.data.encode('ascii')
    except UnicodeEncodeError:
        print("Invalid data")
        return 1
    kwargs = dict(length=args.length, timeout=args.timeout)
    if args.type == 'unicast':
        recv_unicast(args.port, ip=args.ip, **kwargs)
    elif args.type == 'multicast':
        ip = args.ip
        if ip is None:
            ip = ip_address('224.1.1.1')
        if not ip.is_multicast:
            print("Ip address is not multicast")
            return 1
        recv_multicast(str(ip), args.port, **kwargs)
    elif args.type == 'broadcast':
        recv_broadcast(args.port, ip=args.ip, **kwargs)
    else:
        assert False
    return 0


if __name__ == '__main__':
    import sys
    sys.exit(main(sys.argv))
