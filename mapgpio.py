#!/usr/bin/python

# Quick and dirty GPIO mapper for mk_arcade_joystick_rpi.c
# By Temia Eszteri

from __future__ import print_function


def map_gpio(group):
    "Usage: map_gpio(int[])"
    x = group.copy()
    x.sort()
    y = 0
    for i in x:
        y |= 1 << int(i)
    return y


if __name__ == '__main__':
    import sys
    print(hex(map_gpio(sys.argv[1:])))
