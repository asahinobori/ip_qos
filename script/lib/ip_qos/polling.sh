#!/bin/sh

# qos polling daemon

# reset mark to make include available
qos_polling_reset() {
    INC_QOS_CORE=""
    INC_QOS_NF=""
    INC_QOS_TC=""
    INC_QOS_PUBLIC=""
}

qos_polling_reset
. /lib/ip_qos/public.sh

QOS_WRITE_LOG " info, pid $$ QOS_THRESHOLD polling on devs:$DEV_LIST! "

# clear throughput tmp file (if exsit)
rm -f $QOS_TMP_DIR/ifstate_cur 2>/dev/null
rm -f $QOS_TMP_DIR/ifstate_pre 2>/dev/null

local time_unit=`uci get ${THIS_MODULE}.polling.time_unit 2>/dev/null`
local enable_time=`uci get ${THIS_MODULE}.polling.enable_time 2>/dev/null`
local disable_time=`uci get ${THIS_MODULE}.polling.disable_time 2>/dev/null`
local trust_threshold=`uci get ${THIS_MODULE}.polling.trust_threshold 2>/dev/null`

# if no polling setting, set default value as below
[ -z $time_unit ] && time_unit=60
[ -z $enable_time ] && enable_time=2
[ -z $disable_time ] && disable_time=10
[ -z $trust_threshold ] && trust_threshold=0.8

# temporarily set a shorter start and end time
time_unit=10
enable_time=1
disable_time=24

local tt=$trust_threshold

for iface in $ZONE_LIST; do
    # only calc physical interface
    # [ $iface = "WAN1" -o $iface = "WAN2" -o $iface = "WAN3" -o $iface = "WAN4" -o $iface = "LAN" ] || continue
    local ndev=$(zone_get_effect_devices $iface)
    local rx=$(awk '/'$ndev':/{print $2}' /proc/net/dev)
    local tx=$(awk '/'$ndev':/{print $10}' /proc/net/dev)
    [ ${#rx} -gt 3 ] && rx=${rx:0:$((${#rx}-3))} || rx=0
    [ ${#tx} -gt 3 ] && tx=${tx:0:$((${#tx}-3))} || tx=0
    local downlink=`uci get ${THIS_MODULE}.${iface}.downlink 2>/dev/null`
    local uplink=`uci get ${THIS_MODULE}.${iface}.uplink 2>/dev/null`
    [ -z $downlink ] && downlink=${LINERATE/[^0-9]*/}
    [ -z $uplink ] && uplink=${LINERATE/[^0-9]*/}
    indirect_var_set ${iface}_downlink $downlink
    indirect_var_set ${iface}_uplink $uplink
    echo "${ndev} rx $rx tx $tx" >> $QOS_TMP_DIR/ifstate_cur
done

while true; do
    # reset calc param
    active_flag_sum=0
    count=0
    trust_threshold=$tt


    [ -f $QOS_READY ] && count=$disable_time || count=$enable_time
    while [ $count -gt 0 ]; do
        sleep $time_unit

        active_flag=0
        cp -f $QOS_TMP_DIR/ifstate_cur $QOS_TMP_DIR/ifstate_pre 2>/dev/null
        rm -f $QOS_TMP_DIR/ifstate_cur 2>/dev/null
        for iface in $ZONE_LIST; do
            # [ $iface = "WAN1" -o $iface = "WAN2" -o $iface = "WAN3" -o $iface = "WAN4" -o $iface = "LAN" ] || continue
            ndev=$(zone_get_effect_devices $iface)
            rx=$(awk '/'$ndev':/{print $2}' /proc/net/dev)
            [ ${#rx} -gt 3 ] && rx=${rx:0:$((${#rx}-3))} || rx=0
            rx_pre=$(awk '/'$ndev\ '/{print $3}' $QOS_TMP_DIR/ifstate_pre)
            tx=$(awk '/'$ndev':/{print $10}' /proc/net/dev)
            [ ${#tx} -gt 3 ] && tx=${tx:0:$((${#tx}-3))} || tx=0
            tx_pre=$(awk '/'$ndev\ '/{print $5}' $QOS_TMP_DIR/ifstate_pre)
            downlink=`indirect_var_get ${iface}_downlink`
            uplink=`indirect_var_get ${iface}_uplink`

            rx_flag=$((rx*8*100 - rx_pre*8*100 - downlink*$QOS_THRESHOLD*$time_unit))
            tx_flag=$((tx*8*100 - tx_pre*8*100 - uplink*$QOS_THRESHOLD*$time_unit))

            [ $rx_flag -gt 0 -o $tx_flag -gt 0 ] && {
                active_flag=1
            }
            echo "${ndev} rx $rx tx $tx" >> $QOS_TMP_DIR/ifstate_cur
        done
        active_flag_sum=$(($active_flag_sum + $active_flag))

        count=$(($count-1))
    done

    [ ! -f $QOS_READY ] && count=$enable_time || {
        count=$disable_time
        trust_threshold=`echo "$trust_threshold" | awk '{print 1-$1}'`
    }
    rate=`echo "$active_flag_sum $count" | awk '{print $1/$2}'`
    active_flag=`echo "$rate $trust_threshold" | awk '{if($1<$2) {print 0} else {print 1}}'`

    [ $active_flag -gt 0 ] && {
        [ ! -f $QOS_READY ] && {
            QOS_WRITE_LOG " info, throughput overflow $QOS_THRESHOLD%, turn on qos "
            qos_polling_reset
            . /lib/ip_qos/public.sh
            usr_cmd=". /lib/ip_qos/main.sh"
            usr_cmd="$usr_cmd start"
            $usr_cmd
       }
    }

    [ $active_flag -eq 0 ] && {
        [ -f $QOS_READY ] && {
            QOS_WRITE_LOG " info, throughput lower than $QOS_THRESHOLD%, turn off qos "
            qos_polling_reset
            . /lib/ip_qos/public.sh
            usr_cmd=". /lib/ip_qos/main.sh"
            usr_cmd="$usr_cmd stop"
            $usr_cmd
        }
    }
done