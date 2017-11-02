#!/bin/sh

[ "$INC_QOS_CORE" != "" ] && return
export INC_QOS_CORE="INC"

. /lib/ip_qos/public.sh
. /lib/ip_qos/tc.sh
. /lib/ip_qos/nf.sh

qos_reset()
{
    QOS_WRITE_LOG " qos_reset start."
    # qmark reset start
    qmark -r 1

    qos_tc_destroy
    qos_tc_init

    # qmark reset end
    qmark -r 0

    # clean qos connections mark in sfe
    xfwdm -C > /dev/null 2>&1

    rm -f ${QOS_RESET_FLAG} 2>/dev/null
    QOS_WRITE_LOG " qos_reset end."
}

qos_global_init()
{
    # prepare dependence for qos - kmods, ipt-chains
    qos_kmod_load

    # setup ifb0
    ifconfig $IFB_DEV up
    ifconfig $IFB_DEV txqueuelen 1000

    # setup specification
    local ip_spec=$((QOS_CELL_SPEC-2))
    local rule_spec=$QOS_RULE_SPEC
    qmark -s "$ip_spec $rule_spec"

    if [ $WAN_MODE_ONE = "1" ]; then
        local WAN_DEV=$(zone_get_bind_device ${WAN_MODE_ONE_IFACE_NAME})
        qmark -n "$LAN_NETDEV_2 $WAN_DEV"
    else
        qmark -n "$LAN_NETDEV_2 $IFB_DEV"
    fi

    local qos_enable=""

    # if qos module is enabled ?
    config_get qos_enable "$SNAME_SETTING" "qos_enable"
    [ "$qos_enable" != "on" ] && {
        QOS_WRITE_LOG " qos_global_init, global switch is off, no need to continue."
        QOS_RET 1 && return
    }

    QOS_WRITE_LOG " qos_global_init, begin ..."

    [ ${#IFACE_LIST} -eq 0 ] && {
        QOS_WRITE_LOG " qos_global_init, no user interface find, can not config QOS."
        QOS_RET 2
        return
    }
    QOS_STATE set qos_enable "on"
    QOS_STATE set iface_list "$IFACE_LIST"
    for iface in $IFACE_LIST; do
        QOS_STATE set ${iface}.t_name $iface
        QOS_STATE set ${iface}.uplink "$(config_get $iface uplink)"
        QOS_STATE set ${iface}.downlink "$(config_get $iface downlink)"
    done

    QOS_WRITE_LOG " qos_global_init, finish."
    QOS_RET 0
}

qos_rule_install_raw()
{
    local rule_name="$1"
    local if_src="$2"
    local if_dst="$3"
    local rate_min="$4"
    local rate_max="$5"
    local flag="$6"   # if a single IP or a ip_group name, value as ip/ipset_share/ipset_priv
    local ip_or_set="$7"
    local id="$8"
    local dir="$9"

    # add netfilter(iptables) rule
    qos_nf_add_rule $rule_name $if_src $if_dst $flag $ip_or_set $id $dir

    return 0
}

qos_rule_install()
{
    local rule_name="$1"

    QOS_WRITE_LOG " qos_rule_install, install rule $rule_name ..."
    [ "$(QOS_STATE get ${rule_name}.install)" = "true" ] && {
        QOS_WRITE_LOG " qos_rule_install, rule $rule_name has alreay install, skip !"
        QOS_RET 0
        return 1
    }

    config_get if_ping "$rule_name" "if_ping"
    config_get if_pong "$rule_name" "if_pong"
    [ ${#if_ping} -eq 0 -o ${#if_pong} -eq 0 ] && {
        QOS_WRITE_LOG " qos_rule_install, if_ping or if_pong not set, skip."
        QOS_RET 0
        return 1
    }

    # get all options from a rule section
    config_get rate_max "$rule_name" "rate_max"
    [ "$rate_max" = "" ] && rate_max=0
    rate_max=${rate_max/[^0-9]*/}

    config_get rate_min "$rule_name" "rate_min"
    [ "$rate_min" = "" ] && rate_min=10
    rate_min=${rate_min/[^0-9]*/}

    config_get downrate_max "$rule_name" "rate_max_mate"
    [ "$downrate_max" = "" ] && downrate_max=0
    downrate_max=${downrate_max/[^0-9]*/}

    config_get downrate_min "$rule_name" "rate_min_mate"
    [ "$downrate_min" = "" ] && downrate_min=10
    downrate_min=${downrate_min/[^0-9]*/}

    config_get ip_group "$rule_name" "ip_group"
    if [ "$ip_group" != "" ]; then
        [ "$ip_group" = "SELF_DEFINE" ] && {
            # register self define ip group, let group name same as rule name
            ip_group=${rule_name}
            [ `ipset list -n | grep -cw "$ip_group"` -eq 0 ] && {
                QOS_WRITE_LOG " Register self define ip_group ${ip_group} into ipset"
                config_get ipstart "$rule_name" "scope_start"
                config_get ipend "$rule_name" "scope_end"
                `ipset create ${ip_group} hash:net -exist`
                `ipset flush ${ip_group}`
                `ipset add ${ip_group} "${ipstart}-${ipend}" -exist`
            }
        }
        [ "$ip_group" != "IPGROUP_ANY" ] && {
            [ `ipset list -n | grep -cw "$ip_group"` -eq 0 ] && QOS_WRITE_LOG " qos_rule_install, rule ip_group $ip_group not exist in system." && QOS_RET 0 && return 1
        }
    else
        QOS_WRITE_LOG " qos_rule_install, ip_group not found, next." && {
            QOS_RET 0
            return 1
        }
    fi

    local flag="ipset_share"   # single ip (abandoned) flag is "ip", ip_group flag is ipset_share or ipset_priv according to mode
    local ip_or_set="$ip_group" # single ip (abandoned) ip_or_set is "$ip", ip_group ip_or_set is "$ip_group"
    config_get mode "$rule_name" "mode"
    [ "$mode" != "share" -a "$mode" != "priv" ] && mode="share"
    [ "$mode" = "priv" ] && flag="ipset_priv"
    QOS_WRITE_LOG " rule's ip_group: $ip_group, limit mode: $mode"

    local id="${rule_name//[^0-9]/}"
    local md=1
    [ $flag = "ipset_priv" ] && md=0
    local wid="${if_pong//[^0-9]/}"
    [ ${#wid} = 0 ] && wid=0
    [ $WAN_MODE_ONE = "1" ] && wid=0

    qmark -a "$id $md $wid $rate_max $downrate_max $rate_min $downrate_min"

    local handle=qos_rule_install_raw
    $handle "$rule_name" "$if_ping" "$if_pong" "$rate_min" "$rate_max" "$flag" "$ip_or_set" "$id" 0|| {
        QOS_WRITE_LOG " qos_rule_install $rule_name fail."
        QOS_RET 1
        return 1
    }

    $handle "$rule_name"_mate "$if_pong" "$if_ping" "$downrate_min" "$downrate_max" "$flag" "$ip_or_set" "$id" 1|| {
        QOS_WRITE_LOG " qos_rule_install $rule_name downlink fail."
        QOS_RET 1
        return 1
    }

    QOS_STATE set ${rule_name}.install true

    return 0
}

qos_rule_uninstall_raw()
{
    local rule_name="$1"
    local ret

    # delete netfilter(iptables) rule
    ret=$(qos_nf_del_rule "$rule_name" "$rule_name"_mate)

    return 0
}

# qos user rule unistall
qos_rule_uninstall()
{
    local rule_name="$1"

    [ "$(QOS_STATE get ${rule_name}.install)" != "true" ]  && {
        QOS_WRITE_LOG "  qos_rule_uninstall, rule $rule_name not install."
        QOS_RET 0
        return 1
    }

    QOS_WRITE_LOG " qos_rule_uninstall, uninstall rule $rule_name ..."
    qos_rule_uninstall_raw $rule_name && QOS_STATE set ${rule_name}.install false || return 1

    local id=${rule_name//[^0-9]/}
    qmark -d $id

    # delete self define ip_group form ipset
    local ip_group=${rule_name}
    [ ! `ipset list -n | grep -cw "$ip_group"` -eq 0 ] && {
        `ipset destroy ${ip_group} 2>/dev/null`
        QOS_WRITE_LOG " qos_rule_uninstall, Destroy self define ip_group ${ip_group} form ipset"
    }

    return 0
}
