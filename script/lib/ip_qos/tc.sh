#!/bin/sh

[ "$INC_QOS_TC" != "" ] && return
export INC_QOS_TC="INC"

. /lib/ip_qos/public.sh


local QDISC_C=htb # classful qdisc type that we use
local DEFAULT_HTB=ffff
local R2Q_HTB=10
local PRIO_HTB_DEFAULT=7 # lowest priority of htb class
local PRIO_FILTER_DEFAULT=2
local PRIO_FILTER=3
export IFB_DEV=ifb0

local MAX_RATE="`echo $RATE_MAX | sed 's/[^0-9]//g'`kbit"
local MAX_BURST=2500000  # ((1000*1000*1000/100)/8)*2

# kmods insmod and rmmod
qos_kmod_load()
{
    local ko_path="/lib/modules/`uname -r`"
    [ `lsmod | grep -c sch_htb` -eq 0 ] && insmod ${ko_path}/sch_htb.ko
    [ `lsmod | grep -c cls_fw` -eq 0 ] && insmod ${ko_path}/cls_fw.ko
}

qos_kmod_unload()
{
    local ko_path="/lib/modules/`uname -r`"
    [ `lsmod | grep -c sch_htb` -gt 0 ] && rmmod ${ko_path}/sch_htb.ko
    [ `lsmod | grep -c cls_fw` -gt 0 ] && rmmod ${ko_path}/cls_fw.ko
}

# get netdev MTU
dev_mtu_get()
{
    local dev=$1
    local mtu=`ip addr | grep $dev[:@] | awk '{for(i=1;i<NF;i++) {if ($i==pp) {print $(i+1);exit}}}' pp="mtu"`
    QOS_RET $mtu
}

# get qdisc htb burst from rate and mtu
htb_burst_get()
{
    local dev=$1
    local rate=$2
    local burst=0
    local mtu=$(dev_mtu_get $dev)

    # cut of the unit
    rate=`echo $rate | sed 's/[ ]//g' | sed 's/b.*//g'`
    rate=`echo $rate | sed 's/[Kk]/000/'`
    rate=`echo $rate | sed 's/[Mm]/000000/'`
    rate=`echo $rate | sed 's/[Gg]/000000000/'`

    burst=$(($rate/8/50)) # ((rate/100)/8)*2
    [ $burst -lt $mtu ] && burst=$mtu
    [ $burst -gt $MAX_BURST ] && burst=$MAX_BURST

    QOS_RET "$burst"
}

# destroy tc tree on all LAN/WAN netdevs
qos_tc_destroy()
{
    for iface in $IFACE_LIST; do
        local dev=$(zone_get_bind_device ${iface})
        tc qdisc del dev $dev root 2>/dev/null
    done

    tc qdisc del dev $IFB_DEV root 2>/dev/null

    QOS_WRITE_LOG " qos_tc_destroy, flush tc tree."
}

