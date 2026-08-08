#!/bin/sh
#
# Build and start the slip-tun bridge for FUSE.
#
# The fifos are created by slip-tun if they do not already exist and are
# flushed every time FUSE detaches.
#
# Then, in a second console :
#   fuse --rs232-tx=/tmp/tx --rs232-rx=/tmp/rx
#
# Pass -v to slip-tun if you want a log line for every packet.
#
# As well as running the bridge this script sets up the host so that the
# Spectrum can reach the outside world and not just 192.0.2.1 :
#
#   * ip_forward, so the kernel will route between tun0 and the uplink.
#   * MASQUERADE, because 192.0.2.0/24 is TEST-NET-1 and nothing on the
#     internet will route a reply back to it.  Outgoing packets are put on
#     the wire by slip-tun with a raw IP_HDRINCL socket, so they are
#     *locally generated* and only pass through OUTPUT/POSTROUTING - never
#     FORWARD.  POSTROUTING sees them all the same, so the masquerade rule
#     is what makes them leave with the uplink's source address.
#   * FORWARD -o tun0, because the replies do come back the normal way:
#     they arrive on the uplink, conntrack undoes the masquerade back to
#     192.0.2.2, and the kernel then forwards them to tun0.  The -i tun0
#     rule is not needed today (see above) but costs nothing and keeps
#     things working if the raw socket is ever replaced by plain routing.
#
# The rules are removed again when the script exits.

TUN=tun0
NET=192.0.2.0/24

gcc -Wall -O2 slip-tun.c -o slip-tun -DSLIP_ESC_00 || exit 1

# The interface the default route points at, i.e. where we masquerade to.
UPLINK=$(ip -o route get 1.1.1.1 2>/dev/null |
         sed -n 's/.* dev \([^ ]*\).*/\1/p')
if [ -z "$UPLINK" ]; then
	echo "No default route, cannot work out the uplink interface." >&2
	exit 1
fi
echo "Masquerading $NET out of $UPLINK"

# -C tests for the rule, so re-running this script does not stack duplicates.
# The FORWARD rules are inserted at the top so they win over any DROP that
# docker or libvirt may have put in the chain.
add_rules()
{
	sudo sysctl -q -w net.ipv4.ip_forward=1

	sudo iptables -t nat -C POSTROUTING -s "$NET" ! -o "$TUN" -j MASQUERADE 2>/dev/null ||
	sudo iptables -t nat -A POSTROUTING -s "$NET" ! -o "$TUN" -j MASQUERADE

	sudo iptables -C FORWARD -o "$TUN" -j ACCEPT 2>/dev/null ||
	sudo iptables -I FORWARD 1 -o "$TUN" -j ACCEPT

	sudo iptables -C FORWARD -i "$TUN" -j ACCEPT 2>/dev/null ||
	sudo iptables -I FORWARD 1 -i "$TUN" -j ACCEPT
}

del_rules()
{
	sudo iptables -t nat -D POSTROUTING -s "$NET" ! -o "$TUN" -j MASQUERADE 2>/dev/null
	sudo iptables -D FORWARD -o "$TUN" -j ACCEPT 2>/dev/null
	sudo iptables -D FORWARD -i "$TUN" -j ACCEPT 2>/dev/null
}

# slip-tun creates tun0, so the rules have to wait for it.  They are added
# in the background once the interface shows up; slip-tun itself stays in
# the foreground so that ^C still stops it.
(
	i=0
	while [ $i -lt 50 ]; do
		if ip link show "$TUN" >/dev/null 2>&1; then
			add_rules
			exit 0
		fi
		sleep 0.1
		i=$((i + 1))
	done
	echo "$TUN never appeared, firewall rules not added." >&2
) &

trap 'del_rules' EXIT INT TERM

sudo ./slip-tun "$@" "$TUN" /tmp/rx /tmp/tx
