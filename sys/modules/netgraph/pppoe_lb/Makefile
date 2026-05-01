# $FreeBSD$
#
# Makefile for ng_pppoe_lb kernel module
#

KMOD=	ng_pppoe_lb
SRCS=	ng_pppoe_lb.c
SRCS+=	ng_pppoe_lb.h

# Module depends on netgraph framework and ng_ether
KMODDEPS=	netgraph ng_ether

.include <bsd.kmod.mk>