qos_tc_init()
{
    local rule_list=`cat $QOS_STATE_FILE | grep "install=true" | awk -F '.' '{print $1}'`
    [ ${#rule_list} -eq 0 ] && return

    for rule_name in $rule_list; do
        if_ping=`QOS_STATE get ${rule_name}.if_ping`
        if_pong=`QOS_STATE get ${rule_name}.if_pong`
        qos_tc_dev_build_root $if_ping $if_pong 0
        qos_tc_dev_build_root $if_pong $if_ping 1
    done
}

# check if dev qdisc root has been init
# make sure we shall not creat qdisc root twice for this dev
qos_tc_dev_check_root()
{
    local dev="$1"
    local ret=0
    [ "$QDISC_C" = "`tc -p qdisc show dev $dev | awk 'NR==1 {print $2}'`" ] && ret=1
    QOS_RET "$ret"
}

qos_tc_dev_init()
{
    local dev=$1
    local isWAN=$2

    # if root qdisc already exist, just return
    [ $(qos_tc_dev_check_root $dev) -ne 0 ] && {
        QOS_WRITE_LOG " qos_tc_dev_init, tc root on dev $dev was init before, just return." && return
    }

    tc qdisc del dev $dev root 2>/dev/null
    QOS_WRITE_LOG " tc qdisc add dev $dev root handle 1:0 $QDISC_C default $DEFAULT_HTB r2q $R2Q_HTB"
    tc qdisc add dev $dev root handle 1:0 $QDISC_C default $DEFAULT_HTB r2q $R2Q_HTB

    [ $isWAN = "yes" -a $WAN_MODE_ONE != "1" ] && {
        # create a filter on root, redirect to ifb dev
        QOS_WRITE_LOG " tc filter add dev $dev parent 1:0 prio $PRIO_FILTER_DEFAULT u32 match u32 0 0 flowid 1:$DEFAULT_HTB action mirred egress redirect dev $IFB_DEV"
        tc filter add dev $dev parent 1:0 prio $PRIO_FILTER_DEFAULT u32 match u32 0 0 flowid 1:$DEFAULT_HTB action mirred egress redirect dev $IFB_DEV
    }
}

qos_tc_dev_build_root()
{
    local if_src="$1"
    local if_dst="$2"
    local dir="$3"
    local iface_build
    local dev
    local pid


    # dir is 0 for up and is 1 for down
    if [ $dir = 0 ]; then
        iface_build="$if_dst"
        dir="uplink"
        if [ $iface_build = "WAN_ALL" ]; then
            for iface in $IFACE_LIST; do
                dev=$(zone_get_bind_device ${iface})
                [ $(valid_dev $dev) -ne 0 ] && QOS_WRITE_LOG " qos_tc_dev_build_root, $dev invalid, skip." && continue
                [ $iface = "$LAN_IFACE_NAME" ] && continue
                qos_tc_dev_init "$dev" "yes"
            done
        else
            dev=$(zone_get_bind_device ${iface_build})
            qos_tc_dev_init "$dev" "yes"
        fi
        if [ $WAN_MODE_ONE != "1" -a $iface_build != "WAN_ALL" ]; then
            [ $(qos_tc_dev_check_root $IFB_DEV) -ne 0 ] && {
                QOS_WRITE_LOG " qos_tc_dev_init, tc root on dev $IFB_DEV was init before, just return." && return
            }
            qos_tc_dev_init "$IFB_DEV" "no"
            QOS_WRITE_LOG " tc class add dev $IFB_DEV parent 1:0 classid 1:1 htb rate $MAX_RATE ceil $MAX_RATE burst $MAX_BURST cburst $MAX_BURST"
            tc class add dev $IFB_DEV parent 1:0 classid 1:1 htb rate $MAX_RATE ceil $MAX_RATE burst $MAX_BURST cburst $MAX_BURST
            for iface in $IFACE_LIST; do
                [ $iface != "LAN" ] && {
                    local rootrate="`echo $RATE_MAX | sed 's/[^0-9]//g'`kbit"
                    local rootceil="`get_iface_linerate "$iface" "$dir" | sed 's/[^0-9]//g'`kbit"
                    local burst="`htb_burst_get $IFB_DEV $rootrate`"
                    local cburst="`htb_burst_get $IFB_DEV $rootceil`"
                    pid=`echo $iface | sed 's/[^0-9]//g'`
                    pid=$((pid+1))
                    QOS_WRITE_LOG " tc class add dev $IFB_DEV parent 1:1 classid 1:$pid htb rate $rootrate ceil $rootceil burst $burst cburst $cburst"
                    tc class add dev $IFB_DEV parent 1:1 classid 1:$pid htb rate $rootrate ceil $rootceil burst $burst cburst $cburst
                }
            done
        elif [ $WAN_MODE_ONE != "1" -a $iface_build = "WAN_ALL" ]; then
            [ $(qos_tc_dev_check_root $IFB_DEV) -ne 0 ] && {
                QOS_WRITE_LOG " qos_tc_dev_init, tc root on dev $IFB_DEV was init before, just return." && return
            }
            qos_tc_dev_init "$IFB_DEV" "no"
            local rootceil
            for iface in $IFACE_LIST; do
                [ $iface != "LAN" ] && {
                    local ceil="`get_iface_linerate "$iface" "$dir" | sed 's/[^0-9]//g'`"
                    rootceil=$((rootceil+ceil))
                }
            done
            local MAX="${MAX_RATE//[^0-9]/}"
            [ $rootceil -gt $MAX ] && rootceil=$MAX
            rootceil="`echo $rootceil | sed 's/[^0-9]//g'`kbit"
            local cburst="`htb_burst_get $IFB_DEV $rootceil`"
            QOS_WRITE_LOG " tc class add dev $IFB_DEV parent 1:0 classid 1:1 htb rate $rootceil ceil $MAX_RATE burst $MAX_BURST cburst $cburst"
            tc class add dev $IFB_DEV parent 1:0 classid 1:1 htb rate $MAX_RATE ceil $rootceil burst $MAX_BURST cburst $cburst
        else
            local rootceil="`get_iface_linerate "$WAN_MODE_ONE_IFACE_NAME" "$dir" | sed 's/[^0-9]//g'`kbit"
            local cburst="`htb_burst_get $dev $rootceil`"
            QOS_WRITE_LOG " tc class add dev $dev parent 1:0 classid 1:1 htb rate $MAX_RATE ceil $rootceil burst $MAX_BURST cburst $cburst"
            tc class add dev $dev parent 1:0 classid 1:1 htb rate $MAX_RATE ceil $rootceil burst $MAX_BURST cburst $cburst
        fi
    else
        iface_build="$if_src"
        dev=$(zone_get_bind_device ${LAN_IFACE_NAME})
        [ $(valid_dev $dev) -ne 0 ] && QOS_WRITE_LOG " qos_tc_dev_build_root, $dev invalid, skip." && return
        [ $(qos_tc_dev_check_root $dev) -ne 0 ] && {
            QOS_WRITE_LOG " qos_tc_dev_init, tc root on dev $dev was init before, just return." && return
        }
        qos_tc_dev_init "$dev" "no"
        dir="downlink"
        if [ $WAN_MODE_ONE != "1" -a $iface_build != "WAN_ALL" ]; then
            QOS_WRITE_LOG " tc class add dev $dev parent 1:0 classid 1:1 htb rate $MAX_RATE ceil $MAX_RATE burst $MAX_BURST cburst $MAX_BURST"
            tc class add dev $dev parent 1:0 classid 1:1 htb rate $MAX_RATE ceil $MAX_RATE burst $MAX_BURST cburst $MAX_BURST
            for iface in $IFACE_LIST; do
                [ $iface != "LAN" ] && {
                    local rootrate="`echo $RATE_MAX | sed 's/[^0-9]//g'`kbit"
                    local rootceil="`get_iface_linerate "$iface" "$dir" | sed 's/[^0-9]//g'`kbit"
                    local burst="`htb_burst_get $dev $rootrate`"
                    local cburst="`htb_burst_get $dev $rootceil`"
                    pid=`echo $iface | sed 's/[^0-9]//g'`
                    pid=$((pid+1))
                    QOS_WRITE_LOG " tc class add dev $dev parent 1:1 classid 1:$pid htb rate $rootrate ceil $rootceil burst $burst cburst $cburst"
                    tc class add dev $dev parent 1:1 classid 1:$pid htb rate $rootrate ceil $rootceil burst $burst cburst $cburst
                }
            done
        elif [ $WAN_MODE_ONE != "1" -a $iface_build = "WAN_ALL" ]; then
            local rootceil
            for iface in $IFACE_LIST; do
                [ $iface != "LAN" ] && {
                    local ceil="`get_iface_linerate "$iface" "$dir" | sed 's/[^0-9]//g'`"
                    rootceil=$((rootceil+ceil))
                }
            done
            local MAX="${MAX_RATE//[^0-9]/}"
            [ $rootceil -gt $MAX ] && rootceil=$MAX
            rootceil="`echo $rootceil | sed 's/[^0-9]//g'`kbit"
            local cburst="`htb_burst_get $dev $rootceil`"
            QOS_WRITE_LOG " tc class add dev $dev parent 1:0 classid 1:1 htb rate $rootceil ceil $MAX_RATE burst $MAX_BURST cburst $cburst"
            tc class add dev $dev parent 1:0 classid 1:1 htb rate $MAX_RATE ceil $rootceil burst $MAX_BURST cburst $cburst
        else
            local rootrate="`echo $RATE_MAX | sed 's/[^0-9]//g'`kbit"
            local rootceil="`get_iface_linerate "$WAN_MODE_ONE_IFACE_NAME" "$dir" | sed 's/[^0-9]//g'`kbit"
            local burst="`htb_burst_get $dev $rootrate`"
            local cburst="`htb_burst_get $dev $rootceil`"
            QOS_WRITE_LOG " tc class add dev $dev parent 1:0 classid 1:1 htb rate $rootrate ceil $rootceil burst $burst cburst $cburst"
            tc class add dev $dev parent 1:0 classid 1:1 htb rate $rootrate ceil $rootceil burst $burst cburst $cburst
        fi
    fi
}
