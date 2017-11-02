#!/bin/sh

local configdir="/etc/config"
local file="qos"
local tmpdir="/tmp/qos"

[ ! -e $tmpdir ] && mkdir -p $tmpdir

cp -f $configdir/$file $tmpdir/$file

case ${ACTION} in
	ADDBR)
		[ -n "${CONFIF2}" -a -n "${BRIF}" ] && {
			cnt_ping=`sed -rn "/if_ping\s+'*${CONFIF2}'*/p"  ${tmpdir}/$file | wc -l`
			cnt_pong=`sed -rn "/if_pong\s+'*${CONFIF2}'*/p"  ${tmpdir}/$file | wc -l`
			let cnt=cnt_ping + cnt_pong
			[ $cnt = '0' ] && return
			sed -ri "s/if_ping\s+'*${CONFIF2}'*/if_ping '${BRIF}'/g"  ${tmpdir}/$file
			sed -ri "s/if_pong\s+'*${CONFIF2}'*/if_pong '${BRIF}'/g"  ${tmpdir}/$file

			cntx=`uci get network.${CONFIF2}.t_reference`
			cntx=${cntx:-0}
			let cntx=cntx-cnt
			[ ${cntx} -lt 0 ] && cntx=0
			uci set network.${CONFIF2}.t_reference=${cntx}
			uci commit network

			cntx=`uci get network.${BRIF}.t_reference`
			cntx=${cntx:-0}
			let cntx=cnt+cntx
			uci set network.${BRIF}.t_reference=${cntx}
			uci commit network

		}
	;;
	UPDBR)
	;;
	DELBR)
	;;
	*)
	;;
esac

cp -f $tmpdir/$file  $configdir/$file