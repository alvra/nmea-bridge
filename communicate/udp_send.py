#!/usr/bin/env python3.8

import socket
from ipaddress import ip_address
import argparse


def build_parser():
    parser = argparse.ArgumentParser(
        description="Send a udp message.")
    parser.add_argument(
        'type',
        choices=['unicast', 'multicast', 'broadcast'],
        help="the type of transmission")
    parser.add_argument(
        'data',
        help="the data to send")
    parser.add_argument(
        '--ip',
        type=ip_address,
        default=None,
        help="the remote ip address")
    parser.add_argument(
        '--port', '-p',
        type=int,
        default=10110,
        help="the remote port")
    parser.add_argument(
        '--timeout', '-t',
        type=int)
    return parser


def create_socket(timeout=None, ttl=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    if ttl is not None:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, ttl)
    if timeout is not None:
        s.settimeout(timeout)
    return s


def send_unicast(ip, port, data, *, timeout=None, ttl=None):
    s = create_socket(timeout, ttl)
    s.sendto(data, (ip, port))


def send_multicast(ip, port, data, *, timeout=None, ttl=1):
    # https://stackoverflow.com/questions/603852/how-do-you-udp-multicast-in-python
    s = create_socket(timeout, ttl)
    s.sendto(data, (ip, port))


def send_broadcast(port, data, *, ip='<broadcast>', timeout=None, ttl=None):
    # https://gist.github.com/ninedraft/7c47282f8b53ac015c1e326fffb664b5
    s = create_socket(timeout, ttl)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.sendto(data, (ip, port))


def main(argv):
    parser = build_parser()
    args = parser.parse_args(argv[1:])
    try:
        data = args.data.encode('ascii')
    except UnicodeEncodeError:
        print("Invalid data")
        return 1
    kwargs = dict(timeout=args.timeout)
    if args.type == 'unicast':
        if args.ip is None:
            print("An IP Address is required")
            return 1
        send_unicast(str(args.ip), args.port, data, **kwargs)
    elif args.type == 'multicast':
        ip = args.ip
        if ip is None:
            ip = ip_address('224.1.1.1')
        if not ip.is_multicast:
            print("Ip address is not multicast")
            return 1
        send_multicast(str(ip), args.port, data, **kwargs)
    elif args.type == 'broadcast':
        send_broadcast(args.port, data, ip=str(args.ip), **kwargs)
    else:
        assert False
    return 0


if __name__ == '__main__':
    import sys
    sys.exit(main(sys.argv))
